#include "local_controller.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

local_state_t state;

void mark_status_dirty_locked(void) {
    state.status_dirty = 1;
    state.ui_needs_update = 1;
}

traffic_light_mode display_mode(void) {
    if (state.train_pending || state.train_active ||
        state.train_recovery_remaining > 0) {
        return MODE_RAILWAY;
    }
    return state.traffic_mode;
}

int target_matches_local(uint8_t target) {
    return target == state.intersection_id || target == INTERSECTION_ALL;
}

int railway_matches_local(uint8_t id) {
    if (state.intersection_id != I1) {
        return 0;
    }

    /*
     * I1 is affected by railway crossing P1 only.
     * Current Train code sends crossing_t.id as 1 for P1.
     * The shared protocol enum also has P1 as 0, so accept both.
     */
    return id == 1 || id == P1;
}

int railway_display_line(uint8_t id) {
    if (id == P1) {
        return 1;
    }
    return (int)id;
}

void init_message(test_message_t *msg, msg_type_t type,
                  controller_type_t src, controller_type_t dst) {
    memset(msg, 0, sizeof(*msg));
    msg->header.type = type;
    msg->header.src = src;
    msg->header.dst = dst;
    get_timestamp(msg->header.timestamp, sizeof(msg->header.timestamp));
}

void fill_status_locked(status_msg_t *status) {
    memset(status, 0, sizeof(*status));
    status->intersection_id = state.intersection_id;
    status->mode = display_mode();
    status->phase = state.phase;
    status->ns_state = state.ns_light;
    status->ew_state = state.ew_light;
    status->pedestrian_ns = state.ped_ns_walk ? 1 : 0;
    status->pedestrian_ew = state.ped_ew_walk ? 1 : 0;
    status->railway_preempt =
        (state.train_pending || state.train_active ||
         state.train_recovery_remaining > 0) ? 1 : 0;
    status->time_remaining = (uint16_t)(state.time_remaining > 0 ?
        state.time_remaining : 0);
}

void prepare_status_message_locked(test_message_t *msg) {
    status_msg_t status;
    init_message(msg, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    fill_status_locked(&status);
    memcpy(msg->data, &status, sizeof(status));
    state.status_dirty = 0;
}

void set_fault_locked(fault_type_t type, fault_severity_t severity,
                      const char *description) {
    state.fault_active = type != FAULT_NONE;
    state.fault_pending = 1;
    state.fault_type = type;
    state.fault_severity = severity;
    snprintf(state.fault_description, sizeof(state.fault_description),
             "%s", description);
    mark_status_dirty_locked();
}

void clear_fault_locked(void) {
    state.fault_active = 0;
    state.fault_pending = 1;
    state.fault_type = FAULT_NONE;
    state.fault_severity = SEV_LOW;
    snprintf(state.fault_description, sizeof(state.fault_description),
             "%s", "cleared");
    mark_status_dirty_locked();
}

void prepare_fault_message_locked(test_message_t *msg) {
    fault_msg_t fault;
    init_message(msg, MSG_FAULT_ALERT, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    memset(&fault, 0, sizeof(fault));
    fault.source_id = state.intersection_id;
    fault.fault_type = state.fault_type;
    fault.severity = state.fault_severity;
    snprintf(fault.description, sizeof(fault.description),
             "%s", state.fault_description);
    memcpy(msg->data, &fault, sizeof(fault));
}

void local_state_init(connection_mode_t mode) {
#if ENABLE_TRAFFIC_SIMULATION
    srand((unsigned)time(NULL));
#endif
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.mutex, NULL);
    pthread_mutex_init(&state.central_send_mutex, NULL);
    pthread_mutex_init(&state.train_send_mutex, NULL);
    state.mode = mode;
    state.ui_needs_update = 1;
    state.status_dirty = 1;
    state.intersection_id = LOCAL_INTERSECTION_ID;
#if ENABLE_TRAFFIC_SIMULATION
    state.sim_running = 1;
    state.sim_seconds = 6 * 3600;
    state.next_train_in_seconds = random_train_gap_seconds();
    state.next_train_direction = random_train_direction();
    state.next_ns_car_in_seconds = random_car_gap_seconds();
    state.next_ew_car_in_seconds = random_car_gap_seconds();
    state.traffic_mode = is_peak_time(state.sim_seconds) ? MODE_FIXED : MODE_SENSOR;
    apply_time_settings_locked();
#else
    state.traffic_mode = MODE_FIXED;
#endif
    state.phase = PHASE_NS_GREEN;
    state.phase_duration = GREEN_BASE_SEC;
    state.time_remaining = GREEN_BASE_SEC;
    state.ns_light = LIGHT_GREEN;
    state.ew_light = LIGHT_RED;
    connection_init(&state.central_conn, CENTRAL_SERVICE_NAME, mode, &state.mutex);
    connection_init(&state.train_conn, TRAIN_SERVICE_NAME, mode, &state.mutex);
}

void local_state_destroy(void) {
    connection_close(&state.central_conn);
    connection_close(&state.train_conn);
    pthread_mutex_destroy(&state.mutex);
    pthread_mutex_destroy(&state.central_send_mutex);
    pthread_mutex_destroy(&state.train_send_mutex);
}
