#include "local_controller.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const local_timing_config_t local_configs[NUM_INTERSECTIONS] = {
    /* User-selected stagger profile:
     * I1 EW, I2 NS, I3 NS, I4 EW, I5 EW, I6 NS.
     */
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC }
};

const local_timing_config_t* local_config_for_intersection(uint8_t intersection_id) {
    if (intersection_id >= NUM_INTERSECTIONS) {
        intersection_id = I1;
    }
    return &local_configs[intersection_id];
}

local_state_t state;

static void bump_sequence(uint16_t *value) {
    ++(*value);
    if (*value == 0) {
        ++(*value);
    }
}

void mark_status_dirty_locked(void) {
    bump_sequence(&state.status_sequence);
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

static uint8_t railway_crossing_for_local(uint8_t intersection_id) {
    if (intersection_id <= I2) {
        return 1;
    }
    if (intersection_id <= I4) {
        return 2;
    }
    return 3;
}

int railway_matches_local(uint8_t id) {
    uint8_t crossing = railway_crossing_for_local(state.intersection_id);

    /*
     * Current Train code sends crossing_t.id as 1..3 for P1..P3.
     * Accept protocol P1's zero only because it is unambiguous with that
     * dialect; P2/P3 use the current Train IDs 2/3 here.
     */
    return id == crossing || (crossing == 1 && id == P1);
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
    int sim_seconds = state.sim_seconds % SECONDS_PER_DAY;
    if (sim_seconds < 0) {
        sim_seconds += SECONDS_PER_DAY;
    }

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
    status->telemetry_version = STATUS_TELEMETRY_VERSION;
    status->status_sequence = state.status_sequence;
    status->last_command_id = state.last_applied_command_id;
    status->sim_minute_of_day = (uint16_t)(sim_seconds / 60);
    status->temporary_remaining = (uint16_t)(state.temp_mode_remaining > 0 ?
        state.temp_mode_remaining : 0);
    status->coordination_offset_sec = (uint16_t)(state.coordination_offset_sec > 0 ?
        state.coordination_offset_sec : 0);
    status->sensor_ns_count = (uint8_t)state.sensor_ns_count;
    status->sensor_ew_count = (uint8_t)state.sensor_ew_count;
    status->pedestrian_ns_request = state.ped_ns_request ? 1 : 0;
    status->pedestrian_ew_request = state.ped_ew_request ? 1 : 0;
    status->sim_running = state.sim_running ? 1 : 0;
    status->train_pending = state.train_pending ? 1 : 0;
    status->train_active = state.train_active ? 1 : 0;
    status->train_recovery_remaining =
        (uint8_t)(state.train_recovery_remaining > 0 ?
                  state.train_recovery_remaining : 0);
    status->manual_mode_override = state.manual_mode_override ? 1 : 0;
    status->manual_sensor_override = state.manual_sensor_override ? 1 : 0;
    status->coordination_pending = state.coordination_pending ? 1 : 0;
    status->fault_active = state.fault_active ? 1 : 0;
    status->fault_type = state.fault_type;
    status->fault_severity = state.fault_active ? state.fault_severity : 0;
}

/* Remaining time for the current lamp, assuming no new external event. */
int local_vehicle_seconds(const status_msg_t *status, direction_t direction) {
    unsigned lamp = direction == DIR_NS ? status->ns_state : status->ew_state;
    if (status->railway_preempt || status->mode == MODE_FAILSAFE ||
        status->phase > PHASE_EW_YELLOW || lamp == LIGHT_OFF) return -1;
    if (lamp == LIGHT_RED) {
        if (status->coordination_pending) return -1;
        if (status->phase == PHASE_NS_GREEN || status->phase == PHASE_EW_GREEN)
            return status->time_remaining + YELLOW_SEC;
    }
    return status->time_remaining;
}

uint16_t prepare_status_message_locked(test_message_t *msg) {
    status_msg_t status;
    init_message(msg, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    fill_status_locked(&status);
    memcpy(msg->data, &status, sizeof(status));
    return status.status_sequence;
}

void set_fault_locked(fault_type_t type, fault_severity_t severity,
                      const char *description) {
    state.fault_active = type != FAULT_NONE;
    bump_sequence(&state.fault_sequence);
    state.fault_type = type;
    state.fault_severity = severity;
    snprintf(state.fault_description, sizeof(state.fault_description),
             "%s", description);
    mark_status_dirty_locked();
}

void clear_fault_locked(void) {
    state.fault_active = 0;
    bump_sequence(&state.fault_sequence);
    state.fault_type = FAULT_NONE;
    state.fault_severity = SEV_LOW;
    snprintf(state.fault_description, sizeof(state.fault_description),
             "%s", "cleared");
    mark_status_dirty_locked();
}

uint16_t prepare_fault_message_locked(test_message_t *msg) {
    fault_msg_t fault;
    init_message(msg, MSG_FAULT_ALERT, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    memset(&fault, 0, sizeof(fault));
    fault.source_id = state.intersection_id;
    fault.fault_type = state.fault_type;
    fault.severity = state.fault_severity;
    snprintf(fault.description, sizeof(fault.description),
             "%s", state.fault_description);
    memcpy(msg->data, &fault, sizeof(fault));
    return state.fault_sequence;
}

uint16_t prepare_heartbeat_message_locked(test_message_t *msg) {
    heartbeat_msg_t heartbeat;
    init_message(msg, MSG_HEARTBEAT, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.sender_id = state.intersection_id;
    heartbeat.healthy = (!state.fault_active && state.traffic_mode != MODE_FAILSAFE) ? 1 : 0;
    bump_sequence(&state.heartbeat_sequence);
    heartbeat.sequence = state.heartbeat_sequence;
    memcpy(msg->data, &heartbeat, sizeof(heartbeat));
    return heartbeat.sequence;
}

void local_state_init(connection_mode_t mode, uint8_t intersection_id,
                      const char *service_name) {
    const local_timing_config_t *config;
    direction_t initial_direction;
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.mutex, NULL);
    state.mode = mode;
    state.status_sequence = 1;
    state.fault_sequence = 1;
    state.fault_severity = SEV_LOW;
    if (intersection_id >= NUM_INTERSECTIONS) {
        intersection_id = I1;
    }
#if ENABLE_TRAFFIC_SIMULATION
    /* Cores launched in the same second still need different random streams. */
    srand((unsigned)time(NULL) ^ ((unsigned)(intersection_id + 1) * 2654435761U));
#endif
    config = local_config_for_intersection(intersection_id);
    state.intersection_id = intersection_id;
    state.initial_phase = config->initial_phase;
    state.ns_green_sec = config->ns_green_sec;
    state.ew_green_sec = config->ew_green_sec;
    snprintf(state.service_name, sizeof(state.service_name), "%s",
             service_name != NULL ? service_name : LOCAL_SERVICE_NAME);
#if ENABLE_TRAFFIC_SIMULATION
    state.sim_running = 1;
    state.sim_seconds = 6 * 3600;
    state.next_train_in_seconds = random_train_gap_seconds();
    state.next_train_direction = random_train_direction();
    state.next_car_in_seconds = random_car_gap_seconds();
    state.traffic_mode = is_peak_time(state.sim_seconds) ? MODE_FIXED : MODE_SENSOR;
    apply_time_settings_locked();
#else
    state.traffic_mode = MODE_FIXED;
#endif
    state.phase = state.initial_phase;
    local_plan_cycle_locked();
    initial_direction =
        state.phase == PHASE_EW_GREEN ? DIR_EW : DIR_NS;
    state.phase_duration = green_time_for_direction(initial_direction);
    state.time_remaining = state.phase_duration;
    if (state.phase == PHASE_EW_GREEN) {
        state.ns_light = LIGHT_RED;
        state.ew_light = LIGHT_GREEN;
    } else {
        state.ns_light = LIGHT_GREEN;
        state.ew_light = LIGHT_RED;
    }
}

void local_state_destroy(void) {
    pthread_mutex_destroy(&state.mutex);
}
