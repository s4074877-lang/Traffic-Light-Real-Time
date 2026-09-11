#include "../src/ipc.h"
#include "../src/commands.h"
#include <poll.h>
#include <signal.h>
#include <sys/iomsg.h>
#include <sys/wait.h>

#define TEST_CENTRAL "traffic_test_central_only"
#define TEST_LOCAL "traffic_test_central_only_local"
#define TEST_TRAIN "traffic_test_central_only_train"

typedef struct { unsigned type, target, id; } observation_t;
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
    char buffer[16384];
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

static void fixture_peer(const char *name, int notice_fd) {
    name_attach_t *attach = name_attach(NULL, name, 0);
    observation_t observation = {0};
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
        observation.type = frame.message.header.type;
        observation.target = central_command_target(&frame.message);
        observation.id = reply.command_id;
        if (write(notice_fd, &observation, sizeof(observation)) != sizeof(observation)) _exit(4);
        if (frame.message.header.type == MSG_MODE_COMMAND) {
            if (observation.target == I2) { reply.status = -1; reply.command_id = 0; }
            if (observation.target == I6) raise(SIGSTOP);
        }
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }
}

static void start_peer(unsigned index, const char *name) {
    int pipe_fd[2];
    observation_t ready;
    require(pipe(pipe_fd) == 0, "peer fixture pipe");
    children[index] = fork();
    require(children[index] >= 0, "peer fixture fork");
    if (!children[index]) {
        close(pipe_fd[0]);
        fixture_peer(name, pipe_fd[1]);
        _exit(5);
    }
    close(pipe_fd[1]);
    observations[index] = pipe_fd[0];
    require(read(observations[index], &ready, sizeof(ready)) == sizeof(ready) && ready.type == 0,
            "peer fixture ready");
    fcntl(observations[index], F_SETFL, O_NONBLOCK);
}

static void start_central(const char *binary) {
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
    start_peer(0, TEST_LOCAL);
    start_peer(1, TEST_TRAIN);
    start_central(argv[1]);
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

    start_central(argv[1]);
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

    start_central(argv[1]);
    require(wait_text("local heartbeat restored", 1, 5000), "responsive Local connection for normal quit");
    require(wait_text("train heartbeat restored", 1, 5000), "responsive Train connection for normal quit");
    command("quit\n");
    normal_exit_ms = wait_for_exit();
    require(normal_exit_ms < 1500.0, "normal quit completes within 1.5 seconds");
    require(!strstr(output, "Waiting for an outstanding"), "normal shutdown has no pending IPC warning");
    printf("integration: %u checks passed; normal quit %.1f ms\n", checks, normal_exit_ms);
    printf("Legacy limitation verified: a stopped peer retains the exiting process until its reply is released.\n");
    return 0;
}
