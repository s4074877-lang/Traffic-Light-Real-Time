#include <signal.h>
#include <sys/wait.h>

#include "../common/communication/connection.h"
#include "../common/communication/receive.h"

typedef struct {
    pthread_mutex_t mutex;
    unsigned local_snapshots;
    unsigned train_snapshots;
    unsigned accepted;
    unsigned rejected;
    uint16_t command_id;
    uint32_t command_session;
    uint32_t local_sequence[NUM_INTERSECTIONS];
    uint32_t train_sequence[NUM_CROSSINGS];
} mock_state_t;

static int snapshot(int rcvid, any_msg_t *msg, reply_t *reply, void *arg) {
    (void)rcvid;
    mock_state_t *mock = arg;
    unsigned id = msg->payload.status_request.target_id;
    pthread_mutex_lock(&mock->mutex);
    reply->status = REPLY_APPLIED;
    reply->session_id = 1234;
    if (msg->header.dst == CONTROLLER_LOCAL) {
        status_msg_t *status = &reply->payload.status;
        reply->type = MSG_STATUS_UPDATE;
        reply->sequence = ++mock->local_sequence[id];
        status->intersection_id = (uint8_t)id;
        status->mode = id == I1 && mock->accepted ? MODE_SENSOR : MODE_FIXED;
        status->phase = PHASE_NS_GREEN;
        status->ns_state = LIGHT_GREEN;
        status->ew_state = LIGHT_RED;
        status->time_remaining = GREEN_BASE_SEC;
        if (id == I1 && mock->accepted) {
            status->command_id = mock->command_id;
            status->command_session_id = mock->command_session;
            status->command_state = COMMAND_APPLIED;
        }
        mock->local_snapshots |= 1U << id;
    } else {
        reply->type = MSG_RAILWAY_STATUS;
        reply->sequence = ++mock->train_sequence[id];
        reply->payload.railway_status.crossing_id = (uint8_t)id;
        reply->payload.railway_status.train_state = TRAIN_NONE;
        reply->payload.railway_status.gate_state = GATE_OPEN;
        mock->train_snapshots |= 1U << id;
    }
    pthread_mutex_unlock(&mock->mutex);
    return 0;
}

static int set_mode(int rcvid, any_msg_t *msg, reply_t *reply, void *arg) {
    (void)rcvid;
    mock_state_t *mock = arg;
    int delay = 0;
    pthread_mutex_lock(&mock->mutex);
    if (msg->payload.mode_cmd.intersection_id == I1 &&
        msg->payload.mode_cmd.new_mode == MODE_SENSOR) {
        mock->accepted++;
        mock->command_id = msg->payload.mode_cmd.command_id;
        mock->command_session = msg->header.session_id;
        reply->status = REPLY_ACCEPTED;
        delay = mock->accepted == 2;
    } else {
        mock->rejected++;
        reply->status = REPLY_REJECTED;
    }
    pthread_mutex_unlock(&mock->mutex);
    if (delay) {
        const struct timespec duration = { 0, 250000000 };
        nanosleep(&duration, NULL);
    }
    return 0;
}

static void *serve(void *arg) {
    receive_loop(arg);
    return NULL;
}

static int log_contains(const char *path, const char *needle) {
    char text[16384];
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = '\0';
    fclose(file);
    return strstr(text, needle) != NULL;
}

static void pause_briefly(void) {
    const struct timespec duration = { 0, 100000000 };
    nanosleep(&duration, NULL);
}

static int await_log(const char *path, const char *needle) {
    uint64_t deadline = monotonic_ns() + UINT64_C(15000000000);
    while (monotonic_ns() < deadline) {
        if (log_contains(path, needle)) return 1;
        pause_briefly();
    }
    return 0;
}

static int await_exit(pid_t child, int *status) {
    uint64_t deadline = monotonic_ns() + UINT64_C(3000000000);
    while (monotonic_ns() < deadline) {
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) return 1;
        if (result < 0) return 0;
        pause_briefly();
    }
    return 0;
}

static int await_accepted(mock_state_t *mock, unsigned count) {
    uint64_t deadline = monotonic_ns() + UINT64_C(15000000000);
    while (monotonic_ns() < deadline) {
        pthread_mutex_lock(&mock->mutex);
        int ready = mock->accepted == count;
        pthread_mutex_unlock(&mock->mutex);
        if (ready) return 1;
        pause_briefly();
    }
    return 0;
}

static int outcome_after_stop(const char *path, unsigned command_id) {
    char text[16384], expected[80];
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = '\0';
    fclose(file);
    snprintf(expected, sizeof(expected), "Command %u I1 ACCEPTED", command_id);
    const char *stopping = strstr(text, "Central stopping");
    const char *outcome = strstr(text, expected);
    return stopping && outcome && outcome > stopping;
}

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        failed = 1; \
        goto cleanup; \
    } \
} while (0)

int main(int argc, char **argv) {
    int checks = 0, failed = 0, initialized = 0, started = 0;
    int input[2] = { -1, -1 }, output_fd = -1, log_fd = -1, child_status;
    pid_t child = -1;
    char log_path[] = "/tmp/central_integration_log_XXXXXX";
    char output_path[] = "/tmp/central_integration_out_XXXXXX";
    name_attach_t *services[2] = { NULL, NULL };
    receive_context_t receivers[2];
    pthread_t servers[2];
    mock_state_t mock;
    message_handler_entry_t local_handlers[] = {
        { MSG_STATUS_REQUEST, CONTROLLER_CENTRAL, snapshot },
        { MSG_MODE_COMMAND, CONTROLLER_CENTRAL, set_mode }
    };
    message_handler_entry_t train_handlers[] = {
        { MSG_STATUS_REQUEST, CONTROLLER_CENTRAL, snapshot }
    };
    memset(&mock, 0, sizeof(mock));
    pthread_mutex_init(&mock.mutex, NULL);
    signal(SIGPIPE, SIG_IGN);

    CHECK(argc == 2);
    // This fixture must be compiled with its dedicated service namespace.
    CHECK(strcmp(LOCAL_SERVICE_NAME, "traffic_local_controller") != 0);
    CHECK(strcmp(TRAIN_SERVICE_NAME, "traffic_train_controller") != 0);
    CHECK(strcmp(CENTRAL_SERVICE_NAME, "traffic_central_controller") != 0);
    log_fd = mkstemp(log_path);
    CHECK(log_fd >= 0);
    output_fd = mkstemp(output_path);
    CHECK(output_fd >= 0);
    close(log_fd);
    log_fd = -1;
    CHECK(pipe(input) == 0);
    services[0] = connection_register_service(LOCAL_SERVICE_NAME, CONN_MODE_LOCAL);
    CHECK(services[0] != NULL);
    services[1] = connection_register_service(TRAIN_SERVICE_NAME, CONN_MODE_LOCAL);
    CHECK(services[1] != NULL);
    receive_init(&receivers[0], services[0], local_handlers, 2, &mock, CONTROLLER_LOCAL);
    receive_init(&receivers[1], services[1], train_handlers, 1, &mock, CONTROLLER_TRAIN);
    initialized = 2;

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(input[1]);
        if (dup2(input[0], STDIN_FILENO) == -1 || dup2(output_fd, STDOUT_FILENO) == -1 ||
            dup2(output_fd, STDERR_FILENO) == -1) _exit(126);
        close(input[0]);
        close(output_fd);
        execl(argv[1], argv[1], "-l", "-o", log_path, (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    input[0] = -1;
    close(output_fd);
    output_fd = -1;
    for (int i = 0; i < 2; ++i) {
        CHECK(pthread_create(&servers[i], NULL, serve, &receivers[i]) == 0);
        started++;
    }
    CHECK(await_log(log_path, "I1 state synchronized"));
    CHECK(write(input[1], "mode-sensor I7\n", 15) == 15);
    CHECK(await_log(output_path, "Invalid command or arguments"));
    CHECK(write(input[1], "mode-sensor I1\n", 15) == 15);
    CHECK(await_log(log_path, "I1 ACCEPTED"));
    CHECK(await_log(log_path, "I1 APPLIED"));
    CHECK(write(input[1], "mode-fixed I1\n", 14) == 14);
    CHECK(await_log(log_path, "I1 REJECTED"));
    CHECK(await_log(output_path, "I2  FIXED"));
    CHECK(write(input[1], "mode-sensor I1\n", 15) == 15);
    CHECK(await_accepted(&mock, 2));
    CHECK(write(input[1], "quit\n", 5) == 5);
    CHECK(await_exit(child, &child_status));
    child = -1;
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(log_contains(log_path, "Central stopping"));

    pthread_mutex_lock(&mock.mutex);
    unsigned last_command_id = mock.command_id;
    int correct = mock.accepted == 2 && mock.rejected == 1 && mock.command_id != 0 &&
                  mock.command_session != 0 &&
                  mock.local_snapshots == (1U << NUM_INTERSECTIONS) - 1 &&
                  mock.train_snapshots == (1U << NUM_CROSSINGS) - 1;
    pthread_mutex_unlock(&mock.mutex);
    CHECK(correct);
    CHECK(outcome_after_stop(log_path, last_command_id));

cleanup:
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    for (int i = 0; i < initialized; ++i) receive_stop(&receivers[i]);
    for (int i = 0; i < started; ++i) pthread_join(servers[i], NULL);
    for (int i = 0; i < initialized; ++i) receive_destroy(&receivers[i]);
    for (int i = 0; i < 2; ++i) {
        if (services[i]) connection_unregister_service(services[i]);
        if (input[i] >= 0) close(input[i]);
    }
    if (log_fd >= 0) close(log_fd);
    if (output_fd >= 0) close(output_fd);
    if (failed) {
        const char *paths[] = { log_path, output_path };
        for (int i = 0; i < 2; i++) {
            FILE *file = fopen(paths[i], "r");
            if (file) {
                char line[256];
                while (fgets(line, sizeof(line), file)) fputs(line, stderr);
                fclose(file);
            }
        }
    }
    unlink(log_path);
    unlink(output_path);
    pthread_mutex_destroy(&mock.mutex);
    printf("Central integration: %d checks, %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
