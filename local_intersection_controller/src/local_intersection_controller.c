#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>

#include "../../common/common.h"

// Global state
static light_state_t current_state = LIGHT_GREEN;  // Default state
static int connected_to_central = 0;
static int connection_msg_printed = 0;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int central_coid = -1;  // Connection to central

// Thread to handle incoming messages from central
void* message_handler(void *arg) {
    name_attach_t *attach = (name_attach_t *)arg;
    controller_msg_t msg;
    controller_reply_t reply;
    int rcvid;

    while (1) {
        rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            continue;
        }

        if (rcvid == 0) {
            // Pulse received (system message)
            continue;
        }

        // Handle message based on type
        if (msg.type == MSG_COMMAND && msg.controller == CONTROLLER_LOCAL) {
            pthread_mutex_lock(&state_mutex);
            light_state_t old_state = current_state;
            current_state = msg.state.light;

            if (old_state != current_state) {
                char ts[32];
                get_timestamp(ts, sizeof(ts));
                printf("[%s] State changed to: %s%s%s\n",
                       ts,
                       light_state_color(current_state),
                       light_state_str(current_state),
                       COLOR_RESET);
                fflush(stdout);
            }
            pthread_mutex_unlock(&state_mutex);

            reply.status = 0;
            MsgReply(rcvid, 0, &reply, sizeof(reply));

            // Send status update back to central if connected
            if (central_coid != -1) {
                controller_msg_t status_msg;
                status_msg.type = MSG_STATUS_UPDATE;
                status_msg.controller = CONTROLLER_LOCAL;
                status_msg.state.light = current_state;
                get_timestamp(status_msg.timestamp, sizeof(status_msg.timestamp));

                controller_reply_t status_reply;
                if (MsgSend(central_coid, &status_msg, sizeof(status_msg),
                           &status_reply, sizeof(status_reply)) == -1) {
                    // Connection lost
                    pthread_mutex_lock(&state_mutex);
                    connected_to_central = 0;
                    connection_msg_printed = 0;
                    ConnectDetach(central_coid);
                    central_coid = -1;
                    pthread_mutex_unlock(&state_mutex);
                }
            }
        } else {
            reply.status = -1;
            MsgReply(rcvid, 0, &reply, sizeof(reply));
        }
    }

    return NULL;
}

// Thread to try connecting to central controller
void* connection_thread(void *arg) {
    (void)arg;
    char central_path[256];

    while (1) {
        pthread_mutex_lock(&state_mutex);
        int is_connected = connected_to_central;
        pthread_mutex_unlock(&state_mutex);

        if (!is_connected) {
            // Try to connect via QNET
            snprintf(central_path, sizeof(central_path),
                    "/net/%s/dev/name/local/%s", VM_CENTRAL, CENTRAL_SERVICE_NAME);

            int coid = name_open(CENTRAL_SERVICE_NAME, NAME_FLAG_ATTACH_GLOBAL);

            if (coid == -1) {
                // Try direct QNET path
                coid = open(central_path, O_RDWR);
            }

            if (coid != -1) {
                pthread_mutex_lock(&state_mutex);
                central_coid = coid;
                connected_to_central = 1;

                char ts[32];
                get_timestamp(ts, sizeof(ts));
                printf("%s[%s] Connected to central controller%s\n",
                       COLOR_GREEN, ts, COLOR_RESET);
                fflush(stdout);

                // Send initial status
                controller_msg_t status_msg;
                status_msg.type = MSG_STATUS_UPDATE;
                status_msg.controller = CONTROLLER_LOCAL;
                status_msg.state.light = current_state;
                get_timestamp(status_msg.timestamp, sizeof(status_msg.timestamp));

                controller_reply_t reply;
                if (MsgSend(central_coid, &status_msg, sizeof(status_msg),
                           &reply, sizeof(reply)) == -1) {
                    connected_to_central = 0;
                    ConnectDetach(central_coid);
                    central_coid = -1;
                }

                connection_msg_printed = 0;
                pthread_mutex_unlock(&state_mutex);
            } else {
                pthread_mutex_lock(&state_mutex);
                if (!connection_msg_printed) {
                    char ts[32];
                    get_timestamp(ts, sizeof(ts));
                    printf("%s[%s] Waiting connection from central controller%s\n",
                           COLOR_YELLOW, ts, COLOR_RESET);
                    fflush(stdout);
                    connection_msg_printed = 1;
                }
                pthread_mutex_unlock(&state_mutex);
            }
        }

        sleep(2);  // Retry connection every 2 seconds
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("%s========================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s   LOCAL INTERSECTION CONTROLLER%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s========================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Default state: %s%s%s\n\n",
           light_state_color(current_state),
           light_state_str(current_state),
           COLOR_RESET);
    fflush(stdout);

    // Create channel for receiving messages
    name_attach_t *attach = name_attach(NULL, LOCAL_SERVICE_NAME, NAME_FLAG_ATTACH_GLOBAL);
    if (attach == NULL) {
        fprintf(stderr, "Failed to create channel: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    // Start message handler thread
    pthread_t msg_thread;
    if (pthread_create(&msg_thread, NULL, message_handler, attach) != 0) {
        fprintf(stderr, "Failed to create message handler thread\n");
        return EXIT_FAILURE;
    }

    // Start connection thread
    pthread_t conn_thread;
    if (pthread_create(&conn_thread, NULL, connection_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create connection thread\n");
        return EXIT_FAILURE;
    }

    // Main loop: print current state every 2 seconds
    while (1) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));

        pthread_mutex_lock(&state_mutex);
        printf("[%s] %s%s%s\n",
               ts,
               light_state_color(current_state),
               light_state_str(current_state),
               COLOR_RESET);
        pthread_mutex_unlock(&state_mutex);

        fflush(stdout);
        sleep(STATE_PRINT_INTERVAL);
    }

    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
