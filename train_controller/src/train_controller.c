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

// Controller state
typedef struct {
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
    int stopping;
    int central_heartbeat_misses;
    int local_heartbeat_misses;

    pthread_mutex_t mutex;
    pthread_cond_t wakeup;
} train_controller_state_t;

static train_controller_state_t state;
static name_attach_t *attach = NULL;

static int wait_for_stop(unsigned seconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&state.mutex);
    while (!state.stopping && seconds != 0) {
        int result = pthread_cond_timedwait(&state.wakeup, &state.mutex, &deadline);
        if (result != 0) {
            break;
        }
    }
    int stopping = state.stopping;
    pthread_mutex_unlock(&state.mutex);
    return stopping;
}

static int try_connect(connection_t *conn) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    int connected = connection_try_connect(conn);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    return connected;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [-l | -g]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
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
    printf("%s                    TRAIN CONTROLLER%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Connection status
    printf("Connected to central_controller [%s%s%s] last update [%s]\n",
           state.central_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.central_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_central_update[0] ? state.last_central_update : "N/A");

    printf("Connected to local_controller [%s%s%s] last update [%s]\n",
           state.local_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.local_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_local_update[0] ? state.last_local_update : "N/A");

    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Message timestamps
    printf("last message receive central [%s]\n",
           state.last_recv_central[0] ? state.last_recv_central : "N/A");
    printf("last message receive local [%s]\n",
           state.last_recv_local[0] ? state.last_recv_local : "N/A");
    printf("message send central [%s]\n",
           state.last_send_central[0] ? state.last_send_central : "N/A");
    printf("message send local [%s]\n",
           state.last_send_local[0] ? state.last_send_local : "N/A");

    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Commands: send-local | send-central | status | quit\n");
    printf("%s==========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("\n%sMessage:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);

    state.ui_needs_update = 0;
    pthread_mutex_unlock(&state.mutex);
}

// Handler for test messages
static int handle_test_message(int rcvid, any_msg_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    train_controller_state_t *s = (train_controller_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_CENTRAL) {
        strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
        s->last_recv_central[sizeof(s->last_recv_central) - 1] = '\0';
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
    } else if (msg->header.src == CONTROLLER_LOCAL) {
        strncpy(s->last_recv_local, msg->header.timestamp, sizeof(s->last_recv_local) - 1);
        s->last_recv_local[sizeof(s->last_recv_local) - 1] = '\0';
        get_timestamp(s->last_local_update, sizeof(s->last_local_update));
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

// Message handlers array
static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test_message }
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
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    while (!wait_for_stop(0)) {
        // Try connecting to central
        if (try_connect(&state.central_conn)) {
            pthread_mutex_lock(&state.mutex);
            state.central_heartbeat_misses = 0;
            get_timestamp(state.last_central_update, sizeof(state.last_central_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Send initial message to notify central we're connected
            send_test_message(&state.central_conn, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
        }

        // Try connecting to local
        if (try_connect(&state.local_conn)) {
            pthread_mutex_lock(&state.mutex);
            state.local_heartbeat_misses = 0;
            get_timestamp(state.last_local_update, sizeof(state.last_local_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Send initial message to notify local we're connected
            send_test_message(&state.local_conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL);
        }

        if (wait_for_stop(2)) {
            break;
        }
    }

    return NULL;
}

// Thread to refresh UI periodically
static void* ui_refresh_thread(void *arg) {
    (void)arg;

    while (!wait_for_stop(0)) {
        pthread_mutex_lock(&state.mutex);
        int needs_update = state.ui_needs_update;
        pthread_mutex_unlock(&state.mutex);

        if (needs_update) {
            display_ui();
        }

        if (wait_for_stop(UI_CHECK_INTERVAL)) {
            break;
        }
    }

    return NULL;
}

// Thread to check connection health via heartbeat
static void check_heartbeat(connection_t *conn, controller_type_t peer,
                            int *misses, char *last_update, size_t update_size) {
    if (!connection_is_connected(conn)) {
        return;
    }

    uint64_t generation = connection_generation(conn);
    int result = send_heartbeat(conn, CONTROLLER_TRAIN, peer);
    int close_connection = 0;

    pthread_mutex_lock(&state.mutex);
    if (conn->connected && conn->generation == generation) {
        if (result == SEND_OK || result == SEND_REJECTED) {
            *misses = 0;
            get_timestamp(last_update, update_size);
        } else if (*misses < HEARTBEAT_MISS_THRESHOLD) {
            (*misses)++;
        }
        close_connection = *misses >= HEARTBEAT_MISS_THRESHOLD;
        state.ui_needs_update = 1;
    }
    pthread_mutex_unlock(&state.mutex);

    if (close_connection) {
        connection_close_generation(conn, generation);
        pthread_mutex_lock(&state.mutex);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    }
}

static void* heartbeat_thread(void *arg) {
    (void)arg;

    while (!wait_for_stop(HEARTBEAT_INTERVAL)) {
        check_heartbeat(&state.central_conn, CONTROLLER_CENTRAL,
                        &state.central_heartbeat_misses, state.last_central_update,
                        sizeof(state.last_central_update));
        check_heartbeat(&state.local_conn, CONTROLLER_LOCAL,
                        &state.local_heartbeat_misses, state.last_local_update,
                        sizeof(state.last_local_update));
    }

    return NULL;
}

// Execute command
static int execute_command(const char *cmd) {
    if (strcmp(cmd, "status") == 0) {
        display_ui();
        return 0;
    } else if (strcmp(cmd, "send-central") == 0) {
        pthread_mutex_lock(&state.mutex);
        int connected = state.central_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected) {
            printf("%sCentral controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        if (send_test_message(&state.central_conn, CONTROLLER_TRAIN, CONTROLLER_CENTRAL) == 0) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_central, sizeof(state.last_send_central));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        } else {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    } else if (strcmp(cmd, "send-local") == 0) {
        pthread_mutex_lock(&state.mutex);
        int connected = state.local_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected) {
            printf("%sLocal controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        if (send_test_message(&state.local_conn, CONTROLLER_TRAIN, CONTROLLER_LOCAL) == 0) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_local, sizeof(state.last_send_local));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        } else {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    } else {
        printf("%sUnknown command. Use: send-central | send-local | status | quit%s\n", COLOR_RED, COLOR_RESET);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Check for help
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }
    connection_mode_t mode = connection_parse_args(argc, argv);

    // Initialize state
    memset(&state, 0, sizeof(state));
    if (pthread_mutex_init(&state.mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize controller mutex\n");
        return EXIT_FAILURE;
    }
    pthread_condattr_t attributes;
    int result = pthread_condattr_init(&attributes);
    if (result != 0) {
        pthread_mutex_destroy(&state.mutex);
        return EXIT_FAILURE;
    }
    result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (result == 0) {
        result = pthread_cond_init(&state.wakeup, &attributes);
    }
    pthread_condattr_destroy(&attributes);
    if (result != 0) {
        fprintf(stderr, "Failed to initialize controller condition: %s\n", strerror(result));
        pthread_mutex_destroy(&state.mutex);
        return EXIT_FAILURE;
    }
    state.mode = mode;
    state.ui_needs_update = 1;
    connection_init(&state.central_conn, CENTRAL_SERVICE_NAME, mode, &state.mutex);
    connection_init(&state.local_conn, LOCAL_SERVICE_NAME, mode, &state.mutex);

    // Register with name service
    attach = connection_register_service(TRAIN_SERVICE_NAME, mode);
    if (attach == NULL) {
        connection_destroy(&state.central_conn);
        connection_destroy(&state.local_conn);
        pthread_cond_destroy(&state.wakeup);
        pthread_mutex_destroy(&state.mutex);
        return EXIT_FAILURE;
    }

    // Initialize receive context
    receive_context_t recv_ctx;
    receive_init(&recv_ctx, attach, handlers,
                 sizeof(handlers) / sizeof(handlers[0]), &state, CONTROLLER_TRAIN);

    // Start threads
    pthread_t workers[4];
    void *(*worker_functions[])(void *) = {
        message_handler_thread, connection_thread, ui_refresh_thread, heartbeat_thread
    };
    const char *worker_names[] = { "message handler", "connection", "UI", "heartbeat" };
    size_t started = 0;
    int exit_status = EXIT_FAILURE;
    for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
        result = pthread_create(&workers[i], NULL, worker_functions[i],
                                i == 0 ? &recv_ctx : NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to create %s thread: %s\n", worker_names[i], strerror(result));
            goto cleanup;
        }
        started++;
    }

    // Initial UI display
    display_ui();

    // Main loop: read commands
    char cmd[64];
    while (fgets(cmd, sizeof(cmd), stdin) != NULL) {
        cmd[strcspn(cmd, "\r\n")] = '\0';

        if (cmd[0] == '\0') {
            display_ui();
            continue;
        }

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            printf("Exiting...\n");
            break;
        }

        execute_command(cmd);
        wait_for_stop(1);
        display_ui();
    }
    exit_status = ferror(stdin) ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    pthread_mutex_lock(&state.mutex);
    state.stopping = 1;
    pthread_cond_broadcast(&state.wakeup);
    pthread_mutex_unlock(&state.mutex);
    receive_stop(&recv_ctx);
    if (started > 1) {
        pthread_cancel(workers[1]);
    }
    for (size_t i = 0; i < started; i++) {
        pthread_join(workers[i], NULL);
    }
    receive_destroy(&recv_ctx);
    connection_destroy(&state.central_conn);
    connection_destroy(&state.local_conn);
    connection_unregister_service(attach);
    pthread_cond_destroy(&state.wakeup);
    pthread_mutex_destroy(&state.mutex);
    return exit_status;
}
