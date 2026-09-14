#include "local_controller.h"

#include <string.h>

static int handle_test_message(int rcvid, test_message_t *msg,
                               reply_t *reply, void *ctx) {
    local_state_t *s = (local_state_t *)ctx;

    (void)rcvid;

    pthread_mutex_lock(&s->mutex);

    if (msg->header.src == CONTROLLER_CENTRAL) {
        strncpy(s->last_recv_central, msg->header.timestamp,
                sizeof(s->last_recv_central) - 1);
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
    } else if (msg->header.src == CONTROLLER_TRAIN) {
        strncpy(s->last_recv_train, msg->header.timestamp,
                sizeof(s->last_recv_train) - 1);
        get_timestamp(s->last_train_update, sizeof(s->last_train_update));
    }

    s->ui_needs_update = 1;
    pthread_mutex_unlock(&s->mutex);

    reply->status = 0;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}

static int handle_mode_command(int rcvid, test_message_t *msg,
                               reply_t *reply, void *ctx) {
    local_state_t *s = (local_state_t *)ctx;
    mode_cmd_msg_t command;
    int accepted = 0;

    (void)rcvid;

    memcpy(&command, msg->data, sizeof(command));
    reply->command_id = command.command_id;

    pthread_mutex_lock(&s->mutex);
    strncpy(s->last_recv_central, msg->header.timestamp,
            sizeof(s->last_recv_central) - 1);
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
            clear_fault_locked();
            accepted = 1;
        } else if (command.action == CMD_TEMPORARY && command.duration_sec > 0) {
            if (s->temp_mode_remaining <= 0) {
                s->temp_return_mode = s->traffic_mode;
                s->temp_return_manual_override = s->manual_mode_override;
            }
            s->traffic_mode = command.new_mode;
            s->manual_mode_override = 1;
            s->temp_mode_remaining = command.duration_sec;
            clear_fault_locked();
            accepted = 1;
        } else if (command.action == CMD_REVERT && command.duration_sec == 0) {
            if (s->temp_mode_remaining > 0) {
                s->traffic_mode = s->temp_return_mode;
                s->manual_mode_override = s->temp_return_manual_override;
                s->temp_mode_remaining = 0;
            }
            if (s->traffic_mode == MODE_FAILSAFE) {
                s->traffic_mode = MODE_FIXED;
                clear_fault_locked();
            }
            accepted = 1;
        }

        if (accepted) {
            s->last_applied_command_id = command.command_id;
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
    local_state_t *s = (local_state_t *)ctx;
    coordination_command_msg_t command;
    int accepted = 0;

    (void)rcvid;

    memcpy(&command, msg->data, sizeof(command));
    reply->command_id = command.command_id;

    pthread_mutex_lock(&s->mutex);
    strncpy(s->last_recv_central, msg->header.timestamp,
            sizeof(s->last_recv_central) - 1);
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
        s->coordination_offset_sec = command.cycle_offset_sec;
        s->coordination_pending =
            !(s->phase == command.phase && s->time_remaining > 0);
        clear_fault_locked();
        accepted = 1;
        s->last_applied_command_id = command.command_id;
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
    local_state_t *s = (local_state_t *)ctx;
    sensor_msg_t sensor;
    int accepted = 0;

    (void)rcvid;

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
    local_state_t *s = (local_state_t *)ctx;
    ped_msg_t ped;
    int accepted = 0;

    (void)rcvid;

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
    local_state_t *s = (local_state_t *)ctx;
    railway_msg_t railway;
    int accepted = 0;

    (void)rcvid;

    memcpy(&railway, msg->data, sizeof(railway));

    pthread_mutex_lock(&s->mutex);
    if (msg->header.src == CONTROLLER_TRAIN &&
        railway_matches_local(railway.intersection_id)) {
        strncpy(s->last_recv_train, msg->header.timestamp,
                sizeof(s->last_recv_train) - 1);
        get_timestamp(s->last_train_update, sizeof(s->last_train_update));

        if (msg->header.type == MSG_RAILWAY_PREEMPT && railway.active) {
            start_train_message_locked(railway_display_line(railway.intersection_id),
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

static message_handler_entry_t handlers[] = {
    { MSG_TEST, 0, handle_test_message },
    { MSG_MODE_COMMAND, CONTROLLER_CENTRAL, handle_mode_command },
    { MSG_COORDINATION_COMMAND, CONTROLLER_CENTRAL, handle_coordination_command },
    { MSG_SENSOR_UPDATE, 0, handle_sensor_update },
    { MSG_PED_REQUEST, 0, handle_ped_request },
    { MSG_RAILWAY_PREEMPT, CONTROLLER_TRAIN, handle_railway_message },
    { MSG_TRAIN_CLEAR, CONTROLLER_TRAIN, handle_railway_message }
};

void local_receive_init(receive_context_t *recv_ctx, name_attach_t *attach) {
    receive_init(recv_ctx, attach, handlers,
                 sizeof(handlers) / sizeof(handlers[0]), &state);
}

void* message_handler_thread(void *arg) {
    receive_context_t *ctx = (receive_context_t *)arg;
    receive_loop(ctx);
    return NULL;
}
