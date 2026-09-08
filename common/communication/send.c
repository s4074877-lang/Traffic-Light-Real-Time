#include "send.h"
#include <string.h>
#include <sys/neutrino.h>

int send_message(connection_t *conn, test_message_t *msg, reply_t *reply) {
    int coid = connection_get_coid(conn);

    if (coid == -1) {
        return -1;
    }

    if (MsgSend(coid, msg, sizeof(*msg), reply, sizeof(*reply)) == -1) {
        connection_close(conn);
        return -1;
    }

    return 0;
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
