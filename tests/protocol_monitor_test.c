#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../central_controller/src/monitor.h"

#define SEC(n) ((uint64_t)(n) * NANOSECONDS_PER_SEC)

static status_msg_t intersection(unsigned id) {
    status_msg_t status;
    memset(&status, 0, sizeof(status));
    status.intersection_id = (uint8_t)id;
    status.mode = MODE_FIXED;
    status.phase = PHASE_NS_GREEN;
    status.ns_state = LIGHT_GREEN;
    status.ew_state = LIGHT_RED;
    status.time_remaining = GREEN_BASE_SEC;
    return status;
}

static any_msg_t mode_command(void) {
    any_msg_t msg;
    protocol_init_message(&msg, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    msg.payload.mode_cmd.intersection_id = I1;
    msg.payload.mode_cmd.new_mode = MODE_SENSOR;
    msg.payload.mode_cmd.action = CMD_SET_MODE;
    msg.payload.mode_cmd.priority = CMD_PRIO_OPERATOR;
    msg.payload.mode_cmd.command_id = 42;
    return msg;
}

static int valid(const any_msg_t *msg) {
    return protocol_validate_message(msg, protocol_message_size(msg),
                                     (controller_type_t)msg->header.dst);
}

static void test_envelope(void) {
    any_msg_t original = mode_command();
    any_msg_t msg;
    size_t size = protocol_message_size(&original);
    size_t truncated;

    assert(valid(&original));
    assert(protocol_command_id(&original) == 42);
    assert(offsetof(any_msg_t, payload) >= sizeof(msg_header_t));
    for (truncated = 0; truncated < size; ++truncated)
        assert(!protocol_validate_message(&original, truncated, CONTROLLER_LOCAL));
    assert(!protocol_validate_message(&original, size + 1, CONTROLLER_LOCAL));
    assert(!protocol_validate_message(&original, sizeof(original), CONTROLLER_LOCAL));
    assert(!protocol_validate_message(&original, size, CONTROLLER_TRAIN));

    msg = original;
    msg.header.version = PROTOCOL_VERSION - 1;
    assert(!valid(&msg));
    msg = original;
    msg.header.payload_size--;
    assert(!valid(&msg));
    msg = original;
    msg.header.src = CONTROLLER_TRAIN;
    assert(!valid(&msg));
    msg.header.src = 0;
    assert(!valid(&msg));
    msg = original;
    msg.header.dst = CONTROLLER_TRAIN;
    assert(!valid(&msg));
    msg = original;
    msg.header.type = UINT16_MAX;
    assert(!valid(&msg));
}

static void test_command_validation(void) {
    any_msg_t original = mode_command();
    any_msg_t msg = original;
    msg.payload.mode_cmd.intersection_id = INTERSECTION_ALL;
    assert(valid(&msg));
    msg.payload.mode_cmd.intersection_id = NUM_INTERSECTIONS;
    assert(!valid(&msg));
    msg = original;
    msg.payload.mode_cmd.new_mode = MODE_RAILWAY;
    assert(!valid(&msg));
    msg = original;
    msg.payload.mode_cmd.action = CMD_REVERT + 1;
    assert(!valid(&msg));
    msg = original;
    msg.payload.mode_cmd.command_id = 0;
    assert(!valid(&msg));
    msg = original;
    msg.payload.mode_cmd.priority = 0;
    assert(!valid(&msg));
    msg = original;
    msg.payload.mode_cmd.action = CMD_TEMPORARY;
    assert(!valid(&msg));
    msg.payload.mode_cmd.duration_sec = 20;
    assert(valid(&msg));
    msg.payload.mode_cmd.action = CMD_REVERT;
    assert(!valid(&msg));
    msg.payload.mode_cmd.duration_sec = 0;
    assert(valid(&msg));

    protocol_init_message(&msg, MSG_OVERRIDE_REQUEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    msg.payload.override_request.command_id = 7;
    msg.payload.override_request.phase = PHASE_EW_GREEN;
    msg.payload.override_request.action = CMD_TEMPORARY;
    msg.payload.override_request.duration_sec = 10;
    assert(valid(&msg));
    msg.payload.override_request.phase = PHASE_RAILWAY_HOLD;
    assert(!valid(&msg));
    msg.payload.override_request.phase = PHASE_NS_GREEN;
    msg.payload.override_request.action = CMD_SET_MODE;
    assert(!valid(&msg));

    protocol_init_message(&msg, MSG_COORDINATION_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    msg.payload.coordination.command_id = 8;
    msg.payload.coordination.phase = PHASE_NS_GREEN;
    msg.payload.coordination.cycle_offset_sec = NORMAL_CYCLE_SEC - 1;
    assert(valid(&msg));
    msg.payload.coordination.cycle_offset_sec = NORMAL_CYCLE_SEC;
    assert(!valid(&msg));
}

static void test_status_validation(void) {
    any_msg_t msg;
    protocol_init_message(&msg, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    msg.payload.status = intersection(I6);
    assert(valid(&msg));
    msg.payload.status.intersection_id = NUM_INTERSECTIONS;
    assert(!valid(&msg));
    msg.payload.status = intersection(I1);
    msg.payload.status.pedestrian_ns = 2;
    assert(!valid(&msg));
    msg.payload.status = intersection(I1);
    msg.payload.status.command_state = COMMAND_EXPIRED + 1;
    assert(!valid(&msg));

    protocol_init_message(&msg, MSG_RAILWAY_STATUS, CONTROLLER_TRAIN, CONTROLLER_CENTRAL);
    msg.payload.railway_status.crossing_id = P3;
    msg.payload.railway_status.train_state = TRAIN_AT_CROSSING;
    msg.payload.railway_status.gate_state = GATE_CLOSED;
    assert(valid(&msg));
    msg.payload.railway_status.crossing_id = NUM_CROSSINGS;
    assert(!valid(&msg));

    protocol_init_message(&msg, MSG_STATUS_REQUEST, CONTROLLER_CENTRAL, CONTROLLER_TRAIN);
    msg.payload.status_request.target_id = P3;
    msg.payload.status_request.request_id = 9;
    assert(valid(&msg));
    msg.payload.status_request.target_id = I6;
    assert(!valid(&msg));

    protocol_init_message(&msg, MSG_TRAIN_CLEAR, CONTROLLER_TRAIN, CONTROLLER_LOCAL);
    msg.payload.railway.intersection_id = I1;
    assert(valid(&msg));
    msg.payload.railway.active = 1;
    assert(!valid(&msg));
}

static void test_freshness_and_reconnect(void) {
    monitor_t monitor = {0};
    status_msg_t status = intersection(I1);
    unsigned tick;

    assert(!monitor_can_command(&monitor, I1, SEC(10)));
    monitor.peers[0].connected = 1;
    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(10));
    assert(monitor_status(&monitor, &status, 1, 1, SEC(10), 0));
    assert(!monitor_can_command(&monitor, I1, SEC(10)));
    assert(monitor_status(&monitor, &status, 1, 1, SEC(10), 1));
    assert(monitor_can_command(&monitor, I1, SEC(10)));
    assert(!monitor_can_command(&monitor, I2, SEC(10)));
    assert(!monitor_can_command(&monitor, NUM_INTERSECTIONS, SEC(10)));
    assert(monitor_peer_online(&monitor, CONTROLLER_LOCAL, SEC(13) - 1));
    assert(!monitor_peer_online(&monitor, CONTROLLER_LOCAL, SEC(13)));
    assert(!monitor_can_command(&monitor, I1, SEC(9)));

    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 0, SEC(11));
    assert(!monitor_can_command(&monitor, I1, SEC(11)));
    for (tick = 12; tick <= 24; tick += 2)
        monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(tick));
    assert(monitor_can_command(&monitor, I1, SEC(25) - 1));
    assert(!monitor_can_command(&monitor, I1, SEC(25)));

    monitor_disconnect(&monitor, CONTROLLER_LOCAL);
    monitor.peers[0].connected = 1;
    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(26));
    assert(monitor_status(&monitor, &status, 1, 2, SEC(26), 0));
    assert(!monitor_can_command(&monitor, I1, SEC(26)));
    assert(monitor_status(&monitor, &status, 1, 2, SEC(26), 1));
    assert(monitor_can_command(&monitor, I1, SEC(26)));

    assert(!monitor_status(&monitor, &status, 2, 1, SEC(27), 0));
    assert(!monitor_can_command(&monitor, I1, SEC(27)));
    assert(monitor_status(&monitor, &status, 2, 1, SEC(27), 1));
    assert(monitor_can_command(&monitor, I1, SEC(27)));

    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(31));
    assert(monitor_peer_online(&monitor, CONTROLLER_LOCAL, SEC(31)));
    assert(!monitor_can_command(&monitor, I1, SEC(31)));
    assert(monitor_status(&monitor, &status, 2, 2, SEC(31), 0));
    assert(!monitor_can_command(&monitor, I1, SEC(31)));
    assert(monitor_status(&monitor, &status, 2, 2, SEC(31), 1));
    assert(monitor_can_command(&monitor, I1, SEC(31)));
}

static void test_sequence_and_separation(void) {
    monitor_t monitor = {0};
    status_msg_t first = intersection(I1);
    status_msg_t second = intersection(I2);
    railway_status_msg_t railway = {P1, TRAIN_APPROACHING, GATE_CLOSING, FAULT_NONE};

    assert(monitor_status(&monitor, &first, 1, UINT32_MAX - 1, SEC(1), 1));
    assert(monitor_status(&monitor, &second, 1, 2, SEC(1), 1));
    first.time_remaining = 17;
    assert(monitor_status(&monitor, &first, 1, UINT32_MAX, SEC(2), 0));
    assert(monitor_status(&monitor, &first, 1, 0, SEC(3), 0));
    first.time_remaining = 99;
    assert(!monitor_status(&monitor, &first, 1, 0, SEC(4), 0));
    assert(!monitor_status(&monitor, &first, 1, UINT32_MAX, SEC(4), 0));
    assert(!monitor_status(&monitor, &first, 1, UINT32_C(0x80000000), SEC(4), 0));
    assert(monitor.intersections[I1].status.time_remaining == 17);
    assert(monitor.intersections[I1].received_at == SEC(3));
    assert(monitor.intersections[I2].status.time_remaining == GREEN_BASE_SEC);
    assert(monitor.intersections[I2].sequence == 2);

    assert(monitor_railway_status(&monitor, &railway, 1, UINT32_MAX, SEC(1), 1));
    railway.crossing_id = P2;
    assert(monitor_railway_status(&monitor, &railway, 1, 3, SEC(1), 1));
    railway.crossing_id = P1;
    railway.gate_state = GATE_CLOSED;
    assert(monitor_railway_status(&monitor, &railway, 1, 0, SEC(2), 0));
    assert(!monitor_railway_status(&monitor, &railway, 1, 0, SEC(3), 0));
    assert(!monitor_railway_status(&monitor, &railway, 1, UINT32_MAX, SEC(3), 1));
    assert(!monitor_railway_status(&monitor, &railway, 2, 1, SEC(3), 0));
    assert(monitor.crossings[P1].received_at == SEC(2));
    assert(monitor.crossings[P2].status.gate_state == GATE_CLOSING);
    assert(monitor_railway_status(&monitor, &railway, 2, 1, SEC(3), 1));
    assert(monitor.crossings[P1].synchronized);
    monitor_disconnect(&monitor, CONTROLLER_TRAIN);
    assert(monitor.crossings[P1].valid);
    assert(monitor.crossings[P1].status.gate_state == GATE_CLOSED);
    assert(!monitor.crossings[P1].synchronized);
    assert(!monitor.crossings[P2].synchronized);
    assert(monitor_railway_status(&monitor, &railway, 2, 2, SEC(4), 0));
    assert(!monitor.crossings[P1].synchronized);
    assert(monitor_railway_status(&monitor, &railway, 2, 2, SEC(4), 1));
    assert(monitor.crossings[P1].synchronized);
    assert(!monitor.crossings[P2].synchronized);
}

static void test_intersection_health(void) {
    monitor_t monitor = {0};
    status_msg_t first = intersection(I1);
    status_msg_t second = intersection(I2);
    monitor.peers[0].connected = 1;
    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(10));
    assert(monitor_status(&monitor, &first, 1, 1, SEC(10), 1));
    assert(monitor_status(&monitor, &second, 1, 1, SEC(10), 1));
    monitor_device_heartbeat(&monitor, I1, 0, SEC(10));
    monitor_device_heartbeat(&monitor, I2, 1, SEC(10));
    assert(!monitor_can_command(&monitor, I1, SEC(10)));
    assert(monitor_can_command(&monitor, I2, SEC(10)));
    monitor_device_heartbeat(&monitor, I1, 1, SEC(11));
    assert(monitor_can_command(&monitor, I1, SEC(11)));
    monitor_heartbeat(&monitor, CONTROLLER_LOCAL, 1, SEC(14));
    assert(!monitor_can_command(&monitor, I1, SEC(14)));
    monitor_device_heartbeat(&monitor, I1, 1, SEC(14));
    assert(!monitor_can_command(&monitor, I1, SEC(14)));
    assert(monitor_status(&monitor, &first, 1, 2, SEC(14), 0));
    assert(!monitor_can_command(&monitor, I1, SEC(14)));
    assert(monitor_status(&monitor, &first, 1, 2, SEC(14), 1));
    assert(monitor_can_command(&monitor, I1, SEC(14)));
    assert(!monitor_can_command(&monitor, I2, SEC(14)));
}

static void test_command_lifecycle(void) {
    monitor_t monitor = {0};
    status_msg_t status = intersection(I2);
    command_record_t *record;

    monitor.session_id = 100;
    assert(monitor_add_command(&monitor, 42, MSG_MODE_COMMAND, I1));
    assert(!monitor_add_command(&monitor, 42, MSG_MODE_COMMAND, I2));
    assert(!monitor_add_command(&monitor, 0, MSG_MODE_COMMAND, I1));
    assert(!monitor_add_command(&monitor, 43, MSG_MODE_COMMAND, NUM_INTERSECTIONS));
    record = monitor_find_command(&monitor, 42);
    assert(record && record->state == REQUEST_QUEUED);
    record->state = REQUEST_SENDING;
    status.command_id = 42;
    status.command_session_id = 100;
    status.command_state = COMMAND_APPLIED;
    assert(monitor_status(&monitor, &status, 1, 1, SEC(1), 1));
    assert(record->state == REQUEST_SENDING);

    status.intersection_id = I1;
    status.command_session_id = 99;
    assert(monitor_status(&monitor, &status, 1, 1, SEC(1), 1));
    assert(record->state == REQUEST_SENDING);
    status.command_session_id = 100;
    status.command_state = COMMAND_QUEUED;
    assert(monitor_status(&monitor, &status, 1, 1, SEC(1), 1));
    assert(record->state == REQUEST_ACCEPTED);
    status.command_state = COMMAND_APPLIED;
    assert(monitor_status(&monitor, &status, 1, 2, SEC(2), 0));
    assert(record->state == REQUEST_APPLIED);
    status.command_state = COMMAND_QUEUED;
    assert(monitor_status(&monitor, &status, 1, 3, SEC(3), 0));
    assert(record->state == REQUEST_APPLIED);
    status.command_state = COMMAND_REJECTED;
    assert(monitor_status(&monitor, &status, 1, 4, SEC(4), 0));
    assert(record->state == REQUEST_APPLIED);
    status.command_state = COMMAND_EXPIRED;
    assert(monitor_status(&monitor, &status, 1, 5, SEC(5), 0));
    assert(record->state == REQUEST_EXPIRED);
    status.command_state = COMMAND_APPLIED;
    assert(monitor_status(&monitor, &status, 1, 6, SEC(6), 0));
    assert(record->state == REQUEST_EXPIRED);
    status.command_state = COMMAND_REJECTED;
    assert(monitor_status(&monitor, &status, 1, 7, SEC(7), 0));
    assert(record->state == REQUEST_EXPIRED);

    assert(monitor_add_command(&monitor, 43, MSG_MODE_COMMAND, I2));
    record = monitor_find_command(&monitor, 43);
    record->state = REQUEST_ACCEPTED;
    monitor_disconnect(&monitor, CONTROLLER_TRAIN);
    assert(record->state == REQUEST_ACCEPTED);
    monitor_disconnect(&monitor, CONTROLLER_LOCAL);
    assert(record->state == REQUEST_UNCERTAIN);
    status.intersection_id = I2;
    status.command_id = 43;
    status.command_state = COMMAND_APPLIED;
    assert(monitor_status(&monitor, &status, 2, 1, SEC(6), 1));
    assert(record->state == REQUEST_APPLIED);
}

static void test_pending_history_retention(void) {
    monitor_t monitor = {0};
    unsigned id;
    for (id = 1; id <= CENTRAL_COMMAND_HISTORY; ++id)
        assert(monitor_add_command(&monitor, (uint16_t)id, MSG_MODE_COMMAND, I1));
    assert(!monitor_add_command(&monitor, 100, MSG_MODE_COMMAND, I1));
    monitor_find_command(&monitor, 1)->state = REQUEST_UNCERTAIN;
    assert(!monitor_add_command(&monitor, 100, MSG_MODE_COMMAND, I1));
    monitor_find_command(&monitor, 2)->state = REQUEST_REJECTED;
    assert(monitor_add_command(&monitor, 100, MSG_MODE_COMMAND, I2));
    assert(monitor_find_command(&monitor, 1)->state == REQUEST_UNCERTAIN);
    assert(!monitor_find_command(&monitor, 2));
    assert(monitor_find_command(&monitor, 100)->target == I2);
}

int main(void) {
    test_envelope();
    test_command_validation();
    test_status_validation();
    test_freshness_and_reconnect();
    test_sequence_and_separation();
    test_intersection_health();
    test_command_lifecycle();
    test_pending_history_retention();
    puts("Protocol and monitor tests passed.");
    return 0;
}
