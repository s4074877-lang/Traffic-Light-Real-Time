#include "../src/local_controller.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(status_msg_t) <= sizeof(((test_message_t *)0)->data),
               "status_msg_t must fit in test_message_t.data");
_Static_assert(sizeof(heartbeat_msg_t) <= sizeof(((test_message_t *)0)->data),
               "heartbeat_msg_t must fit in test_message_t.data");

static unsigned checks;
static unsigned failures;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        ++failures; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static void reset_core(void) {
    pthread_mutex_lock(&state.mutex);
    state.central_conn.connected = 0;
    state.train_conn.connected = 0;
    state.traffic_mode = MODE_FIXED;
    state.initial_phase = PHASE_NS_GREEN;
    state.phase = PHASE_NS_GREEN;
    state.phase_duration = GREEN_BASE_SEC;
    state.time_remaining = GREEN_BASE_SEC;
    state.ns_green_sec = GREEN_BASE_SEC;
    state.ew_green_sec = GREEN_BASE_SEC;
    state.ns_light = LIGHT_GREEN;
    state.ew_light = LIGHT_RED;
    state.sensor_ns_count = 0;
    state.sensor_ew_count = 0;
    state.ped_ns_request = 0;
    state.ped_ew_request = 0;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    state.sim_seconds = 6 * 3600;
    state.sim_running = 1;
    state.next_train_in_seconds = 0;
    state.next_train_direction = 1;
    state.next_ns_car_in_seconds = 0;
    state.next_ew_car_in_seconds = 0;
    state.train_direction = 0;
    state.train_pass_remaining = 0;
    state.train_waiting_for_clear = 0;
    state.manual_mode_override = 0;
    state.manual_sensor_override = 0;
    state.temp_mode_remaining = 0;
    state.temp_return_mode = MODE_FIXED;
    state.temp_return_manual_override = 0;
    state.coordination_pending = 0;
    state.coordination_phase = PHASE_NS_GREEN;
    state.coordination_offset_sec = 0;
    state.ped_extra_ns_green = 0;
    state.ped_extra_ew_green = 0;
    state.last_applied_command_id = 0;
    state.fault_active = 0;
    state.fault_pending = 0;
    state.fault_type = FAULT_NONE;
    state.fault_severity = SEV_LOW;
    state.fault_description[0] = '\0';
    state.train_pending = 0;
    state.train_active = 0;
    state.train_recovery_remaining = 0;
    mark_status_dirty_locked();
    pthread_mutex_unlock(&state.mutex);
}

static void test_status_telemetry(void) {
    test_message_t frame;
    status_msg_t status;
    uint16_t sequence;

    reset_core();
    pthread_mutex_lock(&state.mutex);
    state.sensor_ns_count = 6;
    state.sensor_ew_count = 2;
    state.ped_ns_request = 1;
    state.sim_running = 0;
    state.sim_seconds = 7 * 3600 + 30 * 60;
    state.train_pending = 1;
    state.train_pass_remaining = 4;
    state.last_applied_command_id = 4321;
    set_fault_locked(FAULT_SENSOR, SEV_MEDIUM, "sensor degraded");
    sequence = prepare_status_message_locked(&frame);
    pthread_mutex_unlock(&state.mutex);

    memset(&status, 0, sizeof(status));
    memcpy(&status, frame.data, sizeof(status));
    CHECK(frame.header.type == MSG_STATUS_UPDATE);
    CHECK(frame.header.src == CONTROLLER_LOCAL);
    CHECK(frame.header.dst == CONTROLLER_CENTRAL);
    CHECK(status.telemetry_version == STATUS_TELEMETRY_VERSION);
    CHECK(status.status_sequence == sequence);
    CHECK(status.last_command_id == 4321);
    CHECK(status.sensor_ns_count == 6);
    CHECK(status.sensor_ew_count == 2);
    CHECK(status.pedestrian_ns_request == 1);
    CHECK(status.sim_running == 0);
    CHECK(status.sim_minute_of_day == 450);
    CHECK(status.train_pending == 1);
    CHECK(status.fault_active == 1);
    CHECK(status.fault_type == FAULT_SENSOR);
    CHECK(status.fault_severity == SEV_MEDIUM);
}

static void test_typed_heartbeat_health(void) {
    test_message_t frame;
    heartbeat_msg_t heartbeat;
    uint16_t first_sequence;

    reset_core();
    pthread_mutex_lock(&state.mutex);
    first_sequence = prepare_heartbeat_message_locked(&frame);
    pthread_mutex_unlock(&state.mutex);
    memcpy(&heartbeat, frame.data, sizeof(heartbeat));
    CHECK(frame.header.type == MSG_HEARTBEAT);
    CHECK(heartbeat.sender_id == I1);
    CHECK(heartbeat.healthy == 1);
    CHECK(heartbeat.sequence == first_sequence);
    CHECK(heartbeat.sequence != 0);

    pthread_mutex_lock(&state.mutex);
    set_fault_locked(FAULT_LIGHT, SEV_CRITICAL, "lamp failure");
    prepare_heartbeat_message_locked(&frame);
    pthread_mutex_unlock(&state.mutex);
    memcpy(&heartbeat, frame.data, sizeof(heartbeat));
    CHECK(heartbeat.sender_id == I1);
    CHECK(heartbeat.healthy == 0);
    CHECK(heartbeat.sequence != first_sequence);
}

static void test_fixed_cycle_timing_without_central(void) {
    int i;

    reset_core();
    pthread_mutex_lock(&state.mutex);
    CHECK(state.central_conn.connected == 0);
    for (i = 0; i < GREEN_BASE_SEC; ++i) {
        traffic_tick_locked();
    }
    CHECK(state.phase == PHASE_NS_YELLOW);
    CHECK(state.time_remaining == YELLOW_SEC);
    CHECK(state.ns_light == LIGHT_YELLOW);
    CHECK(state.ew_light == LIGHT_RED);

    for (i = 0; i < YELLOW_SEC; ++i) {
        traffic_tick_locked();
    }
    CHECK(state.phase == PHASE_EW_GREEN);
    CHECK(state.time_remaining == GREEN_BASE_SEC);
    CHECK(state.ns_light == LIGHT_RED);
    CHECK(state.ew_light == LIGHT_GREEN);
    pthread_mutex_unlock(&state.mutex);
}

static void test_initial_profile_loaded(void) {
    const local_timing_config_t *config = local_config_for_intersection(I1);

    pthread_mutex_lock(&state.mutex);
    CHECK(state.initial_phase == PHASE_EW_GREEN);
    CHECK(state.phase == PHASE_EW_GREEN);
    CHECK(state.ns_light == LIGHT_RED);
    CHECK(state.ew_light == LIGHT_GREEN);
    CHECK(state.phase_duration == config->ew_green_sec);
    CHECK(state.time_remaining == config->ew_green_sec);
    pthread_mutex_unlock(&state.mutex);
}

static void test_configured_intersection_profiles(void) {
    static const phase_t expected_phase[NUM_INTERSECTIONS] = {
        PHASE_EW_GREEN,
        PHASE_NS_GREEN,
        PHASE_NS_GREEN,
        PHASE_EW_GREEN,
        PHASE_EW_GREEN,
        PHASE_NS_GREEN
    };
    unsigned i;

    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        const local_timing_config_t *config =
            local_config_for_intersection((uint8_t)i);
        CHECK(config->initial_phase == expected_phase[i]);
        CHECK(config->ns_green_sec >= GREEN_MIN_SEC);
        CHECK(config->ns_green_sec <= GREEN_MAX_SEC);
        CHECK(config->ew_green_sec >= GREEN_MIN_SEC);
        CHECK(config->ew_green_sec <= GREEN_MAX_SEC);
    }
}

static void test_reset_uses_initial_profile(void) {
    reset_core();
    pthread_mutex_lock(&state.mutex);
    state.initial_phase = PHASE_EW_GREEN;
    state.ns_green_sec = 22;
    state.ew_green_sec = 28;
    state.phase = PHASE_NS_GREEN;
    state.phase_duration = 22;
    state.time_remaining = 7;
    reset_demo_inputs_locked();
    CHECK(state.phase == PHASE_EW_GREEN);
    CHECK(state.ns_light == LIGHT_RED);
    CHECK(state.ew_light == LIGHT_GREEN);
    CHECK(state.phase_duration == 28);
    CHECK(state.time_remaining == 28);
    pthread_mutex_unlock(&state.mutex);
}

static void test_sensor_timing(void) {
    reset_core();
    pthread_mutex_lock(&state.mutex);
    state.traffic_mode = MODE_SENSOR;
    state.sensor_ns_count = SENSOR_CAR_THRESHOLD;
    state.sensor_ew_count = 0;
    CHECK(green_time_for_direction(DIR_NS) == GREEN_BASE_SEC + SENSOR_ADJUST_SEC);
    CHECK(green_time_for_direction(DIR_EW) == GREEN_BASE_SEC - SENSOR_ADJUST_SEC);

    state.sensor_ew_count = SENSOR_CAR_THRESHOLD;
    CHECK(green_time_for_direction(DIR_NS) == GREEN_BASE_SEC);
    CHECK(green_time_for_direction(DIR_EW) == GREEN_BASE_SEC);
    pthread_mutex_unlock(&state.mutex);
}

static void test_pedestrian_request_cap(void) {
    reset_core();
    pthread_mutex_lock(&state.mutex);
    state.phase = PHASE_EW_GREEN;
    state.phase_duration = GREEN_BASE_SEC;
    state.time_remaining = GREEN_BASE_SEC - 2;
    state.ns_light = LIGHT_RED;
    state.ew_light = LIGHT_GREEN;

    add_pedestrian_request_locked(DIR_NS);
    CHECK(state.ped_ns_request == 1);
    CHECK(state.time_remaining == PED_GREEN_CAP_SEC);
    CHECK(state.phase_duration == PED_GREEN_CAP_SEC + 2);
    CHECK(state.ped_extra_ns_green == GREEN_BASE_SEC - 2 - PED_GREEN_CAP_SEC);

    traffic_tick_locked();
    CHECK(state.ped_ns_walk == 1);
    CHECK(state.ped_ns_request == 0);
    pthread_mutex_unlock(&state.mutex);
}

static void test_railway_preemption_and_clear(void) {
    int i;

    reset_core();
    pthread_mutex_lock(&state.mutex);
    start_train_message_locked(1, 3);
    CHECK(state.train_pending == 1);
    CHECK(state.train_waiting_for_clear == 1);
    CHECK(state.phase == PHASE_NS_YELLOW);
    CHECK(state.time_remaining == YELLOW_SEC);

    for (i = 0; i < YELLOW_SEC; ++i) {
        traffic_tick_locked();
    }
    CHECK(state.train_pending == 0);
    CHECK(state.train_active == 1);
    CHECK(state.phase == PHASE_RAILWAY_HOLD);
    CHECK(state.ns_light == LIGHT_RED);
    CHECK(state.ew_light == LIGHT_RED);

    for (i = 0; i < 3; ++i) {
        traffic_tick_locked();
    }
    CHECK(state.train_active == 1);
    CHECK(state.train_pass_remaining == 0);

    clear_train_locked();
    CHECK(state.train_recovery_remaining == RAILWAY_RECOVERY_SEC);
    CHECK(state.phase == PHASE_RAILWAY_HOLD);
    for (i = 0; i < RAILWAY_RECOVERY_SEC; ++i) {
        traffic_tick_locked();
    }
    CHECK(state.train_recovery_remaining == 0);
    CHECK(state.phase == PHASE_NS_GREEN);
    pthread_mutex_unlock(&state.mutex);
}

static void test_runtime_ids_and_railway_mapping(void) {
    static const uint8_t expected_crossings[NUM_INTERSECTIONS] = {
        1, 1, 2, 2, 3, 3
    };
    unsigned i;

    reset_core();
    pthread_mutex_lock(&state.mutex);
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        unsigned other = (i + 1) % NUM_INTERSECTIONS;
        state.intersection_id = (uint8_t)i;
        CHECK(target_matches_local((uint8_t)i));
        CHECK(target_matches_local(INTERSECTION_ALL));
        CHECK(!target_matches_local((uint8_t)other));
        CHECK(railway_matches_local(expected_crossings[i]));
        CHECK(railway_display_line(expected_crossings[i]) == expected_crossings[i]);
        if (expected_crossings[i] != 1) {
            CHECK(!railway_matches_local(1));
        }
        if (expected_crossings[i] != 2) {
            CHECK(!railway_matches_local(2));
        }
        if (expected_crossings[i] != 3) {
            CHECK(!railway_matches_local(3));
        }
    }
    state.intersection_id = I1;
    CHECK(railway_matches_local(P1));
    pthread_mutex_unlock(&state.mutex);
}

int main(void) {
    local_state_init(CONN_MODE_LOCAL, I1, LOCAL_SERVICE_NAME);
    test_initial_profile_loaded();
    test_configured_intersection_profiles();
    test_reset_uses_initial_profile();
    test_status_telemetry();
    test_typed_heartbeat_health();
    test_fixed_cycle_timing_without_central();
    test_sensor_timing();
    test_pedestrian_request_cap();
    test_railway_preemption_and_clear();
    test_runtime_ids_and_railway_mapping();
    local_state_destroy();

    printf("LOCAL_LOGIC_TEST %s checks=%u failures=%u\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
