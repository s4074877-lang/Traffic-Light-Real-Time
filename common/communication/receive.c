#include "receive.h"
#include "send.h"
#include <string.h>
#include <sys/neutrino.h>
#include <sys/iomsg.h>

void receive_init(receive_context_t *ctx, name_attach_t *attach,
                  message_handler_entry_t *handlers, int handler_count,
                  void *user_context, controller_type_t self) {
    ctx->attach = attach;
    ctx->handlers = handlers;
    ctx->handler_count = handler_count;
    ctx->user_context = user_context;
    ctx->self = self;
    ctx->stopping = 0;
    pthread_mutex_init(&ctx->mutex, NULL);
}

static int reply_timeout(void) {
    uint64_t timeout = (uint64_t)MESSAGE_TIMEOUT_MS * 1000000ULL;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_REPLY,
                        NULL, &timeout, NULL);
}

static void reply_error(int rcvid, int error) {
    if (reply_timeout() != -1) {
        MsgError(rcvid, error);
    }
}

void receive_send_reply(int rcvid, reply_t *reply) {
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    if (reply_timeout() != -1) {
        MsgReply(rcvid, (int)sizeof(*reply), reply, sizeof(*reply));
    }
}

void receive_send_error(int rcvid) {
    reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.status = REPLY_REJECTED;
    receive_send_reply(rcvid, &reply);
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

static int receive_wait(receive_context_t *ctx, uint64_t timeout) {
    union {
        any_msg_t message;
        struct _pulse pulse;
        struct _io_connect connect;
    } buffer;
    reply_t reply;
    struct _msg_info info;

    memset(&buffer, 0, sizeof(buffer));
    memset(&info, 0, sizeof(info));
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                     NULL, &timeout, NULL) == -1) {
        return -1;
    }
    int rcvid = MsgReceive(ctx->attach->chid, &buffer, sizeof(buffer), &info);

    if (rcvid == -1) {
        if (errno == ETIMEDOUT || errno == EINTR) {
            return 0;
        }
        return -1;
    }

    if (rcvid == 0) {
        if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
            ConnectDetach(buffer.pulse.scoid);
        } else if (buffer.pulse.code == _PULSE_CODE_UNBLOCK) {
            int blocked_rcvid = buffer.pulse.value.sival_int;
            struct _msg_info blocked_info;
            if (MsgInfo(blocked_rcvid, &blocked_info) == 0 &&
                (blocked_info.flags & _NTO_MI_UNBLOCK_REQ)) {
                reply_error(blocked_rcvid, EINTR);
            }
        }
        return 0;
    }

    if (info.msglen < 0 || info.srcmsglen < (int)sizeof(uint16_t) ||
        info.msglen > info.srcmsglen || info.msglen > (int)sizeof(buffer)) {
        reply_error(rcvid, EPROTO);
        return 1;
    }

    size_t source_size = (size_t)info.srcmsglen;
    size_t reply_size = (size_t)info.dstmsglen;
    size_t received_size = (size_t)info.msglen;
    size_t buffer_size = source_size < sizeof(buffer) ? source_size : sizeof(buffer);
    // Qnet may initially deliver only part of a message, even when it fits.
    if (received_size < buffer_size) {
        if (reply_timeout() == -1) {
            reply_error(rcvid, EIO);
            return 1;
        }
        ssize_t bytes = MsgRead(rcvid, (char *)&buffer + received_size,
                                buffer_size - received_size, received_size);
        if (bytes < 0 || (size_t)bytes != buffer_size - received_size) {
            reply_error(rcvid, EPROTO);
            return 1;
        }
        received_size += (size_t)bytes;
    }

    if (buffer.connect.type == _IO_CONNECT) {
        if (received_size >= offsetof(struct _io_connect, file_type) &&
            buffer.connect.subtype == _IO_CONNECT_OPEN) {
            if (reply_timeout() != -1) {
                MsgReply(rcvid, 0, NULL, 0);
            }
        } else {
            reply_error(rcvid, ENOSYS);
        }
        return 1;
    }
    if (buffer.connect.type >= _IO_BASE && buffer.connect.type <= _IO_MAX) {
        reply_error(rcvid, ENOSYS);
        return 1;
    }

    if (source_size < offsetof(any_msg_t, payload) ||
        source_size > sizeof(any_msg_t) || received_size != source_size ||
        reply_size != sizeof(reply_t) ||
        !protocol_validate_message(&buffer.message, source_size, ctx->self)) {
        reply_error(rcvid, EPROTO);
        return 1;
    }

    any_msg_t *msg = &buffer.message;
    msg->header.timestamp[sizeof(msg->header.timestamp) - 1] = '\0';
    memset(&reply, 0, sizeof(reply));
    reply.status = REPLY_ACCEPTED;
    reply.command_id = protocol_command_id(msg);

    message_handler_t handler = find_handler(ctx, (msg_type_t)msg->header.type,
                                             (controller_type_t)msg->header.src);
    if (handler != NULL) {
        if (handler(rcvid, msg, &reply, ctx->user_context) != 0) {
            reply.status = REPLY_REJECTED;
        }
    } else if (msg->header.type != MSG_HEARTBEAT) {
        reply.status = REPLY_REJECTED;
    }

    receive_send_reply(rcvid, &reply);
    return 1;
}

int receive_once(receive_context_t *ctx) {
    return receive_wait(ctx, 0);
}

void receive_loop(receive_context_t *ctx) {
    while (1) {
        pthread_mutex_lock(&ctx->mutex);
        int stopping = ctx->stopping;
        pthread_mutex_unlock(&ctx->mutex);
        if (stopping) {
            break;
        }
        if (receive_wait(ctx, 200000000ULL) == -1) {
            fprintf(stderr, "Receive failed: %s\n", strerror(errno));
            break;
        }
    }
}

void receive_stop(receive_context_t *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->stopping = 1;
    pthread_mutex_unlock(&ctx->mutex);
}

void receive_destroy(receive_context_t *ctx) {
    pthread_mutex_destroy(&ctx->mutex);
}
