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
    } else vprintf(format, args);
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
    if (message->header.type == MSG_TEST) {
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
           "  coordinate <I1..I6|all> <NS|EW> <offset 0..43>\n"
           "  coordinate-at <delay 1..3600s> <I1..I6|all> <NS|EW> <offset 0..43>\n"
           "  train-cmd <train-up|train-down|train P# up/down|noexit P# up/down>\n"
           "  train-cmd <stuck P#|reset P#|test [1..7]|scale 1..100|status>\n"
           "  schedule | schedule-resume <I1..I6|all> | version\n"
           "  status | commands | faults | events | help | quit\n"
           "  watch  (live status; press Enter to return to the prompt)\n"
           "Queued commands expire after %u seconds. ACCEPTED means receipt only.\n",
           COMMAND_MAX_WAIT_SEC);
    output("Train commands simulate sensor/fault events; Train owns gate safety.\n"
           "Direct p#-fault is blocked: the current Train remote handler can deadlock.\n"
           "coordinate-at sets a Central dispatch time; v1 has no shared activation epoch.\n");
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
    if (!output_buffer && watching && isatty(STDOUT_FILENO)) output("\033[2J\033[H");
    output("========================= CENTRAL CONTROLLER =========================\n");
    output("Build %s (%s)\n", CENTRAL_BUILD_VERSION, CENTRAL_BUILD_STAMP);
    for (i = 0; i < 2; ++i) {
        controller_type_t source = i ? CONTROLLER_TRAIN : CONTROLLER_LOCAL;
        output("%-6s link %-12s contact %s\n", controller_name(source),
               view.peers[i].connected ? "CONNECTED" : "DISCONNECTED",
               central_peer_online(&view, source, now) ? "ONLINE" : "OFFLINE");
    }
    output("\nID  Mode      Phase       NS      EW      Ped N/E Rail Remain Age    State\n");
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        const central_intersection_status_t *entry = &view.intersections[i];
        const status_msg_t *status = &entry->status;
        const char *freshness;
        if (!entry->valid) { output("I%u  Waiting for status\n", i + 1); continue; }
        freshness = !connected[routes[i]] || now < probes[routes[i]] ||
                    now - probes[routes[i]] >= HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC ? "OFFLINE" :
                    !entry->synchronized ? "WAITING UPDATE" :
                    now < entry->received_at || now - entry->received_at >= central_monitor_status_max_age_ns(&view) ? "STALE" : "CURRENT";
        output("I%u  %-9s %-11s %-7s %-7s %u/%u     %u    %3us  %5.1fs %s\n",
               i + 1, mode_name(status->mode), phase_name(status->phase),
               light_name(status->ns_state), light_name(status->ew_state),
               status->pedestrian_ns, status->pedestrian_ew, status->railway_preempt,
               status->time_remaining, age_seconds(now, entry->received_at), freshness);
        if (central_monitor_health(&view, CONTROLLER_LOCAL, i) == 0)
            output("    Reported health DEGRADED; commands blocked until explicit recovery\n");
    }
    output("\nRailway crossings\n");
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        static const char *trains[] = {"NONE", "APPROACHING", "AT CROSSING", "CLEAR"};
        static const char *gates[] = {"OPEN", "CLOSING", "CLOSED", "OPENING", "FAULT"};
        const central_crossing_status_t *entry = &view.crossings[i];
        if (!entry->valid) { output("P%u  Waiting for status\n", i + 1); continue; }
        output("P%u  Train %-11s Gate %-7s Fault %s Age %.1fs %s\n", i + 1,
               trains[entry->status.train_state], gates[entry->status.gate_state],
               fault_name(entry->status.fault), age_seconds(now, entry->received_at),
               !central_peer_online(&view, CONTROLLER_TRAIN, now) ? "OFFLINE" :
               !entry->synchronized ? "WAITING UPDATE" :
                now < entry->received_at || now - entry->received_at >= central_monitor_status_max_age_ns(&view) ? "STALE" : "CURRENT");
        if (central_monitor_health(&view, CONTROLLER_TRAIN, i) == 0)
            output("    Reported health DEGRADED\n");
    }
    output("Train approach signal, flashing lights and individual tracks: NOT REPORTED by v1.\n");
    output("\nRecent events\n");
    for (i = 0; i < count; ++i) output("%s\n", events[(next + 8 - count + i) % 8]);
    if (dropped) output("Log records dropped: %u\n", dropped);
    if (log_failed) output("EVENT LOG FAILED: new events are not being saved\n");
    output("\nStatus age limit %.1fs. Remaining times are reported snapshots.\n",
           (double)central_monitor_status_max_age_ns(&view) / CENTRAL_NSEC);
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
    if (!strcmp(line, "status")) display_ui();
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
                   "Use the Train console for direct fault injection, or stuck P# then train P# up.\n");
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
            output("Invalid dispatch delay; use coordinate-at 1..3600 I# NS|EW 0..43\n");
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
        cancel_automatic(target);
        command_result = enqueue_command(&message, target, 0, 0);
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
    central_ipc_mode_t mode = CENTRAL_IPC_GLOBAL;
    const char *log_path = "/tmp/central_controller.log";
    central_receiver_t receiver;
    central_ui_server_t ui_server = {0};
    const char *schedule_path = NULL;
    const char *services[MAX_PEERS] = {CENTRAL_LOCAL_SERVICE, CENTRAL_TRAIN_SERVICE};
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
        if (central_link_init(&state.links[i], services[i], mode) != 0) goto cleanup;
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
