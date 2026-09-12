#include "../src/ipc.h"
#include "../src/commands.h"
#include <stddef.h>
#include <signal.h>
#include <sys/iomsg.h>
#include <sys/wait.h>

static unsigned checks;
static pid_t servers[2] = {-1, -1};
static int notice[2] = {-1, -1};

/* Fixture controls live after the typed command in its unused data bytes. */
typedef struct {
    uint8_t active;
    int8_t status;
    uint16_t command_id;
    uint32_t length;
} reply_fixture_t;

_Static_assert(sizeof(mode_cmd_msg_t) + sizeof(reply_fixture_t) <=
               sizeof(((test_message_t *)0)->data), "reply fixture fits legacy frame");

static void cleanup(void) {
    unsigned i;
    for (i = 0; i < 2; ++i) {
        if (servers[i] > 0) {
            kill(servers[i], SIGKILL);
            waitpid(servers[i], NULL, 0);
            servers[i] = -1;
        }
        if (notice[i] >= 0) close(notice[i]);
    }
}

static void require(int condition, const char *description) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s (errno=%d)\n", description, errno);
        exit(EXIT_FAILURE);
    }
}

/* Matches the original peers: name_attach's channel retains UNBLOCK, and
 * application replies use kernel status zero even for a payload NACK. */
static void legacy_server(const char *name, int ready_fd) {
    name_attach_t *attach = name_attach(NULL, name, 0);
    if (!attach) _exit(2);
    if (write(ready_fd, "R", 1) != 1) _exit(3);
    for (;;) {
        union { struct _pulse pulse; test_message_t message; } frame;
        int rcvid = MsgReceive(attach->chid, &frame, sizeof(frame), NULL);
        reply_t reply = {0};
        int paused = 0;
        if (rcvid < 0) _exit(4);
        if (rcvid == 0) {
            if (frame.pulse.code == _PULSE_CODE_DISCONNECT)
                ConnectDetach(frame.pulse.scoid);
            continue;
        }
        if (frame.pulse.type == _IO_CONNECT) {
            MsgReply(rcvid, 0, NULL, 0);
            continue;
        }
        if (frame.pulse.type >= _IO_BASE) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        central_timestamp(reply.timestamp, sizeof(reply.timestamp));
        reply.command_id = central_command_id(&frame.message);
        if (frame.message.header.type == MSG_MODE_COMMAND) {
            reply_fixture_t fixture;
            memcpy(&fixture, frame.message.data + sizeof(mode_cmd_msg_t), sizeof(fixture));
            if (fixture.active) {
                if (fixture.length > sizeof(reply)) _exit(8);
                reply.status = fixture.status;
                reply.command_id = fixture.command_id;
                MsgReply(rcvid, 0, &reply, fixture.length);
                continue;
            }
            if (reply.command_id == 99) { reply.status = -1; reply.command_id = 0; }
            if (reply.command_id == 100) reply.command_id = 101;
        }
        if (frame.message.header.type == MSG_TEST) {
            if (!strcmp(frame.message.data, "NACK")) reply.status = -1;
            if (!strcmp(frame.message.data, "BAD_STATUS")) reply.status = 1;
            if (!strcmp(frame.message.data, "BAD_TS"))
                memset(reply.timestamp, 'x', sizeof(reply.timestamp));
            if (!strcmp(frame.message.data, "SHORT")) {
                MsgReply(rcvid, 0, &reply, 1);
                continue;
            }
            if (!strcmp(frame.message.data, "BAD_KERNEL")) {
                MsgReply(rcvid, sizeof(reply), &reply, sizeof(reply));
                continue;
            }
            if (!strcmp(frame.message.data, "STOP")) {
                if (write(ready_fd, "P", 1) != 1) _exit(5);
                raise(SIGSTOP);
                paused = 1;
            }
        }
        MsgReply(rcvid, 0, &reply, sizeof(reply));
        if (paused && write(ready_fd, "D", 1) != 1) _exit(7);
    }
}

static void start_server(unsigned index, const char *name) {
    int pipe_fd[2];
    char ready = 0;
    require(pipe(pipe_fd) == 0, "fixture pipe");
    servers[index] = fork();
    require(servers[index] >= 0, "fixture fork");
    if (!servers[index]) {
        close(pipe_fd[0]);
        legacy_server(name, pipe_fd[1]);
        _exit(6);
    }
    close(pipe_fd[1]);
    notice[index] = pipe_fd[0];
    require(read(notice[index], &ready, 1) == 1 && ready == 'R', "fixture ready");
}

static int receiver_callback(const test_message_t *message, reply_t *reply, void *context) {
    unsigned *calls = context;
    __atomic_add_fetch(calls, 1, __ATOMIC_SEQ_CST);
    reply->status = !strcmp(message->data, "NACK") ? -1 : 0;
    return 0;
}

static void *receive_thread(void *arg) {
    central_receiver_run(arg);
    return NULL;
}

static int raw_send(int coid, const void *message, size_t size, reply_t *reply) {
    uint64_t timeout = CENTRAL_IPC_TIMEOUT_MS * UINT64_C(1000000);
    memset(reply, 0x5a, sizeof(*reply));
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                 NULL, &timeout, NULL);
    return MsgSend(coid, message, size, reply, sizeof(*reply));
}

static void test_reply_prefixes(central_link_t *link) {
    static const uint16_t ids[] = {1, 255, 256, 0xa5a5, 0xff00, UINT16_MAX};
    const size_t fields_end = offsetof(reply_t, command_id) + sizeof(uint16_t);
    test_message_t message;
    reply_t reply;
    unsigned target;
    size_t i, length;
    int outcome;
    require(central_parse_command("mode-fixed I1", &message, &target),
            "partial reply fixture parsed");
    central_command_set_target(&message, I1);
    central_timestamp(message.header.timestamp, sizeof(message.header.timestamp));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        central_command_set_id(&message, ids[i]);
        for (outcome = 0; outcome < 3; ++outcome) {
            reply_fixture_t fixture = {1, outcome == 0 ? 0 : -1,
                                       outcome == 2 ? 0 : ids[i], 0};
            char description[128];
            for (length = 0; length < fields_end; ++length) {
                fixture.length = (uint32_t)length;
                memcpy(message.data + sizeof(mode_cmd_msg_t), &fixture, sizeof(fixture));
                snprintf(description, sizeof(description),
                         "partial %s ID %u: %zu bytes is a protocol error",
                         outcome == 0 ? "ACK" : outcome == 1 ? "NACK" : "zero-ID NACK",
                         (unsigned)ids[i], length);
                require(central_send(link, &message, &reply) == CENTRAL_SEND_PROTOCOL &&
                        errno == EPROTO, description);
            }
            fixture.length = sizeof(reply);
            memcpy(message.data + sizeof(mode_cmd_msg_t), &fixture, sizeof(fixture));
            snprintf(description, sizeof(description), "complete reply ID %u outcome %d",
                     (unsigned)ids[i], outcome);
            require(central_send(link, &message, &reply) ==
                    (outcome == 0 ? CENTRAL_SEND_OK : CENTRAL_SEND_REJECTED), description);
            require(reply.command_id == fixture.command_id && reply.status == fixture.status,
                    "complete legacy reply fields preserved");
        }
    }
    require(central_link_is_connected(link), "malformed replies preserve the transport");
    require(central_send_heartbeat(link, CONTROLLER_LOCAL) == CENTRAL_SEND_OK,
            "link remains usable after partial reply errors");
}

int main(void) {
    central_link_t local, train, receiver_link;
    central_receiver_t receiver;
    pthread_t thread;
    test_message_t message;
    reply_t reply;
    unsigned callback_calls = 0, target;
    uint64_t started, elapsed;
    char local_name[64], train_name[64], receiver_name[64], byte = 0;
    int coid, rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    atexit(cleanup);
    snprintf(local_name, sizeof(local_name), "traffic_test_local_%ld", (long)getpid());
    snprintf(train_name, sizeof(train_name), "traffic_test_train_%ld", (long)getpid());
    snprintf(receiver_name, sizeof(receiver_name), "traffic_test_receiver_%ld", (long)getpid());
    start_server(0, local_name);
    start_server(1, train_name);
    require(central_link_init(&local, local_name, CENTRAL_IPC_LOCAL) == 0, "local init");
    require(central_link_init(&train, train_name, CENTRAL_IPC_LOCAL) == 0, "train init");
    require(central_link_connect(&local) == 1, "legacy local name_open");
    require(central_link_connect(&local) == 0, "already connected");
    require(central_link_connect(&train) == 1, "legacy train name_open");
    require(central_link_is_connected(&local), "local connection state");
    require(central_send_heartbeat(&local, CONTROLLER_LOCAL) == CENTRAL_SEND_OK,
            "original ABI heartbeat and kernel status zero ACK");

    central_message_init(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(message.data, "NACK");
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_REJECTED,
            "legacy payload status -1 is rejection");
    require(central_link_is_connected(&local), "NACK preserves transport");
    require(central_parse_command("mode-fixed I1", &message, &target), "mode fixture parsed");
    central_command_set_target(&message, I1);
    central_timestamp(message.header.timestamp, sizeof(message.header.timestamp));
    central_command_set_id(&message, 99);
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_REJECTED,
            "legacy unknown-handler NACK with zero command ID");
    central_command_set_id(&message, 100);
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_PROTOCOL,
            "ACK for a different command is not accepted");
    central_command_set_id(&message, 102);
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_OK,
            "matching command ID ACK");
    require(reply.command_id == 102, "reply payload remains ABI compatible");

    test_reply_prefixes(&local);

    central_message_init(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(message.data, "BAD_STATUS");
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_PROTOCOL, "unknown reply status");
    strcpy(message.data, "BAD_TS");
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_PROTOCOL, "unterminated reply timestamp");
    strcpy(message.data, "SHORT");
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_PROTOCOL, "truncated legacy reply");
    strcpy(message.data, "BAD_KERNEL");
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_PROTOCOL, "unexpected kernel reply status");

    require(central_receiver_init(&receiver, receiver_name, CENTRAL_IPC_LOCAL,
                                  CONTROLLER_CENTRAL, receiver_callback, &callback_calls) == 0,
            "receiver init");
    require(pthread_create(&thread, NULL, receive_thread, &receiver) == 0, "receiver thread");
    coid = name_open(receiver_name, 0);
    require(coid >= 0, "original name_open client connects to central receiver");
    central_message_init(&message, MSG_TEST, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    strcpy(message.data, "hello");
    require(raw_send(coid, &message, sizeof(message), &reply) == 0 && reply.status == 0,
            "central replies with kernel status zero and payload ACK");
    require(__atomic_load_n(&callback_calls, __ATOMIC_SEQ_CST) == 1, "validated frame reaches callback");
    strcpy(message.data, "NACK");
    require(raw_send(coid, &message, sizeof(message), &reply) == 0 && reply.status == -1,
            "central callback NACK uses original ABI");
    rc = raw_send(coid, &message, sizeof(msg_header_t), &reply);
    require(rc == -1 || reply.status == -1, "short inbound frame rejected");
    require(__atomic_load_n(&callback_calls, __ATOMIC_SEQ_CST) == 2, "short inbound frame bypasses callback");
    message.header.dst = CONTROLLER_TRAIN;
    rc = raw_send(coid, &message, sizeof(message), &reply);
    require(rc == -1 || reply.status == -1, "wrong destination rejected");
    require(__atomic_load_n(&callback_calls, __ATOMIC_SEQ_CST) == 2, "wrong destination bypasses callback");
    message.header.dst = CONTROLLER_CENTRAL;
    memset(message.header.timestamp, 'x', sizeof(message.header.timestamp));
    rc = raw_send(coid, &message, sizeof(message), &reply);
    require(rc == -1 || reply.status == -1, "unterminated inbound timestamp rejected");
    require(__atomic_load_n(&callback_calls, __ATOMIC_SEQ_CST) == 2, "invalid timestamp bypasses callback");
    name_close(coid);
    started = central_monotonic_ns();
    central_receiver_stop(&receiver);
    require(pthread_join(thread, NULL) == 0, "idle receiver joins");
    central_receiver_destroy(&receiver);
    require(central_monotonic_ns() - started < UINT64_C(1500000000), "bounded receiver shutdown");

    central_message_init(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(message.data, "STOP");
    started = central_monotonic_ns();
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_TRANSPORT,
            "SIGSTOP legacy server cannot pin caller");
    elapsed = central_monotonic_ns() - started;
    require(elapsed >= UINT64_C(400000000) && elapsed < UINT64_C(1500000000),
            "caller timeout near 500 ms");
    require(read(notice[0], &byte, 1) == 1 && byte == 'P', "server received request before SIGSTOP");
    started = central_monotonic_ns();
    require(central_send_heartbeat(&train, CONTROLLER_TRAIN) == CENTRAL_SEND_OK,
            "train link responds while local is reply-blocked");
    require(central_monotonic_ns() - started < UINT64_C(1000000000), "other link remains responsive");
    started = central_monotonic_ns();
    require(central_send(&local, &message, &reply) == CENTRAL_SEND_TRANSPORT,
            "busy timed-out worker does not replay command");
    require(errno == EBUSY, "pending operation reports busy without queuing another send");
    require(central_monotonic_ns() - started < UINT64_C(1500000000), "busy worker bounded");
    started = central_monotonic_ns();
    central_link_close(&local);
    require(!central_link_is_connected(&local), "closed pending link reports disconnected");
    rc = central_link_destroy(&local);
    require(rc == -1, "blocked worker owns its retained heap state");
    require(central_monotonic_ns() - started < UINT64_C(1500000000), "destroy bounded with stopped peer");
    require(kill(servers[0], SIGCONT) == 0, "resume only stopped fixture");
    require(read(notice[0], &byte, 1) == 1 && byte == 'D', "legacy peer releases late reply");
    require(central_send_heartbeat(&train, CONTROLLER_TRAIN) == CENTRAL_SEND_OK,
            "late detached reply leaves other link healthy");
    require(central_link_init(&local, local_name, CENTRAL_IPC_LOCAL) == 0, "reinitialize released link");
    require(central_link_connect(&local) == 1, "connect after old worker release");
    require(central_send_heartbeat(&local, CONTROLLER_LOCAL) == CENTRAL_SEND_OK,
            "old timed-out command is never replayed");
    require(central_link_destroy(&local) == 0, "new link destroys cleanly");
    require(central_link_destroy(&train) == 0, "responsive link destroys cleanly");

    require(central_link_init(&receiver_link, receiver_name, CENTRAL_IPC_LOCAL) == 0,
            "missing service init");
    started = central_monotonic_ns();
    require(central_link_connect(&receiver_link) == 0, "missing service connect fails");
    require(!central_link_is_connected(&receiver_link), "missing service stays disconnected");
    require(central_monotonic_ns() - started < UINT64_C(1500000000), "missing service connect bounded");
    require(central_link_destroy(&receiver_link) == 0, "missing service worker cleanup");
    printf("ipc: %u checks passed; stopped-peer timeout %.1f ms\n", checks, elapsed / 1000000.0);
    return 0;
}
