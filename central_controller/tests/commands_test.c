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
    require(central_command_id(&message) == 0 &&
            central_command_target(&message) == (type == MSG_TEST ? expected_target : I1) &&
            message.header.timestamp[0] == '\0', "Sender fills command ID, target and timestamp");
    central_command_set_id(&message, 42);
    central_command_set_target(&message, target);
    require(central_command_id(&message) == 42 && central_command_target(&message) == target,
            "Command ID and target helpers");
    size_t payload_size = type == MSG_MODE_COMMAND ? sizeof(mode_cmd_msg_t) :
        type == MSG_COORDINATION_COMMAND ? sizeof(coordination_command_msg_t) : strlen(message.data) + 1;
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

static void expect_invalid_simulation(const test_message_t *message, const char *description) {
    central_sim_command_t decoded;
    memset(&decoded, 0xa5, sizeof(decoded));
    unsigned char before[sizeof(decoded)];
    memcpy(before, &decoded, sizeof(decoded));
    require(!central_simulation_decode(message, &decoded), description);
    require(memcmp(before, &decoded, sizeof(decoded)) == 0,
            "Invalid simulation payload leaves decoded output unchanged");
    test_message_t copy = *message;
    central_command_set_id(&copy, 7);
    central_command_set_target(&copy, I2);
    require(central_command_id(message) == 0 && central_command_target(message) == NUM_INTERSECTIONS &&
            memcmp(&copy, message, sizeof(copy)) == 0,
            "Invalid/legacy simulation-like messages are not mutated by command helpers");
}

static void test_simulation(void) {
    central_sim_command_t decoded;
    test_message_t message = expect_command("sim-start I1", I1, MSG_TEST);
    require(central_simulation_decode(&message, &decoded) && decoded.action == CENTRAL_SIM_START &&
            decoded.target == I1 && decoded.command_id == 42 && decoded.minute == 0 &&
            strcmp(message.data, "SIM1 0 42 START") == 0, "Canonical start simulation command");
    message = expect_command("sim-stop all", INTERSECTION_ALL, MSG_TEST);
    require(central_simulation_decode(&message, &decoded) && decoded.action == CENTRAL_SIM_STOP &&
            strcmp(message.data, "SIM1 255 42 STOP") == 0, "Canonical broadcast stop command");
    message = expect_command("sim-time I6 23:59", I6, MSG_TEST);
    require(central_simulation_decode(&message, &decoded) && decoded.action == CENTRAL_SIM_TIME &&
            decoded.minute == 1439 && strcmp(message.data, "SIM1 5 42 TIME 1439") == 0,
            "Last minute of simulation day");
    central_command_set_id(&message, UINT16_MAX);
    central_command_set_target(&message, INTERSECTION_ALL);
    require(central_simulation_decode(&message, &decoded) && decoded.action == CENTRAL_SIM_TIME &&
            decoded.minute == 1439 && decoded.target == INTERSECTION_ALL &&
            decoded.command_id == UINT16_MAX, "Simulation helpers preserve action/time at largest fields");
    test_message_t before = message;
    central_command_set_target(&message, NUM_INTERSECTIONS);
    central_command_set_target(&message, 256);
    require(memcmp(&before, &message, sizeof(message)) == 0, "Invalid simulation targets do not wrap");
    central_command_set_id(&message, 0);
    central_command_set_target(&message, I1);
    require(central_simulation_decode(&message, &decoded) && decoded.command_id == 0 &&
            decoded.target == I1 && decoded.minute == 1439 &&
            strcmp(message.data, "SIM1 0 0 TIME 1439") == 0,
            "Re-encoding shorter fields clears old payload tail and allows internal ID zero");
    message = expect_command(" \tsim-time\tall 00:00 \r\n", INTERSECTION_ALL, MSG_TEST);
    require(central_simulation_decode(&message, &decoded) && decoded.minute == 0,
            "Midnight simulation time with CLI whitespace");
    message = expect_command("sim-time I2 07:00", I2, MSG_TEST);
    require(central_simulation_decode(&message, &decoded) && decoded.minute == 420,
            "Peak-hour simulation time");

    const char *invalid[] = {
        "", "train-up", "SIM", "SIM2 0 1 START", "SIM1", "SIM1 0 1", "SIM1 0 1 TIME",
        "SIM1 0 1 START 1", "SIM1 0 1 STOP extra", "SIM1 0 1 TIME 0 extra",
        "SIM1 6 1 START", "SIM1 254 1 START", "SIM1 256 1 START", "SIM1 -1 1 START",
        "SIM1 0 -1 START", "SIM1 0 65536 START", "SIM1 0 999999999999999999999999 START",
        "SIM1 0 1 TIME 1440", "SIM1 0 1 TIME -1", "SIM1 0 1 TIME +1", "SIM1 0 1 TIME 1.0",
        "SIM1 0 1 TIME 000", "SIM1 00 1 START", "SIM1 0 01 START", "SIM1 0 +1 START",
        " SIM1 0 1 START", "SIM1 0 1 START ", "SIM1  0 1 START", "SIM1\t0 1 START",
        "SIM1 0 1 START\n", "SIM1 0 1 start", "SIM1 0 1 UNKNOWN"
    };
    test_message_t valid = message;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        message = valid;
        memset(message.data, 0, sizeof(message.data));
        strcpy(message.data, invalid[i]);
        expect_invalid_simulation(&message, invalid[i]);
    }
    message = valid;
    memset(message.data, 'x', sizeof(message.data));
    expect_invalid_simulation(&message, "Unterminated payload rejected");
    message = valid;
    message.data[strlen(message.data) + 1] = 'x';
    expect_invalid_simulation(&message, "Embedded terminator cannot hide trailing data");
    message = valid;
    message.header.src = CONTROLLER_LOCAL;
    expect_invalid_simulation(&message, "Only Central can send simulation commands");
    message = valid;
    message.header.dst = CONTROLLER_TRAIN;
    expect_invalid_simulation(&message, "Train-directed test messages remain outside simulation contract");
    message = valid;
    message.header.reserved = 1;
    expect_invalid_simulation(&message, "Reserved header field rejected");
    message = valid;
    memset(message.header.timestamp, 'x', sizeof(message.header.timestamp));
    expect_invalid_simulation(&message, "Unterminated simulation header timestamp rejected");
    message = valid;
    message.header.type = MSG_OVERRIDE_REQUEST;
    expect_invalid_simulation(&message, "Simulation text in another envelope is not a command");
    require(!central_simulation_decode(NULL, &decoded) && !central_simulation_decode(&valid, NULL),
            "Null simulation decode arguments rejected");
}

int main(void) {
    test_simulation();
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
    message = expect_command("coordinate all EW 63", INTERSECTION_ALL, MSG_COORDINATION_COMMAND);
    memcpy(&coordination, message.data, sizeof(coordination));
    require(coordination.phase == PHASE_EW_GREEN && coordination.cycle_offset_sec == 63,
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
    require(coordination.phase == PHASE_EW_GREEN && coordination.cycle_offset_sec == 63,
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
        "coordinate I1 NS 64", "coordinate I1 NS -1", "coordinate I1 NS +1",
        "coordinate I1 NS 0.5", "coordinate I1 ns 1", "coordinate I1 RED 1",
        "coordinate I9 EW 0", "coordinate I1 NS 0 extra",
        "override I1 NS 10", "release all",
        "sim-start", "sim-stop", "sim-time", "sim-start I0", "sim-start I7",
        "sim-start I1 extra", "sim-stop all extra", "sim-stop ALL", "sim-stop i1",
        "sim-time I1", "sim-time I1 24:00", "sim-time I1 23:60", "sim-time I1 7:00",
        "sim-time I1 07:0", "sim-time I1 07:00:00", "sim-time I1 07:00 extra",
        "sim-time I1 -1:00", "sim-time I1 12:ab", "sim-time I1 +1:00", "sim-time I1 420"
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
