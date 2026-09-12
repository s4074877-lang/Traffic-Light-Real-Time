#ifndef TRAIN_UI_H
#define TRAIN_UI_H

#include <pthread.h>
#include <stdint.h>
#include "../../../common/protocol.h"

// UI Screen/View mode
typedef enum {
    UI_SCREEN_MENU = 0,     // Main menu
    UI_SCREEN_STATUS,       // Status display
    UI_SCREEN_COMMAND       // Command input mode
} ui_screen_t;

// Track direction for crossing
typedef enum {
    TRACK_UP = 0,   // W->S direction
    TRACK_DOWN = 1  // S->W direction
} track_direction_t;

// Track state for a single direction
typedef enum {
    TRACK_NONE = 0,
    TRACK_APPROACHING,
    TRACK_ON_CROSSING,
    TRACK_CLEARED
} track_state_t;

// Road flash state
typedef enum {
    FLASH_OFF = 0,
    FLASH_ON
} flash_state_t;

// Train frequency mode
typedef enum {
    FREQ_PEAK = 0,      // 2 min intervals
    FREQ_NIGHT          // 20 min intervals
} train_frequency_t;

// Train signal state
typedef enum {
    SIGNAL_PROCEED = 0,
    SIGNAL_STOP
} train_signal_t;

// Connection status
typedef enum {
    CONN_CONNECTED = 0,
    CONN_LOST
} connection_status_t;

// Fault type for crossing
typedef enum {
    CROSSING_FAULT_NONE = 0,
    CROSSING_FAULT_GATE_CLOSE_TIMEOUT,
    CROSSING_FAULT_GATE_OPEN_TIMEOUT,
    CROSSING_FAULT_SENSOR_FAILURE,
    CROSSING_FAULT_COMM_LOSS
} crossing_fault_t;

// Single crossing state
typedef struct {
    track_state_t track_up;         // W->S track state
    track_state_t track_down;       // S->W track state
    gate_state_t boom_gate;         // Gate state from protocol.h
    flash_state_t road_flash;       // Road flash lights
    crossing_fault_t fault;         // Current fault if any
    char preempt_time[16];          // HH:MM:SS when preempt sent
    char clear_time[16];            // HH:MM:SS when clear sent
} crossing_state_t;

// Message info for display
typedef struct {
    char timestamp[16];             // HH:MM:SS
    char type[32];                  // Message type name
    char endpoint[16];              // Source or destination
} message_info_t;

// Complete UI state
typedef struct {
    // Connection status
    connection_status_t central_status;
    connection_status_t local_status;
    char central_last_update[16];   // Last state change HH:MM:SS
    char local_last_update[16];     // Last state change HH:MM:SS

    // Train configuration
    train_frequency_t frequency;
    int time_scale;                 // e.g., x10
    train_signal_t signal;

    // Crossing states (P1, P2, P3)
    crossing_state_t crossings[NUM_CROSSINGS];

    // Message tracking
    message_info_t last_sent;
    message_info_t last_received;
    int active_faults;

    // Current screen
    ui_screen_t current_screen;

    // Update flag
    int needs_update;

    // Thread safety
    pthread_mutex_t mutex;
} train_ui_state_t;

// Initialize UI state
void train_ui_init(train_ui_state_t *ui);

// Destroy UI state (cleanup mutex)
void train_ui_destroy(train_ui_state_t *ui);

// Display the current screen (menu, status, or command)
void train_ui_display(train_ui_state_t *ui);

// Display main menu
void train_ui_display_menu(train_ui_state_t *ui);

// Display status screen
void train_ui_display_status(train_ui_state_t *ui);

// Display command input screen
void train_ui_display_command(train_ui_state_t *ui);

// Set current screen (thread-safe)
void train_ui_set_screen(train_ui_state_t *ui, ui_screen_t screen);

// Get current screen (thread-safe)
ui_screen_t train_ui_get_screen(train_ui_state_t *ui);

// Mark UI as needing update (thread-safe)
void train_ui_request_update(train_ui_state_t *ui);

// Check if UI needs update (thread-safe)
int train_ui_needs_update(train_ui_state_t *ui);

// Clear the update flag after displaying (thread-safe)
void train_ui_clear_update_flag(train_ui_state_t *ui);

// Update connection status (thread-safe, only updates timestamp on state change)
void train_ui_set_connection(train_ui_state_t *ui, controller_type_t controller,
                             connection_status_t status, const char *last_update);

// Update crossing state (thread-safe)
void train_ui_set_crossing(train_ui_state_t *ui, int crossing_id,
                           const crossing_state_t *crossing);

// Update track state for a crossing (thread-safe)
void train_ui_set_track_state(train_ui_state_t *ui, int crossing_id,
                              track_direction_t direction, track_state_t state);

// Update boom gate state (thread-safe)
void train_ui_set_gate_state(train_ui_state_t *ui, int crossing_id,
                             gate_state_t state);

// Update crossing fault (thread-safe)
void train_ui_set_crossing_fault(train_ui_state_t *ui, int crossing_id,
                                 crossing_fault_t fault);

// Set preempt sent time (thread-safe)
void train_ui_set_preempt_time(train_ui_state_t *ui, int crossing_id,
                               const char *timestamp);

// Set clear sent time (thread-safe)
void train_ui_set_clear_time(train_ui_state_t *ui, int crossing_id,
                             const char *timestamp);

// Update train frequency mode (thread-safe)
void train_ui_set_frequency(train_ui_state_t *ui, train_frequency_t freq);

// Update train signal (thread-safe)
void train_ui_set_signal(train_ui_state_t *ui, train_signal_t signal);

// Update time scale (thread-safe)
void train_ui_set_time_scale(train_ui_state_t *ui, int scale);

// Update last sent message info (thread-safe)
void train_ui_set_last_sent(train_ui_state_t *ui, const char *timestamp,
                            const char *type, const char *dest);

// Update last received message info (thread-safe)
void train_ui_set_last_received(train_ui_state_t *ui, const char *timestamp,
                                const char *type, const char *src);

// Update active fault count (thread-safe)
void train_ui_set_active_faults(train_ui_state_t *ui, int count);

// UI refresh thread function (to be used with pthread_create)
void* train_ui_refresh_thread(void *arg);

#endif // TRAIN_UI_H
