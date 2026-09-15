#include "local_process.h"

#include <poll.h>
#include <termios.h>

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
        else printf("I%d [OFFLINE] No current state available.\n", i + 1);
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
