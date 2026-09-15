#include "ui_ipc.h"
#include "version.h"

#include <errno.h>
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
        const char *prefix = color ? keyword_color(line, cursor, &length) : NULL;
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
    printf("Type help for commands; watch for live status; quit closes this UI.\n> ");
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
                    else if (!strcmp(command, "quit")) interrupted = 1;
                    else if (!strcmp(command, "watch")) {
                        watching = 1;
                        next_refresh = 0;
                        if (color) fputs("\033[?25l", stdout);
                    }
                    else if (*command) request_core(name, command, color);
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
