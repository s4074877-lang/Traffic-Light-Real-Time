#include "local_controller.h"

#include <string.h>

int execute_command(const char *cmd) {
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
