#include "local_controller.h"

static int clamp_green_time(int seconds) {
    if (seconds < GREEN_MIN_SEC) {
        return GREEN_MIN_SEC;
    }
    if (seconds > GREEN_MAX_SEC) {
        return GREEN_MAX_SEC;
    }
    return seconds;
}

static int configured_green_time_for_direction(direction_t direction) {
    int seconds = direction == DIR_NS ? state.ns_green_sec : state.ew_green_sec;

    if (seconds <= 0) {
        seconds = GREEN_BASE_SEC;
    }

    return clamp_green_time(seconds);
}

int local_fixed_cycle_seconds_locked(void) {
    return configured_green_time_for_direction(DIR_NS) + YELLOW_SEC +
           configured_green_time_for_direction(DIR_EW) + YELLOW_SEC;
}

int green_time_for_direction(direction_t direction) {
    int ns_high = state.sensor_ns_count >= SENSOR_CAR_THRESHOLD;
    int ew_high = state.sensor_ew_count >= SENSOR_CAR_THRESHOLD;
    int seconds = configured_green_time_for_direction(direction);

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

void local_plan_cycle_locked(void) {
    int ns = green_time_for_direction(DIR_NS);
    /* Reserve capacity for pedestrian time carried across the cycle boundary. */
    int minimum = GREEN_MIN_SEC + state.ped_extra_ew_green;
    int maximum = GREEN_MAX_SEC - state.ped_extra_ns_green;
    if (ns < minimum) ns = minimum;
    if (ns > maximum) ns = maximum;
    state.cycle_ns_green = ns;
    state.cycle_ew_green = 2 * GREEN_BASE_SEC - ns;
    state.cycle_plan_valid = 1;
}

static int next_green_time_for_direction(direction_t direction) {
    phase_t phase = direction == DIR_NS ? PHASE_NS_GREEN : PHASE_EW_GREEN;
    int seconds;
    if (!state.cycle_plan_valid || phase == state.initial_phase)
        local_plan_cycle_locked();
    seconds = direction == DIR_NS ? state.cycle_ns_green : state.cycle_ew_green;

    if (direction == DIR_NS) {
        seconds += state.ped_extra_ns_green;
        state.ped_extra_ns_green = 0;
    } else {
        seconds += state.ped_extra_ew_green;
        state.ped_extra_ew_green = 0;
    }

    return clamp_green_time(seconds);
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

static void set_phase_locked(phase_t phase, int duration) {
    state.phase = phase;
    state.phase_duration = duration;
    state.time_remaining = duration;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    update_lights_locked();
    mark_status_dirty_locked();
}

void set_initial_phase_locked(void) {
    direction_t direction =
        state.initial_phase == PHASE_EW_GREEN ? DIR_EW : DIR_NS;
    set_phase_locked(state.initial_phase, next_green_time_for_direction(direction));
}

static void set_coordination_phase_locked(phase_t base_phase, int offset_sec) {
    state.ped_extra_ns_green = 0;
    state.ped_extra_ew_green = 0;
    state.cycle_ns_green = configured_green_time_for_direction(DIR_NS);
    state.cycle_ew_green = configured_green_time_for_direction(DIR_EW);
    state.cycle_plan_valid = 1;
    direction_t first_direction =
        base_phase == PHASE_NS_GREEN ? DIR_NS : DIR_EW;
    direction_t second_direction =
        base_phase == PHASE_NS_GREEN ? DIR_EW : DIR_NS;
    int first_green_sec = configured_green_time_for_direction(first_direction);
    int second_green_sec = configured_green_time_for_direction(second_direction);
    int cycle_sec = first_green_sec + second_green_sec + 2 * YELLOW_SEC;
    int offset = offset_sec % cycle_sec;
    phase_t first_green = base_phase;
    phase_t first_yellow =
        base_phase == PHASE_NS_GREEN ? PHASE_NS_YELLOW : PHASE_EW_YELLOW;
    phase_t second_green =
        base_phase == PHASE_NS_GREEN ? PHASE_EW_GREEN : PHASE_NS_GREEN;
    phase_t second_yellow =
        base_phase == PHASE_NS_GREEN ? PHASE_EW_YELLOW : PHASE_NS_YELLOW;

    if (offset < first_green_sec) {
        set_phase_locked(first_green, first_green_sec - offset);
    } else if (offset < first_green_sec + YELLOW_SEC) {
        set_phase_locked(first_yellow,
                         first_green_sec + YELLOW_SEC - offset);
    } else if (offset < first_green_sec + YELLOW_SEC + second_green_sec) {
        set_phase_locked(second_green,
                         first_green_sec + YELLOW_SEC + second_green_sec -
                         offset);
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
    /* Check stored outputs before the normal phase mapping can overwrite them. */
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
            if (state.sim_running && !state.train_connected) {
                state.next_train_in_seconds = random_train_gap_seconds();
                state.next_train_direction = random_train_direction();
            }
#endif
            set_initial_phase_locked();
        }
        return;
    }

    if (state.time_remaining > 0) {
        state.time_remaining--;
    }

    update_lights_locked();
    update_pedestrian_locked();


    if (state.time_remaining <= 0) {
        advance_phase_locked();
    }
}

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
        /* A repeated message must not skip or restart the clearing Yellow. */
        if (state.train_active) {
            set_phase_locked(PHASE_RAILWAY_HOLD, 0);
        }
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
