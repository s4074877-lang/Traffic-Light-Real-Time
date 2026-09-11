#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Protocol version
#define PROTOCOL_VERSION 1

// ============================================
// System configuration
// ============================================

#define NUM_INTERSECTIONS   6   // I1 - I6
#define NUM_CROSSINGS       3   // P1 - P3 (boom gate locations)

// Railway-affected intersection pairs (from the map):
//   P1 -> I1, I2      P2 -> I3, I4      P3 -> I5, I6

// ============================================
// Timing constants (seconds) - design assumptions
// ============================================

#define GREEN_BASE_SEC      20
#define GREEN_MIN_SEC       10
#define GREEN_MAX_SEC       30
#define YELLOW_SEC          2
#define PED_WALK_START_SEC  1  // Starts this many seconds after Green
#define PED_WALK_END_SEC    5  // Ends this many seconds before Yellow

#define SENSOR_CAR_THRESHOLD 5  // >= 5 cars = high demand
#define SENSOR_ADJUST_SEC    5  // add to busy phase, remove from other

#define HEARTBEAT_PERIOD_SEC 1
#define HEARTBEAT_MISS_LIMIT 3  // 3 misses = link down
#define RAILWAY_RECOVERY_SEC 2  // hold red after TRAIN_CLEAR

// ============================================
// Message routing
// ============================================
//
//   Message              Sender    Receiver   Struct
//   -------------------------------------------------------------
//   MSG_MODE_COMMAND     Central   Local      mode_cmd_msg_t
//   MSG_COORDINATION_COMMAND Central Local    coordination_command_msg_t
//   MSG_OVERRIDE_REQUEST Local     Central    override_request_msg_t
//   MSG_DISPLAY_UPDATE   Central   Local      display_update_msg_t
//
//   MSG_STATUS_UPDATE    Local     Central    status_msg_t
//   MSG_FAULT_ALERT      Local     Central    fault_msg_t
//   MSG_HEARTBEAT        Local     Central    heartbeat_msg_t
//
//   MSG_RAILWAY_PREEMPT  Train     Local      railway_msg_t
//   MSG_TRAIN_CLEAR      Train     Local      railway_msg_t
//   MSG_RAILWAY_STATUS   Train     Central    railway_status_msg_t
//   MSG_FAULT_ALERT      Train     Central    fault_msg_t
//   MSG_HEARTBEAT        Train     Central    heartbeat_msg_t
//
//   MSG_SENSOR_UPDATE    IO task   Local      sensor_msg_t   (internal)
//   MSG_PED_REQUEST      IO task   Local      ped_msg_t      (internal)
//   MSG_TEST             any       any        test_message_t (demo only)
//
// Central never sends to Train. Train never sends a lamp command.
// Local never sends to Local.

// Message types
typedef enum {
    MSG_TEST = 1,           // Any -> Any (test harness only)
    MSG_HEARTBEAT = 2,      // Local/Train -> Central
    MSG_MODE_COMMAND = 3,   // Central -> Local
    MSG_STATUS_UPDATE = 4,  // Local -> Central
    MSG_FAULT_ALERT = 5,    // Local/Train -> Central
    MSG_RAILWAY_PREEMPT = 6,// Train -> Local
    MSG_TRAIN_CLEAR = 7,    // Train -> Local
    MSG_RAILWAY_STATUS = 8, // Train -> Central
    MSG_SENSOR_UPDATE = 9,  // IO task -> Local (same node)
    MSG_PED_REQUEST = 10,   // IO task -> Local (same node)
    MSG_COORDINATION_COMMAND = 11, // Central -> Local
    MSG_OVERRIDE_REQUEST = 12,      // Local -> Central
    MSG_DISPLAY_UPDATE = 13        // Central -> Local display
} msg_type_t;

// Controller types
typedef enum {
    CONTROLLER_LOCAL = 1,
    CONTROLLER_TRAIN = 2,
    CONTROLLER_CENTRAL = 3
} controller_type_t;

// Message header
typedef struct {
    uint16_t type;          // msg_type_t
    uint16_t src;           // Source controller (controller_type_t)
    uint16_t dst;           // Destination controller (controller_type_t)
    uint16_t reserved;      // Padding
    char timestamp[32];     // HH:MM:SS format
} msg_header_t;

// Test message payload
typedef struct {
    msg_header_t header;
    char data[64];          // Optional message data
} test_message_t;

// Reply structure (returned by MsgReply for every message type)
typedef struct {
    int8_t status;          // 0 = success, -1 = error
    char timestamp[32];     // Reply timestamp
    uint16_t command_id;    // Echo of mode_cmd_msg_t.command_id, else 0
} reply_t;


// ============================================
// Central Controller Details
// ============================================
// Central only supervises: it receives STATUS_UPDATE / FAULT_ALERT /
// RAILWAY_STATUS / HEARTBEAT, and sends MODE_COMMAND.
// It never commands an individual lamp colour.

typedef enum {
    LINK_UP = 0,
    LINK_DOWN = 1           // 3 missed heartbeats
} link_state_t;

// Heartbeat payload (for MSG_HEARTBEAT) - used by all controllers
typedef struct {
    uint8_t sender_id;      // Intersection or crossing ID
    uint8_t healthy;        // 1 = OK, 0 = degraded
    uint16_t sequence;      // Increments each heartbeat
} heartbeat_msg_t;


// ============================================
// Train Controller Details
// ============================================

typedef enum { // railway crossing (where the boom gate is placed)
    P1,
    P2,
    P3
} train_intersection_id;

typedef enum { // Railway state reported by the train controller
    TRAIN_NONE = 0,         // No train, crossing clear
    TRAIN_APPROACHING = 1,  // Preemption required
    TRAIN_AT_CROSSING = 2,  // Train on the crossing
    TRAIN_CLEAR = 3         // Confirmed clear, recovery may start
} train_state_t;

typedef enum { // Boom gate position
    GATE_OPEN = 0,
    GATE_CLOSING = 1,
    GATE_CLOSED = 2,
    GATE_OPENING = 3,
    GATE_FAULT = 4
} gate_state_t;

// Railway status payload (for MSG_RAILWAY_STATUS, Railway -> Central)
typedef struct {
    uint8_t crossing_id;    // train_intersection_id
    uint8_t train_state;    // train_state_t
    uint8_t gate_state;     // gate_state_t
    uint8_t fault;          // fault_type_t (FAULT_NONE if OK)
} railway_status_msg_t;


// ============================================
// Traffic Light Status Details
// ============================================

typedef enum { // road intersection (vehicle + pedestrian signals, no boom gate)
    I1,
    I2,
    I3,
    I4,
    I5,
    I6
} local_intersection_id;

typedef enum {
    MODE_FIXED = 0,
    MODE_SENSOR = 1,
    MODE_RAILWAY = 2,
    MODE_FAILSAFE = 3
} traffic_light_mode;

typedef enum { // Light states
    LIGHT_OFF = 0,
    LIGHT_RED = 1,
    LIGHT_YELLOW = 2,
    LIGHT_GREEN = 3
} light_state_t;

typedef enum { // Local state machine phases
    PHASE_NS_GREEN = 0,
    PHASE_NS_YELLOW = 1,
    PHASE_EW_GREEN = 2,
    PHASE_EW_YELLOW = 3,
    PHASE_RAILWAY_HOLD = 4  // railway-feeding movement held red
} phase_t;

typedef enum { // Which approach a sensor / ped button belongs to
    DIR_NS = 0,
    DIR_EW = 1
} direction_t;

typedef enum { // Fault types
    FAULT_NONE = 0,
    FAULT_LIGHT = 1,               // Lamp / output failure
    FAULT_SENSOR = 2,              // Vehicle sensor not trustworthy
    FAULT_COMM = 3,                // Link to Central lost
    FAULT_GATE = 4,                // Boom gate fault
    FAULT_NOT_WORKING = 5          // Conflicting greens detected
} fault_type_t;

typedef enum { // Fault severity
    SEV_LOW = 1,
    SEV_MEDIUM = 2,
    SEV_CRITICAL = 3
} fault_severity_t;


typedef struct { // Intersection status payload (for MSG_STATUS_UPDATE)
    uint8_t intersection_id;    // Which intersection
    uint8_t mode;               // traffic_light_mode
    uint8_t phase;              // phase_t
    uint8_t ns_state;           // North-South light (light_state_t)
    uint8_t ew_state;           // East-West light (light_state_t)
    uint8_t pedestrian_ns;      // Pedestrian N-S crossing active
    uint8_t pedestrian_ew;      // Pedestrian E-W crossing active
    uint8_t railway_preempt;    // 1 = railway preemption active
    uint16_t time_remaining;    // Seconds until next change
} status_msg_t;

// Fault alert payload (for MSG_FAULT_ALERT)
typedef struct {
    uint8_t source_id;          // Local intersection or railway crossing
    uint8_t fault_type;         // fault_type_t
    uint8_t severity;           // 1=low, 2=medium, 3=critical
    uint8_t reserved;
    char description[32];       // Human-readable fault info
} fault_msg_t;

// Mode command action (for MSG_MODE_COMMAND)
typedef enum {
    CMD_SET_MODE  = 0,      // Change mode, stays until next command
    CMD_TEMPORARY = 1,      // Apply for duration_sec, then revert
    CMD_REVERT    = 2       // Cancel a temporary command now
} cmd_action_t;

#define CMD_PRIO_SCHEDULE 1 // Time-of-day schedule
#define CMD_PRIO_OPERATOR 2 // Operator override, overwrites a queued command

#define INTERSECTION_ALL 0xFF   // Broadcast to all locals

// Mode command payload (for MSG_MODE_COMMAND)
typedef struct {
    uint8_t  intersection_id;   // local_intersection_id, or INTERSECTION_ALL
    uint8_t  new_mode;          // Central may request MODE_FIXED or MODE_SENSOR
    uint8_t  action;            // cmd_action_t
    uint8_t  priority;          // CMD_PRIO_*
    uint16_t duration_sec;      // CMD_TEMPORARY only, 0 otherwise
    uint16_t command_id;        // Set by Central, echoed in reply_t for logs
} mode_cmd_msg_t;

// Railway preemption payload (for MSG_RAILWAY_PREEMPT)
// Also used for MSG_TRAIN_CLEAR with active = 0
typedef struct {
    uint8_t intersection_id;
    uint8_t active;             // 1 = train approaching, 0 = clear
    uint16_t eta_seconds;       // Time until train arrives
} railway_msg_t;

// Vehicle sensor payload (for MSG_SENSOR_UPDATE, local input task -> lc_core)
typedef struct {
    uint8_t intersection_id;
    uint8_t direction;          // direction_t
    uint8_t car_count;          // Detected vehicles on that approach
    uint8_t reserved;
} sensor_msg_t;

// Pedestrian button payload (for MSG_PED_REQUEST)
typedef struct {
    uint8_t intersection_id;
    uint8_t direction;          // direction_t (which crossing)
    uint8_t pressed;            // 1 = request
    uint8_t reserved;
} ped_msg_t;

// Coordination command payload (for MSG_COORDINATION_COMMAND)
typedef struct {
    uint8_t intersection_id;    // local_intersection_id, or INTERSECTION_ALL
    uint8_t mode;               // traffic_light_mode
    uint8_t phase;              // Requested coordination phase
    uint8_t reserved;
    uint16_t cycle_offset_sec;
    uint16_t command_id;
} coordination_command_msg_t;

// Override request payload (for MSG_OVERRIDE_REQUEST)
typedef struct {
    uint8_t source_id;          // Intersection or crossing requesting override
    uint8_t requested_mode;     // traffic_light_mode
    uint8_t reason;             // fault_type_t or implementation-defined reason
    uint8_t reserved;
    uint16_t duration_sec;
    uint16_t request_id;
} override_request_msg_t;

// Display update payload (for MSG_DISPLAY_UPDATE)
typedef struct {
    uint8_t intersection_id;
    uint8_t mode;               // traffic_light_mode
    uint8_t phase;              // phase_t
    uint8_t ns_state;           // light_state_t
    uint8_t ew_state;           // light_state_t
    uint8_t pedestrian_ns;
    uint8_t pedestrian_ew;
    uint8_t railway_preempt;
    uint16_t time_remaining;
} display_update_msg_t;

// ============================================
// Receive buffer
// ============================================
// Use this as the MsgReceive() buffer, then switch on msg.header.type

typedef union {
    msg_header_t        header;
    test_message_t      test;
    heartbeat_msg_t     heartbeat;
    status_msg_t        status;
    fault_msg_t         fault;
    mode_cmd_msg_t      mode_cmd;
    railway_msg_t       railway;
    railway_status_msg_t railway_status;
    sensor_msg_t        sensor;
    ped_msg_t           ped;
    coordination_command_msg_t coordination;
    override_request_msg_t override_request;
    display_update_msg_t display_update;
} any_msg_t;

#endif // PROTOCOL_H