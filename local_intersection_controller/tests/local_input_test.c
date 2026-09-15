#include "../src/local_process.h"

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static void test_console_commands(void) {
    uint16_t before = state.status_sequence;
    CHECK(!valid_input(""));
    CHECK(!valid_input("nn"));
    CHECK(!valid_input("send-central"));
    CHECK(execute_command("invalid") == -1);
    CHECK(state.status_sequence == before);
    CHECK(valid_input("n"));
    CHECK(execute_command("n") == 0);
    CHECK(state.ped_ns_request == 1);
    CHECK(state.status_sequence != before);
}

static void test_coordination_same_phase(void) {
    test_message_t message;
    reply_t reply;
    coordination_command_msg_t command = {0};
    init_message(&message, MSG_COORDINATION_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    command.intersection_id = I1;
    command.mode = MODE_FIXED;
    command.phase = state.phase;
    command.cycle_offset_sec = 5;
    command.command_id = 1;
    memcpy(message.data, &command, sizeof(command));
    CHECK(local_dispatch_message(&message, &reply) == 0);
    CHECK(reply.status == 0);
    CHECK(state.coordination_pending == 1);
    CHECK(state.coordination_offset_sec == 5);
    CHECK(state.last_applied_command_id == 1);
}

static void test_simulation_command_telemetry(void) {
    test_message_t message;
    reply_t reply;
    init_message(&message, MSG_TEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
    strcpy(message.data, "SIM1 0 9 STOP");
    CHECK(local_dispatch_message(&message, &reply) == 0);
    CHECK(reply.status == 0);
    CHECK(reply.command_id == 9);
    CHECK(state.last_applied_command_id == 9);
    CHECK(state.sim_running == 0);
}

static void test_sensor_bounds(void) {
    test_message_t message;
    reply_t reply;
    sensor_msg_t sensor = {I1, DIR_NS, MAX_SENSOR_CARS, 0};
    init_message(&message, MSG_SENSOR_UPDATE, CONTROLLER_LOCAL, CONTROLLER_LOCAL);
    memcpy(message.data, &sensor, sizeof(sensor));
    CHECK(local_dispatch_message(&message, &reply) == 0);
    CHECK(reply.status == 0);
    CHECK(state.sensor_ns_count == MAX_SENSOR_CARS);
    sensor.car_count = MAX_SENSOR_CARS + 1;
    memcpy(message.data, &sensor, sizeof(sensor));
    CHECK(local_dispatch_message(&message, &reply) == 0);
    CHECK(reply.status == -1);
    CHECK(state.sensor_ns_count == MAX_SENSOR_CARS);
}

static void test_view_selection(void) {
    int selected = INTERSECTION_ALL;
    CHECK(local_ui_parse_view("view I3", &selected));
    CHECK(selected == I3);
    CHECK(local_ui_parse_view("view i6", &selected));
    CHECK(selected == I6);
    CHECK(!local_ui_parse_view("view I7", &selected));
    CHECK(!local_ui_parse_view("view I30", &selected));
    CHECK(!local_ui_parse_view("view I3 extra", &selected));
    CHECK(!local_ui_parse_view("n", &selected));
    CHECK(!local_ui_parse_view("", &selected));
    CHECK(selected == I6);
    CHECK(local_ui_parse_view("view all", &selected));
    CHECK(selected == INTERSECTION_ALL);
    CHECK(local_ui_parse_view("view I1", &selected));
    CHECK(selected == I1);
}

static void test_vehicle_countdown(void) {
    status_msg_t s = {0};
    s.phase = PHASE_EW_GREEN;
    s.ns_state = LIGHT_RED;
    s.ew_state = LIGHT_GREEN;
    s.time_remaining = 14;
    CHECK(local_vehicle_seconds(&s, DIR_NS) == 16);
    CHECK(local_vehicle_seconds(&s, DIR_EW) == 14);
    s.phase = PHASE_EW_YELLOW;
    s.ew_state = LIGHT_YELLOW;
    s.time_remaining = 2;
    CHECK(local_vehicle_seconds(&s, DIR_NS) == 2);
    CHECK(local_vehicle_seconds(&s, DIR_EW) == 2);
    s.phase = PHASE_NS_GREEN;
    s.ns_state = LIGHT_GREEN;
    s.ew_state = LIGHT_RED;
    s.time_remaining = 20;
    CHECK(local_vehicle_seconds(&s, DIR_NS) == 20);
    CHECK(local_vehicle_seconds(&s, DIR_EW) == 22);
    s.coordination_pending = 1;
    CHECK(local_vehicle_seconds(&s, DIR_EW) == -1);
    CHECK(local_vehicle_seconds(&s, DIR_NS) == 20);
    s.railway_preempt = 1;
    CHECK(local_vehicle_seconds(&s, DIR_NS) == -1);
    s.railway_preempt = 0;
    s.mode = MODE_FAILSAFE;
    CHECK(local_vehicle_seconds(&s, DIR_NS) == -1);
}

static void test_arrival_directions(void) {
    unsigned seed;
    int seen[3] = {0};
    reset_demo_inputs_locked();
    state.sim_running = 1;
    state.manual_sensor_override = 0;
    state.phase = PHASE_NS_GREEN;
    state.train_pending = state.train_active = state.train_recovery_remaining = 0;
    for (seed = 1; seed <= 100; ++seed) {
        int expected;
        srand(seed);
        expected = rand() % 3;
        srand(seed);
        state.sensor_ns_count = state.sensor_ew_count = 0;
        state.next_car_in_seconds = 1;
        update_vehicle_counts_locked();
        CHECK(state.sensor_ns_count == (expected == 0 || expected == 2));
        CHECK(state.sensor_ew_count == (expected == 1 || expected == 2));
        CHECK(state.next_car_in_seconds > 0);
        seen[expected] = 1;
    }
    CHECK(seen[0] && seen[1] && seen[2]);
    state.train_active = 1;
    state.sensor_ns_count = state.sensor_ew_count = MAX_SENSOR_CARS;
    state.next_car_in_seconds = 1;
    update_vehicle_counts_locked();
    CHECK(state.sensor_ns_count == MAX_SENSOR_CARS);
    CHECK(state.sensor_ew_count == MAX_SENSOR_CARS);
    state.manual_sensor_override = 1;
    state.sensor_ns_count = state.sensor_ew_count = 0;
    state.next_car_in_seconds = 1;
    update_vehicle_counts_locked();
    CHECK(state.sensor_ns_count == 0 && state.sensor_ew_count == 0);
    reset_demo_inputs_locked();
}

int main(void) {
    local_state_init(CONN_MODE_LOCAL, I1, LOCAL_SERVICE_NAME);
    test_console_commands();
    test_coordination_same_phase();
    test_simulation_command_telemetry();
    test_sensor_bounds();
    test_view_selection();
    test_vehicle_countdown();
    test_arrival_directions();
    local_state_destroy();
    printf("LOCAL_INPUT_TEST %s checks=%u failures=%u\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
