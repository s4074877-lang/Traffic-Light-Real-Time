#ifndef LOCAL_PROCESS_H
#define LOCAL_PROCESS_H

#include "local_controller.h"
#include <signal.h>

#define LOCAL_TICK_NS        1000000000ULL
#define LOCAL_POLL_NS         100000000ULL
#define LOCAL_IPC_TIMEOUT_NS  250000000ULL
#define LOCAL_LINK_TIMEOUT_NS (HEARTBEAT_MISS_LIMIT * LOCAL_TICK_NS)

/* Private, same-node protocol. No pointers or synchronization objects cross IPC. */
#define CORE_REQUEST 0x70
enum { CORE_SNAPSHOT = 1, CORE_FRAME, CORE_INPUT, CORE_LINKS };
typedef struct {
    uint16_t type;
    uint16_t operation;
    test_message_t frame;
    char command[64];
    int central_up;
    int train_up;
    uint64_t delivered_event;
} core_request_t;

typedef struct {
    reply_t result;
    status_msg_t status;
    fault_msg_t fault;
    uint16_t fault_sequence;
    uint64_t event_id;
    status_msg_t event_status;
    char service_name[LOCAL_SERVICE_NAME_MAX];
    int global_mode;
    int central_connected;
    int train_connected;
    char last_recv_central[32];
    char last_recv_train[32];
    int initial_phase;
    int ns_green_sec;
    int ew_green_sec;
    int sim_seconds;
} core_reply_t;


extern volatile sig_atomic_t local_stopping;
uint64_t monotonic_ns(void);
name_attach_t *attach_service(const char *name, int global);
int open_service(const char *name, int global);
int exchange(int *coid, const void *request, size_t length,
             void *reply, size_t reply_length);
int core_call(int *coid, const char *name, core_request_t *request,
              core_reply_t *reply);
int receive_application(int chid, void *buffer, size_t capacity,
                        struct _msg_info *info);
int valid_input(const char *command);
int run_core(const char *name);
int run_comm(const char *service, const char *core_name, connection_mode_t mode);
int run_display(const char *core_name, int display_all);
int run_input(const char *core_name);
void local_print_heading(int selected);
void local_print_panel(const core_reply_t *reply, int compact);
void local_print_commands(void);
int local_ui_parse_view(const char *command, int *selected);
int run_ui(const char *core_name, uint8_t intersection_id, int show_all);
int run_launcher(connection_mode_t mode);

#endif
