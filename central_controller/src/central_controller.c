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

typedef enum {
    REQUEST_QUEUED,
    REQUEST_SENDING,
    REQUEST_ACCEPTED,
    REQUEST_REJECTED,
    REQUEST_UNCONFIRMED
} request_state_t;

typedef struct {
    uint16_t id;
    unsigned target;
    request_state_t state;
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
    int running, log_stopping;
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
    static const char *names[] = {"QUEUED", "SENDING", "ACCEPTED", "REJECTED", "UNCONFIRMED"};
    return value <= REQUEST_UNCONFIRMED ? names[value] : "UNKNOWN";
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
           "  status | commands | faults | help | quit\n");
}

static void display_ui(void) {
    central_monitor_t view;
    char events[8][EVENT_TEXT_SIZE];
    unsigned count, next, dropped, i;
    uint64_t now = central_monotonic_ns();
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
                    age_seconds(now, entry->received_at) >= CENTRAL_STATUS_STALE_SEC ? "STALE" : "CURRENT";
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
        const central_crossing_status_t *entry = &view.crossings[i];
        if (!entry->valid) { printf("P%u  Waiting for status\n", i + 1); continue; }
        printf("P%u  Train %-11s Gate %-7s Fault %u Age %.1fs %s\n", i + 1,
               trains[entry->status.train_state], gates[entry->status.gate_state],
               entry->status.fault, age_seconds(now, entry->received_at),
               !central_peer_online(&view, CONTROLLER_TRAIN, now) ? "OFFLINE" :
               !entry->synchronized ? "WAITING UPDATE" :
               age_seconds(now, entry->received_at) >= CENTRAL_STATUS_STALE_SEC ? "STALE" : "CURRENT");
    }
    printf("\nRecent events\n");
    for (i = 0; i < count; ++i) printf("%s\n", events[(next + 8 - count + i) % 8]);
    if (dropped) printf("Log records dropped: %u\n", dropped);
    printf("\nType help for commands. Remaining times are reported snapshots.\n> ");
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
            fprintf(stderr, "Event log write failed\n");
            break;
        }
    }
    return NULL;
}

static int handle_message(const test_message_t *message, reply_t *reply, void *context) {
    central_state_t *s = context;
    uint64_t now = central_monotonic_ns();
    int result = 0;
    pthread_mutex_lock(&s->mutex);
    switch (message->header.type) {
        case MSG_HEARTBEAT:
            central_observe_peer(&s->monitor, message->header.src, now);
            break;
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

static void discard_queue_locked(central_state_t *s) {
    unsigned i;
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        if (s->commands[i].id && s->commands[i].state == REQUEST_QUEUED)
            s->commands[i].state = REQUEST_REJECTED;
    }
    s->queue_head = s->queue_count = 0;
}

static int take_command(central_state_t *s, test_message_t *message) {
    command_record_t *record;
    uint16_t id;
    pthread_mutex_lock(&s->mutex);
    if (!s->running || !s->queue_count) { pthread_mutex_unlock(&s->mutex); return 0; }
    *message = s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1) % COMMAND_QUEUE_SIZE;
    --s->queue_count;
    id = central_command_id(message);
    record = find_command_locked(s, id);
    if (!record || record->state != REQUEST_QUEUED ||
        !central_can_command(&s->monitor, record->target, central_monotonic_ns())) {
        if (record && record->state == REQUEST_QUEUED) {
            record->state = REQUEST_REJECTED;
            event_locked(s, "Command %u canceled before send: current state unavailable", id);
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    record->state = REQUEST_SENDING;
    central_timestamp(message->header.timestamp, sizeof(message->header.timestamp));
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

static void send_command(peer_context_t *peer, test_message_t *message) {
    central_state_t *s = peer->state;
    reply_t reply;
    uint16_t id = central_command_id(message);
    int result = central_send(&s->links[peer->index], message, &reply);
    command_record_t *record;
    pthread_mutex_lock(&s->mutex);
    record = find_command_locked(s, id);
    if (record) {
        record->state = result == CENTRAL_SEND_OK ? REQUEST_ACCEPTED :
                        result == CENTRAL_SEND_REJECTED ? REQUEST_REJECTED : REQUEST_UNCONFIRMED;
        event_locked(s, "Command %u I%u %s", id, record->target + 1, request_name(record->state));
    }
    pthread_mutex_unlock(&s->mutex);
}

static void *peer_thread(void *argument) {
    peer_context_t *peer = argument;
    central_state_t *s = peer->state;
    central_link_t *link = &s->links[peer->index];
    uint64_t next_tick = central_monotonic_ns();
    unsigned misses = 0;
    for (;;) {
        struct timespec deadline;
        int running;
        pthread_mutex_lock(&s->mutex);
        while (s->running && central_monotonic_ns() < next_tick) {
            deadline.tv_sec = (time_t)(next_tick / CENTRAL_NSEC);
            deadline.tv_nsec = (long)(next_tick % CENTRAL_NSEC);
            pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        }
        running = s->running;
        pthread_mutex_unlock(&s->mutex);
        if (!running) break;
        next_tick = central_monotonic_ns() + HEARTBEAT_PERIOD_SEC * CENTRAL_NSEC;
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
        } else if (++misses >= HEARTBEAT_MISS_LIMIT) {
            central_link_close(link);
            pthread_mutex_lock(&s->mutex);
            central_peer_disconnected(&s->monitor, peer->source);
            if (peer->source == CONTROLLER_LOCAL) discard_queue_locked(s);
            event_locked(s, "%s offline after %u missed heartbeats", controller_name(peer->source), misses);
            pthread_mutex_unlock(&s->mutex);
            continue;
        } else continue;
        if (peer->source == CONTROLLER_LOCAL) {
            test_message_t message;
            if (take_command(s, &message)) send_command(peer, &message);
        }
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
    printf("Command  Target  State\n");
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
        const command_record_t *record = &commands[(next + i) % COMMAND_HISTORY_SIZE];
        if (record->id) printf("%-8u I%-6u %s\n", record->id, record->target + 1, request_name(record->state));
    }
    printf("ACCEPTED confirms receipt; the Local status shows the reported operating mode.\n");
}

static void print_faults(void) {
    fault_msg_t faults[2][NUM_INTERSECTIONS];
    int active[2][NUM_INTERSECTIONS];
    unsigned i, j;
    pthread_mutex_lock(&state.mutex);
    memcpy(faults, state.faults, sizeof(faults));
    memcpy(active, state.fault_active, sizeof(active));
    pthread_mutex_unlock(&state.mutex);
    printf("Fault alerts awaiting a clear notification\n");
    for (i = 0; i < 2; ++i) for (j = 0; j < (i ? NUM_CROSSINGS : NUM_INTERSECTIONS); ++j)
        if (active[i][j]) printf("%c%u type %u severity %u\n", i ? 'P' : 'I', j + 1,
                                 faults[i][j].fault_type, faults[i][j].severity);
}

static int enqueue_command(const test_message_t *prototype, unsigned target) {
    unsigned first = target == INTERSECTION_ALL ? 0 : target;
    unsigned last = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    unsigned available = 0, i;
    uint64_t now = central_monotonic_ns();
    pthread_mutex_lock(&state.mutex);
    for (i = 0; i < COMMAND_HISTORY_SIZE; ++i)
        if (!state.commands[i].id || (state.commands[i].state != REQUEST_QUEUED &&
                                     state.commands[i].state != REQUEST_SENDING)) ++available;
    if (state.queue_count + last - first > COMMAND_QUEUE_SIZE || available < last - first) {
        event_locked(&state, "Command queue is full");
        pthread_mutex_unlock(&state.mutex);
        return -1;
    }
    for (i = first; i < last; ++i) if (!central_can_command(&state.monitor, i, now)) {
        event_locked(&state, "I%u requires a fresh Local status and healthy link before commands", i + 1);
        pthread_mutex_unlock(&state.mutex);
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
        state.command_next = (slot + 1) % COMMAND_HISTORY_SIZE;
        central_command_set_id(&message, id);
        central_command_set_target(&message, i);
        state.queue[(state.queue_head + state.queue_count++) % COMMAND_QUEUE_SIZE] = message;
        event_locked(&state, "Command %u queued for I%u", id, i + 1);
    }
    pthread_mutex_unlock(&state.mutex);
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
    char line[256];
    size_t used = 0;
    uint64_t last_display = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [-l|-g] [-o log-file]\n", argv[0]);
            print_help();
            return EXIT_SUCCESS;
        }
        if (!strcmp(argv[i], "-l")) mode = CENTRAL_IPC_LOCAL;
        else if (!strcmp(argv[i], "-g")) mode = CENTRAL_IPC_GLOBAL;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) log_path = argv[++i];
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
    while (!interrupted) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        uint64_t now = central_monotonic_ns();
        int ready;
        if (now - last_display >= CENTRAL_NSEC) { display_ui(); last_display = now; }
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
                    if (overflow) printf("Command too long\n");
                    else execute_command(line);
                    used = 0;
                    overflow = 0;
                    printf("> ");
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
