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

    /* Green reduces the count; red keeps cars waiting. */
    if (!state.train_pending && !state.train_active &&
        state.train_recovery_remaining <= 0) {
        if (state.phase == PHASE_NS_GREEN) {
            let_cars_pass_locked(DIR_NS);
        } else if (state.phase == PHASE_EW_GREEN) {
            let_cars_pass_locked(DIR_EW);
        }
    }

    /* Random arrivals increase NS, EW or both, up to MAX_SENSOR_CARS. */
    if (!state.manual_sensor_override) {
        if (state.next_car_in_seconds > 0) state.next_car_in_seconds--;
        if (state.next_car_in_seconds <= 0) {
            int arrival = rand() % 3; /* 0: NS, 1: EW, 2: both. */
            if (arrival == 0 || arrival == 2) add_car_locked(DIR_NS);
            if (arrival == 1 || arrival == 2) add_car_locked(DIR_EW);
            state.next_car_in_seconds = random_car_gap_seconds();
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
        !state.train_connected &&
        state.next_train_in_seconds > 0) {
        state.next_train_in_seconds--;
    }

    if (state.next_train_in_seconds <= 0 &&
        !state.train_pending &&
        !state.train_active &&
        !state.train_connected &&
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
    state.next_car_in_seconds = random_car_gap_seconds();
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
    state.next_car_in_seconds = random_car_gap_seconds();
    apply_time_settings_locked();
#else
    state.traffic_mode = MODE_FIXED;
#endif
    set_initial_phase_locked();
    mark_status_dirty_locked();
}
