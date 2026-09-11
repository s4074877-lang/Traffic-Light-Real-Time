#ifndef RECEIVE_H
#define RECEIVE_H

#include "../common.h"
#include <sys/dispatch.h>
#include <pthread.h>

// Message handler function type
// Parameters: rcvid, received message, pointer to reply (handler should fill it)
// Returns: 0 on success, -1 on error. Handlers complete inline and never retain rcvid.
typedef int (*message_handler_t)(int rcvid, any_msg_t *msg, reply_t *reply, void *context);

// Handler registration structure
typedef struct {
    msg_type_t msg_type;           // Message type to handle
    controller_type_t from;        // From which controller (or 0 for any)
    message_handler_t handler;     // Handler function
} message_handler_entry_t;

// Receive context for the loop
typedef struct {
    name_attach_t *attach;                    // Name attach pointer
    message_handler_entry_t *handlers;        // Array of handlers
    int handler_count;                        // Number of handlers
    void *user_context;                       // User-provided context passed to handlers
    controller_type_t self;
    pthread_mutex_t mutex;
    int stopping;
} receive_context_t;

// Initialize receive context
void receive_init(receive_context_t *ctx, name_attach_t *attach,
                  message_handler_entry_t *handlers, int handler_count,
                  void *user_context, controller_type_t self);

// Main receive loop - blocks and dispatches messages
void receive_loop(receive_context_t *ctx);

void receive_stop(receive_context_t *ctx);
// Call after the receive thread has been joined.
void receive_destroy(receive_context_t *ctx);

// Receive and handle a single message (non-blocking alternative)
// Returns: 1 if message handled, 0 if no message, -1 on error
int receive_once(receive_context_t *ctx);

// Helper to send reply
void receive_send_reply(int rcvid, reply_t *reply);

// Helper to send error reply
void receive_send_error(int rcvid);

#endif // RECEIVE_H
