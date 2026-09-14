#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/dispatch.h>
#include <time.h>

#include "protocol.h"

// Service names for name service
#define LOCAL_SERVICE_NAME      "traffic_local_controller"
#define TRAIN_SERVICE_NAME      "traffic_train_controller"
#define CENTRAL_SERVICE_NAME    "traffic_central_controller"

// Qnet configuration - change VM names here for cross-VM communication
// Set to NULL or empty string "" for local connections (same VM)
#define CONNECTION_DIR          "/tmp/connection"
#define LOCAL_VM_NAME           "vm1_local_intersection"      // VM running Local controller(s)
#define TRAIN_VM_NAME           "vm2_train_controller"      // VM running Train controller
#define CENTRAL_VM_NAME         "vm3_central_controller"      // VM running Central controller

// Maximum service path length for Qnet paths
#define MAX_SERVICE_PATH        256

// Build Qnet service path: /net/{vm}/dev/name/global/{service} or just {service}
static inline void build_qnet_path(char *path, size_t size, const char *vm_name,
                                    const char *service, int global_mode) {
    if (vm_name && *vm_name) {
        // Remote VM via Qnet
        snprintf(path, size, "/net/%s/dev/name/%s/%s", vm_name,
                 global_mode ? "global" : "local", service);
    } else {
        // Local service (same VM)
        snprintf(path, size, "%s", service);
    }
}

// Timing
#define UI_CHECK_INTERVAL    1  // seconds - check for UI updates
#define HEARTBEAT_INTERVAL   1  // seconds - check connection alive
#define HEARTBEAT_MISS_THRESHOLD 3 // consecutive misses before link is down

// ANSI color codes
#define COLOR_RESET     "\033[0m"
#define COLOR_RED       "\033[31m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_BLUE      "\033[34m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_BOLD      "\033[1m"

// Helper function to get timestamp string
static inline void get_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%H:%M:%S", tm_info);
}

// Helper to get controller name string
static inline const char* controller_name(controller_type_t type) {
    switch (type) {
        case CONTROLLER_LOCAL:   return "local";
        case CONTROLLER_TRAIN:   return "train";
        case CONTROLLER_CENTRAL: return "central";
        default:                 return "unknown";
    }
}

#endif // COMMON_H
