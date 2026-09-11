#include "send.h"
#include <string.h>
#include <sys/neutrino.h>

static void unlock_io(void *arg) {
    pthread_mutex_unlock((pthread_mutex_t *)arg);
}

static int send_locked(connection_t *conn, const any_msg_t *msg, reply_t *reply) {
    int coid = connection_get_coid(conn);
    if (coid == -1) {
        errno = ENOTCONN;
        return SEND_TRANSPORT_ERROR;
    }

    uint64_t timeout = (uint64_t)MESSAGE_TIMEOUT_MS * 1000000ULL;
    size_t message_size = protocol_message_size(msg);
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) {
        return SEND_TRANSPORT_ERROR;
    }
    int result = MsgSend(coid, msg, message_size, reply, sizeof(*reply));
    if (result == -1) {
        return SEND_TRANSPORT_ERROR;
    }

    // MsgReply uses its kernel status to identify the complete reply format.
    if (result != (int)sizeof(*reply) || reply->reserved != 0 ||
        reply->command_id != protocol_command_id(msg) ||
        (reply->status != REPLY_ACCEPTED && reply->status != REPLY_APPLIED &&
         reply->status != REPLY_REJECTED)) {
        errno = EPROTO;
        return SEND_PROTOCOL_ERROR;
    }
    if (reply->status == REPLY_REJECTED) {
        errno = EACCES;
        return SEND_REJECTED;
    }

    uint16_t expected_type = 0;
    if (msg->header.type == MSG_STATUS_REQUEST) {
        expected_type = msg->header.dst == CONTROLLER_LOCAL ?
                        MSG_STATUS_UPDATE : MSG_RAILWAY_STATUS;
    }
    if (reply->type != expected_type) {
        errno = EPROTO;
        return SEND_PROTOCOL_ERROR;
    }
    return SEND_OK;
}

int send_message(connection_t *conn, const any_msg_t *msg, reply_t *reply) {
    if (conn == NULL || msg == NULL || reply == NULL) {
        errno = EINVAL;
        return SEND_PROTOCOL_ERROR;
    }
    memset(reply, 0, sizeof(*reply));
    reply->status = REPLY_REJECTED;
    if (!protocol_validate_message(msg, protocol_message_size(msg),
                                   (controller_type_t)msg->header.dst)) {
        errno = EINVAL;
        return SEND_PROTOCOL_ERROR;
    }

    int result;
    int saved_errno;
    pthread_mutex_lock(&conn->io_mutex);
    pthread_cleanup_push(unlock_io, &conn->io_mutex);
    result = send_locked(conn, msg, reply);
    saved_errno = errno;
    pthread_cleanup_pop(1);
    errno = saved_errno;
    return result;
}

int send_test_message(connection_t *conn, controller_type_t src, controller_type_t dst) {
    any_msg_t msg;
    protocol_init_message(&msg, MSG_TEST, src, dst);
    get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));

    reply_t reply;
    return send_message(conn, &msg, &reply);
}

int send_heartbeat(connection_t *conn, controller_type_t src, controller_type_t dst) {
    any_msg_t msg;
    protocol_init_message(&msg, MSG_HEARTBEAT, src, dst);
    msg.payload.heartbeat.sender_id = CONTROLLER_NODE_ID;
    msg.payload.heartbeat.healthy = 1;
    get_timestamp(msg.header.timestamp, sizeof(msg.header.timestamp));

    reply_t reply;
    return send_message(conn, &msg, &reply);
}
