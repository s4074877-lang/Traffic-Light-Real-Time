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

    printf("monitor: %u checks passed\n", checks);
    return 0;
}
