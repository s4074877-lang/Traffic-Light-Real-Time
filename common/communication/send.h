#ifndef SEND_H
#define SEND_H

#include "../common.h"
#include "connection.h"

// Send a test message to another controller
// Returns: 0 on success, -1 on failure
int send_test_message(connection_t *conn, controller_type_t src, controller_type_t dst);

// Send a heartbeat to check if connection is alive
// Returns: 0 on success (connected), -1 on failure (disconnected)
int send_heartbeat(connection_t *conn, controller_type_t src, controller_type_t dst);

// Send a raw message and get reply
// Returns: 0 on success, -1 on failure (also closes connection on failure)
int send_message(connection_t *conn, test_message_t *msg, reply_t *reply);

#endif // SEND_H
