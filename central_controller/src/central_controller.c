#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <stdarg.h>
#include <errno.h>
#include <stdatomic.h>

#include "ipc.h"
#include "commands.h"
#include "monitor.h"
#include "operator_policy.h"
#include "ui_ipc.h"
#include "version.h"

#define COMMAND_QUEUE_SIZE 32
#define COMMAND_HISTORY_SIZE 64
#define EVENT_QUEUE_SIZE 64
#define EVENT_TEXT_SIZE 192
#define COMMAND_MAX_WAIT_SEC 5
#define COMMAND_TEXT_SIZE 80
#define MAX_PEERS (NUM_INTERSECTIONS + 2)

typedef struct {
    test_message_t message;
    uint16_t id;
    uint64_t not_before;
} queued_command_t;

typedef enum {
    REQUEST_QUEUED,
    REQUEST_SENDING,
    REQUEST_ACCEPTED,
    REQUEST_REJECTED,
    REQUEST_UNCONFIRMED,
    REQUEST_CANCELED,
    REQUEST_EXPIRED
} request_state_t;

typedef struct {
    uint16_t id;
    unsigned target;
    unsigned peer;
    int automatic;
    mode_cmd_msg_t mode_request;
    request_state_t state;
    char description[COMMAND_TEXT_SIZE];
    char detail[96];
    uint64_t queued_at, sent_at, finished_at;
} command_record_t;

typedef struct {
    central_monitor_t monitor;
    central_link_t links[MAX_PEERS];
    queued_command_t queue[MAX_PEERS][COMMAND_QUEUE_SIZE];
    unsigned queue_head[MAX_PEERS], queue_count[MAX_PEERS];
    unsigned local_route[NUM_INTERSECTIONS], peer_count;
    int endpoint_connected[MAX_PEERS];
    uint64_t endpoint_probe[MAX_PEERS];
    unsigned endpoint_epoch[MAX_PEERS];
    command_record_t commands[COMMAND_HISTORY_SIZE];
    unsigned command_next;
    uint16_t next_id;
    char events[EVENT_QUEUE_SIZE][EVENT_TEXT_SIZE];
    unsigned event_head, event_count, dropped_events;
    char recent[8][EVENT_TEXT_SIZE];
    unsigned recent_next, recent_count;
    fault_msg_t faults[2][NUM_INTERSECTIONS];
    int fault_active[2][NUM_INTERSECTIONS];
    uint64_t fault_received_at[2][NUM_INTERSECTIONS];
    int running, log_stopping, log_failed;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    FILE *log_file;
} central_state_t;

typedef struct {
    central_state_t *state;
    controller_type_t source;
    unsigned index;
} peer_context_t;

static central_state_t state;
static central_ipc_mode_t display_mode = CENTRAL_IPC_LOCAL;
static volatile sig_atomic_t interrupted;
static atomic_int shutdown_requested;
/* Only the operator thread reads/writes this flag. */
static int watching;
static pthread_mutex_t operator_mutex = PTHREAD_MUTEX_INITIALIZER;
static central_schedule_t daily_schedule;
static central_operator_policy_t operator_policy;
static int schedule_enabled;
static int scheduled_mode[NUM_INTERSECTIONS];
static unsigned scheduled_epoch[NUM_INTERSECTIONS];
static uint16_t policy_command[NUM_INTERSECTIONS], policy_ack[NUM_INTERSECTIONS];

/* A UI request renders into its own bounded buffer. Embedded console output
   uses stdout; neither path holds the shared-state lock during terminal I/O. */
static _Thread_local char *output_buffer;
static _Thread_local size_t output_capacity, output_used;
static _Thread_local int output_truncated, command_result;
static _Thread_local int dashboard_rendering;

#define UI_RESET       "\033[0m"
#define UI_BOLD        "\033[1m"
#define UI_DIM         "\033[2m"
#define UI_RED         "\033[1;31m"
#define UI_GREEN       "\033[1;32m"
#define UI_YELLOW      "\033[1;33m"
#define UI_BLUE        "\033[1;34m"
#define UI_MAGENTA     "\033[1;35m"
#define UI_CYAN        "\033[1;36m"
#define UI_WHITE       "\033[1;37m"

static int ui_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int ui_keyword_match(const char *start, const char *cursor, const char *keyword) {
    size_t length = strlen(keyword);
    if (strncmp(cursor, keyword, length) != 0) return 0;
    if (cursor != start && ui_word_char((unsigned char)cursor[-1])) return 0;
    if (ui_word_char((unsigned char)keyword[length - 1]) &&
        ui_word_char((unsigned char)cursor[length])) return 0;
    return 1;
}

static const char *ui_keyword_color(const char *start, const char *cursor, size_t *length) {
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
        {"CENTRAL CONTROL ROOM", UI_CYAN}, {"LIVE TRAFFIC MAP", UI_CYAN}, {"INTERSECTIONS", UI_CYAN},
        {"CONNECTIONS", UI_CYAN}, {"RECENT EVENTS", UI_CYAN}, {"CONTROLS", UI_CYAN}
    };
    size_t i;
    for (i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        if (ui_keyword_match(start, cursor, tokens[i].text)) {
            *length = strlen(tokens[i].text);
            return tokens[i].color;
        }
    }
    *length = 0;
    return NULL;
}

static int ui_lamp_label_triplet(const char *cursor) {
    if (!cursor) return 0;
    return (cursor[0] == 'R' || cursor[0] == 'r') &&
           cursor[1] == ' ' && cursor[2] == ' ' &&
           (cursor[3] == 'Y' || cursor[3] == 'y') &&
           cursor[4] == ' ' && cursor[5] == ' ' &&
           (cursor[6] == 'G' || cursor[6] == 'g');
}

static void ui_console_render_label_triplet(const char *cursor) {
    static const char visible[] = {'R', 'Y', 'G'};
    static const char *colors[] = {UI_RED, UI_YELLOW, UI_GREEN};
    const int active[3] = {
        cursor[0] == 'R', cursor[3] == 'Y', cursor[6] == 'G'
    };
    unsigned i;
    for (i = 0; i < 3; ++i) {
        fputs(active[i] ? colors[i] : UI_DIM "\033[37m", stdout);
        putchar(visible[i]);
        fputs(UI_RESET, stdout);
        if (i != 2) fputs("  ", stdout);
    }
}

static void ui_console_write(const char *text) {
    const char *line = text, *cursor = text;
    unsigned lamp_slot = 0;
    while (*cursor) {
        if (cursor == line && (*cursor == '+' ||
            (cursor[0] == '|' && cursor[1] == '-' && cursor[2] == '-'))) {
            const char *end = strchr(cursor, '\n');
            size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
            fputs(UI_DIM "\033[36m", stdout);
            fwrite(cursor, 1, length, stdout);
            fputs(UI_RESET, stdout);
            cursor += length;
            if (*cursor == '\n') { putchar('\n'); ++cursor; line = cursor; }
            continue;
        }
        if (*cursor == '\n') {
            putchar('\n');
            ++cursor;
            line = cursor;
            lamp_slot = 0;
            continue;
        }

        if (!strncmp(cursor, "{O}", 3) || !strncmp(cursor, "{o}", 3)) {
            static const char *colors[] = {UI_RED, UI_YELLOW, UI_GREEN};
            const int active = cursor[1] == 'O';
            fputs(active ? colors[lamp_slot % 3] : UI_DIM "\033[37m", stdout);
            fputs("{o}", stdout);
            fputs(UI_RESET, stdout);
            cursor += 3;
            ++lamp_slot;
            continue;
        }
        if (!strncmp(cursor, "{?}", 3)) {
            fputs(UI_YELLOW, stdout);
            fwrite(cursor, 1, 3, stdout);
            fputs(UI_RESET, stdout);
            cursor += 3;
            ++lamp_slot;
            continue;
        }
        if (ui_lamp_label_triplet(cursor)) {
            ui_console_render_label_triplet(cursor);
            cursor += 7;
            continue;
        }
        /* Railway map DN lane label: "DN" in green, arrows in the default colour */
        if (!strncmp(cursor, "DN >>>", 6)) {
            fputs(UI_GREEN, stdout);
            fwrite(cursor, 1, 2, stdout);
            fputs(UI_RESET, stdout);
            fwrite(cursor + 2, 1, 4, stdout);
            cursor += 6;
            continue;
        }
        /* Railway map trains: UP <[<<<][<<<] and DN [>>>][>>>]> in orange */
        if (!strncmp(cursor, "<[<<<][<<<]", 11) || !strncmp(cursor, "[>>>][>>>]>", 11)) {
            fputs("\033[1;38;5;208m", stdout);
            fwrite(cursor, 1, 11, stdout);
            fputs(UI_RESET, stdout);
            cursor += 11;
            continue;
        }

        size_t length = 0;
        const char *color = ui_keyword_color(line, cursor, &length);
        if (color && length) {
            fputs(color, stdout);
            fwrite(cursor, 1, length, stdout);
            fputs(UI_RESET, stdout);
            cursor += length;
        } else {
            putchar((unsigned char)*cursor++);
        }
    }
}

static void output(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (output_buffer) {
        if (output_used < output_capacity) {
            int count = vsnprintf(output_buffer + output_used, output_capacity - output_used, format, args);
            if (count > 0) {
                size_t left = output_capacity - output_used;
                if ((size_t)count >= left) output_truncated = 1;
                output_used += (size_t)count < left ? (size_t)count : left - 1;
            }
        }
    } else if (dashboard_rendering && isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL) {
        char text[2048];
        vsnprintf(text, sizeof(text), format, args);
        ui_console_write(text);
    } else {
        vprintf(format, args);
    }
    va_end(args);
}

static const char *fault_name(unsigned value) {
    static const char *names[] = {"NONE", "LIGHT FAULT", "SENSOR FAULT", "COMM FAULT", "GATE FAULT", "CONFLICTING GREENS"};
    return value <= FAULT_NOT_WORKING ? names[value] : "UNKNOWN";
}

static const char *severity_name(unsigned value) {
    return value == SEV_LOW ? "LOW" : value == SEV_MEDIUM ? "MEDIUM" :
           value == SEV_CRITICAL ? "CRITICAL" : "UNKNOWN";
}

static void handle_signal(int signo) {
    (void)signo;
    interrupted = 1;
}

static const char *mode_name(unsigned value) {
    static const char *names[] = {"FIXED", "SENSOR", "RAILWAY", "FAILSAFE"};
    return value <= MODE_FAILSAFE ? names[value] : "UNKNOWN";
}

static const char *phase_name(unsigned value) {
    static const char *names[] = {"NS GREEN", "NS YELLOW", "EW GREEN", "EW YELLOW", "RAIL HOLD"};
    return value <= PHASE_RAILWAY_HOLD ? names[value] : "UNKNOWN";
}

static const char *light_name(unsigned value) {
    static const char *names[] = {"OFF", "RED", "YELLOW", "GREEN"};
    return value <= LIGHT_GREEN ? names[value] : "UNKNOWN";
}

static const char *request_name(request_state_t value) {
    static const char *names[] = {"QUEUED", "SENDING", "ACCEPTED", "REJECTED", "UNCONFIRMED", "CANCELED", "EXPIRED"};
    return value <= REQUEST_EXPIRED ? names[value] : "UNKNOWN";
}

static void describe_command(const test_message_t *message, char *text, size_t size) {
    central_sim_command_t simulation;
    if (central_simulation_decode(message, &simulation)) {
        if (simulation.action == CENTRAL_SIM_TIME)
            snprintf(text, size, "sim-time %02u:%02u", simulation.minute / 60, simulation.minute % 60);
        else snprintf(text, size, "%s", simulation.action == CENTRAL_SIM_START ? "sim-start" : "sim-stop");
    } else if (message->header.type == MSG_TEST) {
        snprintf(text, size, "train-sim %.63s", message->data);
    } else if (message->header.type == MSG_MODE_COMMAND) {
        mode_cmd_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        if (command.action == CMD_REVERT) snprintf(text, size, "mode-revert");
        else if (command.action == CMD_TEMPORARY)
            snprintf(text, size, "mode-temp %s %us", mode_name(command.new_mode), command.duration_sec);
        else snprintf(text, size, "mode-%s", mode_name(command.new_mode));
    } else {
        coordination_command_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        snprintf(text, size, "coordinate %s offset=%us",
                 command.phase == PHASE_NS_GREEN ? "NS" : "EW", command.cycle_offset_sec);
    }
}

static double age_seconds(uint64_t now, uint64_t received) {
    return now >= received ? (double)(now - received) / CENTRAL_NSEC : 0.0;
}

#define UI_INNER_WIDTH 104

static void ui_border(void) {
    char line[UI_INNER_WIDTH + 5];
    size_t i;
    line[0] = '+';
    for (i = 0; i < UI_INNER_WIDTH + 2; ++i) line[i + 1] = '-';
    line[UI_INNER_WIDTH + 3] = '+';
    line[UI_INNER_WIDTH + 4] = '\0';
    output("%s\n", line);
}

static void ui_rowf(const char *format, ...) {
    char content[512];
    va_list args;
    size_t length;
    va_start(args, format);
    vsnprintf(content, sizeof(content), format, args);
    va_end(args);
    length = strlen(content);
    if (length > UI_INNER_WIDTH) content[UI_INNER_WIDTH] = '\0';
    output("| %-*.*s |\n", UI_INNER_WIDTH, UI_INNER_WIDTH, content);
}

static void ui_panel(const char *title) {
    ui_border();
    ui_rowf("%s", title);
    ui_border();
}

static const char *ui_connection(const central_peer_status_t *peer, int online) {
    if (!peer->connected) return "DISCONNECTED";
    return online ? "CONNECTED" : "LOST";
}

static const char *ui_health(const central_monitor_t *monitor, controller_type_t source,
                             unsigned id) {
    int health = central_monitor_health(monitor, source, id);
    return health > 0 ? "HEALTHY" : health == 0 ? "DEGRADED" : "UNKNOWN";
}

static void event_locked(central_state_t *s, const char *format, ...) {
    char text[EVENT_TEXT_SIZE] = {0};
    char timestamp[32];
    size_t used;
    unsigned slot;
    va_list args;
    central_timestamp(timestamp, sizeof(timestamp));
    snprintf(text, sizeof(text), "[%s] ", timestamp);
    used = strlen(text);
    va_start(args, format);
    vsnprintf(text + used, sizeof(text) - used, format, args);
    va_end(args);
    memcpy(s->recent[s->recent_next], text, sizeof(text));
    s->recent_next = (s->recent_next + 1) % 8;
    if (s->recent_count < 8) ++s->recent_count;
    if (s->event_count == EVENT_QUEUE_SIZE) {
        s->event_head = (s->event_head + 1) % EVENT_QUEUE_SIZE;
        --s->event_count;
        ++s->dropped_events;
    }
    slot = (s->event_head + s->event_count++) % EVENT_QUEUE_SIZE;
    memcpy(s->events[slot], text, sizeof(text));
    pthread_cond_broadcast(&s->changed);
}

static void print_help(void) {
    output("Commands:\n"
           "  mode-fixed [I1..I6|all]\n"
           "  mode-sensor [I1..I6|all]\n"
           "  mode-temp <I1..I6|all> <fixed|sensor> <seconds>\n"
           "  mode-revert <I1..I6|all>\n"
           "  sim-start <I1..I6|all> | sim-stop <I1..I6|all>\n"
           "  sim-time <I1..I6|all> <HH:MM>\n"
           "  coordinate <I1..I6|all> <NS|EW> <offset 0..63>\n"
           "  coordinate-at <delay 1..3600s> <I1..I6|all> <NS|EW> <offset 0..63>\n"
           "  train-cmd <train-up|train-down|train P# up/down|noexit P# up/down>\n"
           "  train-cmd <reset P#|scale 1..100|status>\n"
           "  schedule | schedule-resume <I1..I6|all> | version\n"
           "  map | status | commands | faults | events | help | quit\n"
           "  watch  (legacy live detailed status)\n"
           "Queued commands expire after %u seconds. ACCEPTED means receipt only.\n",
           COMMAND_MAX_WAIT_SEC);
    output("Train commands simulate sensor/fault events; Train owns gate safety.\n"
           "Local sim commands require the new Local simulation handler; legacy ACKs do not confirm them.\n"
           "sim-stop stops generated traffic inputs; Local light control continues.\n"
           "Use Local simulated time for traffic demos with Central --schedule disabled.\n"
           "Direct p#-fault is blocked: the current Train remote handler can deadlock.\n"
           "Current Local stores validated coordination offsets and applies them at a safe phase boundary.\n"
           "coordinate-at sets a Central dispatch time; v1 has no shared activation epoch.\n");
}


static void map_lamps(char *buffer, size_t size, unsigned state) {
    if (!buffer || !size) return;
    if (state > LIGHT_GREEN) {
        snprintf(buffer, size, "{?}{?}{?}");
        return;
    }
    /* Uppercase O is only an INTERNAL active marker. central_ui always renders
       every bulb visually as lowercase {o}; only ANSI color/brightness changes. */
    snprintf(buffer, size, "{%c}{%c}{%c}",
             state == LIGHT_RED ? 'O' : 'o',
             state == LIGHT_YELLOW ? 'O' : 'o',
             state == LIGHT_GREEN ? 'O' : 'o');
}

static void map_lamp_labels(char *buffer, size_t size, unsigned state) {
    if (!buffer || !size) return;
    if (state > LIGHT_GREEN) {
        snprintf(buffer, size, "?  ?  ?");
        return;
    }
    /* Uppercase marks the active label; lowercase marks inactive labels.
       central_ui renders all three visibly as R/Y/G and colors only the active one. */
    snprintf(buffer, size, "%c  %c  %c",
             state == LIGHT_RED ? 'R' : 'r',
             state == LIGHT_YELLOW ? 'Y' : 'y',
             state == LIGHT_GREEN ? 'G' : 'g');
}

static const char *map_ped_state(unsigned walk, unsigned request) {
    /* Fixed-width six-character tokens keep every live-map card aligned. */
    if (walk) return "[WALK]";
    if (request) return "[REQ ]";
    return "[STOP]";
}

static const char *map_train_state(unsigned state_value) {
    static const char *names[] = {"NONE", "APPROACHING", "AT CROSSING", "CLEAR"};
    return state_value <= TRAIN_CLEAR ? names[state_value] : "UNKNOWN";
}

static const char *map_gate_state(unsigned state_value) {
    static const char *names[] = {"OPEN", "CLOSING", "CLOSED", "OPENING", "FAULT"};
    return state_value <= GATE_FAULT ? names[state_value] : "UNKNOWN";
}

/* Railway lanes on the live map. Nothing is animated: each lane shows only
   what Train reports for that track. The dotted lanes span row columns
   MAP_TRACK_FIRST..MAP_TRACK_LAST and P1..P3 sit at map_crossing_col. */
enum { MAP_TRACK_FIRST = 8, MAP_TRACK_LAST = 95, MAP_TRAIN_WIDTH = 11 };
static const unsigned map_crossing_col[NUM_CROSSINGS] = {21, 48, 75}; /* the 'P' of P1..P3 */

/* Crossing index each lane's train was last reported AT (UP, DN), or -1.
   Written only while rendering the map. */
static int map_lane_last_at[2] = {-1, -1};

/* Build one lane row. UP trains run right-to-left P3->P2->P1 and DN trains
   left-to-right P1->P2->P3. The rear-most crossing on the route that reports
   the lane's train APPROACHING or AT CROSSING decides where it is drawn:
   just before that crossing, or on it. After the train leaves a crossing it
   waits between that crossing and the next until the next one reports; after
   the last crossing on the route, or while Train is not reporting, the lane
   is empty. */
static void map_train_lane(char *row, size_t size, const central_monitor_t *view, int down) {
    static const char up_sprite[MAP_TRAIN_WIDTH + 1] = "<[<<<][<<<]";
    static const char down_sprite[MAP_TRAIN_WIDTH + 1] = "[>>>][>>>]>";
    const char *sprite = down ? down_sprite : up_sprite;
    int *last_at = &map_lane_last_at[down ? 1 : 0];
    unsigned step;

    if (!row || size < UI_INNER_WIDTH + 1 || !view) return;
    memset(row, ' ', UI_INNER_WIDTH);
    row[UI_INNER_WIDTH] = '\0';
    memset(row + MAP_TRACK_FIRST, '.', MAP_TRACK_LAST - MAP_TRACK_FIRST + 1);
    if (down) memcpy(row, "DN >>>", 6);
    else memcpy(row + UI_INNER_WIDTH - 6, "<<< UP", 6);

    for (step = 0; step < NUM_CROSSINGS; ++step) {
        const central_crossing_status_t *entry = &view->crossings[step];
        if (!entry->valid || !entry->synchronized || !entry->status.track_states) {
            *last_at = -1; /* Train not reporting: forget the waiting train */
            return;
        }
    }

    for (step = 0; step < NUM_CROSSINGS; ++step) {
        unsigned i = down ? step : NUM_CROSSINGS - 1 - step;
        const central_crossing_status_t *entry = &view->crossings[i];
        unsigned lane, col, start;
        lane = down ? entry->status.down_state : entry->status.up_state;
        if (lane != TRAIN_APPROACHING && lane != TRAIN_AT_CROSSING) continue;
        col = map_crossing_col[i];
        if (lane == TRAIN_AT_CROSSING) *last_at = (int)i;
        if (down) /* head '>' is the sprite's last character */
            start = (lane == TRAIN_AT_CROSSING ? col + 1 : col - 3) - (MAP_TRAIN_WIDTH - 1);
        else      /* head '<' is the sprite's first character */
            start = lane == TRAIN_AT_CROSSING ? col : col + 3;
        memcpy(row + start, sprite, MAP_TRAIN_WIDTH);
        return;
    }

    /* No crossing reports the train: wait between the crossing it left and the next */
    if (*last_at >= 0) {
        int next = down ? *last_at + 1 : *last_at - 1;
        if (next < 0 || next >= NUM_CROSSINGS) {
            *last_at = -1; /* Left the last crossing on the route */
            return;
        }
        memcpy(row + (map_crossing_col[*last_at] + map_crossing_col[next]) / 2 - MAP_TRAIN_WIDTH / 2,
               sprite, MAP_TRAIN_WIDTH);
    }
}

static void display_map(void) {
    central_monitor_t view;
    uint64_t now;
    int local_online, train_online;
    char ns[NUM_INTERSECTIONS][16], ew[NUM_INTERSECTIONS][16];
    char ns_label[NUM_INTERSECTIONS][8], ew_label[NUM_INTERSECTIONS][8];
    const char *ped_ns[NUM_INTERSECTIONS], *ped_ew[NUM_INTERSECTIONS];
    char train_up[UI_INNER_WIDTH + 1], train_down[UI_INNER_WIDTH + 1];
    unsigned i;

    pthread_mutex_lock(&state.mutex);
    view = state.monitor;
    pthread_mutex_unlock(&state.mutex);

    now = central_monotonic_ns();
    local_online = central_peer_online(&view, CONTROLLER_LOCAL, now);
    train_online = central_peer_online(&view, CONTROLLER_TRAIN, now);

    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        if (!view.intersections[i].valid) {
            snprintf(ns[i], sizeof(ns[i]), "{?}{?}{?}");
            snprintf(ew[i], sizeof(ew[i]), "{?}{?}{?}");
            snprintf(ns_label[i], sizeof(ns_label[i]), "?  ?  ?");
            snprintf(ew_label[i], sizeof(ew_label[i]), "?  ?  ?");
            ped_ns[i] = "[----]";
            ped_ew[i] = "[----]";
        } else {
            const status_msg_t *status = &view.intersections[i].status;
            map_lamps(ns[i], sizeof(ns[i]), status->ns_state);
            map_lamps(ew[i], sizeof(ew[i]), status->ew_state);
            map_lamp_labels(ns_label[i], sizeof(ns_label[i]), status->ns_state);
            map_lamp_labels(ew_label[i], sizeof(ew_label[i]), status->ew_state);
            ped_ns[i] = map_ped_state((unsigned)status->pedestrian_ns,
                                      (unsigned)status->pedestrian_ns_request);
            ped_ew[i] = map_ped_state((unsigned)status->pedestrian_ew,
                                      (unsigned)status->pedestrian_ew_request);
        }
    }
    map_train_lane(train_up, sizeof(train_up), &view, 0);
    map_train_lane(train_down, sizeof(train_down), &view, 1);

    dashboard_rendering = 1;
    ui_panel("LIVE TRAFFIC MAP  |  SCHEMATIC VIEW - I1..I6 / P1..P3");
    ui_rowf("Lamp shape stays {o}; active bulb + R/Y/G label use color only | railway lanes show Train-reported state");
    ui_rowf("");
    ui_rowf("%24s%27s%27s", "I1", "I3", "I5");
    ui_rowf("            +--------------------+     +--------------------+     +--------------------+");
    ui_rowf("            | N-S %-9s      |     | N-S %-9s      |     | N-S %-9s      |", ns[0], ns[2], ns[4]);
    ui_rowf("            |      %-7s       |     |      %-7s       |     |      %-7s       |", ns_label[0], ns_label[2], ns_label[4]);
    ui_rowf("            |                    |     |                    |     |                    |");
    ui_rowf("            | E-W %-9s      |     | E-W %-9s      |     | E-W %-9s      |", ew[0], ew[2], ew[4]);
    ui_rowf("            |      %-7s       |     |      %-7s       |     |      %-7s       |", ew_label[0], ew_label[2], ew_label[4]);
    ui_rowf("            | PED N-S %-6s     |     | PED N-S %-6s     |     | PED N-S %-6s     |", ped_ns[0], ped_ns[2], ped_ns[4]);
    ui_rowf("            | PED E-W %-6s     |     | PED E-W %-6s     |     | PED E-W %-6s     |", ped_ew[0], ped_ew[2], ped_ew[4]);
    ui_rowf("            +---------+----------+     +---------+----------+     +---------+----------+");
    ui_rowf("                      |                          |                          |");
    ui_rowf("        =============P1=========================P2=========================P3===================");
    ui_rowf("%s", train_up);
    ui_rowf("%s", train_down);
    ui_rowf("        ========================================================================================");
    ui_rowf("                      |                          |                          |");
    ui_rowf("            +---------+----------+     +---------+----------+     +---------+----------+");
    ui_rowf("            | N-S %-9s      |     | N-S %-9s      |     | N-S %-9s      |", ns[1], ns[3], ns[5]);
    ui_rowf("            |      %-7s       |     |      %-7s       |     |      %-7s       |", ns_label[1], ns_label[3], ns_label[5]);
    ui_rowf("            |                    |     |                    |     |                    |");
    ui_rowf("            | E-W %-9s      |     | E-W %-9s      |     | E-W %-9s      |", ew[1], ew[3], ew[5]);
    ui_rowf("            |      %-7s       |     |      %-7s       |     |      %-7s       |", ew_label[1], ew_label[3], ew_label[5]);
    ui_rowf("            | PED N-S %-6s     |     | PED N-S %-6s     |     | PED N-S %-6s     |", ped_ns[1], ped_ns[3], ped_ns[5]);
    ui_rowf("            | PED E-W %-6s     |     | PED E-W %-6s     |     | PED E-W %-6s     |", ped_ew[1], ped_ew[3], ped_ew[5]);
    ui_rowf("            +--------------------+     +--------------------+     +--------------------+");
    ui_rowf("%24s%27s%27s", "I2", "I4", "I6");
    ui_rowf("");

    ui_panel("RAILWAY CROSSINGS  |  LIVE STATE");
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        const central_crossing_status_t *entry = &view.crossings[i];
        if (!entry->valid) {
            ui_rowf("P%u  %-12s | GATE %-8s | HEALTH %-8s | LINK %-4s",
                    i + 1, "WAITING", "--", "UNKNOWN", train_online ? "UP" : "DOWN");
        } else {
            ui_rowf("P%u  %-12s | GATE %-8s | HEALTH %-8s | LINK %-4s",
                    i + 1, map_train_state(entry->status.train_state),
                    map_gate_state(entry->status.gate_state),
                    ui_health(&view, CONTROLLER_TRAIN, i), train_online ? "UP" : "DOWN");
        }
    }

    ui_panel("SYSTEM STATUS  |  DISTRIBUTED HEALTH");
    ui_rowf("LOCAL %-12s | TRAIN %-12s | SYSTEM %-10s",
            ui_connection(&view.peers[0], local_online),
            ui_connection(&view.peers[1], train_online),
            local_online && train_online ? "HEALTHY" : "DEGRADED");

    ui_panel("LIVE CONTROLS  |  TYPE COMMAND THEN ENTER - MAP CONTINUES");
    ui_rowf("[F1-F6] FIXED | [S1-S6] SENSOR | [TU] TRAIN UP | [TD] TRAIN DN | [TB] BOTH | [E] EVENTS | [D] DETAILS");
    ui_rowf("[M] MENU | [H] HELP | [0] QUIT DISPLAY | blank ENTER pauses live refresh");
    ui_border();
    dashboard_rendering = 0;
}

static void display_ui(void) {
    central_monitor_t view;
    char events[8][EVENT_TEXT_SIZE];
    unsigned count, next, dropped, i;
    int log_failed;
    unsigned routes[NUM_INTERSECTIONS];
    int connected[MAX_PEERS];
    uint64_t probes[MAX_PEERS];
    uint64_t now;
    int local_online, train_online;
    pthread_mutex_lock(&state.mutex);
    view = state.monitor;
    memcpy(events, state.recent, sizeof(events));
    count = state.recent_count;
    next = state.recent_next;
    dropped = state.dropped_events;
    log_failed = state.log_failed;
    memcpy(routes, state.local_route, sizeof(routes));
    memcpy(connected, state.endpoint_connected, sizeof(connected));
    memcpy(probes, state.endpoint_probe, sizeof(probes));
    pthread_mutex_unlock(&state.mutex);
    now = central_monotonic_ns();
    local_online = central_peer_online(&view, CONTROLLER_LOCAL, now);
    train_online = central_peer_online(&view, CONTROLLER_TRAIN, now);

    if (!output_buffer && watching && isatty(STDOUT_FILENO)) output("\033[2J\033[H");
    dashboard_rendering = 1;

    ui_panel("CENTRAL CONTROL ROOM  |  LIVE SUPERVISORY DASHBOARD");
    ui_rowf("Node: VM3  | Mode: %-6s | System: ONLINE | View: %-9s | Build: %s",
            display_mode == CENTRAL_IPC_GLOBAL ? "GLOBAL" : "LOCAL",
            watching ? "LIVE VIEW" : "SNAPSHOT", CENTRAL_BUILD_VERSION);
    ui_rowf("Railway: %-8s | Local: %-12s | Train: %-12s | Refresh: 1.0s | Status limit: %.1fs",
            train_online ? "ACTIVE" : "STANDBY",
            ui_connection(&view.peers[0], local_online),
            ui_connection(&view.peers[1], train_online),
            (double)central_monitor_status_max_age_ns(&view) / CENTRAL_NSEC);
    ui_rowf("Central process: OPERATIONAL | Qnet/GNS transport handled independently | Operator UI: READY");

    ui_panel("INTERSECTIONS  |  SIGNALS / SENSORS / RAILWAY PRE-EMPTION");
    ui_rowf("ID | MODE     | PHASE        | NS     | EW     | PED NS/EW | RAIL  | HEALTH   | LINK");
    ui_rowf("---+----------+--------------+--------+--------+-----------+-------+----------+-------------");
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        const central_intersection_status_t *entry = &view.intersections[i];
        const status_msg_t *status = &entry->status;
        const char *freshness;
        const char *health = ui_health(&view, CONTROLLER_LOCAL, i);
        if (!entry->valid) {
            ui_rowf("I%u | %-8s | %-12s | %-6s | %-6s | %-9s | %-5s | %-8s | %-11s",
                    i + 1, "WAITING", "NO REPORT", "--", "--", "--/--", "--", "UNKNOWN", "NO DATA");
            continue;
        }
        freshness = !connected[routes[i]] || now < probes[routes[i]] ||
                    now - probes[routes[i]] >= HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC ? "OFFLINE" :
                    !entry->synchronized ? "WAITING UPDATE" :
                    now < entry->received_at ||
                    now - entry->received_at >= central_monitor_status_max_age_ns(&view) ? "STALE" : "CURRENT";
        ui_rowf("I%u | %-8s | %-12s | %-6s | %-6s | %-4s/%-4s | %-5s | %-8s | %-11s",
                i + 1, mode_name(status->mode), phase_name(status->phase),
                light_name(status->ns_state), light_name(status->ew_state),
                status->pedestrian_ns ? "WALK" : "STOP",
                status->pedestrian_ew ? "WALK" : "STOP",
                status->railway_preempt ? "HOLD" : "CLEAR", health, freshness);
        if (status->telemetry_version) {
            ui_rowf("   Detail | Rem:%3us | Sensor NS/EW:%u/%u | PedReq:%u/%u | Sim:%s %02u:%02u | Age:%4.1fs",
                    status->time_remaining,
                    (unsigned)status->sensor_ns_count, (unsigned)status->sensor_ew_count,
                    (unsigned)status->pedestrian_ns_request, (unsigned)status->pedestrian_ew_request,
                    status->sim_running ? "RUN" : "STOP",
                    (unsigned)status->sim_minute_of_day / 60,
                    (unsigned)status->sim_minute_of_day % 60,
                    age_seconds(now, entry->received_at));
            ui_rowf("          Train P/A/R:%u/%u/%us | Seq:%u | Last cmd:%u | Fault:%s",
                    (unsigned)status->train_pending, (unsigned)status->train_active,
                    (unsigned)status->train_recovery_remaining,
                    (unsigned)status->status_sequence, (unsigned)status->last_command_id,
                    status->fault_active ? fault_name(status->fault_type) : "CLEAR");
        } else {
            ui_rowf("   Detail | Age:%4.1fs | Telemetry:BASIC | Health:%s | Link:%s",
                    age_seconds(now, entry->received_at), health, freshness);
        }
    }

    ui_panel("RAILWAY / CROSSINGS  |  TRAIN CONTROL AND GATE SAFETY");
    ui_rowf("ID | TRAIN STATE  | GATE     | FAULT              | HEALTH   | AGE    | LINK");
    ui_rowf("---+--------------+----------+--------------------+----------+--------+--------");
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        static const char *trains[] = {"NONE", "APPROACHING", "AT CROSSING", "CLEAR"};
        static const char *gates[] = {"OPEN", "CLOSING", "CLOSED", "OPENING", "FAULT"};
        const central_crossing_status_t *entry = &view.crossings[i];
        const char *link = train_online ? "UP" : "DOWN";
        if (!entry->valid) {
            ui_rowf("P%u | %-12s | %-8s | %-18s | %-8s | %-6s | %-6s",
                    i + 1, "WAITING", "--", "UNKNOWN", "UNKNOWN", "--", link);
            continue;
        }
        ui_rowf("P%u | %-12s | %-8s | %-18s | %-8s | %5.1fs | %-6s",
                i + 1, trains[entry->status.train_state], gates[entry->status.gate_state],
                fault_name(entry->status.fault), ui_health(&view, CONTROLLER_TRAIN, i),
                age_seconds(now, entry->received_at), link);
    }

    ui_panel("CONNECTIONS  |  DISTRIBUTED SYSTEM HEALTH");
    ui_rowf("ENDPOINT           | LINK          | CONTACT       | HEALTH");
    ui_rowf("-------------------+---------------+---------------+--------------------");
    ui_rowf("LOCAL CONTROLLERS  | %-13s | %-13s | %-18s",
            ui_connection(&view.peers[0], local_online),
            local_online ? "HEARTBEAT OK" : "LOST",
            ui_health(&view, CONTROLLER_LOCAL, 0));
    ui_rowf("TRAIN CONTROLLER   | %-13s | %-13s | %-18s",
            ui_connection(&view.peers[1], train_online),
            train_online ? "HEARTBEAT OK" : "LOST",
            ui_health(&view, CONTROLLER_TRAIN, 0));

    ui_panel("RECENT EVENTS  |  ROLLING ACTIVITY FEED");
    if (!count) ui_rowf("No events received yet.");
    for (i = 0; i < count; ++i) ui_rowf("%s", events[(next + 8 - count + i) % 8]);
    if (dropped) ui_rowf("Dropped log records: %u", dropped);
    if (log_failed) ui_rowf("EVENT LOG FAILED: new events are not being saved.");

    ui_panel("CONTROLS  |  QUICK KEYS");
    ui_rowf("[L] LIVE MAP   [D] LIVE DETAILS   [S] STATUS   [E] EVENTS   [F] FAULTS   [C] HISTORY");
    ui_rowf("[M] MENU       [H] FULL HELP                             [0] QUIT DISPLAY");
    ui_rowf("Live map refresh 0.5s; details refresh 1.0s; commands remain Central supervisory requests.");
    ui_border();

    dashboard_rendering = 0;
    if (!output_buffer && watching) output("Live view: press Enter to return to the command prompt.\n");
    if (!output_buffer) fflush(stdout);
}

static void *log_thread(void *argument) {
    central_state_t *s = argument;
    for (;;) {
        char text[EVENT_TEXT_SIZE];
        pthread_mutex_lock(&s->mutex);
        while (!s->event_count && !s->log_stopping) pthread_cond_wait(&s->changed, &s->mutex);
        if (!s->event_count && s->log_stopping) { pthread_mutex_unlock(&s->mutex); break; }
        memcpy(text, s->events[s->event_head], sizeof(text));
        s->event_head = (s->event_head + 1) % EVENT_QUEUE_SIZE;
        --s->event_count;
        pthread_mutex_unlock(&s->mutex);
        if (fprintf(s->log_file, "%s\n", text) < 0 || fflush(s->log_file) == EOF) {
            pthread_mutex_lock(&s->mutex);
            s->log_failed = 1;
            pthread_mutex_unlock(&s->mutex);
            fprintf(stderr, "Event log write failed\n");
            break;
        }
    }
    return NULL;
}

static int handle_message(const test_message_t *message, reply_t *reply, void *context) {
    central_state_t *s = context;
    uint64_t now;
    int result = 0;
    pthread_mutex_lock(&s->mutex);
    /* Sample after taking the lock: concurrent probe observations must never
       overtake an earlier timestamp and spuriously invalidate fresh reports. */
    now = central_monotonic_ns();
    switch (message->header.type) {
        case MSG_HEARTBEAT: {
            heartbeat_msg_t heartbeat;
            int before, after;
            memcpy(&heartbeat, message->data, sizeof(heartbeat));
            before = central_monitor_health(&s->monitor, message->header.src, heartbeat.sender_id);
            if (!central_monitor_heartbeat(&s->monitor, message->header.src, &heartbeat, now)) result = -1;
            after = central_monitor_health(&s->monitor, message->header.src, heartbeat.sender_id);
            if (result == 0 && before != after && after >= 0)
                event_locked(s, "%s %u reported health %s", controller_name(message->header.src),
                             heartbeat.sender_id + 1, after ? "HEALTHY" : "DEGRADED");
            break;
        }
        case MSG_TEST:
            event_locked(s, "Test message from %s", controller_name(message->header.src));
            break;
        case MSG_STATUS_UPDATE: {
            status_msg_t status;
            memcpy(&status, message->data, sizeof(status));
            if (!central_monitor_status(&s->monitor, &status, now)) result = -1;
            break;
        }
        case MSG_RAILWAY_STATUS: {
            railway_status_msg_t status;
            memcpy(&status, message->data, sizeof(status));
            if (!central_monitor_railway(&s->monitor, &status, now)) result = -1;
            break;
        }
        case MSG_FAULT_ALERT: {
            fault_msg_t fault;
            char description[sizeof(fault.description) + 1];
            int index = central_peer_index(message->header.src);
            unsigned limit = index == 0 ? NUM_INTERSECTIONS : NUM_CROSSINGS;
            unsigned i;
            memcpy(&fault, message->data, sizeof(fault));
            if (index < 0 || fault.source_id >= limit || fault.fault_type > FAULT_NOT_WORKING ||
                fault.severity < SEV_LOW || fault.severity > SEV_CRITICAL) {
                result = -1;
                break;
            }
            memcpy(description, fault.description, sizeof(fault.description));
            description[sizeof(fault.description)] = '\0';
            for (i = 0; description[i]; ++i) if ((unsigned char)description[i] < 32) description[i] = ' ';
            s->faults[index][fault.source_id] = fault;
            s->fault_active[index][fault.source_id] = fault.fault_type != FAULT_NONE;
            s->fault_received_at[index][fault.source_id] = now;
            event_locked(s, "%s %u fault %u severity %u: %s", controller_name(message->header.src),
                         fault.source_id + 1, fault.fault_type, fault.severity, description);
            break;
        }
        default: result = -1; break;
    }
    pthread_mutex_unlock(&s->mutex);
    reply->status = result == 0 ? 0 : -1;
    return result;
}

static void *receive_thread(void *argument) {
    central_receiver_run(argument);
    return NULL;
}

static command_record_t *find_command_locked(central_state_t *s, uint16_t id) {
    unsigned i;
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i)
        if (id && s->commands[i].id == id) return &s->commands[i];
    return NULL;
}

static uint16_t next_id_locked(central_state_t *s) {
    int reserved;
    do {
        unsigned i;
        if (++s->next_id == 0) ++s->next_id;
        reserved = find_command_locked(s, s->next_id) != NULL;
        for (i = 0; i < NUM_INTERSECTIONS; ++i)
            if (policy_command[i] == s->next_id) reserved = 1;
    } while (reserved);
    return s->next_id;
}

static void finish_command_locked(central_state_t *s, command_record_t *record,
                                  request_state_t result, const char *detail) {
    record->state = result;
    record->finished_at = central_monotonic_ns();
    snprintf(record->detail, sizeof(record->detail), "%s", detail);
    event_locked(s, "Command %u %s%u %s [%s]: %s", record->id,
                 record->peer == 1 ? "Train " : "I", record->peer == 1 ? 1 : record->target + 1,
                 request_name(result), record->description, detail);
}

static void discard_queue_locked(central_state_t *s, unsigned peer) {
    unsigned i;
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        if (s->commands[i].id && s->commands[i].peer == peer && s->commands[i].state == REQUEST_QUEUED)
            finish_command_locked(s, &s->commands[i], REQUEST_CANCELED, "link lost or Central stopping; not sent");
    }
    s->queue_head[peer] = s->queue_count[peer] = 0;
}

static int local_ready_locked(central_state_t *s, unsigned target, uint64_t now) {
    unsigned peer = s->local_route[target];
    return s->endpoint_connected[peer] && now >= s->endpoint_probe[peer] &&
           now - s->endpoint_probe[peer] < HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC &&
           central_can_command(&s->monitor, target, now);
}

static int queue_ready_locked(central_state_t *s, unsigned peer, uint64_t now) {
    unsigned i;
    for (i = 0; i < s->queue_count[peer]; ++i)
        if (s->queue[peer][(s->queue_head[peer] + i) % COMMAND_QUEUE_SIZE].not_before <= now) return 1;
    return 0;
}

static int take_command(central_state_t *s, unsigned peer, queued_command_t *work) {
    command_record_t *record;
    uint16_t id;
    uint64_t now;
    pthread_mutex_lock(&s->mutex);
    unsigned i, count = s->queue_count[peer];
    now = central_monotonic_ns();
    if (!s->running || !queue_ready_locked(s, peer, now)) { pthread_mutex_unlock(&s->mutex); return 0; }
    for (i = 0; i < count; ++i) {
        unsigned slot = (s->queue_head[peer] + i) % COMMAND_QUEUE_SIZE;
        if (s->queue[peer][slot].not_before <= now) {
            unsigned j;
            *work = s->queue[peer][slot];
            for (j = i; j + 1 < count; ++j)
                s->queue[peer][(s->queue_head[peer] + j) % COMMAND_QUEUE_SIZE] =
                    s->queue[peer][(s->queue_head[peer] + j + 1) % COMMAND_QUEUE_SIZE];
            --s->queue_count[peer];
            break;
        }
    }
    id = work->id;
    record = find_command_locked(s, id);
    now = central_monotonic_ns();
    if (record && record->state == REQUEST_QUEUED &&
        now >= work->not_before && now - work->not_before >= COMMAND_MAX_WAIT_SEC * CENTRAL_NSEC) {
        finish_command_locked(s, record, REQUEST_EXPIRED, "queue deadline expired; not sent");
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (!record || record->state != REQUEST_QUEUED ||
        !(peer == 1 ? s->endpoint_connected[peer] && central_peer_online(&s->monitor, CONTROLLER_TRAIN, now) :
          local_ready_locked(s, record->target, now))) {
        if (record && record->state == REQUEST_QUEUED) {
            finish_command_locked(s, record, REQUEST_CANCELED, "fresh status/healthy Local unavailable; not sent");
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    record->state = REQUEST_SENDING;
    record->sent_at = now;
    snprintf(record->detail, sizeof(record->detail), "waiting for %s reply", peer == 1 ? "Train" : "Local");
    central_timestamp(work->message.header.timestamp, sizeof(work->message.header.timestamp));
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

static int send_command(peer_context_t *peer, queued_command_t *work) {
    central_state_t *s = peer->state;
    reply_t reply;
    uint16_t id = work->id;
    int result = central_send(&s->links[peer->index], &work->message, &reply);
    int saved_error = errno;
    char detail[96];
    command_record_t *record;
    pthread_mutex_lock(&s->mutex);
    record = find_command_locked(s, id);
    if (record) {
        if (result == CENTRAL_SEND_OK) snprintf(detail, sizeof(detail), "%s",
            peer->index == 1 ? "Train receipt only; BUSY/application not reported" : "receipt confirmed; application not confirmed");
        else if (result == CENTRAL_SEND_REJECTED) snprintf(detail, sizeof(detail), "%s rejected request; v1 supplies no reason", controller_name(peer->source));
        else if (result == CENTRAL_SEND_PROTOCOL && work->message.header.type == MSG_TEST &&
                 work->message.header.dst == CONTROLLER_LOCAL)
            snprintf(detail, sizeof(detail), "Local simulation reply invalid or ID not confirmed; not retried");
        else snprintf(detail, sizeof(detail), "%s (errno=%d); outcome unknown; not retried",
                      result == CENTRAL_SEND_PROTOCOL ? "invalid reply" :
                      saved_error == EBUSY ? "previous IPC still pending" :
                      saved_error == ETIMEDOUT ? "reply deadline expired" : "transport failed", saved_error);
        finish_command_locked(s, record, result == CENTRAL_SEND_OK ? REQUEST_ACCEPTED :
                              result == CENTRAL_SEND_REJECTED ? REQUEST_REJECTED : REQUEST_UNCONFIRMED, detail);
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}

static void *peer_thread(void *argument) {
    peer_context_t *peer = argument;
    central_state_t *s = peer->state;
    central_link_t *link = &s->links[peer->index];
    uint64_t next_tick = central_monotonic_ns();
    const uint64_t period = HEARTBEAT_PERIOD_SEC * CENTRAL_NSEC;
    unsigned misses = 0;
    int command_ready = 0;
    if (peer->source == CONTROLLER_LOCAL) {
        unsigned target;
        for (target = 0; target < NUM_INTERSECTIONS; ++target)
            if (s->local_route[target] == peer->index) break;
        if (target == NUM_INTERSECTIONS) return NULL;
    }
    for (;;) {
        struct timespec deadline;
        int running;
        pthread_mutex_lock(&s->mutex);
        while (s->running && central_monotonic_ns() < next_tick &&
               !(command_ready && queue_ready_locked(s, peer->index, central_monotonic_ns()))) {
            uint64_t wake = next_tick;
            unsigned i;
            if (command_ready) for (i = 0; i < s->queue_count[peer->index]; ++i) {
                uint64_t due = s->queue[peer->index][(s->queue_head[peer->index] + i) % COMMAND_QUEUE_SIZE].not_before;
                if (due < wake) wake = due;
            }
            deadline.tv_sec = (time_t)(wake / CENTRAL_NSEC);
            deadline.tv_nsec = (long)(wake % CENTRAL_NSEC);
            pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        }
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
        uint64_t now = central_monotonic_ns();
        if (now < next_tick) {
            queued_command_t work;
            if (command_ready && take_command(s, peer->index, &work)) {
                int sent = send_command(peer, &work);
                if (sent != CENTRAL_SEND_OK && sent != CENTRAL_SEND_REJECTED) command_ready = 0;
            }
            continue;
        }
        /* Absolute releases skip missed periods: no cumulative drift or catch-up burst.
           A due probe precedes new commands; an in-flight send still costs up to 500 ms. */
        next_tick += ((now - next_tick) / period + 1) * period;
        command_ready = 0;
        if (!central_link_is_connected(link)) {
            central_link_connect(link);
            if (!central_link_is_connected(link)) continue;
            pthread_mutex_lock(&s->mutex);
            s->endpoint_connected[peer->index] = 1;
            s->monitor.peers[central_peer_index(peer->source)].connected = 1;
            event_locked(s, "%s endpoint %u connected", controller_name(peer->source), peer->index);
            pthread_mutex_unlock(&s->mutex);
            misses = 0;
        }
        if (central_send_heartbeat(link, peer->source) == CENTRAL_SEND_OK) {
            int restored;
            misses = 0;
            pthread_mutex_lock(&s->mutex);
            restored = central_observe_peer(&s->monitor, peer->source, central_monotonic_ns());
            s->endpoint_probe[peer->index] = central_monotonic_ns();
            if (restored) event_locked(s, "%s heartbeat restored", controller_name(peer->source));
            pthread_mutex_unlock(&s->mutex);
            command_ready = 1;
        } else if (++misses >= HEARTBEAT_MISS_LIMIT) {
            central_link_close(link);
            pthread_mutex_lock(&s->mutex);
            unsigned target;
            int any_local = 0;
            s->endpoint_connected[peer->index] = 0;
            s->endpoint_probe[peer->index] = 0;
            ++s->endpoint_epoch[peer->index];
            if (peer->source == CONTROLLER_TRAIN) central_peer_disconnected(&s->monitor, peer->source);
            else {
                for (target = 0; target < NUM_INTERSECTIONS; ++target) {
                    if (s->local_route[target] == peer->index) s->monitor.intersections[target].synchronized = 0;
                    if (s->endpoint_connected[s->local_route[target]]) any_local = 1;
                }
                if (!any_local) central_peer_disconnected(&s->monitor, CONTROLLER_LOCAL);
            }
            discard_queue_locked(s, peer->index);
            event_locked(s, "%s offline after %u missed heartbeats", controller_name(peer->source), misses);
            pthread_mutex_unlock(&s->mutex);
            continue;
        } else continue;
        /* The next iteration dispatches queued work immediately, or sleeps until
           a queue notification/absolute heartbeat release. */
    }
    return NULL;
}

static void print_commands(void) {
    command_record_t commands[COMMAND_HISTORY_SIZE];
    unsigned i, next;
    pthread_mutex_lock(&state.mutex);
    memcpy(commands, state.commands, sizeof(commands));
    next = state.command_next;
    pthread_mutex_unlock(&state.mutex);
    uint64_t now = central_monotonic_ns();
    output("Command  Target  State        Request\n");
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        const command_record_t *record = &commands[(next + i) % COMMAND_HISTORY_SIZE];
        if (record->id) {
            uint64_t stop = record->sent_at ? record->sent_at : record->finished_at ? record->finished_at : now;
            output("%-8u %s%-6u %-12s %s\n", record->id,
                   record->peer == 1 ? "Train " : "I", record->peer == 1 ? 1 : record->target + 1,
                   request_name(record->state), record->description);
            output("    %s; queued %.3fs", record->detail, age_seconds(stop, record->queued_at));
            if (record->sent_at) output("; IPC %.3fs", age_seconds(record->finished_at ? record->finished_at : now, record->sent_at));
            output("; age %.1fs\n", age_seconds(now, record->queued_at));
        }
    }
    output("ACCEPTED confirms receipt; status shows reported operation. Train v1 does not report command BUSY/results.\n");
}

static void print_faults(void) {
    fault_msg_t faults[2][NUM_INTERSECTIONS];
    int active[2][NUM_INTERSECTIONS];
    uint64_t received[2][NUM_INTERSECTIONS];
    unsigned i, j;
    pthread_mutex_lock(&state.mutex);
    memcpy(faults, state.faults, sizeof(faults));
    memcpy(active, state.fault_active, sizeof(active));
    memcpy(received, state.fault_received_at, sizeof(received));
    pthread_mutex_unlock(&state.mutex);
    output("Fault alerts awaiting a clear notification\n");
    for (i = 0; i < 2; ++i) for (j = 0; j < (i ? NUM_CROSSINGS : NUM_INTERSECTIONS); ++j)
        if (active[i][j]) {
            char description[sizeof(faults[i][j].description) + 1];
            size_t k;
            memcpy(description, faults[i][j].description, sizeof(faults[i][j].description));
            description[sizeof(description) - 1] = '\0';
            for (k = 0; description[k]; ++k) if ((unsigned char)description[k] < 32) description[k] = ' ';
            output("%c%u type %u severity %u (%s, %s) age %.1fs: %s\n", i ? 'P' : 'I', j + 1,
                   faults[i][j].fault_type, faults[i][j].severity,
                   fault_name(faults[i][j].fault_type), severity_name(faults[i][j].severity),
                   age_seconds(central_monotonic_ns(), received[i][j]), description);
        }
}

static void print_events(void) {
    char recent[8][EVENT_TEXT_SIZE];
    unsigned next, count, i, dropped;
    int failed;
    pthread_mutex_lock(&state.mutex);
    memcpy(recent, state.recent, sizeof(recent));
    next = state.recent_next; count = state.recent_count;
    failed = state.log_failed; dropped = state.dropped_events;
    pthread_mutex_unlock(&state.mutex);
    for (i = 0; i < count; ++i) output("%s\n", recent[(next + 8 - count + i) % 8]);
    output("Log records dropped: %u%s\n", dropped, failed ? "; EVENT LOG FAILED" : "");
}

static int enqueue_command(const test_message_t *prototype, unsigned target, unsigned delay, int automatic) {
    int train = prototype->header.dst == CONTROLLER_TRAIN;
    unsigned first = train ? 0 : target == INTERSECTION_ALL ? 0 : target;
    unsigned last = train ? 1 : target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    unsigned needed[MAX_PEERS] = {0};
    unsigned available = 0, i;
    uint64_t now;
    pthread_mutex_lock(&state.mutex);
    if (!state.running) { pthread_mutex_unlock(&state.mutex); output("Central is stopping; request not queued\n"); return -1; }
    now = central_monotonic_ns();
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i)
        if (!state.commands[i].id || (state.commands[i].state != REQUEST_QUEUED &&
                                     state.commands[i].state != REQUEST_SENDING)) ++available;
    for (i = first; i < last; ++i) ++needed[train ? 1 : state.local_route[i]];
    int full = available < last - first;
    for (i = 0; i < state.peer_count; ++i)
        if (state.queue_count[i] + needed[i] > COMMAND_QUEUE_SIZE) full = 1;
    if (full) {
        event_locked(&state, "Command queue is full");
        pthread_mutex_unlock(&state.mutex);
        output("Command queue is full; request not queued\n");
        return -1;
    }
    if (train && (!state.endpoint_connected[1] || !central_peer_online(&state.monitor, CONTROLLER_TRAIN, now))) {
        pthread_mutex_unlock(&state.mutex);
        output("Train requires a healthy communication link; request not queued\n");
        return -1;
    }
    for (i = first; !train && i < last; ++i) if (!local_ready_locked(&state, i, now)) {
        event_locked(&state, "I%u requires a fresh Local status and healthy link before commands", i + 1);
        pthread_mutex_unlock(&state.mutex);
        output("I%u requires a fresh Local status and healthy link before commands; request not queued\n", i + 1);
        return -1;
    }
    for (i = first; i < last; ++i) {
        test_message_t message = *prototype;
        unsigned slot = state.command_next;
        unsigned peer = train ? 1 : state.local_route[i];
        uint16_t id = next_id_locked(&state);
        while (state.commands[slot].id && (state.commands[slot].state == REQUEST_QUEUED ||
                                          state.commands[slot].state == REQUEST_SENDING))
            slot = (slot + 1) % COMMAND_HISTORY_SIZE;
        state.commands[slot].id = id;
        state.commands[slot].target = i;
        state.commands[slot].peer = peer;
        state.commands[slot].automatic = automatic;
        memset(&state.commands[slot].mode_request, 0, sizeof(mode_cmd_msg_t));
        if (message.header.type == MSG_MODE_COMMAND) {
            memcpy(&state.commands[slot].mode_request, message.data, sizeof(mode_cmd_msg_t));
            if (!automatic) policy_command[i] = id;
        }
        state.commands[slot].state = REQUEST_QUEUED;
        describe_command(&message, state.commands[slot].description, sizeof(state.commands[slot].description));
        snprintf(state.commands[slot].detail, sizeof(state.commands[slot].detail), "dispatch in %us; queue allowance %us", delay, COMMAND_MAX_WAIT_SEC);
        state.commands[slot].queued_at = now;
        state.commands[slot].sent_at = state.commands[slot].finished_at = 0;
        state.command_next = (slot + 1) % COMMAND_HISTORY_SIZE;
        if (!train) {
            central_command_set_id(&message, id);
            central_command_set_target(&message, i);
        }
        queued_command_t *work = &state.queue[peer][(state.queue_head[peer] + state.queue_count[peer]++) % COMMAND_QUEUE_SIZE];
        work->message = message;
        work->id = id;
        work->not_before = now + delay * CENTRAL_NSEC;
        event_locked(&state, "Command %u queued for %s%u [%s]", id,
                     train ? "Train " : "I", train ? 1 : i + 1, state.commands[slot].description);
    }
    pthread_mutex_unlock(&state.mutex);
    output("Queued %u request(s); use commands for outcomes. ACCEPTED confirms receipt only.\n", last - first);
    return 0;
}

static void cancel_automatic(unsigned target) {
    unsigned peer, i;
    pthread_mutex_lock(&state.mutex);
    for (peer = 0; peer < state.peer_count; ++peer) {
        unsigned kept = 0, count = state.queue_count[peer];
        for (i = 0; i < count; ++i) {
            queued_command_t work = state.queue[peer][(state.queue_head[peer] + i) % COMMAND_QUEUE_SIZE];
            command_record_t *record = find_command_locked(&state, work.id);
            if (record && record->automatic && (target == INTERSECTION_ALL || record->target == target))
                finish_command_locked(&state, record, REQUEST_CANCELED, "superseded by operator; not sent");
            else state.queue[peer][(state.queue_head[peer] + kept++) % COMMAND_QUEUE_SIZE] = work;
        }
        state.queue_count[peer] = kept;
    }
    pthread_mutex_unlock(&state.mutex);
}

/* Called under operator_mutex. Fixed-size policy calculations do no file I/O.
   Queue insertion is once per desired scheduled mode, never an uncertain-send retry. */
static void schedule_tick(void) {
    time_t wall = time(NULL);
    struct tm local;
    unsigned target, minute;
    uint64_t now = central_monotonic_ns();
    if (!schedule_enabled || !localtime_r(&wall, &local)) return;
    minute = (unsigned)(local.tm_hour * 60 + local.tm_min);
    for (target = 0; target < NUM_INTERSECTIONS; ++target) {
        uint8_t desired;
        int ready, held = policy_command[target] && policy_ack[target] != policy_command[target];
        unsigned epoch;
        command_record_t record = {0};
        pthread_mutex_lock(&state.mutex);
        command_record_t *pending = find_command_locked(&state, policy_command[target]);
        if (pending) record = *pending;
        ready = local_ready_locked(&state, target, now);
        epoch = state.endpoint_epoch[state.local_route[target]];
        pthread_mutex_unlock(&state.mutex);
        if (record.id) {
            held = record.state != REQUEST_ACCEPTED;
            if (!held && policy_ack[target] != record.id) {
                const mode_cmd_msg_t *command = &record.mode_request;
                central_policy_note_operator(&operator_policy, target, command->action,
                    command->new_mode, command->duration_sec, record.finished_at);
                policy_ack[target] = record.id;
            }
        }
        if (held || central_policy_operator_active(&operator_policy, target, now)) continue;
        if (!central_schedule_mode(&daily_schedule, target, minute, &desired)) continue;
        if (scheduled_mode[target] == desired) continue;
        if (!ready) continue;
        test_message_t message;
        mode_cmd_msg_t command = {0};
        central_message_init(&message, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        command.intersection_id = (uint8_t)target;
        command.new_mode = desired;
        command.action = CMD_SET_MODE;
        command.priority = CMD_PRIO_SCHEDULE;
        memcpy(message.data, &command, sizeof(command));
        /* Keep periodic policy output off the interactive prompt. */
        char sink[256];
        char *saved_buffer = output_buffer;
        size_t saved_capacity = output_capacity, saved_used = output_used;
        output_buffer = sink; output_capacity = sizeof(sink); output_used = 0;
        if (enqueue_command(&message, target, 0, 1) == 0) {
            scheduled_mode[target] = desired;
            scheduled_epoch[target] = epoch;
        }
        output_buffer = saved_buffer; output_capacity = saved_capacity; output_used = saved_used;
    }
}

static void print_schedule(void) {
    unsigned i;
    output("Daily schedule %s; target QNX local clock/timezone.\n", schedule_enabled ? "ENABLED" : "DISABLED");
    for (i = 0; i < daily_schedule.count; ++i) {
        const central_schedule_entry_t *entry = &daily_schedule.entries[i];
        output("%02u:%02u %-6s %s", entry->minute_of_day / 60, entry->minute_of_day % 60,
               mode_name(entry->mode), entry->target == INTERSECTION_ALL ? "all" : "I");
        if (entry->target != INTERSECTION_ALL) output("%u", entry->target + 1);
        output("\n");
    }
    for (i = 0; i < NUM_INTERSECTIONS; ++i)
        output("I%u operator hold %s; last scheduled intent %s\n", i + 1,
               central_policy_operator_active(&operator_policy, i, central_monotonic_ns()) ? "ACTIVE" : "NONE",
               scheduled_mode[i] < 0 ? "NONE" : mode_name((unsigned)scheduled_mode[i]));
    output("Operator intent is recorded separately from reported operation.\n"
           "Rejected/uncertain operator requests hold automation until schedule-resume.\n"
           "Temporary suppression starts at receipt; Local must enforce its own expiry.\n"
           "Schedule requests are not replayed automatically after reconnect.\n");
}

static void *schedule_thread(void *context) {
    central_state_t *s = context;
    const uint64_t period = CENTRAL_NSEC / 5;
    uint64_t next = central_monotonic_ns();
    for (;;) {
        struct timespec deadline;
        uint64_t now;
        int running;
        pthread_mutex_lock(&s->mutex);
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
        pthread_mutex_lock(&operator_mutex);
        schedule_tick();
        pthread_mutex_unlock(&operator_mutex);
        now = central_monotonic_ns();
        next += ((now - next) / period + 1) * period;
        deadline.tv_sec = (time_t)(next / CENTRAL_NSEC);
        deadline.tv_nsec = (long)(next % CENTRAL_NSEC);
        pthread_mutex_lock(&s->mutex);
        while (s->running && central_monotonic_ns() < next)
            pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
    }
    return NULL;
}

static char *trim_command(char *line) {
    size_t length;
    while (*line == ' ' || *line == '\t') ++line;
    length = strlen(line);
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
        line[--length] = '\0';
    return line;
}

static void execute_command(char *line) {
    test_message_t message;
    unsigned target;
    command_result = 0;
    line = trim_command(line);
    if (!*line) return;
    if (!strcmp(line, "map")) display_map();
    else if (!strcmp(line, "status")) display_ui();
    else if (!strcmp(line, "commands")) print_commands();
    else if (!strcmp(line, "faults")) print_faults();
    else if (!strcmp(line, "events")) print_events();
    else if (!strcmp(line, "watch")) { watching = 1; display_ui(); }
    else if (!strcmp(line, "help")) print_help();
    else if (!strcmp(line, "version")) output("%s (%s)\n", CENTRAL_BUILD_VERSION, CENTRAL_BUILD_STAMP);
    else if (!strcmp(line, "schedule")) print_schedule();
    else if (!strcmp(line, "quit") || !strcmp(line, "shutdown")) {
        atomic_store(&shutdown_requested, 1);
        output("Central shutdown requested\n");
    }
    else if (!strncmp(line, "schedule-resume ", 16)) {
        char parsed[96];
        snprintf(parsed, sizeof(parsed), "mode-revert %s", line + 16);
        if (!schedule_enabled || !central_parse_command(parsed, &message, &target)) {
            command_result = -1;
            output("Requires --schedule file and target I1..I6 or all\n");
        } else {
            unsigned first = target == INTERSECTION_ALL ? 0 : target;
            unsigned last = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1, i;
            central_policy_resume_schedule(&operator_policy, target);
            for (i = first; i < last; ++i) { policy_command[i] = policy_ack[i] = 0; scheduled_mode[i] = -1; }
            cancel_automatic(target);
            output("Central schedule resumed; requests wait for fresh Local status. Local owns safe transitions.\n");
        }
    }
    else if (!strncmp(line, "train-cmd ", 10)) {
        char payload[CENTRAL_TRAIN_PAYLOAD_SIZE];
        if (!central_parse_train_command(line + 10, payload)) { command_result = -1; output("Invalid Train simulation command. Type help.\n"); }
        else if (payload[0] == 'p' && strstr(payload, "-fault")) {
            command_result = -1;
            output("Not sent: current Train remote p#-fault handler can deadlock on its mutex.\n"
                   "Use the Train console for direct fault injection, or noexit P# up for a train timeout.\n");
        } else {
            central_message_init(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_TRAIN);
            memcpy(message.data, payload, sizeof(payload));
            command_result = enqueue_command(&message, 0, 0, 0);
        }
    }
    else if (!strncmp(line, "coordinate-at ", 14)) {
        char *end;
        unsigned long delay;
        char parsed[256];
        errno = 0; delay = strtoul(line + 14, &end, 10);
        if (errno || line[14] < '0' || line[14] > '9' || *end != ' ' || delay < 1 || delay > 3600) {
            command_result = -1;
            output("Invalid dispatch delay; use coordinate-at 1..3600 I# NS|EW 0..63\n");
        } else {
            snprintf(parsed, sizeof(parsed), "coordinate %s", end + 1);
            if (!central_parse_command(parsed, &message, &target)) { command_result = -1; output("Invalid coordination arguments\n"); }
            else {
                command_result = enqueue_command(&message, target, (unsigned)delay, 0);
                output("Central dispatch only; simultaneous Local activation is not supported by v1.\n");
            }
        }
    }
    else if (central_parse_command(line, &message, &target)) {
        if (message.header.type != MSG_TEST) cancel_automatic(target);
        command_result = enqueue_command(&message, target, 0, 0);
        if (command_result == 0 && message.header.type == MSG_TEST && schedule_enabled)
            output("Central schedule still uses wall time; use a run without --schedule for Local time-of-day demos.\n");
        if (command_result == 0 && message.header.type == MSG_MODE_COMMAND) {
            mode_cmd_msg_t command;
            memcpy(&command, message.data, sizeof(command));
            central_policy_note_operator(&operator_policy, target, command.action, command.new_mode,
                                         command.duration_sec, central_monotonic_ns());
            unsigned first = target == INTERSECTION_ALL ? 0 : target;
            unsigned last = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1, i;
            for (i = first; i < last; ++i) scheduled_mode[i] = -1;
        }
    }
    else { command_result = -1; output("Invalid command or arguments. Type help for usage.\n"); }
}

static int ui_command(const char *request, char *response, size_t capacity, void *context) {
    char line[CENTRAL_UI_REQUEST_SIZE], *command;
    int result;
    (void)context;
    pthread_mutex_lock(&operator_mutex);
    output_buffer = response; output_capacity = capacity; output_used = 0;
    output_truncated = command_result = 0;
    if (capacity) response[0] = '\0';
    snprintf(line, sizeof(line), "%s", request);
    command = trim_command(line);
    if (!strcmp(command, "watch")) display_ui();
    else if (!strcmp(command, "quit")) output("Close the display with quit; use shutdown to stop Central.\n");
    else execute_command(command);
    result = command_result;
    if (output_truncated) {
        const char marker[] = "\nOUTPUT TRUNCATED; inspect the event log.\n";
        size_t length = sizeof(marker);
        if (capacity >= length) memcpy(response + capacity - length, marker, length);
        result = -1;
    }
    output_buffer = NULL; output_capacity = output_used = 0;
    pthread_mutex_unlock(&operator_mutex);
    return result;
}

int main(int argc, char *argv[]) {
    central_ipc_mode_t mode = CENTRAL_IPC_LOCAL;
    const char *log_path = "/tmp/central_controller.log";
    central_receiver_t receiver;
    central_ui_server_t ui_server = {0};
    const char *schedule_path = NULL;
    const char *services[MAX_PEERS] = {CENTRAL_LOCAL_SERVICE, CENTRAL_TRAIN_SERVICE};
    char default_local[NUM_INTERSECTIONS][64];
    unsigned route[NUM_INTERSECTIONS] = {0}, configured[NUM_INTERSECTIONS] = {0}, peer_count = 2;
    pthread_condattr_t attributes;
    sigset_t shutdown_signals;
    struct sigaction action;
    pthread_t peers[MAX_PEERS], logger, incoming, scheduler;
    peer_context_t contexts[MAX_PEERS];
    unsigned initialized_links = 0, started_peers = 0;
    int receiver_ready = 0, receiver_started = 0, logger_started = 0;
    int ui_ready = 0, headless = 0;
    int scheduler_started = 0;
    int result = EXIT_FAILURE, overflow = 0, i;
    unsigned status_age_seconds = CENTRAL_STATUS_STALE_SEC;
    char line[256];
    size_t used = 0;
    uint64_t last_display = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            output("Usage: %s [-l|-g] [-o log-file] [-s status-age-seconds]\n"
                   "       [--headless] [--schedule file] [--local-endpoint I#=service]...\n"
                   "Status age: 1..60 seconds, default %u; a supervisory freshness policy.\n",
                   argv[0], CENTRAL_STATUS_STALE_SEC);
            print_help();
            return EXIT_SUCCESS;
        }
        if (!strcmp(argv[i], "-l")) mode = CENTRAL_IPC_LOCAL;
        else if (!strcmp(argv[i], "-g")) mode = CENTRAL_IPC_GLOBAL;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--schedule") && i + 1 < argc) schedule_path = argv[++i];
        else if (!strcmp(argv[i], "--local-endpoint") && i + 1 < argc) {
            const char *mapping = argv[++i];
            unsigned target, peer;
            size_t k;
            if (strlen(mapping) < 4 || mapping[0] != 'I' || mapping[1] < '1' || mapping[1] > '6' || mapping[2] != '=') {
                fprintf(stderr, "Local endpoint must be I1..I6=service\n"); return EXIT_FAILURE;
            }
            target = (unsigned)(mapping[1] - '1');
            const char *service = mapping + 3;
            if (configured[target] || !*service || strlen(service) > 100 || !strcmp(service, CENTRAL_TRAIN_SERVICE)) {
                fprintf(stderr, "Duplicate or invalid Local endpoint\n"); return EXIT_FAILURE;
            }
            for (k = 0; service[k]; ++k) {
                unsigned char c = (unsigned char)service[k];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
                    fprintf(stderr, "Endpoint names allow letters, digits, _, -, . only\n"); return EXIT_FAILURE;
                }
            }
            for (peer = 0; peer < peer_count; ++peer) if (!strcmp(services[peer], service)) break;
            if (peer == peer_count) services[peer_count++] = service;
            route[target] = peer; configured[target] = 1;
        }
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            errno = 0;
            unsigned long seconds = strtoul(value, &end, 10);
            if (errno || value == end || *end || *value < '0' || *value > '9' || seconds < 1 || seconds > 60) {
                fprintf(stderr, "Status age must be an integer from 1 to 60 seconds\n");
                return EXIT_FAILURE;
            }
            status_age_seconds = (unsigned)seconds;
        }
        else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); return EXIT_FAILURE; }
    }
    display_mode = mode;
    /* The Local launcher publishes one comm endpoint per intersection. */
    for (i = 1; i < NUM_INTERSECTIONS; ++i) {
        unsigned peer;
        if (configured[i]) continue;
        snprintf(default_local[i], sizeof(default_local[i]), "%s%d", CENTRAL_LOCAL_SERVICE_PREFIX, i + 1);
        for (peer = 0; peer < peer_count; ++peer) if (!strcmp(services[peer], default_local[i])) break;
        if (peer == peer_count) services[peer_count++] = default_local[i];
        route[i] = peer;
    }
    if (schedule_path) {
        char error[256];
        if (!central_schedule_load(schedule_path, &daily_schedule, error, sizeof(error))) {
            fprintf(stderr, "Schedule error: %s\n", error); return EXIT_FAILURE;
        }
        schedule_enabled = 1;
    }
    for (i = 0; i < NUM_INTERSECTIONS; ++i) scheduled_mode[i] = -1;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0)
        return EXIT_FAILURE;

    memset(&state, 0, sizeof(state));
    state.peer_count = peer_count;
    memcpy(state.local_route, route, sizeof(route));
    central_monitor_init(&state.monitor, status_age_seconds);
    if (pthread_mutex_init(&state.mutex, NULL) != 0) return EXIT_FAILURE;
    if (pthread_condattr_init(&attributes) != 0) { pthread_mutex_destroy(&state.mutex); return EXIT_FAILURE; }
    if (pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&state.changed, &attributes) != 0) {
        pthread_condattr_destroy(&attributes);
        pthread_mutex_destroy(&state.mutex);
        return EXIT_FAILURE;
    }
    pthread_condattr_destroy(&attributes);
    state.running = 1;
    for (i = 0; i < (int)peer_count; ++i) {
        int init_result;
        if (mode == CENTRAL_IPC_GLOBAL) {
            // Global mode: connect to remote VMs via /net/{vm}/dev/name/local/
            const char *remote_vm = (i == 1) ? VM2_TRAIN_NAME : VM1_LOCAL_NAME;
            init_result = central_link_init_remote(&state.links[i], services[i], remote_vm);
        } else {
            // Local mode: connect via /dev/name/local/
            init_result = central_link_init(&state.links[i], services[i], mode);
        }
        if (init_result != 0) goto cleanup;
        ++initialized_links;
    }
    state.log_file = fopen(log_path, "a");
    if (!state.log_file) { perror(log_path); goto cleanup; }
    if (central_receiver_init(&receiver, CENTRAL_SERVICE, mode, CONTROLLER_CENTRAL,
                              handle_message, &state) != 0) goto cleanup;
    receiver_ready = 1;
    if (pthread_create(&logger, NULL, log_thread, &state) != 0) goto cleanup;
    logger_started = 1;
    if (pthread_create(&incoming, NULL, receive_thread, &receiver) != 0) goto cleanup;
    receiver_started = 1;
    for (i = 0; i < (int)peer_count; ++i) {
        contexts[i].state = &state;
        contexts[i].index = (unsigned)i;
        contexts[i].source = i == 1 ? CONTROLLER_TRAIN : CONTROLLER_LOCAL;
        if (pthread_create(&peers[i], NULL, peer_thread, &contexts[i]) != 0) goto cleanup;
        ++started_peers;
    }
    if (central_ui_server_start(&ui_server, CENTRAL_UI_SERVICE, ui_command, &state) != 0) {
        perror("Central display service"); goto cleanup;
    }
    ui_ready = 1;
    if (schedule_enabled) {
        if (pthread_create(&scheduler, NULL, schedule_thread, &state) != 0) goto cleanup;
        scheduler_started = 1;
    }
    // Workers inherit the blocked mask; only the operator thread handles shutdown.
    if (pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL) != 0) goto cleanup;
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central started");
    for (i = 0; i < NUM_INTERSECTIONS; ++i)
        event_locked(&state, "I%u routes to %s", (unsigned)i + 1, services[route[i]]);
    pthread_mutex_unlock(&state.mutex);
    result = EXIT_SUCCESS;
    if (!headless) {
        display_ui();
        output("Type help for commands; watch for live status.\n> ");
    } else output("Central core %s; display service %s; attach with central_ui.\n", CENTRAL_BUILD_VERSION, CENTRAL_UI_SERVICE);
    fflush(stdout);
    while (!interrupted && !atomic_load(&shutdown_requested)) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        uint64_t now = central_monotonic_ns();
        int ready;
        if (watching && now - last_display >= CENTRAL_NSEC) { display_ui(); last_display = now; }
        ready = poll(headless ? NULL : &input, headless ? 0 : 1, 200);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); result = EXIT_FAILURE; break; }
        if (ready && (input.revents & (POLLIN | POLLHUP))) {
            char buffer[128];
            ssize_t amount = read(STDIN_FILENO, buffer, sizeof(buffer));
            ssize_t position;
            if (amount == 0) break;
            if (amount < 0) { if (errno == EINTR) continue; perror("read"); result = EXIT_FAILURE; break; }
            for (position = 0; position < amount && !interrupted && !atomic_load(&shutdown_requested); ++position) {
                if (buffer[position] == '\n') {
                    line[used] = '\0';
                    if (watching) { watching = 0; output("Live view stopped. Type status to inspect a snapshot.\n"); }
                    else if (overflow) output("Command too long\n");
                    else {
                        char rendered[CENTRAL_UI_RESPONSE_SIZE];
                        pthread_mutex_lock(&operator_mutex);
                        output_buffer = rendered; output_capacity = sizeof(rendered); output_used = 0;
                        output_truncated = 0; rendered[0] = '\0';
                        execute_command(line);
                        output_buffer = NULL; output_capacity = output_used = 0;
                        pthread_mutex_unlock(&operator_mutex);
                        fputs(rendered, stdout);
                        if (output_truncated) fputs("\nOUTPUT TRUNCATED\n", stdout);
                    }
                    used = 0;
                    overflow = 0;
                    if (watching) last_display = central_monotonic_ns();
                    else if (!interrupted) output("> ");
                    fflush(stdout);
                } else if (used + 1 < sizeof(line)) line[used++] = buffer[position];
                else overflow = 1;
            }
        } else if (ready && (input.revents & (POLLERR | POLLNVAL))) { result = EXIT_FAILURE; break; }
    }

cleanup:
    if (ui_ready) central_ui_server_destroy(&ui_server);
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central stopping");
    state.running = 0;
    for (i = 0; i < (int)peer_count; ++i) discard_queue_locked(&state, (unsigned)i);
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
    if (scheduler_started) pthread_join(scheduler, NULL);
    if (receiver_ready) central_receiver_stop(&receiver);
    for (i = 0; i < (int)started_peers; ++i) pthread_join(peers[i], NULL);
    if (receiver_started) pthread_join(incoming, NULL);
    pthread_mutex_lock(&state.mutex);
    state.log_stopping = 1;
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
    if (logger_started) pthread_join(logger, NULL);
    if (receiver_ready) central_receiver_destroy(&receiver);
    for (i = 0; i < (int)initialized_links; ++i) {
        if (central_link_destroy(&state.links[i]) != 0)
            fprintf(stderr, "Waiting for an outstanding legacy IPC request to be released by its peer (%s)\n",
                    controller_name(i == 1 ? CONTROLLER_TRAIN : CONTROLLER_LOCAL));
    }
    if (state.log_file) fclose(state.log_file);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.mutex);
    return result;
}
