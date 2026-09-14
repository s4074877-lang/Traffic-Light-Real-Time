#include "local_controller.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

static void status_deadline(struct timespec *deadline) {
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += STATUS_PERIOD_SEC;
}

void* connection_thread(void *arg) {
    (void)arg;

    while (1) {
        if (connection_try_connect(&state.central_conn)) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_central_update,
                          sizeof(state.last_central_update));
            mark_status_dirty_locked();
            pthread_mutex_unlock(&state.mutex);
        }

        if (connection_try_connect(&state.train_conn)) {
            pthread_mutex_lock(&state.mutex);
            get_timestamp(state.last_train_update,
                          sizeof(state.last_train_update));
            state.ui_needs_update = 1;
            pthread_mutex_unlock(&state.mutex);

            pthread_mutex_lock(&state.train_send_mutex);
            send_test_message(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
            pthread_mutex_unlock(&state.train_send_mutex);
        }

        sleep(2);
    }

    return NULL;
}

void* ui_refresh_thread(void *arg) {
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

void* traffic_thread(void *arg) {
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

void* status_thread(void *arg) {
    (void)arg;

    while (1) {
        test_message_t status_msg;
        test_message_t fault_msg;
        reply_t reply;
        int send_status = 0;
        int send_fault = 0;
        uint16_t status_sequence = 0;
        uint16_t fault_sequence = 0;

        pthread_mutex_lock(&state.mutex);
        struct timespec deadline;
        status_deadline(&deadline);
        while (!state.status_dirty && !state.fault_pending) {
            int wait_result =
                pthread_cond_timedwait(&state.status_cond, &state.mutex,
                                       &deadline);
            if (wait_result == ETIMEDOUT || wait_result != 0) {
                break;
            }
        }

        if (state.central_conn.connected) {
            status_sequence = prepare_status_message_locked(&status_msg);
            send_status = 1;

            if (state.fault_pending) {
                fault_sequence = prepare_fault_message_locked(&fault_msg);
                send_fault = 1;
            }
        }
        pthread_mutex_unlock(&state.mutex);

        if (!send_status && !send_fault) {
            sleep(STATUS_PERIOD_SEC);
            continue;
        }

        if (send_fault || send_status) {
            pthread_mutex_lock(&state.central_send_mutex);

            if (send_fault && send_message(&state.central_conn, &fault_msg, &reply) == 0 &&
                reply.status == 0) {
                pthread_mutex_lock(&state.mutex);
                if (state.fault_sequence == fault_sequence) {
                    state.fault_pending = 0;
                }
                get_timestamp(state.last_send_central,
                              sizeof(state.last_send_central));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }

            if (send_status && send_message(&state.central_conn, &status_msg, &reply) == 0 &&
                reply.status == 0) {
                pthread_mutex_lock(&state.mutex);
                if (state.status_sequence == status_sequence) {
                    state.status_dirty = 0;
                }
                get_timestamp(state.last_send_central,
                              sizeof(state.last_send_central));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }

            pthread_mutex_unlock(&state.central_send_mutex);
        }
    }

    return NULL;
}

void* heartbeat_thread(void *arg) {
    (void)arg;

    while (1) {
        sleep(HEARTBEAT_INTERVAL);

        if (connection_is_connected(&state.central_conn)) {
            int heartbeat_failed;
            test_message_t heartbeat_msg;
            reply_t reply;
            pthread_mutex_lock(&state.mutex);
            prepare_heartbeat_message_locked(&heartbeat_msg);
            pthread_mutex_unlock(&state.mutex);

            pthread_mutex_lock(&state.central_send_mutex);
            heartbeat_failed =
                send_message(&state.central_conn, &heartbeat_msg, &reply);
            pthread_mutex_unlock(&state.central_send_mutex);

            if (heartbeat_failed != 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_central_update,
                              sizeof(state.last_central_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }
        }

        if (connection_is_connected(&state.train_conn)) {
            int heartbeat_failed;
            pthread_mutex_lock(&state.train_send_mutex);
            heartbeat_failed =
                send_heartbeat(&state.train_conn, CONTROLLER_LOCAL, CONTROLLER_TRAIN);
            pthread_mutex_unlock(&state.train_send_mutex);

            if (heartbeat_failed != 0) {
                pthread_mutex_lock(&state.mutex);
                get_timestamp(state.last_train_update,
                              sizeof(state.last_train_update));
                state.ui_needs_update = 1;
                pthread_mutex_unlock(&state.mutex);
            }
        }
    }

    return NULL;
}
