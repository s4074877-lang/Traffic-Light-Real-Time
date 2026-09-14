#include "local_controller.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef LOCAL_INTERSECTION_ID
#define LOCAL_INTERSECTION_ID I1
#endif

static int service_name_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= LOCAL_SERVICE_NAME_MAX) {
        return 0;
    }

    for (i = 0; name[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)name[i];
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        int digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '_' && c != '-' && c != '.') {
            return 0;
        }
    }

    return 1;
}

static int parse_intersection_id(const char *text, uint8_t *intersection_id) {
    const char *number = text;

    if (text == NULL || intersection_id == NULL || text[0] == '\0') {
        return 0;
    }

    if (text[0] == 'I' || text[0] == 'i') {
        number = text + 1;
    }

    if (number[0] >= '1' && number[0] <= '6' && number[1] == '\0') {
        *intersection_id = (uint8_t)(number[0] - '1');
        return 1;
    }

    return 0;
}

static void default_service_name(uint8_t intersection_id, int explicit_id,
                                 char *buffer, size_t size) {
    if (!explicit_id && intersection_id == I1) {
        snprintf(buffer, size, "%s", LOCAL_SERVICE_NAME);
        return;
    }

    snprintf(buffer, size, "traffic_local_I%u", (unsigned)intersection_id + 1);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [-l | -g] [-i I1..I6] [-n service]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
    printf("  -i  Runtime intersection ID for this Local instance\n");
    printf("  -n  Name-service endpoint to publish\n");
    printf("      Default: traffic_local_controller for legacy I1,\n");
    printf("               traffic_local_I# when -i is supplied\n");
}

int main(int argc, char *argv[]) {
    connection_mode_t mode = CONN_MODE_GLOBAL;
    uint8_t intersection_id = LOCAL_INTERSECTION_ID;
    int explicit_intersection = 0;
    char service_name[LOCAL_SERVICE_NAME_MAX];
    const char *service_arg = NULL;
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
        } else if (strcmp(argv[i], "-l") == 0) {
            mode = CONN_MODE_LOCAL;
        } else if (strcmp(argv[i], "-g") == 0) {
            mode = CONN_MODE_GLOBAL;
        } else if ((strcmp(argv[i], "-i") == 0 ||
                    strcmp(argv[i], "--intersection") == 0) &&
                   i + 1 < argc) {
            if (!parse_intersection_id(argv[++i], &intersection_id)) {
                fprintf(stderr, "Intersection must be I1..I6\n");
                return EXIT_FAILURE;
            }
            explicit_intersection = 1;
        } else if ((strcmp(argv[i], "-n") == 0 ||
                    strcmp(argv[i], "--service") == 0) &&
                   i + 1 < argc) {
            service_arg = argv[++i];
            if (!service_name_valid(service_arg)) {
                fprintf(stderr, "Service name must be 1..63 chars: A-Z a-z 0-9 _ - .\n");
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (intersection_id >= NUM_INTERSECTIONS) {
        fprintf(stderr, "Configured intersection must be I1..I6\n");
        return EXIT_FAILURE;
    }

    default_service_name(intersection_id, explicit_intersection,
                         service_name, sizeof(service_name));
    if (service_arg != NULL) {
        snprintf(service_name, sizeof(service_name), "%s", service_arg);
    }

    local_state_init(mode, intersection_id, service_name);

    printf("Local instance I%u using service '%s'\n",
           (unsigned)intersection_id + 1, service_name);

    attach = connection_register_service(service_name, mode);
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
