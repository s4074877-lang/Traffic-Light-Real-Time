#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "../../common/common.h"
#include "../../common/communication/connection.h"
#include "../../common/communication/send.h"
#include "../../common/communication/receive.h"
#include "ui/train_ui.h"
#include "crossing/crossing.h"
#include "crossing/rail_sim.h"

// Maximum preempt/clear/fault messages waiting for the sender thread
#define OUTBOX_CAPACITY 64

// Outbox destination for Central (Locals use intersection index 0..5)
#define OUTBOX_CENTRAL (-1)

// Controller state
typedef struct
{
    // Connections
    connection_t central_conn;
    connection_t local_conns[NUM_INTERSECTIONS]; // traffic_local_I1..I6
    connection_t legacy_local_conn;              // I1 published as LOCAL_SERVICE_NAME
    connection_mode_t mode;

    // Timestamps
    char last_recv_central[32];
    char last_recv_local[32];
    char last_send_central[32];
    char last_send_local[32];
    char last_central_update[32];
    char last_local_update[32];

    // Flags
    int ui_needs_update;

    pthread_mutex_t mutex;
} train_controller_state_t;

// Message waiting to be delivered by the sender thread
typedef struct
{
    int destination;       // Local intersection index 0..5, or OUTBOX_CENTRAL
    size_t size;           // Bytes of msg to send
    const char *type_name; // For the UI "last sent" line
    union
    {
        msg_header_t header;
        railway_full_msg_t railway;
        fault_full_msg_t fault;
    } msg;
} outbox_item_t;

static train_controller_state_t state;
static train_ui_state_t ui_state;
static name_attach_t *attach = NULL;

// Crossing callbacks run under the simulator lock, so they only queue
// messages here; the sender thread does the (timed) IPC.
static struct
{
    outbox_item_t items[OUTBOX_CAPACITY];
    int head;
    int count;
    int dropped;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
} outbox = {.mutex = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

// Crossing state (P1, P2, P3)
static crossing_t crossings[NUM_CROSSINGS];

// Forward declarations for crossing callbacks
static void cx_gate_command(crossing_t *cx, gate_command_t cmd);
static void cx_set_flash(crossing_t *cx, bool on);
static void cx_send_preempt(crossing_t *cx);
static void cx_send_clear(crossing_t *cx);
static void cx_fault_alert(crossing_t *cx, cx_fault_t fault);
static void cx_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds);
static void cx_cancel_timer(crossing_t *cx, timer_id_t timer_id);
static void cx_state_changed(crossing_t *cx);

// Forward declaration for status sending
static void send_railway_status(const crossing_t *snapshot, int count);

// Crossing operations callbacks
static const cx_ops_t crossing_ops = {
    .gate_command = cx_gate_command,
    .set_flash = cx_set_flash,
    .send_preempt = cx_send_preempt,
    .send_clear = cx_send_clear,
    .fault_alert = cx_fault_alert,
    .start_timer = cx_start_timer,
    .cancel_timer = cx_cancel_timer,
    .state_changed = cx_state_changed
};

static void print_usage(const char *prog)
{
    printf("Usage: %s [-l | -g]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
}

// Display UI using the new UI module
static void display_ui(void)
{
    train_ui_display(&ui_state);
}

// Helper to get message type name
static const char *msg_type_name(uint16_t type)
{
    switch (type)
    {
    case MSG_TEST:
        return "TEST";
    case MSG_HEARTBEAT:
        return "HEARTBEAT";
    case MSG_MODE_COMMAND:
        return "MODE_CMD";
    case MSG_STATUS_UPDATE:
        return "STATUS";
    case MSG_FAULT_ALERT:
        return "FAULT";
    case MSG_RAILWAY_PREEMPT:
        return "PREEMPT";
    case MSG_TRAIN_CLEAR:
        return "CLEAR";
    case MSG_RAILWAY_STATUS:
        return "RLY_STATUS";
    case MSG_SENSOR_UPDATE:
        return "SENSOR";
    case MSG_PED_REQUEST:
        return "PED_REQ";
    default:
        return "UNKNOWN";
    }
}

// ============================================
// Local Links (I1..I6)
// ============================================

// Connected link to Local intersection index 0..5, or NULL
static connection_t *local_link(int index)
{
    if (connection_is_connected(&state.local_conns[index]))
    {
        return &state.local_conns[index];
    }
    if (index == 0 && connection_is_connected(&state.legacy_local_conn))
    {
        return &state.legacy_local_conn;
    }
    return NULL;
}

// The UI has a single LOCAL row: connected while any intersection is reachable
static void update_local_ui_status(void)
{
    char timestamp[32];
    int connected = 0;

    for (int i = 0; i < NUM_INTERSECTIONS; i++)
    {
        if (local_link(i) != NULL)
        {
            connected++;
        }
    }

    get_timestamp(timestamp, sizeof(timestamp));
    train_ui_set_connection(&ui_state, CONTROLLER_LOCAL,
                            connected > 0 ? CONN_CONNECTED : CONN_LOST, timestamp);
}

// ============================================
// Outbox
// ============================================

static void outbox_push(const outbox_item_t *item)
{
    pthread_mutex_lock(&outbox.mutex);
    if (outbox.count < OUTBOX_CAPACITY)
    {
        outbox.items[(outbox.head + outbox.count) % OUTBOX_CAPACITY] = *item;
        outbox.count++;
        pthread_cond_signal(&outbox.ready);
    }
    else
    {
        outbox.dropped++;
    }
    pthread_mutex_unlock(&outbox.mutex);
}

static void deliver(const outbox_item_t *item)
{
    char timestamp[32];
    char dest[16];
    connection_t *conn;
    reply_t reply;

    if (item->destination == OUTBOX_CENTRAL)
    {
        conn = connection_is_connected(&state.central_conn) ? &state.central_conn : NULL;
        snprintf(dest, sizeof(dest), "CENTRAL");
    }
    else
    {
        conn = local_link(item->destination);
        snprintf(dest, sizeof(dest), "LOCAL I%d", item->destination + 1);
    }

    if (conn == NULL)
    {
        return; // Not connected; the connection thread will retry the link
    }

    get_timestamp(timestamp, sizeof(timestamp));
    if (send_message_timeout(conn, &item->msg, item->size, &reply, SEND_TIMEOUT_MS) != 0)
    {
        if (item->destination == OUTBOX_CENTRAL)
        {
            train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_LOST, timestamp);
        }
        else
        {
            update_local_ui_status();
        }
        return;
    }

    train_ui_set_last_sent(&ui_state, timestamp, item->type_name, dest);
}

// Thread to deliver queued messages without blocking the simulator
static void *sender_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        outbox_item_t item;

        pthread_mutex_lock(&outbox.mutex);
        while (outbox.count == 0)
        {
            pthread_cond_wait(&outbox.ready, &outbox.mutex);
        }
        item = outbox.items[outbox.head];
        outbox.head = (outbox.head + 1) % OUTBOX_CAPACITY;
        outbox.count--;
        pthread_mutex_unlock(&outbox.mutex);

        deliver(&item);
    }

    return NULL;
}

// Handler for test messages
static int handle_test_message(int rcvid, test_message_t *msg, reply_t *reply, void *ctx)
{
    (void)rcvid;
    train_controller_state_t *s = (train_controller_state_t *)ctx;
    char timestamp[32];
    char command[sizeof(msg->data)];

    command[0] = '\0';

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_CENTRAL)
    {
        strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
        // Update UI state
        train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_CONNECTED, s->last_central_update);

        // Command present in data is run after releasing the mutex
        memcpy(command, msg->data, sizeof(command));
        command[sizeof(command) - 1] = '\0';
    }
    else if (msg->header.src == CONTROLLER_LOCAL)
    {
        strncpy(s->last_recv_local, msg->header.timestamp, sizeof(s->last_recv_local) - 1);
        get_timestamp(s->last_local_update, sizeof(s->last_local_update));
        // Update UI state
        train_ui_set_connection(&ui_state, CONTROLLER_LOCAL, CONN_CONNECTED, s->last_local_update);
    }

    // Update last received message info
    get_timestamp(timestamp, sizeof(timestamp));
    train_ui_set_last_received(&ui_state, timestamp,
                               msg_type_name(msg->header.type),
                               controller_name(msg->header.src));

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    // The simulator has its own lock
    if (command[0] != '\0')
    {
        char cmd_reply[256];
        rail_sim_command(command, cmd_reply, sizeof(cmd_reply));
        train_ui_set_time_scale(&ui_state, rail_sim_get_time_scale());
    }

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

// Message handlers array
static message_handler_entry_t handlers[] = {
    {MSG_TEST, 0, handle_test_message}};

// Thread to handle incoming messages
static void *message_handler_thread(void *arg)
{
    receive_context_t *ctx = (receive_context_t *)arg;
    receive_loop(ctx);
    return NULL;
}

// Thread to manage connections
static void *connection_thread(void *arg)
{
    (void)arg;
    char timestamp[32];

    while (1)
    {
        // Try connecting to central
        if (connection_try_connect(&state.central_conn))
        {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_central_update, sizeof(state.last_central_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Update UI state
            get_timestamp(timestamp, sizeof(timestamp));
            train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_CONNECTED, timestamp);

            // Send initial message to notify central we're connected
            send_test_message(&state.central_conn, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
            train_ui_set_last_sent(&ui_state, timestamp, "TEST", "CENTRAL");
        }

        // Try connecting to each Local intersection
        for (int i = 0; i < NUM_INTERSECTIONS; i++)
        {
            connection_t *conn = &state.local_conns[i];
            int newly_connected = connection_try_connect(conn);

            // I1 may still run under the legacy service name
            if (!newly_connected && i == 0 && !connection_is_connected(conn))
            {
                conn = &state.legacy_local_conn;
                newly_connected = connection_try_connect(conn);
            }

            if (newly_connected)
            {
                char dest[16];

                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_local_update, sizeof(state.last_local_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);

                // Update UI state
                get_timestamp(timestamp, sizeof(timestamp));
                update_local_ui_status();

                // Send initial message to notify local we're connected
                send_test_message(conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL);
                snprintf(dest, sizeof(dest), "LOCAL I%d", i + 1);
                train_ui_set_last_sent(&ui_state, timestamp, "TEST", dest);
            }
        }

        sleep(2);
    }

    return NULL;
}

// Thread to refresh UI periodically
static void *ui_refresh_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        // Only auto-refresh when on status screen
        ui_screen_t current = train_ui_get_screen(&ui_state);

        if (current == UI_SCREEN_STATUS)
        {
            // Check both old state flag and new UI module flag
            pthread_mutex_lock(&state.mutex);
            int needs_update = state.ui_needs_update;
            state.ui_needs_update = 0;
            pthread_mutex_unlock(&state.mutex);

            if (needs_update || train_ui_needs_update(&ui_state))
            {
                display_ui();
            }
        }

        sleep(UI_CHECK_INTERVAL);
    }

    return NULL;
}

// Thread to check connection health via heartbeat
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    char timestamp[32];

    while (1)
    {
        sleep(HEARTBEAT_INTERVAL);

        // Check central connection
        if (connection_is_connected(&state.central_conn))
        {
            if (send_heartbeat(&state.central_conn, CONTROLLER_TRAIN, CONTROLLER_CENTRAL) != 0)
            {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_central_update, sizeof(state.last_central_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);

                // Connection lost - update UI (timestamp updated only on state change)
                get_timestamp(timestamp, sizeof(timestamp));
                train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_LOST, timestamp);
            }
            // No update on successful heartbeat - only state changes matter
        }

        // Check each Local connection
        for (int i = 0; i < NUM_INTERSECTIONS; i++)
        {
            connection_t *conn = local_link(i);

            if (conn != NULL && send_heartbeat(conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL) != 0)
            {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_local_update, sizeof(state.last_local_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);

                // Link lost - LOCAL row shows lost only when no Local remains
                update_local_ui_status();
            }
        }
    }

    return NULL;
}

// Thread to tick the simulator (never blocks on IPC)
static void *sim_tick_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        usleep(SIM_TICK_MS * 1000);
        rail_sim_tick();
    }

    return NULL;
}

// Thread to send periodic railway status to Central
static void *status_thread(void *arg)
{
    (void)arg;
    crossing_t snapshot[NUM_CROSSINGS];

    while (1)
    {
        sleep(STATUS_REPORT_INTERVAL_SEC);
        int count = rail_sim_snapshot(snapshot, NUM_CROSSINGS);
        send_railway_status(snapshot, count);
    }

    return NULL;
}

// ============================================
// Crossing Callback Implementations
// ============================================
// These run under the simulator lock: they must only queue IPC, never send.

static void cx_gate_command(crossing_t *cx, gate_command_t cmd)
{
    // Forward to simulator
    rail_sim_gate_command(cx, cmd);
}

static void cx_set_flash(crossing_t *cx, bool on)
{
    int idx = cx->id - 1;
    if (idx >= 0 && idx < NUM_CROSSINGS)
    {
        // Only update flash state, don't touch other fields
        pthread_mutex_lock(&ui_state.mutex);
        ui_state.crossings[idx].road_flash = on ? FLASH_ON : FLASH_OFF;
        ui_state.needs_update = 1;
        pthread_mutex_unlock(&ui_state.mutex);
    }
}

// Queue a PREEMPT or CLEAR for both Local intersections next to this crossing
static void queue_railway_message(const crossing_t *cx, msg_type_t type)
{
    const uint8_t locals[] = {cx->local_id_1, cx->local_id_2};

    for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); i++)
    {
        if (locals[i] < 1 || locals[i] > NUM_INTERSECTIONS)
        {
            continue;
        }

        outbox_item_t item;
        memset(&item, 0, sizeof(item));
        item.destination = locals[i] - 1;
        item.size = sizeof(item.msg.railway);
        item.type_name = type == MSG_RAILWAY_PREEMPT ? "PREEMPT" : "CLEAR";

        item.msg.railway.header.type = type;
        item.msg.railway.header.src = CONTROLLER_TRAIN;
        item.msg.railway.header.dst = CONTROLLER_LOCAL;
        get_timestamp(item.msg.railway.header.timestamp, sizeof(item.msg.railway.header.timestamp));
        // Local matches on the crossing ID (1..3), not its own intersection ID
        item.msg.railway.payload.intersection_id = cx->id;
        item.msg.railway.payload.active = type == MSG_RAILWAY_PREEMPT ? 1 : 0;
        item.msg.railway.payload.eta_seconds = type == MSG_RAILWAY_PREEMPT ? GATE_CLOSE_DELAY_SEC + 5 : 0;

        outbox_push(&item);
    }
}

static void cx_send_preempt(crossing_t *cx)
{
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    // Send RAILWAY_PREEMPT to both affected Local controllers
    queue_railway_message(cx, MSG_RAILWAY_PREEMPT);

    // Update UI with preempt time
    train_ui_set_preempt_time(&ui_state, cx->id - 1, timestamp);
}

static void cx_send_clear(crossing_t *cx)
{
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    // Send TRAIN_CLEAR to both affected Local controllers
    queue_railway_message(cx, MSG_TRAIN_CLEAR);

    // Update UI with clear time
    train_ui_set_clear_time(&ui_state, cx->id - 1, timestamp);
}

static void cx_fault_alert(crossing_t *cx, cx_fault_t fault)
{
    // Send FAULT_ALERT to Central
    outbox_item_t item;
    memset(&item, 0, sizeof(item));
    item.destination = OUTBOX_CENTRAL;
    item.size = sizeof(item.msg.fault);
    item.type_name = "FAULT";

    item.msg.fault.header.type = MSG_FAULT_ALERT;
    item.msg.fault.header.src = CONTROLLER_TRAIN;
    item.msg.fault.header.dst = CONTROLLER_CENTRAL;
    get_timestamp(item.msg.fault.header.timestamp, sizeof(item.msg.fault.header.timestamp));
    item.msg.fault.payload.source_id = cx->id;
    item.msg.fault.payload.fault_type = FAULT_GATE;
    item.msg.fault.payload.severity = SEV_CRITICAL;
    snprintf(item.msg.fault.payload.description, sizeof(item.msg.fault.payload.description), "%s: %s",
             cx->name, cx_fault_str(fault));

    outbox_push(&item);

    // Update UI fault count
    int fault_count = 0;
    for (int i = 0; i < NUM_CROSSINGS; i++)
    {
        if (crossings[i].fault != CX_FAULT_NONE)
        {
            fault_count++;
        }
    }
    train_ui_set_active_faults(&ui_state, fault_count);

    // Update train signal if any fault
    train_ui_set_signal(&ui_state, crossing_any_fault(crossings, NUM_CROSSINGS) ? SIGNAL_STOP : SIGNAL_PROCEED);
}

static void cx_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds)
{
    rail_sim_start_timer(cx, timer_id, seconds);
}

static void cx_cancel_timer(crossing_t *cx, timer_id_t timer_id)
{
    rail_sim_cancel_timer(cx, timer_id);
}

static void cx_state_changed(crossing_t *cx)
{
    int idx = cx->id - 1;
    if (idx >= 0 && idx < NUM_CROSSINGS)
    {
        // Update UI with current crossing state
        train_ui_set_track_state(&ui_state, idx, TRACK_UP, (track_state_t)cx->track[CX_TRACK_UP]);
        train_ui_set_track_state(&ui_state, idx, TRACK_DOWN, (track_state_t)cx->track[CX_TRACK_DOWN]);
        train_ui_set_gate_state(&ui_state, idx, cx->gate);

        // Map crossing fault to UI fault type
        crossing_fault_t ui_fault = CROSSING_FAULT_NONE;
        switch (cx->fault)
        {
        case CX_FAULT_GATE_CLOSE_TIMEOUT:
            ui_fault = CROSSING_FAULT_GATE_CLOSE_TIMEOUT;
            break;
        case CX_FAULT_GATE_OPEN_TIMEOUT:
            ui_fault = CROSSING_FAULT_GATE_OPEN_TIMEOUT;
            break;
        default:
            if (cx->fault != CX_FAULT_NONE)
            {
                ui_fault = CROSSING_FAULT_GATE_CLOSE_TIMEOUT; // Generic fault display
            }
            break;
        }
        train_ui_set_crossing_fault(&ui_state, idx, ui_fault);

        // Update train signal and fault count
        int fault_count = 0;
        for (int i = 0; i < NUM_CROSSINGS; i++)
        {
            if (crossings[i].fault != CX_FAULT_NONE)
            {
                fault_count++;
            }
        }
        train_ui_set_active_faults(&ui_state, fault_count);
        train_ui_set_signal(&ui_state, fault_count > 0 ? SIGNAL_STOP : SIGNAL_PROCEED);

        // Request UI update
        train_ui_request_update(&ui_state);
    }
}

// Send railway status to Central (called periodically with a crossing snapshot)
static void send_railway_status(const crossing_t *snapshot, int count)
{
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    if (!connection_is_connected(&state.central_conn))
    {
        return;
    }

    for (int i = 0; i < count; i++)
    {
        const crossing_t *cx = &snapshot[i];

        railway_status_full_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.header.type = MSG_RAILWAY_STATUS;
        msg.header.src = CONTROLLER_TRAIN;
        msg.header.dst = CONTROLLER_CENTRAL;
        get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));
        msg.payload.crossing_id = cx->id;

        // Determine train state from track states
        if (cx->track[CX_TRACK_UP] == CX_TRACK_ON_CROSSING || cx->track[CX_TRACK_DOWN] == CX_TRACK_ON_CROSSING)
        {
            msg.payload.train_state = TRAIN_AT_CROSSING;
        }
        else if (cx->track[CX_TRACK_UP] == CX_TRACK_APPROACHING || cx->track[CX_TRACK_DOWN] == CX_TRACK_APPROACHING)
        {
            msg.payload.train_state = TRAIN_APPROACHING;
        }
        else if (cx->track[CX_TRACK_UP] == CX_TRACK_CLEARED || cx->track[CX_TRACK_DOWN] == CX_TRACK_CLEARED)
        {
            msg.payload.train_state = TRAIN_CLEAR;
        }
        else
        {
            msg.payload.train_state = TRAIN_NONE;
        }

        msg.payload.gate_state = cx->gate;
        msg.payload.fault = (cx->fault != CX_FAULT_NONE) ? FAULT_GATE : FAULT_NONE;

        reply_t reply;
        if (send_message_timeout(&state.central_conn, &msg, sizeof(msg), &reply, SEND_TIMEOUT_MS) != 0)
        {
            train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_LOST, timestamp);
            return;
        }
    }

    train_ui_set_last_sent(&ui_state, timestamp, "RLY_STATUS", "CENTRAL");
}

// Execute command
static int execute_command(const char *cmd)
{
    char timestamp[32];

    if (strcmp(cmd, "send-central") == 0)
    {
        pthread_mutex_lock(&state.mutex);
        int connected = state.central_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected)
        {
            printf("%sCentral controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        if (send_test_message(&state.central_conn, CONTROLLER_TRAIN, CONTROLLER_CENTRAL) == 0)
        {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_central, sizeof(state.last_send_central));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Update UI state
            get_timestamp(timestamp, sizeof(timestamp));
            train_ui_set_last_sent(&ui_state, timestamp, "TEST", "CENTRAL");
        }
        else
        {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    }
    else if (strcmp(cmd, "send-local") == 0)
    {
        int sent = 0;

        // Send to every connected Local intersection
        for (int i = 0; i < NUM_INTERSECTIONS; i++)
        {
            connection_t *conn = local_link(i);
            if (conn != NULL && send_test_message(conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL) == 0)
            {
                sent++;
            }
        }

        if (sent == 0)
        {
            printf("%sNo Local controller connected%s\n", COLOR_RED, COLOR_RESET);
            update_local_ui_status();
            return -1;
        }

        pthread_mutex_lock(&state.mutex);
        get_timestamp(state.last_send_local, sizeof(state.last_send_local));
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);

        // Update UI state
        get_timestamp(timestamp, sizeof(timestamp));
        train_ui_set_last_sent(&ui_state, timestamp, "TEST", "LOCAL");
        printf("%sTest message sent to %d Local controller(s)%s\n", COLOR_GREEN, sent, COLOR_RESET);
    }
    else
    {
        // Try as a crossing/simulator command
        char reply[512];
        bool ok = rail_sim_command(cmd, reply, sizeof(reply));
        train_ui_set_time_scale(&ui_state, rail_sim_get_time_scale());
        if (ok)
        {
            printf("%s%s%s\n", COLOR_GREEN, reply, COLOR_RESET);
            return 0;
        }
        else if (strncmp(reply, "ERROR:", 6) == 0)
        {
            printf("%s%s%s\n", COLOR_RED, reply, COLOR_RESET);
            return -1;
        }
        else
        {
            // Command was recognized but returned info/warning
            printf("%s\n", reply);
            return 0;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    // Parse command line arguments
    connection_mode_t mode = connection_parse_args(argc, argv);

    // Check for help
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    // Initialize state
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.mutex, NULL);
    state.mode = mode;
    state.ui_needs_update = 1;
    if (mode == CONN_MODE_GLOBAL) {
        // Global mode: connect to remote VMs via /net/{vm}/dev/name/local/
        connection_init_remote(&state.central_conn, CENTRAL_SERVICE_NAME,
                               VM3_CENTRAL_NAME, &state.mutex);
        for (int i = 0; i < NUM_INTERSECTIONS; i++)
        {
            char name[64];
            snprintf(name, sizeof(name), "%s%d", LOCAL_INTERSECTION_SERVICE_PREFIX, i + 1);
            connection_init_remote(&state.local_conns[i], name, VM1_LOCAL_NAME, &state.mutex);
        }
        connection_init_remote(&state.legacy_local_conn, LOCAL_SERVICE_NAME,
                               VM1_LOCAL_NAME, &state.mutex);
    } else {
        // Local mode: connect via /dev/name/local/
        connection_init(&state.central_conn, CENTRAL_SERVICE_NAME, mode, &state.mutex);
        for (int i = 0; i < NUM_INTERSECTIONS; i++)
        {
            char name[64];
            snprintf(name, sizeof(name), "%s%d", LOCAL_INTERSECTION_SERVICE_PREFIX, i + 1);
            connection_init(&state.local_conns[i], name, mode, &state.mutex);
        }
        connection_init(&state.legacy_local_conn, LOCAL_SERVICE_NAME, mode, &state.mutex);
    }

    // Initialize UI state
    train_ui_init(&ui_state);

    // Initialize crossings (P1, P2, P3)
    crossing_init(&crossings[0], 1, "P1", 1, 2, &crossing_ops, &state);  // P1 affects I1, I2
    crossing_init(&crossings[1], 2, "P2", 3, 4, &crossing_ops, &state);  // P2 affects I3, I4
    crossing_init(&crossings[2], 3, "P3", 5, 6, &crossing_ops, &state);  // P3 affects I5, I6

    // Initialize rail simulator
    rail_sim_init(crossings, NUM_CROSSINGS, DEFAULT_TIME_SCALE);
    train_ui_set_time_scale(&ui_state, DEFAULT_TIME_SCALE);

    // Register with name service
    attach = connection_register_service(TRAIN_SERVICE_NAME, mode);
    if (attach == NULL)
    {
        return EXIT_FAILURE;
    }

    // Initialize receive context
    receive_context_t recv_ctx;
    receive_init(&recv_ctx, attach, handlers,
                 sizeof(handlers) / sizeof(handlers[0]), &state);

    // Start threads
    pthread_t msg_thread, conn_thread, ui_thread, hb_thread, sim_thread, send_thread, stat_thread;

    if (pthread_create(&msg_thread, NULL, message_handler_thread, &recv_ctx) != 0)
    {
        fprintf(stderr, "Failed to create message handler thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&conn_thread, NULL, connection_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create connection thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&ui_thread, NULL, ui_refresh_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create UI thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&hb_thread, NULL, heartbeat_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create heartbeat thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&send_thread, NULL, sender_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create sender thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&sim_thread, NULL, sim_tick_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create simulator tick thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&stat_thread, NULL, status_thread, NULL) != 0)
    {
        fprintf(stderr, "Failed to create status thread\n");
        return EXIT_FAILURE;
    }

    // Initial UI display (start with menu)
    display_ui();

    // Main loop: handle menu navigation
    char input[64];
    int running = 1;

    while (running)
    {
        if (fgets(input, sizeof(input), stdin) != NULL)
        {
            input[strcspn(input, "\n")] = '\0';

            ui_screen_t current = train_ui_get_screen(&ui_state);

            switch (current)
            {
            case UI_SCREEN_MENU:
                if (strcmp(input, "1") == 0)
                {
                    train_ui_set_screen(&ui_state, UI_SCREEN_STATUS);
                    display_ui();
                }
                else if (strcmp(input, "2") == 0)
                {
                    train_ui_set_screen(&ui_state, UI_SCREEN_COMMAND);
                    display_ui();
                }
                else if (strcmp(input, "3") == 0 || strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0)
                {
                    printf("Exiting...\n");
                    running = 0;
                }
                else if (strlen(input) > 0)
                {
                    printf("%sInvalid option. Press Enter to continue.%s", COLOR_RED, COLOR_RESET);
                    fflush(stdout);
                }
                else
                {
                    display_ui();
                }
                break;

            case UI_SCREEN_STATUS:
                if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0)
                {
                    train_ui_set_screen(&ui_state, UI_SCREEN_MENU);
                    display_ui();
                }
                else
                {
                    // Refresh status on any other input (including empty)
                    display_ui();
                }
                break;

            case UI_SCREEN_COMMAND:
                if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0)
                {
                    train_ui_set_screen(&ui_state, UI_SCREEN_MENU);
                    display_ui();
                }
                else if (strlen(input) > 0)
                {
                    execute_command(input);
                    sleep(1);
                    display_ui();
                }
                else
                {
                    display_ui();
                }
                break;
            }
        }
    }

    // Cleanup
    rail_sim_destroy();
    connection_close(&state.central_conn);
    for (int i = 0; i < NUM_INTERSECTIONS; i++)
    {
        connection_close(&state.local_conns[i]);
    }
    connection_close(&state.legacy_local_conn);
    connection_unregister_service(attach);
    pthread_mutex_destroy(&state.mutex);
    train_ui_destroy(&ui_state);
    return EXIT_SUCCESS;
}
