#ifndef CENTRAL_COMMANDS_H
#define CENTRAL_COMMANDS_H

#include "../../common/protocol.h"

// Returns 1 for a valid command and 0 otherwise, leaving outputs unchanged.
// target is an intersection ID or INTERSECTION_ALL. The caller assigns the
// message's intersection ID, command ID and timestamp before sending it.
int central_parse_command(const char *line, test_message_t *message, unsigned *target);

typedef enum {
    CENTRAL_SIM_START,
    CENTRAL_SIM_STOP,
    CENTRAL_SIM_TIME
} central_sim_action_t;

typedef struct {
    unsigned target;
    uint16_t command_id;
    central_sim_action_t action;
    unsigned minute;
} central_sim_command_t;

// Central -> Local MSG_TEST extension, without changing the shared envelope ABI:
// "SIM1 <target> <command_id> START", "... STOP", or "... TIME <minute>".
// Decimal fields are canonical, separated by one space; all bytes after the
// terminating NUL are zero. Targets are 0..5 or 255; minutes are 0..1439.
// ID 0 and target 255 are allowed while preparing commands; IPC requires a
// nonzero ID and an individual 0..5 target after Central expands "all".
// Both successful and rejected replies must echo that command ID.
// START/STOP control only synthetic input generation, never the controller or
// lamps. TIME changes simulation time without releasing manual overrides or
// resetting lamp phases. Local must implement and ACK this contract explicitly.
// Returns 1 on success; malformed/non-simulation messages leave output unchanged.
int central_simulation_decode(const test_message_t *message, central_sim_command_t *output);

uint16_t central_command_id(const test_message_t *message);
unsigned central_command_target(const test_message_t *message);
void central_command_set_id(test_message_t *message, uint16_t id);
void central_command_set_target(test_message_t *message, unsigned target);

#endif
