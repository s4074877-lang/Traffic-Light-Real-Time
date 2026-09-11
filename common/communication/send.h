#ifndef SEND_H
#define SEND_H

#include "../common.h"
#include "connection.h"

#define MESSAGE_TIMEOUT_MS 500

typedef enum {
    SEND_OK = 0,
    SEND_TRANSPORT_ERROR = -1,
    SEND_REJECTED = -2,
    SEND_PROTOCOL_ERROR = -3
} send_result_t;

// Send a test message to another controller
// Returns: 0 on success, -1 on failure
int send_test_message(connection_t *conn, controller_type_t src, controller_type_t dst);

// Send a heartbeat to check if connection is alive
// Returns SEND_OK when the peer responds; failures do not close the connection.
int send_heartbeat(connection_t *conn, controller_type_t src, controller_type_t dst);

// Send one typed message. The reply status distinguishes accepted from applied.
// A timeout leaves the result uncertain; the caller owns retry/disconnect policy.
int send_message(connection_t *conn, const any_msg_t *msg, reply_t *reply);

#endif // SEND_H
