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

// Timing
#define UI_CHECK_INTERVAL    1  // seconds - check for UI updates
#define HEARTBEAT_INTERVAL   3  // seconds - check connection alive

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
