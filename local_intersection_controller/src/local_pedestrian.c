#include "local_controller.h"

static int pedestrian_service_phase(phase_t phase, direction_t direction) {
    return (phase == PHASE_EW_GREEN && direction == DIR_NS) ||
           (phase == PHASE_NS_GREEN && direction == DIR_EW);
}

void update_pedestrian_locked(void) {
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
        } else {
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
        } else {
            state.ped_ew_walk = 0;
        }
    } else {
        state.ped_ew_walk = 0;
    }
}

static void cap_green_for_pedestrian_locked(direction_t direction) {
    int elapsed;
    int removed;
    int capacity;

    if (!pedestrian_service_phase(state.phase, direction) ||
        state.time_remaining <= PED_GREEN_CAP_SEC) {
        return;
    }

    elapsed = state.phase_duration - state.time_remaining;
    removed = state.time_remaining - PED_GREEN_CAP_SEC;
    /* A transfer to the second phase must fit its already allocated Green.
     * Across a boundary, the next cycle reserves room before sampling sensors. */
    if (state.phase == state.initial_phase && state.cycle_plan_valid) {
        int next = direction == DIR_NS ? state.cycle_ns_green : state.cycle_ew_green;
        int extra = direction == DIR_NS ? state.ped_extra_ns_green : state.ped_extra_ew_green;
        capacity = GREEN_MAX_SEC - next - extra;
    } else {
        capacity = GREEN_MAX_SEC - GREEN_MIN_SEC -
                   state.ped_extra_ns_green - state.ped_extra_ew_green;
    }
    if (capacity < 0) capacity = 0;
    if (removed > capacity) removed = capacity;
    state.time_remaining -= removed;
    state.phase_duration = elapsed + state.time_remaining;

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
