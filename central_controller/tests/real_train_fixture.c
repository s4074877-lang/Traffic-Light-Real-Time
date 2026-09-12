/* Build the teammate's unchanged Train implementation into an isolated test
 * executable. Only its service names differ, so validation cannot accidentally
 * connect a fixture Central to a production Train or Local service. */
#include "../../common/common.h"
#undef LOCAL_SERVICE_NAME
#undef TRAIN_SERVICE_NAME
#undef CENTRAL_SERVICE_NAME
#define LOCAL_SERVICE_NAME "traffic_test_central_only_local"
#define TRAIN_SERVICE_NAME "traffic_test_central_only_train"
#define CENTRAL_SERVICE_NAME "traffic_test_central_only"
#include "../../train_controller/src/train_controller.c"
