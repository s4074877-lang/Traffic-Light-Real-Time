#include "local_controller.h"

static const local_timing_config_t local_configs[NUM_INTERSECTIONS] = {
    /* User-selected stagger profile:
     * I1 EW, I2 NS, I3 NS, I4 EW, I5 EW, I6 NS.
     */
    { PHASE_EW_GREEN, 22, 28 },
    { PHASE_NS_GREEN, 30, 18 },
    { PHASE_NS_GREEN, 26, 20 },
    { PHASE_EW_GREEN, 18, 30 },
    { PHASE_EW_GREEN, 20, 26 },
    { PHASE_NS_GREEN, 28, 22 }
};

const local_timing_config_t* local_config_for_intersection(uint8_t intersection_id) {
    if (intersection_id >= NUM_INTERSECTIONS) {
        intersection_id = I1;
    }
    return &local_configs[intersection_id];
}
