#include "../src/ipc.h"
#include "../src/commands.h"
#include "../src/ui_ipc.h"
#include <poll.h>
#include <signal.h>
#include <sys/iomsg.h>
#include <sys/wait.h>

#define TEST_CENTRAL "traffic_test_central_only"
#define TEST_LOCAL "traffic_test_central_only_local"
#define TEST_TRAIN "traffic_test_central_only_train"
#define TEST_UI "traffic_test_central_only_ui"
#define OBSERVATION_LIMIT 128

typedef struct {
    unsigned type, target;
    uint64_t received_at;
    test_message_t message;
} observation_t;

static pid_t children[5] = {-1, -1, -1, -1, -1};
static int notice[3] = {-1, -1, -1};
static int output_fd = -1;
static observation_t observations[3][OBSERVATION_LIMIT];
static unsigned observation_count[3], checks;
static char output[32768], response[CENTRAL_UI_RESPONSE_SIZE], log_path[128], schedule_path[128];
static size_t output_used;
static int log_owned, schedule_owned;
static int response_status;

static void cleanup(void) {
    unsigned i;
    for (i = 0; i < 5; ++i) {
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            waitpid(children[i], NULL, 0);
            children[i] = -1;
        }
    }
    for (i = 0; i < 3; ++i) if (notice[i] >= 0) close(notice[i]);
    if (output_fd >= 0) close(output_fd);
    if (log_owned) unlink(log_path);
    if (schedule_owned) unlink(schedule_path);
}

static void require(int condition, const char *description) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s (errno=%d)\nLast UI response:\n%s\nCore output:\n%s\n",
                description, errno, response, output);
        exit(EXIT_FAILURE);
    }
}

static void collect(void) {
    ssize_t size;
    unsigned peer;
    if (output_fd >= 0) {
        while (output_used < sizeof(output) - 1 &&
               (size = read(output_fd, output + output_used, sizeof(output) - 1 - output_used)) > 0)
            output_used += size;
        output[output_used] = '\0';
    }
    for (peer = 0; peer < 3; ++peer) {
        observation_t value;
        if (notice[peer] < 0) continue;
        while (read(notice[peer], &value, sizeof(value)) == sizeof(value)) {
            require(observation_count[peer] < OBSERVATION_LIMIT, "bounded fixture observation storage");
            observations[peer][observation_count[peer]++] = value;
        }
    }
}

static void pump(unsigned milliseconds) {
    uint64_t until = central_monotonic_ns() + milliseconds * UINT64_C(1000000);
    do {
        struct pollfd fds[4] = {
            {notice[0], POLLIN, 0}, {notice[1], POLLIN, 0},
            {notice[2], POLLIN, 0}, {output_fd, POLLIN, 0}
        };
        collect();
        poll(fds, 4, 10);
    } while (central_monotonic_ns() < until);
    collect();
}

static void peer_fixture(const char *name, int fd, unsigned peer) {
    name_attach_t *attach = name_attach(NULL, name, 0);
    observation_t value = {0};
    if (!attach || write(fd, &value, sizeof(value)) != sizeof(value)) _exit(2);
    for (;;) {
        union { struct _pulse pulse; test_message_t message; } frame;
        reply_t reply = {0};
        int rcvid = MsgReceive(attach->chid, &frame, sizeof(frame), NULL);
        if (rcvid < 0) _exit(3);
        if (rcvid == 0) {
            if (frame.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(frame.pulse.scoid);
            continue;
        }
        if (frame.pulse.type == _IO_CONNECT) { MsgReply(rcvid, 0, NULL, 0); continue; }
        if (frame.pulse.type >= _IO_BASE) { MsgError(rcvid, ENOSYS); continue; }
        memset(&value, 0, sizeof(value));
        value.type = frame.message.header.type;
        value.target = central_command_target(&frame.message);
        value.received_at = central_monotonic_ns();
        value.message = frame.message;
        if (write(fd, &value, sizeof(value)) != sizeof(value)) _exit(4);
        central_timestamp(reply.timestamp, sizeof(reply.timestamp));
        reply.command_id = central_command_id(&frame.message);
        if (peer == 2 && value.type == MSG_MODE_COMMAND) {
            reply.status = -1;
            reply.command_id = 0; /* unchanged Local's legacy NACK convention */
        }
        if (peer == 1 && value.type == MSG_TEST && !strcmp(frame.message.data, "train-down"))
            raise(SIGSTOP);
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }
}

static void start_peer(unsigned peer, const char *name) {
    int pipe_fd[2];
    observation_t ready;
    struct pollfd fd;
    require(pipe(pipe_fd) == 0, "peer observation pipe");
    children[peer] = fork();
    require(children[peer] >= 0, "peer fixture fork");
    if (!children[peer]) {
        close(pipe_fd[0]);
        peer_fixture(name, pipe_fd[1], peer);
        _exit(5);
    }
    close(pipe_fd[1]);
    notice[peer] = pipe_fd[0];
    fd.fd = notice[peer]; fd.events = POLLIN; fd.revents = 0;
    require(poll(&fd, 1, 2000) > 0 &&
            read(notice[peer], &ready, sizeof(ready)) == sizeof(ready) && ready.type == 0,
            "peer fixture ready without replacing an existing service");
    require(fcntl(notice[peer], F_SETFL, O_NONBLOCK) == 0, "peer observations nonblocking");
}

static void start_core(const char *binary, const char *endpoint) {
    int pipe_fd[2];
    require(pipe(pipe_fd) == 0, "core output pipe");
    children[3] = fork();
    require(children[3] >= 0, "headless core fork");
    if (!children[3]) {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
            dup2(pipe_fd[1], STDOUT_FILENO) < 0 || dup2(pipe_fd[1], STDERR_FILENO) < 0)
            _exit(6);
        close(null_fd);
        close(pipe_fd[0]); close(pipe_fd[1]);
        execl(binary, binary, "-l", "--headless", "-s", "60", "-o", log_path,
              "--local-endpoint", endpoint, "--schedule", schedule_path, (char *)NULL);
        _exit(7);
    }
    close(pipe_fd[1]);
    output_fd = pipe_fd[0];
    require(fcntl(output_fd, F_SETFL, O_NONBLOCK) == 0, "core output nonblocking");
}

static int request(const char *text) {
    response_status = -1;
    response[0] = '\0';
    int result = central_ui_client_request(TEST_UI, text, response, sizeof(response), &response_status);
    collect();
    return result == 0 && response_status == 0;
}

static void command(const char *text) {
    require(request(text), "operator command returns a UI response");
}

static int line_contains(const char *text, const char *row, const char *fragment) {
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *match;
        if (!end) end = line + strlen(line);
        if (!strncmp(line, row, strlen(row)) &&
            (match = strstr(line, fragment)) != NULL && match < end)
            return 1;
        line = *end ? end + 1 : end;
    }
    return 0;
}

static int wait_response(const char *command_text, const char *fragment, unsigned timeout_ms) {
    uint64_t until = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    do {
        if (request(command_text) && strstr(response, fragment)) return 1;
        pump(20);
    } while (central_monotonic_ns() < until);
    return 0;
}

static observation_t *find_event(unsigned peer, unsigned type, unsigned target,
                                  const char *payload, uint64_t after) {
    unsigned i;
    collect();
    for (i = 0; i < observation_count[peer]; ++i) {
        observation_t *event = &observations[peer][i];
        if (event->type != type || event->received_at < after) continue;
        if ((type == MSG_MODE_COMMAND || type == MSG_COORDINATION_COMMAND) &&
            event->target != target) continue;
        if (payload && strcmp(event->message.data, payload)) continue;
        return event;
    }
    return NULL;
}

static observation_t *wait_event(unsigned peer, unsigned type, unsigned target,
                                  const char *payload, uint64_t after, unsigned timeout_ms) {
    uint64_t until = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    observation_t *event;
    do {
        event = find_event(peer, type, target, payload, after);
        if (event) return event;
        pump(10);
    } while (central_monotonic_ns() < until);
    return NULL;
}

static void send_frame(int coid, const void *frame, size_t size) {
    uint64_t timeout = UINT64_C(1000000000);
    reply_t reply;
    memset(&reply, 0x5a, sizeof(reply));
    require(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                         NULL, &timeout, NULL) == 0, "telemetry send timeout armed");
    require(MsgSend(coid, frame, size, &reply, sizeof(reply)) == 0 && reply.status == 0,
            "Central accepts peer telemetry with the actual payload length");
}

static void send_local_status(int coid, unsigned id) {
    test_message_t frame;
    status_msg_t value = {0};
    central_message_init(&frame, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    value.intersection_id = id;
    value.mode = MODE_FIXED;
    value.phase = PHASE_NS_GREEN;
    value.ns_state = LIGHT_GREEN;
    value.ew_state = LIGHT_RED;
    value.time_remaining = 20;
    memcpy(frame.data, &value, sizeof(value));
    send_frame(coid, &frame, sizeof(frame));
}

static void send_compact_railway(int coid, unsigned id, unsigned train, unsigned gate) {
    test_message_t envelope;
    railway_status_full_msg_t frame;
    central_message_init(&envelope, MSG_RAILWAY_STATUS, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
    memset(&frame, 0, sizeof(frame));
    frame.header = envelope.header;
    frame.payload.crossing_id = id; /* current Train cx->id is one-based */
    frame.payload.train_state = train;
    frame.payload.gate_state = gate;
    send_frame(coid, &frame, sizeof(frame));
}

static void check_ui_lifecycle(void) {
    int status = 0, result = 0;
    uint64_t until;
    children[4] = fork();
    require(children[4] >= 0, "independent display client fork");
    if (!children[4]) {
        char snapshot[CENTRAL_UI_RESPONSE_SIZE];
        int reply_status = -1;
        int rc = central_ui_client_request(TEST_UI, "status", snapshot, sizeof(snapshot), &reply_status);
        _exit(rc == 0 && reply_status == 0 && strstr(snapshot, "CENTRAL CONTROLLER") ? 0 : 8);
    }
    until = central_monotonic_ns() + UINT64_C(2000000000);
    do {
        result = waitpid(children[4], &status, WNOHANG);
        if (result == children[4]) break;
        pump(20);
    } while (central_monotonic_ns() < until);
    require(result == children[4] && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "independent UI reads a snapshot and disconnects cleanly");
    children[4] = -1;
    require(waitpid(children[3], &status, WNOHANG) == 0,
            "headless Central survives stdin EOF and display client exit");
    require(request("status") && strstr(response, "CENTRAL CONTROLLER"),
            "new display connection receives a snapshot from the same running core");
    require(request("  watch\t ") && strstr(response, "CENTRAL CONTROLLER"),
            "whitespace around UI watch still requests a snapshot");
    require(request("  quit\t ") && strstr(response, "shutdown"),
            "whitespace around UI quit retains display-only semantics");
    require(request("status") && strstr(response, "CENTRAL CONTROLLER"),
            "trimmed UI quit does not shut down the core");
}

static void stop_core(void) {
    uint64_t until = central_monotonic_ns() + UINT64_C(2500000000);
    int result = 0, status = 0;
    command("shutdown");
    do {
        result = waitpid(children[3], &status, WNOHANG);
        if (result == children[3]) break;
        pump(20);
    } while (central_monotonic_ns() < until);
    require(result == children[3] && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "explicit UI shutdown cleanly stops responsive headless core");
    children[3] = -1;
}

static void check_schedule(int coid) {
    observation_t *event;
    mode_cmd_msg_t mode;
    uint64_t started = central_monotonic_ns();
    require(find_event(0, MSG_MODE_COMMAND, I3, NULL, 0) == NULL,
            "schedule does not dispatch to an intersection before its first status");
    send_local_status(coid, I3);
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "fresh status enables the configured daily schedule request");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.new_mode == MODE_SENSOR && mode.action == CMD_SET_MODE &&
            mode.priority == CMD_PRIO_SCHEDULE,
            "daily schedule uses lower-priority mode request on the existing Local wire");
    started = central_monotonic_ns();
    command("mode-fixed I3");
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "operator override reaches scheduled intersection");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.new_mode == MODE_FIXED && mode.priority == CMD_PRIO_OPERATOR,
            "operator request has the protocol's operator priority");
    uint64_t after_operator = event->received_at + 1;
    pump(1100);
    require(find_event(0, MSG_MODE_COMMAND, I3, NULL, after_operator) == NULL,
            "daily sensor schedule does not overwrite a persistent fixed operator override");
    started = central_monotonic_ns();
    command("mode-temp I3 sensor 1");
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "temporary override reaches the scheduled intersection");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.action == CMD_TEMPORARY && mode.new_mode == MODE_SENSOR &&
            mode.duration_sec == 1 && mode.priority == CMD_PRIO_OPERATOR,
            "temporary mode keeps its duration and operator priority on the wire");
    after_operator = event->received_at + 1;
    pump(1250);
    require(find_event(0, MSG_MODE_COMMAND, I3, NULL, after_operator) == NULL,
            "temporary expiry retains underlying persistent operator hold");
    started = central_monotonic_ns();
    command("mode-revert I3");
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "explicit temporary revert reaches Local");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.action == CMD_REVERT && mode.duration_sec == 0,
            "temporary revert is a distinct zero-duration action");
    after_operator = event->received_at + 1;
    pump(250);
    require(find_event(0, MSG_MODE_COMMAND, I3, NULL, after_operator) == NULL,
            "temporary revert does not clear the persistent operator hold");
    started = central_monotonic_ns();
    command("schedule-resume I3");
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "explicit schedule resume reissues current scheduled mode");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.new_mode == MODE_SENSOR && mode.priority == CMD_PRIO_SCHEDULE,
            "resumed automation returns to configured sensor policy at schedule priority");
    started = central_monotonic_ns();
    command("mode-temp I3 fixed 1");
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, started, 2000);
    require(event != NULL, "temporary override also works after persistent hold is released");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.action == CMD_TEMPORARY && mode.new_mode == MODE_FIXED && mode.duration_sec == 1,
            "temporary fixed request is transmitted intact");
    after_operator = event->received_at;
    event = wait_event(0, MSG_MODE_COMMAND, I3, NULL, after_operator + 1, 2500);
    require(event != NULL && event->received_at - after_operator >= UINT64_C(900000000),
            "automation resumes only after the temporary receipt-relative duration");
    memcpy(&mode, event->message.data, sizeof(mode));
    require(mode.new_mode == MODE_SENSOR && mode.priority == CMD_PRIO_SCHEDULE,
            "temporary expiry without a persistent hold resumes the daily sensor policy");
    command("status");
    require(line_contains(response, "I3 ", "FIXED"),
            "schedule receipts also preserve the Local's last reported mode");
}

int main(int argc, char *argv[]) {
    char alternate_name[64], endpoint[80];
    test_message_t envelope;
    fault_full_msg_t fault;
    observation_t *event;
    uint64_t started, released;
    int coid, stop_status = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    require(argc == 2, "central fixture binary argument");
    atexit(cleanup);
    snprintf(log_path, sizeof(log_path), "/tmp/traffic_central_features_%ld.log", (long)getpid());
    snprintf(schedule_path, sizeof(schedule_path), "/tmp/traffic_central_features_%ld.schedule", (long)getpid());
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(log_fd >= 0, "reserve owned core log fixture");
    log_owned = 1;
    require(close(log_fd) == 0, "close reserved core log fixture");
    int schedule_fd = open(schedule_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    const char schedule_text[] = "00:00 sensor I3\n";
    require(schedule_fd >= 0, "reserve owned daily schedule fixture");
    schedule_owned = 1;
    require(write(schedule_fd, schedule_text, sizeof(schedule_text) - 1) ==
            sizeof(schedule_text) - 1 && close(schedule_fd) == 0, "write owned daily schedule fixture");
    snprintf(alternate_name, sizeof(alternate_name), "traffic_test_i2_%ld", (long)getpid());
    snprintf(endpoint, sizeof(endpoint), "I2=%s", alternate_name);
    start_peer(0, TEST_LOCAL);
    start_peer(1, TEST_TRAIN);
    start_peer(2, alternate_name);
    start_core(argv[1], endpoint);
    require(wait_response("status", "CENTRAL CONTROLLER", 5000), "headless core exposes private display IPC");
    require(wait_event(0, MSG_HEARTBEAT, 0, NULL, 0, 3000) != NULL &&
            wait_event(1, MSG_HEARTBEAT, 0, NULL, 0, 3000) != NULL &&
            wait_event(2, MSG_HEARTBEAT, 0, NULL, 0, 3000) != NULL,
            "Central independently probes default Local, Train and configured I2 endpoint");
    coid = name_open(TEST_CENTRAL, 0);
    require(coid >= 0, "peer telemetry connects to Central receiver");
    send_local_status(coid, I1);
    send_local_status(coid, I2);
    send_compact_railway(coid, 1, TRAIN_NONE, GATE_OPEN);
    send_compact_railway(coid, 2, TRAIN_APPROACHING, GATE_CLOSING);
    send_compact_railway(coid, 3, TRAIN_AT_CROSSING, GATE_CLOSED);
    command("status");
    require(line_contains(response, "P1 ", "Train NONE") && line_contains(response, "P1 ", "Gate OPEN"),
            "compact Train P1 appears as clear/open on P1 row");
    require(line_contains(response, "P2 ", "Train APPROACHING") && line_contains(response, "P2 ", "Gate CLOSING"),
            "compact Train P2 appears as approaching/closing on P2 row");
    require(line_contains(response, "P3 ", "Train AT CROSSING") && line_contains(response, "P3 ", "Gate CLOSED"),
            "compact Train P3 appears as occupied/closed on P3 row");
    require(strstr(response, "NOT REPORTED") != NULL,
            "display identifies unavailable Train fields instead of inventing signal/flash states");

    central_message_init(&envelope, MSG_FAULT_ALERT, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
    memset(&fault, 0, sizeof(fault));
    fault.header = envelope.header;
    fault.payload.source_id = 3;
    fault.payload.fault_type = FAULT_GATE;
    fault.payload.severity = SEV_CRITICAL;
    strcpy(fault.payload.description, "P3 gate jammed");
    send_frame(coid, &fault, sizeof(fault));
    command("faults");
    require(line_contains(response, "P3 ", "P3 gate jammed") &&
            strstr(response, "GATE") != NULL && strstr(response, "CRITICAL") != NULL,
            "compact fault attaches P3 detail and descriptive fault/severity names");
    check_ui_lifecycle();

    started = central_monotonic_ns();
    command("train-cmd train-up");
    require(wait_event(1, MSG_TEST, 0, "train-up", started, 2000) != NULL,
            "allowlisted train command arrives unchanged at Train MSG_TEST handler");
    require(wait_response("commands", "Train receipt only; BUSY/application not reported", 2000),
            "Train ACK is described as receipt without claiming BUSY or application outcome");
    command("status");
    require(line_contains(response, "P1 ", "Train NONE") && line_contains(response, "I1 ", "FIXED"),
            "command ACK does not replace reported Train or Local state");
    started = central_monotonic_ns();
    require(!request("train-cmd boom-up") && response_status < 0,
            "invalid Train command returns a nonzero UI rejection status");
    pump(150);
    require(find_event(1, MSG_TEST, 0, "boom-up", started) == NULL,
            "unsupported raw Train command is not transmitted");
    require(!request("train-cmd p1-fault") && response_status < 0,
            "known self-deadlocking Train fault command is rejected by Central");
    pump(100);
    require(find_event(1, MSG_TEST, 0, "p1-fault", started) == NULL,
            "blocked fault injection never reaches the teammate's Train handler");
    require(!request("mode-fixed I4") && response_status < 0,
            "Local command lacking fresh target status returns a UI rejection");
    require(find_event(0, MSG_MODE_COMMAND, I4, NULL, started) == NULL,
            "rejected stale target command never reaches Local");

    started = central_monotonic_ns();
    command("mode-sensor I2");
    require(wait_event(2, MSG_MODE_COMMAND, I2, NULL, started, 2000) != NULL,
            "I2 command is routed to its configured Local endpoint");
    require(find_event(0, MSG_MODE_COMMAND, I2, NULL, started) == NULL,
            "I2 command is not duplicated to the shared Local endpoint");
    require(wait_response("commands", "REJECTED", 2000), "independent Local endpoint NACK is retained");
    command("mode-sensor I1");
    require(wait_event(0, MSG_MODE_COMMAND, I1, NULL, started, 2000) != NULL,
            "default Local still receives I1 after separate I2 endpoint rejects its command");

    started = central_monotonic_ns();
    command("train-cmd train-down");
    require(wait_event(1, MSG_TEST, 0, "train-down", started, 2000) != NULL,
            "Train receives command before fixture deliberately stalls");
    released = central_monotonic_ns();
    do {
        if (waitpid(children[1], &stop_status, WNOHANG | WUNTRACED) == children[1]) break;
        pump(10);
    } while (central_monotonic_ns() - released < UINT64_C(1000000000));
    require(WIFSTOPPED(stop_status), "only owned Train fixture is stopped in its command handler");
    released = central_monotonic_ns();
    command("mode-fixed I1");
    event = wait_event(0, MSG_MODE_COMMAND, I1, NULL, released, 1500);
    require(event != NULL && event->received_at - released < UINT64_C(1500000000),
            "Train reply stall does not block independent Local command delivery");
    require(wait_response("commands", "UNCONFIRMED", 2000),
            "stalled Train command is uncertain rather than silently accepted or retried");
    require(kill(children[1], SIGCONT) == 0, "resume only owned stopped Train fixture");
    pump(250);

    send_local_status(coid, I1);
    started = central_monotonic_ns();
    command("coordinate-at 2 I1 NS 3");
    command("mode-sensor I1");
    event = wait_event(0, MSG_MODE_COMMAND, I1, NULL, started, 1500);
    require(event != NULL && event->received_at - started < UINT64_C(1500000000),
            "future coordination release does not block an immediately ready command");
    event = wait_event(0, MSG_COORDINATION_COMMAND, I1, NULL, started, 3500);
    require(event != NULL && event->received_at - started >= UINT64_C(1900000000),
            "future coordination is not sent before its requested Central release time");
    coordination_command_msg_t coordination;
    memcpy(&coordination, event->message.data, sizeof(coordination));
    require(coordination.phase == PHASE_NS_GREEN && coordination.cycle_offset_sec == 3,
            "delayed dispatch preserves coordination request fields on peer wire");
    unsigned i, train_down_count = 0;
    collect();
    for (i = 0; i < observation_count[1]; ++i)
        if (observations[1][i].type == MSG_TEST &&
            !strcmp(observations[1][i].message.data, "train-down")) ++train_down_count;
    require(train_down_count == 1, "uncertain Train command is not replayed after its late reply");

    check_schedule(coid);
    check_ui_lifecycle();
    name_close(coid);
    stop_core();
    printf("central_features: %u checks passed; compact Train, separate Local routes, independent queues, headless UI, delayed dispatch and operator schedule precedence\n", checks);
    return 0;
}
