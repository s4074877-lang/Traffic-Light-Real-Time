#include "local_process.h"

#define STATUS_HISTORY_SIZE 64
typedef struct {
    uint64_t id;
    status_msg_t status;
} status_event_t;
static status_event_t history[STATUS_HISTORY_SIZE];
static unsigned history_next;
static uint64_t event_id;

static void record_status(void) {
    status_msg_t status;
    unsigned latest = (history_next + STATUS_HISTORY_SIZE - 1) % STATUS_HISTORY_SIZE;
    fill_status_locked(&status);
    if (event_id && history[latest].status.status_sequence == status.status_sequence) {
        return;
    }
    if (!event_id) {
        event_id = monotonic_ns();
    }
    history[history_next].id = ++event_id;
    history[history_next].status = status;
    history_next = (history_next + 1) % STATUS_HISTORY_SIZE;
}

static void next_status_event(uint64_t delivered, core_reply_t *reply) {
    unsigned i;
    status_event_t *selected = &history[(history_next + STATUS_HISTORY_SIZE - 1) % STATUS_HISTORY_SIZE];
    /* A reconnect starts with current authoritative state, not stale history. */
    if (delivered) {
        for (i = 0; i < STATUS_HISTORY_SIZE; ++i) {
            status_event_t *candidate = &history[i];
            if (candidate->id > delivered && candidate->id < selected->id)
                selected = candidate;
        }
    }
    reply->event_id = selected->id;
    reply->event_status = selected->status;
}

static void fill_core_reply(core_reply_t *reply) {
    test_message_t fault;
    pthread_mutex_lock(&state.mutex);
    fill_status_locked(&reply->status);
    snprintf(reply->service_name, sizeof(reply->service_name), "%s", state.service_name);
    reply->global_mode = state.mode == CONN_MODE_GLOBAL;
    reply->central_connected = state.central_connected;
    reply->train_connected = state.train_connected;
    snprintf(reply->last_recv_central, sizeof(reply->last_recv_central), "%s", state.last_recv_central);
    snprintf(reply->last_recv_train, sizeof(reply->last_recv_train), "%s", state.last_recv_train);
    reply->initial_phase = state.initial_phase;
    reply->ns_green_sec = state.ns_green_sec;
    reply->ew_green_sec = state.ew_green_sec;
    reply->sim_seconds = state.sim_seconds;
    reply->fault_sequence = prepare_fault_message_locked(&fault);
    memcpy(&reply->fault, fault.data, sizeof(reply->fault));
    pthread_mutex_unlock(&state.mutex);
}

int run_core(const char *name) {
    name_attach_t *attach = attach_service(name, 0);
    uint64_t next_tick = monotonic_ns() + LOCAL_TICK_NS;
    uint64_t link_deadline = 0;
    if (!attach) {
        perror("Cannot publish Local core service");
        return EXIT_FAILURE;
    }
    record_status();
    /* A single owner executes both inputs and phase transitions. Network and
     * terminal output never run here; the receive timeout releases each tick. */
    while (!local_stopping) {
        core_request_t request;
        core_reply_t reply;
        struct _msg_info info;
        uint64_t now = monotonic_ns();
        if (now >= next_tick) {
            pthread_mutex_lock(&state.mutex);
            if (link_deadline && now >= link_deadline) {
                state.central_connected = 0;
                state.train_connected = 0;
            }
#if ENABLE_TRAFFIC_SIMULATION
            update_time_of_day_locked();
#endif
            update_temporary_mode_locked();
            traffic_tick_locked();
#if ENABLE_TRAFFIC_SIMULATION
            update_vehicle_counts_locked();
#endif
            mark_status_dirty_locked();
            record_status();
            pthread_mutex_unlock(&state.mutex);
            /* Never compress missed ticks into immediate lamp transitions. */
            next_tick = monotonic_ns() + LOCAL_TICK_NS;
        }
        int rcvid = receive_application(attach->chid, &request, sizeof(request), &info);
        if (rcvid < 0) continue;
        if (info.srcmsglen != sizeof(request) || request.type != CORE_REQUEST ||
            info.dstmsglen != sizeof(reply)) {
            MsgError(rcvid, EINVAL);
            continue;
        }
        memset(&reply, 0, sizeof(reply));
        reply.result.status = -1;
        phase_t previous_phase = state.phase;
        if (request.operation == CORE_SNAPSHOT) {
            reply.result.status = 0;
        } else if (request.operation == CORE_FRAME) {
            if (local_dispatch_message(&request.frame, &reply.result) != 0)
                reply.result.status = -1;
        } else if (request.operation == CORE_INPUT && valid_input(request.command)) {
            reply.result.status = execute_command(request.command) == 0 ? 0 : -1;
        } else if (request.operation == CORE_LINKS) {
            pthread_mutex_lock(&state.mutex);
            state.central_connected = request.central_up != 0;
            state.train_connected = request.train_up != 0;
            pthread_mutex_unlock(&state.mutex);
            link_deadline = monotonic_ns() + LOCAL_LINK_TIMEOUT_NS;
            reply.result.status = 0;
        }
        fill_core_reply(&reply);
        if (state.phase != previous_phase) {
            /* A phase started by an input gets a full first second too. */
            next_tick = monotonic_ns() + LOCAL_TICK_NS;
        }
        record_status();
        next_status_event(request.delivered_event, &reply);
        get_timestamp(reply.result.timestamp, sizeof(reply.result.timestamp));
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }
    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
