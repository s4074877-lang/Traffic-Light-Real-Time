#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "../../common/common.h"
#include "../../common/communication/connection.h"
#include "../../common/communication/send.h"
#include "../../common/communication/receive.h"

#define DEMO_HIGH_CAR_COUNT 6
#define PED_GREEN_CAP_SEC 10
#define SIM_SECONDS_PER_TICK 1
#define SECONDS_PER_DAY 86400
#define TRAIN_PASSING_SECONDS 10
#define MAX_SENSOR_CARS 12
#define LOCAL_INTERSECTION_ID I1
#define STATUS_PERIOD_SEC 1

/*
 * Demo switches:
 * Change 0 to 1 when you want these parts back for testing.
 */
#define ENABLE_DEMO_COMMANDS 0
#define ENABLE_TRAFFIC_SIMULATION 1

// Controller state
typedef struct {
    // Connections
    connection_t central_conn;
    connection_t train_conn;
    connection_mode_t mode;

    // Timestamps
    char last_recv_central[32];
    char last_recv_train[32];
    char last_send_central[32];
    char last_send_train[32];
    char last_central_update[32];
    char last_train_update[32];

    // Traffic light logic
    traffic_light_mode traffic_mode;
    phase_t phase;
    light_state_t ns_light;
    light_state_t ew_light;
    int time_remaining;
    int phase_duration;

    int sensor_ns_count;
    int sensor_ew_count;
    int next_ns_car_in_seconds;
    int next_ew_car_in_seconds;

    int ped_ns_request;
    int ped_ew_request;
    int ped_ns_walk;
    int ped_ew_walk;

    int sim_seconds;
    int next_train_in_seconds;
    int next_train_direction;
    int train_direction;
    int train_pass_remaining;
    int train_waiting_for_clear;
    int manual_mode_override;
    int manual_sensor_override;

    int temp_mode_remaining;
    traffic_light_mode temp_return_mode;
    int temp_return_manual_override;
    int coordination_pending;
    phase_t coordination_phase;
    int ped_extra_ns_green;
    int ped_extra_ew_green;

    uint8_t intersection_id;
    int fault_active;
    int fault_pending;
    fault_type_t fault_type;
    fault_severity_t fault_severity;
    char fault_description[32];

    int train_pending;
    int train_active;
    int train_recovery_remaining;

    // Flags
    int ui_needs_update;
    int status_dirty;

    pthread_mutex_t mutex;
    pthread_mutex_t central_send_mutex;
    pthread_mutex_t train_send_mutex;
} local_state_t;

static local_state_t state;
static name_attach_t *attach = NULL;

static void start_train_locked(int direction);
static void clear_train_locked(void);
static void mark_status_dirty_locked(void);
static void apply_time_settings_locked(void);

static void print_usage(const char *prog) {
    printf("Usage: %s [-l | -g]\n", prog);
    printf("  -l  Local mode (single VM testing)\n");
    printf("  -g  Global mode (multi VM with GNS) [default]\n");
}

// Clear screen
static void clear_screen(void) {
    printf("\033[2J\033[H");
}

static const char* mode_text(traffic_light_mode mode) {
    switch (mode) {
        case MODE_FIXED:    return "FIXED";
        case MODE_SENSOR:   return "SENSOR";
        case MODE_RAILWAY:  return "RAILWAY";
        case MODE_FAILSAFE: return "FAILSAFE";
        default:            return "UNKNOWN";
    }
}

static const char* light_text(light_state_t light) {
    switch (light) {
        case LIGHT_OFF:    return "OFF";
        case LIGHT_RED:    return "RED";
        case LIGHT_YELLOW: return "YELLOW";
        case LIGHT_GREEN:  return "GREEN";
        default:           return "UNKNOWN";
    }
}

static int is_peak_time(int seconds) {
    int hour = seconds / 3600;
    return (hour >= 7 && hour < 9) || (hour >= 16 && hour < 18);
}

static int is_night_time(int seconds) {
    int hour = seconds / 3600;
    return hour >= 22 || hour < 5;
}

static const char* period_text(int seconds) {
    if (is_peak_time(seconds)) {
        return "PEAK";
    }
    if (is_night_time(seconds)) {
        return "NIGHT";
    }
    return "OFFPEAK";
}

static void format_sim_time(char *buf, size_t len) {
    int hour = state.sim_seconds / 3600;
    int minute = (state.sim_seconds % 3600) / 60;
    int second = state.sim_seconds % 60;
    snprintf(buf, len, "%02d:%02d:%02d", hour, minute, second);
}

static int random_train_gap_seconds(void) {
    if (is_peak_time(state.sim_seconds)) {
        return 120 + rand() % 181;  // 2-5 minutes at peak
    }
    if (is_night_time(state.sim_seconds)) {
        return 1080 + rand() % 361; // about 18-24 minutes at night
    }
    return 480 + rand() % 421;      // 8-15 minutes during the day
}

static int random_train_direction(void) {
    return (rand() % 2) + 1;
}

static int random_car_gap_seconds(void) {
    if (is_peak_time(state.sim_seconds)) {
        return 2 + rand() % 3;   // 2-4 seconds at peak time
    }
    if (is_night_time(state.sim_seconds)) {
        return 14 + rand() % 12; // 14-25 seconds at night
    }
    return 5 + rand() % 6;       // 5-10 seconds during normal hours
}

static traffic_light_mode display_mode(void) {
    if (state.train_pending || state.train_active ||
        state.train_recovery_remaining > 0) {
        return MODE_RAILWAY;
    }
    return state.traffic_mode;
}

static void mark_status_dirty_locked(void) {
    state.status_dirty = 1;
    state.ui_needs_update = 1;
}

static int target_matches_local(uint8_t target) {
    return target == state.intersection_id || target == INTERSECTION_ALL;
}

static int railway_matches_local(uint8_t id) {
    int crossing_id;
    int first_intersection;

    if (id == state.intersection_id) {
        return 1;
    }

    if (id >= 1 && id <= NUM_CROSSINGS) {
        crossing_id = id - 1;
    } else if (id < NUM_CROSSINGS) {
        crossing_id = id;
    } else {
        return 0;
    }

    first_intersection = crossing_id * 2;
    return state.intersection_id == first_intersection ||
           state.intersection_id == first_intersection + 1;
}

static void init_message(test_message_t *msg, msg_type_t type,
                         controller_type_t src, controller_type_t dst) {
    memset(msg, 0, sizeof(*msg));
    msg->header.type = type;
    msg->header.src = src;
    msg->header.dst = dst;
    get_timestamp(msg->header.timestamp, sizeof(msg->header.timestamp));
}

static void fill_status_locked(status_msg_t *status) {
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

static void prepare_status_message_locked(test_message_t *msg) {
    status_msg_t status;
    init_message(msg, MSG_STATUS_UPDATE, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
    fill_status_locked(&status);
    memcpy(msg->data, &status, sizeof(status));
    state.status_dirty = 0;
}

static void set_fault_locked(fault_type_t type, fault_severity_t severity,
                             const char *description) {
    state.fault_active = type != FAULT_NONE;
    state.fault_pending = 1;
    state.fault_type = type;
    state.fault_severity = severity;
    snprintf(state.fault_description, sizeof(state.fault_description),
             "%s", description);
    mark_status_dirty_locked();
}

static void prepare_fault_message_locked(test_message_t *msg) {
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

static int clamp_green_time(int seconds) {
    if (seconds < GREEN_MIN_SEC) {
        return GREEN_MIN_SEC;
    }
    if (seconds > GREEN_MAX_SEC) {
        return GREEN_MAX_SEC;
    }
    return seconds;
}

static int green_time_for_direction(direction_t direction) {
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

static int phase_matches_direction(phase_t phase, direction_t direction) {
    return (phase == PHASE_NS_GREEN && direction == DIR_NS) ||
           (phase == PHASE_EW_GREEN && direction == DIR_EW);
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

static void set_phase_locked(phase_t phase, int duration) {
    state.phase = phase;
    state.phase_duration = duration;
    state.time_remaining = duration;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    update_lights_locked();
    mark_status_dirty_locked();
}

static void advance_phase_locked(void) {
    if (state.coordination_pending &&
        (state.phase == PHASE_NS_YELLOW || state.phase == PHASE_EW_YELLOW)) {
        set_phase_locked(state.coordination_phase,
                         next_green_time_for_direction(state.coordination_phase == PHASE_NS_GREEN ?
                                                       DIR_NS : DIR_EW));
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

static void traffic_tick_locked(void) {
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
            if (!state.train_conn.connected) {
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

static void update_temporary_mode_locked(void) {
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

static void apply_time_settings_locked(void) {
    if (!state.manual_mode_override) {
        state.traffic_mode = is_peak_time(state.sim_seconds) ?
            MODE_FIXED : MODE_SENSOR;
        mark_status_dirty_locked();
    }
}

static void update_vehicle_counts_locked(void) {
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

static void update_time_of_day_locked(void) {
#if ENABLE_TRAFFIC_SIMULATION
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

static void set_sim_hour_locked(int hour) {
    state.sim_seconds = hour * 3600;
    state.manual_mode_override = 0;
    state.manual_sensor_override = 0;
    apply_time_settings_locked();
#if ENABLE_TRAFFIC_SIMULATION
    state.next_train_in_seconds = random_train_gap_seconds();
    state.next_train_direction = random_train_direction();
    state.next_ns_car_in_seconds = random_car_gap_seconds();
    state.next_ew_car_in_seconds = random_car_gap_seconds();
#endif
}

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

static void add_pedestrian_request_locked(direction_t direction) {
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

static void toggle_sensor_locked(direction_t direction) {
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

static void start_train_locked(int direction) {
    start_train_common_locked(direction, 0, 0);
}

static void start_train_message_locked(int direction, int eta_seconds) {
    if (eta_seconds < 0) {
        eta_seconds = 0;
    }
    start_train_common_locked(direction, 1, eta_seconds);
}

static void clear_train_locked(void) {
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

static void reset_demo_inputs_locked(void) {
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
    state.ped_extra_ns_green = 0;
    state.ped_extra_ew_green = 0;
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

// Display UI
static void display_ui(void) {
#if ENABLE_TRAFFIC_SIMULATION
    char sim_time[16];
#endif

    pthread_mutex_lock(&state.mutex);
#if ENABLE_TRAFFIC_SIMULATION
    format_sim_time(sim_time, sizeof(sim_time));
#endif

    clear_screen();

    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s                    LOCAL CONTROLLER%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Connection status
    printf("Connected to central_controller [%s%s%s] last update [%s]\n",
           state.central_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.central_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_central_update[0] ? state.last_central_update : "N/A");

    printf("Connected to train_controller [%s%s%s] last update [%s]\n",
           state.train_conn.connected ? COLOR_GREEN : COLOR_RED,
           state.train_conn.connected ? "CONNECTED" : "DISCONNECTED",
           COLOR_RESET,
           state.last_train_update[0] ? state.last_train_update : "N/A");

    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);

    // Message timestamps
    printf("last message receive central [%s]\n",
           state.last_recv_central[0] ? state.last_recv_central : "N/A");
    printf("last message receive train [%s]\n",
           state.last_recv_train[0] ? state.last_recv_train : "N/A");
    printf("message send central [%s]\n",
           state.last_send_central[0] ? state.last_send_central : "N/A");
    printf("message send train [%s]\n",
           state.last_send_train[0] ? state.last_send_train : "N/A");

    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Traffic mode [%s]\n", mode_text(display_mode()));
#if ENABLE_TRAFFIC_SIMULATION
    printf("Sim time [%s] period [%s]\n",
           sim_time, period_text(state.sim_seconds));
#endif
    printf("Vehicle lights: NS [%s]  EW [%s]\n",
           light_text(state.ns_light), light_text(state.ew_light));
    printf("Pedestrian: NS [%s%s]  EW [%s%s]\n",
           state.ped_ns_walk ? "WALK" : "STOP",
           state.ped_ns_request ? ", requested" : "",
           state.ped_ew_walk ? "WALK" : "STOP",
           state.ped_ew_request ? ", requested" : "");
#if ENABLE_TRAFFIC_SIMULATION || ENABLE_DEMO_COMMANDS
    printf("Sensors: NS cars [%d]%s  EW cars [%d]%s\n",
           state.sensor_ns_count,
           state.sensor_ns_count >= SENSOR_CAR_THRESHOLD ? " SENSOR-DETECTED" : "",
           state.sensor_ew_count,
           state.sensor_ew_count >= SENSOR_CAR_THRESHOLD ? " SENSOR-DETECTED" : "");

    if (state.train_pending) {
        printf("Train: line %d approaching, clearing road in [%d] sec\n",
               state.train_direction, state.time_remaining);
        printf("Next train: countdown starts after current train clears\n");
    } else if (state.train_active) {
        if (state.train_waiting_for_clear) {
            printf("Train: line %d active, waiting for TRAIN_CLEAR",
                   state.train_direction);
            if (state.train_pass_remaining > 0) {
                printf(" eta [%d] sec", state.train_pass_remaining);
            }
            printf("\n");
        } else {
            printf("Train: line %d at crossing, clear in [%d] sec\n",
                   state.train_direction, state.train_pass_remaining);
        }
        printf("Next train: countdown starts after current train clears\n");
    } else if (state.train_recovery_remaining > 0) {
        printf("Train: clear, recovery remaining [%d] sec\n",
               state.train_recovery_remaining);
        printf("Next train: countdown starts after recovery\n");
    } else {
        printf("Train: none active\n");
        if (state.train_conn.connected) {
            printf("Railway source: train_controller messages\n");
        } else {
            printf("Next train: line %d in [%d] sec\n",
                   state.next_train_direction, state.next_train_in_seconds);
        }
    }
#endif

    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Message send: send-central | send-train\n");
#if ENABLE_DEMO_COMMANDS
    printf("Commands: m mode | n ped-NS | e ped-EW | x sensor-NS | z sensor-EW\n");
#if ENABLE_TRAFFIC_SIMULATION
    printf("          1 train-line1 | 2 train-line2 | p peak | o offpeak | l night\n");
#else
    printf("          1 train-line1 | 2 train-line2\n");
#endif
    printf("          c train-clear | r reset | q quit\n");
#else
    printf("Demo commands are commented out. Set ENABLE_DEMO_COMMANDS to 1 to use them.\n");
    printf("Commands: send-central | send-train | q quit\n");
#endif
    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("\n%sMessage:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);

    state.ui_needs_update = 0;
    pthread_mutex_unlock(&state.mutex);
}

// Handler for test messages
static int handle_test_message(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_CENTRAL) {
        strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
    } else if (msg->header.src == CONTROLLER_TRAIN) {
        strncpy(s->last_recv_train, msg->header.timestamp, sizeof(s->last_recv_train) - 1);
        get_timestamp(s->last_train_update, sizeof(s->last_train_update));
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_mode_command(int rcvid, test_message_t *msg, reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;
    mode_cmd_msg_t command;
    int accepted = 0;

    memcpy(&command, msg->data, sizeof(command));
    reply->command_id = command.command_id;

    pthread_mutex_lock(&s->mutex);
    strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
    get_timestamp(s->last_central_update, sizeof(s->last_central_update));

    if (msg->header.src == CONTROLLER_CENTRAL &&
        target_matches_local(command.intersection_id) &&
        command.command_id != 0 &&
        command.new_mode <= MODE_SENSOR &&
        command.priority >= CMD_PRIO_SCHEDULE &&
        command.priority <= CMD_PRIO_OPERATOR &&
        command.action <= CMD_REVERT) {
        if (command.action == CMD_SET_MODE && command.duration_sec == 0) {
            s->traffic_mode = command.new_mode;
            s->manual_mode_override = 1;
            s->temp_mode_remaining = 0;
            accepted = 1;
        } else if (command.action == CMD_TEMPORARY && command.duration_sec > 0) {
            s->temp_return_mode = s->traffic_mode;
            s->temp_return_manual_override = s->manual_mode_override;
            s->traffic_mode = command.new_mode;
            s->manual_mode_override = 1;
            s->temp_mode_remaining = command.duration_sec;
            accepted = 1;
        } else if (command.action == CMD_REVERT && command.duration_sec == 0) {
            if (s->temp_mode_remaining > 0) {
                s->traffic_mode = s->temp_return_mode;
                s->manual_mode_override = s->temp_return_manual_override;
                s->temp_mode_remaining = 0;
            } else {
                s->manual_mode_override = 0;
                apply_time_settings_locked();
            }
            accepted = 1;
        }

        if (accepted) {
            mark_status_dirty_locked();
        }
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_coordination_command(int rcvid, test_message_t *msg,
                                       reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;
    coordination_command_msg_t command;
    int accepted = 0;

    memcpy(&command, msg->data, sizeof(command));
    reply->command_id = command.command_id;

    pthread_mutex_lock(&s->mutex);
    strncpy(s->last_recv_central, msg->header.timestamp, sizeof(s->last_recv_central) - 1);
    get_timestamp(s->last_central_update, sizeof(s->last_central_update));

    if (msg->header.src == CONTROLLER_CENTRAL &&
        target_matches_local(command.intersection_id) &&
        command.command_id != 0 &&
        command.mode == MODE_FIXED &&
        command.reserved == 0 &&
        command.cycle_offset_sec < 2 * (GREEN_BASE_SEC + YELLOW_SEC) &&
        (command.phase == PHASE_NS_GREEN || command.phase == PHASE_EW_GREEN)) {
        s->traffic_mode = MODE_FIXED;
        s->manual_mode_override = 1;
        s->coordination_phase = command.phase;
        s->coordination_pending =
            !(s->phase == command.phase && s->time_remaining > 0);
        accepted = 1;
        mark_status_dirty_locked();
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_sensor_update(int rcvid, test_message_t *msg,
                                reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;
    sensor_msg_t sensor;
    int accepted = 0;

    memcpy(&sensor, msg->data, sizeof(sensor));

    pthread_mutex_lock(&s->mutex);
    if (target_matches_local(sensor.intersection_id) &&
        sensor.direction <= DIR_EW) {
        s->manual_sensor_override = 1;
        if (sensor.direction == DIR_NS) {
            s->sensor_ns_count = sensor.car_count;
        } else {
            s->sensor_ew_count = sensor.car_count;
        }
        accepted = 1;
        mark_status_dirty_locked();
    }
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_ped_request(int rcvid, test_message_t *msg,
                              reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;
    ped_msg_t ped;
    int accepted = 0;

    memcpy(&ped, msg->data, sizeof(ped));

    pthread_mutex_lock(&s->mutex);
    if (target_matches_local(ped.intersection_id) &&
        ped.direction <= DIR_EW && ped.pressed) {
        add_pedestrian_request_locked((direction_t)ped.direction);
        accepted = 1;
    }
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_railway_message(int rcvid, test_message_t *msg,
                                  reply_t *reply, void *ctx) {
    (void)rcvid;
    local_state_t *s = (local_state_t *)ctx;
    railway_msg_t railway;
    int accepted = 0;

    memcpy(&railway, msg->data, sizeof(railway));

    pthread_mutex_lock(&s->mutex);
    if (msg->header.src == CONTROLLER_TRAIN &&
        railway_matches_local(railway.intersection_id)) {
        strncpy(s->last_recv_train, msg->header.timestamp, sizeof(s->last_recv_train) - 1);
        get_timestamp(s->last_train_update, sizeof(s->last_train_update));

        if (msg->header.type == MSG_RAILWAY_PREEMPT && railway.active) {
            start_train_message_locked(railway.intersection_id,
                                       railway.eta_seconds);
            accepted = 1;
        } else if (msg->header.type == MSG_TRAIN_CLEAR) {
            clear_train_locked();
            accepted = 1;
        }
    }
    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

// Message handlers array
static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test_message },
    { MSG_MODE_COMMAND, CONTROLLER_CENTRAL, handle_mode_command },
    { MSG_COORDINATION_COMMAND, CONTROLLER_CENTRAL, handle_coordination_command },
    { MSG_SENSOR_UPDATE, 0, handle_sensor_update },
    { MSG_PED_REQUEST, 0, handle_ped_request },
    { MSG_RAILWAY_PREEMPT, CONTROLLER_TRAIN, handle_railway_message },
    { MSG_TRAIN_CLEAR, CONTROLLER_TRAIN, handle_railway_message }
};

// Thread to handle incoming messages
static void* message_handler_thread(void *arg) {
    receive_context_t *ctx = (receive_context_t *)arg;
    receive_loop(ctx);
    return NULL;
}

// Thread to manage connections
static void* connection_thread(void *arg) {
    (void)arg;

    while (1) {
        // Try connecting to central
        if (connection_try_connect(&state.central_conn)) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_central_update, sizeof(state.last_central_update));
            mark_status_dirty_locked();
            pthread_mutex_unlock(&state.mutex);
        }

        // Try connecting to train
        if (connection_try_connect(&state.train_conn)) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_train_update, sizeof(state.last_train_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Send initial message to notify train we're connected
            pthread_mutex_lock(&state.train_send_mutex);
            send_test_message(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
            pthread_mutex_unlock(&state.train_send_mutex);
        }

        sleep(2);
    }

    return NULL;
}

// Thread to refresh UI periodically
static void* ui_refresh_thread(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&state.mutex);
        int needs_update = state.ui_needs_update;
        pthread_mutex_unlock(&state.mutex);

        if (needs_update) {
            display_ui();
        }

        sleep(UI_CHECK_INTERVAL);
    }

    return NULL;
}

// Thread to run the local traffic light state machine
static void* traffic_thread(void *arg) {
    (void)arg;

    while (1) {
        sleep(1);

        pthread_mutex_lock(&state.mutex);
#if ENABLE_TRAFFIC_SIMULATION
        update_time_of_day_locked();
#endif
        update_temporary_mode_locked();
        traffic_tick_locked();
#if ENABLE_TRAFFIC_SIMULATION
        update_vehicle_counts_locked();
#endif
        mark_status_dirty_locked();
        pthread_mutex_unlock(&state.mutex);
    }

    return NULL;
}

// Thread to publish Local status/faults to Central
static void* status_thread(void *arg) {
    (void)arg;

    while (1) {
        test_message_t status_msg;
        test_message_t fault_msg;
        reply_t reply;
        int send_status = 0;
        int send_fault = 0;

        sleep(STATUS_PERIOD_SEC);

        pthread_mutex_lock(&state.mutex);
        if (state.central_conn.connected) {
            prepare_status_message_locked(&status_msg);
            send_status = 1;

            if (state.fault_pending) {
                prepare_fault_message_locked(&fault_msg);
                send_fault = 1;
            }
        }
        pthread_mutex_unlock(&state.mutex);

        if (send_fault || send_status) {
            pthread_mutex_lock(&state.central_send_mutex);

            if (send_fault && send_message(&state.central_conn, &fault_msg, &reply) == 0 &&
                reply.status == 0) {
                pthread_mutex_lock(&state.mutex);
                state.fault_pending = 0;
                get_timestamp(state.last_send_central, sizeof(state.last_send_central));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }

            if (send_status && send_message(&state.central_conn, &status_msg, &reply) == 0 &&
                reply.status == 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_send_central, sizeof(state.last_send_central));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }

            pthread_mutex_unlock(&state.central_send_mutex);
        }
    }

    return NULL;
}

// Thread to check connection health via heartbeat
static void* heartbeat_thread(void *arg) {
    (void)arg;

    while (1) {
        sleep(HEARTBEAT_INTERVAL);

        // Check central connection
        if (connection_is_connected(&state.central_conn)) {
            int heartbeat_failed;
            pthread_mutex_lock(&state.central_send_mutex);
            heartbeat_failed =
                send_heartbeat(&state.central_conn, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
            pthread_mutex_unlock(&state.central_send_mutex);

            if (heartbeat_failed != 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_central_update, sizeof(state.last_central_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }
        }

        // Check train connection
        if (connection_is_connected(&state.train_conn)) {
            int heartbeat_failed;
            pthread_mutex_lock(&state.train_send_mutex);
            heartbeat_failed =
                send_heartbeat(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
            pthread_mutex_unlock(&state.train_send_mutex);

            if (heartbeat_failed != 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_train_update, sizeof(state.last_train_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }
        }
    }

    return NULL;
}

// Execute command
static int execute_command(const char *cmd) {
#if ENABLE_DEMO_COMMANDS
    if (strcmp(cmd, "m") == 0) {
        pthread_mutex_lock(&state.mutex);
        state.manual_mode_override = 1;
        state.traffic_mode = state.traffic_mode == MODE_FIXED ? MODE_SENSOR : MODE_FIXED;
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "n") == 0) {
        pthread_mutex_lock(&state.mutex);
        add_pedestrian_request_locked(DIR_NS);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "e") == 0) {
        pthread_mutex_lock(&state.mutex);
        add_pedestrian_request_locked(DIR_EW);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "x") == 0) {
        pthread_mutex_lock(&state.mutex);
        toggle_sensor_locked(DIR_NS);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "z") == 0) {
        pthread_mutex_lock(&state.mutex);
        toggle_sensor_locked(DIR_EW);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "1") == 0) {
        pthread_mutex_lock(&state.mutex);
        start_train_locked(1);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "2") == 0) {
        pthread_mutex_lock(&state.mutex);
        start_train_locked(2);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
#if ENABLE_TRAFFIC_SIMULATION
    } else if (strcmp(cmd, "p") == 0) {
        pthread_mutex_lock(&state.mutex);
        set_sim_hour_locked(7);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "o") == 0) {
        pthread_mutex_lock(&state.mutex);
        set_sim_hour_locked(12);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "l") == 0) {
        pthread_mutex_lock(&state.mutex);
        set_sim_hour_locked(22);
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
#endif
    } else if (strcmp(cmd, "c") == 0) {
        pthread_mutex_lock(&state.mutex);
        clear_train_locked();
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else if (strcmp(cmd, "r") == 0) {
        pthread_mutex_lock(&state.mutex);
        reset_demo_inputs_locked();
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
    } else
#endif
    if (strcmp(cmd, "send-central") == 0) {
        pthread_mutex_lock(&state.mutex);
        int connected = state.central_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected) {
            printf("%sCentral controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        pthread_mutex_lock(&state.central_send_mutex);
        int sent = send_test_message(&state.central_conn, CONTROLLER_LOCAL, CONTROLLER_CENTRAL);
        pthread_mutex_unlock(&state.central_send_mutex);

        if (sent == 0) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_central, sizeof(state.last_send_central));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        } else {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    } else if (strcmp(cmd, "send-train") == 0) {
        pthread_mutex_lock(&state.mutex);
        int connected = state.train_conn.connected;
        pthread_mutex_unlock(&state.mutex);

        if (!connected) {
            printf("%sTrain controller not connected%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }

        pthread_mutex_lock(&state.train_send_mutex);
        int sent = send_test_message(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
        pthread_mutex_unlock(&state.train_send_mutex);

        if (sent == 0) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_send_train, sizeof(state.last_send_train));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        } else {
            printf("%sFailed to send message%s\n", COLOR_RED, COLOR_RESET);
            return -1;
        }
    } else {
#if ENABLE_DEMO_COMMANDS
#if ENABLE_TRAFFIC_SIMULATION
        printf("%sUnknown command. Use one of: m n e x z 1 2 p o l c r q%s\n",
               COLOR_RED, COLOR_RESET);
#else
        printf("%sUnknown command. Use one of: m n e x z 1 2 c r q%s\n",
               COLOR_RED, COLOR_RESET);
#endif
#else
        printf("%sUnknown command. Demo commands are disabled. Use: send-central | send-train | q%s\n",
               COLOR_RED, COLOR_RESET);
#endif
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Parse command line arguments
    connection_mode_t mode = connection_parse_args(argc, argv);

    // Check for help
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    // Initialize state
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

    // Register with name service
    attach = connection_register_service(LOCAL_SERVICE_NAME, mode);
    if (attach == NULL) {
        return EXIT_FAILURE;
    }

    // Initialize receive context
    receive_context_t recv_ctx;
    receive_init(&recv_ctx, attach, handlers,
                 sizeof(handlers) / sizeof(handlers[0]), &state);

    // Start threads
    pthread_t msg_thread, conn_thread, ui_thread, hb_thread;
    pthread_t traffic_thread_id, status_thread_id;

    if (pthread_create(&msg_thread, NULL, message_handler_thread, &recv_ctx) != 0) {
        fprintf(stderr, "Failed to create message handler thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&conn_thread, NULL, connection_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create connection thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&ui_thread, NULL, ui_refresh_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create UI thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&hb_thread, NULL, heartbeat_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create heartbeat thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&traffic_thread_id, NULL, traffic_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create traffic thread\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&status_thread_id, NULL, status_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create status thread\n");
        return EXIT_FAILURE;
    }

    // Initial UI display
    display_ui();

    // Main loop: read commands
    char cmd[64];
    while (1) {
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            cmd[strcspn(cmd, "\n")] = '\0';

            if (strlen(cmd) == 0) {
                display_ui();
                continue;
            }

            if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
                printf("Exiting...\n");
                break;
            }

            execute_command(cmd);
            sleep(1);
            display_ui();
        }
    }

    // Cleanup
    connection_close(&state.central_conn);
    connection_close(&state.train_conn);
    connection_unregister_service(attach);
    pthread_mutex_destroy(&state.mutex);
    pthread_mutex_destroy(&state.central_send_mutex);
    pthread_mutex_destroy(&state.train_send_mutex);
    return EXIT_SUCCESS;
}
