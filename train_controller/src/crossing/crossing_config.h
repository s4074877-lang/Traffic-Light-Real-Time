#ifndef CROSSING_CONFIG_H
#define CROSSING_CONFIG_H

// ============================================
// Crossing Module Configuration
// ============================================
// All timing values are in seconds (simulation time)
// Modify these before compile to adjust behavior

// Gate timing
#define GATE_CLOSE_DELAY_SEC    5       // Delay after TRAIN_APPROACH before gate starts closing
#define GATE_CLOSE_TIMEOUT_SEC  10      // Max time for gate to close (fault if exceeded)
#define GATE_OPEN_TIMEOUT_SEC   10      // Max time for gate to open (fault if exceeded)

// Train timing
#define TRAIN_TRAVEL_TIME_SEC   10      // Time for train to travel between crossings (P1->P2->P3)
#define TRAIN_CROSSING_TIME_SEC 5       // Time train spends on crossing before exit
#define TRAIN_TIMEOUT_SEC       60      // Max time train can be on crossing (fault if exceeded)

// Status reporting
#define STATUS_REPORT_INTERVAL_SEC  1   // How often to send status to Central

// Simulation
#define DEFAULT_TIME_SCALE      10      // Default simulation speed multiplier
#define SIM_TICK_MS             100     // Simulator tick interval in milliseconds

// Event queue
#define MAX_PENDING_EVENTS      32      // Maximum queued simulation events

// Test timing (faster for automated tests)
#define TEST_GATE_CLOSE_DELAY_SEC   1
#define TEST_TRAIN_TRAVEL_TIME_SEC  2
#define TEST_TRAIN_CROSSING_TIME_SEC 1

#endif // CROSSING_CONFIG_H
