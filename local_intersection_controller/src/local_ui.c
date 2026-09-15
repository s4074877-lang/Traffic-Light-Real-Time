#include "local_process.h"

int run_display(const char *core_name, int display_all) {
    int connections[NUM_INTERSECTIONS];
    char names[NUM_INTERSECTIONS][LOCAL_SERVICE_NAME_MAX + 8];
    int count = display_all ? NUM_INTERSECTIONS : 1;
    int i;

    for (i = 0; i < count; ++i) {
        connections[i] = -1;
        if (display_all) {
            snprintf(names[i], sizeof(names[i]), "traffic_local_I%d_core", i + 1);
        } else {
            snprintf(names[i], sizeof(names[i]), "%s", core_name);
        }
    }

    while (!local_stopping) {
        core_reply_t replies[NUM_INTERSECTIONS];
        int online[NUM_INTERSECTIONS];

        /* Read all snapshots before drawing so a slow peer does not leave half a table. */
        for (i = 0; i < count; ++i) {
            core_request_t request = {0};
            request.operation = CORE_SNAPSHOT;
            online[i] = core_call(&connections[i], names[i], &request, &replies[i]) == 0 &&
                        replies[i].result.status == 0;
            if (display_all && online[i] && replies[i].status.intersection_id != i) {
                online[i] = 0;
            }
        }

        local_print_heading(display_all ? INTERSECTION_ALL :
                            online[0] ? replies[0].status.intersection_id : INTERSECTION_ALL);
        for (i = 0; i < count; ++i) {
            if (online[i]) {
                local_print_panel(&replies[i], display_all);
            } else if (display_all) {
                printf("I%d  OFFLINE  --     --       --  --     --     --    --     --\n", i + 1);
            } else {
                printf("%s: OFFLINE; no current lamp state\n", names[i]);
            }
        }
        fflush(stdout);
        sleep(1);
    }

    for (i = 0; i < count; ++i) {
        if (connections[i] != -1) {
            name_close(connections[i]);
        }
    }
    return EXIT_SUCCESS;
}

int run_input(const char *core_name) {
    int core = -1;
    char line[128];
    printf("Local input: m n e x z 1 2 p o l c r; q closes input only\n");
    while (!local_stopping && fgets(line, sizeof(line), stdin)) {
        core_request_t request = {0};
        core_reply_t reply;
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, "q") || !strcmp(line, "quit")) break;
        if (!line[0]) continue;
        if (strlen(line) >= sizeof(request.command)) {
            puts("Command too long");
            continue;
        }
        strcpy(request.command, line);
        if (!valid_input(request.command)) { puts("Unknown input command"); continue; }
        request.operation = CORE_INPUT;
        if (core_call(&core, core_name, &request, &reply) != 0)
            puts("Core unavailable; command not acknowledged (not retried)");
        else puts(reply.result.status == 0 ? "OK" : "Rejected");
    }
    if (core != -1) name_close(core);
    return EXIT_SUCCESS;
}
