#ifndef CROSSING_CONFIG_H
#define CROSSING_CONFIG_H

// ============================================
// Crossing Module Configuration
// ============================================
// All timing values are in seconds (simulation time)
// Modify these before compile to adjust behavior

// Gate timing
#define GATE_CLOSE_DELAY_SEC    5       // Delay after TRAIN_APPROACH before gate starts closing
#define GATE_MOVE_SEC           3       // Time for the gate to finish closing or opening
#define GATE_CLOSE_TIMEOUT_SEC  10      // Max time for gate to close (fault if exceeded)
#define GATE_OPEN_TIMEOUT_SEC   10      // Max time for gate to open (fault if exceeded)

// Track layout: distance between neighbouring crossings, in units 1..5.
// One unit is DISTANCE_UNIT_SEC of travel, measured from the front of the
// train reaching one crossing to the front reaching the next.
#define DISTANCE_UNIT_SEC       5
#define DISTANCE_MIN_UNITS      1
#define DISTANCE_MAX_UNITS      5
#define DEFAULT_DISTANCE_UNITS  2       // 10 s

#ifndef DISTANCE_P1_P2_UNITS
#define DISTANCE_P1_P2_UNITS    3
#endif
#ifndef DISTANCE_P2_P3_UNITS
#define DISTANCE_P2_P3_UNITS    3
#endif

// Train length: seconds from the front entering a crossing until the tail
// clears it. A train longer than the distance to the next crossing is still
// on this crossing when the next crossing receives its warning.
#define TRAIN_TIMEOUT_SEC       60      // Max time train can be on crossing (fault if exceeded)
#define DEFAULT_TRAIN_LENGTH_SEC 5

#ifndef TRAIN_LENGTH_SEC
#define TRAIN_LENGTH_SEC        7
#endif

// Warning: seconds before the front of the train reaches a crossing that
// RAILWAY_PREEMPT ("train is coming") is sent. It must leave time for the
// close delay, gate movement and a margin; a value outside the allowed range
// is refused at startup and the default is used instead.
#define TRAIN_WARNING_MARGIN_SEC 2
#define TRAIN_WARNING_MIN_SEC   (GATE_CLOSE_DELAY_SEC + GATE_MOVE_SEC + TRAIN_WARNING_MARGIN_SEC)
#define TRAIN_WARNING_MAX_SEC   120
#define DEFAULT_TRAIN_WARNING_SEC 10

#ifndef TRAIN_WARNING_SEC
#define TRAIN_WARNING_SEC       DEFAULT_TRAIN_WARNING_SEC
#endif

// Status reporting
#define STATUS_REPORT_INTERVAL_SEC  1   // How often to send status to Central

// Simulation
#define DEFAULT_TIME_SCALE      10      // Default simulation speed multiplier
#define SIM_TICK_MS             100     // Simulator tick interval in milliseconds

// Event queue
#define MAX_PENDING_EVENTS      64      // Maximum queued simulation events (UP + DOWN trains and timers)

#endif // CROSSING_CONFIG_H
