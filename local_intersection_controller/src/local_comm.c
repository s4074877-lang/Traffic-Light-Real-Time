#include "local_process.h"

typedef struct { const char *core_name; connection_mode_t mode; } comm_context_t;

static int send_payload(int *coid, msg_type_t type, const void *payload, size_t size) {
    test_message_t frame;
    reply_t reply;
    init_message(&frame, type, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    memcpy(frame.data, payload, size);
    memset(&reply, 0, sizeof(reply));
    reply.status = -1;
    return exchange(coid, &frame, sizeof(frame), &reply, sizeof(reply)) == 0 &&
           reply.status == 0 ? 0 : -1;
}

static void *report_thread(void *argument) {
    comm_context_t *context = argument;
    int core = -1, central = -1, train = -1;
    uint16_t last_fault = 0, sequence = 0;
    uint64_t delivered_event = 0;
    uint64_t next_heartbeat = 0;
    while (!local_stopping) {
        core_request_t request = {0};
        core_reply_t reply;
        uint64_t now = monotonic_ns();
        int heartbeat_due = now >= next_heartbeat;
        if (heartbeat_due) {
            if (central == -1) {
                central = open_service(CENTRAL_SERVICE_NAME, context->mode == CONN_MODE_GLOBAL);
                delivered_event = 0;
                last_fault = 0;
            }
            if (train == -1)
                train = open_service(TRAIN_SERVICE_NAME, context->mode == CONN_MODE_GLOBAL);
            if (train != -1) {
                test_message_t frame;
                reply_t ack;
                init_message(&frame, MSG_HEARTBEAT, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
                exchange(&train, &frame, sizeof(frame), &ack, sizeof(ack));
            }
            next_heartbeat = now + 1000000000ULL;
        }
        request.operation = CORE_LINKS;
        request.central_up = central != -1;
        request.train_up = train != -1;
        request.delivered_event = delivered_event;
        if (core_call(&core, context->core_name, &request, &reply) == 0 &&
            reply.result.status == 0) {
            if (central != -1 && (delivered_event != reply.event_id || heartbeat_due)) {
                if (send_payload(&central, MSG_STATUS_UPDATE, &reply.event_status, sizeof(reply.event_status)) == 0)
                    delivered_event = reply.event_id;
            }
            if (central != -1 && last_fault != reply.fault_sequence) {
                if (send_payload(&central, MSG_FAULT_ALERT, &reply.fault, sizeof(reply.fault)) == 0)
                    last_fault = reply.fault_sequence;
            }
            if (central != -1 && heartbeat_due) {
                heartbeat_msg_t heartbeat = {0};
                heartbeat.sender_id = reply.status.intersection_id;
                heartbeat.healthy = !reply.status.fault_active && reply.status.mode != MODE_FAILSAFE;
                if (++sequence == 0) ++sequence;
                heartbeat.sequence = sequence;
                send_payload(&central, MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
            }
        }
        usleep(100000);
    }
    if (core != -1) name_close(core);
    if (central != -1) name_close(central);
    if (train != -1) name_close(train);
    return NULL;
}

int run_comm(const char *service, const char *core_name, connection_mode_t mode) {
    name_attach_t *attach = attach_service(service, mode == CONN_MODE_GLOBAL);
    comm_context_t context = { core_name, mode };
    pthread_t reporter;
    int core = -1;
    if (!attach) return EXIT_FAILURE;
    if (pthread_create(&reporter, NULL, report_thread, &context) != 0) {
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }
    while (!local_stopping) {
        core_request_t request = {0};
        core_reply_t response;
        test_message_t frame;
        struct _msg_info info;
        int rcvid = receive_application(attach->chid, &frame, sizeof(frame), &info);
        if (rcvid < 0) continue;
        int railway = frame.header.type == MSG_RAILWAY_PREEMPT ||
                      frame.header.type == MSG_TRAIN_CLEAR;
        if (info.dstmsglen != sizeof(reply_t) ||
            (info.srcmsglen != sizeof(frame) &&
             !(railway && info.srcmsglen == sizeof(railway_full_msg_t))) ||
            frame.header.dst != CONTROLLER_LOCAL ||
            !memchr(frame.header.timestamp, '\0', sizeof(frame.header.timestamp))) {
            MsgError(rcvid, EINVAL);
            continue;
        }
        request.operation = CORE_FRAME;
        request.frame = frame;
        if (core_call(&core, core_name, &request, &response) != 0) {
            MsgError(rcvid, EHOSTDOWN);
        } else {
            MsgReply(rcvid, 0, &response.result, sizeof(response.result));
        }
    }
    pthread_join(reporter, NULL);
    if (core != -1) name_close(core);
    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
