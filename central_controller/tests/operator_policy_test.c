#include "../src/operator_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS_SECOND UINT64_C(1000000000)

static unsigned checks;

static void require(int condition, const char *description) {
    checks++;
    if (!condition) {
        fprintf(stderr, "Operator policy check failed: %s\n", description);
        exit(EXIT_FAILURE);
    }
}

static void train_accept(const char *line, const char *expected) {
    char payload[CENTRAL_TRAIN_PAYLOAD_SIZE];
    memset(payload, 0xa5, sizeof(payload));
    require(central_parse_train_command(line, payload), line);
    require(strcmp(payload, expected) == 0, "Train payload is canonical");
    for (size_t i = strlen(payload) + 1; i < sizeof(payload); i++)
        require(payload[i] == 0, "Unused Train payload is zero initialized");
}

static void train_reject(const char *line) {
    char payload[CENTRAL_TRAIN_PAYLOAD_SIZE], before[CENTRAL_TRAIN_PAYLOAD_SIZE];
    memset(payload, 0xa5, sizeof(payload));
    memcpy(before, payload, sizeof(before));
    require(!central_parse_train_command(line, payload), "Invalid Train command rejected");
    require(memcmp(before, payload, sizeof(payload)) == 0,
            "Invalid command does not alter caller output");
}

static central_schedule_entry_t parse_entry(const char *line) {
    central_schedule_entry_t entry;
    require(central_schedule_parse_line(line, &entry) == 1, line);
    return entry;
}

static void add_entry(central_schedule_t *schedule, const char *line) {
    central_schedule_entry_t entry = parse_entry(line);
    require(central_schedule_add(schedule, &entry), "Valid entry can be added");
}

static void expect_mode(const central_schedule_t *schedule, unsigned target,
                        unsigned minute, unsigned expected) {
    uint8_t mode = 99;
    require(central_schedule_mode(schedule, target, minute, &mode),
            "Daily schedule has a mode");
    require(mode == expected, "Daily schedule chooses most recent applicable entry");
}

static void write_fixture(const char *path, const void *contents, size_t length) {
    FILE *file = fopen(path, "wb");
    require(file != NULL, "Open isolated schedule fixture");
    require(fwrite(contents, 1, length, file) == length, "Write schedule fixture");
    require(fclose(file) == 0, "Close schedule fixture");
}

static void test_train_parser(void) {
    train_accept("train-up", "train-up");
    train_accept("train-down", "train-down");
    train_accept(" \ttrain p1 UP  ", "train P1 up");
    train_accept("noexit P3 DOWN", "noexit P3 down");
    train_accept("stuck p2", "stuck P2");
    train_accept("reset P3", "reset P3");
    train_accept("P1-fault", "p1-fault");
    train_accept("p3-fault", "p3-fault");
    train_accept("test", "test");
    train_accept("test 01", "test 1");
    train_accept("test 7", "test 7");
    train_accept("scale 001", "scale 1");
    train_accept("scale 100", "scale 100");
    train_accept("status", "status");
    for (unsigned crossing = 1; crossing <= NUM_CROSSINGS; crossing++) {
        char line[64];
        snprintf(line, sizeof(line), "train P%u up", crossing);
        train_accept(line, line);
        snprintf(line, sizeof(line), "train P%u down", crossing);
        train_accept(line, line);
        snprintf(line, sizeof(line), "noexit P%u up", crossing);
        train_accept(line, line);
        snprintf(line, sizeof(line), "noexit P%u down", crossing);
        train_accept(line, line);
    }
    const char *invalid[] = {
        "", " \t", "train", "train-up extra", "train-down extra", "train P0 up",
        "train P4 up", "train 1 up", "train P01 up", "train P1 sideways", "train P1",
        "train P1 up extra", "noexit P1 upside", "stuck P0", "stuck P4", "stuck P1 x",
        "reset P1;train-up", "reset P1 extra", "p0-fault", "p4-fault", "p1-fault extra",
        "fault P1", "test 0", "test 8", "test -1", "test +1", "test 1x", "test 1 2",
        "scale 0", "scale 101", "scale -1", "scale 1.0", "scale 100 extra",
        "scale 999999999999999999999999999999", "status extra", "help", "gate-up",
        "train-up\nreset P1", "train-up; reset P1", "$(train-up)", "stuck\001 P1"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) train_reject(invalid[i]);
    train_reject(NULL);
    require(!central_parse_train_command("status", NULL), "Null output rejected");
    char too_long[257];
    memset(too_long, ' ', sizeof(too_long));
    memcpy(too_long, "status", 6);
    too_long[sizeof(too_long) - 1] = '\0';
    train_reject(too_long);
}

static void test_schedule_parser(void) {
    central_schedule_entry_t entry = parse_entry("00:00 fixed all");
    require(entry.minute_of_day == 0 && entry.target == INTERSECTION_ALL &&
            entry.mode == MODE_FIXED, "Midnight broadcast parsed");
    entry = parse_entry("23:59 sensor I6 # end of day\r\n");
    require(entry.minute_of_day == 1439 && entry.target == I6 && entry.mode == MODE_SENSOR,
            "Last minute and inline comment parsed");
    for (unsigned target = 0; target < NUM_INTERSECTIONS; target++) {
        char line[64];
        snprintf(line, sizeof(line), "07:30 fixed I%u", target + 1);
        entry = parse_entry(line);
        require(entry.target == target && entry.minute_of_day == 450,
                "Intersection schedule IDs match protocol");
    }
    const char *invalid[] = {
        "24:00 fixed all", "23:60 sensor all", "7:00 fixed I1", "07:0 fixed I1",
        "007:00 fixed I1", "07-00 fixed I1", "-1:00 fixed I1", "aa:bb fixed I1",
        "07:00 railway I1", "07:00 failsafe I1", "07:00 fixed I0", "07:00 sensor I7",
        "07:00 fixed i1", "07:00 fixed I01", "07:00 fixed", "07:00 fixed all extra",
        "07:00 fixed all extra more", "07:00 fixed\001 all"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memset(&entry, 0xa5, sizeof(entry));
        central_schedule_entry_t before;
        memcpy(&before, &entry, sizeof(before));
        require(central_schedule_parse_line(invalid[i], &entry) == -1, invalid[i]);
        require(memcmp(&before, &entry, sizeof(entry)) == 0,
                "Invalid schedule line preserves output");
    }
    require(central_schedule_parse_line(" # Comment", &entry) == 0, "Comment ignored");
    require(central_schedule_parse_line(" \t\r\n", &entry) == 0, "Blank line ignored");
    require(central_schedule_parse_line(NULL, &entry) == -1, "Null schedule line rejected");
    require(central_schedule_parse_line("00:00 fixed all", NULL) == -1, "Null entry rejected");

    central_schedule_t schedule = {0};
    /* Deliberately unsorted: time selection must not depend on file order. */
    add_entry(&schedule, "22:00 sensor all");
    add_entry(&schedule, "07:00 fixed all");
    add_entry(&schedule, "09:00 sensor I1");
    for (unsigned minute = 0; minute < 1440; minute++) {
        expect_mode(&schedule, I1, minute,
                    minute >= 420 && minute < 540 ? MODE_FIXED : MODE_SENSOR);
        expect_mode(&schedule, I6, minute,
                    minute >= 420 && minute < 1320 ? MODE_FIXED : MODE_SENSOR);
    }
    central_schedule_t before = schedule;
    entry = parse_entry("07:00 sensor I2");
    require(!central_schedule_add(&schedule, &entry), "Same-time all/individual overlap rejected");
    require(memcmp(&before, &schedule, sizeof(schedule)) == 0, "Conflicting add is atomic");
    entry = parse_entry("09:00 fixed I1");
    require(!central_schedule_add(&schedule, &entry), "Duplicate target/time rejected");
    entry = parse_entry("09:00 fixed I2");
    require(central_schedule_add(&schedule, &entry), "Same time for disjoint targets allowed");
    entry = parse_entry("09:00 fixed all");
    require(!central_schedule_add(&schedule, &entry), "Broadcast cannot shadow explicit same-time entry");
    central_schedule_t sparse = {0};
    add_entry(&sparse, "13:00 fixed I3");
    expect_mode(&sparse, I3, 0, MODE_FIXED);
    uint8_t mode = 99;
    require(!central_schedule_mode(&sparse, I1, 0, &mode) && mode == 99,
            "Missing target schedule does not invent a default");
    require(!central_schedule_mode(&schedule, I1, 1440, &mode), "Invalid wall time rejected");
    require(!central_schedule_mode(&schedule, INTERSECTION_ALL, 0, &mode),
            "Lookup requires one actual intersection");
    central_schedule_t full = {0};
    for (unsigned minute = 0; minute < CENTRAL_SCHEDULE_MAX_ENTRIES; minute++) {
        entry.minute_of_day = minute;
        entry.target = INTERSECTION_ALL;
        entry.mode = MODE_FIXED;
        require(central_schedule_add(&full, &entry), "Add within fixed capacity");
    }
    entry.minute_of_day++;
    require(!central_schedule_add(&full, &entry), "Schedule capacity is bounded");
}

static void test_schedule_file(void) {
    char path[] = "/tmp/traffic_operator_policy_XXXXXX";
    int descriptor = mkstemp(path);
    require(descriptor >= 0 && close(descriptor) == 0, "Create isolated schedule fixture");
    const char valid[] = "# daily schedule\r\n07:00 fixed all\r\n22:00 sensor all";
    write_fixture(path, valid, sizeof(valid) - 1);
    central_schedule_t schedule = {0};
    char error[160];
    require(central_schedule_load(path, &schedule, error, sizeof(error)), "Load CRLF/no final newline file");
    require(schedule.count == 2 && error[0] == '\0', "Successful load replaces output and clears error");
    expect_mode(&schedule, I4, 0, MODE_SENSOR);
    central_schedule_t before = schedule;
    const char conflict[] = "07:00 fixed all\n07:00 sensor I1\n";
    write_fixture(path, conflict, sizeof(conflict) - 1);
    require(!central_schedule_load(path, &schedule, error, sizeof(error)), "Conflicting file rejected");
    require(strstr(error, "line 2") != NULL && memcmp(&before, &schedule, sizeof(schedule)) == 0,
            "Load error has line number and preserves entire old schedule");
    const char malformed[] = "07:00 fixed all\n24:00 sensor all\n";
    write_fixture(path, malformed, sizeof(malformed) - 1);
    require(!central_schedule_load(path, &schedule, error, sizeof(error)), "Malformed file rejected");
    require(memcmp(&before, &schedule, sizeof(schedule)) == 0, "Malformed load is atomic");
    const char binary[] = "07:00 fixed all\0ignored\n";
    write_fixture(path, binary, sizeof(binary) - 1);
    require(!central_schedule_load(path, &schedule, error, sizeof(error)) && strstr(error, "NUL") != NULL,
            "Embedded NUL cannot hide trailing input");
    char long_line[257];
    memset(long_line, '#', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\n';
    write_fixture(path, long_line, sizeof(long_line));
    require(!central_schedule_load(path, &schedule, error, sizeof(error)), "Oversized comment line rejected");
    char many_lines[CENTRAL_SCHEDULE_MAX_FILE_LINES + 1];
    memset(many_lines, '\n', sizeof(many_lines));
    write_fixture(path, many_lines, sizeof(many_lines));
    require(!central_schedule_load(path, &schedule, error, sizeof(error)), "Physical line count bounded");
    write_fixture(path, many_lines, CENTRAL_SCHEDULE_MAX_FILE_LINES);
    require(central_schedule_load(path, &schedule, error, sizeof(error)) && schedule.count == 0,
            "Maximum number of blank lines is accepted");
    require(unlink(path) == 0, "Remove only owned fixture");
    require(!central_schedule_load(path, &schedule, error, sizeof(error)), "Missing file returns a diagnostic");
    require(error[0] != '\0', "Missing file diagnostic is not empty");
    require(!central_schedule_load(NULL, &schedule, NULL, 0), "Null path handled");
}

static void expect_desired(const central_operator_policy_t *policy,
                           const central_schedule_t *schedule, unsigned target,
                           unsigned minute, uint64_t now, uint8_t expected_mode,
                           uint8_t expected_priority) {
    uint8_t mode = 99, priority = 99;
    require(central_policy_desired_mode(policy, schedule, target, minute, now, &mode, &priority),
            "Desired mode available");
    require(mode == expected_mode && priority == expected_priority,
            "Operator and schedule priority select expected desired mode");
}

static void test_operator_precedence(void) {
    central_operator_policy_t policy = {0};
    central_schedule_t schedule = {0};
    add_entry(&schedule, "07:00 fixed all");
    add_entry(&schedule, "22:00 sensor all");
    uint64_t now = 100 * NS_SECOND;
    expect_desired(&policy, &schedule, I1, 12 * 60, now, MODE_FIXED, CMD_PRIO_SCHEDULE);
    require(!central_policy_operator_active(&policy, I1, now), "No default operator hold");
    require(central_policy_note_operator(&policy, I1, CMD_SET_MODE, MODE_SENSOR, 0, now),
            "Record persistent operator override");
    require(central_policy_operator_active(&policy, I1, now), "Persistent operator suppresses schedule");
    expect_desired(&policy, &schedule, I1, 12 * 60, now, MODE_SENSOR, CMD_PRIO_OPERATOR);
    require(central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED, 10, now),
            "Temporary override overlays persistent operator intent");
    expect_desired(&policy, &schedule, I1, 23 * 60, now + 9 * NS_SECOND,
                   MODE_FIXED, CMD_PRIO_OPERATOR);
    expect_desired(&policy, &schedule, I1, 12 * 60, now + 10 * NS_SECOND,
                   MODE_SENSOR, CMD_PRIO_OPERATOR);
    require(central_policy_note_operator(&policy, I1, CMD_REVERT, MODE_FIXED, 0, now),
            "Revert command records cancel-temporary semantics");
    expect_desired(&policy, &schedule, I1, 12 * 60, now, MODE_SENSOR, CMD_PRIO_OPERATOR);
    require(central_policy_resume_schedule(&policy, I1), "Explicit resume clears persistent hold");
    require(!central_policy_operator_active(&policy, I1, now), "Schedule resumes after explicit release");
    require(central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED, 1, now),
            "Temporary operator mode without persistent baseline");
    expect_desired(&policy, &schedule, I1, 23 * 60, now + NS_SECOND - 1,
                   MODE_FIXED, CMD_PRIO_OPERATOR);
    expect_desired(&policy, &schedule, I1, 23 * 60, now + NS_SECOND,
                   MODE_SENSOR, CMD_PRIO_SCHEDULE);
    require(!central_policy_operator_active(&policy, I1, now + NS_SECOND),
            "Temporary hold expires exactly at monotonic deadline");
    require(central_policy_note_operator(&policy, I2, CMD_TEMPORARY, MODE_SENSOR, 60, now),
            "Second target temporary command");
    expect_desired(&policy, &schedule, I2, 0, now + 10 * NS_SECOND, MODE_SENSOR, CMD_PRIO_OPERATOR);
    expect_desired(&policy, &schedule, I2, 12 * 60, now + 10 * NS_SECOND,
                   MODE_SENSOR, CMD_PRIO_OPERATOR);
    require(central_policy_note_operator(&policy, I2, CMD_SET_MODE, MODE_FIXED, 0, now),
            "Persistent command supersedes temporary overlay");
    expect_desired(&policy, &schedule, I2, 0, now, MODE_FIXED, CMD_PRIO_OPERATOR);
    require(central_policy_note_operator(&policy, INTERSECTION_ALL, CMD_TEMPORARY, MODE_SENSOR,
                                         30, now), "Broadcast temporary intent");
    for (unsigned target = 0; target < NUM_INTERSECTIONS; target++)
        expect_desired(&policy, &schedule, target, 12 * 60, now, MODE_SENSOR, CMD_PRIO_OPERATOR);
    require(central_policy_resume_schedule(&policy, INTERSECTION_ALL), "Release all operator holds");
    for (unsigned target = 0; target < NUM_INTERSECTIONS; target++)
        require(!central_policy_operator_active(&policy, target, now), "Broadcast release covers every target");
    central_operator_policy_t before = policy;
    require(!central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED, 0, now),
            "Zero temporary duration rejected");
    require(!central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED,
                                          UINT16_MAX + 1U, now), "Unrepresentable duration rejected");
    require(!central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED, 1,
                                          UINT64_MAX - NS_SECOND + 1), "Monotonic deadline overflow rejected");
    require(!central_policy_note_operator(&policy, I1, CMD_SET_MODE, MODE_RAILWAY, 0, now),
            "Operator cannot select railway safety mode");
    require(!central_policy_note_operator(&policy, I1, CMD_SET_MODE, MODE_FIXED, 10, now),
            "Persistent command cannot carry duration");
    require(!central_policy_note_operator(&policy, NUM_INTERSECTIONS, CMD_REVERT, 0, 0, now),
            "Invalid target rejected");
    require(!central_policy_note_operator(&policy, I1, 99, 0, 0, now), "Unknown action rejected");
    require(memcmp(&before, &policy, sizeof(policy)) == 0, "Invalid policy updates leave all targets intact");
    uint8_t mode = 99, priority = 99;
    require(!central_policy_desired_mode(&policy, NULL, I1, 0, now, &mode, &priority) &&
            mode == 99 && priority == 99, "No schedule and no override produce no invented desired state");
    require(central_policy_note_operator(&policy, I1, CMD_TEMPORARY, MODE_FIXED, 1, 0),
            "Monotonic time zero is valid");
    require(central_policy_operator_active(&policy, I1, 0), "Temporary deadline works from clock origin");
}

int main(void) {
    test_train_parser();
    test_schedule_parser();
    test_schedule_file();
    test_operator_precedence();
    printf("Operator policy: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
