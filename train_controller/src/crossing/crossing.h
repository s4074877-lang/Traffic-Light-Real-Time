#ifndef CROSSING_H
#define CROSSING_H

#include <stdint.h>
#include <stdbool.h>
#include "crossing_config.h"
#include "../../../common/protocol.h"

// ============================================
// Track Direction
// ============================================
// Use CX_ prefix to avoid conflicts with train_ui.h
typedef enum
{
    CX_TRACK_UP = 0,  // W → E (P3 → P2 → P1)
    CX_TRACK_DOWN = 1 // E → W (P1 → P2 → P3)
} cx_track_direction_t;

#define NUM_TRACKS 2

// ============================================
// Track State (per direction)
// ============================================
typedef enum
{
    CX_TRACK_NONE = 0,        // No train detected
    CX_TRACK_APPROACHING = 1, // Train approaching, preemption sent
    CX_TRACK_ON_CROSSING = 2, // Train on the crossing
    CX_TRACK_CLEARED = 3      // Train exited, waiting for gate to open
} cx_track_state_t;

// Use gate_state_t from protocol.h (GATE_OPEN, GATE_CLOSING, etc.)

// ============================================
// Crossing Fault Types
// ============================================
typedef enum
{
    CX_FAULT_NONE = 0,
    CX_FAULT_GATE_CLOSE_TIMEOUT = 1, // Gate took too long to close
    CX_FAULT_GATE_OPEN_TIMEOUT = 2,  // Gate took too long to open
    CX_FAULT_GATE_UNEXPECTED = 3,    // Gate moved unexpectedly
    CX_FAULT_TRAIN_TIMEOUT = 4,      // Train never exited
    CX_FAULT_TRAIN_EARLY = 5,        // Train entered before gate closed
    CX_FAULT_INJECTED = 6            // Test-injected fault
} cx_fault_t;

// ============================================
// Events (input to crossing logic)
// ============================================
typedef enum
{
    CX_EVENT_TRAIN_APPROACH = 1, // Train detected approaching
    CX_EVENT_TRAIN_ENTER = 2,    // Train entered crossing
    CX_EVENT_TRAIN_EXIT = 3,     // Train exited crossing
    CX_EVENT_GATE_CLOSED = 4,    // Gate finished closing
    CX_EVENT_GATE_OPENED = 5,    // Gate finished opening
    CX_EVENT_TIMER = 6,          // Timer expired
    CX_EVENT_INJECT_FAULT = 7,   // Inject fault (test)
    CX_EVENT_RESET_FAULT = 8     // Reset fault (from Central)
} crossing_event_t;

// ============================================
// Gate Commands (output)
// ============================================
typedef enum
{
    GATE_CMD_CLOSE = 0,
    GATE_CMD_OPEN = 1
} gate_command_t;

// ============================================
// Timer IDs
// ============================================
typedef enum
{
    TIMER_GATE_CLOSE_DELAY = 0,   // Delay before starting gate close
    TIMER_GATE_CLOSE_TIMEOUT = 1, // Timeout waiting for gate to close
    TIMER_GATE_OPEN_TIMEOUT = 2,  // Timeout waiting for gate to open
    TIMER_TRAIN_TIMEOUT = 3       // Timeout waiting for train to exit
} timer_id_t;

#define NUM_TIMERS 4

// Forward declaration
struct crossing_t;

// ============================================
// Callback Operations
// ============================================
typedef struct
{
    // Command the gate to open or close
    void (*gate_command)(struct crossing_t *cx, gate_command_t cmd);

    // Turn flashing lights on or off
    void (*set_flash)(struct crossing_t *cx, bool on);

    // Send RAILWAY_PREEMPT to Local Controllers
    void (*send_preempt)(struct crossing_t *cx);

    // Send TRAIN_CLEAR to Local Controllers
    void (*send_clear)(struct crossing_t *cx);

    // Send FAULT_ALERT to Central
    void (*fault_alert)(struct crossing_t *cx, cx_fault_t fault);

    // Start a timer (seconds)
    void (*start_timer)(struct crossing_t *cx, timer_id_t timer_id, int seconds);

    // Cancel a timer
    void (*cancel_timer)(struct crossing_t *cx, timer_id_t timer_id);

    // Notify state changed (for UI update)
    void (*state_changed)(struct crossing_t *cx);
} cx_ops_t;

// ============================================
// Crossing State
// ============================================
typedef struct crossing_t
{
    // Identity
    uint8_t id;         // Crossing ID (1, 2, 3 for P1, P2, P3)
    char name[4];       // "P1", "P2", "P3"
    uint8_t local_id_1; // First affected intersection (I1, I3, I5)
    uint8_t local_id_2; // Second affected intersection (I2, I4, I6)

    // State
    cx_track_state_t track[NUM_TRACKS]; // State per direction
    gate_state_t gate;                  // Current gate state
    bool flash_on;                      // Road flashing lights
    cx_fault_t fault;                   // Current fault (CX_FAULT_NONE if OK)
    bool reset_pending;                 // True if gate should open after close (reset sequence)

    // Timer generation counters (to ignore stale timer events)
    uint32_t timer_gen[NUM_TIMERS];

    // Preempt/Clear timestamps (for UI)
    char preempt_time[16]; // Last preempt sent time "HH:MM:SS"
    char clear_time[16];   // Last clear sent time "HH:MM:SS"

    // Callbacks
    const cx_ops_t *ops;

    // User data (for callbacks to access controller state)
    void *user_data;
} crossing_t;

// ============================================
// API Functions
// ============================================

// Initialize a crossing
void crossing_init(crossing_t *cx, uint8_t id, const char *name,
                   uint8_t local_id_1, uint8_t local_id_2,
                   const cx_ops_t *ops, void *user_data);

// Handle an event
void crossing_handle_event(crossing_t *cx, crossing_event_t event,
                           cx_track_direction_t dir, uint32_t timer_gen);

// Check if both tracks are clear
bool crossing_both_tracks_clear(const crossing_t *cx);

// Check if any crossing in array has a fault (for train signal)
bool crossing_any_fault(const crossing_t *crossings, int count);

// Get string representation of states (for display/debug)
const char *cx_track_state_str(cx_track_state_t state);
const char *gate_state_str(gate_state_t state);
const char *cx_fault_str(cx_fault_t fault);

#endif // CROSSING_H
