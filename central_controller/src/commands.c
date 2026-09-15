#include "commands.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int parse_target(const char *text, unsigned *target) {
    if (strcmp(text, "all") == 0) {
        *target = INTERSECTION_ALL;
        return 1;
    }
    if (text[0] == 'I' && text[1] >= '1' && text[1] <= '0' + NUM_INTERSECTIONS &&
        text[2] == '\0') {
        *target = (unsigned)(text[1] - '1');
        return 1;
    }
    return 0;
}

static int parse_number(const char *text, unsigned minimum, unsigned maximum,
                        unsigned *number) {
    unsigned value = 0;
    if (*text == '\0') {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        unsigned digit = (unsigned)(*p - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10)) {
            return 0;
        }
        value = value * 10 + digit;
    }
    if (value < minimum) {
        return 0;
    }
    *number = value;
    return 1;
}

static int parse_mode(const char *text, uint8_t *mode) {
    if (strcmp(text, "fixed") == 0) {
        *mode = MODE_FIXED;
        return 1;
    }
    if (strcmp(text, "sensor") == 0) {
        *mode = MODE_SENSOR;
        return 1;
    }
    return 0;
}

static int parse_phase(const char *text, uint8_t *phase) {
    if (strcmp(text, "NS") == 0) {
        *phase = PHASE_NS_GREEN;
        return 1;
    }
    if (strcmp(text, "EW") == 0) {
        *phase = PHASE_EW_GREEN;
        return 1;
    }
    return 0;
}

static void initialize_message(test_message_t *message, msg_type_t type) {
    memset(message, 0, sizeof(*message));
    message->header.type = (uint16_t)type;
    message->header.src = CONTROLLER_CENTRAL;
    message->header.dst = CONTROLLER_LOCAL;
}

static void simulation_encode(test_message_t *message, const central_sim_command_t *command) {
    memset(message->data, 0, sizeof(message->data));
    if (command->action == CENTRAL_SIM_TIME) {
        snprintf(message->data, sizeof(message->data), "SIM1 %u %u TIME %u",
                 command->target, (unsigned)command->command_id, command->minute);
    } else {
        snprintf(message->data, sizeof(message->data), "SIM1 %u %u %s",
                 command->target, (unsigned)command->command_id,
                 command->action == CENTRAL_SIM_START ? "START" : "STOP");
    }
}

int central_simulation_decode(const test_message_t *message, central_sim_command_t *output) {
    if (message == NULL || output == NULL || message->header.type != MSG_TEST ||
        message->header.src != CONTROLLER_CENTRAL || message->header.dst != CONTROLLER_LOCAL ||
        message->header.reserved != 0 ||
        memchr(message->header.timestamp, '\0', sizeof(message->header.timestamp)) == NULL) {
        return 0;
    }
    const char *end = memchr(message->data, '\0', sizeof(message->data));
    if (end == NULL) {
        return 0;
    }
    for (const char *p = end + 1; p < message->data + sizeof(message->data); ++p) {
        if (*p != '\0') {
            return 0;
        }
    }
    char buffer[sizeof(message->data)];
    memcpy(buffer, message->data, sizeof(buffer));
    char *tokens[5] = {buffer};
    size_t count = 1;
    for (char *p = buffer; *p; ++p) {
        if (*p == ' ') {
            if (count == sizeof(tokens) / sizeof(tokens[0])) {
                return 0;
            }
            *p = '\0';
            tokens[count++] = p + 1;
        }
    }
    central_sim_command_t command = {0};
    unsigned id;
    if (count < 4 || strcmp(tokens[0], "SIM1") != 0 ||
        !parse_number(tokens[1], 0, INTERSECTION_ALL, &command.target) ||
        (command.target >= NUM_INTERSECTIONS && command.target != INTERSECTION_ALL) ||
        !parse_number(tokens[2], 0, UINT16_MAX, &id)) {
        return 0;
    }
    command.command_id = (uint16_t)id;
    if (count == 4 && strcmp(tokens[3], "START") == 0) {
        command.action = CENTRAL_SIM_START;
    } else if (count == 4 && strcmp(tokens[3], "STOP") == 0) {
        command.action = CENTRAL_SIM_STOP;
    } else if (count == 5 && strcmp(tokens[3], "TIME") == 0 &&
               parse_number(tokens[4], 0, 1439, &command.minute)) {
        command.action = CENTRAL_SIM_TIME;
    } else {
        return 0;
    }
    test_message_t canonical = {0};
    simulation_encode(&canonical, &command);
    if (memcmp(canonical.data, message->data, sizeof(canonical.data)) != 0) {
        return 0;
    }
    *output = command;
    return 1;
}

static int parse_simulation_time(const char *text, unsigned *minute) {
    if (strlen(text) != 5 || text[2] != ':' ||
        text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' ||
        text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9') {
        return 0;
    }
    unsigned hours = (unsigned)(text[0] - '0') * 10 + (unsigned)(text[1] - '0');
    unsigned minutes = (unsigned)(text[3] - '0') * 10 + (unsigned)(text[4] - '0');
    if (hours > 23 || minutes > 59) {
        return 0;
    }
    *minute = hours * 60 + minutes;
    return 1;
}

int central_parse_command(const char *line, test_message_t *message, unsigned *target) {
    char buffer[256];
    char *tokens[4];
    size_t count = 0;
    if (line == NULL || message == NULL || target == NULL) {
        return 0;
    }
    size_t length = strlen(line);
    if (length >= sizeof(buffer)) {
        return 0;
    }
    memcpy(buffer, line, length + 1);
    char *cursor = buffer;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count == sizeof(tokens) / sizeof(tokens[0])) {
            return 0;
        }
        tokens[count++] = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor) {
            *cursor++ = '\0';
        }
    }
    if (count == 0) {
        return 0;
    }

    test_message_t command;
    mode_cmd_msg_t mode = {0};
    coordination_command_msg_t coordination = {0};
    unsigned selected_target = INTERSECTION_ALL;
    unsigned seconds;
    mode.priority = CMD_PRIO_OPERATOR;

    if (strcmp(tokens[0], "mode-fixed") == 0 || strcmp(tokens[0], "mode-sensor") == 0) {
        if (count > 2 || (count == 2 && !parse_target(tokens[1], &selected_target))) {
            return 0;
        }
        initialize_message(&command, MSG_MODE_COMMAND);
        mode.new_mode = strcmp(tokens[0], "mode-fixed") == 0 ? MODE_FIXED : MODE_SENSOR;
        mode.action = CMD_SET_MODE;
        memcpy(command.data, &mode, sizeof(mode));
    } else if (strcmp(tokens[0], "mode-temp") == 0) {
        if (count != 4 || !parse_target(tokens[1], &selected_target) ||
            !parse_number(tokens[3], 1, UINT16_MAX, &seconds) ||
            !parse_mode(tokens[2], &mode.new_mode)) {
            return 0;
        }
        initialize_message(&command, MSG_MODE_COMMAND);
        mode.action = CMD_TEMPORARY;
        mode.duration_sec = (uint16_t)seconds;
        memcpy(command.data, &mode, sizeof(mode));
    } else if (strcmp(tokens[0], "mode-revert") == 0) {
        if (count != 2 || !parse_target(tokens[1], &selected_target)) {
            return 0;
        }
        initialize_message(&command, MSG_MODE_COMMAND);
        mode.action = CMD_REVERT;
        memcpy(command.data, &mode, sizeof(mode));
    } else if (strcmp(tokens[0], "coordinate") == 0) {
        unsigned cycle_seconds = COORDINATION_MAX_CYCLE_SEC;
        if (count != 4 || !parse_target(tokens[1], &selected_target) ||
            !parse_number(tokens[3], 0, cycle_seconds - 1, &seconds) ||
            !parse_phase(tokens[2], &coordination.phase)) {
            return 0;
        }
        initialize_message(&command, MSG_COORDINATION_COMMAND);
        coordination.mode = MODE_FIXED;
        coordination.cycle_offset_sec = (uint16_t)seconds;
        memcpy(command.data, &coordination, sizeof(coordination));
    } else if (strcmp(tokens[0], "sim-start") == 0 || strcmp(tokens[0], "sim-stop") == 0 ||
               strcmp(tokens[0], "sim-time") == 0) {
        int set_time = strcmp(tokens[0], "sim-time") == 0;
        central_sim_command_t simulation = {0};
        if (count != (set_time ? 3U : 2U) || !parse_target(tokens[1], &selected_target) ||
            (set_time && !parse_simulation_time(tokens[2], &simulation.minute))) {
            return 0;
        }
        simulation.target = selected_target;
        simulation.action = set_time ? CENTRAL_SIM_TIME :
            strcmp(tokens[0], "sim-start") == 0 ? CENTRAL_SIM_START : CENTRAL_SIM_STOP;
        initialize_message(&command, MSG_TEST);
        simulation_encode(&command, &simulation);
    } else {
        return 0;
    }

    *message = command;
    *target = selected_target;
    return 1;
}

uint16_t central_command_id(const test_message_t *message) {
    if (message == NULL) {
        return 0;
    }
    if (message->header.type == MSG_MODE_COMMAND) {
        mode_cmd_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        return command.command_id;
    }
    if (message->header.type == MSG_COORDINATION_COMMAND) {
        coordination_command_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        return command.command_id;
    }
    central_sim_command_t simulation;
    if (central_simulation_decode(message, &simulation)) {
        return simulation.command_id;
    }
    return 0;
}

unsigned central_command_target(const test_message_t *message) {
    if (message == NULL) {
        return NUM_INTERSECTIONS;
    }
    if (message->header.type == MSG_MODE_COMMAND) {
        mode_cmd_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        return command.intersection_id;
    }
    if (message->header.type == MSG_COORDINATION_COMMAND) {
        coordination_command_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        return command.intersection_id;
    }
    central_sim_command_t simulation;
    if (central_simulation_decode(message, &simulation)) {
        return simulation.target;
    }
    return NUM_INTERSECTIONS;
}

void central_command_set_id(test_message_t *message, uint16_t id) {
    if (message == NULL) {
        return;
    }
    if (message->header.type == MSG_MODE_COMMAND) {
        mode_cmd_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        command.command_id = id;
        memcpy(message->data, &command, sizeof(command));
    } else if (message->header.type == MSG_COORDINATION_COMMAND) {
        coordination_command_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        command.command_id = id;
        memcpy(message->data, &command, sizeof(command));
    } else {
        central_sim_command_t simulation;
        if (central_simulation_decode(message, &simulation)) {
            simulation.command_id = id;
            simulation_encode(message, &simulation);
        }
    }
}

void central_command_set_target(test_message_t *message, unsigned target) {
    if (message == NULL || (target >= NUM_INTERSECTIONS && target != INTERSECTION_ALL)) {
        return;
    }
    if (message->header.type == MSG_MODE_COMMAND) {
        mode_cmd_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        command.intersection_id = (uint8_t)target;
        memcpy(message->data, &command, sizeof(command));
    } else if (message->header.type == MSG_COORDINATION_COMMAND) {
        coordination_command_msg_t command;
        memcpy(&command, message->data, sizeof(command));
        command.intersection_id = (uint8_t)target;
        memcpy(message->data, &command, sizeof(command));
    } else {
        central_sim_command_t simulation;
        if (central_simulation_decode(message, &simulation)) {
            simulation.target = target;
            simulation_encode(message, &simulation);
        }
    }
}
