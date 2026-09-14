#include "local_controller.h"

#include <ctype.h>
#include <string.h>

static int parse_sim_number(const char *text, unsigned maximum, unsigned *value) {
    unsigned parsed = 0;

    if (!text || !*text || (text[0] == '0' && text[1] != '\0')) return 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
        unsigned digit;
        if (!isdigit(*cursor)) return 0;
        digit = (unsigned)(*cursor - '0');
        if (parsed > maximum / 10U ||
            (parsed == maximum / 10U && digit > maximum % 10U)) return 0;
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return 1;
}

static int parse_sim_command(const char *text, unsigned *target,
                             unsigned *command_id, unsigned *minute,
                             int *action) {
    char copy[sizeof(((test_message_t *)0)->data)];
    char *tokens[5] = {0};
    const char *terminator;
    size_t length;
    size_t count = 0;

    if (!text) return 0;
    terminator = memchr(text, '\0', sizeof(copy));
    if (!terminator) return 0;
    length = (size_t)(terminator - text);
    if (length >= sizeof(copy)) return 0;
    memcpy(copy, text, length + 1);
    char *cursor = copy;
    while (*cursor) {
        if (count == sizeof(tokens) / sizeof(tokens[0])) return 0;
        tokens[count++] = cursor;
        while (*cursor && *cursor != ' ') ++cursor;
        if (*cursor) {
            *cursor++ = '\0';
            if (!*cursor || *cursor == ' ') return 0;
        }
    }
    if (count < 4 || strcmp(tokens[0], "SIM1") != 0 ||
        !parse_sim_number(tokens[1], NUM_INTERSECTIONS - 1, target) ||
        !parse_sim_number(tokens[2], UINT16_MAX, command_id) ||
        *command_id == 0) return 0;
    if (count == 4 && strcmp(tokens[3], "START") == 0) {
        *action = 1;
        return 1;
    }
    if (count == 4 && strcmp(tokens[3], "STOP") == 0) {
        *action = 2;
        return 1;
    }
    if (count == 5 && strcmp(tokens[3], "TIME") == 0 &&
        parse_sim_number(tokens[4], 1439, minute)) {
        *action = 3;
        return 1;
    }
    return 0;
}

#if ENABLE_TRAFFIC_SIMULATION
static int handle_simulation_message(int rcvid, test_message_t *msg,
                                     reply_t *reply, void *ctx) {
    local_state_t *s = (local_state_t *)ctx;
    unsigned target = 0, command_id = 0, minute = 0;
    int action = 0;
    int accepted = 0;

    (void)rcvid;
    if (!parse_sim_command(msg->data, &target, &command_id, &minute, &action)) {
        reply->status = -1;
        get_timestamp(reply->timestamp, sizeof(reply->timestamp));
        return 0;
    }
    reply->command_id = (uint16_t)command_id;
    pthread_mutex_lock(&s->mutex);
    if (msg->header.src == CONTROLLER_CENTRAL &&
        msg->header.dst == CONTROLLER_LOCAL &&
        target_matches_local((uint8_t)target)) {
        strncpy(s->last_recv_central, msg->header.timestamp,
                sizeof(s->last_recv_central) - 1);
        get_timestamp(s->last_central_update, sizeof(s->last_central_update));
        if (action == 1) {
            s->sim_running = 1;
            s->next_ns_car_in_seconds = random_car_gap_seconds();
            s->next_ew_car_in_seconds = random_car_gap_seconds();
            s->next_train_in_seconds = random_train_gap_seconds();
            s->next_train_direction = random_train_direction();
            accepted = 1;
        } else if (action == 2) {
            s->sim_running = 0;
            accepted = 1;
        } else {
            set_sim_minute_locked(minute);
            accepted = 1;
        }
        if (accepted) mark_status_dirty_locked();
        s->ui_needs_update = 1;
    }
    pthread_mutex_unlock(&s->mutex);
    reply->status = accepted ? 0 : -1;
    get_timestamp(reply->timestamp, sizeof(reply->timestamp));
    return 0;
}
#endif

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
#if ENABLE_TRAFFIC_SIMULATION
    { MSG_TEST, CONTROLLER_CENTRAL, handle_simulation_message },
#endif
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
