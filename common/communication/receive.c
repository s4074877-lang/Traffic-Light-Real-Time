#include "receive.h"
#include <string.h>
#include <sys/neutrino.h>

void receive_init(receive_context_t *ctx, name_attach_t *attach,
                  message_handler_entry_t *handlers, int handler_count,
                  void *user_context) {
    ctx->attach = attach;
    ctx->handlers = handlers;
    ctx->handler_count = handler_count;
    ctx->user_context = user_context;
}

void receive_send_reply(int rcvid, reply_t *reply) {
    MsgReply(rcvid, 0, reply, sizeof(*reply));
}

void receive_send_error(int rcvid) {
    reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.status = -1;
    MsgReply(rcvid, 0, &reply, sizeof(reply));
}

static message_handler_t find_handler(receive_context_t *ctx,
                                       msg_type_t msg_type,
                                       controller_type_t from) {
    for (int i = 0; i < ctx->handler_count; i++) {
        message_handler_entry_t *entry = &ctx->handlers[i];

        if (entry->msg_type != msg_type) {
            continue;
        }

        // 0 means accept from any controller
        if (entry->from != 0 && entry->from != from) {
            continue;
        }

        return entry->handler;
    }

    return NULL;
}

int receive_once(receive_context_t *ctx) {
    test_message_t msg;
    reply_t reply;
    struct _msg_info info;

    int rcvid = MsgReceive(ctx->attach->chid, &msg, sizeof(msg), &info);

    if (rcvid == -1) {
        return -1;
    }

    if (rcvid == 0) {
        // Pulse received - ignore
        return 0;
    }

    // Handle heartbeat automatically - just reply success
    if (msg.header.type == MSG_HEARTBEAT) {
        memset(&reply, 0, sizeof(reply));
        reply.status = 0;
        get_timestamp(reply.timestamp, sizeof(reply.timestamp));
        receive_send_reply(rcvid, &reply);
        return 1;
    }

    // Find handler for this message type
    message_handler_t handler = find_handler(ctx, msg.header.type, msg.header.src);

    if (handler != NULL) {
        memset(&reply, 0, sizeof(reply));
        int result = handler(rcvid, &msg, &reply, ctx->user_context);

        if (result == 0) {
            receive_send_reply(rcvid, &reply);
        } else {
            receive_send_error(rcvid);
        }
    } else {
        receive_send_error(rcvid);
    }

    return 1;
}

void receive_loop(receive_context_t *ctx) {
    while (1) {
        receive_once(ctx);
    }
}
