#include "local_process.h"

/* Parse only view commands. A failed parse never changes the selection. */
int local_ui_parse_view(const char *command, int *selected) {
    if (strcmp(command, "view all") == 0) {
        *selected = INTERSECTION_ALL;
        return 1;
    }
    if (strlen(command) == 7 && strncmp(command, "view ", 5) == 0 &&
        (command[5] == 'I' || command[5] == 'i') &&
        command[6] >= '1' && command[6] <= '6') {
        *selected = command[6] - '1';
        return 1;
    }
    return 0;
}
