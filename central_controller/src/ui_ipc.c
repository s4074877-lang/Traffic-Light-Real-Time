#include "ui_ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#define UI_MESSAGE_TYPE 0x7000u
#define UI_PROTOCOL_VERSION 1u
#define UI_RESPONSE_MAGIC 0x43554931u

typedef struct {
    uint16_t type;
    uint16_t version;
    char text[CENTRAL_UI_REQUEST_SIZE];
} ui_request_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t status;
    uint32_t length;
    char text[CENTRAL_UI_RESPONSE_SIZE];
    uint32_t trailer;
} ui_response_t;

struct central_ui_server_state {
    name_attach_t *attach;
    pthread_t thread;
    pthread_mutex_t mutex;
    int stopping;
    central_ui_callback_t callback;
    void *context;
};

static int valid_name(const char *name) {
    size_t i;
    if (!name || !*name || strlen(name) > 100) return 0;
    for (i = 0; name[i]; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int valid_text(const char *text, size_t capacity) {
    size_t i;
    for (i = 0; i < capacity; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!c) return 1;
        if ((c < 32 && c != '\t') || c > 126) return 0;
    }
    return 0;
}

static int reply_deadline(void) {
    uint64_t timeout = CENTRAL_UI_TIMEOUT_MS * 1000000ULL;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
}

static void reject(int rcvid, int error) {
    if (reply_deadline() != -1) MsgError(rcvid, error);
}

static int receive_request(struct central_ui_server_state *state) {
    union {
        ui_request_t request;
        struct _pulse pulse;
        struct _io_connect connect;
    } buffer;
    struct _msg_info info;
    uint64_t timeout = 100000000ULL;
    int rcvid;
    memset(&buffer, 0, sizeof(buffer));
    memset(&info, 0, sizeof(info));
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL) == -1)
        return -1;
    rcvid = MsgReceive(state->attach->chid, &buffer, sizeof(buffer), &info);
    if (rcvid == -1) return errno == ETIMEDOUT || errno == EINTR ? 0 : -1;
    if (rcvid == 0) {
        if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(buffer.pulse.scoid);
        return 0;
    }
    if (info.msglen < (int)sizeof(uint16_t) || info.msglen > info.srcmsglen) {
        reject(rcvid, EPROTO);
        return 0;
    }
    if (buffer.connect.type == _IO_CONNECT) {
        if (info.msglen >= (int)offsetof(struct _io_connect, file_type) &&
            buffer.connect.subtype == _IO_CONNECT_OPEN) {
            if (reply_deadline() != -1) MsgReply(rcvid, 0, NULL, 0);
        } else reject(rcvid, ENOSYS);
        return 0;
    }
    if (info.srcmsglen != (int)sizeof(ui_request_t) ||
        info.msglen != (int)sizeof(ui_request_t) ||
        info.dstmsglen != (int)sizeof(ui_response_t) ||
        buffer.request.type != UI_MESSAGE_TYPE ||
        buffer.request.version != UI_PROTOCOL_VERSION ||
        !valid_text(buffer.request.text, sizeof(buffer.request.text))) {
        reject(rcvid, EPROTO);
        return 0;
    }
    ui_response_t response;
    memset(&response, 0, sizeof(response));
    response.magic = UI_RESPONSE_MAGIC;
    response.version = UI_PROTOCOL_VERSION;
    response.trailer = UI_RESPONSE_MAGIC;
    response.status = state->callback(buffer.request.text, response.text,
                                       sizeof(response.text), state->context);
    if (!memchr(response.text, '\0', sizeof(response.text))) {
        response.status = -1;
        snprintf(response.text, sizeof(response.text), "Central response exceeded the display buffer.\n");
    }
    response.length = (uint32_t)strlen(response.text);
    if (reply_deadline() != -1) MsgReply(rcvid, (int)sizeof(response), &response, sizeof(response));
    return 0;
}

static void *server_thread(void *context) {
    struct central_ui_server_state *state = context;
    for (;;) {
        int stopping;
        pthread_mutex_lock(&state->mutex);
        stopping = state->stopping;
        pthread_mutex_unlock(&state->mutex);
        if (stopping || receive_request(state) == -1) break;
    }
    return NULL;
}

int central_ui_server_start(central_ui_server_t *server, const char *name,
                            central_ui_callback_t callback, void *context) {
    int error, chid;
    dispatch_t *dispatch;
    struct central_ui_server_state *state;
    if (!server || !valid_name(name) || !callback) { errno = EINVAL; return -1; }
    server->state = NULL;
    state = calloc(1, sizeof(*state));
    if (!state) return -1;
    error = pthread_mutex_init(&state->mutex, NULL);
    if (error) { free(state); errno = error; return -1; }
    /* No _NTO_CHF_UNBLOCK: a timed-out UI may leave immediately. A late reply
     * can fail harmlessly; the core never retains a pointer into client memory. */
    chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (chid == -1) goto fail_mutex;
    dispatch = dispatch_create_channel(chid, 0);
    if (!dispatch) {
        error = errno;
        ChannelDestroy(chid);
        errno = error;
        goto fail_mutex;
    }
    state->attach = name_attach(dispatch, name, 0);
    if (!state->attach) {
        error = errno;
        dispatch_destroy(dispatch);
        errno = error;
        goto fail_mutex;
    }
    state->callback = callback;
    state->context = context;
    error = pthread_create(&state->thread, NULL, server_thread, state);
    if (error) {
        name_detach(state->attach, 0);
        errno = error;
        goto fail_mutex;
    }
    server->state = state;
    return 0;
fail_mutex:
    error = errno;
    pthread_mutex_destroy(&state->mutex);
    free(state);
    errno = error;
    return -1;
}

void central_ui_server_stop(central_ui_server_t *server) {
    if (!server || !server->state) return;
    pthread_mutex_lock(&server->state->mutex);
    server->state->stopping = 1;
    pthread_mutex_unlock(&server->state->mutex);
}

void central_ui_server_destroy(central_ui_server_t *server) {
    struct central_ui_server_state *state;
    if (!server || !server->state) return;
    state = server->state;
    central_ui_server_stop(server);
    pthread_join(state->thread, NULL);
    name_detach(state->attach, 0);
    pthread_mutex_destroy(&state->mutex);
    free(state);
    server->state = NULL;
}

int central_ui_client_request(const char *name, const char *request,
                              char *output, size_t capacity, int *status) {
    ui_request_t message;
    ui_response_t response;
    struct timespec now;
    uint64_t deadline;
    int coid, error, result;
    if (!valid_name(name) || !request || !output || !capacity ||
        !valid_text(request, CENTRAL_UI_REQUEST_SIZE)) { errno = EINVAL; return -1; }
    output[0] = '\0';
    memset(&message, 0, sizeof(message));
    message.type = UI_MESSAGE_TYPE;
    message.version = UI_PROTOCOL_VERSION;
    memcpy(message.text, request, strlen(request) + 1);
    /* A short reply must not validate through uninitialized/zero-filled fields. */
    memset(&response, 0xa5, sizeof(response));
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return -1;
    deadline = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec +
               CENTRAL_UI_TIMEOUT_MS * 1000000ULL;
    if (TimerTimeout(CLOCK_MONOTONIC, TIMER_ABSTIME | _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &deadline, NULL) == -1) return -1;
    coid = name_open(name, 0);
    if (coid == -1) return -1;
    if (TimerTimeout(CLOCK_MONOTONIC, TIMER_ABSTIME | _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &deadline, NULL) == -1) {
        error = errno;
        name_close(coid);
        errno = error;
        return -1;
    }
    result = MsgSend(coid, &message, sizeof(message), &response, sizeof(response));
    error = errno;
    name_close(coid);
    if (result == -1) { errno = error; return -1; }
    if (result != (int)sizeof(response) || response.magic != UI_RESPONSE_MAGIC ||
        response.version != UI_PROTOCOL_VERSION || response.reserved ||
        response.trailer != UI_RESPONSE_MAGIC ||
        response.length >= sizeof(response.text) ||
        response.text[response.length] ||
        strnlen(response.text, sizeof(response.text)) != response.length) {
        errno = EPROTO;
        return -1;
    }
    if (response.length >= capacity) { errno = EMSGSIZE; return -1; }
    memcpy(output, response.text, response.length + 1);
    if (status) *status = response.status;
    return 0;
}
