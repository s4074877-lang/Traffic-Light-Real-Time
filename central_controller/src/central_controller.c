#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <stdarg.h>

#include "../../common/common.h"
#include "../../common/communication/connection.h"
#include "../../common/communication/send.h"
#include "../../common/communication/receive.h"
#include "monitor.h"
#include "commands.h"

#define EVENT_TEXT_SIZE 192

typedef struct {
    monitor_t monitor;
    connection_t connections[2];
    any_msg_t queue[CENTRAL_COMMAND_QUEUE];
    unsigned queue_head, queue_count;
    uint16_t next_id;
    uint32_t sequence;
    char events[CENTRAL_EVENT_HISTORY][EVENT_TEXT_SIZE];
    unsigned event_head, event_count, dropped_events;
    char recent[8][EVENT_TEXT_SIZE];
    unsigned recent_next, recent_count;
    fault_msg_t faults[2][NUM_INTERSECTIONS];
    int active_faults[2][NUM_INTERSECTIONS];
    int running;
    int log_stopping;
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

static void handle_signal(int signo) {
    (void)signo;
    interrupted = 1;
}

static void event_locked(central_state_t *s, const char *format, ...) {
    char text[EVENT_TEXT_SIZE] = {0};
    char timestamp[32];
    size_t used;
    unsigned slot;
    va_list args;
    get_timestamp(timestamp, sizeof(timestamp));
    snprintf(text, sizeof(text), "[%s] ", timestamp);
    used = strlen(text);
    va_start(args, format);
    vsnprintf(text + used, sizeof(text) - used, format, args);
    va_end(args);
    memcpy(s->recent[s->recent_next], text, sizeof(text));
    s->recent_next = (s->recent_next + 1) % 8;
    if (s->recent_count < 8) ++s->recent_count;
    if (s->event_count == CENTRAL_EVENT_HISTORY) {
        s->event_head = (s->event_head + 1) % CENTRAL_EVENT_HISTORY;
        --s->event_count;
        ++s->dropped_events;
    }
    slot = (s->event_head + s->event_count++) % CENTRAL_EVENT_HISTORY;
    memcpy(s->events[slot], text, sizeof(text));
    pthread_cond_broadcast(&s->changed);
}

static const char *mode_name(unsigned mode) {
    static const char *names[] = {"FIXED", "SENSOR", "RAILWAY", "FAILSAFE"};
    return mode <= MODE_FAILSAFE ? names[mode] : "UNKNOWN";
}

static const char *phase_name(unsigned phase) {
    static const char *names[] = {"NS GREEN", "NS YELLOW", "EW GREEN", "EW YELLOW", "RAIL HOLD"};
    return phase <= PHASE_RAILWAY_HOLD ? names[phase] : "UNKNOWN";
}

static const char *light_name(unsigned light) {
    static const char *names[] = {"OFF", "RED", "YELLOW", "GREEN"};
    return light <= LIGHT_GREEN ? names[light] : "UNKNOWN";
}

static const char *request_name(request_state_t status) {
    static const char *names[] = {"QUEUED", "SENDING", "ACCEPTED", "APPLIED",
                                  "REJECTED", "UNCONFIRMED", "EXPIRED"};
    return status <= REQUEST_EXPIRED ? names[status] : "UNKNOWN";
}

static double age_seconds(uint64_t now, uint64_t received) {
    return now >= received ? (double)(now - received) / NANOSECONDS_PER_SEC : 0.0;
}

static void print_help(void) {
    printf("Commands:\n"
           "  mode-fixed [I1..I6|all]\n"
           "  mode-sensor [I1..I6|all]\n"
           "  mode-temp <I1..I6|all> <fixed|sensor> <seconds>\n"
           "  mode-revert <I1..I6|all>\n"
           "  coordinate <I1..I6|all> <NS|EW> <offset 0..43>\n"
           "  override <I1..I6|all> <NS|EW> <seconds>\n"
           "  release <I1..I6|all>\n"
           "  status | commands | faults | help | quit\n");
}

static void display_ui(void) {
    monitor_t view;
    char events[8][EVENT_TEXT_SIZE];
    unsigned count, next, dropped, i;
    uint64_t now = monotonic_ns();
    pthread_mutex_lock(&state.mutex);
    view = state.monitor;
    memcpy(events, state.recent, sizeof(events));
    count = state.recent_count;
    next = state.recent_next;
    dropped = state.dropped_events;
    pthread_mutex_unlock(&state.mutex);

    if (isatty(STDOUT_FILENO)) printf("\033[2J\033[H");
    printf("========================= CENTRAL CONTROLLER =========================\n");
    for (i = 0; i < 2; ++i) {
        controller_type_t source = i == 0 ? CONTROLLER_LOCAL : CONTROLLER_TRAIN;
        const peer_status_t *peer = &view.peers[i];
        int online = monitor_peer_online(&view, source, now);
        unsigned misses = peer->seen ? (unsigned)age_seconds(now, peer->last_heartbeat) : 3;
        if (misses > HEARTBEAT_MISS_LIMIT) misses = HEARTBEAT_MISS_LIMIT;
        printf("%-6s link %-12s health %-8s heartbeat misses %u/%u\n",
               controller_name(source), peer->connected ? "CONNECTED" : "DISCONNECTED",
               !online ? "OFFLINE" : peer->healthy ? "OK" : "DEGRADED",
               misses, HEARTBEAT_MISS_LIMIT);
    }
    printf("\nID  Mode      Phase       NS      EW      Ped N/E Rail Remain Age    State\n");
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        const intersection_status_t *entry = &view.intersections[i];
        const status_msg_t *status = &entry->status;
        const char *freshness;
        if (!entry->valid) {
            printf("I%u  Waiting for status\n", i + 1);
            continue;
        }
        freshness = !monitor_peer_online(&view, CONTROLLER_LOCAL, now) ||
                    (entry->heartbeat_seen && age_seconds(now, entry->last_heartbeat) >= HEARTBEAT_MISS_LIMIT)
                    ? "OFFLINE" : entry->heartbeat_seen && !entry->healthy ? "DEGRADED" :
                    age_seconds(now, entry->received_at) >= STATUS_STALE_SEC ? "STALE" :
                    !entry->synchronized ? "SYNCING" : "CURRENT";
        printf("I%u  %-9s %-11s %-7s %-7s %u/%u     %u    %3us  %5.1fs %s\n",
               i + 1, mode_name(status->mode), phase_name(status->phase),
               light_name(status->ns_state), light_name(status->ew_state),
               status->pedestrian_ns, status->pedestrian_ew, status->railway_preempt,
               status->time_remaining, age_seconds(now, entry->received_at), freshness);
    }
    printf("\nRailway crossings\n");
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        static const char *trains[] = {"NONE", "APPROACHING", "AT CROSSING", "CLEAR"};
        static const char *gates[] = {"OPEN", "CLOSING", "CLOSED", "OPENING", "FAULT"};
        const crossing_status_t *entry = &view.crossings[i];
        if (!entry->valid) { printf("P%u  Waiting for status\n", i + 1); continue; }
        printf("P%u  Train %-11s Gate %-7s Fault %u Age %.1fs %s\n", i + 1,
               trains[entry->status.train_state], gates[entry->status.gate_state],
               entry->status.fault, age_seconds(now, entry->received_at),
               !monitor_peer_online(&view, CONTROLLER_TRAIN, now) ? "OFFLINE" :
               age_seconds(now, entry->received_at) >= STATUS_STALE_SEC ? "STALE" :
               !entry->synchronized ? "SYNCING" : "CURRENT");
    }
    printf("\nRecent events\n");
    for (i = 0; i < count; ++i) printf("%s\n", events[(next + 8 - count + i) % 8]);
    if (dropped) printf("Log records dropped: %u\n", dropped);
    printf("\nType help for commands. Remaining times are reported snapshots.\n> ");
    fflush(stdout);
}

static void *log_thread(void *arg) {
    central_state_t *s = arg;
    for (;;) {
        char text[EVENT_TEXT_SIZE];
        pthread_mutex_lock(&s->mutex);
        while (!s->event_count && !s->log_stopping) pthread_cond_wait(&s->changed, &s->mutex);
        if (!s->event_count && s->log_stopping) { pthread_mutex_unlock(&s->mutex); break; }
        memcpy(text, s->events[s->event_head], sizeof(text));
        s->event_head = (s->event_head + 1) % CENTRAL_EVENT_HISTORY;
        --s->event_count;
        pthread_mutex_unlock(&s->mutex);
        if (fprintf(s->log_file, "%s\n", text) < 0 || fflush(s->log_file) == EOF) {
            fprintf(stderr, "Event log write failed\n");
            break;
        }
    }
    return NULL;
}

static int handle_heartbeat(int rcvid, any_msg_t *msg, reply_t *reply, void *context) {
    central_state_t *s = context;
    (void)rcvid;
    pthread_mutex_lock(&s->mutex);
    if (msg->header.src == CONTROLLER_LOCAL &&
        msg->payload.heartbeat.sender_id != CONTROLLER_NODE_ID) {
        monitor_device_heartbeat(&s->monitor, msg->payload.heartbeat.sender_id,
                                  msg->payload.heartbeat.healthy, monotonic_ns());
    } else monitor_heartbeat(&s->monitor, msg->header.src, msg->payload.heartbeat.healthy, monotonic_ns());
    pthread_mutex_unlock(&s->mutex);
    reply->status = REPLY_ACCEPTED;
    return 0;
}

static void status_locked(central_state_t *s, const status_msg_t *status,
                          uint32_t session, uint32_t sequence, int snapshot) {
    command_record_t *record = monitor_find_command(&s->monitor, status->command_id);
    request_state_t previous = record ? record->state : REQUEST_QUEUED;
    int was_synced = s->monitor.intersections[status->intersection_id].synchronized;
    if (!monitor_status(&s->monitor, status, session, sequence, monotonic_ns(), snapshot)) return;
    if (snapshot && !was_synced) event_locked(s, "I%u state synchronized", status->intersection_id + 1);
    if (record && record->state != previous)
        event_locked(s, "Command %u I%u %s", record->id, record->target + 1, request_name(record->state));
}

static int handle_status(int rcvid, any_msg_t *msg, reply_t *reply, void *context) {
    central_state_t *s = context;
    (void)rcvid;
    pthread_mutex_lock(&s->mutex);
    status_locked(s, &msg->payload.status, msg->header.session_id, msg->header.sequence, 0);
    pthread_mutex_unlock(&s->mutex);
    reply->status = REPLY_ACCEPTED;
    return 0;
}

static int handle_railway(int rcvid, any_msg_t *msg, reply_t *reply, void *context) {
    central_state_t *s = context;
    (void)rcvid;
    pthread_mutex_lock(&s->mutex);
    monitor_railway_status(&s->monitor, &msg->payload.railway_status,
                           msg->header.session_id, msg->header.sequence, monotonic_ns(), 0);
    pthread_mutex_unlock(&s->mutex);
    reply->status = REPLY_ACCEPTED;
    return 0;
}

static int handle_fault(int rcvid, any_msg_t *msg, reply_t *reply, void *context) {
    central_state_t *s = context;
    const fault_msg_t *fault = &msg->payload.fault;
    char description[sizeof(fault->description) + 1];
    int index = monitor_peer_index(msg->header.src);
    unsigned i;
    (void)rcvid;
    memcpy(description, fault->description, sizeof(fault->description));
    description[sizeof(fault->description)] = '\0';
    for (i = 0; description[i]; ++i) if ((unsigned char)description[i] < 32) description[i] = ' ';
    pthread_mutex_lock(&s->mutex);
    s->faults[index][fault->source_id] = *fault;
    s->active_faults[index][fault->source_id] = fault->fault_type != FAULT_NONE;
    event_locked(s, "%s %u fault %u severity %u: %s", controller_name(msg->header.src),
                 fault->source_id + 1, fault->fault_type, fault->severity, description);
    pthread_mutex_unlock(&s->mutex);
    reply->status = REPLY_ACCEPTED;
    return 0;
}

static int handle_test(int rcvid, any_msg_t *msg, reply_t *reply, void *context) {
    central_state_t *s = context;
    (void)rcvid;
    pthread_mutex_lock(&s->mutex);
    event_locked(s, "Test message from %s", controller_name(msg->header.src));
    pthread_mutex_unlock(&s->mutex);
    reply->status = REPLY_ACCEPTED;
    return 0;
}

static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test },
    { MSG_HEARTBEAT, CONTROLLER_LOCAL, handle_heartbeat },
    { MSG_HEARTBEAT, CONTROLLER_TRAIN, handle_heartbeat },
    { MSG_STATUS_UPDATE, CONTROLLER_LOCAL, handle_status },
    { MSG_RAILWAY_STATUS, CONTROLLER_TRAIN, handle_railway },
    { MSG_FAULT_ALERT, CONTROLLER_LOCAL, handle_fault },
    { MSG_FAULT_ALERT, CONTROLLER_TRAIN, handle_fault }
};

static void *message_thread(void *arg) {
    receive_loop(arg);
    return NULL;
}

static uint16_t next_id_locked(central_state_t *s) {
    do { if (++s->next_id == 0) ++s->next_id; }
    while (monitor_find_command(&s->monitor, s->next_id));
    return s->next_id;
}

static void stamp_locked(central_state_t *s, any_msg_t *msg) {
    msg->header.session_id = s->monitor.session_id;
    msg->header.sequence = ++s->sequence;
    get_timestamp(msg->header.timestamp, sizeof(msg->header.timestamp));
}

static int take_command(central_state_t *s, any_msg_t *msg) {
    command_record_t *record;
    uint16_t id;
    pthread_mutex_lock(&s->mutex);
    if (!s->queue_count || !s->running) { pthread_mutex_unlock(&s->mutex); return 0; }
    *msg = s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1) % CENTRAL_COMMAND_QUEUE;
    --s->queue_count;
    id = protocol_command_id(msg);
    record = monitor_find_command(&s->monitor, id);
    if (!record || record->state != REQUEST_QUEUED ||
        !monitor_can_command(&s->monitor, record->target, monotonic_ns())) {
        if (record && record->state == REQUEST_QUEUED) {
            record->state = REQUEST_REJECTED;
            event_locked(s, "Command %u canceled before send: current state unavailable", id);
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (msg->header.type == MSG_OVERRIDE_REQUEST &&
        msg->payload.override_request.action == CMD_TEMPORARY) {
        const status_msg_t *status = &s->monitor.intersections[record->target].status;
        if (status->railway_preempt || status->mode == MODE_RAILWAY || status->mode == MODE_FAILSAFE) {
            record->state = REQUEST_REJECTED;
            event_locked(s, "Command %u canceled before send: railway hold or fail-safe", id);
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
    }
    record->state = REQUEST_SENDING;
    stamp_locked(s, msg);
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

static void send_command(peer_context_t *peer, any_msg_t *msg) {
    central_state_t *s = peer->state;
    reply_t reply;
    uint16_t id = protocol_command_id(msg);
    int result = send_message(&s->connections[peer->index], msg, &reply);
    command_record_t *record;
    pthread_mutex_lock(&s->mutex);
    record = monitor_find_command(&s->monitor, id);
    if (record && record->state == REQUEST_SENDING) {
        if (result == SEND_OK && reply.type == 0)
            record->state = reply.status == REPLY_APPLIED ? REQUEST_APPLIED : REQUEST_ACCEPTED;
        else if (result == SEND_REJECTED) record->state = REQUEST_REJECTED;
        else record->state = REQUEST_UNCERTAIN;
        event_locked(s, "Command %u I%u %s", id, record->target + 1, request_name(record->state));
    }
    pthread_mutex_unlock(&s->mutex);
}

static void request_status(peer_context_t *peer, unsigned target, int *last_error) {
    central_state_t *s = peer->state;
    any_msg_t msg;
    reply_t reply;
    int result, valid = 0;
    protocol_init_message(&msg, MSG_STATUS_REQUEST, CONTROLLER_CENTRAL, peer->source);
    msg.payload.status_request.target_id = (uint8_t)target;
    pthread_mutex_lock(&s->mutex);
    msg.payload.status_request.request_id = next_id_locked(s);
    stamp_locked(s, &msg);
    pthread_mutex_unlock(&s->mutex);
    result = send_message(&s->connections[peer->index], &msg, &reply);
    if (result == SEND_OK && reply.status == REPLY_APPLIED) {
        if (peer->source == CONTROLLER_LOCAL)
            valid = reply.type == MSG_STATUS_UPDATE && protocol_valid_status(&reply.payload.status) &&
                    reply.payload.status.intersection_id == target;
        else valid = reply.type == MSG_RAILWAY_STATUS &&
                     protocol_valid_railway_status(&reply.payload.railway_status) &&
                     reply.payload.railway_status.crossing_id == target;
    }
    pthread_mutex_lock(&s->mutex);
    if (valid) {
        if (peer->source == CONTROLLER_LOCAL)
            status_locked(s, &reply.payload.status, reply.session_id, reply.sequence, 1);
        else monitor_railway_status(&s->monitor, &reply.payload.railway_status,
                                    reply.session_id, reply.sequence, monotonic_ns(), 1);
        *last_error = 0;
    } else {
        int error = result == SEND_REJECTED ? 1 : 2;
        if (*last_error != error)
            event_locked(s, "%s snapshot %s", controller_name(peer->source),
                         error == 1 ? "not supported by peer" : "unavailable or invalid");
        *last_error = error;
    }
    pthread_mutex_unlock(&s->mutex);
}

static void *peer_thread(void *arg) {
    peer_context_t *peer = arg;
    central_state_t *s = peer->state;
    connection_t *conn = &s->connections[peer->index];
    uint64_t next_tick = monotonic_ns();
    unsigned target = 0, misses = 0;
    int last_snapshot_error = 0, snapshot_due = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        struct timespec deadline;
        uint64_t now;
        int running;
        pthread_mutex_lock(&s->mutex);
        while (s->running && (now = monotonic_ns()) < next_tick) {
            deadline.tv_sec = (time_t)(next_tick / NANOSECONDS_PER_SEC);
            deadline.tv_nsec = (long)(next_tick % NANOSECONDS_PER_SEC);
            pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        }
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
        next_tick = monotonic_ns() + HEARTBEAT_PERIOD_SEC * NANOSECONDS_PER_SEC;
        if (!connection_is_connected(conn)) {
            int connected;
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            connected = connection_try_connect(conn);
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
            if (!connected) continue;
            pthread_mutex_lock(&s->mutex);
            monitor_disconnect(&s->monitor, peer->source);
            s->monitor.peers[peer->index].connected = 1;
            event_locked(s, "%s connected; requesting current state", controller_name(peer->source));
            pthread_mutex_unlock(&s->mutex);
            misses = 0;
            target = 0;
            last_snapshot_error = 0;
            snapshot_due = 0;
        }
        if (send_heartbeat(conn, CONTROLLER_CENTRAL, peer->source) == SEND_OK) {
            int was_online;
            misses = 0;
            pthread_mutex_lock(&s->mutex);
            was_online = monitor_peer_online(&s->monitor, peer->source, monotonic_ns());
            monitor_heartbeat(&s->monitor, peer->source,
                              s->monitor.peers[peer->index].seen ?
                              s->monitor.peers[peer->index].healthy : 1, monotonic_ns());
            if (!was_online) event_locked(s, "%s heartbeat restored", controller_name(peer->source));
            pthread_mutex_unlock(&s->mutex);
        } else if (++misses >= HEARTBEAT_MISS_LIMIT) {
            connection_close(conn);
            pthread_mutex_lock(&s->mutex);
            monitor_disconnect(&s->monitor, peer->source);
            if (peer->source == CONTROLLER_LOCAL) s->queue_head = s->queue_count = 0;
            event_locked(s, "%s link unavailable after %u missed heartbeats",
                         controller_name(peer->source), misses);
            pthread_mutex_unlock(&s->mutex);
            continue;
        } else continue;
        if (peer->source == CONTROLLER_LOCAL && !snapshot_due) {
            any_msg_t msg;
            if (take_command(s, &msg)) {
                send_command(peer, &msg);
                snapshot_due = 1;
                continue;
            }
        }
        request_status(peer, target, &last_snapshot_error);
        snapshot_due = 0;
        target = (target + 1) % (peer->source == CONTROLLER_LOCAL ? NUM_INTERSECTIONS : NUM_CROSSINGS);
    }
    return NULL;
}

static void print_commands(void) {
    monitor_t view;
    unsigned i;
    pthread_mutex_lock(&state.mutex);
    view = state.monitor;
    pthread_mutex_unlock(&state.mutex);
    printf("Command  Target  Type  State\n");
    for (i = 0; i < CENTRAL_COMMAND_HISTORY; ++i) {
        const command_record_t *record = &view.commands[(view.command_next + i) % CENTRAL_COMMAND_HISTORY];
        if (record->id) printf("%-8u I%-6u %-5u %s\n", record->id, record->target + 1,
                               record->type, request_name(record->state));
    }
}

static void print_faults(void) {
    fault_msg_t faults[2][NUM_INTERSECTIONS];
    int active[2][NUM_INTERSECTIONS];
    unsigned i, j;
    pthread_mutex_lock(&state.mutex);
    memcpy(faults, state.faults, sizeof(faults));
    memcpy(active, state.active_faults, sizeof(active));
    pthread_mutex_unlock(&state.mutex);
    printf("Active faults\n");
    for (i = 0; i < 2; ++i) for (j = 0; j < (i ? NUM_CROSSINGS : NUM_INTERSECTIONS); ++j) {
        const fault_msg_t *f = &faults[i][j];
        if (active[i][j]) printf("%c%u type %u severity %u\n", i ? 'P' : 'I', j + 1,
                                 f->fault_type, f->severity);
    }
}

static int enqueue_command(any_msg_t *prototype, unsigned target) {
    unsigned first = target == INTERSECTION_ALL ? 0 : target;
    unsigned last = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    unsigned i, available = 0;
    uint64_t now = monotonic_ns();
    pthread_mutex_lock(&state.mutex);
    for (i = 0; i < CENTRAL_COMMAND_HISTORY; ++i) {
        const command_record_t *record = &state.monitor.commands[i];
        if (!record->id || record->state == REQUEST_APPLIED || record->state == REQUEST_REJECTED ||
            record->state == REQUEST_EXPIRED) ++available;
    }
    if (state.queue_count + last - first > CENTRAL_COMMAND_QUEUE || available < last - first) {
        event_locked(&state, "Command queue/history is full; awaiting pending results");
        pthread_mutex_unlock(&state.mutex);
        return -1;
    }
    for (i = first; i < last; ++i) {
        const status_msg_t *status = &state.monitor.intersections[i].status;
        if (!monitor_can_command(&state.monitor, i, now)) {
            event_locked(&state, "I%u requires a fresh snapshot and healthy link before commands", i + 1);
            pthread_mutex_unlock(&state.mutex);
            return -1;
        }
        if (prototype->header.type == MSG_OVERRIDE_REQUEST &&
            prototype->payload.override_request.action == CMD_TEMPORARY &&
            (status->railway_preempt || status->mode == MODE_RAILWAY || status->mode == MODE_FAILSAFE)) {
            event_locked(&state, "I%u override unavailable during railway hold or fail-safe", i + 1);
            pthread_mutex_unlock(&state.mutex);
            return -1;
        }
    }
    for (i = first; i < last; ++i) {
        any_msg_t msg = *prototype;
        uint16_t id = next_id_locked(&state);
        unsigned slot = (state.queue_head + state.queue_count++) % CENTRAL_COMMAND_QUEUE;
        if (msg.header.type == MSG_MODE_COMMAND) {
            msg.payload.mode_cmd.intersection_id = (uint8_t)i;
            msg.payload.mode_cmd.command_id = id;
        } else if (msg.header.type == MSG_COORDINATION_COMMAND) {
            msg.payload.coordination.intersection_id = (uint8_t)i;
            msg.payload.coordination.command_id = id;
        } else {
            msg.payload.override_request.intersection_id = (uint8_t)i;
            msg.payload.override_request.command_id = id;
        }
        monitor_add_command(&state.monitor, id, msg.header.type, (uint8_t)i);
        state.queue[slot] = msg;
        event_locked(&state, "Command %u queued for I%u", id, i + 1);
    }
    pthread_mutex_unlock(&state.mutex);
    return 0;
}

static void execute_command(char *line) {
    any_msg_t msg;
    unsigned target;
    size_t length;
    while (*line == ' ' || *line == '\t') ++line;
    length = strlen(line);
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
        line[--length] = '\0';
    if (!*line) return;
    if (!strcmp(line, "status")) display_ui();
    else if (!strcmp(line, "help")) print_help();
    else if (!strcmp(line, "commands")) print_commands();
    else if (!strcmp(line, "faults")) print_faults();
    else if (!strcmp(line, "quit")) interrupted = 1;
    else if (central_parse_command(line, &msg, &target)) enqueue_command(&msg, target);
    else printf("Invalid command or arguments. Type help for usage.\n");
}

int main(int argc, char *argv[]) {
    connection_mode_t mode = CONN_MODE_GLOBAL;
    const char *log_path = "/tmp/central_controller.log";
    name_attach_t *attach = NULL;
    receive_context_t receiver;
    pthread_condattr_t condattr;
    pthread_t receive_worker, logger, peers[2];
    peer_context_t contexts[2];
    unsigned started_peers = 0;
    int receive_started = 0, log_started = 0, receiver_initialized = 0;
    int result = EXIT_FAILURE, i, overflow = 0;
    char line[256];
    size_t used = 0;
    uint64_t last_display = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [-l|-g] [-o log-file]\n", argv[0]);
            print_help();
            return EXIT_SUCCESS;
        }
        if (!strcmp(argv[i], "-l")) mode = CONN_MODE_LOCAL;
        else if (!strcmp(argv[i], "-g")) mode = CONN_MODE_GLOBAL;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) log_path = argv[++i];
        else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); return EXIT_FAILURE; }
    }
    memset(&state, 0, sizeof(state));
    if (pthread_mutex_init(&state.mutex, NULL) != 0) return EXIT_FAILURE;
    if (pthread_condattr_init(&condattr) != 0) { pthread_mutex_destroy(&state.mutex); return EXIT_FAILURE; }
    if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&state.changed, &condattr) != 0) {
        pthread_condattr_destroy(&condattr);
        pthread_mutex_destroy(&state.mutex);
        return EXIT_FAILURE;
    }
    pthread_condattr_destroy(&condattr);
    state.running = 1;
    state.monitor.session_id = (uint32_t)(monotonic_ns() ^ ((uint64_t)getpid() << 16) ^ (uint64_t)time(NULL));
    if (!state.monitor.session_id) state.monitor.session_id = 1;
    state.next_id = (uint16_t)state.monitor.session_id;
    connection_init(&state.connections[0], LOCAL_SERVICE_NAME, mode, &state.mutex);
    connection_init(&state.connections[1], TRAIN_SERVICE_NAME, mode, &state.mutex);
    state.log_file = fopen(log_path, "a");
    if (!state.log_file) { perror(log_path); goto cleanup; }
    attach = connection_register_service(CENTRAL_SERVICE_NAME, mode);
    if (!attach) goto cleanup;
    receive_init(&receiver, attach, handlers, sizeof(handlers) / sizeof(handlers[0]),
                 &state, CONTROLLER_CENTRAL);
    receiver_initialized = 1;
    if (pthread_create(&logger, NULL, log_thread, &state) != 0) goto cleanup;
    log_started = 1;
    if (pthread_create(&receive_worker, NULL, message_thread, &receiver) != 0) goto cleanup;
    receive_started = 1;
    for (i = 0; i < 2; ++i) {
        contexts[i].state = &state;
        contexts[i].source = i == 0 ? CONTROLLER_LOCAL : CONTROLLER_TRAIN;
        contexts[i].index = (unsigned)i;
        if (pthread_create(&peers[i], NULL, peer_thread, &contexts[i]) != 0) goto cleanup;
        ++started_peers;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central started, session %u", state.monitor.session_id);
    pthread_mutex_unlock(&state.mutex);
    result = EXIT_SUCCESS;
    while (!interrupted) {
        struct pollfd input = { STDIN_FILENO, POLLIN, 0 };
        uint64_t now = monotonic_ns();
        int poll_result;
        if (now - last_display >= NANOSECONDS_PER_SEC) { display_ui(); last_display = now; }
        poll_result = poll(&input, 1, 200);
        if (poll_result < 0) { if (errno == EINTR) continue; perror("poll"); result = EXIT_FAILURE; break; }
        if (poll_result && (input.revents & (POLLIN | POLLHUP))) {
            char buffer[128];
            ssize_t amount = read(STDIN_FILENO, buffer, sizeof(buffer));
            ssize_t position;
            if (amount == 0) break;
            if (amount < 0) { if (errno == EINTR) continue; perror("read"); result = EXIT_FAILURE; break; }
            for (position = 0; position < amount && !interrupted; ++position) {
                if (buffer[position] == '\n') {
                    line[used] = '\0';
                    if (!overflow) execute_command(line);
                    else printf("Command too long\n");
                    used = 0;
                    overflow = 0;
                    printf("> ");
                    fflush(stdout);
                } else if (used + 1 < sizeof(line)) line[used++] = buffer[position];
                else overflow = 1;
            }
        } else if (poll_result && (input.revents & (POLLERR | POLLNVAL))) {
            result = EXIT_FAILURE;
            break;
        }
    }
cleanup:
    pthread_mutex_lock(&state.mutex);
    event_locked(&state, "Central stopping");
    state.running = 0;
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
    if (receiver_initialized) receive_stop(&receiver);
    for (i = 0; i < (int)started_peers; ++i) pthread_cancel(peers[i]);
    for (i = 0; i < (int)started_peers; ++i) pthread_join(peers[i], NULL);
    if (receive_started) pthread_join(receive_worker, NULL);
    pthread_mutex_lock(&state.mutex);
    state.log_stopping = 1;
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
    if (log_started) pthread_join(logger, NULL);
    if (receiver_initialized) receive_destroy(&receiver);
    connection_destroy(&state.connections[0]);
    connection_destroy(&state.connections[1]);
    if (attach) connection_unregister_service(attach);
    if (state.log_file) fclose(state.log_file);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.mutex);
    return result;
}
