#ifndef RAIL_SIM_H
#define RAIL_SIM_H

#include "crossing.h"
#include <stdbool.h>
#include <stddef.h>

// ============================================
// Rail Simulator
// ============================================
// Simulates train movement and gate mechanics.
// Provides test commands for manual and automated testing.
// All timing is scaled by the time_scale factor.

// ============================================
// Initialization
// ============================================

// Initialize the simulator with array of crossings
// time_scale: simulation speed multiplier (e.g., 10 = 10x faster)
void rail_sim_init(crossing_t *crossings, int num_crossings, int time_scale);

// Cleanup simulator resources
void rail_sim_destroy(void);

// ============================================
// Tick (call every SIM_TICK_MS)
// ============================================

// Process pending events and deliver due events to crossings
// Returns: number of events processed
int rail_sim_tick(void);

// ============================================
// Gate Control (called by crossing logic)
// ============================================

// Command the gate (called from cx_ops_t.gate_command callback)
void rail_sim_gate_command(crossing_t *cx, gate_command_t cmd);

// ============================================
// Timer Control (called by crossing logic)
// ============================================

// Start a timer (called from cx_ops_t.start_timer callback)
void rail_sim_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds);

// Cancel a timer (called from cx_ops_t.cancel_timer callback)
void rail_sim_cancel_timer(crossing_t *cx, timer_id_t timer_id);

// ============================================
// Command Interface
// ============================================

// Process a command string (from console or Central)
// cmd: command string (e.g., "train-up", "p1-fault", "test")
// reply: buffer for reply message
// reply_len: size of reply buffer
// Returns: true if command was recognized
bool rail_sim_command(const char *cmd, char *reply, size_t reply_len);

// ============================================
// Time Scale Control
// ============================================

// Get current time scale
int rail_sim_get_time_scale(void);

// Set time scale (1 = real-time, 10 = 10x faster)
void rail_sim_set_time_scale(int scale);

// ============================================
// Status
// ============================================

// Check if a simulation command is currently running
bool rail_sim_is_busy(void);

// Get simulation elapsed time in seconds (scaled)
int rail_sim_get_elapsed_sec(void);

#endif // RAIL_SIM_H
