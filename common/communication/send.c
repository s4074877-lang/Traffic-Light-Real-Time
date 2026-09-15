#include "send.h"
#include <string.h>
#include <sys/neutrino.h>

int send_message_timeout(connection_t *conn, const void *msg, size_t msg_size,
                         reply_t *reply, unsigned timeout_ms) {
    int coid = connection_get_coid(conn);

    if (coid == -1) {
        return -1;
    }

    // Arm a timeout for the next blocking kernel call (the MsgSend below) so an
    // unresponsive peer cannot block the caller indefinitely
    uint64_t timeout = (uint64_t)timeout_ms * 1000000ULL;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) {
        return -1;
    }

    if (MsgSend(coid, msg, msg_size, reply, sizeof(*reply)) == -1) {
        connection_close(conn);
        return -1;
    }

    return 0;
}

int send_message(connection_t *conn, test_message_t *msg, reply_t *reply) {
    return send_message_timeout(conn, msg, sizeof(*msg), reply, SEND_TIMEOUT_MS);
}

int send_test_message(connection_t *conn, controller_type_t src, controller_type_t dst) {
    if (!connection_is_connected(conn)) {
        return -1;
    }

    test_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_TEST;
    msg.header.src = src;
    msg.header.dst = dst;
    get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));

    reply_t reply;
    return send_message(conn, &msg, &reply);
}

int send_heartbeat(connection_t *conn, controller_type_t src, controller_type_t dst) {
    if (!connection_is_connected(conn)) {
        return -1;
    }

    test_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_HEARTBEAT;
    msg.header.src = src;
    msg.header.dst = dst;
    get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));

    reply_t reply;
    return send_message(conn, &msg, &reply);
}
