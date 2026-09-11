#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <stdarg.h>
#include <errno.h>

#include "ipc.h"
#include "commands.h"
#include "monitor.h"

#define COMMAND_QUEUE_SIZE 32
#define COMMAND_HISTORY_SIZE 64
#define EVENT_QUEUE_SIZE 64
#define EVENT_TEXT_SIZE 192
#define COMMAND_MAX_WAIT_SEC 5
#define COMMAND_TEXT_SIZE 80

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
    request_state_t state;
    char description[COMMAND_TEXT_SIZE];
    char detail[96];
    uint64_t queued_at, sent_at, finished_at;
} command_record_t;

typedef struct {
    central_monitor_t monitor;
    central_link_t links[2];
    test_message_t queue[COMMAND_QUEUE_SIZE];
    unsigned queue_head, queue_count;
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
/* Only the operator thread reads/writes this flag. */
static int watching;

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
    if (message->header.type == MSG_MODE_COMMAND) {
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
    printf("Commands:\n"
           "  mode-fixed [I1..I6|all]\n"
           "  mode-sensor [I1..I6|all]\n"
           "  mode-temp <I1..I6|all> <fixed|sensor> <seconds>\n"
           "  mode-revert <I1..I6|all>\n"
           "  coordinate <I1..I6|all> <NS|EW> <offset 0..43>\n"
           "  status | commands | faults | events | help | quit\n"
           "  watch  (live status; press Enter to return to the prompt)\n"
           "Queued commands expire after %u seconds. ACCEPTED means receipt only.\n",
           COMMAND_MAX_WAIT_SEC);
}

static void display_ui(void) {
    central_monitor_t view;
    char events[8][EVENT_TEXT_SIZE];
    unsigned count, next, dropped, i;
    int log_failed;
    uint64_t now;
    pthread_mutex_lock(&state.mutex);
    view = state.monitor;
    memcpy(events, state.recent, sizeof(events));
    count = state.recent_count;
    next = state.recent_next;
    dropped = state.dropped_events;
    log_failed = state.log_failed;
    pthread_mutex_unlock(&state.mutex);
    now = central_monotonic_ns();
    if (watching && isatty(STDOUT_FILENO)) printf("\033[2J\033[H");
    printf("========================= CENTRAL CONTROLLER =========================\n");
    for (i = 0; i < 2; ++i) {
        controller_type_t source = i ? CONTROLLER_TRAIN : CONTROLLER_LOCAL;
        printf("%-6s link %-12s contact %s\n", controller_name(source),
               view.peers[i].connected ? "CONNECTED" : "DISCONNECTED",
               central_peer_online(&view, source, now) ? "ONLINE" : "OFFLINE");
    }
    printf("\nID  Mode      Phase       NS      EW      Ped N/E Rail Remain Age    State\n");
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        const central_intersection_status_t *entry = &view.intersections[i];
        const status_msg_t *status = &entry->status;
        const char *freshness;
        if (!entry->valid) { printf("I%u  Waiting for status\n", i + 1); continue; }
        freshness = !central_peer_online(&view, CONTROLLER_LOCAL, now) ? "OFFLINE" :
                    !entry->synchronized ? "WAITING UPDATE" :
                    now < entry->received_at || now - entry->received_at >= central_monitor_status_max_age_ns(&view) ? "STALE" : "CURRENT";
        printf("I%u  %-9s %-11s %-7s %-7s %u/%u     %u    %3us  %5.1fs %s\n",
               i + 1, mode_name(status->mode), phase_name(status->phase),
               light_name(status->ns_state), light_name(status->ew_state),
               status->pedestrian_ns, status->pedestrian_ew, status->railway_preempt,
               status->time_remaining, age_seconds(now, entry->received_at), freshness);
        if (central_monitor_health(&view, CONTROLLER_LOCAL, i) == 0)
            printf("    Reported health DEGRADED; commands blocked until explicit recovery\n");
    }
    printf("\nRailway crossings\n");
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        static const char *trains[] = {"NONE", "APPROACHING", "AT CROSSING", "CLEAR"};
        static const char *gates[] = {"OPEN", "CLOSING", "CLOSED", "OPENING", "FAULT"};
        const central_crossing_status_t *entry = &view.crossings[i];
        if (!entry->valid) { printf("P%u  Waiting for status\n", i + 1); continue; }
        printf("P%u  Train %-11s Gate %-7s Fault %u Age %.1fs %s\n", i + 1,
               trains[entry->status.train_state], gates[entry->status.gate_state],
               entry->status.fault, age_seconds(now, entry->received_at),
               !central_peer_online(&view, CONTROLLER_TRAIN, now) ? "OFFLINE" :
               !entry->synchronized ? "WAITING UPDATE" :
                now < entry->received_at || now - entry->received_at >= central_monitor_status_max_age_ns(&view) ? "STALE" : "CURRENT");
        if (central_monitor_health(&view, CONTROLLER_TRAIN, i) == 0)
            printf("    Reported health DEGRADED\n");
    }
    printf("\nRecent events\n");
    for (i = 0; i < count; ++i) printf("%s\n", events[(next + 8 - count + i) % 8]);
    if (dropped) printf("Log records dropped: %u\n", dropped);
    if (log_failed) printf("EVENT LOG FAILED: new events are not being saved\n");
    printf("\nStatus age limit %.1fs. Remaining times are reported snapshots.\n",
           (double)central_monitor_status_max_age_ns(&view) / CENTRAL_NSEC);
    if (watching) printf("Live view: press Enter to return to the command prompt.\n");
    fflush(stdout);
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
    do { if (++s->next_id == 0) ++s->next_id; } while (find_command_locked(s, s->next_id));
    return s->next_id;
}

static void finish_command_locked(central_state_t *s, command_record_t *record,
                                  request_state_t result, const char *detail) {
    record->state = result;
    record->finished_at = central_monotonic_ns();
    snprintf(record->detail, sizeof(record->detail), "%s", detail);
    event_locked(s, "Command %u I%u %s [%s]: %s", record->id, record->target + 1,
                 request_name(result), record->description, detail);
}

static void discard_queue_locked(central_state_t *s) {
    unsigned i;
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        if (s->commands[i].id && s->commands[i].state == REQUEST_QUEUED)
            finish_command_locked(s, &s->commands[i], REQUEST_CANCELED, "link lost or Central stopping; not sent");
    }
    s->queue_head = s->queue_count = 0;
}

static int take_command(central_state_t *s, test_message_t *message) {
    command_record_t *record;
    uint16_t id;
    uint64_t now;
    pthread_mutex_lock(&s->mutex);
    if (!s->running || !s->queue_count) { pthread_mutex_unlock(&s->mutex); return 0; }
    *message = s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1) % COMMAND_QUEUE_SIZE;
    --s->queue_count;
    id = central_command_id(message);
    record = find_command_locked(s, id);
    now = central_monotonic_ns();
    if (record && record->state == REQUEST_QUEUED &&
        now >= record->queued_at && now - record->queued_at >= COMMAND_MAX_WAIT_SEC * CENTRAL_NSEC) {
        finish_command_locked(s, record, REQUEST_EXPIRED, "queue deadline expired; not sent");
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (!record || record->state != REQUEST_QUEUED ||
        !central_can_command(&s->monitor, record->target, now)) {
        if (record && record->state == REQUEST_QUEUED) {
            finish_command_locked(s, record, REQUEST_CANCELED, "fresh status/healthy Local unavailable; not sent");
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    record->state = REQUEST_SENDING;
    record->sent_at = now;
    snprintf(record->detail, sizeof(record->detail), "waiting for Local reply");
    central_timestamp(message->header.timestamp, sizeof(message->header.timestamp));
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

static int send_command(peer_context_t *peer, test_message_t *message) {
    central_state_t *s = peer->state;
    reply_t reply;
    uint16_t id = central_command_id(message);
    int result = central_send(&s->links[peer->index], message, &reply);
    int saved_error = errno;
    char detail[96];
    command_record_t *record;
    pthread_mutex_lock(&s->mutex);
    record = find_command_locked(s, id);
    if (record) {
        if (result == CENTRAL_SEND_OK) snprintf(detail, sizeof(detail), "receipt confirmed; application not confirmed");
        else if (result == CENTRAL_SEND_REJECTED) snprintf(detail, sizeof(detail), "Local rejected request; v1 supplies no reason");
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
    for (;;) {
        struct timespec deadline;
        int running;
        pthread_mutex_lock(&s->mutex);
        while (s->running && central_monotonic_ns() < next_tick &&
               !(peer->source == CONTROLLER_LOCAL && command_ready && s->queue_count)) {
            deadline.tv_sec = (time_t)(next_tick / CENTRAL_NSEC);
            deadline.tv_nsec = (long)(next_tick % CENTRAL_NSEC);
            pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        }
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
        uint64_t now = central_monotonic_ns();
        if (now < next_tick) {
            test_message_t message;
            if (peer->source == CONTROLLER_LOCAL && command_ready && take_command(s, &message)) {
                int sent = send_command(peer, &message);
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
            s->monitor.peers[peer->index].connected = 1;
            event_locked(s, "%s connected", controller_name(peer->source));
            pthread_mutex_unlock(&s->mutex);
            misses = 0;
        }
        if (central_send_heartbeat(link, peer->source) == CENTRAL_SEND_OK) {
            int restored;
            misses = 0;
            pthread_mutex_lock(&s->mutex);
            restored = central_observe_peer(&s->monitor, peer->source, central_monotonic_ns());
            if (restored) event_locked(s, "%s heartbeat restored", controller_name(peer->source));
            pthread_mutex_unlock(&s->mutex);
            command_ready = 1;
        } else if (++misses >= HEARTBEAT_MISS_LIMIT) {
            central_link_close(link);
            pthread_mutex_lock(&s->mutex);
            central_peer_disconnected(&s->monitor, peer->source);
            if (peer->source == CONTROLLER_LOCAL) discard_queue_locked(s);
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
    printf("Command  Target  State        Request\n");
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        const command_record_t *record = &commands[(next + i) % COMMAND_HISTORY_SIZE];
        if (record->id) {
            uint64_t stop = record->sent_at ? record->sent_at : record->finished_at ? record->finished_at : now;
            printf("%-8u I%-6u %-12s %s\n", record->id, record->target + 1,
                   request_name(record->state), record->description);
            printf("    %s; queued %.3fs", record->detail, age_seconds(stop, record->queued_at));
            if (record->sent_at) printf("; IPC %.3fs", age_seconds(record->finished_at ? record->finished_at : now, record->sent_at));
            printf("; age %.1fs\n", age_seconds(now, record->queued_at));
        }
    }
    printf("ACCEPTED confirms receipt; the Local status shows the reported operating mode.\n");
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
    printf("Fault alerts awaiting a clear notification\n");
    for (i = 0; i < 2; ++i) for (j = 0; j < (i ? NUM_CROSSINGS : NUM_INTERSECTIONS); ++j)
        if (active[i][j]) {
            char description[sizeof(faults[i][j].description) + 1];
            size_t k;
            memcpy(description, faults[i][j].description, sizeof(faults[i][j].description));
            description[sizeof(description) - 1] = '\0';
            for (k = 0; description[k]; ++k) if ((unsigned char)description[k] < 32) description[k] = ' ';
            printf("%c%u type %u severity %u age %.1fs: %s\n", i ? 'P' : 'I', j + 1,
                   faults[i][j].fault_type, faults[i][j].severity,
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
    for (i = 0; i < count; ++i) puts(recent[(next + 8 - count + i) % 8]);
    printf("Log records dropped: %u%s\n", dropped, failed ? "; EVENT LOG FAILED" : "");
}

static int enqueue_command(const test_message_t *prototype, unsigned target) {
    unsigned first = target == INTERSECTION_ALL ? 0 : target;
    unsigned last = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    unsigned available = 0, i;
    uint64_t now;
    pthread_mutex_lock(&state.mutex);
    now = central_monotonic_ns();
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i)
        if (!state.commands[i].id || (state.commands[i].state != REQUEST_QUEUED &&
                                     state.commands[i].state != REQUEST_SENDING)) ++available;
    if (state.queue_count + last - first > COMMAND_QUEUE_SIZE || available < last - first) {
        event_locked(&state, "Command queue is full");
        pthread_mutex_unlock(&state.mutex);
        printf("Command queue is full; request not queued\n");
        return -1;
    }
    for (i = first; i < last; ++i) if (!central_can_command(&state.monitor, i, now)) {
        event_locked(&state, "I%u requires a fresh Local status and healthy link before commands", i + 1);
        pthread_mutex_unlock(&state.mutex);
        printf("I%u requires a fresh Local status and healthy link before commands; request not queued\n", i + 1);
        return -1;
    }
    for (i = first; i < last; ++i) {
        test_message_t message = *prototype;
        unsigned slot = state.command_next;
        uint16_t id = next_id_locked(&state);
        while (state.commands[slot].id && (state.commands[slot].state == REQUEST_QUEUED ||
                                          state.commands[slot].state == REQUEST_SENDING))
            slot = (slot + 1) % COMMAND_HISTORY_SIZE;
        state.commands[slot].id = id;
        state.commands[slot].target = i;
        state.commands[slot].state = REQUEST_QUEUED;
        describe_command(&message, state.commands[slot].description, sizeof(state.commands[slot].description));
        snprintf(state.commands[slot].detail, sizeof(state.commands[slot].detail), "waiting to send; expires in %us", COMMAND_MAX_WAIT_SEC);
        state.commands[slot].queued_at = now;
        state.commands[slot].sent_at = state.commands[slot].finished_at = 0;
        state.command_next = (slot + 1) % COMMAND_HISTORY_SIZE;
        central_command_set_id(&message, id);
        central_command_set_target(&message, i);
        state.queue[(state.queue_head + state.queue_count++) % COMMAND_QUEUE_SIZE] = message;
        event_locked(&state, "Command %u queued for I%u [%s]", id, i + 1, state.commands[slot].description);
    }
    pthread_mutex_unlock(&state.mutex);
    printf("Queued %u request(s); use commands for outcomes. ACCEPTED confirms receipt only.\n", last - first);
    return 0;
}

static void execute_command(char *line) {
    test_message_t message;
    unsigned target;
    size_t length;
    while (*line == ' ' || *line == '\t') ++line;
    length = strlen(line);
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
        line[--length] = '\0';
    if (!*line) return;
    if (!strcmp(line, "status")) display_ui();
    else if (!strcmp(line, "commands")) print_commands();
    else if (!strcmp(line, "faults")) print_faults();
    else if (!strcmp(line, "events")) print_events();
    else if (!strcmp(line, "watch")) { watching = 1; display_ui(); }
    else if (!strcmp(line, "help")) print_help();
    else if (!strcmp(line, "quit")) interrupted = 1;
    else if (central_parse_command(line, &message, &target)) enqueue_command(&message, target);
    else printf("Invalid command or arguments. Type help for usage.\n");
}

int main(int argc, char *argv[]) {
    central_ipc_mode_t mode = CENTRAL_IPC_GLOBAL;
    const char *log_path = "/tmp/central_controller.log";
    central_receiver_t receiver;
    pthread_condattr_t attributes;
    sigset_t shutdown_signals;
    struct sigaction action;
    pthread_t peers[2], logger, incoming;
    peer_context_t contexts[2];
    unsigned initialized_links = 0, started_peers = 0;
    int receiver_ready = 0, receiver_started = 0, logger_started = 0;
    int result = EXIT_FAILURE, overflow = 0, i;
    unsigned status_age_seconds = CENTRAL_STATUS_STALE_SEC;
    char line[256];
    size_t used = 0;
    uint64_t last_display = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [-l|-g] [-o log-file] [-s status-age-seconds]\n"
                   "Status age: 1..60 seconds, default %u; a supervisory freshness policy.\n",
                   argv[0], CENTRAL_STATUS_STALE_SEC);
            print_help();
            return EXIT_SUCCESS;
        }
        if (!strcmp(argv[i], "-l")) mode = CENTRAL_IPC_LOCAL;
        else if (!strcmp(argv[i], "-g")) mode = CENTRAL_IPC_GLOBAL;
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
    if (central_link_init(&state.links[0], CENTRAL_LOCAL_SERVICE, mode) != 0) goto cleanup;
    ++initialized_links;
    if (central_link_init(&state.links[1], CENTRAL_TRAIN_SERVICE, mode) != 0) goto cleanup;
    ++initialized_links;
    state.log_file = fopen(log_path, "a");
    if (!state.log_file) { perror(log_path); goto cleanup; }
    if (central_receiver_init(&receiver, CENTRAL_SERVICE, mode, CONTROLLER_CENTRAL,
                              handle_message, &state) != 0) goto cleanup;
    receiver_ready = 1;
    if (pthread_create(&logger, NULL, log_thread, &state) != 0) goto cleanup;
    logger_started = 1;
    if (pthread_create(&incoming, NULL, receive_thread, &receiver) != 0) goto cleanup;
    receiver_started = 1;
    for (i = 0; i < 2; ++i) {
        contexts[i].state = &state;
        contexts[i].index = (unsigned)i;
        contexts[i].source = i ? CONTROLLER_TRAIN : CONTROLLER_LOCAL;
        if (pthread_create(&peers[i], NULL, peer_thread, &contexts[i]) != 0) goto cleanup;
        ++started_peers;
    }
    // Workers inherit the blocked mask; only the operator thread handles shutdown.
    if (pthread_sigmask(SIG_UNBLOCK, &shutdown_signals, NULL) != 0) goto cleanup;
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central started");
    pthread_mutex_unlock(&state.mutex);
    result = EXIT_SUCCESS;
    display_ui();
    printf("Type help for commands; watch for live status.\n> ");
    fflush(stdout);
    while (!interrupted) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        uint64_t now = central_monotonic_ns();
        int ready;
        if (watching && now - last_display >= CENTRAL_NSEC) { display_ui(); last_display = now; }
        ready = poll(&input, 1, 200);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); result = EXIT_FAILURE; break; }
        if (ready && (input.revents & (POLLIN | POLLHUP))) {
            char buffer[128];
            ssize_t amount = read(STDIN_FILENO, buffer, sizeof(buffer));
            ssize_t position;
            if (amount == 0) break;
            if (amount < 0) { if (errno == EINTR) continue; perror("read"); result = EXIT_FAILURE; break; }
            for (position = 0; position < amount && !interrupted; ++position) {
                if (buffer[position] == '\n') {
                    line[used] = '\0';
                    if (watching) { watching = 0; printf("Live view stopped. Type status to inspect a snapshot.\n"); }
                    else if (overflow) printf("Command too long\n");
                    else execute_command(line);
                    used = 0;
                    overflow = 0;
                    if (watching) last_display = central_monotonic_ns();
                    else if (!interrupted) printf("> ");
                    fflush(stdout);
                } else if (used + 1 < sizeof(line)) line[used++] = buffer[position];
                else overflow = 1;
            }
        } else if (ready && (input.revents & (POLLERR | POLLNVAL))) { result = EXIT_FAILURE; break; }
    }

cleanup:
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central stopping");
    state.running = 0;
    discard_queue_locked(&state);
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
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
                    controller_name(i ? CONTROLLER_TRAIN : CONTROLLER_LOCAL));
    }
    if (state.log_file) fclose(state.log_file);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.mutex);
    return result;
}
