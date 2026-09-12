#include <stdio.h>
#include <string.h>
#include "../src/monitor.h"

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int check_configurable_freshness(void) {
    static const unsigned ages[] = {1, 2, CENTRAL_STATUS_STALE_SEC, 60};
    central_monitor_t monitor = {0};
    status_msg_t status = {0};
    uint64_t start = 200 * CENTRAL_NSEC;
    unsigned i, second;

    CHECK(central_monitor_status_max_age_ns(&monitor) ==
          CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC);
    central_monitor_init(&monitor, 0);
    CHECK(monitor.status_max_age_ns == CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC);
    for (i = 0; i < sizeof(ages) / sizeof(ages[0]); ++i) {
        memset(&monitor, 0xa5, sizeof(monitor));
        central_monitor_init(&monitor, ages[i]);
        CHECK(central_monitor_status_max_age_ns(&monitor) == ages[i] * CENTRAL_NSEC);
        CHECK(!monitor.peers[0].connected && !monitor.intersections[I1].valid);
        CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == -1);
        monitor.peers[0].connected = 1;
        CHECK(central_monitor_status(&monitor, &status, start));
        CHECK(!central_can_command(&monitor, I1, start - 1));
        for (second = 1; second < ages[i]; ++second)
            central_observe_peer(&monitor, CONTROLLER_LOCAL, start + second * CENTRAL_NSEC);
        CHECK(central_can_command(&monitor, I1, start + ages[i] * CENTRAL_NSEC - 1));
        central_observe_peer(&monitor, CONTROLLER_LOCAL, start + ages[i] * CENTRAL_NSEC);
        CHECK(central_peer_online(&monitor, CONTROLLER_LOCAL, start + ages[i] * CENTRAL_NSEC));
        CHECK(!central_can_command(&monitor, I1, start + ages[i] * CENTRAL_NSEC));
        CHECK(central_monitor_status(&monitor, &status, start + ages[i] * CENTRAL_NSEC));
        CHECK(central_can_command(&monitor, I1, start + ages[i] * CENTRAL_NSEC));
    }
    return 0;
}

static int check_reported_health(void) {
    central_monitor_t monitor, before;
    heartbeat_msg_t heartbeat = {0}, legacy = {0};
    status_msg_t status = {0};
    uint64_t now = 300 * CENTRAL_NSEC;
    uint64_t degraded_at;
    unsigned i;

    central_monitor_init(&monitor, 0);
    monitor.peers[0].connected = 1;
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        status.intersection_id = i;
        CHECK(central_monitor_status(&monitor, &status, now));
    }
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &legacy, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == -1);
    CHECK(central_can_command(&monitor, I1, now));

    now += CENTRAL_NSEC;
    heartbeat.sender_id = I1;
    heartbeat.healthy = 0;
    heartbeat.sequence = 1;
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    degraded_at = now;
    CHECK(central_peer_online(&monitor, CONTROLLER_LOCAL, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == 0);
    CHECK(!central_can_command(&monitor, I1, now));
    CHECK(central_can_command(&monitor, I2, now));

    now += CENTRAL_NSEC;
    central_observe_peer(&monitor, CONTROLLER_LOCAL, now); /* Probe ACK is contact only. */
    status.intersection_id = I1;
    CHECK(central_monitor_status(&monitor, &status, now));
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &legacy, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == 0);
    CHECK(monitor.health[0][I1].received_at == degraded_at);
    CHECK(!central_can_command(&monitor, I1, now));
    heartbeat.sender_id = I2;
    heartbeat.healthy = 1;
    heartbeat.sequence = 0;
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I2) == 1);
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == 0);
    CHECK(central_can_command(&monitor, I2, now));

    heartbeat.sender_id = P1;
    heartbeat.healthy = 0;
    heartbeat.sequence = 1;
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_TRAIN, &heartbeat, now));
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_TRAIN, &legacy, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_TRAIN, P1) == 0);
    CHECK(central_can_command(&monitor, I2, now));
    heartbeat.sender_id = I6;
    heartbeat.sequence = 0; /* Nonzero IDs remain unambiguous even with sequence zero. */
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I6) == 0);
    CHECK(!central_can_command(&monitor, I6, now));

    before = monitor;
    CHECK(!central_monitor_heartbeat(&monitor, CONTROLLER_CENTRAL, &heartbeat, now));
    CHECK(!central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, NULL, now));
    CHECK(!central_monitor_heartbeat(NULL, CONTROLLER_LOCAL, &heartbeat, now));
    heartbeat.sender_id = NUM_INTERSECTIONS;
    CHECK(!central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    heartbeat.sender_id = NUM_CROSSINGS;
    CHECK(!central_monitor_heartbeat(&monitor, CONTROLLER_TRAIN, &heartbeat, now));
    heartbeat.sender_id = I1;
    heartbeat.healthy = 2;
    CHECK(!central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    CHECK(memcmp(&monitor, &before, sizeof(monitor)) == 0);
    CHECK(central_monitor_health(&monitor, CONTROLLER_CENTRAL, 0) == -1);
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, NUM_INTERSECTIONS) == -1);
    CHECK(central_monitor_health(&monitor, CONTROLLER_TRAIN, NUM_CROSSINGS) == -1);

    central_peer_disconnected(&monitor, CONTROLLER_LOCAL);
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == 0);
    monitor.peers[0].connected = 1;
    CHECK(central_observe_peer(&monitor, CONTROLLER_LOCAL, now));
    CHECK(central_monitor_status(&monitor, &status, now));
    CHECK(!central_can_command(&monitor, I1, now));
    heartbeat.healthy = 1;
    heartbeat.sequence = 2;
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I1) == 1);
    CHECK(central_can_command(&monitor, I1, now));
    CHECK(central_monitor_health(&monitor, CONTROLLER_LOCAL, I6) == 0);

    now += HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC;
    CHECK(!central_peer_online(&monitor, CONTROLLER_LOCAL, now));
    CHECK(central_monitor_heartbeat(&monitor, CONTROLLER_LOCAL, &heartbeat, now));
    CHECK(central_peer_online(&monitor, CONTROLLER_LOCAL, now));
    CHECK(!monitor.intersections[I1].synchronized);
    CHECK(!central_can_command(&monitor, I1, now)); /* Healthy contact is not a fresh status. */
    CHECK(central_monitor_status(&monitor, &status, now));
    CHECK(central_can_command(&monitor, I1, now));
    return 0;
}

int main(void) {
    central_monitor_t monitor = {0};
    central_monitor_t before;
    status_msg_t status = {0};
    railway_status_msg_t railway = {0};
    uint64_t start = 100 * CENTRAL_NSEC;
    unsigned i;

    CHECK(central_peer_index(CONTROLLER_LOCAL) == 0);
    CHECK(central_peer_index(CONTROLLER_TRAIN) == 1);
    CHECK(central_peer_index(CONTROLLER_CENTRAL) == -1);
    CHECK(!central_peer_online(&monitor, CONTROLLER_LOCAL, start));
    CHECK(!central_can_command(&monitor, I1, start));
    CHECK(!central_can_command(&monitor, INTERSECTION_ALL, start));

    monitor.peers[0].connected = 1;
    status.ns_state = LIGHT_GREEN;
    status.ew_state = LIGHT_RED;
    status.time_remaining = 20;
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        status.intersection_id = i;
        status.time_remaining = 20 + i;
        CHECK(central_monitor_status(&monitor, &status, start));
    }
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        CHECK(monitor.intersections[i].status.intersection_id == i);
        CHECK(monitor.intersections[i].status.time_remaining == 20 + i);
        CHECK(central_can_command(&monitor, i, start));
    }
    CHECK(!monitor.crossings[P1].valid);
    CHECK(!central_peer_online(&monitor, CONTROLLER_TRAIN, start));

    before = monitor;
    status.intersection_id = NUM_INTERSECTIONS;
    CHECK(!central_monitor_status(&monitor, &status, start));
    CHECK(memcmp(&before, &monitor, sizeof(monitor)) == 0);
    status.intersection_id = I1;
#define INVALID_STATUS(field, value) do { \
    status_msg_t invalid = status; \
    invalid.field = value; \
    CHECK(!central_monitor_status(&monitor, &invalid, start)); \
    CHECK(memcmp(&before, &monitor, sizeof(monitor)) == 0); \
} while (0)
    INVALID_STATUS(mode, MODE_FAILSAFE + 1);
    INVALID_STATUS(phase, PHASE_RAILWAY_HOLD + 1);
    INVALID_STATUS(ns_state, LIGHT_GREEN + 1);
    INVALID_STATUS(ew_state, LIGHT_GREEN + 1);
    INVALID_STATUS(pedestrian_ns, 2);
    INVALID_STATUS(pedestrian_ew, 2);
    INVALID_STATUS(railway_preempt, 2);

    CHECK(central_peer_online(&monitor, CONTROLLER_LOCAL,
                              start + HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC - 1));
    CHECK(!central_peer_online(&monitor, CONTROLLER_LOCAL,
                               start + HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC));
    CHECK(!central_peer_online(&monitor, CONTROLLER_LOCAL, start - 1));
    CHECK(!central_can_command(&monitor, I1,
                               start + HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC));
    CHECK(central_observe_peer(&monitor, CONTROLLER_LOCAL,
                               start + HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC));
    CHECK(monitor.peers[0].connected);
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        CHECK(monitor.intersections[i].valid);
        CHECK(!monitor.intersections[i].synchronized);
        CHECK(!central_can_command(&monitor, i,
                                    start + HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC));
    }
    start += HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC;
    status.intersection_id = I2;
    CHECK(central_monitor_status(&monitor, &status, start));
    CHECK(central_can_command(&monitor, I2, start));
    CHECK(!central_can_command(&monitor, I1, start));

    for (i = 1; i < CENTRAL_STATUS_STALE_SEC; ++i)
        central_observe_peer(&monitor, CONTROLLER_LOCAL, start + i * CENTRAL_NSEC);
    CHECK(central_can_command(&monitor, I2,
                              start + CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC - 1));
    central_observe_peer(&monitor, CONTROLLER_LOCAL,
                          start + CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC);
    CHECK(central_peer_online(&monitor, CONTROLLER_LOCAL,
                              start + CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC));
    CHECK(!central_can_command(&monitor, I2,
                               start + CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC));
    start += CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC;
    CHECK(central_monitor_status(&monitor, &status, start));
    CHECK(central_can_command(&monitor, I2, start));

    for (i = 0; i < NUM_CROSSINGS; ++i) {
        railway.crossing_id = i;
        railway.train_state = i;
        railway.gate_state = i;
        CHECK(central_monitor_railway(&monitor, &railway, start));
    }
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        CHECK(monitor.crossings[i].status.crossing_id == i);
        CHECK(monitor.crossings[i].status.train_state == i);
    }
    before = monitor;
#define INVALID_RAILWAY(field, value) do { \
    railway_status_msg_t invalid = railway; \
    invalid.field = value; \
    CHECK(!central_monitor_railway(&monitor, &invalid, start)); \
    CHECK(memcmp(&before, &monitor, sizeof(monitor)) == 0); \
} while (0)
    INVALID_RAILWAY(crossing_id, NUM_CROSSINGS);
    INVALID_RAILWAY(train_state, TRAIN_CLEAR + 1);
    INVALID_RAILWAY(gate_state, GATE_FAULT + 1);
    INVALID_RAILWAY(fault, FAULT_NOT_WORKING + 1);

    central_peer_disconnected(&monitor, CONTROLLER_TRAIN);
    CHECK(!central_peer_online(&monitor, CONTROLLER_TRAIN, start));
    CHECK(central_can_command(&monitor, I2, start));
    for (i = 0; i < NUM_CROSSINGS; ++i) {
        CHECK(monitor.crossings[i].valid);
        CHECK(!monitor.crossings[i].synchronized);
        CHECK(monitor.crossings[i].status.train_state == i);
    }
    CHECK(central_observe_peer(&monitor, CONTROLLER_TRAIN, start));
    CHECK(!monitor.crossings[P1].synchronized);
    railway.crossing_id = P3;
    CHECK(central_monitor_railway(&monitor, &railway, start));
    CHECK(monitor.crossings[P3].synchronized);
    CHECK(!monitor.crossings[P1].synchronized);

    central_peer_disconnected(&monitor, CONTROLLER_LOCAL);
    CHECK(monitor.intersections[I2].valid);
    CHECK(!monitor.intersections[I2].synchronized);
    CHECK(!central_can_command(&monitor, I2, start));
    CHECK(monitor.crossings[P3].synchronized);
    before = monitor;
    central_peer_disconnected(&monitor, CONTROLLER_CENTRAL);
    CHECK(!central_observe_peer(&monitor, CONTROLLER_CENTRAL, start));
    CHECK(memcmp(&before, &monitor, sizeof(monitor)) == 0);

    CHECK(check_configurable_freshness() == 0);
    CHECK(check_reported_health() == 0);
    printf("monitor: %u checks passed\n", checks);
    return 0;
}
