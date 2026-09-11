/* Run server and client on separate QNX nodes with GNS already configured.
 * Uses a caller-supplied test-only service; never registers production names. */
#include "../src/ipc.h"
#include "../src/commands.h"
#include <stdatomic.h>

static atomic_int completed;
static unsigned heartbeat_count, accepted_count, rejected_count;

static int handle(const test_message_t *message, reply_t *reply, void *context) {
    (void)context;
    if (message->header.type == MSG_HEARTBEAT) ++heartbeat_count;
    else if (message->header.type == MSG_MODE_COMMAND) {
        if (central_command_target(message) == I2) {
            ++rejected_count;
            reply->status = -1;
        } else ++accepted_count;
    } else if (message->header.type == MSG_TEST && !strcmp(message->data, "complete")) {
        atomic_store(&completed, 1);
    } else return -1;
    return 0;
}

static void *receive_worker(void *argument) {
    central_receiver_run(argument);
    return NULL;
}

static int run_server(const char *name) {
    central_receiver_t receiver;
    pthread_t thread;
    if (central_receiver_init(&receiver, name, CENTRAL_IPC_GLOBAL, CONTROLLER_LOCAL,
                              handle, NULL) != 0) { perror("global receiver"); return 1; }
    if (pthread_create(&thread, NULL, receive_worker, &receiver) != 0) {
        central_receiver_destroy(&receiver);
        return 1;
    }
    printf("GLOBAL_READY %s\n", name);
    fflush(stdout);
    uint64_t deadline = central_monotonic_ns() + UINT64_C(20000000000);
    while (!atomic_load(&completed) && central_monotonic_ns() < deadline) delay(20);
    central_receiver_stop(&receiver);
    pthread_join(thread, NULL);
    central_receiver_destroy(&receiver);
    int success = atomic_load(&completed) && heartbeat_count == 1 &&
                  accepted_count == 2 && rejected_count == 1;
    printf("GLOBAL_SERVER %s heartbeat=%u accepted=%u rejected=%u\n",
           success ? "PASS" : "FAIL", heartbeat_count, accepted_count, rejected_count);
    return success ? 0 : 1;
}

static int run_client(const char *name) {
    central_link_t link;
    test_message_t message;
    reply_t reply;
    unsigned target;
    int success = 0;
    if (central_link_init(&link, name, CENTRAL_IPC_GLOBAL) != 0) return 1;
    uint64_t started = central_monotonic_ns();
    uint64_t deadline = started + UINT64_C(5000000000);
    while (!central_link_is_connected(&link) && central_monotonic_ns() < deadline) {
        central_link_connect(&link);
        if (!central_link_is_connected(&link)) delay(100);
    }
    if (!central_link_is_connected(&link) ||
        central_send_heartbeat(&link, CONTROLLER_LOCAL) != CENTRAL_SEND_OK) goto cleanup;
    if (!central_parse_command("mode-temp I3 sensor 7", &message, &target)) goto cleanup;
    central_command_set_target(&message, target);
    central_command_set_id(&message, 1);
    if (central_send(&link, &message, &reply) != CENTRAL_SEND_OK || reply.command_id != 1) goto cleanup;
    central_command_set_id(&message, UINT16_MAX);
    if (central_send(&link, &message, &reply) != CENTRAL_SEND_OK || reply.command_id != UINT16_MAX) goto cleanup;
    central_command_set_target(&message, I2);
    central_command_set_id(&message, 2);
    if (central_send(&link, &message, &reply) != CENTRAL_SEND_REJECTED || reply.command_id != 2) goto cleanup;
    central_message_init(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    snprintf(message.data, sizeof(message.data), "complete");
    if (central_send(&link, &message, &reply) != CENTRAL_SEND_OK) goto cleanup;
    success = 1;
cleanup:
    if (!success) perror("global transport exchange");
    if (central_link_destroy(&link) != 0) success = 0;
    printf("GLOBAL_CLIENT %s elapsed=%.1fms\n", success ? "PASS" : "FAIL",
           (central_monotonic_ns() - started) / 1000000.0);
    return success ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 3 || strncmp(argv[2], "traffic_test_", 13) || strchr(argv[2], '/')) {
        fprintf(stderr, "Usage: %s server|client traffic_test_UNIQUE_NAME\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "server")) return run_server(argv[2]);
    if (!strcmp(argv[1], "client")) return run_client(argv[2]);
    return 1;
}
