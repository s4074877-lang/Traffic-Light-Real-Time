#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <fcntl.h>

#include "../../common/common.h"

// Controller status structure
typedef struct {
    int connected;
    int coid;                    // Connection ID to controller
    char vm_name[16];
    char last_update[32];
    union {
        light_state_t light;
        crossing_state_t crossing;
    } state;
    char pending_command[32];
    char command_timestamp[32];
} controller_status_t;

// Global state
static controller_status_t local_status = {0};
static controller_status_t train_status = {0};
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ui_needs_update = 1;

// Clear screen
static void clear_screen(void) {
    printf("\033[2J\033[H");
}

// Display UI
static void display_ui(void) {
    pthread_mutex_lock(&status_mutex);

    clear_screen();

    printf("%s%s=======================================%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
    printf("%s%s       CENTRAL CONTROLLER%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
    printf("%s%s=======================================%s\n\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET);

    // Local intersection status
    printf("%s[LOCAL INTERSECTION]%s\n", COLOR_BOLD, COLOR_RESET);
    printf("  VM: %s%-8s%s | ", COLOR_BLUE, VM_LOCAL, COLOR_RESET);
    printf("Status: %s%-12s%s | ",
           local_status.connected ? COLOR_GREEN : COLOR_RED,
           local_status.connected ? "Connected" : "Disconnected",
           COLOR_RESET);
    printf("State: %s%-8s%s\n",
           light_state_color(local_status.state.light),
           light_state_str(local_status.state.light),
           COLOR_RESET);
    printf("  Last Update: %-12s | ", local_status.last_update[0] ? local_status.last_update : "N/A");
    printf("Pending Cmd: %-10s | ", local_status.pending_command[0] ? local_status.pending_command : "None");
    printf("Cmd Time: %s\n\n", local_status.command_timestamp[0] ? local_status.command_timestamp : "N/A");

    // Train controller status
    printf("%s[TRAIN CONTROLLER]%s\n", COLOR_BOLD, COLOR_RESET);
    printf("  VM: %s%-8s%s | ", COLOR_BLUE, VM_TRAIN, COLOR_RESET);
    printf("Status: %s%-12s%s | ",
           train_status.connected ? COLOR_GREEN : COLOR_RED,
           train_status.connected ? "Connected" : "Disconnected",
           COLOR_RESET);
    printf("State: %s%-8s%s\n",
           crossing_state_color(train_status.state.crossing),
           crossing_state_str(train_status.state.crossing),
           COLOR_RESET);
    printf("  Last Update: %-12s | ", train_status.last_update[0] ? train_status.last_update : "N/A");
    printf("Pending Cmd: %-10s | ", train_status.pending_command[0] ? train_status.pending_command : "None");
    printf("Cmd Time: %s\n\n", train_status.command_timestamp[0] ? train_status.command_timestamp : "N/A");

    printf("%s=======================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("Commands: local-RED | local-YELLOW | local-GREEN\n");
    printf("          train-BLOCK | train-OPEN\n");
    printf("          quit (to exit)\n");
    printf("%s=======================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("\n%sCommand:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);

    ui_needs_update = 0;
    pthread_mutex_unlock(&status_mutex);
}

// Parse and execute command
static int execute_command(const char *cmd) {
    char target[32], state[32];
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    // Parse command (format: target-STATE)
    if (sscanf(cmd, "%31[^-]-%31s", target, state) != 2) {
        printf("%sInvalid command format. Use: target-STATE%s\n", COLOR_RED, COLOR_RESET);
        return -1;
    }

    // Convert to lowercase for comparison
    for (int i = 0; target[i]; i++) target[i] = tolower(target[i]);
    for (int i = 0; state[i]; i++) state[i] = toupper(state[i]);

    controller_msg_t msg;
    msg.type = MSG_COMMAND;
    get_timestamp(msg.timestamp, sizeof(msg.timestamp));

    if (strcmp(target, "local") == 0) {
        msg.controller = CONTROLLER_LOCAL;

        if (strcmp(state, "RED") == 0) {
            msg.state.light = LIGHT_RED;
        } else if (strcmp(state, "YELLOW") == 0) {
            msg.state.light = LIGHT_YELLOW;
        } else if (strcmp(state, "GREEN") == 0) {
            msg.state.light = LIGHT_GREEN;
        } else {
            printf("%sInvalid local state. Use: RED, YELLOW, GREEN%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        pthread_mutex_lock(&status_mutex);
        if (!local_status.connected || local_status.coid == -1) {
            printf("%sLocal controller not connected%s\n", COLOR_RED, COLOR_RESET);
            pthread_mutex_unlock(&status_mutex);
            return -1;
        }

        strncpy(local_status.pending_command, state, sizeof(local_status.pending_command) - 1);
        strncpy(local_status.command_timestamp, ts, sizeof(local_status.command_timestamp) - 1);
        int coid = local_status.coid;
        pthread_mutex_unlock(&status_mutex);

        controller_reply_t reply;
        if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1) {
            printf("%sFailed to send command to local controller%s\n", COLOR_RED, COLOR_RESET);
            pthread_mutex_lock(&status_mutex);
            local_status.connected = 0;
            local_status.coid = -1;
            local_status.pending_command[0] = '\0';
            pthread_mutex_unlock(&status_mutex);
            return -1;
        }

        printf("%sCommand sent to local controller%s\n", COLOR_GREEN, COLOR_RESET);

    } else if (strcmp(target, "train") == 0) {
        msg.controller = CONTROLLER_TRAIN;

        if (strcmp(state, "BLOCK") == 0) {
            msg.state.crossing = CROSSING_BLOCK;
        } else if (strcmp(state, "OPEN") == 0) {
            msg.state.crossing = CROSSING_OPEN;
        } else {
            printf("%sInvalid train state. Use: BLOCK, OPEN%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        pthread_mutex_lock(&status_mutex);
        if (!train_status.connected || train_status.coid == -1) {
            printf("%sTrain controller not connected%s\n", COLOR_RED, COLOR_RESET);
            pthread_mutex_unlock(&status_mutex);
            return -1;
        }

        strncpy(train_status.pending_command, state, sizeof(train_status.pending_command) - 1);
        strncpy(train_status.command_timestamp, ts, sizeof(train_status.command_timestamp) - 1);
        int coid = train_status.coid;
        pthread_mutex_unlock(&status_mutex);

        controller_reply_t reply;
        if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1) {
            printf("%sFailed to send command to train controller%s\n", COLOR_RED, COLOR_RESET);
            pthread_mutex_lock(&status_mutex);
            train_status.connected = 0;
            train_status.coid = -1;
            train_status.pending_command[0] = '\0';
            pthread_mutex_unlock(&status_mutex);
            return -1;
        }

        printf("%sCommand sent to train controller%s\n", COLOR_GREEN, COLOR_RESET);

    } else {
        printf("%sUnknown target: %s. Use: local or train%s\n", COLOR_RED, target, COLOR_RESET);
        return -1;
    }

    ui_needs_update = 1;
    return 0;
}

// Thread to handle incoming messages from controllers
void* message_handler(void *arg) {
    name_attach_t *attach = (name_attach_t *)arg;
    controller_msg_t msg;
    controller_reply_t reply;
    int rcvid;
    struct _msg_info info;

    while (1) {
        rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            continue;
        }

        if (rcvid == 0) {
            // Pulse - could be connection/disconnection notification
            continue;
        }

        // Handle status update from controllers
        if (msg.type == MSG_STATUS_UPDATE) {
            pthread_mutex_lock(&status_mutex);

            if (msg.controller == CONTROLLER_LOCAL) {
                local_status.connected = 1;
                local_status.state.light = msg.state.light;
                strncpy(local_status.last_update, msg.timestamp, sizeof(local_status.last_update) - 1);
                local_status.pending_command[0] = '\0';  // Clear pending on update

                // Store connection if not already
                if (local_status.coid == -1) {
                    // Get connection to reply back
                    local_status.coid = ConnectAttach(info.nd, info.pid, info.chid, _NTO_SIDE_CHANNEL, 0);
                }
            } else if (msg.controller == CONTROLLER_TRAIN) {
                train_status.connected = 1;
                train_status.state.crossing = msg.state.crossing;
                strncpy(train_status.last_update, msg.timestamp, sizeof(train_status.last_update) - 1);
                train_status.pending_command[0] = '\0';  // Clear pending on update

                // Store connection if not already
                if (train_status.coid == -1) {
                    train_status.coid = ConnectAttach(info.nd, info.pid, info.chid, _NTO_SIDE_CHANNEL, 0);
                }
            }

            ui_needs_update = 1;
            pthread_mutex_unlock(&status_mutex);

            reply.status = 0;
            MsgReply(rcvid, 0, &reply, sizeof(reply));
        } else {
            reply.status = -1;
            MsgReply(rcvid, 0, &reply, sizeof(reply));
        }
    }

    return NULL;
}

// Thread to try connecting to controllers
void* connection_thread(void *arg) {
    (void)arg;

    while (1) {
        // Try connecting to local controller
        pthread_mutex_lock(&status_mutex);
        int local_connected = local_status.connected;
        pthread_mutex_unlock(&status_mutex);

        if (!local_connected) {
            int coid = name_open(LOCAL_SERVICE_NAME, NAME_FLAG_ATTACH_GLOBAL);
            if (coid != -1) {
                pthread_mutex_lock(&status_mutex);
                local_status.coid = coid;
                local_status.connected = 1;
                strncpy(local_status.vm_name, VM_LOCAL, sizeof(local_status.vm_name) - 1);
                ui_needs_update = 1;
                pthread_mutex_unlock(&status_mutex);
            }
        }

        // Try connecting to train controller
        pthread_mutex_lock(&status_mutex);
        int train_connected = train_status.connected;
        pthread_mutex_unlock(&status_mutex);

        if (!train_connected) {
            int coid = name_open(TRAIN_SERVICE_NAME, NAME_FLAG_ATTACH_GLOBAL);
            if (coid != -1) {
                pthread_mutex_lock(&status_mutex);
                train_status.coid = coid;
                train_status.connected = 1;
                strncpy(train_status.vm_name, VM_TRAIN, sizeof(train_status.vm_name) - 1);
                ui_needs_update = 1;
                pthread_mutex_unlock(&status_mutex);
            }
        }

        sleep(2);
    }

    return NULL;
}

// Thread to refresh UI periodically
void* ui_refresh_thread(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&status_mutex);
        int needs_update = ui_needs_update;
        pthread_mutex_unlock(&status_mutex);

        if (needs_update) {
            display_ui();
        }

        sleep(1);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Initialize status
    memset(&local_status, 0, sizeof(local_status));
    memset(&train_status, 0, sizeof(train_status));
    local_status.coid = -1;
    train_status.coid = -1;
    local_status.state.light = LIGHT_GREEN;      // Default
    train_status.state.crossing = CROSSING_OPEN; // Default
    strncpy(local_status.vm_name, VM_LOCAL, sizeof(local_status.vm_name) - 1);
    strncpy(train_status.vm_name, VM_TRAIN, sizeof(train_status.vm_name) - 1);

    // Create channel for receiving messages
    name_attach_t *attach = name_attach(NULL, CENTRAL_SERVICE_NAME, NAME_FLAG_ATTACH_GLOBAL);
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

    // Start UI refresh thread
    pthread_t ui_thread;
    if (pthread_create(&ui_thread, NULL, ui_refresh_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create UI refresh thread\n");
        return EXIT_FAILURE;
    }

    // Initial UI display
    display_ui();

    // Main loop: read commands
    char cmd[64];
    while (1) {
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            // Remove newline
            cmd[strcspn(cmd, "\n")] = '\0';

            // Skip empty commands
            if (strlen(cmd) == 0) {
                display_ui();
                continue;
            }

            // Check for quit
            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
                printf("Exiting...\n");
                break;
            }

            execute_command(cmd);
            sleep(1);  // Brief pause to allow status update
            display_ui();
        }
    }

    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
