#ifndef CENTRAL_COMMANDS_H
#define CENTRAL_COMMANDS_H

#include "../../common/protocol.h"

// Returns 1 for a valid command and 0 otherwise, leaving outputs unchanged.
// target is an intersection ID or INTERSECTION_ALL. The caller assigns the
// message's intersection ID, command ID and timestamp before sending it.
int central_parse_command(const char *line, test_message_t *message, unsigned *target);

uint16_t central_command_id(const test_message_t *message);
unsigned central_command_target(const test_message_t *message);
void central_command_set_id(test_message_t *message, uint16_t id);
void central_command_set_target(test_message_t *message, unsigned target);

#endif
