#include "ipc.h"
#include "commands.h"
#include <stddef.h>
#include <sys/iomsg.h>

typedef enum { JOB_NONE, JOB_CONNECT, JOB_SEND } central_job_t;

struct central_link_state {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    char *name;
    int flags;
    int coid;
    int connected;
    int close_requested;
    int stopping;
    int exited;
    int detached;
    int busy;
    int done;
    int abandoned;
    central_job_t job;
    uint64_t generation;
    uint64_t job_generation;
    test_message_t request;
    reply_t reply;
    int result;
    int error;
};

uint64_t central_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

void central_timestamp(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm value;
    buffer[0] = '\0';
    if (localtime_r(&now, &value) != NULL) {
        strftime(buffer, size, "%H:%M:%S", &value);
    }
}

void central_message_init(test_message_t *message, msg_type_t type,
                          controller_type_t source, controller_type_t destination) {
    memset(message, 0, sizeof(*message));
    message->header.type = (uint16_t)type;
    message->header.src = (uint16_t)source;
    message->header.dst = (uint16_t)destination;
    central_timestamp(message->header.timestamp, sizeof(message->header.timestamp));
}

static int controller_valid(unsigned controller) {
    return controller >= CONTROLLER_LOCAL && controller <= CONTROLLER_CENTRAL;
}

static int target_valid(unsigned target) {
    return target < NUM_INTERSECTIONS || target == INTERSECTION_ALL;
}

int central_frame_valid(const test_message_t *message, size_t size,
                         controller_type_t destination) {
    if (message == NULL || size != sizeof(*message) ||
        !controller_valid(destination) || !controller_valid(message->header.src) ||
        message->header.dst != destination || message->header.reserved != 0 ||
        memchr(message->header.timestamp, '\0', sizeof(message->header.timestamp)) == NULL) {
        return 0;
    }
    unsigned source = message->header.src;
    switch (message->header.type) {
        case MSG_TEST:
            return memchr(message->data, '\0', sizeof(message->data)) != NULL;
        case MSG_HEARTBEAT: {
            heartbeat_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            unsigned count = source == CONTROLLER_TRAIN ? NUM_CROSSINGS : NUM_INTERSECTIONS;
            return value.sender_id < count && value.healthy <= 1;
        }
        case MSG_STATUS_UPDATE: {
            status_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            return source == CONTROLLER_LOCAL && destination == CONTROLLER_CENTRAL &&
                value.intersection_id < NUM_INTERSECTIONS && value.mode <= MODE_FAILSAFE &&
                value.phase <= PHASE_RAILWAY_HOLD && value.ns_state <= LIGHT_GREEN &&
                value.ew_state <= LIGHT_GREEN && value.pedestrian_ns <= 1 &&
                value.pedestrian_ew <= 1 && value.railway_preempt <= 1;
        }
        case MSG_RAILWAY_STATUS: {
            railway_status_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            return source == CONTROLLER_TRAIN && destination == CONTROLLER_CENTRAL &&
                value.crossing_id < NUM_CROSSINGS && value.train_state <= TRAIN_CLEAR &&
                value.gate_state <= GATE_FAULT && value.fault <= FAULT_NOT_WORKING;
        }
        case MSG_FAULT_ALERT: {
            fault_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            unsigned count = source == CONTROLLER_TRAIN ? NUM_CROSSINGS : NUM_INTERSECTIONS;
            return (source == CONTROLLER_LOCAL || source == CONTROLLER_TRAIN) &&
                destination == CONTROLLER_CENTRAL && value.source_id < count &&
                value.fault_type <= FAULT_NOT_WORKING && value.severity >= SEV_LOW &&
                value.severity <= SEV_CRITICAL && value.reserved == 0 &&
                memchr(value.description, '\0', sizeof(value.description)) != NULL;
        }
        case MSG_MODE_COMMAND: {
            mode_cmd_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            return source == CONTROLLER_CENTRAL && destination == CONTROLLER_LOCAL &&
                target_valid(value.intersection_id) && value.new_mode <= MODE_SENSOR &&
                value.action <= CMD_REVERT && value.priority >= CMD_PRIO_SCHEDULE &&
                value.priority <= CMD_PRIO_OPERATOR && value.command_id != 0 &&
                (value.action == CMD_TEMPORARY ? value.duration_sec != 0 : value.duration_sec == 0);
        }
        case MSG_COORDINATION_COMMAND: {
            coordination_command_msg_t value;
            memcpy(&value, message->data, sizeof(value));
            return source == CONTROLLER_CENTRAL && destination == CONTROLLER_LOCAL &&
                target_valid(value.intersection_id) && value.mode == MODE_FIXED &&
                (value.phase == PHASE_NS_GREEN || value.phase == PHASE_EW_GREEN) &&
                value.reserved == 0 && value.command_id != 0 &&
                value.cycle_offset_sec < 2 * (GREEN_BASE_SEC + YELLOW_SEC);
        }
        default:
            return 0;
    }
}

static struct timespec deadline_after_ms(unsigned milliseconds) {
    uint64_t deadline = central_monotonic_ns() + (uint64_t)milliseconds * 1000000ULL;
    struct timespec result;
    result.tv_sec = (time_t)(deadline / 1000000000ULL);
    result.tv_nsec = (long)(deadline % 1000000000ULL);
    return result;
}

static int send_timeout(void) {
    uint64_t timeout = (uint64_t)CENTRAL_IPC_TIMEOUT_MS * 1000000ULL;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                        NULL, &timeout, NULL);
}

static void clear_timeout(void) {
    TimerTimeout(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);
}

static void free_link_state(central_link_state_t *state) {
    pthread_cond_destroy(&state->changed);
    pthread_mutex_destroy(&state->mutex);
    free(state->name);
    free(state);
}

static int validate_reply(const test_message_t *request, const reply_t *reply,
                           int kernel_status) {
    uint16_t id = central_command_id(request);
    if (kernel_status != 0 ||
        memchr(reply->timestamp, '\0', sizeof(reply->timestamp)) == NULL) {
        return CENTRAL_SEND_PROTOCOL;
    }
    if (reply->status == -1) {
        return reply->command_id == 0 || reply->command_id == id ?
            CENTRAL_SEND_REJECTED : CENTRAL_SEND_PROTOCOL;
    }
    if (reply->status != 0 || reply->command_id != id) {
        return CENTRAL_SEND_PROTOCOL;
    }
    return CENTRAL_SEND_OK;
}

static void *link_worker(void *argument) {
    central_link_state_t *state = argument;
    pthread_mutex_lock(&state->mutex);
    for (;;) {
        if (state->close_requested || state->stopping) {
            int coid = state->coid;
            state->coid = -1;
            state->connected = 0;
            state->close_requested = 0;
            if (coid != -1) {
                pthread_mutex_unlock(&state->mutex);
                name_close(coid);
                pthread_mutex_lock(&state->mutex);
            }
        }
        if (state->stopping) {
            break;
        }
        if (state->job == JOB_NONE) {
            pthread_cond_wait(&state->changed, &state->mutex);
            continue;
        }
        central_job_t job = state->job;
        uint64_t generation = state->job_generation;
        int coid = state->coid;
        state->job = JOB_NONE;
        test_message_t request = state->request;
        reply_t reply;
        memset(&reply, 0xa5, sizeof(reply));
        reply.command_id = (uint16_t)(central_command_id(&request) ^ UINT16_MAX);
        pthread_mutex_unlock(&state->mutex);

        int result = CENTRAL_SEND_TRANSPORT;
        int error = 0;
        int discovered = -1;
        if (send_timeout() == -1) {
            error = errno;
        } else if (job == JOB_CONNECT) {
            discovered = name_open(state->name, state->flags);
            error = discovered == -1 ? errno : 0;
        } else {
            int status = MsgSend(coid, &request, sizeof(request), &reply, sizeof(reply));
            if (status == -1) {
                error = errno;
            } else {
                result = validate_reply(&request, &reply, status);
                error = result == CENTRAL_SEND_PROTOCOL ? EPROTO : 0;
            }
        }
        clear_timeout();

        pthread_mutex_lock(&state->mutex);
        int current = !state->stopping && !state->close_requested &&
            generation == state->generation;
        if (job == JOB_CONNECT) {
            if (current && !state->abandoned && discovered != -1) {
                state->coid = discovered;
                state->connected = 1;
                result = 1;
            } else {
                result = 0;
                if (discovered != -1) {
                    pthread_mutex_unlock(&state->mutex);
                    name_close(discovered);
                    pthread_mutex_lock(&state->mutex);
                }
            }
        } else if (!current) {
            result = CENTRAL_SEND_TRANSPORT;
            error = ECANCELED;
        }
        state->result = result;
        state->error = error;
        state->reply = reply;
        if (state->abandoned) {
            state->busy = 0;
            state->abandoned = 0;
        } else {
            state->done = 1;
        }
        pthread_cond_broadcast(&state->changed);
    }
    state->exited = 1;
    int detached = state->detached;
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);
    if (detached) {
        free_link_state(state);
    }
    return NULL;
}

int central_link_init(central_link_t *link, const char *name, central_ipc_mode_t mode) {
    if (link == NULL || name == NULL || *name == '\0' ||
        (mode != CENTRAL_IPC_LOCAL && mode != CENTRAL_IPC_GLOBAL)) {
        errno = EINVAL;
        return -1;
    }
    link->state = NULL;
    central_link_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return -1;
    }
    state->name = strdup(name);
    if (state->name == NULL) {
        free(state);
        return -1;
    }
    state->coid = -1;
    state->flags = mode == CENTRAL_IPC_GLOBAL ? NAME_FLAG_ATTACH_GLOBAL : 0;
    int error = pthread_mutex_init(&state->mutex, NULL);
    if (error != 0) {
        free(state->name);
        free(state);
        errno = error;
        return -1;
    }
    pthread_condattr_t attributes;
    error = pthread_condattr_init(&attributes);
    if (error == 0) {
        error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        if (error == 0) {
            error = pthread_cond_init(&state->changed, &attributes);
        }
        pthread_condattr_destroy(&attributes);
    }
    if (error != 0) {
        pthread_mutex_destroy(&state->mutex);
        free(state->name);
        free(state);
        errno = error;
        return -1;
    }
    error = pthread_create(&state->worker, NULL, link_worker, state);
    if (error != 0) {
        free_link_state(state);
        errno = error;
        return -1;
    }
    link->state = state;
    return 0;
}

static int run_job(central_link_t *link, central_job_t job,
                    const test_message_t *message, reply_t *reply) {
    int failure = job == JOB_CONNECT ? 0 : CENTRAL_SEND_TRANSPORT;
    if (link == NULL || link->state == NULL) {
        errno = ENOTCONN;
        return failure;
    }
    central_link_state_t *state = link->state;
    struct timespec deadline = deadline_after_ms(CENTRAL_IPC_TIMEOUT_MS);
    pthread_mutex_lock(&state->mutex);
    if (state->stopping || state->busy || state->close_requested ||
        (job == JOB_CONNECT ? state->connected : !state->connected)) {
        errno = state->busy ? EBUSY : ENOTCONN;
        pthread_mutex_unlock(&state->mutex);
        return failure;
    }
    state->busy = 1;
    state->done = 0;
    state->abandoned = 0;
    state->job = job;
    state->job_generation = state->generation;
    if (message != NULL) {
        state->request = *message;
    } else {
        memset(&state->request, 0, sizeof(state->request));
    }
    pthread_cond_broadcast(&state->changed);
    int error = 0;
    while (!state->done && error == 0) {
        error = pthread_cond_timedwait(&state->changed, &state->mutex, &deadline);
    }
    if (!state->done) {
        if (state->job != JOB_NONE) {
            // A request that never reached the worker must not execute later.
            state->job = JOB_NONE;
            state->busy = 0;
        } else {
            state->abandoned = 1;
        }
        pthread_mutex_unlock(&state->mutex);
        errno = error;
        return failure;
    }
    int result = state->result;
    int saved_error = state->error;
    if (reply != NULL && (result == CENTRAL_SEND_OK || result == CENTRAL_SEND_REJECTED)) {
        *reply = state->reply;
    }
    state->busy = 0;
    state->done = 0;
    pthread_mutex_unlock(&state->mutex);
    errno = saved_error;
    return result;
}

int central_link_connect(central_link_t *link) {
    return run_job(link, JOB_CONNECT, NULL, NULL);
}

int central_link_is_connected(central_link_t *link) {
    if (link == NULL || link->state == NULL) {
        return 0;
    }
    central_link_state_t *state = link->state;
    pthread_mutex_lock(&state->mutex);
    int connected = state->connected;
    pthread_mutex_unlock(&state->mutex);
    return connected;
}

void central_link_close(central_link_t *link) {
    if (link == NULL || link->state == NULL) {
        return;
    }
    central_link_state_t *state = link->state;
    pthread_mutex_lock(&state->mutex);
    state->connected = 0;
    state->close_requested = 1;
    state->generation++;
    if (state->job != JOB_NONE) {
        state->result = state->job == JOB_CONNECT ? 0 : CENTRAL_SEND_TRANSPORT;
        state->error = ECANCELED;
        state->job = JOB_NONE;
        state->done = 1;
    }
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);
}

int central_link_destroy(central_link_t *link) {
    if (link == NULL || link->state == NULL) {
        return 0;
    }
    central_link_state_t *state = link->state;
    link->state = NULL;
    struct timespec deadline = deadline_after_ms(CENTRAL_IPC_TIMEOUT_MS);
    pthread_mutex_lock(&state->mutex);
    state->stopping = 1;
    state->connected = 0;
    state->generation++;
    pthread_cond_broadcast(&state->changed);
    int error = 0;
    while (!state->exited && error == 0) {
        error = pthread_cond_timedwait(&state->changed, &state->mutex, &deadline);
    }
    if (!state->exited) {
        // A legacy channel may hold a reply-blocked sender after its timeout.
        // The worker owns every object it can still access after this return.
        state->detached = 1;
        pthread_detach(state->worker);
        pthread_mutex_unlock(&state->mutex);
        errno = error;
        return -1;
    }
    pthread_mutex_unlock(&state->mutex);
    pthread_join(state->worker, NULL);
    free_link_state(state);
    return 0;
}

int central_send(central_link_t *link, const test_message_t *message, reply_t *reply) {
    if (message == NULL || reply == NULL ||
        !central_frame_valid(message, sizeof(*message), (controller_type_t)message->header.dst)) {
        errno = EINVAL;
        return CENTRAL_SEND_PROTOCOL;
    }
    memset(reply, 0, sizeof(*reply));
    return run_job(link, JOB_SEND, message, reply);
}

int central_send_heartbeat(central_link_t *link, controller_type_t destination) {
    test_message_t message;
    central_message_init(&message, MSG_HEARTBEAT, CONTROLLER_CENTRAL, destination);
    heartbeat_msg_t heartbeat = {0};
    heartbeat.healthy = 1;
    memcpy(message.data, &heartbeat, sizeof(heartbeat));
    reply_t reply;
    return central_send(link, &message, &reply);
}

static int reply_timeout(void) {
    uint64_t timeout = (uint64_t)CENTRAL_IPC_TIMEOUT_MS * 1000000ULL;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
}

static void reject_frame(int rcvid, int error) {
    if (reply_timeout() != -1) {
        MsgError(rcvid, error);
    }
}

int central_receiver_init(central_receiver_t *receiver, const char *name,
                           central_ipc_mode_t mode, controller_type_t self,
                           central_message_handler_t handler, void *context) {
    if (receiver == NULL || name == NULL || *name == '\0' || !controller_valid(self) ||
        (mode != CENTRAL_IPC_LOCAL && mode != CENTRAL_IPC_GLOBAL)) {
        errno = EINVAL;
        return -1;
    }
    memset(receiver, 0, sizeof(*receiver));
    int error = pthread_mutex_init(&receiver->mutex, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (chid == -1) {
        pthread_mutex_destroy(&receiver->mutex);
        return -1;
    }
    dispatch_t *dispatch = dispatch_create_channel(chid, 0);
    if (dispatch == NULL) {
        error = errno;
        ChannelDestroy(chid);
        pthread_mutex_destroy(&receiver->mutex);
        errno = error;
        return -1;
    }
    unsigned flags = mode == CENTRAL_IPC_GLOBAL ? NAME_FLAG_ATTACH_GLOBAL : 0;
    receiver->attach = name_attach(dispatch, name, flags);
    if (receiver->attach == NULL) {
        error = errno;
        dispatch_destroy(dispatch);
        pthread_mutex_destroy(&receiver->mutex);
        errno = error;
        return -1;
    }
    receiver->self = self;
    receiver->handler = handler;
    receiver->context = context;
    return 0;
}

static int receive_frame(central_receiver_t *receiver) {
    union {
        test_message_t message;
        struct _pulse pulse;
        struct _io_connect connect;
    } buffer;
    struct _msg_info info;
    memset(&buffer, 0, sizeof(buffer));
    memset(&info, 0, sizeof(info));
    uint64_t timeout = 200000000ULL;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL) == -1) {
        return -1;
    }
    int rcvid = MsgReceive(receiver->attach->chid, &buffer, sizeof(buffer), &info);
    if (rcvid == -1) {
        return errno == ETIMEDOUT || errno == EINTR ? 0 : -1;
    }
    if (rcvid == 0) {
        if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
            ConnectDetach(buffer.pulse.scoid);
        }
        return 0;
    }
    if (info.srcmsglen < (int)sizeof(uint16_t) || info.msglen < 0 ||
        info.msglen > info.srcmsglen || info.msglen > (int)sizeof(buffer)) {
        reject_frame(rcvid, EPROTO);
        return 0;
    }
    size_t source_size = (size_t)info.srcmsglen;
    size_t received = (size_t)info.msglen;
    size_t wanted = source_size < sizeof(buffer) ? source_size : sizeof(buffer);
    if (received < wanted) {
        if (reply_timeout() == -1) {
            reject_frame(rcvid, EIO);
            return 0;
        }
        ssize_t count = MsgRead(rcvid, (char *)&buffer + received, wanted - received, received);
        if (count < 0 || (size_t)count != wanted - received) {
            reject_frame(rcvid, EPROTO);
            return 0;
        }
        received += (size_t)count;
    }
    if (buffer.connect.type == _IO_CONNECT) {
        if (received >= offsetof(struct _io_connect, file_type) &&
            buffer.connect.subtype == _IO_CONNECT_OPEN) {
            if (reply_timeout() != -1) {
                MsgReply(rcvid, 0, NULL, 0);
            }
        } else {
            reject_frame(rcvid, ENOSYS);
        }
        return 0;
    }
    if (buffer.connect.type >= _IO_BASE && buffer.connect.type <= _IO_MAX) {
        reject_frame(rcvid, ENOSYS);
        return 0;
    }
    if (received != source_size || info.dstmsglen != sizeof(reply_t) ||
        !central_frame_valid(&buffer.message, source_size, receiver->self)) {
        reject_frame(rcvid, EPROTO);
        return 0;
    }
    reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.command_id = central_command_id(&buffer.message);
    if (receiver->handler != NULL) {
        if (receiver->handler(&buffer.message, &reply, receiver->context) != 0) {
            reply.status = -1;
        }
    } else if (buffer.message.header.type != MSG_HEARTBEAT) {
        reply.status = -1;
    }
    central_timestamp(reply.timestamp, sizeof(reply.timestamp));
    if (reply_timeout() != -1) {
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }
    return 0;
}

void central_receiver_run(central_receiver_t *receiver) {
    for (;;) {
        pthread_mutex_lock(&receiver->mutex);
        int stopping = receiver->stopping;
        pthread_mutex_unlock(&receiver->mutex);
        if (stopping || receive_frame(receiver) == -1) {
            return;
        }
    }
}

void central_receiver_stop(central_receiver_t *receiver) {
    pthread_mutex_lock(&receiver->mutex);
    receiver->stopping = 1;
    pthread_mutex_unlock(&receiver->mutex);
}

void central_receiver_destroy(central_receiver_t *receiver) {
    if (receiver->attach != NULL) {
        name_detach(receiver->attach, 0);
        receiver->attach = NULL;
    }
    pthread_mutex_destroy(&receiver->mutex);
}
