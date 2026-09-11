#ifndef CENTRAL_COMMANDS_H
#define CENTRAL_COMMANDS_H

#include "../../common/protocol.h"

// Returns 1 for a valid command and 0 otherwise, leaving outputs unchanged.
// target is an intersection ID or INTERSECTION_ALL. The caller fills the
// prototype's intersection ID, command ID, session, sequence and timestamp.
int central_parse_command(const char *line, any_msg_t *prototype, unsigned *target);

#endif
