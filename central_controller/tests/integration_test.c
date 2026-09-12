#include "../src/ipc.h"
#include "../src/commands.h"
#include <poll.h>
#include <signal.h>
#include <sys/iomsg.h>
#include <sys/wait.h>

#define TEST_CENTRAL "traffic_test_central_only"
#define TEST_LOCAL "traffic_test_central_only_local"
#define TEST_TRAIN "traffic_test_central_only_train"

typedef struct {
    unsigned type, target, id;
    uint64_t received_at;
    mode_cmd_msg_t mode;
    coordination_command_msg_t coordination;
} observation_t;
static pid_t children[3] = {-1, -1, -1};
static int observations[2] = {-1, -1};
static int input_fd = -1, output_fd = -1;
static char output[65536], log_path[128];
static size_t output_used;
static unsigned checks;

static void cleanup(void) {
    unsigned i;
    for (i = 0; i < 3; ++i) if (children[i] > 0) {
        kill(children[i], SIGKILL);
        waitpid(children[i], NULL, 0);
    }
    for (i = 0; i < 2; ++i) if (observations[i] >= 0) close(observations[i]);
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);
    if (log_path[0]) unlink(log_path);
}

static void require(int condition, const char *description) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s (errno=%d)\n%s\n", description, errno, output);
        exit(EXIT_FAILURE);
    }
}

static void drain_output(void) {
    ssize_t length;
    while (output_used < sizeof(output) - 1 &&
           (length = read(output_fd, output + output_used, sizeof(output) - 1 - output_used)) > 0)
        output_used += length;
    output[output_used] = '\0';
}

static int log_contains(const char *text) {
    char buffer[65536];
    FILE *file = fopen(log_path, "r");
    size_t length;
    if (!file) return 0;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return strstr(buffer, text) != NULL;
}

static int wait_text(const char *text, int in_log, unsigned timeout_ms) {
    uint64_t deadline = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    do {
        struct pollfd fd = {output_fd, POLLIN, 0};
        drain_output();
        if (in_log ? log_contains(text) : strstr(output, text) != NULL) return 1;
        poll(&fd, 1, 20);
    } while (central_monotonic_ns() < deadline);
    return 0;
}

static void command(const char *text) {
    require(write(input_fd, text, strlen(text)) == (ssize_t)strlen(text), "write CLI command");
}

static void fixture_peer(const char *name, int notice_fd, int normal_commands) {
    name_attach_t *attach = name_attach(NULL, name, 0);
    observation_t observation = {0};
    unsigned command_delay_ms = 0;
    if (!attach || write(notice_fd, &observation, sizeof(observation)) != sizeof(observation)) _exit(2);
    for (;;) {
        union { struct _pulse pulse; test_message_t message; } frame;
        reply_t reply = {0};
        int rcvid = MsgReceive(attach->chid, &frame, sizeof(frame), NULL);
        if (rcvid < 0) _exit(3);
        if (!rcvid) {
            if (frame.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(frame.pulse.scoid);
            continue;
        }
        if (frame.pulse.type == _IO_CONNECT) { MsgReply(rcvid, 0, NULL, 0); continue; }
        if (frame.pulse.type >= _IO_BASE) { MsgError(rcvid, ENOSYS); continue; }
        central_timestamp(reply.timestamp, sizeof(reply.timestamp));
        reply.command_id = central_command_id(&frame.message);
        if (frame.message.header.type == MSG_TEST && normal_commands) {
            if (!strcmp(frame.message.data, "fixture:delay=80")) command_delay_ms = 80;
            else if (!strcmp(frame.message.data, "fixture:delay=250")) command_delay_ms = 250;
            else if (!strcmp(frame.message.data, "fixture:delay=0")) command_delay_ms = 0;
            else reply.status = -1;
            MsgReply(rcvid, 0, &reply, sizeof(reply));
            continue;
        }
        memset(&observation, 0, sizeof(observation));
        observation.type = frame.message.header.type;
        observation.target = central_command_target(&frame.message);
        observation.id = reply.command_id;
        observation.received_at = central_monotonic_ns();
        if (observation.type == MSG_MODE_COMMAND)
            memcpy(&observation.mode, frame.message.data, sizeof(observation.mode));
        if (observation.type == MSG_COORDINATION_COMMAND)
            memcpy(&observation.coordination, frame.message.data, sizeof(observation.coordination));
        if (write(notice_fd, &observation, sizeof(observation)) != sizeof(observation)) _exit(4);
        if (!normal_commands && frame.message.header.type == MSG_MODE_COMMAND) {
            if (observation.target == I2) { reply.status = -1; reply.command_id = 0; }
            if (observation.target == I6) raise(SIGSTOP);
        }
        if (normal_commands && command_delay_ms &&
            (observation.type == MSG_MODE_COMMAND || observation.type == MSG_COORDINATION_COMMAND)) {
            struct timespec delay = {0, (long)command_delay_ms * 1000000L};
            while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {}
        }
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }
}

static void start_peer(unsigned index, const char *name, int normal_commands) {
    int pipe_fd[2];
    observation_t ready;
    require(pipe(pipe_fd) == 0, "peer fixture pipe");
    children[index] = fork();
    require(children[index] >= 0, "peer fixture fork");
    if (!children[index]) {
        close(pipe_fd[0]);
        fixture_peer(name, pipe_fd[1], normal_commands);
        _exit(5);
    }
    close(pipe_fd[1]);
    observations[index] = pipe_fd[0];
    require(read(observations[index], &ready, sizeof(ready)) == sizeof(ready) && ready.type == 0,
            "peer fixture ready");
    fcntl(observations[index], F_SETFL, O_NONBLOCK);
}

static void start_central(const char *binary, const char *freshness) {
    int input_pipe[2], output_pipe[2];
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);
    input_fd = output_fd = -1;
    output_used = 0;
    output[0] = '\0';
    unlink(log_path);
    require(pipe(input_pipe) == 0 && pipe(output_pipe) == 0, "central pipes");
    children[2] = fork();
    require(children[2] >= 0, "central fork");
    if (!children[2]) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        if (freshness)
            execl(binary, binary, "-l", "-s", freshness, "-o", log_path, (char *)NULL);
        else
            execl(binary, binary, "-l", "-o", log_path, (char *)NULL);
        _exit(6);
    }
    close(input_pipe[0]); close(output_pipe[1]);
    input_fd = input_pipe[1]; output_fd = output_pipe[0];
    fcntl(output_fd, F_SETFL, O_NONBLOCK);
}

static int send_frame(int coid, test_message_t *frame, reply_t *reply) {
    uint64_t timeout = UINT64_C(1000000000);
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    memset(reply, 0x5a, sizeof(*reply));
    return MsgSend(coid, frame, sizeof(*frame), reply, sizeof(*reply));
}

static void send_status(int coid, unsigned id) {
    test_message_t frame;
    reply_t reply;
    status_msg_t status = {0};
    status.intersection_id = id;
    status.ns_state = LIGHT_GREEN;
    status.ew_state = LIGHT_RED;
    status.time_remaining = 20 + id;
    central_message_init(&frame, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    memcpy(frame.data, &status, sizeof(status));
    require(send_frame(coid, &frame, &reply) == 0 && reply.status == 0, "legacy Local status accepted");
}

static void send_railway(int coid, unsigned id) {
    test_message_t frame;
    reply_t reply;
    railway_status_msg_t status = {0};
    status.crossing_id = id;
    status.train_state = id == P3 ? TRAIN_AT_CROSSING : TRAIN_NONE;
    status.gate_state = id == P3 ? GATE_CLOSED : GATE_OPEN;
    central_message_init(&frame, MSG_RAILWAY_STATUS, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
    memcpy(frame.data, &status, sizeof(status));
    require(send_frame(coid, &frame, &reply) == 0 && reply.status == 0, "legacy Train status accepted");
}

static int observation_seen(unsigned peer, unsigned type, unsigned target, unsigned timeout_ms) {
    uint64_t deadline = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    do {
        observation_t event;
        struct pollfd fd = {observations[peer], POLLIN, 0};
        while (read(observations[peer], &event, sizeof(event)) == sizeof(event))
            if (event.type == type && (type == MSG_HEARTBEAT || event.target == target)) return 1;
        drain_output();
        poll(&fd, 1, 20);
    } while (central_monotonic_ns() < deadline);
    return 0;
}

static double wait_for_exit(void) {
    uint64_t started = central_monotonic_ns();
    int rc, status = 0;
    do {
        struct pollfd fd = {output_fd, POLLIN, 0};
        drain_output();
        rc = waitpid(children[2], &status, WNOHANG);
        if (rc == children[2]) break;
        poll(&fd, 1, 20);
    } while (central_monotonic_ns() - started < UINT64_C(2500000000));
    require(rc == children[2], "Central exits after all legacy requests are released");
    children[2] = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Central shutdown exit status");
    require(log_contains("Central stopping"), "shutdown event flushed");
    return (central_monotonic_ns() - started) / 1000000.0;
}

static void release_stopped_peer(void) {
    int status;
    require(wait_text("Waiting for an outstanding legacy IPC request to be released by its peer",
                       0, 2000), "operator warned of outstanding legacy request during shutdown");
    require(waitpid(children[2], &status, WNOHANG) == 0,
            "legacy UNBLOCK retains process until peer releases reply");
    require(kill(children[0], SIGCONT) == 0, "resume only the stopped Local fixture");
    wait_for_exit();
}

static void clear_observations(void) {
    observation_t event;
    unsigned peer;
    for (peer = 0; peer < 2; ++peer)
        while (read(observations[peer], &event, sizeof(event)) == sizeof(event)) {}
}

static void quiet_output_start(void) {
    drain_output();
    output_used = 0;
    output[0] = '\0';
}

static void pump_output(unsigned milliseconds) {
    uint64_t deadline = central_monotonic_ns() + milliseconds * UINT64_C(1000000);
    do {
        struct pollfd fd = {output_fd, POLLIN, 0};
        drain_output();
        poll(&fd, 1, 20);
    } while (central_monotonic_ns() < deadline);
    drain_output();
}

static observation_t next_command(unsigned timeout_ms) {
    uint64_t deadline = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    observation_t event = {0};
    do {
        struct pollfd fd = {observations[0], POLLIN, 0};
        while (read(observations[0], &event, sizeof(event)) == sizeof(event))
            if (event.type == MSG_MODE_COMMAND || event.type == MSG_COORDINATION_COMMAND)
                return event;
        drain_output();
        poll(&fd, 1, 20);
    } while (central_monotonic_ns() < deadline);
    require(0, "Local receives the next command before deadline");
    memset(&event, 0, sizeof(event));
    return event;
}

static void set_fixture_delay(const char *setting) {
    test_message_t frame;
    reply_t reply;
    int coid = name_open(TEST_LOCAL, 0);
    require(coid >= 0, "normal fixture control connection");
    central_message_init(&frame, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    snprintf(frame.data, sizeof(frame.data), "fixture:delay=%s", setting);
    require(send_frame(coid, &frame, &reply) == 0 && reply.status == 0,
            "set bounded fixture command-reply delay");
    name_close(coid);
}

static void flood_commands(unsigned count) {
    char batch[4096];
    size_t used = 0;
    unsigned i;
    for (i = 0; i < count; ++i)
        used += (size_t)snprintf(batch + used, sizeof(batch) - used, "mode-sensor I1\n");
    require(used < sizeof(batch), "bounded test command batch");
    command(batch);
}

/* Exercise scheduling under sustained queued work, while the independent
 * telemetry sender keeps the Local status fresh. Observation pipes are drained
 * continuously so fixture output cannot become the tested bottleneck. */
static unsigned observe_pressure(int coid, unsigned duration_ms, uint64_t started,
                                 int check_expiry) {
    uint64_t until = started + duration_ms * UINT64_C(1000000);
    uint64_t next_status = started, last_heartbeat[2] = {started, started};
    uint64_t max_gap[2] = {0, 0}, last_command = 0;
    unsigned heartbeat_count[2] = {0, 0}, command_count = 0, peer;
    do {
        struct pollfd fds[3] = {
            {observations[0], POLLIN, 0}, {observations[1], POLLIN, 0}, {output_fd, POLLIN, 0}
        };
        uint64_t now = central_monotonic_ns();
        if (now >= next_status) {
            send_status(coid, I1);
            next_status = now + UINT64_C(800000000);
        }
        for (peer = 0; peer < 2; ++peer) {
            observation_t event;
            while (read(observations[peer], &event, sizeof(event)) == sizeof(event)) {
                if (event.type == MSG_HEARTBEAT && event.received_at >= started) {
                    uint64_t gap = event.received_at - last_heartbeat[peer];
                    if (gap > max_gap[peer]) max_gap[peer] = gap;
                    last_heartbeat[peer] = event.received_at;
                    ++heartbeat_count[peer];
                } else if (peer == 0 && event.type == MSG_MODE_COMMAND) {
                    ++command_count;
                    last_command = event.received_at;
                }
            }
        }
        drain_output();
        poll(fds, 3, 20);
    } while (central_monotonic_ns() < until);
    require(command_count >= 10, "flood exercises multiple actual Local sends");
    for (peer = 0; peer < 2; ++peer) {
        require(heartbeat_count[peer] >= 3, "both peers receive heartbeats during command flood");
        require(max_gap[peer] < UINT64_C(2000000000),
                "queued commands do not postpone peer heartbeats for two seconds");
        require(central_monotonic_ns() - last_heartbeat[peer] < UINT64_C(2000000000),
                "both peers retain recent heartbeat service after flood");
    }
    if (check_expiry) {
        require(command_count < 32, "overdue queue tail is not transmitted");
        require(last_command - started < UINT64_C(5400000000),
                "no old operator request starts after its five-second queue lifetime");
        command("commands\n");
        require(wait_text("EXPIRED", 0, 2000), "history explains expired unsent requests");
        require(wait_text("queue deadline expired; not sent", 0, 2000),
                "expired outcome distinguishes unsent work from uncertain delivery");
    }
    return command_count;
}

static void normal_command_regressions(const char *binary) {
    uint64_t started;
    unsigned i, targets = 0;
    unsigned ids[NUM_INTERSECTIONS] = {0};
    int coid;
    observation_t event;

    require(kill(children[0], SIGTERM) == 0, "stop only legacy Local test fixture");
    require(waitpid(children[0], NULL, 0) == children[0], "legacy Local fixture stopped");
    children[0] = -1;
    close(observations[0]); observations[0] = -1;
    start_peer(0, TEST_LOCAL, 1);
    start_central(binary, NULL); /* exercise the new default freshness policy */
    require(wait_text("local heartbeat restored", 1, 5000), "normal Local connected");
    require(wait_text("train heartbeat restored", 1, 5000), "normal Train connected");
    coid = name_open(TEST_CENTRAL, 0);
    require(coid >= 0, "normal Central service ready");

    quiet_output_start();
    command("hel");
    pump_output(1250);
    require(output_used == 0, "default view leaves partial input undisturbed across heartbeat ticks");
    command("p\n");
    require(wait_text("Commands:", 0, 2000), "fragmented CLI input remains intact");
    require(strstr(output, "\033[2J") == NULL, "default CLI does not clear the screen");
    quiet_output_start();
    command("watch\n");
    require(wait_text("Live view: press Enter", 0, 2000), "live status requires explicit watch");
    require(wait_text("Status age limit 5.0s", 0, 2000), "default status freshness is five seconds");
    quiet_output_start();
    require(wait_text("CENTRAL CONTROLLER", 0, 1600), "watch refreshes live status");
    command("\nhelp\n");
    require(wait_text("Commands:", 0, 2000), "Enter leaves watch and permits the next command");
    quiet_output_start();
    pump_output(1250);
    require(output_used == 0, "leaving watch restores stable prompt output");
    command("events\n");
    require(wait_text("Log records dropped:", 0, 2000), "events exposes logging status on request");

    for (i = 0; i < NUM_INTERSECTIONS; ++i) send_status(coid, i);
    clear_observations();
    started = central_monotonic_ns();
    command("mode-fixed all\n");
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        unsigned previous;
        event = next_command(1800);
        require(event.type == MSG_MODE_COMMAND && event.target < NUM_INTERSECTIONS,
                "all dispatch uses per-intersection mode payloads");
        require(!(targets & (1U << event.target)), "all dispatch addresses each target once");
        targets |= 1U << event.target;
        require(event.id != 0 && event.mode.command_id == event.id,
                "every dispatched mode carries a nonzero correlation ID");
        for (previous = 0; previous < i; ++previous)
            require(ids[previous] != event.id, "all dispatch command IDs are distinct");
        ids[i] = event.id;
        require(event.mode.new_mode == MODE_FIXED && event.mode.action == CMD_SET_MODE &&
                event.mode.priority == CMD_PRIO_OPERATOR && event.mode.duration_sec == 0,
                "fixed-mode payload survives parsing, queueing and wire transmission");
    }
    require(targets == (1U << NUM_INTERSECTIONS) - 1, "all six targets receive the request");
    require(event.received_at - started < UINT64_C(2000000000),
            "six ready commands dispatch within two seconds, without one-per-heartbeat pacing");
    require(wait_text("Command 6 I6 ACCEPTED", 1, 2000), "successful all completes with six receipts");

    send_status(coid, I1);
    send_status(coid, I3);
    command("mode-temp I1 sensor 30\nmode-revert I1\ncoordinate I3 NS 3\n");
    event = next_command(1800);
    require(event.type == MSG_MODE_COMMAND && event.target == I1 &&
            event.mode.new_mode == MODE_SENSOR && event.mode.action == CMD_TEMPORARY &&
            event.mode.duration_sec == 30 && event.mode.priority == CMD_PRIO_OPERATOR,
            "temporary command keeps requested mode, action, duration and priority on wire");
    event = next_command(1800);
    require(event.type == MSG_MODE_COMMAND && event.target == I1 &&
            event.mode.action == CMD_REVERT && event.mode.duration_sec == 0,
            "revert is transmitted as a separate action with zero duration");
    event = next_command(1800);
    require(event.type == MSG_COORDINATION_COMMAND && event.target == I3 &&
            event.coordination.mode == MODE_FIXED && event.coordination.phase == PHASE_NS_GREEN &&
            event.coordination.cycle_offset_sec == 3 && event.coordination.command_id == event.id,
            "coordination preserves phase, offset and matching command ID");
    require(wait_text("Command 9 I3 ACCEPTED", 1, 2000), "all typed command receipts recorded");
    quiet_output_start();
    command("commands\n");
    require(wait_text("mode-FIXED", 0, 2000), "history retains permanent mode description");
    require(wait_text("mode-temp SENSOR 30s", 0, 2000), "history retains temporary mode and duration");
    require(wait_text("mode-revert", 0, 2000), "history retains revert description");
    require(wait_text("coordinate NS offset=3s", 0, 2000), "history retains coordination direction and offset");
    require(wait_text("queued ", 0, 2000), "history includes elapsed queue timing");

    set_fixture_delay("80");
    send_status(coid, I1);
    clear_observations();
    quiet_output_start();
    started = central_monotonic_ns();
    flood_commands(128);
    observe_pressure(coid, 4200, started, 0);
    require(strstr(output, "Command queue is full") != NULL,
            "operator flood has an explicit bounded-queue rejection");

    set_fixture_delay("250");
    send_status(coid, I1);
    clear_observations();
    quiet_output_start();
    started = central_monotonic_ns();
    flood_commands(32);
    observe_pressure(coid, 6500, started, 1);
    set_fixture_delay("0");
    command("quit\n");
    wait_for_exit();
    name_close(coid);
}

int main(int argc, char *argv[]) {
    int coid, rc;
    double normal_exit_ms;
    test_message_t frame;
    reply_t reply;
    status_msg_t invalid = {0};
    fault_msg_t fault = {0};
    observation_t event;
    require(argc == 2, "central test binary argument");
    atexit(cleanup);
    snprintf(log_path, sizeof(log_path), "/tmp/traffic_central_only_test_%ld.log", (long)getpid());
    start_peer(0, TEST_LOCAL, 0);
    start_peer(1, TEST_TRAIN, 0);
    start_central(argv[1], "60");
    require(wait_text("local heartbeat restored", 1, 5000), "central connects to unchanged local ABI");
    require(wait_text("train heartbeat restored", 1, 5000), "central connects to unchanged train ABI");
    coid = name_open(TEST_CENTRAL, 0);
    require(coid >= 0, "legacy sender name_open to central");
    send_status(coid, I1);
    send_status(coid, I6);
    send_railway(coid, P1);
    send_railway(coid, P3);
    command("status\n");
    require(wait_text("I1  FIXED", 0, 2000), "I1 status rendered");
    require(wait_text("Status age limit 60.0s", 0, 2000), "explicit status-age option overrides default");
    require(wait_text("I6  FIXED", 0, 2000), "I6 status rendered separately");
    require(wait_text("P1  Train NONE", 0, 2000), "P1 clear status rendered");
    require(wait_text("P3  Train AT CROSSING", 0, 2000), "P3 train status rendered separately");
    command("mode-fixed all\n");
    require(wait_text("I2 requires a fresh Local status", 1, 2000), "broadcast requires each intersection status");
    require(!log_contains("queued for"), "incomplete broadcast queues no commands");

    central_message_init(&frame, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    invalid.intersection_id = NUM_INTERSECTIONS;
    memcpy(frame.data, &invalid, sizeof(invalid));
    rc = send_frame(coid, &frame, &reply);
    require(rc < 0 || reply.status == -1, "out-of-range Local payload rejected");
    central_message_init(&frame, MSG_FAULT_ALERT, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
    fault.source_id = P3; fault.fault_type = FAULT_GATE; fault.severity = SEV_CRITICAL;
    strcpy(fault.description, "gate sensor");
    memcpy(frame.data, &fault, sizeof(fault));
    require(send_frame(coid, &frame, &reply) == 0 && reply.status == 0, "Train fault accepted");
    command("faults\n");
    require(wait_text("P3 type 4 severity 3", 0, 2000), "crossing fault assigned to P3");

    command("mode-sensor I1\n");
    require(observation_seen(0, MSG_MODE_COMMAND, I1, 4000), "command targets I1 on legacy wire");
    require(wait_text("Command 1 I1 ACCEPTED", 1, 4000), "matching payload ACK recorded");
    command("commands\nstatus\n");
    require(wait_text("ACCEPTED confirms receipt", 0, 2000), "receipt semantics visible to operator");
    output_used = 0; output[0] = '\0';
    command("status\n");
    require(wait_text("I1  FIXED", 0, 2000), "ACK does not fabricate applied mode");
    send_status(coid, I2);
    command("mode-fixed I2\n");
    require(observation_seen(0, MSG_MODE_COMMAND, I2, 4000), "command targets I2 on legacy wire");
    require(wait_text("Command 2 I2 REJECTED", 1, 4000), "original handler NACK recorded");

    command("mode-sensor I6\n");
    require(observation_seen(0, MSG_MODE_COMMAND, I6, 4000), "fixture receives command then SIGSTOP");
    require(wait_text("Command 3 I6 UNCONFIRMED", 1, 3000), "stopped peer command outcome is unconfirmed");
    while (read(observations[1], &event, sizeof(event)) == sizeof(event)) {}
    require(observation_seen(1, MSG_HEARTBEAT, 0, 2000), "Train health checks continue during Local hang");
    command("quit\n");
    release_stopped_peer();
    name_close(coid);

    start_central(argv[1], "60");
    require(wait_text("local heartbeat restored", 1, 5000), "Local reconnects after restart");
    require(wait_text("train heartbeat restored", 1, 5000), "Train reconnects after restart");
    coid = name_open(TEST_CENTRAL, 0);
    require(coid >= 0, "restarted central service ready");
    send_status(coid, I6);
    while (read(observations[0], &event, sizeof(event)) == sizeof(event)) {}
    command("mode-sensor I6\n");
    require(observation_seen(0, MSG_MODE_COMMAND, I6, 4000), "fixture stops during second command");
    require(wait_text("Command 1 I6 UNCONFIRMED", 1, 3000), "second stopped request times out");
    require(kill(children[2], SIGTERM) == 0, "send SIGTERM only to test Central");
    release_stopped_peer();
    name_close(coid);

    start_central(argv[1], "60");
    require(wait_text("local heartbeat restored", 1, 5000), "responsive Local connection for normal quit");
    require(wait_text("train heartbeat restored", 1, 5000), "responsive Train connection for normal quit");
    command("quit\n");
    normal_exit_ms = wait_for_exit();
    require(normal_exit_ms < 1500.0, "normal quit completes within 1.5 seconds");
    require(!strstr(output, "Waiting for an outstanding"), "normal shutdown has no pending IPC warning");
    normal_command_regressions(argv[1]);
    printf("integration: %u checks passed; normal quit %.1f ms\n", checks, normal_exit_ms);
    printf("Legacy limitation verified: a stopped peer retains the exiting process until its reply is released.\n");
    return 0;
}
