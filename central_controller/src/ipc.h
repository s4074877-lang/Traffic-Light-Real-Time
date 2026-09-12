#ifndef CENTRAL_IPC_H
#define CENTRAL_IPC_H

#include "../../common/common.h"
#include <pthread.h>

#ifndef CENTRAL_LOCAL_SERVICE
#define CENTRAL_LOCAL_SERVICE LOCAL_SERVICE_NAME
#endif
#ifndef CENTRAL_TRAIN_SERVICE
#define CENTRAL_TRAIN_SERVICE TRAIN_SERVICE_NAME
#endif
#ifndef CENTRAL_SERVICE
#define CENTRAL_SERVICE CENTRAL_SERVICE_NAME
#endif

#define CENTRAL_IPC_TIMEOUT_MS 500

typedef enum {
    CENTRAL_IPC_LOCAL = 0,
    CENTRAL_IPC_GLOBAL = 1
} central_ipc_mode_t;

typedef enum {
    CENTRAL_SEND_OK = 0,
    CENTRAL_SEND_TRANSPORT = -1,
    CENTRAL_SEND_REJECTED = -2,
    CENTRAL_SEND_PROTOCOL = -3
} central_send_result_t;

typedef struct central_link_state central_link_state_t;
typedef struct {
    central_link_state_t *state;
} central_link_t;

int central_link_init(central_link_t *link, const char *name, central_ipc_mode_t mode);
// Returns 1 for a newly established connection, 0 otherwise. May wait up to 500 ms.
int central_link_connect(central_link_t *link);
int central_link_is_connected(central_link_t *link);
void central_link_close(central_link_t *link);
// Call after application threads using the link have joined. Returns -1 if a
// legacy server still holds an IPC worker; its heap state remains privately owned.
// QNX may also defer process exit until that server replies or terminates.
int central_link_destroy(central_link_t *link);
int central_send(central_link_t *link, const test_message_t *message, reply_t *reply);
int central_send_heartbeat(central_link_t *link, controller_type_t destination);

void central_timestamp(char *buffer, size_t size);
uint64_t central_monotonic_ns(void);
void central_message_init(test_message_t *message, msg_type_t type,
                          controller_type_t source, controller_type_t destination);
uint16_t central_command_id(const test_message_t *message);
int central_frame_valid(const test_message_t *message, size_t size,
                         controller_type_t destination);
// Convert an exact wire frame into the validated internal envelope. Legacy
// envelopes use protocol zero-based IDs. Compact Train status/fault frames use
// the current Train simulator's 1..3 IDs; compact heartbeat IDs remain zero-based
// as defined by heartbeat_msg_t. Failed conversion leaves output unchanged.
int central_frame_normalize(const void *frame, size_t size,
                             controller_type_t destination, test_message_t *output);

// The receive thread validates and normalizes a complete wire frame before
// invoking the callback. Return 0 to reply, or -1 to reject. Callbacks complete inline.
typedef int (*central_message_handler_t)(const test_message_t *message,
                                         reply_t *reply, void *context);
typedef struct {
    name_attach_t *attach;
    pthread_mutex_t mutex;
    int stopping;
    controller_type_t self;
    central_message_handler_t handler;
    void *context;
} central_receiver_t;

int central_receiver_init(central_receiver_t *receiver, const char *name,
                           central_ipc_mode_t mode, controller_type_t self,
                           central_message_handler_t handler, void *context);
void central_receiver_run(central_receiver_t *receiver);
void central_receiver_stop(central_receiver_t *receiver);
// Call after the receive thread has joined.
void central_receiver_destroy(central_receiver_t *receiver);

#endif
