#include "local_controller.h"

#include <stdlib.h>

int is_peak_time(int seconds) {
    int hour = seconds / 3600;
    return (hour >= 7 && hour < 9) || (hour >= 16 && hour < 18);
}

int is_night_time(int seconds) {
    int hour = seconds / 3600;
    return hour >= 22 || hour < 5;
}

int random_train_gap_seconds(void) {
    if (is_peak_time(state.sim_seconds)) {
        return 120 + rand() % 181;  // 2-5 minutes at peak
    }
    if (is_night_time(state.sim_seconds)) {
        return 1080 + rand() % 361; // about 18-24 minutes at night
    }
    return 480 + rand() % 421;      // 8-15 minutes during the day
}

int random_train_direction(void) {
    return (rand() % 2) + 1;
}

int random_car_gap_seconds(void) {
    if (is_peak_time(state.sim_seconds)) {
        return 2 + rand() % 3;   // 2-4 seconds at peak time
    }
    if (is_night_time(state.sim_seconds)) {
        return 14 + rand() % 12; // 14-25 seconds at night
    }
    return 5 + rand() % 6;       // 5-10 seconds during normal hours
}

static int clamp_green_time(int seconds) {
    if (seconds < GREEN_MIN_SEC) {
        return GREEN_MIN_SEC;
    }
    if (seconds > GREEN_MAX_SEC) {
        return GREEN_MAX_SEC;
    }
    return seconds;
}

int green_time_for_direction(direction_t direction) {
    int ns_high = state.sensor_ns_count >= SENSOR_CAR_THRESHOLD;
    int ew_high = state.sensor_ew_count >= SENSOR_CAR_THRESHOLD;
    int seconds = GREEN_BASE_SEC;

    if (state.traffic_mode == MODE_SENSOR) {
        if (direction == DIR_NS && ns_high && !ew_high) {
            seconds += SENSOR_ADJUST_SEC;
        } else if (direction == DIR_NS && ew_high && !ns_high) {
            seconds -= SENSOR_ADJUST_SEC;
        } else if (direction == DIR_EW && ew_high && !ns_high) {
            seconds += SENSOR_ADJUST_SEC;
        } else if (direction == DIR_EW && ns_high && !ew_high) {
            seconds -= SENSOR_ADJUST_SEC;
        }
    }

    return clamp_green_time(seconds);
}

static int next_green_time_for_direction(direction_t direction) {
    int seconds = green_time_for_direction(direction);

    if (direction == DIR_NS) {
        seconds += state.ped_extra_ns_green;
        state.ped_extra_ns_green = 0;
    } else {
        seconds += state.ped_extra_ew_green;
        state.ped_extra_ew_green = 0;
    }

    return clamp_green_time(seconds);
}

static int pedestrian_service_phase(phase_t phase, direction_t direction) {
    return (phase == PHASE_EW_GREEN && direction == DIR_NS) ||
           (phase == PHASE_NS_GREEN && direction == DIR_EW);
}

static void add_car_locked(direction_t direction) {
    if (direction == DIR_NS && state.sensor_ns_count < MAX_SENSOR_CARS) {
        state.sensor_ns_count++;
    } else if (direction == DIR_EW && state.sensor_ew_count < MAX_SENSOR_CARS) {
        state.sensor_ew_count++;
    }
}

static void let_cars_pass_locked(direction_t direction) {
    int cars_to_pass = is_peak_time(state.sim_seconds) ? 2 : 1;

    if (direction == DIR_NS) {
        if (state.sensor_ns_count < cars_to_pass) {
            cars_to_pass = state.sensor_ns_count;
        }
        state.sensor_ns_count -= cars_to_pass;
    } else {
        if (state.sensor_ew_count < cars_to_pass) {
            cars_to_pass = state.sensor_ew_count;
        }
        state.sensor_ew_count -= cars_to_pass;
    }
}

static void update_lights_locked(void) {
    if (state.train_active || state.train_recovery_remaining > 0 ||
        state.phase == PHASE_RAILWAY_HOLD) {
        state.ns_light = LIGHT_RED;
        state.ew_light = LIGHT_RED;
        state.ped_ns_walk = 0;
        state.ped_ew_walk = 0;
        return;
    }

    switch (state.phase) {
        case PHASE_NS_GREEN:
            state.ns_light = LIGHT_GREEN;
            state.ew_light = LIGHT_RED;
            break;
        case PHASE_NS_YELLOW:
            state.ns_light = LIGHT_YELLOW;
            state.ew_light = LIGHT_RED;
            break;
        case PHASE_EW_GREEN:
            state.ns_light = LIGHT_RED;
            state.ew_light = LIGHT_GREEN;
            break;
        case PHASE_EW_YELLOW:
            state.ns_light = LIGHT_RED;
            state.ew_light = LIGHT_YELLOW;
            break;
        default:
            state.ns_light = LIGHT_RED;
            state.ew_light = LIGHT_RED;
            break;
    }
}

static void update_pedestrian_locked(void) {
    int elapsed = state.phase_duration - state.time_remaining;

    if (state.train_active || state.train_recovery_remaining > 0) {
        state.ped_ns_walk = 0;
        state.ped_ew_walk = 0;
        return;
    }

    if (pedestrian_service_phase(state.phase, DIR_NS)) {
        if (elapsed >= PED_WALK_START_SEC &&
            state.time_remaining > PED_WALK_END_SEC) {
            state.ped_ns_walk = 1;
            state.ped_ns_request = 0;
        } else if (state.ped_ns_walk &&
                   state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ns_walk = 0;
        } else if (state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ns_walk = 0;
        }
    } else {
        state.ped_ns_walk = 0;
    }

    if (pedestrian_service_phase(state.phase, DIR_EW)) {
        if (elapsed >= PED_WALK_START_SEC &&
            state.time_remaining > PED_WALK_END_SEC) {
            state.ped_ew_walk = 1;
            state.ped_ew_request = 0;
        } else if (state.ped_ew_walk &&
                   state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ew_walk = 0;
        } else if (state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ew_walk = 0;
        }
    } else {
        state.ped_ew_walk = 0;
    }
}

static int hold_seconds_locked(void) {
    if (state.train_active) {
        if (state.train_waiting_for_clear && state.train_pass_remaining <= 0) {
            return -1;
        }
        return state.train_pass_remaining;
    }
    if (state.train_recovery_remaining > 0) {
        return state.train_recovery_remaining;
    }
    return -1;
}

int vehicle_signal_seconds_locked(direction_t direction) {
    if (state.train_active || state.train_recovery_remaining > 0 ||
        state.phase == PHASE_RAILWAY_HOLD ||
        state.traffic_mode == MODE_FAILSAFE) {
        return hold_seconds_locked();
    }

    switch (state.phase) {
        case PHASE_NS_GREEN:
            return direction == DIR_NS ?
                state.time_remaining : state.time_remaining + YELLOW_SEC;
        case PHASE_NS_YELLOW:
            return state.time_remaining;
        case PHASE_EW_GREEN:
            return direction == DIR_EW ?
                state.time_remaining : state.time_remaining + YELLOW_SEC;
        case PHASE_EW_YELLOW:
            return state.time_remaining;
        default:
            return -1;
    }
}

int pedestrian_signal_seconds_locked(direction_t direction) {
    int elapsed = state.phase_duration - state.time_remaining;

    if (state.train_active || state.train_recovery_remaining > 0 ||
        state.phase == PHASE_RAILWAY_HOLD ||
        state.traffic_mode == MODE_FAILSAFE) {
        return hold_seconds_locked();
    }

    if ((direction == DIR_NS && state.ped_ns_walk) ||
        (direction == DIR_EW && state.ped_ew_walk)) {
        int walk_left = state.time_remaining - PED_WALK_END_SEC;
        return walk_left > 0 ? walk_left : 0;
    }

    if (pedestrian_service_phase(state.phase, direction)) {
        if (elapsed < PED_WALK_START_SEC) {
            return PED_WALK_START_SEC - elapsed;
        }

        if (state.time_remaining > PED_WALK_END_SEC) {
            return 0;
        }

        if (direction == DIR_NS) {
            return state.time_remaining + YELLOW_SEC +
                   green_time_for_direction(DIR_NS) + YELLOW_SEC +
                   PED_WALK_START_SEC;
        }
        return state.time_remaining + YELLOW_SEC +
               green_time_for_direction(DIR_EW) + YELLOW_SEC +
               PED_WALK_START_SEC;
    }

    if (direction == DIR_NS) {
        if (state.phase == PHASE_NS_GREEN) {
            return state.time_remaining + YELLOW_SEC + PED_WALK_START_SEC;
        }
        if (state.phase == PHASE_NS_YELLOW) {
            return state.time_remaining + PED_WALK_START_SEC;
        }
        if (state.phase == PHASE_EW_YELLOW) {
            return state.time_remaining + green_time_for_direction(DIR_NS) +
                   YELLOW_SEC + PED_WALK_START_SEC;
        }
    } else {
        if (state.phase == PHASE_EW_GREEN) {
            return state.time_remaining + YELLOW_SEC + PED_WALK_START_SEC;
        }
        if (state.phase == PHASE_EW_YELLOW) {
            return state.time_remaining + PED_WALK_START_SEC;
        }
        if (state.phase == PHASE_NS_YELLOW) {
            return state.time_remaining + green_time_for_direction(DIR_EW) +
                   YELLOW_SEC + PED_WALK_START_SEC;
        }
    }

    return -1;
}

static void set_phase_locked(phase_t phase, int duration) {
    state.phase = phase;
    state.phase_duration = duration;
    state.time_remaining = duration;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    update_lights_locked();
    mark_status_dirty_locked();
}

static void set_coordination_phase_locked(phase_t base_phase, int offset_sec) {
    int cycle_sec = 2 * (GREEN_BASE_SEC + YELLOW_SEC);
    int offset = offset_sec % cycle_sec;
    phase_t first_green = base_phase;
    phase_t first_yellow =
        base_phase == PHASE_NS_GREEN ? PHASE_NS_YELLOW : PHASE_EW_YELLOW;
    phase_t second_green =
        base_phase == PHASE_NS_GREEN ? PHASE_EW_GREEN : PHASE_NS_GREEN;
    phase_t second_yellow =
        base_phase == PHASE_NS_GREEN ? PHASE_EW_YELLOW : PHASE_NS_YELLOW;

    if (offset < GREEN_BASE_SEC) {
        set_phase_locked(first_green, GREEN_BASE_SEC - offset);
    } else if (offset < GREEN_BASE_SEC + YELLOW_SEC) {
        set_phase_locked(first_yellow,
                         GREEN_BASE_SEC + YELLOW_SEC - offset);
    } else if (offset < 2 * GREEN_BASE_SEC + YELLOW_SEC) {
        set_phase_locked(second_green,
                         2 * GREEN_BASE_SEC + YELLOW_SEC - offset);
    } else {
        set_phase_locked(second_yellow, cycle_sec - offset);
    }
}

static void advance_phase_locked(void) {
    if (state.coordination_pending &&
        (state.phase == PHASE_NS_YELLOW || state.phase == PHASE_EW_YELLOW)) {
        set_coordination_phase_locked(state.coordination_phase,
                                      state.coordination_offset_sec);
        state.coordination_pending = 0;
        return;
    }

    switch (state.phase) {
        case PHASE_NS_GREEN:
            set_phase_locked(PHASE_NS_YELLOW, YELLOW_SEC);
            break;
        case PHASE_NS_YELLOW:
            set_phase_locked(PHASE_EW_GREEN, next_green_time_for_direction(DIR_EW));
            break;
        case PHASE_EW_GREEN:
            set_phase_locked(PHASE_EW_YELLOW, YELLOW_SEC);
            break;
        case PHASE_EW_YELLOW:
        default:
            set_phase_locked(PHASE_NS_GREEN, next_green_time_for_direction(DIR_NS));
            break;
    }
}

void traffic_tick_locked(void) {
    if (state.traffic_mode == MODE_FAILSAFE) {
        state.phase = PHASE_RAILWAY_HOLD;
        state.time_remaining = 0;
        update_lights_locked();
        mark_status_dirty_locked();
        return;
    }

    if (state.train_pending) {
        if (state.time_remaining > 0) {
            state.time_remaining--;
        }

        update_lights_locked();

        if (state.time_remaining <= 0) {
            state.train_pending = 0;
            state.train_active = 1;
            set_phase_locked(PHASE_RAILWAY_HOLD, 0);
        }
        return;
    }

    if (state.train_active) {
        if (state.train_waiting_for_clear) {
            if (state.train_pass_remaining > 0) {
                state.train_pass_remaining--;
            }
            state.phase = PHASE_RAILWAY_HOLD;
            state.time_remaining = state.train_pass_remaining;
            update_lights_locked();
            mark_status_dirty_locked();
            return;
        }

        if (state.train_pass_remaining > 0) {
            state.train_pass_remaining--;
        }

        if (state.train_pass_remaining <= 0) {
            clear_train_locked();
        } else {
            set_phase_locked(PHASE_RAILWAY_HOLD, state.train_pass_remaining);
        }
        return;
    }

    if (state.train_recovery_remaining > 0) {
        state.train_recovery_remaining--;
        state.phase = PHASE_RAILWAY_HOLD;
        state.time_remaining = state.train_recovery_remaining;
        update_lights_locked();

        if (state.train_recovery_remaining == 0) {
#if ENABLE_TRAFFIC_SIMULATION
            if (state.sim_running && !state.train_conn.connected) {
                state.next_train_in_seconds = random_train_gap_seconds();
                state.next_train_direction = random_train_direction();
            }
#endif
            set_phase_locked(PHASE_NS_GREEN, green_time_for_direction(DIR_NS));
        }
        return;
    }

    if (state.time_remaining > 0) {
        state.time_remaining--;
    }

    update_lights_locked();
    update_pedestrian_locked();

    if (state.ns_light == LIGHT_GREEN && state.ew_light == LIGHT_GREEN) {
        state.traffic_mode = MODE_FAILSAFE;
        state.phase = PHASE_RAILWAY_HOLD;
        state.time_remaining = 0;
        state.ns_light = LIGHT_RED;
        state.ew_light = LIGHT_RED;
        state.ped_ns_walk = 0;
        state.ped_ew_walk = 0;
        set_fault_locked(FAULT_NOT_WORKING, SEV_CRITICAL,
                         "conflicting green");
        return;
    }

    if (state.time_remaining <= 0) {
        advance_phase_locked();
    }
}

void update_temporary_mode_locked(void) {
    if (state.temp_mode_remaining <= 0) {
        return;
    }

    state.temp_mode_remaining--;
    if (state.temp_mode_remaining == 0) {
        state.traffic_mode = state.temp_return_mode;
        state.manual_mode_override = state.temp_return_manual_override;
        apply_time_settings_locked();
        mark_status_dirty_locked();
    }
}

void apply_time_settings_locked(void) {
    if (!state.manual_mode_override && state.traffic_mode != MODE_FAILSAFE) {
        state.traffic_mode = is_peak_time(state.sim_seconds) ?
            MODE_FIXED : MODE_SENSOR;
        mark_status_dirty_locked();
    }
}

void update_vehicle_counts_locked(void) {
    if (!state.sim_running) {
        return;
    }

    if (!state.manual_sensor_override) {
        if (state.next_ns_car_in_seconds > 0) {
            state.next_ns_car_in_seconds--;
        }
        if (state.next_ew_car_in_seconds > 0) {
            state.next_ew_car_in_seconds--;
        }

        if (state.next_ns_car_in_seconds <= 0) {
            add_car_locked(DIR_NS);
            state.next_ns_car_in_seconds = random_car_gap_seconds();
        }
        if (state.next_ew_car_in_seconds <= 0) {
            add_car_locked(DIR_EW);
            state.next_ew_car_in_seconds = random_car_gap_seconds();
        }
    }

    if (!state.train_pending && !state.train_active &&
        state.train_recovery_remaining <= 0) {
        if (state.phase == PHASE_NS_GREEN) {
            let_cars_pass_locked(DIR_NS);
        } else if (state.phase == PHASE_EW_GREEN) {
            let_cars_pass_locked(DIR_EW);
        }
    }
}

void update_time_of_day_locked(void) {
#if ENABLE_TRAFFIC_SIMULATION
    if (!state.sim_running) {
        return;
    }

    state.sim_seconds =
        (state.sim_seconds + SIM_SECONDS_PER_TICK) % SECONDS_PER_DAY;

    apply_time_settings_locked();

    if (!state.train_pending && !state.train_active &&
        state.train_recovery_remaining <= 0 &&
        !state.train_conn.connected &&
        state.next_train_in_seconds > 0) {
        state.next_train_in_seconds--;
    }

    if (state.next_train_in_seconds <= 0 &&
        !state.train_pending &&
        !state.train_active &&
        !state.train_conn.connected &&
        state.train_recovery_remaining <= 0) {
        start_train_locked(state.next_train_direction);
        state.next_train_in_seconds = 0;
    }
#endif
}

#if ENABLE_TRAFFIC_SIMULATION
static void set_sim_seconds_locked(int seconds) {
    state.sim_seconds = seconds % SECONDS_PER_DAY;
    if (state.sim_seconds < 0) {
        state.sim_seconds += SECONDS_PER_DAY;
    }
    apply_time_settings_locked();
#if ENABLE_TRAFFIC_SIMULATION
    state.next_train_in_seconds = random_train_gap_seconds();
    state.next_train_direction = random_train_direction();
    state.next_ns_car_in_seconds = random_car_gap_seconds();
    state.next_ew_car_in_seconds = random_car_gap_seconds();
#endif
}

void set_sim_hour_locked(int hour) {
    state.manual_mode_override = 0;
    state.manual_sensor_override = 0;
    set_sim_seconds_locked(hour * 3600);
}

void set_sim_minute_locked(unsigned minute) {
    state.manual_mode_override = 0;
    state.manual_sensor_override = 0;
    set_sim_seconds_locked((int)(minute * 60U));
}
#endif

static void cap_green_for_pedestrian_locked(direction_t direction) {
    int elapsed;
    int removed;

    if (!pedestrian_service_phase(state.phase, direction) ||
        state.time_remaining <= PED_GREEN_CAP_SEC) {
        return;
    }

    elapsed = state.phase_duration - state.time_remaining;
    removed = state.time_remaining - PED_GREEN_CAP_SEC;
    state.time_remaining = PED_GREEN_CAP_SEC;
    state.phase_duration = elapsed + PED_GREEN_CAP_SEC;

    if (direction == DIR_NS) {
        state.ped_extra_ns_green += removed;
    } else {
        state.ped_extra_ew_green += removed;
    }
}

void add_pedestrian_request_locked(direction_t direction) {
    int already_requested;

    if (direction == DIR_NS) {
        already_requested = state.ped_ns_request;
        state.ped_ns_request = 1;
    } else {
        already_requested = state.ped_ew_request;
        state.ped_ew_request = 1;
    }

    if (!already_requested) {
        cap_green_for_pedestrian_locked(direction);
    }
    mark_status_dirty_locked();
}

#if ENABLE_DEMO_COMMANDS
void toggle_sensor_locked(direction_t direction) {
    state.manual_sensor_override = 1;

    if (direction == DIR_NS) {
        state.sensor_ns_count =
            state.sensor_ns_count >= SENSOR_CAR_THRESHOLD ? 0 : DEMO_HIGH_CAR_COUNT;
    } else {
        state.sensor_ew_count =
            state.sensor_ew_count >= SENSOR_CAR_THRESHOLD ? 0 : DEMO_HIGH_CAR_COUNT;
    }
    mark_status_dirty_locked();
}
#endif

static void start_train_common_locked(int direction, int wait_for_clear,
                                      int eta_seconds) {
    if (state.train_active || state.train_pending) {
        return;
    }

    state.train_pending = 1;
    state.train_direction = direction;
    state.train_pass_remaining = wait_for_clear ?
        eta_seconds : TRAIN_PASSING_SECONDS;
    state.train_waiting_for_clear = wait_for_clear;
    state.train_recovery_remaining = 0;
    state.next_train_in_seconds = 0;
    state.ped_extra_ns_green = 0;
    state.ped_extra_ew_green = 0;

    if (state.phase == PHASE_NS_GREEN) {
        set_phase_locked(PHASE_NS_YELLOW, YELLOW_SEC);
    } else if (state.phase == PHASE_EW_GREEN) {
        set_phase_locked(PHASE_EW_YELLOW, YELLOW_SEC);
    } else if (state.phase != PHASE_NS_YELLOW && state.phase != PHASE_EW_YELLOW) {
        state.train_pending = 0;
        state.train_active = 1;
        set_phase_locked(PHASE_RAILWAY_HOLD, 0);
    }

    mark_status_dirty_locked();
}

void start_train_locked(int direction) {
    start_train_common_locked(direction, 0, 0);
}

void start_train_message_locked(int direction, int eta_seconds) {
    if (eta_seconds < 0) {
        eta_seconds = 0;
    }

    if (state.train_active || state.train_pending) {
        state.train_direction = direction;
        state.train_waiting_for_clear = 1;
        state.train_pass_remaining = eta_seconds;
        state.next_train_in_seconds = 0;
        set_phase_locked(PHASE_RAILWAY_HOLD, 0);
        mark_status_dirty_locked();
        return;
    }

    start_train_common_locked(direction, 1, eta_seconds);
}

void clear_train_locked(void) {
    if (state.train_active || state.train_pending) {
        state.train_pending = 0;
        state.train_active = 0;
        state.train_direction = 0;
        state.train_pass_remaining = 0;
        state.train_waiting_for_clear = 0;
        state.train_recovery_remaining = RAILWAY_RECOVERY_SEC;
        state.ped_extra_ns_green = 0;
        state.ped_extra_ew_green = 0;
        set_phase_locked(PHASE_RAILWAY_HOLD, RAILWAY_RECOVERY_SEC);
        mark_status_dirty_locked();
    }
}

void reset_demo_inputs_locked(void) {
    state.sensor_ns_count = 0;
    state.sensor_ew_count = 0;
    state.ped_ns_request = 0;
    state.ped_ew_request = 0;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    state.manual_mode_override = 0;
    state.manual_sensor_override = 0;
    state.train_pending = 0;
    state.train_active = 0;
    state.train_direction = 0;
    state.train_pass_remaining = 0;
    state.train_waiting_for_clear = 0;
    state.train_recovery_remaining = 0;
    state.temp_mode_remaining = 0;
    state.coordination_pending = 0;
    state.coordination_offset_sec = 0;
    state.ped_extra_ns_green = 0;
    state.ped_extra_ew_green = 0;
    state.sim_running = 1;
    clear_fault_locked();
#if ENABLE_TRAFFIC_SIMULATION
    state.next_train_in_seconds = random_train_gap_seconds();
    state.next_train_direction = random_train_direction();
    state.next_ns_car_in_seconds = random_car_gap_seconds();
    state.next_ew_car_in_seconds = random_car_gap_seconds();
    apply_time_settings_locked();
#else
    state.traffic_mode = MODE_FIXED;
#endif
    set_phase_locked(PHASE_NS_GREEN, green_time_for_direction(DIR_NS));
    mark_status_dirty_locked();
}
