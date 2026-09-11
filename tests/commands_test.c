#include "../central_controller/src/commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void require(int condition, const char *description) {
    checks++;
    if (!condition) {
        fprintf(stderr, "Command parser check failed: %s\n", description);
        exit(EXIT_FAILURE);
    }
}

static any_msg_t expect_command(const char *line, unsigned expected_target, msg_type_t type) {
    any_msg_t message;
    unsigned target = NUM_INTERSECTIONS;
    require(central_parse_command(line, &message, &target) == 1, line);
    require(target == expected_target, line);
    require(message.header.type == type, line);
    require(protocol_command_id(&message) == 0, "Command ID belongs to the sender");

    switch (type) {
        case MSG_MODE_COMMAND:
            message.payload.mode_cmd.command_id = 42;
            message.payload.mode_cmd.intersection_id = (uint8_t)target;
            break;
        case MSG_COORDINATION_COMMAND:
            message.payload.coordination.command_id = 42;
            message.payload.coordination.intersection_id = (uint8_t)target;
            break;
        case MSG_OVERRIDE_REQUEST:
            message.payload.override_request.command_id = 42;
            message.payload.override_request.intersection_id = (uint8_t)target;
            break;
        default:
            require(0, "Unexpected command type");
    }
    require(protocol_validate_message(&message, protocol_message_size(&message), CONTROLLER_LOCAL),
            "A parsed command must be valid for the Local receiver");
    return message;
}

static void expect_rejected(const char *line) {
    any_msg_t message;
    memset(&message, 0xa5, sizeof(message));
    unsigned char before[sizeof(message)];
    memcpy(before, &message, sizeof(message));
    unsigned target = 77;
    require(central_parse_command(line, &message, &target) == 0, line);
    require(memcmp(&message, before, sizeof(message)) == 0 && target == 77,
            "Invalid input must leave outputs unchanged");
}

int main(void) {
    any_msg_t message = expect_command("mode-fixed", INTERSECTION_ALL, MSG_MODE_COMMAND);
    require(message.payload.mode_cmd.new_mode == MODE_FIXED &&
            message.payload.mode_cmd.action == CMD_SET_MODE &&
            message.payload.mode_cmd.priority == CMD_PRIO_OPERATOR &&
            message.payload.mode_cmd.duration_sec == 0, "Persistent fixed mode");

    message = expect_command("mode-sensor I6", I6, MSG_MODE_COMMAND);
    require(message.payload.mode_cmd.new_mode == MODE_SENSOR, "Sensor mode at I6");
    expect_command("mode-fixed I1", I1, MSG_MODE_COMMAND);
    expect_command(" \tmode-sensor\tall \r\n", INTERSECTION_ALL, MSG_MODE_COMMAND);

    message = expect_command("mode-temp I1 sensor 65535", I1, MSG_MODE_COMMAND);
    require(message.payload.mode_cmd.new_mode == MODE_SENSOR &&
            message.payload.mode_cmd.action == CMD_TEMPORARY &&
            message.payload.mode_cmd.duration_sec == UINT16_MAX, "Maximum temporary duration");
    message = expect_command("mode-temp all fixed 1", INTERSECTION_ALL, MSG_MODE_COMMAND);
    require(message.payload.mode_cmd.new_mode == MODE_FIXED &&
            message.payload.mode_cmd.duration_sec == 1, "Minimum temporary duration");
    message = expect_command("mode-revert I6", I6, MSG_MODE_COMMAND);
    require(message.payload.mode_cmd.action == CMD_REVERT &&
            message.payload.mode_cmd.duration_sec == 0, "Cancel temporary mode");

    message = expect_command("coordinate I3 NS 0", I3, MSG_COORDINATION_COMMAND);
    require(message.payload.coordination.phase == PHASE_NS_GREEN &&
            message.payload.coordination.mode == MODE_FIXED &&
            message.payload.coordination.cycle_offset_sec == 0, "Start of coordination cycle");
    message = expect_command("coordinate all EW 43", INTERSECTION_ALL, MSG_COORDINATION_COMMAND);
    require(message.payload.coordination.phase == PHASE_EW_GREEN &&
            message.payload.coordination.cycle_offset_sec == NORMAL_CYCLE_SEC - 1,
            "Last valid coordination offset");

    message = expect_command("override I4 EW 1", I4, MSG_OVERRIDE_REQUEST);
    require(message.payload.override_request.phase == PHASE_EW_GREEN &&
            message.payload.override_request.action == CMD_TEMPORARY &&
            message.payload.override_request.duration_sec == 1, "Temporary EW override");
    message = expect_command("override all NS 65535", INTERSECTION_ALL, MSG_OVERRIDE_REQUEST);
    require(message.payload.override_request.phase == PHASE_NS_GREEN &&
            message.payload.override_request.duration_sec == UINT16_MAX, "Maximum override duration");
    message = expect_command("release all", INTERSECTION_ALL, MSG_OVERRIDE_REQUEST);
    require(message.payload.override_request.action == CMD_REVERT &&
            message.payload.override_request.duration_sec == 0, "Release overrides");
    expect_command("release I6", I6, MSG_OVERRIDE_REQUEST);

    const char *invalid[] = {
        "", " \t\r\n", "status", "help", "quit", "unknown",
        "mode-fixed I0", "mode-fixed I7", "mode-fixed I10", "mode-fixed I",
        "mode-fixed i1", "mode-fixed ALL", "mode-fixed all trailing",
        "mode-sensor I1 extra more tokens", "mode-temp", "mode-temp I1 sensor",
        "mode-temp I1 railway 5", "mode-temp I1 SENSOR 5", "mode-temp I1 sensor 0",
        "mode-temp I1 sensor 65536", "mode-temp I1 sensor -1", "mode-temp I1 sensor +1",
        "mode-temp I1 sensor 1.0", "mode-temp I1 sensor 1e2", "mode-temp I1 sensor 1s",
        "mode-temp I1 sensor 99999999999999999999999999999999999",
        "mode-revert", "mode-revert I1 extra", "coordinate I1 NS",
        "coordinate I1 NS 44", "coordinate I1 NS -1", "coordinate I1 NS +1",
        "coordinate I1 NS 0.5", "coordinate I1 ns 1", "coordinate I1 RED 1",
        "coordinate I9 EW 0", "override", "override I1 EW", "override I1 NS 0",
        "override I1 NS 65536", "override I1 NS -2", "override I1 STOP 10",
        "override I1 NS 10 extra", "release", "release I7", "release I1 1"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        expect_rejected(invalid[i]);
    }
    char long_line[300];
    memset(long_line, ' ', sizeof(long_line) - 1);
    memcpy(long_line, "mode-fixed", strlen("mode-fixed"));
    long_line[sizeof(long_line) - 1] = '\0';
    expect_rejected(long_line);

    unsigned target;
    require(central_parse_command(NULL, &message, &target) == 0, "Null input");
    require(central_parse_command("mode-fixed", NULL, &target) == 0, "Null message output");
    require(central_parse_command("mode-fixed", &message, NULL) == 0, "Null target output");

    printf("Command parser: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
