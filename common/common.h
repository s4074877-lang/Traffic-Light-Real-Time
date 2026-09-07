#ifndef COMMON_H
#define COMMON_H

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>

// VM hostnames for QNET
#define VM_LOCAL        "vm1"
#define VM_TRAIN        "vm2"
#define VM_CENTRAL      "vm3"

// Service names for name_attach
#define LOCAL_SERVICE_NAME      "local_controller"
#define TRAIN_SERVICE_NAME      "train_controller"
#define CENTRAL_SERVICE_NAME    "central_controller"

// Timing
#define STATE_PRINT_INTERVAL    2  // seconds

// ANSI color codes
#define COLOR_RESET     "\033[0m"
#define COLOR_RED       "\033[31m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_BLUE      "\033[34m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_BOLD      "\033[1m"

// Controller types
typedef enum {
    CONTROLLER_LOCAL = 1,
    CONTROLLER_TRAIN = 2
} controller_type_t;

// Traffic light states (for local intersection)
typedef enum {
    LIGHT_RED = 0,
    LIGHT_YELLOW = 1,
    LIGHT_GREEN = 2
} light_state_t;

// Train crossing states
typedef enum {
    CROSSING_OPEN = 0,
    CROSSING_BLOCK = 1
} crossing_state_t;

// Message types
typedef enum {
    MSG_COMMAND = 1,        // Central -> Controller: change state
    MSG_STATUS_UPDATE = 2,  // Controller -> Central: report state change
    MSG_HEARTBEAT = 3       // Periodic connection check
} msg_type_t;

// Message structure for communication
typedef struct {
    msg_type_t type;
    controller_type_t controller;
    union {
        light_state_t light;
        crossing_state_t crossing;
    } state;
    char timestamp[32];
} controller_msg_t;

// Reply structure
typedef struct {
    int status;  // 0 = success, -1 = error
} controller_reply_t;

// Helper function to get timestamp string
static inline void get_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%H:%M:%S", tm_info);
}

// Helper to convert light state to string
static inline const char* light_state_str(light_state_t state) {
    switch (state) {
        case LIGHT_RED:    return "RED";
        case LIGHT_YELLOW: return "YELLOW";
        case LIGHT_GREEN:  return "GREEN";
        default:           return "UNKNOWN";
    }
}

// Helper to convert crossing state to string
static inline const char* crossing_state_str(crossing_state_t state) {
    switch (state) {
        case CROSSING_OPEN:  return "OPEN";
        case CROSSING_BLOCK: return "BLOCK";
        default:             return "UNKNOWN";
    }
}

// Helper to get color for light state
static inline const char* light_state_color(light_state_t state) {
    switch (state) {
        case LIGHT_RED:    return COLOR_RED;
        case LIGHT_YELLOW: return COLOR_YELLOW;
        case LIGHT_GREEN:  return COLOR_GREEN;
        default:           return COLOR_RESET;
    }
}

// Helper to get color for crossing state
static inline const char* crossing_state_color(crossing_state_t state) {
    switch (state) {
        case CROSSING_OPEN:  return COLOR_GREEN;
        case CROSSING_BLOCK: return COLOR_RED;
        default:             return COLOR_RESET;
    }
}

#endif // COMMON_H
