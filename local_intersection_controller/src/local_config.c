#include "local_controller.h"

static const local_timing_config_t local_configs[NUM_INTERSECTIONS] = {
    /* User-selected stagger profile:
     * I1 EW, I2 NS, I3 NS, I4 EW, I5 EW, I6 NS.
     */
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_EW_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC },
    { PHASE_NS_GREEN, GREEN_BASE_SEC, GREEN_BASE_SEC }
};

const local_timing_config_t* local_config_for_intersection(uint8_t intersection_id) {
    if (intersection_id >= NUM_INTERSECTIONS) {
        intersection_id = I1;
    }
    return &local_configs[intersection_id];
}
