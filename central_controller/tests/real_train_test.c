#include "../src/ipc.h"
#include "../src/ui_ipc.h"
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#define TEST_UI "traffic_test_central_only_ui"

static pid_t children[2] = {-1, -1};
static int output_fd[2] = {-1, -1}, train_input = -1;
static char output[2][32768], response[CENTRAL_UI_RESPONSE_SIZE], log_path[128];
static size_t output_used[2];
static unsigned checks;
static int log_owned;

static void cleanup(void) {
    unsigned i;
    /* Signal both owned processes before waiting: either might otherwise be
     * waiting on a reply from the other during an unsuccessful integration. */
    for (i = 0; i < 2; ++i) if (children[i] > 0) kill(children[i], SIGKILL);
    for (i = 0; i < 2; ++i) {
        if (children[i] > 0) waitpid(children[i], NULL, 0);
        if (output_fd[i] >= 0) close(output_fd[i]);
    }
    if (train_input >= 0) close(train_input);
    if (log_owned) unlink(log_path);
}

static void require(int condition, const char *description) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s (errno=%d)\nLast Central UI:\n%s\nCentral output:\n%s\nTrain output:\n%s\n",
                description, errno, response, output[0], output[1]);
        exit(EXIT_FAILURE);
    }
}

static void collect(void) {
    unsigned i;
    for (i = 0; i < 2; ++i) {
        char block[4096];
        ssize_t size;
        if (output_fd[i] < 0) continue;
        while ((size = read(output_fd[i], block, sizeof(block))) > 0) {
            size_t keep = (size_t)size;
            if (output_used[i] + keep >= sizeof(output[i])) {
                size_t discard = output_used[i] + keep - sizeof(output[i]) + 1;
                memmove(output[i], output[i] + discard, output_used[i] - discard);
                output_used[i] -= discard;
            }
            memcpy(output[i] + output_used[i], block, keep);
            output_used[i] += keep;
            output[i][output_used[i]] = '\0';
        }
    }
}

static void pump(unsigned milliseconds) {
    uint64_t until = central_monotonic_ns() + milliseconds * UINT64_C(1000000);
    do {
        struct pollfd fds[2] = {{output_fd[0], POLLIN, 0}, {output_fd[1], POLLIN, 0}};
        collect();
        poll(fds, 2, 20);
    } while (central_monotonic_ns() < until);
    collect();
}

static void start_process(unsigned index, const char *binary) {
    int out[2], input[2] = {-1, -1};
    require(pipe(out) == 0, "isolated controller output pipe");
    if (index == 1) require(pipe(input) == 0, "Train stdin remains open without EOF spinning");
    children[index] = fork();
    require(children[index] >= 0, "controller fixture fork");
    if (!children[index]) {
        int stdin_fd = index == 1 ? input[0] : open("/dev/null", O_RDONLY);
        if (stdin_fd < 0 || dup2(stdin_fd, STDIN_FILENO) < 0 ||
            dup2(out[1], STDOUT_FILENO) < 0 || dup2(out[1], STDERR_FILENO) < 0)
            _exit(2);
        close(stdin_fd);
        close(out[0]); close(out[1]);
        if (index == 1) {
            close(input[1]);
            execl(binary, binary, "-l", (char *)NULL);
        } else {
            execl(binary, binary, "-l", "--headless", "-o", log_path, (char *)NULL);
        }
        _exit(3);
    }
    close(out[1]);
    output_fd[index] = out[0];
    require(fcntl(output_fd[index], F_SETFL, O_NONBLOCK) == 0, "controller outputs are drained without blocking");
    if (index == 1) {
        close(input[0]);
        train_input = input[1];
    }
}

static int request(const char *text) {
    int status = -1;
    response[0] = '\0';
    int result = central_ui_client_request(TEST_UI, text, response, sizeof(response), &status);
    collect();
    return result == 0 && status == 0;
}

static int line_contains(const char *text, const char *row, const char *fragment) {
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n'), *found;
        if (!end) end = line + strlen(line);
        if (!strncmp(line, row, strlen(row)) && (found = strstr(line, fragment)) != NULL && found < end)
            return 1;
        line = *end ? end + 1 : end;
    }
    return 0;
}

static int wait_snapshot(const char *row, const char *fragment, unsigned timeout_ms) {
    uint64_t until = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    do {
        if (request("status") && line_contains(response, row, fragment)) return 1;
        pump(30);
    } while (central_monotonic_ns() < until);
    return 0;
}

static void command_receipt(const char *command, const char *description) {
    uint64_t until;
    require(request(command), "Central queues the real Train simulation command");
    until = central_monotonic_ns() + UINT64_C(2000000000);
    do {
        if (request("commands") && strstr(response, description) &&
            strstr(response, "Train receipt only; BUSY/application not reported")) {
            /* Every command gets its own record. Wait until the matching
             * description's record has ACCEPTED, not only some earlier ACK. */
            const char *match = strstr(response, description);
            const char *begin = match;
            while (begin > response && begin[-1] != '\n') --begin;
            const char *end = strchr(match, '\n');
            const char *accepted = strstr(begin, "ACCEPTED");
            if (accepted && (!end || accepted < end)) return;
        }
        pump(20);
    } while (central_monotonic_ns() < until);
    require(0, "real Train command has a matching receipt-only history record");
}

static void wait_owned_exit(unsigned index, unsigned timeout_ms) {
    uint64_t until = central_monotonic_ns() + timeout_ms * UINT64_C(1000000);
    int result = 0, status = 0;
    do {
        result = waitpid(children[index], &status, WNOHANG);
        if (result == children[index]) break;
        pump(20);
    } while (central_monotonic_ns() < until);
    require(result == children[index], "owned controller fixture exits before cleanup deadline");
    if (index == 0) require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Central exits successfully after real Train integration");
    children[index] = -1;
}

int main(int argc, char *argv[]) {
    int fd;
    uint64_t started, until;
    int saw_closed = 0, saw_fault = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    require(argc == 3, "Central fixture and real Train fixture binary arguments");
    atexit(cleanup);
    snprintf(log_path, sizeof(log_path), "/tmp/traffic_real_train_%ld.log", (long)getpid());
    fd = open(log_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    require(fd >= 0, "reserve owned Central event log");
    log_owned = 1;
    require(close(fd) == 0, "close reserved event log");
    start_process(0, argv[1]);
    require(wait_snapshot("Build ", "(", 5000), "Central headless UI is ready");
    start_process(1, argv[2]);
    require(wait_snapshot("P3 ", "CURRENT", 7000), "unchanged real Train reports its complete compact crossing batch");
    require(line_contains(response, "P1 ", "CURRENT") && line_contains(response, "P2 ", "CURRENT"),
            "unchanged real Train reports P1, P2 and P3 on their own rows");
    require(line_contains(response, "P1 ", "Gate OPEN") && line_contains(response, "P2 ", "Gate OPEN") &&
            line_contains(response, "P3 ", "Gate OPEN"), "real Train initial gates are all reported OPEN");

    /* Five simulated seconds per real second leaves closed phases long enough
     * to observe through Train's one-second telemetry period. This is a real
     * existing simulator command, not a change to the teammate's timings. */
    command_receipt("train-cmd scale 5", "train-sim scale 5");
    command_receipt("train-cmd stuck P1", "train-sim stuck P1");
    command_receipt("train-cmd train-up", "train-sim train-up");
    started = central_monotonic_ns();
    until = started + UINT64_C(11000000000);
    do {
        if (request("status")) {
            if (line_contains(response, "P3 ", "Gate CLOSED") || line_contains(response, "P2 ", "Gate CLOSED"))
                saw_closed = 1;
            if (line_contains(response, "P1 ", "Gate FAULT") && line_contains(response, "P1 ", "Fault GATE"))
                saw_fault = 1;
        }
        pump(30);
    } while (central_monotonic_ns() < until);
    require(saw_closed, "normal crossing gate CLOSED is displayed from real Train telemetry");
    require(saw_fault, "stuck P1 causes a reported gate fault during the real train-up sequence");
    require(request("faults") && line_contains(response, "P1 ", "P1:") && strstr(response, "CRITICAL"),
            "real Train compact fault retains P1 description and critical severity");
    require(request("status") && line_contains(response, "P2 ", "Gate OPEN") && line_contains(response, "P3 ", "Gate OPEN"),
            "unaffected crossings report OPEN again after their train movement");

    /* The full sequence has ended before reset. An ACK while Train is BUSY
     * would not prove it accepted reset, because its current reply omits BUSY. */
    command_receipt("train-cmd reset P1", "train-sim reset P1");
    require(wait_snapshot("P1 ", "Gate OPEN", 4000) && line_contains(response, "P1 ", "Fault NONE"),
            "real Train reset eventually reports P1 OPEN with no current fault");
    require(request("faults") && line_contains(response, "P1 ", "P1:"),
            "historical fault stays latched because real Train sends no explicit fault-clear alert");
    require(request("commands") && strstr(response, "Train receipt only; BUSY/application not reported"),
            "real integration still presents Train command ACK as receipt only");

    require(kill(children[1], SIGTERM) == 0, "stop only owned real Train test process");
    wait_owned_exit(1, 2000);
    require(request("shutdown"), "explicitly shut down the owned Central fixture");
    wait_owned_exit(0, 2500);
    printf("real_train: %u checks passed; unchanged Train compact telemetry, sensor simulation, gate fault/reset and receipt-only command history\n", checks);
    return 0;
}
