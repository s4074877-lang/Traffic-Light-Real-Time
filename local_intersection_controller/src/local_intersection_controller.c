#include "local_controller.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    printf("Usage: %s [-l | -g]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
}

int main(int argc, char *argv[]) {
    connection_mode_t mode = connection_parse_args(argc, argv);
    name_attach_t *attach = NULL;
    receive_context_t recv_ctx;
    pthread_t msg_thread;
    pthread_t conn_thread;
    pthread_t ui_thread;
    pthread_t hb_thread;
    pthread_t traffic_thread_id;
    pthread_t status_thread_id;
    char cmd[64];
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    local_state_init(mode);

    attach = connection_register_service(LOCAL_SERVICE_NAME, mode);
    if (attach == NULL) {
        local_state_destroy();
        return EXIT_FAILURE;
    }

    local_receive_init(&recv_ctx, attach);

    if (pthread_create(&msg_thread, NULL, message_handler_thread, &recv_ctx) != 0) {
        fprintf(stderr, "Failed to create message handler thread\n");
        connection_unregister_service(attach);
        local_state_destroy();
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

    if (pthread_create(&traffic_thread_id, NULL, traffic_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create traffic thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&status_thread_id, NULL, status_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create status thread\n");
        return EXIT_FAILURE;
    }

    display_ui();

    while (1) {
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            cmd[strcspn(cmd, "\n")] = '\0';

            if (strlen(cmd) == 0) {
                display_ui();
                continue;
            }

            if (strcmp(cmd, "q") == 0 ||
                strcmp(cmd, "quit") == 0 ||
                strcmp(cmd, "exit") == 0) {
                printf("Exiting...\n");
                break;
            }

            execute_command(cmd);
            sleep(1);
            display_ui();
        }
    }

    connection_unregister_service(attach);
    local_state_destroy();
    return EXIT_SUCCESS;
}
