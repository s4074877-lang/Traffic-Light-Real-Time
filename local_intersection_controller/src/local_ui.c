#include "local_controller.h"

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

static const char* phase_text(phase_t phase) {
    switch (phase) {
        case PHASE_NS_GREEN:     return "NS_GREEN";
        case PHASE_NS_YELLOW:    return "NS_YELLOW";
        case PHASE_EW_GREEN:     return "EW_GREEN";
        case PHASE_EW_YELLOW:    return "EW_YELLOW";
        case PHASE_RAILWAY_HOLD: return "RAILWAY_HOLD";
        default:                 return "UNKNOWN";
    }
}

#if ENABLE_TRAFFIC_SIMULATION
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
#endif

static void format_vehicle_signal(char *buf, size_t len,
                                  light_state_t light, int seconds) {
    if (seconds < 0) {
        snprintf(buf, len, "%s WAIT", light_text(light));
    } else {
        snprintf(buf, len, "%s %d sec", light_text(light), seconds);
    }
}

static void format_pedestrian_signal(char *buf, size_t len, int is_walk,
                                     int requested, int seconds) {
    char request_text[16];

    snprintf(request_text, sizeof(request_text), "%s",
             requested ? ", requested" : "");

    if (is_walk) {
        snprintf(buf, len, "WALK %d sec%s",
                 seconds > 0 ? seconds : 0, request_text);
    } else if (seconds < 0) {
        snprintf(buf, len, "STOP WAIT%s", request_text);
    } else {
        snprintf(buf, len, "STOP %d sec%s",
                 seconds, request_text);
    }
}

void display_ui(void) {
#if ENABLE_TRAFFIC_SIMULATION
    char sim_time[16];
#endif
    char ns_vehicle[32];
    char ew_vehicle[32];
    char ns_pedestrian[48];
    char ew_pedestrian[48];

    pthread_mutex_lock(&state.mutex);
#if ENABLE_TRAFFIC_SIMULATION
    format_sim_time(sim_time, sizeof(sim_time));
#endif
    format_vehicle_signal(ns_vehicle, sizeof(ns_vehicle), state.ns_light,
                          vehicle_signal_seconds_locked(DIR_NS));
    format_vehicle_signal(ew_vehicle, sizeof(ew_vehicle), state.ew_light,
                          vehicle_signal_seconds_locked(DIR_EW));
    format_pedestrian_signal(ns_pedestrian, sizeof(ns_pedestrian),
                             state.ped_ns_walk, state.ped_ns_request,
                             pedestrian_signal_seconds_locked(DIR_NS));
    format_pedestrian_signal(ew_pedestrian, sizeof(ew_pedestrian),
                             state.ped_ew_walk, state.ped_ew_request,
                             pedestrian_signal_seconds_locked(DIR_EW));

    clear_screen();

    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("%s                 LOCAL CONTROLLER I%u%s\n",
           COLOR_BOLD, (unsigned)state.intersection_id + 1, COLOR_RESET);
    printf("%s========================================================%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Service [%s] mode [%s]\n",
           state.service_name, connection_mode_str(state.mode));

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
    printf("Phase [%s] remaining [%d sec]\n",
           phase_text(state.phase), state.time_remaining);
    printf("Timing profile: initial [%s] cycle [%d sec]\n",
           phase_text(state.initial_phase), local_fixed_cycle_seconds_locked());
    printf("  NS green [%d sec] EW green [%d sec]\n",
           state.ns_green_sec, state.ew_green_sec);
    printf("Telemetry seq [%u] last command [%u] health [%s]\n",
           (unsigned)state.status_sequence,
           (unsigned)state.last_applied_command_id,
           (!state.fault_active && state.traffic_mode != MODE_FAILSAFE) ?
           "HEALTHY" : "DEGRADED");
#if ENABLE_TRAFFIC_SIMULATION
    printf("Sim time [%s] period [%s]\n",
           sim_time, period_text(state.sim_seconds));
#endif
    printf("Vehicle lights:\n");
    printf("  NS [%s]\n", ns_vehicle);
    printf("  EW [%s]\n", ew_vehicle);
    printf("Pedestrian:\n");
    printf("  NS [%s]\n", ns_pedestrian);
    printf("  EW [%s]\n", ew_pedestrian);
#if ENABLE_TRAFFIC_SIMULATION || ENABLE_DEMO_COMMANDS
    printf("Sensors:\n");
    printf("  NS cars [%d]%s\n",
           state.sensor_ns_count,
           state.sensor_ns_count >= SENSOR_CAR_THRESHOLD ?
           " SENSOR-DETECTED" : "");
    printf("  EW cars [%d]%s\n",
           state.sensor_ew_count,
           state.sensor_ew_count >= SENSOR_CAR_THRESHOLD ?
           " SENSOR-DETECTED" : "");

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
