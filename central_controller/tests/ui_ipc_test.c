#include "../src/ui_ipc.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); } } while (0)

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int callback(const char *request, char *output, size_t capacity, void *context) {
    (void)context;
    if (!strcmp(request, "slow")) usleep(800000);
    if (!strcmp(request, "unterminated")) { memset(output, 'X', capacity); return 0; }
    snprintf(output, capacity, "answer:%s\n", request);
    return !strcmp(request, "reject") ? -7 : 0;
}

/* Private protocol fixtures deliberately exercise the receiver boundary. */
typedef struct {
    uint16_t type, version;
    char text[CENTRAL_UI_REQUEST_SIZE];
} raw_request_t;
typedef struct {
    uint32_t magic;
    uint16_t version, reserved;
    int32_t status;
    uint32_t length;
    char text[CENTRAL_UI_RESPONSE_SIZE];
    uint32_t trailer;
} raw_response_t;

static void malformed_frames(const char *name) {
    raw_request_t request;
    raw_response_t response;
    uint64_t timeout;
    int coid = name_open(name, 0);
    CHECK(coid != -1);
    if (coid == -1) return;
    memset(&request, 0, sizeof(request));
    request.type = 0x7000;
    request.version = 1;
    strcpy(request.text, "status");
    timeout = 1000000000ULL;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    CHECK(MsgSend(coid, &request, sizeof(request) - 1, &response, sizeof(response)) == -1 && errno == EPROTO);
    request.version = 2;
    timeout = 1000000000ULL;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    CHECK(MsgSend(coid, &request, sizeof(request), &response, sizeof(response)) == -1 && errno == EPROTO);
    request.version = 1;
    memset(request.text, 'X', sizeof(request.text));
    timeout = 1000000000ULL;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    CHECK(MsgSend(coid, &request, sizeof(request), &response, sizeof(response)) == -1 && errno == EPROTO);
    strcpy(request.text, "status");
    timeout = 1000000000ULL;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    CHECK(MsgSend(coid, &request, sizeof(request), &response, sizeof(response) - 1) == -1 && errno == EPROTO);
    request.text[0] = '\033';
    timeout = 1000000000ULL;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    CHECK(MsgSend(coid, &request, sizeof(request), &response, sizeof(response)) == -1 && errno == EPROTO);
    name_close(coid);
}

static void alarm_handler(int signo) { (void)signo; }

static void stopped_server(const char *name) {
    int ready[2], status;
    char marker = 0;
    pid_t child;
    char output[CENTRAL_UI_RESPONSE_SIZE];
    struct sigaction action;
    int result = pipe(ready);
    CHECK(result == 0);
    if (result == -1) return;
    child = fork();
    CHECK(child != -1);
    if (child == -1) { close(ready[0]); close(ready[1]); return; }
    if (!child) {
        central_ui_server_t server = {0};
        close(ready[0]);
        marker = central_ui_server_start(&server, name, callback, NULL) == 0 ? 'Y' : 'N';
        if (write(ready[1], &marker, 1) != 1) _exit(2);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    CHECK(read(ready[0], &marker, 1) == 1 && marker == 'Y');
    close(ready[0]);
    if (marker == 'Y') {
        CHECK(kill(child, SIGSTOP) == 0);
        CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        memset(&action, 0, sizeof(action));
        action.sa_handler = alarm_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGALRM, &action, NULL);
        alarm(3); /* Keep a failed name_open timeout from hanging the suite. */
        uint64_t start = monotonic_ns();
        result = central_ui_client_request(name, "status", output, sizeof(output), &status);
        int request_error = errno;
        double elapsed_ms = (monotonic_ns() - start) / 1000000.0;
        alarm(0);
        CHECK(result == -1 && request_error == ETIMEDOUT);
        CHECK(elapsed_ms >= 400.0 && elapsed_ms < 750.0);
        printf("UI stopped-service request: %.1f ms\n", elapsed_ms);
    }
    kill(child, SIGCONT);
    kill(child, SIGTERM);
    CHECK(waitpid(child, &status, 0) == child);
}

int main(void) {
    central_ui_server_t server = {0};
    char name[80], output[CENTRAL_UI_RESPONSE_SIZE], long_request[CENTRAL_UI_REQUEST_SIZE + 1];
    int status = 0;
    snprintf(name, sizeof(name), "traffic_test_ui_%ld", (long)getpid());
    CHECK(central_ui_server_start(&server, name, callback, NULL) == 0);
    if (!server.state) return EXIT_FAILURE;
    CHECK(central_ui_client_request(name, "status", output, sizeof(output), &status) == 0);
    CHECK(!strcmp(output, "answer:status\n") && status == 0);
    CHECK(central_ui_client_request(name, "reject", output, sizeof(output), &status) == 0);
    CHECK(status == -7 && !strcmp(output, "answer:reject\n"));
    CHECK(central_ui_client_request(name, "unterminated", output, sizeof(output), &status) == 0);
    CHECK(status == -1 && strstr(output, "exceeded"));
    CHECK(central_ui_client_request(name, "status", output, 2, &status) == -1 && errno == EMSGSIZE);
    CHECK(central_ui_client_request("../bad", "status", output, sizeof(output), &status) == -1 && errno == EINVAL);
    CHECK(central_ui_client_request(name, "status\nquit", output, sizeof(output), &status) == -1 && errno == EINVAL);
    memset(long_request, 'X', sizeof(long_request));
    long_request[sizeof(long_request) - 1] = '\0';
    CHECK(central_ui_client_request(name, long_request, output, sizeof(output), &status) == -1 && errno == EINVAL);
    malformed_frames(name);
    uint64_t start = monotonic_ns();
    CHECK(central_ui_client_request(name, "slow", output, sizeof(output), &status) == -1 && errno == ETIMEDOUT);
    double elapsed_ms = (monotonic_ns() - start) / 1000000.0;
    CHECK(elapsed_ms >= 400.0 && elapsed_ms < 750.0);
    printf("UI slow-callback request: %.1f ms\n", elapsed_ms);
    CHECK(central_ui_client_request(name, "status", output, sizeof(output), &status) == 0);
    CHECK(!strcmp(output, "answer:status\n"));
    central_ui_server_stop(&server);
    central_ui_server_destroy(&server);
    CHECK(server.state == NULL);
    central_ui_server_destroy(&server);
    CHECK(central_ui_client_request(name, "status", output, sizeof(output), &status) == -1);
    CHECK(central_ui_server_start(&server, name, callback, NULL) == 0);
    CHECK(central_ui_client_request(name, "status", output, sizeof(output), &status) == 0);
    central_ui_server_destroy(&server);
    stopped_server(name);
    printf("UI IPC: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
