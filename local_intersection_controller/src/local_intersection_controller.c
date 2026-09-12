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
    int manual_mode_override;
    int manual_sensor_override;

    int train_pending;
    int train_active;
    int train_recovery_remaining;

    // Flags
    int ui_needs_update;

    pthread_mutex_t mutex;
} local_state_t;

static local_state_t state;
static name_attach_t *attach = NULL;

static void start_train_locked(int direction);
static void clear_train_locked(void);

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
        return 3 + rand() % 4;   // 3-6 seconds at peak time
    }
    if (is_night_time(state.sim_seconds)) {
        return 20 + rand() % 16; // 20-35 seconds at night
    }
    return 7 + rand() % 9;       // 7-15 seconds during normal hours
}

static traffic_light_mode display_mode(void) {
    if (state.train_pending || state.train_active ||
        state.train_recovery_remaining > 0) {
        return MODE_RAILWAY;
    }
    return state.traffic_mode;
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

static int phase_matches_direction(phase_t phase, direction_t direction) {
    return (phase == PHASE_NS_GREEN && direction == DIR_NS) ||
           (phase == PHASE_EW_GREEN && direction == DIR_EW);
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

    if (state.phase == PHASE_NS_GREEN && state.ped_ns_request) {
        if (elapsed >= PED_WALK_START_SEC && state.time_remaining > PED_WALK_END_SEC) {
            state.ped_ns_walk = 1;
        }
        if (state.ped_ns_walk && state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ns_walk = 0;
            state.ped_ns_request = 0;
        }
    }

    if (state.phase == PHASE_EW_GREEN && state.ped_ew_request) {
        if (elapsed >= PED_WALK_START_SEC && state.time_remaining > PED_WALK_END_SEC) {
            state.ped_ew_walk = 1;
        }
        if (state.ped_ew_walk && state.time_remaining <= PED_WALK_END_SEC) {
            state.ped_ew_walk = 0;
            state.ped_ew_request = 0;
        }
    }
}

static void set_phase_locked(phase_t phase, int duration) {
    state.phase = phase;
    state.phase_duration = duration;
    state.time_remaining = duration;
    state.ped_ns_walk = 0;
    state.ped_ew_walk = 0;
    update_lights_locked();
}

static void advance_phase_locked(void) {
    switch (state.phase) {
        case PHASE_NS_GREEN:
            set_phase_locked(PHASE_NS_YELLOW, YELLOW_SEC);
            break;
        case PHASE_NS_YELLOW:
            set_phase_locked(PHASE_EW_GREEN, green_time_for_direction(DIR_EW));
            break;
        case PHASE_EW_GREEN:
            set_phase_locked(PHASE_EW_YELLOW, YELLOW_SEC);
            break;
        case PHASE_EW_YELLOW:
        default:
            set_phase_locked(PHASE_NS_GREEN, green_time_for_direction(DIR_NS));
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
            state.next_train_in_seconds = random_train_gap_seconds();
            state.next_train_direction = random_train_direction();
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

    if (state.time_remaining <= 0) {
        advance_phase_locked();
    }
}

static void apply_time_settings_locked(void) {
    if (!state.manual_mode_override) {
        state.traffic_mode = is_peak_time(state.sim_seconds) ?
            MODE_FIXED : MODE_SENSOR;
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
        state.next_train_in_seconds > 0) {
        state.next_train_in_seconds--;
    }

    if (state.next_train_in_seconds <= 0 &&
        !state.train_pending &&
        !state.train_active &&
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

static void add_pedestrian_request_locked(direction_t direction) {
    if (direction == DIR_NS) {
        state.ped_ns_request = 1;
    } else {
        state.ped_ew_request = 1;
    }

    if (phase_matches_direction(state.phase, direction) &&
        state.time_remaining > PED_GREEN_CAP_SEC) {
        state.time_remaining = PED_GREEN_CAP_SEC;
    }
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
}

static void start_train_locked(int direction) {
    if (state.train_active || state.train_pending) {
        return;
    }

    state.train_pending = 1;
    state.train_direction = direction;
    state.train_pass_remaining = TRAIN_PASSING_SECONDS;
    state.train_recovery_remaining = 0;
    state.next_train_in_seconds = 0;

    if (state.phase == PHASE_NS_GREEN) {
        set_phase_locked(PHASE_NS_YELLOW, YELLOW_SEC);
    } else if (state.phase == PHASE_EW_GREEN) {
        set_phase_locked(PHASE_EW_YELLOW, YELLOW_SEC);
    } else if (state.phase != PHASE_NS_YELLOW && state.phase != PHASE_EW_YELLOW) {
        state.train_pending = 0;
        state.train_active = 1;
        set_phase_locked(PHASE_RAILWAY_HOLD, 0);
    }
}

static void clear_train_locked(void) {
    if (state.train_active || state.train_pending) {
        state.train_pending = 0;
        state.train_active = 0;
        state.train_direction = 0;
        state.train_pass_remaining = 0;
        state.train_recovery_remaining = RAILWAY_RECOVERY_SEC;
        set_phase_locked(PHASE_RAILWAY_HOLD, RAILWAY_RECOVERY_SEC);
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
    state.train_recovery_remaining = 0;
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
        printf("Train: line %d at crossing, clear in [%d] sec\n",
               state.train_direction, state.train_pass_remaining);
        printf("Next train: countdown starts after current train clears\n");
    } else if (state.train_recovery_remaining > 0) {
        printf("Train: clear, recovery remaining [%d] sec\n",
               state.train_recovery_remaining);
        printf("Next train: countdown starts after recovery\n");
    } else {
        printf("Train: none active\n");
        printf("Next train: line %d in [%d] sec\n",
               state.next_train_direction, state.next_train_in_seconds);
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

// Message handlers array
static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test_message }  // 0 = accept from any controller
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
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);
        }

        // Try connecting to train
        if (connection_try_connect(&state.train_conn)) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_train_update, sizeof(state.last_train_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            // Send initial message to notify train we're connected
            send_test_message(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
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
        traffic_tick_locked();
#if ENABLE_TRAFFIC_SIMULATION
        update_vehicle_counts_locked();
#endif
        state.ui_needs_update = 1;
        pthread_mutex_unlock(&state.mutex);
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
            if (send_heartbeat(&state.central_conn, CONTROLLER_LOCAL, CONTROLLER_CENTRAL) != 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_central_update, sizeof(state.last_central_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }
        }

        // Check train connection
        if (connection_is_connected(&state.train_conn)) {
            if (send_heartbeat(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN) != 0) {
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

        if (send_test_message(&state.central_conn, CONTROLLER_LOCAL, CONTROLLER_CENTRAL) == 0) {
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

        if (send_test_message(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN) == 0) {
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
    state.mode = mode;
    state.ui_needs_update = 1;
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
    pthread_t msg_thread, conn_thread, ui_thread, hb_thread, traffic_thread_id;

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
    return EXIT_SUCCESS;
}
