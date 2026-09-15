#include "ui_ipc.h"
#include "version.h"

#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

#define UI_RESET       "\033[0m"
#define UI_DIM_CYAN    "\033[2;36m"
#define UI_RED         "\033[1;31m"
#define UI_GREEN       "\033[1;32m"
#define UI_YELLOW      "\033[1;33m"
#define UI_BLUE        "\033[1;34m"
#define UI_MAGENTA     "\033[1;35m"
#define UI_CYAN        "\033[1;36m"
#define UI_WHITE       "\033[1;37m"

static int word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int keyword_match(const char *start, const char *cursor, const char *keyword) {
    size_t length = strlen(keyword);
    if (strncmp(cursor, keyword, length) != 0) return 0;
    if (cursor != start && word_char((unsigned char)cursor[-1])) return 0;
    if (word_char((unsigned char)keyword[length - 1]) &&
        word_char((unsigned char)cursor[length])) return 0;
    return 1;
}

static const char *keyword_color(const char *start, const char *cursor, size_t *length) {
    static const struct { const char *text; const char *color; } tokens[] = {
        {"CONFLICTING GREENS", UI_RED}, {"DISCONNECTED", UI_RED},
        {"HEARTBEAT OK", UI_GREEN}, {"WAITING UPDATE", UI_YELLOW},
        {"AT CROSSING", UI_MAGENTA}, {"RAIL HOLD", UI_RED},
        {"OPERATIONAL", UI_GREEN}, {"APPROACHING", UI_MAGENTA},
        {"DEGRADED", UI_RED}, {"OFFLINE", UI_RED}, {"LOST", UI_RED},
        {"FAULT", UI_RED}, {"FAILSAFE", UI_RED}, {"NO REPORT", UI_YELLOW}, {"NO DATA", UI_YELLOW},
        {"STOP", UI_RED}, {"RED", UI_RED},
        {"CLOSED", UI_YELLOW}, {"CLOSING", UI_YELLOW},
        {"STALE", UI_YELLOW}, {"WAITING", UI_YELLOW}, {"UNKNOWN", UI_YELLOW},
        {"YELLOW", UI_YELLOW}, {"HOLD", UI_YELLOW}, {"STANDBY", UI_YELLOW},
        {"CONNECTED", UI_GREEN}, {"HEALTHY", UI_GREEN}, {"CURRENT", UI_GREEN},
        {"ONLINE", UI_GREEN}, {"CLEAR", UI_GREEN}, {"GREEN", UI_GREEN},
        {"WALK", UI_GREEN}, {"OPENING", UI_GREEN}, {"OPEN", UI_GREEN},
        {"UP", UI_GREEN}, {"RUN", UI_GREEN}, {"READY", UI_GREEN},
        {"RAILWAY", UI_MAGENTA}, {"TRAIN", UI_MAGENTA}, {"ACTIVE", UI_MAGENTA},
        {"P1", UI_MAGENTA}, {"P2", UI_MAGENTA}, {"P3", UI_MAGENTA},
        {"SENSORS", UI_BLUE}, {"SENSOR", UI_BLUE}, {"FIXED", UI_CYAN}, {"GLOBAL", UI_CYAN},
        {"LOCAL", UI_CYAN}, {"SNAPSHOT", UI_WHITE}, {"LIVE VIEW", UI_WHITE},
        {"CENTRAL CONTROL ROOM", UI_CYAN}, {"INTERSECTIONS", UI_CYAN},
        {"CONNECTIONS", UI_CYAN}, {"RECENT EVENTS", UI_CYAN}, {"CONTROLS", UI_CYAN}
    };
    size_t i;
    for (i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        if (keyword_match(start, cursor, tokens[i].text)) {
            *length = strlen(tokens[i].text);
            return tokens[i].color;
        }
    }
    *length = 0;
    return NULL;
}

static const char *inline_key_color(const char *cursor, size_t *length) {
    const char *end;
    if (!cursor || cursor[0] != '[') return NULL;
    end = strchr(cursor, ']');
    if (!end || end - cursor < 2 || end - cursor > 3) return NULL;
    *length = (size_t)(end - cursor + 1);
    return cursor[1] == '0' ? UI_RED : UI_YELLOW;
}

static void render_line(const char *line, int color) {
    const char *cursor = line;
    if (color && (*line == '+' || (line[0] == '|' && line[1] == '-' && line[2] == '-'))) {
        fputs(UI_DIM_CYAN, stdout);
        fputs(line, stdout);
        fputs(UI_RESET, stdout);
        return;
    }
    while (*cursor) {
        size_t length = 0;
        const char *prefix = color ? inline_key_color(cursor, &length) : NULL;
        if (!prefix) prefix = color ? keyword_color(line, cursor, &length) : NULL;
        if (prefix && length) {
            fputs(prefix, stdout);
            fwrite(cursor, 1, length, stdout);
            fputs(UI_RESET, stdout);
            cursor += length;
        } else {
            unsigned char c = (unsigned char)*cursor++;
            if (c >= 32 || c == '\t') putchar(c);
        }
    }
}

static void render(char *text, int color) {
    char *line = text;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        render_line(line, color);
        if (!end) break;
        putchar('\n');
        line = end + 1;
    }
}

static int request_core(const char *name, const char *request, int color) {
    char output[CENTRAL_UI_RESPONSE_SIZE];
    int status = 0;
    if (central_ui_client_request(name, request, output, sizeof(output), &status) != 0) {
        fprintf(stderr, "Central UI request failed: %s.\n", strerror(errno));
        if (strcmp(request, "status") && strcmp(request, "help") &&
            strcmp(request, "commands") && strcmp(request, "faults") && strcmp(request, "events"))
            fprintf(stderr, "Receipt is unknown; inspect commands and status before retrying.\n");
        return -1;
    }
    render(output, color);
    return status;
}


static const char *shortcut_color(const char *key, size_t length) {
    if (length < 3 || key[0] != '[' || key[length - 1] != ']') return UI_WHITE;
    switch ((unsigned char)toupper((unsigned char)key[1])) {
        case 'F': return UI_CYAN;     /* Fixed mode */
        case 'S': return UI_BLUE;     /* Sensor mode */
        case 'A': return UI_GREEN;    /* Start simulation */
        case 'X': return UI_YELLOW;   /* Stop simulation */
        case 'T': return UI_MAGENTA;  /* Train */
        case '0': return UI_RED;      /* Quit */
        case '1': case '2': case '3': case '4': case '5': return UI_GREEN;
        default: return UI_WHITE;
    }
}

#define QUICK_MENU_TEXT_WIDTH 74

static void print_menu_border(int color) {
    int i;
    if (color) fputs(UI_DIM_CYAN, stdout);
    putchar('+');
    for (i = 0; i < QUICK_MENU_TEXT_WIDTH + 2; ++i) putchar('-');
    putchar('+');
    if (color) fputs(UI_RESET, stdout);
    putchar('\n');
}

static void print_menu_text(const char *text, int color) {
    size_t length = strlen(text), i = 0;
    if (length > QUICK_MENU_TEXT_WIDTH) length = QUICK_MENU_TEXT_WIDTH;
    if (color) fputs(UI_DIM_CYAN, stdout);
    fputs("| ", stdout);
    if (color) fputs(UI_RESET, stdout);

    while (i < length) {
        if (text[i] == '[') {
            size_t end = i + 1;
            while (end < length && text[end] != ']') ++end;
            if (end < length && text[end] == ']') {
                size_t key_length = end - i + 1;
                if (color) fputs(shortcut_color(text + i, key_length), stdout);
                fwrite(text + i, 1, key_length, stdout);
                if (color) fputs(UI_RESET, stdout);
                i = end + 1;
                continue;
            }
        }
        putchar((unsigned char)text[i]);
        ++i;
    }

    for (i = length; i < QUICK_MENU_TEXT_WIDTH; ++i) putchar(' ');
    if (color) fputs(UI_DIM_CYAN, stdout);
    fputs(" |", stdout);
    if (color) fputs(UI_RESET, stdout);
    putchar('\n');
}

static void print_menu_center(const char *text, int color) {
    char line[QUICK_MENU_TEXT_WIDTH + 1];
    size_t length = strlen(text);
    size_t left;
    if (length > QUICK_MENU_TEXT_WIDTH) length = QUICK_MENU_TEXT_WIDTH;
    memset(line, ' ', QUICK_MENU_TEXT_WIDTH);
    line[QUICK_MENU_TEXT_WIDTH] = '\0';
    left = (QUICK_MENU_TEXT_WIDTH - length) / 2;
    memcpy(line + left, text, length);
    print_menu_text(line, color);
}

static void print_quick_menu(int color) {
    print_menu_border(color);
    print_menu_center("OPERATOR QUICK MENU  -  TYPE [KEY] THEN PRESS ENTER", color);
    print_menu_border(color);

    print_menu_text("VIEW / MONITOR", color);
    print_menu_text("[L] Live Dashboard             [S] Status Snapshot", color);
    print_menu_text("[E] Recent Events              [F] Active Faults", color);
    print_menu_text("[C] Command History", color);
    print_menu_border(color);

    print_menu_text("TRAFFIC MODE  -  SELECT INTERSECTION", color);
    print_menu_text("FIXED : [F1] I1  [F2] I2  [F3] I3  [F4] I4  [F5] I5  [F6] I6", color);
    print_menu_text("        [FA] ALL INTERSECTIONS", color);
    print_menu_text("SENSOR: [S1] I1  [S2] I2  [S3] I3  [S4] I4  [S5] I5  [S6] I6", color);
    print_menu_text("        [SA] ALL INTERSECTIONS", color);
    print_menu_border(color);

    print_menu_text("TRAFFIC SIMULATION", color);
    print_menu_text("START : [A1] I1  [A2] I2  [A3] I3  [A4] I4  [A5] I5  [A6] I6", color);
    print_menu_text("        [AA] START ALL", color);
    print_menu_text("STOP  : [X1] I1  [X2] I2  [X3] I3  [X4] I4  [X5] I5  [X6] I6", color);
    print_menu_text("        [XA] STOP ALL", color);
    print_menu_border(color);

    print_menu_text("RAILWAY / TRAIN", color);
    print_menu_text("[TU] Train UP       [TD] Train DOWN       [TS] Train Status", color);
    print_menu_border(color);

    print_menu_text("DISPLAY / HELP", color);
    print_menu_text("[H] Full Help       [M] Show Menu         [0] Quit Display", color);
    print_menu_border(color);
    print_menu_text("Example: type [S3] + Enter  ->  set I3 to SENSOR mode", color);
    print_menu_border(color);
}

/* Translate presentation-friendly shortcuts into the existing Central command
 * language. Returns 1 when translated, 0 when the input should be forwarded
 * unchanged, 2 for local UI actions (menu/watch/quit). */
static int translate_shortcut(const char *input, char *output, size_t size,
                              int *start_watch, int *quit_ui, int *show_menu) {
    char key[32];
    size_t i, length = strlen(input);
    if (length >= sizeof(key)) return 0;
    for (i = 0; i <= length; ++i) key[i] = (char)toupper((unsigned char)input[i]);

    *start_watch = *quit_ui = *show_menu = 0;

    /* Presentation shortcuts. Numeric aliases remain accepted for compatibility. */
    if (!strcmp(key, "L") || !strcmp(key, "1")) { *start_watch = 1; return 2; }
    if (!strcmp(key, "S") || !strcmp(key, "2")) { snprintf(output, size, "status"); return 1; }
    if (!strcmp(key, "E") || !strcmp(key, "3")) { snprintf(output, size, "events"); return 1; }
    if (!strcmp(key, "F") || !strcmp(key, "4")) { snprintf(output, size, "faults"); return 1; }
    if (!strcmp(key, "C") || !strcmp(key, "5")) { snprintf(output, size, "commands"); return 1; }
    if (!strcmp(key, "H")) { snprintf(output, size, "help"); return 1; }
    if (!strcmp(key, "M")) { *show_menu = 1; return 2; }
    if (!strcmp(key, "0") || !strcmp(key, "Q")) { *quit_ui = 1; return 2; }

    if (!strcmp(key, "FA")) { snprintf(output, size, "mode-fixed all"); return 1; }
    if (!strcmp(key, "SA")) { snprintf(output, size, "mode-sensor all"); return 1; }
    if (!strcmp(key, "AA")) { snprintf(output, size, "sim-start all"); return 1; }
    if (!strcmp(key, "XA")) { snprintf(output, size, "sim-stop all"); return 1; }
    if (!strcmp(key, "TU")) { snprintf(output, size, "train-cmd train-up"); return 1; }
    if (!strcmp(key, "TD")) { snprintf(output, size, "train-cmd train-down"); return 1; }
    if (!strcmp(key, "TS")) { snprintf(output, size, "train-cmd status"); return 1; }

    if (length == 2 && key[1] >= '1' && key[1] <= '6') {
        unsigned id = (unsigned)(key[1] - '0');
        if (key[0] == 'F') { snprintf(output, size, "mode-fixed I%u", id); return 1; }
        if (key[0] == 'S') { snprintf(output, size, "mode-sensor I%u", id); return 1; }
        if (key[0] == 'A') { snprintf(output, size, "sim-start I%u", id); return 1; }
        if (key[0] == 'X') { snprintf(output, size, "sim-stop I%u", id); return 1; }
    }

    return 0;
}

static void usage(const char *program) {
    printf("Usage: %s [-n local-service] [--no-color] [-c command]\n"
           "Separate Central display, build %s (%s %s).\n"
           "watch refreshes status once per second; Enter stops watch.\n"
           "status, help, commands, faults and events inspect the core.\n"
           "Other commands are forwarded once. quit closes only this UI.\n"
           "shutdown requests that the core stop.\n", program, CENTRAL_BUILD_VERSION, __DATE__, __TIME__);
}

int main(int argc, char **argv) {
    const char *name = CENTRAL_UI_SERVICE;
    const char *once = NULL;
    int color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
    int watching = 0, overflow = 0;
    size_t used = 0;
    char line[CENTRAL_UI_REQUEST_SIZE];
    uint64_t next_refresh = 0;
    struct sigaction action;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return EXIT_SUCCESS; }
        if (!strcmp(argv[i], "--no-color")) color = 0;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) once = argv[++i];
        else { usage(argv[0]); return EXIT_FAILURE; }
    }
    if (once) {
        if (!strcmp(once, "quit")) return EXIT_SUCCESS;
        return request_core(name, once, color) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return EXIT_FAILURE;
    printf("Central display %s (%s %s); service %s.\n", CENTRAL_BUILD_VERSION, __DATE__, __TIME__, name);
    request_core(name, "status", color);
    print_quick_menu(color);
    printf("> ");
    fflush(stdout);
    while (!interrupted) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        int ready;
        uint64_t now = monotonic_ns();
        if (watching && now >= next_refresh) {
            if (color) fputs("\033[2J\033[H", stdout);
            request_core(name, "status", color);
            printf("Live view: Enter stops refresh; Central continues if this UI exits.\n");
            fflush(stdout);
            next_refresh = monotonic_ns() + 1000000000ULL;
        }
        ready = poll(&input, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); return EXIT_FAILURE; }
        if (ready && (input.revents & (POLLIN | POLLHUP))) {
            char buffer[128];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (!count) break;
            if (count < 0) { if (errno == EINTR) continue; perror("read"); return EXIT_FAILURE; }
            for (ssize_t j = 0; j < count && !interrupted; ++j) {
                if (buffer[j] == '\n') {
                    char *command = line;
                    line[used] = '\0';
                    while (*command == ' ' || *command == '\t') ++command;
                    size_t length = strlen(command);
                    while (length && (command[length - 1] == '\r' || command[length - 1] == ' ' ||
                                      command[length - 1] == '\t')) command[--length] = '\0';
                    if (watching) {
                        watching = 0;
                        if (color) fputs("\033[?25h", stdout);
                        puts("Live view stopped.");
                    }
                    else if (overflow) puts("Command too long; request was not sent.");
                    else if (*command) {
                        char translated[CENTRAL_UI_REQUEST_SIZE];
                        int start_watch = 0, quit_ui = 0, show_menu = 0;
                        int shortcut = translate_shortcut(command, translated, sizeof(translated),
                                                          &start_watch, &quit_ui, &show_menu);
                        if (shortcut == 2) {
                            if (quit_ui) interrupted = 1;
                            else if (show_menu) print_quick_menu(color);
                            else if (start_watch) {
                                watching = 1;
                                next_refresh = 0;
                                if (color) fputs("\033[?25l", stdout);
                            }
                        } else if (shortcut == 1) request_core(name, translated, color);
                        else if (!strcmp(command, "quit")) interrupted = 1;
                        else if (!strcmp(command, "watch")) {
                            watching = 1;
                            next_refresh = 0;
                            if (color) fputs("\033[?25l", stdout);
                        }
                        else request_core(name, command, color);
                    }
                    used = 0;
                    overflow = 0;
                    if (!watching && !interrupted) printf("> ");
                    fflush(stdout);
                } else if (used + 1 < sizeof(line)) line[used++] = buffer[j];
                else overflow = 1;
            }
        } else if (ready && (input.revents & (POLLERR | POLLNVAL))) return EXIT_FAILURE;
    }
    if (color) fputs("\033[?25h\033[0m", stdout);
    return EXIT_SUCCESS;
}
