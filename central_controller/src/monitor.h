#ifndef CENTRAL_MONITOR_H
#define CENTRAL_MONITOR_H

#include "../../common/protocol.h"

#define CENTRAL_NSEC UINT64_C(1000000000)
#define CENTRAL_STATUS_STALE_SEC 60

typedef struct {
    int connected;
    int seen;
    uint64_t last_seen;
} central_peer_status_t;

typedef struct {
    int valid;
    int synchronized;
    uint64_t received_at;
    status_msg_t status;
} central_intersection_status_t;

typedef struct {
    int valid;
    int synchronized;
    uint64_t received_at;
    railway_status_msg_t status;
} central_crossing_status_t;

typedef struct {
    central_peer_status_t peers[2];
    central_intersection_status_t intersections[NUM_INTERSECTIONS];
    central_crossing_status_t crossings[NUM_CROSSINGS];
} central_monitor_t;

int central_peer_index(controller_type_t source);
int central_peer_online(const central_monitor_t *monitor, controller_type_t source, uint64_t now);
int central_observe_peer(central_monitor_t *monitor, controller_type_t source, uint64_t now);
void central_peer_disconnected(central_monitor_t *monitor, controller_type_t source);
int central_monitor_status(central_monitor_t *monitor, const status_msg_t *status, uint64_t now);
int central_monitor_railway(central_monitor_t *monitor, const railway_status_msg_t *status, uint64_t now);
int central_can_command(const central_monitor_t *monitor, unsigned target, uint64_t now);

#endif
