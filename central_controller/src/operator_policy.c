#include "operator_policy.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define POLICY_LINE_SIZE 256
#define NS_PER_SECOND UINT64_C(1000000000)

static int tokenize(const char *line, char buffer[POLICY_LINE_SIZE],
                    char *tokens[4], size_t *count, int allow_comment) {
    if (line == NULL) return 0;
    size_t length = 0;
    while (length < POLICY_LINE_SIZE && line[length] != '\0') length++;
    if (length == POLICY_LINE_SIZE) return 0;
    memcpy(buffer, line, length + 1);
    *count = 0;
    char *cursor = buffer;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || (allow_comment && *cursor == '#')) break;
        if (*count == 4) return 0;
        tokens[(*count)++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            if (allow_comment && *cursor == '#') {
                *cursor = '\0';
                return 1;
            }
            if (iscntrl((unsigned char)*cursor)) return 0;
            cursor++;
        }
        if (*cursor != '\0') *cursor++ = '\0';
    }
    return 1;
}

static int parse_unsigned(const char *token, unsigned minimum, unsigned maximum,
                          unsigned *value) {
    unsigned parsed = 0;
    if (*token == '\0') return 0;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned digit = (unsigned)(*p - '0');
        if (parsed > maximum / 10 ||
            (parsed == maximum / 10 && digit > maximum % 10)) return 0;
        parsed = parsed * 10 + digit;
    }
    if (parsed < minimum) return 0;
    *value = parsed;
    return 1;
}

static int parse_crossing(const char *token, unsigned *crossing) {
    if (strlen(token) != 2 || (token[0] != 'P' && token[0] != 'p') ||
        token[1] < '1' || token[1] > '0' + NUM_CROSSINGS) return 0;
    *crossing = (unsigned)(token[1] - '0');
    return 1;
}

static int parse_direction(const char *token, const char **direction) {
    if (strcmp(token, "up") == 0 || strcmp(token, "UP") == 0) {
        *direction = "up";
        return 1;
    }
    if (strcmp(token, "down") == 0 || strcmp(token, "DOWN") == 0) {
        *direction = "down";
        return 1;
    }
    return 0;
}

int central_parse_train_command(const char *line,
                                char payload[CENTRAL_TRAIN_PAYLOAD_SIZE]) {
    char buffer[POLICY_LINE_SIZE], canonical[CENTRAL_TRAIN_PAYLOAD_SIZE] = {0};
    char *tokens[4];
    size_t count;
    if (payload == NULL || !tokenize(line, buffer, tokens, &count, 0) || count == 0)
        return 0;
    unsigned crossing, value;
    const char *direction;
    if (count == 1 &&
        (strcmp(tokens[0], "train-up") == 0 || strcmp(tokens[0], "train-down") == 0 ||
         strcmp(tokens[0], "test") == 0 || strcmp(tokens[0], "status") == 0)) {
        snprintf(canonical, sizeof(canonical), "%s", tokens[0]);
    } else if (count == 1 && strlen(tokens[0]) == 8 &&
               (tokens[0][0] == 'p' || tokens[0][0] == 'P') &&
               tokens[0][1] >= '1' && tokens[0][1] <= '0' + NUM_CROSSINGS &&
               strcmp(tokens[0] + 2, "-fault") == 0) {
        snprintf(canonical, sizeof(canonical), "p%c-fault", tokens[0][1]);
    } else if (count == 3 &&
               (strcmp(tokens[0], "train") == 0 || strcmp(tokens[0], "noexit") == 0) &&
               parse_crossing(tokens[1], &crossing) &&
               parse_direction(tokens[2], &direction)) {
        snprintf(canonical, sizeof(canonical), "%s P%u %s", tokens[0], crossing,
                 direction);
    } else if (count == 2 &&
               (strcmp(tokens[0], "stuck") == 0 || strcmp(tokens[0], "reset") == 0) &&
               parse_crossing(tokens[1], &crossing)) {
        snprintf(canonical, sizeof(canonical), "%s P%u", tokens[0], crossing);
    } else if (count == 2 && strcmp(tokens[0], "test") == 0 &&
               parse_unsigned(tokens[1], 1, 7, &value)) {
        snprintf(canonical, sizeof(canonical), "test %u", value);
    } else if (count == 2 && strcmp(tokens[0], "scale") == 0 &&
               parse_unsigned(tokens[1], 1, 100, &value)) {
        snprintf(canonical, sizeof(canonical), "scale %u", value);
    } else {
        return 0;
    }
    memcpy(payload, canonical, sizeof(canonical));
    return 1;
}

static int valid_target(unsigned target) {
    return target < NUM_INTERSECTIONS || target == INTERSECTION_ALL;
}

static int valid_entry(const central_schedule_entry_t *entry) {
    return entry != NULL && entry->minute_of_day < 24 * 60 &&
           valid_target(entry->target) &&
           (entry->mode == MODE_FIXED || entry->mode == MODE_SENSOR);
}

int central_schedule_parse_line(const char *line, central_schedule_entry_t *entry) {
    char buffer[POLICY_LINE_SIZE], *tokens[4];
    size_t count;
    if (entry == NULL || !tokenize(line, buffer, tokens, &count, 1)) return -1;
    if (count == 0) return 0;
    if (count != 3 || strlen(tokens[0]) != 5 || tokens[0][2] != ':' ||
        tokens[0][0] < '0' || tokens[0][0] > '9' ||
        tokens[0][1] < '0' || tokens[0][1] > '9' ||
        tokens[0][3] < '0' || tokens[0][3] > '9' ||
        tokens[0][4] < '0' || tokens[0][4] > '9') return -1;
    unsigned hour = (unsigned)(tokens[0][0] - '0') * 10 + (unsigned)(tokens[0][1] - '0');
    unsigned minute = (unsigned)(tokens[0][3] - '0') * 10 + (unsigned)(tokens[0][4] - '0');
    if (hour >= 24 || minute >= 60) return -1;
    central_schedule_entry_t parsed = {0};
    parsed.minute_of_day = hour * 60 + minute;
    if (strcmp(tokens[1], "fixed") == 0) parsed.mode = MODE_FIXED;
    else if (strcmp(tokens[1], "sensor") == 0) parsed.mode = MODE_SENSOR;
    else return -1;
    if (strcmp(tokens[2], "all") == 0) parsed.target = INTERSECTION_ALL;
    else if (strlen(tokens[2]) == 2 && tokens[2][0] == 'I' &&
             tokens[2][1] >= '1' && tokens[2][1] <= '0' + NUM_INTERSECTIONS)
        parsed.target = (unsigned)(tokens[2][1] - '1');
    else return -1;
    *entry = parsed;
    return 1;
}

int central_schedule_add(central_schedule_t *schedule,
                         const central_schedule_entry_t *entry) {
    if (schedule == NULL || !valid_entry(entry) ||
        schedule->count >= CENTRAL_SCHEDULE_MAX_ENTRIES) return 0;
    for (size_t i = 0; i < schedule->count; i++) {
        const central_schedule_entry_t *existing = &schedule->entries[i];
        if (existing->minute_of_day == entry->minute_of_day &&
            (existing->target == entry->target || existing->target == INTERSECTION_ALL ||
             entry->target == INTERSECTION_ALL)) return 0;
    }
    schedule->entries[schedule->count++] = *entry;
    return 1;
}

static void load_error(char *error, size_t error_size, unsigned line,
                       const char *description) {
    if (error == NULL || error_size == 0) return;
    if (line != 0) snprintf(error, error_size, "Schedule line %u: %s", line, description);
    else snprintf(error, error_size, "%s", description);
}

int central_schedule_load(const char *path, central_schedule_t *schedule,
                          char *error, size_t error_size) {
    if (path == NULL || schedule == NULL) {
        load_error(error, error_size, 0, "Missing schedule path or output");
        return 0;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        load_error(error, error_size, 0, strerror(errno));
        return 0;
    }
    central_schedule_t parsed = {0};
    unsigned line_number = 1;
    size_t length = 0;
    char line[POLICY_LINE_SIZE];
    int ok = 1;
    for (;;) {
        int character = fgetc(file);
        if (character != EOF && line_number > CENTRAL_SCHEDULE_MAX_FILE_LINES) {
            load_error(error, error_size, line_number, "Too many physical lines");
            ok = 0;
            break;
        }
        if (character == EOF && ferror(file)) {
            load_error(error, error_size, line_number, "Read failed");
            ok = 0;
            break;
        }
        if (character == '\0') {
            load_error(error, error_size, line_number, "NUL byte is not valid text");
            ok = 0;
            break;
        }
        if (character != EOF && character != '\n') {
            if (length == sizeof(line) - 1) {
                load_error(error, error_size, line_number, "Line exceeds 255 bytes");
                ok = 0;
                break;
            }
            line[length++] = (char)character;
            continue;
        }
        if (character == EOF && length == 0) break;
        line[length] = '\0';
        central_schedule_entry_t entry;
        int result = central_schedule_parse_line(line, &entry);
        if (result < 0 || (result > 0 && !central_schedule_add(&parsed, &entry))) {
            load_error(error, error_size, line_number,
                       result < 0 ? "Expected HH:MM fixed|sensor I1..I6|all" :
                                    "Conflicting time/target or more than 32 entries");
            ok = 0;
            break;
        }
        length = 0;
        line_number++;
        if (character == EOF) break;
    }
    if (fclose(file) != 0 && ok) {
        load_error(error, error_size, 0, "Closing schedule file failed");
        ok = 0;
    }
    if (!ok) return 0;
    *schedule = parsed;
    if (error != NULL && error_size != 0) error[0] = '\0';
    return 1;
}

int central_schedule_mode(const central_schedule_t *schedule, unsigned target,
                          unsigned minute_of_day, uint8_t *mode) {
    if (schedule == NULL || mode == NULL || target >= NUM_INTERSECTIONS ||
        minute_of_day >= 24 * 60 || schedule->count > CENTRAL_SCHEDULE_MAX_ENTRIES)
        return 0;
    unsigned nearest = 24 * 60;
    uint8_t selected = 0;
    for (size_t i = 0; i < schedule->count; i++) {
        const central_schedule_entry_t *entry = &schedule->entries[i];
        if (!valid_entry(entry)) return 0;
        if (entry->target != INTERSECTION_ALL && entry->target != target) continue;
        unsigned elapsed = (minute_of_day + 24 * 60 - entry->minute_of_day) % (24 * 60);
        if (elapsed < nearest) {
            nearest = elapsed;
            selected = entry->mode;
        }
    }
    if (nearest == 24 * 60) return 0;
    *mode = selected;
    return 1;
}

int central_policy_note_operator(central_operator_policy_t *policy,
                                 unsigned target, uint8_t action, uint8_t mode,
                                 unsigned duration_sec, uint64_t now_ns) {
    if (policy == NULL || !valid_target(target)) return 0;
    uint64_t expires = 0;
    if (action == CMD_SET_MODE || action == CMD_TEMPORARY) {
        if (mode != MODE_FIXED && mode != MODE_SENSOR) return 0;
        if (action == CMD_SET_MODE && duration_sec != 0) return 0;
        if (action == CMD_TEMPORARY) {
            if (duration_sec == 0 || duration_sec > UINT16_MAX) return 0;
            uint64_t duration = (uint64_t)duration_sec * NS_PER_SECOND;
            if (now_ns > UINT64_MAX - duration) return 0;
            expires = now_ns + duration;
        }
    } else if (action != CMD_REVERT || duration_sec != 0) {
        return 0;
    }
    unsigned begin = target == INTERSECTION_ALL ? 0 : target;
    unsigned end = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    for (unsigned i = begin; i < end; i++) {
        if (action == CMD_SET_MODE) {
            policy->persistent_active[i] = 1;
            policy->persistent_mode[i] = mode;
            policy->temporary_until_ns[i] = 0;
        } else if (action == CMD_TEMPORARY) {
            policy->temporary_mode[i] = mode;
            policy->temporary_until_ns[i] = expires;
        } else {
            policy->temporary_until_ns[i] = 0;
        }
    }
    return 1;
}

int central_policy_resume_schedule(central_operator_policy_t *policy,
                                   unsigned target) {
    if (policy == NULL || !valid_target(target)) return 0;
    unsigned begin = target == INTERSECTION_ALL ? 0 : target;
    unsigned end = target == INTERSECTION_ALL ? NUM_INTERSECTIONS : target + 1;
    for (unsigned i = begin; i < end; i++) {
        policy->persistent_active[i] = 0;
        policy->temporary_until_ns[i] = 0;
    }
    return 1;
}

int central_policy_operator_active(const central_operator_policy_t *policy,
                                   unsigned target, uint64_t now_ns) {
    return policy != NULL && target < NUM_INTERSECTIONS &&
           (policy->persistent_active[target] || policy->temporary_until_ns[target] > now_ns);
}

int central_policy_desired_mode(const central_operator_policy_t *policy,
                                const central_schedule_t *schedule,
                                unsigned target, unsigned minute_of_day,
                                uint64_t now_ns, uint8_t *mode, uint8_t *priority) {
    if (policy == NULL || mode == NULL || priority == NULL || target >= NUM_INTERSECTIONS ||
        minute_of_day >= 24 * 60) return 0;
    uint8_t selected;
    if (policy->temporary_until_ns[target] > now_ns) {
        selected = policy->temporary_mode[target];
    } else if (policy->persistent_active[target]) {
        selected = policy->persistent_mode[target];
    } else {
        if (!central_schedule_mode(schedule, target, minute_of_day, &selected)) return 0;
        *mode = selected;
        *priority = CMD_PRIO_SCHEDULE;
        return 1;
    }
    *mode = selected;
    *priority = CMD_PRIO_OPERATOR;
    return 1;
}
