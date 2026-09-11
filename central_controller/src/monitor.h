#ifndef CENTRAL_MONITOR_H
#define CENTRAL_MONITOR_H

#include "../../common/protocol.h"

#define CENTRAL_NSEC UINT64_C(1000000000)
#define CENTRAL_STATUS_STALE_SEC 5

typedef struct {
    int seen;
    int healthy;
    uint64_t received_at;
} central_reported_health_t;

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
    uint64_t status_max_age_ns;
    central_peer_status_t peers[2];
    central_reported_health_t health[2][NUM_INTERSECTIONS];
    central_intersection_status_t intersections[NUM_INTERSECTIONS];
    central_crossing_status_t crossings[NUM_CROSSINGS];
} central_monitor_t;

/* A zero age selects the default. The application validates its runtime range. */
void central_monitor_init(central_monitor_t *monitor, unsigned max_age_seconds);
/* Zero-initialized monitors also use the default age. */
uint64_t central_monitor_status_max_age_ns(const central_monitor_t *monitor);
/* Returns -1 for unknown, 0 for reported degraded, or 1 for reported healthy. */
int central_monitor_health(const central_monitor_t *monitor, controller_type_t source,
                           unsigned sender_id);
/* Accepts contact separately from reported health. An all-zero heartbeat is the
 * legacy empty payload and leaves health unchanged. Typed degraded I1/P1
 * reports need a nonzero sequence to distinguish them from that payload.
 * Reported health survives link loss until an explicit heartbeat replaces it. */
int central_monitor_heartbeat(central_monitor_t *monitor, controller_type_t source,
                              const heartbeat_msg_t *heartbeat, uint64_t now);
int central_peer_index(controller_type_t source);
int central_peer_online(const central_monitor_t *monitor, controller_type_t source, uint64_t now);
int central_observe_peer(central_monitor_t *monitor, controller_type_t source, uint64_t now);
void central_peer_disconnected(central_monitor_t *monitor, controller_type_t source);
int central_monitor_status(central_monitor_t *monitor, const status_msg_t *status, uint64_t now);
int central_monitor_railway(central_monitor_t *monitor, const railway_status_msg_t *status, uint64_t now);
int central_can_command(const central_monitor_t *monitor, unsigned target, uint64_t now);

#endif
