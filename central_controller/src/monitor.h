#ifndef CENTRAL_MONITOR_H
#define CENTRAL_MONITOR_H

#include "../../common/protocol.h"

#define STATUS_STALE_SEC 15
#define CENTRAL_COMMAND_HISTORY 64
#define CENTRAL_EVENT_HISTORY 64
#define CENTRAL_COMMAND_QUEUE 32
#define NANOSECONDS_PER_SEC UINT64_C(1000000000)

typedef struct {
    int connected;
    int seen;
    int healthy;
    uint64_t last_heartbeat;
} peer_status_t;

typedef struct {
    int valid;
    int synchronized;
    int heartbeat_seen;
    int healthy;
    uint64_t last_heartbeat;
    uint32_t session_id;
    uint32_t sequence;
    uint64_t received_at;
    status_msg_t status;
} intersection_status_t;

typedef struct {
    int valid;
    int synchronized;
    uint32_t session_id;
    uint32_t sequence;
    uint64_t received_at;
    railway_status_msg_t status;
} crossing_status_t;

typedef enum {
    REQUEST_QUEUED,
    REQUEST_SENDING,
    REQUEST_ACCEPTED,
    REQUEST_APPLIED,
    REQUEST_REJECTED,
    REQUEST_UNCERTAIN,
    REQUEST_EXPIRED
} request_state_t;

typedef struct {
    uint16_t id;
    uint16_t type;
    uint8_t target;
    request_state_t state;
} command_record_t;

typedef struct {
    uint32_t session_id;
    peer_status_t peers[2];
    intersection_status_t intersections[NUM_INTERSECTIONS];
    crossing_status_t crossings[NUM_CROSSINGS];
    command_record_t commands[CENTRAL_COMMAND_HISTORY];
    unsigned command_count;
    unsigned command_next;
} monitor_t;

int monitor_peer_index(controller_type_t source);
void monitor_heartbeat(monitor_t *monitor, controller_type_t source, int healthy, uint64_t now);
void monitor_device_heartbeat(monitor_t *monitor, unsigned id, int healthy, uint64_t now);
int monitor_peer_online(const monitor_t *monitor, controller_type_t source, uint64_t now);
void monitor_disconnect(monitor_t *monitor, controller_type_t source);
int monitor_status(monitor_t *monitor, const status_msg_t *status,
                   uint32_t session, uint32_t sequence, uint64_t now, int snapshot);
int monitor_railway_status(monitor_t *monitor, const railway_status_msg_t *status,
                           uint32_t session, uint32_t sequence, uint64_t now, int snapshot);
int monitor_can_command(const monitor_t *monitor, unsigned target, uint64_t now);
command_record_t *monitor_find_command(monitor_t *monitor, uint16_t id);
int monitor_add_command(monitor_t *monitor, uint16_t id, uint16_t type, uint8_t target);

#endif
