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

// Controller state
typedef struct {
    // Connections
    connection_t local_conn;
    connection_t train_conn;
    connection_mode_t mode;

    // Timestamps
    char last_recv_local[32];
    char last_recv_train[32];
    char last_send_local[32];
    char last_send_train[32];
    char last_local_update[32];
    char last_train_update[32];
    char local_status[64];
    char train_status[64];
    char last_fault[64];

    int local_heartbeat_misses;
    int train_heartbeat_misses;

    // Flags
    int ui_needs_update;

    pthread_mutex_t mutex;
} central_state_t;

static central_state_t state;
static name_attach_t *attach = NULL;

static void print_usage(const char *prog) {
    printf("Usage: %s [-l | -g]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
}

static void copy_message_data(char *dest, size_t dest_len, const char *src) {
    strncpy(dest, src, dest_len - 1);
    dest[dest_len - 1] = '\0';
}

// Clear screen
static void clear_screen(void) {
    printf("\033[2J\033[H");
}

// Display UI
static void display_ui(void) {
    pthread_mutex_lock(&state.mutex);

    clear_screen();

    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s                    CENTRAL CONTROLLER%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Connection status
    printf("Connected to train_controller [%s%s%s] last update [%s]\n",
           state.train_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.train_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_train_update[0] ? state.last_train_update : "N/A");

    printf("Connected to local_controller [%s%s%s] last update [%s]\n",
           state.local_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.local_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_local_update[0] ? state.last_local_update : "N/A");

    printf("Local status: %s\n",
           state.local_status[0] ? state.local_status : "N/A");
    printf("Railway status: %s\n",
           state.train_status[0] ? state.train_status : "N/A");
    printf("Last fault: %s\n",
           state.last_fault[0] ? state.last_fault : "N/A");
    printf("Heartbeat misses: local %d/%d, railway %d/%d\n",
           state.local_heartbeat_misses, HEARTBEAT_MISS_THRESHOLD,
           state.train_heartbeat_misses, HEARTBEAT_MISS_THRESHOLD);

    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Message timestamps
    printf("last message receive train [%s]\n",
           state.last_recv_train[0] ? state.last_recv_train : "N/A");
    printf("last message receive local [%s]\n",
           state.last_recv_local[0] ? state.last_recv_local : "N/A");
    printf("message send local [%s]\n",
           state.last_send_local[0] ? state.last_send_local : "N/A");
    printf("message send train [%s]\n",
           state.last_send_train[0] ? state.last_send_train : "N/A");

    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Commands: mode-fixed | mode-sensor | status | quit\n");
    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("\n%sMessage:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);

    state.ui_needs_update = 0;
    pthread_mutex_unlock(&state.mutex);
}

// Handler for test messages
static int handle_test_message(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    central_state_t *s = (central_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_LOCAL) {
        strncpy(s->last_recv_local, msg->header.timestamp, sizeof(s->last_recv_local) - 1);
        get_timestamp(s->last_local_update, sizeof(s->last_local_update));
    } else if (msg->header.src == CONTROLLER_TRAIN) {
        strncpy(s->last_recv_train, msg->header.timestamp, sizeof(s->last_recv_train) - 1);
        get_timestamp(s->last_train_update, sizeof(s->last_train_update));
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_status_update(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    central_state_t *s = (central_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);
    copy_message_data(s->local_status, sizeof(s->local_status), msg->data);
    get_timestamp(s->last_local_update, sizeof(s->last_local_update));
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_railway_status(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    central_state_t *s = (central_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);
    copy_message_data(s->train_status, sizeof(s->train_status), msg->data);
    get_timestamp(s->last_train_update, sizeof(s->last_train_update));
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_fault_alert(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    central_state_t *s = (central_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);
    copy_message_data(s->last_fault, sizeof(s->last_fault), msg->data);
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

// Message handlers array
static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test_message },
    { MSG_STATUS_UPDATE, CONTROLLER_LOCAL, handle_status_update },
    { MSG_RAILWAY_STATUS, CONTROLLER_TRAIN, handle_railway_status },
    { MSG_FAULT_ALERT, 0, handle_fault_alert }
};

// Thread to handle incoming messages
static void* message_handler_thread(void *arg) {
    receive_context_t *ctx = (receive_context_t *)arg;
    receive_loop(ctx);
    return NULL;
}

// Thread to manage connections
static void* connection_thread(void *arg) {
    (void)arg;

    while (1) {
        // Try connecting to local
        if (connection_try_connect(&state.local_conn)) {
            pthread_mutex_lock(&state.mutex);
            state.local_heartbeat_misses = 0;
            get_timestamp(state.last_local_update, sizeof(state.last_local_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        }

        // Try connecting to train
        if (connection_try_connect(&state.train_conn)) {
            pthread_mutex_lock(&state.mutex);
            state.train_heartbeat_misses = 0;
            get_timestamp(state.last_train_update, sizeof(state.last_train_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        }

        sleep(2);
    }

    return NULL;
}

// Thread to refresh UI periodically
static void* ui_refresh_thread(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&state.mutex);
        int needs_update = state.ui_needs_update;
        pthread_mutex_unlock(&state.mutex);

        if (needs_update) {
            display_ui();
        }

        sleep(UI_CHECK_INTERVAL);
    }

    return NULL;
}

// Thread to check connection health via heartbeat
static void* heartbeat_thread(void *arg) {
    (void)arg;

    while (1) {
        sleep(HEARTBEAT_INTERVAL);

        // Check local connection
        if (connection_is_connected(&state.local_conn)) {
            int result = send_heartbeat(&state.local_conn, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
            pthread_mutex_lock(&state.mutex);
            if (result == 0) {
                state.local_heartbeat_misses = 0;
            } else if (state.local_heartbeat_misses < HEARTBEAT_MISS_THRESHOLD) {
                state.local_heartbeat_misses++;
            }
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        }

        // Check train connection
        if (connection_is_connected(&state.train_conn)) {
            int result = send_heartbeat(&state.train_conn, CONTROLLER_CENTRAL, CONTROLLER_TRAIN);
            pthread_mutex_lock(&state.mutex);
            if (result == 0) {
                state.train_heartbeat_misses = 0;
            } else if (state.train_heartbeat_misses < HEARTBEAT_MISS_THRESHOLD) {
                state.train_heartbeat_misses++;
            }
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        }
    }

    return NULL;
}

// Execute command
static int send_mode_command(const char *mode) {
    pthread_mutex_lock(&state.mutex);
    int connected = state.local_conn.connected;
    pthread_mutex_unlock(&state.mutex);

    if (!connected) {
        printf("%sLocal controller not connected%s\n", COLOR_RED, COLOR_RESET);
        return -1;
    }

    test_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_MODE_COMMAND;
    msg.header.src = CONTROLLER_CENTRAL;
    msg.header.dst = CONTROLLER_LOCAL;
    copy_message_data(msg.data, sizeof(msg.data), mode);
    get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));

    reply_t reply;
    if (send_message(&state.local_conn, &msg, &reply) != 0) {
        printf("%sFailed to send mode command%s\n", COLOR_RED, COLOR_RESET);
        return -1;
    }

    pthread_mutex_lock(&state.mutex);
    get_timestamp(state.last_send_local, sizeof(state.last_send_local));
    state.ui_needs_update = 1;
    pthread_mutex_unlock(&state.mutex);
    return 0;
}

static int execute_command(const char *cmd) {
    if (strcmp(cmd, "mode-fixed") == 0) {
        return send_mode_command("FIXED");
    } else if (strcmp(cmd, "mode-sensor") == 0) {
        return send_mode_command("SENSOR");
    } else if (strcmp(cmd, "status") == 0) {
        display_ui();
        return 0;
    }

    printf("%sUnknown command. Use: mode-fixed | mode-sensor | status | quit%s\n",
           COLOR_RED, COLOR_RESET);
    return -1;
}

int main(int argc, char *argv[]) {
    // Parse command line arguments
    connection_mode_t mode = connection_parse_args(argc, argv);

    // Check for help
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    // Initialize state
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.mutex, NULL);
    state.mode = mode;
    state.ui_needs_update = 1;
    connection_init(&state.local_conn, LOCAL_SERVICE_NAME, mode, &state.mutex);
    connection_init(&state.train_conn, TRAIN_SERVICE_NAME, mode, &state.mutex);

    // Register with name service
    attach = connection_register_service(CENTRAL_SERVICE_NAME, mode);
    if (attach == NULL) {
        if (mode == CONN_MODE_GLOBAL) {
            fprintf(stderr, "Make sure 'gns -s' is running on this VM\n");
        }
        return EXIT_FAILURE;
    }

    // Initialize receive context
    receive_context_t recv_ctx;
    receive_init(&recv_ctx, attach, handlers,
                 sizeof(handlers) / sizeof(handlers[0]), &state);

    // Start threads
    pthread_t msg_thread, conn_thread, ui_thread, hb_thread;

    if (pthread_create(&msg_thread, NULL, message_handler_thread, &recv_ctx) != 0) {
        fprintf(stderr, "Failed to create message handler thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&conn_thread, NULL, connection_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create connection thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&ui_thread, NULL, ui_refresh_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create UI thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&hb_thread, NULL, heartbeat_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create heartbeat thread\n");
        return EXIT_FAILURE;
    }

    // Initial UI display
    display_ui();

    // Main loop: read commands
    char cmd[64];
    while (1) {
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            cmd[strcspn(cmd, "\n")] = '\0';

            if (strlen(cmd) == 0) {
                display_ui();
                continue;
            }

            if (strcmp(cmd, "quit") == 0) {
                printf("Exiting...\n");
                break;
            }

            execute_command(cmd);
            sleep(1);
            display_ui();
        }
    }

    // Cleanup
    connection_close(&state.local_conn);
    connection_close(&state.train_conn);
    connection_unregister_service(attach);
    pthread_mutex_destroy(&state.mutex);
    return EXIT_SUCCESS;
}
