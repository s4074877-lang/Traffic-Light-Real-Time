#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "../../common/common.h"
#include "../../common/communication/connection.h"
#include "../../common/communication/send.h"
#include "../../common/communication/receive.h"
#include "ui/train_ui.h"

// Controller state
typedef struct
{
    // Connections
    connection_t central_conn;
    connection_t local_conn;
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

static train_controller_state_t state;
static train_ui_state_t ui_state;
static name_attach_t *attach = NULL;

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

// Handler for test messages
static int handle_test_message(int rcvid, test_message_t *msg, reply_t *reply, void *ctx)
{
    (void)rcvid;
    train_controller_state_t *s = (train_controller_state_t *)ctx;
    char timestamp[32];

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_CENTRAL)
    {
        strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
        // Update UI state
        train_ui_set_connection(&ui_state, CONTROLLER_CENTRAL, CONN_CONNECTED, s->last_central_update);
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

        // Try connecting to local
        if (connection_try_connect(&state.local_conn))
        {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_local_update, sizeof(state.last_local_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Update UI state
            get_timestamp(timestamp, sizeof(timestamp));
            train_ui_set_connection(&ui_state, CONTROLLER_LOCAL, CONN_CONNECTED, timestamp);

            // Send initial message to notify local we're connected
            send_test_message(&state.local_conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL);
            train_ui_set_last_sent(&ui_state, timestamp, "TEST", "LOCAL");
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

        // Check local connection
        if (connection_is_connected(&state.local_conn))
        {
            if (send_heartbeat(&state.local_conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL) != 0)
            {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_local_update, sizeof(state.last_local_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);

                // Connection lost - update UI (timestamp updated only on state change)
                get_timestamp(timestamp, sizeof(timestamp));
                train_ui_set_connection(&ui_state, CONTROLLER_LOCAL, CONN_LOST, timestamp);
            }
            // No update on successful heartbeat - only state changes matter
        }
    }

    return NULL;
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
        pthread_mutex_lock(&state.mutex);
        int connected = state.local_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected)
        {
            printf("%sLocal controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        if (send_test_message(&state.local_conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL) == 0)
        {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_local, sizeof(state.last_send_local));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Update UI state
            get_timestamp(timestamp, sizeof(timestamp));
            train_ui_set_last_sent(&ui_state, timestamp, "TEST", "LOCAL");
        }
        else
        {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    }
    else
    {
        printf("%sUnknown command. Use: send-central | send-local%s\n", COLOR_RED, COLOR_RESET);
        return -1;
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
    connection_init(&state.central_conn, CENTRAL_SERVICE_NAME, mode, &state.mutex);
    connection_init(&state.local_conn, LOCAL_SERVICE_NAME, mode, &state.mutex);

    // Initialize UI state
    train_ui_init(&ui_state);

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
    pthread_t msg_thread, conn_thread, ui_thread, hb_thread;

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
    connection_close(&state.central_conn);
    connection_close(&state.local_conn);
    connection_unregister_service(attach);
    pthread_mutex_destroy(&state.mutex);
    train_ui_destroy(&ui_state);
    return EXIT_SUCCESS;
}
