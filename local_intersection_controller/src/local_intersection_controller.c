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
    printf("Usage: %s [-l | -g] [-i I1..I6] [-n service] [--role core|comm|io|display|ui]\n", prog);
    printf("  --role  Separate process responsibility; no role/ID/name starts all Local processes\n");
    printf("  --all   Show I1..I6 together with --role display (standard service names)\n");
    printf("  --role ui  Interactive view switching and input; starts with all intersections\n");
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
    int display_all = 0;
    char service_name[LOCAL_SERVICE_NAME_MAX];
    const char *service_arg = NULL;
    const char *role = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-l") == 0) {
            mode = CONN_MODE_LOCAL;
        } else if (strcmp(argv[i], "-g") == 0) {
            mode = CONN_MODE_GLOBAL;
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--all") == 0) {
            display_all = 1;
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

    if (role == NULL) {
        role = (!explicit_intersection && service_arg == NULL) ? "launch" : "core";
    }

    if (display_all && ((strcmp(role, "display") != 0 && strcmp(role, "ui") != 0) ||
                        explicit_intersection || service_arg != NULL)) {
        fprintf(stderr, "Use --all with --role display or ui, without -i or -n\n");
        return EXIT_FAILURE;
    }

    default_service_name(intersection_id, explicit_intersection,
                         service_name, sizeof(service_name));
    if (service_arg != NULL) {
        snprintf(service_name, sizeof(service_name), "%s", service_arg);
    }

    if (strcmp(role, "ui") == 0 && !explicit_intersection && service_arg == NULL) {
        display_all = 1;
    }

    return local_process_run(role, mode, intersection_id, service_name, display_all);
}
