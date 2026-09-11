#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

#include "../common/communication/connection.h"
#include "../common/communication/send.h"
#include "../common/communication/receive.h"

typedef struct {
    int notify_fd;
    int release_fd;
    receive_context_t *receiver;
} server_context_t;

typedef struct {
    connection_t *connection;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int done;
    int result;
    int error;
    uint64_t elapsed;
} sender_context_t;

static int handle_test(int rcvid, any_msg_t *msg, reply_t *reply, void *arg) {
    (void)rcvid;
    server_context_t *server = (server_context_t *)arg;
    if (strcmp(msg->payload.test.data, "hold") == 0) {
        char release;
        if (write(server->notify_fd, "H", 1) != 1 ||
            read(server->release_fd, &release, 1) != 1) {
            return -1;
        }
    } else if (strcmp(msg->payload.test.data, "reject") == 0) {
        reply->status = REPLY_REJECTED;
    } else if (strcmp(msg->payload.test.data, "applied") == 0) {
        reply->status = REPLY_APPLIED;
    } else if (strcmp(msg->payload.test.data, "bad-reply") == 0) {
        reply->command_id = 1;
    } else if (strcmp(msg->payload.test.data, "stop") == 0) {
        receive_stop(server->receiver);
    }
    return 0;
}

static int run_server(const char *name, int notify_fd, int release_fd) {
    name_attach_t *service = connection_register_service(name, CONN_MODE_LOCAL);
    if (service == NULL) return 1;
    receive_context_t receiver;
    server_context_t context = { notify_fd, release_fd, &receiver };
    message_handler_entry_t handlers[] = {{ MSG_TEST, CONTROLLER_CENTRAL, handle_test }};
    receive_init(&receiver, service, handlers, 1, &context, CONTROLLER_LOCAL);
    if (write(notify_fd, "R", 1) != 1) return 1;
    receive_loop(&receiver);
    receive_destroy(&receiver);
    connection_unregister_service(service);
    return 0;
}

static int receive_notice(int fd, char expected) {
    struct pollfd input = { fd, POLLIN, 0 };
    char value;
    return poll(&input, 1, 2000) == 1 && read(fd, &value, 1) == 1 && value == expected;
}

static void *send_held_message(void *arg) {
    sender_context_t *sender = (sender_context_t *)arg;
    any_msg_t msg;
    reply_t reply;
    protocol_init_message(&msg, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(msg.payload.test.data, "hold");
    uint64_t start = monotonic_ns();
    int result = send_message(sender->connection, &msg, &reply);
    int error = errno;
    uint64_t elapsed = monotonic_ns() - start;
    pthread_mutex_lock(&sender->mutex);
    sender->result = result;
    sender->error = error;
    sender->elapsed = elapsed;
    sender->done = 1;
    pthread_cond_signal(&sender->condition);
    pthread_mutex_unlock(&sender->mutex);
    return NULL;
}

static int wait_for_sender(sender_context_t *sender) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&sender->mutex);
    while (!sender->done) {
        if (pthread_cond_timedwait(&sender->condition, &sender->mutex, &deadline) != 0)
            break;
    }
    int done = sender->done;
    pthread_mutex_unlock(&sender->mutex);
    return done;
}

static int rejects_frame(connection_t *connection, const any_msg_t *msg,
                         size_t size, size_t reply_size) {
    reply_t reply;
    uint64_t timeout = 1000000000ULL;
    int coid = connection_get_coid(connection);
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) return 0;
    int result = MsgSend(coid, msg, size, &reply, reply_size);
    return result == -1 && errno == EPROTO;
}

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        failed = 1; \
        goto cleanup; \
    } \
} while (0)

int main(void) {
    int checks = 0, failed = 0;
    int notification[2] = { -1, -1 }, release[2] = { -1, -1 };
    pid_t child = -1;
    pthread_t sender_thread;
    int sender_started = 0;
    pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
    connection_t connection;
    char service_name[64];
    snprintf(service_name, sizeof(service_name), "traffic_transport_test_%ld", (long)getpid());
    connection_init(&connection, service_name, CONN_MODE_LOCAL, &state_mutex);
    sender_context_t sender;
    memset(&sender, 0, sizeof(sender));
    sender.connection = &connection;
    pthread_mutex_init(&sender.mutex, NULL);
    pthread_cond_init(&sender.condition, NULL);

    CHECK(pipe(notification) == 0);
    CHECK(pipe(release) == 0);
    child = fork();
    CHECK(child != -1);
    if (child == 0) {
        close(notification[0]);
        close(release[1]);
        _exit(run_server(service_name, notification[1], release[0]));
    }
    close(notification[1]);
    notification[1] = -1;
    close(release[0]);
    release[0] = -1;
    CHECK(receive_notice(notification[0], 'R'));
    CHECK(connection_try_connect(&connection) == 1);
    CHECK(connection_try_connect(&connection) == 0);
    CHECK(send_heartbeat(&connection, CONTROLLER_CENTRAL, CONTROLLER_LOCAL) == SEND_OK);

    any_msg_t msg;
    reply_t reply;
    protocol_init_message(&msg, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(msg.payload.test.data, "applied");
    CHECK(send_message(&connection, &msg, &reply) == SEND_OK && reply.status == REPLY_APPLIED);
    strcpy(msg.payload.test.data, "reject");
    CHECK(send_message(&connection, &msg, &reply) == SEND_REJECTED);
    CHECK(connection_is_connected(&connection));
    strcpy(msg.payload.test.data, "bad-reply");
    CHECK(send_message(&connection, &msg, &reply) == SEND_PROTOCOL_ERROR);
    CHECK(connection_is_connected(&connection));

    protocol_init_message(&msg, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    msg.payload.mode_cmd.intersection_id = I1;
    msg.payload.mode_cmd.priority = CMD_PRIO_OPERATOR;
    msg.payload.mode_cmd.command_id = 42;
    CHECK(send_message(&connection, &msg, &reply) == SEND_REJECTED && reply.command_id == 42);
    CHECK(rejects_frame(&connection, &msg, 1, sizeof(reply)));
    CHECK(rejects_frame(&connection, &msg, protocol_message_size(&msg) - 1, sizeof(reply)));
    CHECK(rejects_frame(&connection, &msg, protocol_message_size(&msg), sizeof(reply) - 1));
    msg.header.version++;
    CHECK(rejects_frame(&connection, &msg, protocol_message_size(&msg), sizeof(reply)));
    msg.header.version = PROTOCOL_VERSION;
    msg.payload.mode_cmd.intersection_id = NUM_INTERSECTIONS;
    CHECK(rejects_frame(&connection, &msg, protocol_message_size(&msg), sizeof(reply)));
    msg.payload.mode_cmd.intersection_id = I1;
    msg.header.dst = CONTROLLER_TRAIN;
    CHECK(rejects_frame(&connection, &msg, protocol_message_size(&msg), sizeof(reply)));

    uint64_t generation = connection_generation(&connection);
    connection_close_generation(&connection, generation + 1);
    CHECK(connection_is_connected(&connection));
    connection_close_generation(&connection, generation);
    CHECK(!connection_is_connected(&connection));
    CHECK(connection_try_connect(&connection) == 1);
    connection_close_generation(&connection, generation);
    CHECK(connection_is_connected(&connection));

    // SEND-blocked timeout while the dedicated test server is stopped.
    CHECK(kill(child, SIGSTOP) == 0);
    int child_status;
    CHECK(waitpid(child, &child_status, WUNTRACED) == child && WIFSTOPPED(child_status));
    uint64_t start = monotonic_ns();
    int result = send_heartbeat(&connection, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    int send_errno = errno;
    uint64_t elapsed = monotonic_ns() - start;
    CHECK(result == SEND_TRANSPORT_ERROR && send_errno == ETIMEDOUT);
    CHECK(elapsed >= 400000000ULL && elapsed < 1500000000ULL);
    CHECK(connection_is_connected(&connection));
    CHECK(kill(child, SIGCONT) == 0);
    CHECK(send_heartbeat(&connection, CONTROLLER_CENTRAL, CONTROLLER_LOCAL) == SEND_OK);

    // Stop only after the handler confirms receipt to exercise REPLY-blocked timeout.
    CHECK(pthread_create(&sender_thread, NULL, send_held_message, &sender) == 0);
    sender_started = 1;
    CHECK(receive_notice(notification[0], 'H'));
    CHECK(kill(child, SIGSTOP) == 0);
    CHECK(waitpid(child, &child_status, WUNTRACED) == child && WIFSTOPPED(child_status));
    CHECK(wait_for_sender(&sender));
    pthread_join(sender_thread, NULL);
    sender_started = 0;
    CHECK(sender.result == SEND_TRANSPORT_ERROR && sender.error == ETIMEDOUT);
    CHECK(sender.elapsed >= 400000000ULL && sender.elapsed < 1500000000ULL);
    CHECK(connection_is_connected(&connection));
    CHECK(kill(child, SIGCONT) == 0);
    CHECK(write(release[1], "C", 1) == 1);
    CHECK(send_heartbeat(&connection, CONTROLLER_CENTRAL, CONTROLLER_LOCAL) == SEND_OK);

    protocol_init_message(&msg, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(msg.payload.test.data, "stop");
    CHECK(send_message(&connection, &msg, &reply) == SEND_OK);
    CHECK(waitpid(child, &child_status, 0) == child);
    child = -1;
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

cleanup:
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (sender_started) pthread_join(sender_thread, NULL);
    connection_destroy(&connection);
    pthread_cond_destroy(&sender.condition);
    pthread_mutex_destroy(&sender.mutex);
    pthread_mutex_destroy(&state_mutex);
    for (int i = 0; i < 2; i++) {
        if (notification[i] >= 0) close(notification[i]);
        if (release[i] >= 0) close(release[i]);
    }
    printf("Transport: %d checks, %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
