#include "local_process.h"

/* The same validation is used by the input process and the core. */
int valid_input(const char *command) {
    if (command == NULL || command[0] == '\0' || command[1] != '\0') {
        return 0;
    }
#if ENABLE_DEMO_COMMANDS
    if (strchr("mnexz12cr", command[0]) != NULL) {
        return 1;
    }
#if ENABLE_TRAFFIC_SIMULATION
    if (strchr("pol", command[0]) != NULL) {
        return 1;
    }
#endif
#endif
    return 0;
}

/* No console or network operations here: only update core state. */
int execute_command(const char *command) {
    if (!valid_input(command)) {
        return -1;
    }

    pthread_mutex_lock(&state.mutex);
#if ENABLE_DEMO_COMMANDS
    switch (command[0]) {
        case 'm':
            state.manual_mode_override = 1;
            state.traffic_mode = state.traffic_mode == MODE_FIXED ? MODE_SENSOR : MODE_FIXED;
            break;
        case 'n': add_pedestrian_request_locked(DIR_NS); break;
        case 'e': add_pedestrian_request_locked(DIR_EW); break;
        case 'x': toggle_sensor_locked(DIR_NS); break;
        case 'z': toggle_sensor_locked(DIR_EW); break;
        case '1': start_train_locked(1); break;
        case '2': start_train_locked(2); break;
        case 'c': clear_train_locked(); break;
        case 'r': reset_demo_inputs_locked(); break;
#if ENABLE_TRAFFIC_SIMULATION
        case 'p': set_sim_hour_locked(7); break;
        case 'o': set_sim_hour_locked(12); break;
        case 'l': set_sim_hour_locked(22); break;
#endif
    }
#endif
    mark_status_dirty_locked();
    pthread_mutex_unlock(&state.mutex);
    return 0;
}
