#ifndef CENTRAL_OPERATOR_POLICY_H
#define CENTRAL_OPERATOR_POLICY_H

#include "../../common/protocol.h"
#include <stddef.h>
#include <stdint.h>

#define CENTRAL_TRAIN_PAYLOAD_SIZE 64
#define CENTRAL_SCHEDULE_MAX_ENTRIES 32
#define CENTRAL_SCHEDULE_MAX_FILE_LINES 1024

/* The caller strips the outer CLI "train " prefix. This parser accepts only
 * commands implemented by the current Train simulator, and produces a bounded,
 * canonical string. A valid command is not evidence that Train applied it.
 * Returns 1 on success; invalid input leaves payload unchanged. */
int central_parse_train_command(const char *line,
                                char payload[CENTRAL_TRAIN_PAYLOAD_SIZE]);

typedef struct {
    unsigned minute_of_day; /* Local wall-clock minute, 0 .. 1439. */
    unsigned target;        /* I1 .. I6 or INTERSECTION_ALL. */
    uint8_t mode;           /* MODE_FIXED or MODE_SENSOR. */
} central_schedule_entry_t;

typedef struct {
    size_t count;
    central_schedule_entry_t entries[CENTRAL_SCHEDULE_MAX_ENTRIES];
} central_schedule_t;

/* "HH:MM fixed|sensor I1|I2|I3|I4|I5|I6|all". Empty lines and # comments
 * return 0, valid entries return 1, invalid lines return -1. Output is unchanged
 * except on success. Duplicate times affecting the same target are rejected by
 * central_schedule_add, including an all/individual overlap. */
int central_schedule_parse_line(const char *line, central_schedule_entry_t *entry);
int central_schedule_add(central_schedule_t *schedule,
                         const central_schedule_entry_t *entry);

/* Startup-only file I/O, not a periodic real-time task. Loading is atomic:
 * errors leave schedule unchanged. Maximum 32 entries / 1024 physical lines. */
int central_schedule_load(const char *path, central_schedule_t *schedule,
                          char *error, size_t error_size);

/* Selects the latest applicable daily entry, wrapping to the previous day if
 * necessary. Returns 0 when that target has no schedule; mode stays unchanged. */
int central_schedule_mode(const central_schedule_t *schedule, unsigned target,
                          unsigned minute_of_day, uint8_t *mode);

typedef struct {
    uint8_t persistent_active[NUM_INTERSECTIONS];
    uint8_t persistent_mode[NUM_INTERSECTIONS];
    uint8_t temporary_mode[NUM_INTERSECTIONS];
    uint64_t temporary_until_ns[NUM_INTERSECTIONS];
} central_operator_policy_t;

/* Zero initialization means no operator overrides. This records Central's
 * desired operator policy only, never a confirmed peer state. The caller decides
 * when command delivery is sufficiently established to record an intent.
 * SET replaces the persistent override and cancels any temporary overlay;
 * TEMPORARY overlays the persistent override for a monotonic duration;
 * REVERT cancels only the temporary overlay, matching the shared protocol.
 * Invalid inputs (including deadline overflow) leave policy unchanged. */
int central_policy_note_operator(central_operator_policy_t *policy,
                                 unsigned target, uint8_t action, uint8_t mode,
                                 unsigned duration_sec, uint64_t now_ns);

/* Explicit Central operator action to release persistent and temporary holds.
 * This only changes Central policy; caller must separately issue an appropriate
 * mode command if the desired scheduled mode needs to be applied by Local. */
int central_policy_resume_schedule(central_operator_policy_t *policy,
                                   unsigned target);

/* Schedule dispatch is suppressed while either override is active. */
int central_policy_operator_active(const central_operator_policy_t *policy,
                                   unsigned target, uint64_t now_ns);

/* Priority is CMD_PRIO_OPERATOR or CMD_PRIO_SCHEDULE. On temporary expiry the
 * previous persistent override wins, otherwise the current schedule wins.
 * No timing state is inferred from the wall clock; no peer command is sent.
 * A result describes desired mode, not an applied lamp state. */
int central_policy_desired_mode(const central_operator_policy_t *policy,
                                const central_schedule_t *schedule,
                                unsigned target, unsigned minute_of_day,
                                uint64_t now_ns, uint8_t *mode,
                                uint8_t *priority);

#endif
