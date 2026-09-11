#include "commands.h"
#include <ctype.h>
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

int central_parse_command(const char *line, any_msg_t *prototype, unsigned *target) {
    char buffer[256];
    char *tokens[4];
    size_t count = 0;
    if (line == NULL || prototype == NULL || target == NULL) {
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

    any_msg_t command;
    unsigned selected_target = INTERSECTION_ALL;
    unsigned seconds;
    if (strcmp(tokens[0], "mode-fixed") == 0 || strcmp(tokens[0], "mode-sensor") == 0) {
        if (count > 2 || (count == 2 && !parse_target(tokens[1], &selected_target))) {
            return 0;
        }
        protocol_init_message(&command, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        command.payload.mode_cmd.new_mode = strcmp(tokens[0], "mode-fixed") == 0
                                         ? MODE_FIXED : MODE_SENSOR;
        command.payload.mode_cmd.action = CMD_SET_MODE;
        command.payload.mode_cmd.priority = CMD_PRIO_OPERATOR;
    } else if (strcmp(tokens[0], "mode-temp") == 0) {
        if (count != 4 || !parse_target(tokens[1], &selected_target) ||
            !parse_number(tokens[3], 1, UINT16_MAX, &seconds)) {
            return 0;
        }
        protocol_init_message(&command, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        if (!parse_mode(tokens[2], &command.payload.mode_cmd.new_mode)) {
            return 0;
        }
        command.payload.mode_cmd.action = CMD_TEMPORARY;
        command.payload.mode_cmd.priority = CMD_PRIO_OPERATOR;
        command.payload.mode_cmd.duration_sec = (uint16_t)seconds;
    } else if (strcmp(tokens[0], "mode-revert") == 0) {
        if (count != 2 || !parse_target(tokens[1], &selected_target)) {
            return 0;
        }
        protocol_init_message(&command, MSG_MODE_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        command.payload.mode_cmd.action = CMD_REVERT;
        command.payload.mode_cmd.priority = CMD_PRIO_OPERATOR;
    } else if (strcmp(tokens[0], "coordinate") == 0) {
        if (count != 4 || !parse_target(tokens[1], &selected_target) ||
            !parse_number(tokens[3], 0, NORMAL_CYCLE_SEC - 1, &seconds)) {
            return 0;
        }
        protocol_init_message(&command, MSG_COORDINATION_COMMAND, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        if (!parse_phase(tokens[2], &command.payload.coordination.phase)) {
            return 0;
        }
        command.payload.coordination.mode = MODE_FIXED;
        command.payload.coordination.cycle_offset_sec = (uint16_t)seconds;
    } else if (strcmp(tokens[0], "override") == 0) {
        if (count != 4 || !parse_target(tokens[1], &selected_target) ||
            !parse_number(tokens[3], 1, UINT16_MAX, &seconds)) {
            return 0;
        }
        protocol_init_message(&command, MSG_OVERRIDE_REQUEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        if (!parse_phase(tokens[2], &command.payload.override_request.phase)) {
            return 0;
        }
        command.payload.override_request.action = CMD_TEMPORARY;
        command.payload.override_request.duration_sec = (uint16_t)seconds;
    } else if (strcmp(tokens[0], "release") == 0) {
        if (count != 2 || !parse_target(tokens[1], &selected_target)) {
            return 0;
        }
        protocol_init_message(&command, MSG_OVERRIDE_REQUEST, CONTROLLER_CENTRAL, CONTROLLER_LOCAL);
        command.payload.override_request.action = CMD_REVERT;
    } else {
        return 0;
    }

    *prototype = command;
    *target = selected_target;
    return 1;
}
