#include "local_process.h"

#include <sys/iomsg.h>

volatile sig_atomic_t local_stopping;

static void stop_process(int signo) {
    (void)signo;
    local_stopping = 1;
}

uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * LOCAL_TICK_NS + now.tv_nsec;
}

static int timeout_send(void) {
    uint64_t timeout = LOCAL_IPC_TIMEOUT_NS;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                        NULL, &timeout, NULL);
}

name_attach_t *attach_service(const char *name, int global) {
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    dispatch_t *dispatch;
    name_attach_t *attach;
    (void)global; // Always use local namespace now
    if (chid == -1) return NULL;
    dispatch = dispatch_create_channel(chid, 0);
    if (!dispatch) {
        ChannelDestroy(chid);
        return NULL;
    }
    // Always register in local namespace
    // For global mode, other VMs connect via /net/{this_vm}/dev/name/local/{service}
    attach = name_attach(dispatch, name, 0);
    if (!attach) dispatch_destroy(dispatch);
    return attach;
}

int open_service(const char *name, int global) {
    int coid;
    char path[256];
    if (timeout_send() == -1) return -1;
    if (global) {
        // Global mode: connect via /net/{vm}/dev/name/local/{service}
        const char *remote_vm = NULL;
        if (strcmp(name, CENTRAL_SERVICE_NAME) == 0) {
            remote_vm = VM3_CENTRAL_NAME;
        } else if (strcmp(name, TRAIN_SERVICE_NAME) == 0) {
            remote_vm = VM2_TRAIN_NAME;
        }
        if (remote_vm) {
            snprintf(path, sizeof(path), "/net/%s/dev/name/local/%s", remote_vm, name);
            coid = name_open(path, 0);
        } else {
            // Unknown service, try local
            coid = name_open(name, 0);
        }
    } else {
        // Local mode: connect via /dev/name/local/
        coid = name_open(name, 0);
    }
    TimerTimeout(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);
    return coid;
}

int exchange(int *coid, const void *request, size_t length,
                    void *reply, size_t reply_length) {
    int result;
    if (*coid == -1 || timeout_send() == -1) return -1;
    result = MsgSend(*coid, request, length, reply, reply_length);
    TimerTimeout(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);
    if (result != 0) {
        name_close(*coid);
        *coid = -1;
        return -1;
    }
    return 0;
}

int core_call(int *coid, const char *name, core_request_t *request,
                     core_reply_t *reply) {
    if (*coid == -1) *coid = open_service(name, 0);
    request->type = CORE_REQUEST;
    memset(reply, 0, sizeof(*reply));
    reply->result.status = -1;
    return exchange(coid, request, sizeof(*request), reply, sizeof(*reply));
}

/* Handle name_open handshakes and disconnect pulses before application frames. */
int receive_application(int chid, void *buffer, size_t capacity,
                               struct _msg_info *info) {
    uint64_t timeout = LOCAL_POLL_NS;
    int rcvid;
    uint16_t type;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                     NULL, &timeout, NULL) == -1) return -1;
    memset(buffer, 0, capacity);
    rcvid = MsgReceive(chid, buffer, capacity, info);
    if (rcvid <= 0) {
        if (rcvid == 0) {
            struct _pulse *pulse = buffer;
            if (pulse->code == _PULSE_CODE_DISCONNECT) ConnectDetach(pulse->scoid);
        }
        return -1;
    }
    if (info->msglen < (int)sizeof(type)) {
        MsgError(rcvid, EINVAL);
        return -1;
    }
    memcpy(&type, buffer, sizeof(type));
    if (type == _IO_CONNECT) {
        MsgReply(rcvid, 0, NULL, 0);
        return -1;
    }
    if (type >= _IO_BASE && type <= _IO_MAX) {
        MsgError(rcvid, ENOSYS);
        return -1;
    }
    if (info->srcmsglen != info->msglen || info->msglen > (int)capacity) {
        MsgError(rcvid, EMSGSIZE);
        return -1;
    }
    return rcvid;
}

int local_process_run(const char *role, connection_mode_t mode,
                      uint8_t intersection_id, const char *service_name, int display_all) {
    char core_name[LOCAL_SERVICE_NAME_MAX + 8];
    struct sigaction action;
    int result;
    snprintf(core_name, sizeof(core_name), "%s_core", service_name);
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_process;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    if (strcmp(role, "launch") == 0) return run_launcher(mode);
    if (strcmp(role, "core") == 0) {
        local_state_init(mode, intersection_id, service_name);
        result = run_core(core_name);
        local_state_destroy();
        return result;
    }
    if (strcmp(role, "comm") == 0) return run_comm(service_name, core_name, mode);
    if (strcmp(role, "io") == 0) return run_input(core_name);
    if (strcmp(role, "display") == 0) return run_display(core_name, display_all);
    if (strcmp(role, "ui") == 0) return run_ui(core_name, intersection_id, display_all);
    fprintf(stderr, "Role must be core, comm, io, display or ui\n");
    return EXIT_FAILURE;
}
