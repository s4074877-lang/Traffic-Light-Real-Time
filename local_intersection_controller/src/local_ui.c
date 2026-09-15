#include "local_process.h"

#include <poll.h>
#include <termios.h>

/* Terminal output shared by the live UI and read-only display. */
static const char *divider = "============================================================";

static const char *phase_name(unsigned phase) {
    const char *names[] = {"NS_GREEN", "NS_YELLOW", "EW_GREEN", "EW_YELLOW", "RAILWAY_HOLD"};
    return phase < 5 ? names[phase] : "UNKNOWN";
}

static const char *lamp_name(unsigned lamp) {
    const char *names[] = {"OFF", "RED", "YELLOW", "GREEN"};
    return lamp < 4 ? names[lamp] : "UNKNOWN";
}

static const char *traffic_mode(unsigned mode) {
    const char *names[] = {"FIXED", "SENSOR", "RAILWAY", "FAILSAFE"};
    return mode < 4 ? names[mode] : "UNKNOWN";
}

static const char *link_name(int connected) {
    return connected ? COLOR_GREEN "CONNECTED" COLOR_RESET :
                       COLOR_RED "DISCONNECTED" COLOR_RESET;
}

static const char *lamp_color(unsigned lamp) {
    if (lamp == LIGHT_GREEN) return COLOR_GREEN;
    if (lamp == LIGHT_YELLOW) return COLOR_YELLOW;
    if (lamp == LIGHT_RED) return COLOR_RED;
    return COLOR_RESET;
}

static void print_vehicle(const status_msg_t *s, direction_t direction, int width) {
    unsigned lamp = direction == DIR_NS ? s->ns_state : s->ew_state;
    int seconds = local_vehicle_seconds(s, direction);
    char value[20];
    if (seconds < 0) snprintf(value, sizeof(value), "%s --", lamp_name(lamp));
    else snprintf(value, sizeof(value), "%s %ds", lamp_name(lamp), seconds);
    printf("%s%s%s", lamp_color(lamp), value, COLOR_RESET);
    if (width > (int)strlen(value))
        printf("%*s", width - (int)strlen(value), "");
}

static void print_pedestrian_state(int walk) {
    printf("%s%s%s", walk ? COLOR_GREEN : COLOR_RED,
           walk ? "WALK" : "STOP", COLOR_RESET);
}

static void print_health(int fault) {
    printf("%s%s%s", fault ? COLOR_RED : COLOR_GREEN,
           fault ? "FAULT" : "OK", COLOR_RESET);
}

static void local_print_heading(int selected) {
    printf("\033[2J\033[H%s\n", divider);
    if (selected == INTERSECTION_ALL) puts("                 LOCAL CONTROLLERS I1-I6");
    else printf("                    LOCAL CONTROLLER I%d\n", selected + 1);
    puts(divider);
    if (selected == INTERSECTION_ALL) {
        printf("%-3s%-9s%-12s%-12s%-8s%-9s%s\n",
               "ID", "MODE", "NS", "EW", "PED N/E", "CARS N/E", "HEALTH");
        puts(divider);
    }
}

static void print_pedestrian(const char *direction, int walk, int request, unsigned remaining) {
    printf("  %s [", direction);
    print_pedestrian_state(walk);
    if (walk && remaining > PED_WALK_END_SEC)
        printf(" %u sec", remaining - PED_WALK_END_SEC);
    printf("] request [%s%s%s]\n", request ? COLOR_YELLOW : COLOR_RESET,
           request ? "PENDING" : "NONE", COLOR_RESET);
}

static void local_print_panel(const core_reply_t *reply, int compact) {
    const status_msg_t *s = &reply->status;
    if (compact) {
        printf("I%u %-8s ", s->intersection_id + 1, traffic_mode(s->mode));
        print_vehicle(s, DIR_NS, 12);
        print_vehicle(s, DIR_EW, 12);
        printf("%s%s%s/%s%s%s     %-2u/%-2u    ",
               s->pedestrian_ns ? COLOR_GREEN : COLOR_RED, s->pedestrian_ns ? "W" : "S", COLOR_RESET,
               s->pedestrian_ew ? COLOR_GREEN : COLOR_RED, s->pedestrian_ew ? "W" : "S", COLOR_RESET,
               s->sensor_ns_count, s->sensor_ew_count);
        print_health(s->fault_active);
        putchar('\n');
    } else {
        printf("Service [%s] mode [%s]\n", reply->service_name, reply->global_mode ? "GLOBAL" : "LOCAL");
        printf("Connected to central_controller [%s]\n", link_name(reply->central_connected));
        printf("Connected to train_controller [%s]\n", link_name(reply->train_connected));
        puts(divider);
        printf("Last message receive central [%s]\n", reply->last_recv_central[0] ? reply->last_recv_central : "N/A");
        printf("Last message receive train [%s]\n", reply->last_recv_train[0] ? reply->last_recv_train : "N/A");
        puts(divider);
        printf("Traffic mode [%s]\nPhase [%s] remaining [%u sec]\n",
               traffic_mode(s->mode), phase_name(s->phase), s->time_remaining);
        printf("Telemetry seq [%u] last command [%u] health [", s->status_sequence,
               s->last_command_id);
        print_health(s->fault_active);
        puts("]");
        printf("Sim time [%02d:%02d:%02d]\n", reply->sim_seconds / 3600,
               reply->sim_seconds / 60 % 60, reply->sim_seconds % 60);
        puts("Vehicle lights:");
        printf("  NS ["); print_vehicle(s, DIR_NS, 0); puts("]");
        printf("  EW ["); print_vehicle(s, DIR_EW, 0); puts("]");
        puts("Pedestrian:");
        print_pedestrian("NS", s->pedestrian_ns, s->pedestrian_ns_request, s->time_remaining);
        print_pedestrian("EW", s->pedestrian_ew, s->pedestrian_ew_request, s->time_remaining);
        printf("Sensors:\n  NS cars [%u]\n  EW cars [%u]\n", s->sensor_ns_count, s->sensor_ew_count);
        printf("Train: %s%s%s\n", s->train_active ? COLOR_RED :
               (s->train_pending || s->railway_preempt) ? COLOR_YELLOW : COLOR_GREEN,
               s->train_active ? "active" : s->train_pending ? "pending" :
               s->railway_preempt ? "recovery" : "none active", COLOR_RESET);
        if (s->fault_active) printf(COLOR_RED "Fault type [%u] severity [%u]" COLOR_RESET "\n", s->fault_type, s->fault_severity);
    }
    if (!compact) puts(divider);
}

static void local_print_commands(void) {
    puts("View: view I1..I6 | view all");
    puts("Commands: m mode | n ped-NS | e ped-EW | x sensor-NS");
    puts("          z sensor-EW | 1 train-line1 | 2 train-line2");
    puts("          p peak | o offpeak | l light | c train-clear");
    puts("          r reset | q quit");
    puts(divider);
}

/* Interactive terminal: select a controller, then send it commands. */
typedef struct {
    int selected;
    int connections[NUM_INTERSECTIONS];
    char names[NUM_INTERSECTIONS][LOCAL_SERVICE_NAME_MAX + 8];
    core_reply_t snapshots[NUM_INTERSECTIONS];
    int online[NUM_INTERSECTIONS];
    char input[64];
    size_t used;
    char notice[128];
} console_t;

static void refresh_snapshots(console_t *ui) {
    int i;
    for (i = 0; i < NUM_INTERSECTIONS && !local_stopping; ++i) {
        core_request_t request = {0};
        if (ui->selected != INTERSECTION_ALL && ui->selected != i) continue;
        request.operation = CORE_SNAPSHOT;
        ui->online[i] = core_call(&ui->connections[i], ui->names[i], &request,
                                 &ui->snapshots[i]) == 0 &&
                        ui->snapshots[i].result.status == 0 &&
                        ui->snapshots[i].status.intersection_id == i;
    }
}

static void draw_console(const console_t *ui) {
    int i;
    local_print_heading(ui->selected);
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        if (ui->selected != INTERSECTION_ALL && ui->selected != i) continue;
        if (ui->online[i]) local_print_panel(&ui->snapshots[i], ui->selected == INTERSECTION_ALL);
        else printf("I%d [" COLOR_RED "OFFLINE" COLOR_RESET "] No current state available.\n", i + 1);
    }
    if (ui->selected == INTERSECTION_ALL) {
        puts(divider);
        puts("Pedestrian: W=WALK S=STOP | Select I1-I6 for details");
    }
    local_print_commands();
    printf("%s\n\nMessage: %s", ui->notice, ui->input);
    fflush(stdout);
}

static int submit_command(console_t *ui) {
    core_request_t request = {0};
    core_reply_t reply;
    int target = ui->selected;

    if (!strcmp(ui->input, "q") || !strcmp(ui->input, "quit") ||
        !strcmp(ui->input, "exit")) return 0;
    if (local_ui_parse_view(ui->input, &ui->selected)) {
        snprintf(ui->notice, sizeof(ui->notice), "View changed.");
        return 1;
    }
    if (!ui->input[0]) return 1;
    if (!valid_input(ui->input)) {
        snprintf(ui->notice, sizeof(ui->notice), "Unknown command. Use view I1..I6, view all or a Local input command.");
        return 1;
    }
    if (target == INTERSECTION_ALL) {
        snprintf(ui->notice, sizeof(ui->notice), "Select an intersection first, for example: view I3");
        return 1;
    }
    /* Check the target's current identity before sending a non-retried input. */
    refresh_snapshots(ui);
    if (!ui->online[target]) {
        snprintf(ui->notice, sizeof(ui->notice), "I%d is offline or has a different ID; nothing sent.", target + 1);
        return 1;
    }
    request.operation = CORE_INPUT;
    snprintf(request.command, sizeof(request.command), "%s", ui->input);
    if (core_call(&ui->connections[target], ui->names[target], &request, &reply) != 0) {
        snprintf(ui->notice, sizeof(ui->notice), "I%d did not acknowledge; command was not retried.", target + 1);
    } else {
        snprintf(ui->notice, sizeof(ui->notice), "I%d: %s [%s]", target + 1,
                 reply.result.status == 0 ? "OK" : "Rejected", ui->input);
    }
    return 1;
}

int run_ui(const char *core_name, uint8_t intersection_id, int show_all) {
    console_t ui = {0};
    struct termios original, editing;
    int terminal = isatty(STDIN_FILENO);
    int running = 1;
    int redraw = 1;
    int result = EXIT_SUCCESS;
    uint64_t next_refresh = 0;
    int i;

    ui.selected = show_all ? INTERSECTION_ALL : intersection_id;
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        ui.connections[i] = -1;
        snprintf(ui.names[i], sizeof(ui.names[i]), "traffic_local_I%d_core", i + 1);
    }
    if (!show_all) snprintf(ui.names[intersection_id], sizeof(ui.names[0]), "%s", core_name);
    snprintf(ui.notice, sizeof(ui.notice), "view I3 | view all | m n e x z 1 2 p o l c r | q exits");

    /* Keep the typed line in our own buffer so live refresh cannot erase it. */
    if (terminal) {
        if (tcgetattr(STDIN_FILENO, &original) == -1) return EXIT_FAILURE;
        editing = original;
        editing.c_lflag &= ~(ICANON | ECHO);
        editing.c_cc[VMIN] = 1;
        editing.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &editing) == -1) return EXIT_FAILURE;
    }

    while (running && !local_stopping) {
        struct pollfd input = { STDIN_FILENO, POLLIN, 0 };
        uint64_t now = monotonic_ns();
        if (now >= next_refresh) {
            refresh_snapshots(&ui);
            next_refresh = monotonic_ns() + LOCAL_TICK_NS;
            redraw = 1;
        }
        if (redraw) { draw_console(&ui); redraw = 0; }
        int ready = poll(&input, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            result = EXIT_FAILURE;
            break;
        }
        if (ready && (input.revents & (POLLIN | POLLHUP))) {
            unsigned char character;
            ssize_t count = read(STDIN_FILENO, &character, 1);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                result = EXIT_FAILURE;
                break;
            }
            if (character == '\n' || character == '\r') {
                running = submit_command(&ui);
                ui.used = 0;
                ui.input[0] = '\0';
                next_refresh = 0;
            } else if (character == 4 && ui.used == 0) {
                break;
            } else if (character == 8 || character == 127) {
                if (ui.used > 0) ui.input[--ui.used] = '\0';
            } else if (character >= 32 && character <= 126 && ui.used + 1 < sizeof(ui.input)) {
                ui.input[ui.used++] = (char)character;
                ui.input[ui.used] = '\0';
            }
            redraw = 1;
        } else if (ready && (input.revents & (POLLERR | POLLNVAL))) {
            result = EXIT_FAILURE;
            break;
        }
    }

    if (terminal) tcsetattr(STDIN_FILENO, TCSANOW, &original);
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        if (ui.connections[i] != -1) name_close(ui.connections[i]);
    }
    puts("\nLocal UI closed.");
    return result;
}

/* Optional standalone roles for separate display and input terminals. */
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
                printf("I%d [" COLOR_RED "OFFLINE" COLOR_RESET "] No current state available.\n", i + 1);
            } else {
                printf("%s: " COLOR_RED "OFFLINE" COLOR_RESET "; no current lamp state\n", names[i]);
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
