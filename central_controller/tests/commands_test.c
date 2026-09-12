#include "../src/commands.h"
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

static test_message_t expect_command(const char *line, unsigned expected_target, msg_type_t type) {
    test_message_t message;
    unsigned target = NUM_INTERSECTIONS;
    require(central_parse_command(line, &message, &target) == 1, line);
    require(target == expected_target, line);
    require(message.header.type == type && message.header.src == CONTROLLER_CENTRAL &&
            message.header.dst == CONTROLLER_LOCAL && message.header.reserved == 0, line);
    require(central_command_id(&message) == 0 && central_command_target(&message) == I1 &&
            message.header.timestamp[0] == '\0', "Sender fills command ID, target and timestamp");
    central_command_set_id(&message, 42);
    central_command_set_target(&message, target);
    require(central_command_id(&message) == 42 && central_command_target(&message) == target,
            "Command ID and target helpers");
    size_t payload_size = type == MSG_MODE_COMMAND ? sizeof(mode_cmd_msg_t)
                                                  : sizeof(coordination_command_msg_t);
    int zero_tail = 1;
    for (size_t i = payload_size; i < sizeof(message.data); i++) {
        zero_tail = zero_tail && message.data[i] == 0;
    }
    require(zero_tail, "Unused message data stays zero");
    return message;
}

static void expect_rejected(const char *line) {
    test_message_t message;
    memset(&message, 0xa5, sizeof(message));
    unsigned char before[sizeof(message)];
    memcpy(before, &message, sizeof(message));
    unsigned target = 77;
    require(central_parse_command(line, &message, &target) == 0, line);
    require(memcmp(&message, before, sizeof(message)) == 0 && target == 77,
            "Invalid input must leave outputs unchanged");
}

int main(void) {
    test_message_t message = expect_command("mode-fixed", INTERSECTION_ALL, MSG_MODE_COMMAND);
    mode_cmd_msg_t mode;
    memcpy(&mode, message.data, sizeof(mode));
    require(mode.new_mode == MODE_FIXED && mode.action == CMD_SET_MODE &&
            mode.priority == CMD_PRIO_OPERATOR && mode.duration_sec == 0,
            "Persistent fixed mode");

    message = expect_command("mode-sensor I6", I6, MSG_MODE_COMMAND);
    memcpy(&mode, message.data, sizeof(mode));
    require(mode.new_mode == MODE_SENSOR, "Sensor mode at I6");
    expect_command("mode-fixed I1", I1, MSG_MODE_COMMAND);
    expect_command(" \tmode-sensor\tall \r\n", INTERSECTION_ALL, MSG_MODE_COMMAND);

    message = expect_command("mode-temp I1 sensor 65535", I1, MSG_MODE_COMMAND);
    memcpy(&mode, message.data, sizeof(mode));
    require(mode.new_mode == MODE_SENSOR && mode.action == CMD_TEMPORARY &&
            mode.duration_sec == UINT16_MAX, "Maximum temporary duration");
    message = expect_command("mode-temp all fixed 1", INTERSECTION_ALL, MSG_MODE_COMMAND);
    memcpy(&mode, message.data, sizeof(mode));
    require(mode.new_mode == MODE_FIXED && mode.duration_sec == 1, "Minimum temporary duration");
    message = expect_command("mode-revert I6", I6, MSG_MODE_COMMAND);
    memcpy(&mode, message.data, sizeof(mode));
    require(mode.action == CMD_REVERT && mode.duration_sec == 0, "Cancel temporary mode");

    coordination_command_msg_t coordination;
    message = expect_command("coordinate I3 NS 0", I3, MSG_COORDINATION_COMMAND);
    memcpy(&coordination, message.data, sizeof(coordination));
    require(coordination.phase == PHASE_NS_GREEN && coordination.mode == MODE_FIXED &&
            coordination.cycle_offset_sec == 0, "Start of coordination cycle");
    message = expect_command("coordinate all EW 43", INTERSECTION_ALL, MSG_COORDINATION_COMMAND);
    memcpy(&coordination, message.data, sizeof(coordination));
    require(coordination.phase == PHASE_EW_GREEN && coordination.cycle_offset_sec == 43,
            "Last valid coordination offset");

    unsigned char before[sizeof(message)];
    memcpy(before, &message, sizeof(message));
    central_command_set_target(&message, NUM_INTERSECTIONS);
    central_command_set_target(&message, 256);
    require(memcmp(before, &message, sizeof(message)) == 0, "Invalid target does not wrap or mutate");
    central_command_set_id(&message, UINT16_MAX);
    central_command_set_target(&message, I6);
    require(central_command_id(&message) == UINT16_MAX && central_command_target(&message) == I6,
            "Full command ID and target ranges");
    memcpy(&coordination, message.data, sizeof(coordination));
    require(coordination.phase == PHASE_EW_GREEN && coordination.cycle_offset_sec == 43,
            "Helpers preserve the command parameters");

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
        "coordinate I9 EW 0", "coordinate I1 NS 0 extra",
        "override I1 NS 10", "release all"
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
    central_command_set_id(NULL, 42);
    central_command_set_target(NULL, I1);
    require(central_command_id(NULL) == 0 && central_command_target(NULL) == NUM_INTERSECTIONS,
            "Null helper input");

    memset(&message, 0x5a, sizeof(message));
    message.header.type = MSG_OVERRIDE_REQUEST;
    memcpy(before, &message, sizeof(message));
    central_command_set_id(&message, 42);
    central_command_set_target(&message, I1);
    require(central_command_id(&message) == 0 &&
            central_command_target(&message) == NUM_INTERSECTIONS &&
            memcmp(before, &message, sizeof(message)) == 0, "Non-command payload stays unchanged");

    printf("Command parser: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
