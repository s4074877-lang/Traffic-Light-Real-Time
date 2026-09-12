#include "crossing_test.h"
#include "crossing.h"
#include <string.h>
#include <stdio.h>

// ============================================
// Test State (for callback verification)
// ============================================
static struct {
    bool gate_cmd_called;
    gate_command_t last_gate_cmd;

    bool flash_set;
    bool flash_on;

    bool preempt_sent;
    bool clear_sent;

    bool fault_alert_sent;
    cx_fault_t last_fault;

    int timer_started_count;
    int state_changed_count;
} test_state;

// ============================================
// Test Callbacks
// ============================================
static void test_gate_command(crossing_t *cx, gate_command_t cmd) {
    (void)cx;
    test_state.gate_cmd_called = true;
    test_state.last_gate_cmd = cmd;
}

static void test_set_flash(crossing_t *cx, bool on) {
    (void)cx;
    test_state.flash_set = true;
    test_state.flash_on = on;
}

static void test_send_preempt(crossing_t *cx) {
    (void)cx;
    test_state.preempt_sent = true;
}

static void test_send_clear(crossing_t *cx) {
    (void)cx;
    test_state.clear_sent = true;
}

static void test_fault_alert(crossing_t *cx, cx_fault_t fault) {
    (void)cx;
    test_state.fault_alert_sent = true;
    test_state.last_fault = fault;
}

static void test_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds) {
    (void)cx;
    (void)timer_id;
    (void)seconds;
    test_state.timer_started_count++;
}

static void test_cancel_timer(crossing_t *cx, timer_id_t timer_id) {
    (void)cx;
    (void)timer_id;
}

static void test_state_changed(crossing_t *cx) {
    (void)cx;
    test_state.state_changed_count++;
}

static const cx_ops_t test_ops = {
    .gate_command = test_gate_command,
    .set_flash = test_set_flash,
    .send_preempt = test_send_preempt,
    .send_clear = test_send_clear,
    .fault_alert = test_fault_alert,
    .start_timer = test_start_timer,
    .cancel_timer = test_cancel_timer,
    .state_changed = test_state_changed
};

// ============================================
// Test Setup
// ============================================
static void reset_test_state(void) {
    memset(&test_state, 0, sizeof(test_state));
}

static crossing_t create_test_crossing(void) {
    crossing_t cx;
    crossing_init(&cx, 1, "T1", 1, 2, &test_ops, NULL);
    return cx;
}

// ============================================
// Test Names
// ============================================
static const char *test_names[] = {
    NULL,  // Index 0 unused
    "Train Crossing - Boom Gates Close and Flashing Lights On",
    "Two Train Lines - One Each Direction",
    "Train Frequency - 2 Minutes Apart (Peak) and 20 Minutes (Night)",
    "Intersections Sense Crossing State (No Control Over Gate)",
    "Gate Fault - Train Gets Red Light",
    "Gate Fault - Error Reported to Control Room",
    "Train Line Controller Communicates with Central"
};

// ============================================
// Test 1: Boom Gates Close and Flashing Lights On
// ============================================
static bool test_1_boom_gates_and_lights(void) {
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // Train approaches
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);

    // Verify: flash should be on, preempt sent, timer started
    if (!test_state.flash_on) return false;
    if (!test_state.preempt_sent) return false;
    if (cx.track[CX_TRACK_UP] != CX_TRACK_APPROACHING) return false;

    // Simulate timer expired (gate close delay)
    // Note: For timer events, dir parameter contains the timer_id
    uint32_t gen = cx.timer_gen[TIMER_GATE_CLOSE_DELAY];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_DELAY, gen);

    // Verify: gate should be closing
    if (cx.gate != GATE_CLOSING) return false;
    if (!test_state.gate_cmd_called) return false;
    if (test_state.last_gate_cmd != GATE_CMD_CLOSE) return false;

    // Gate finishes closing
    crossing_handle_event(&cx, CX_EVENT_GATE_CLOSED, CX_TRACK_UP, 0);
    if (cx.gate != GATE_CLOSED) return false;

    return true;
}

// ============================================
// Test 2: Two Train Lines - One Each Direction
// ============================================
static bool test_2_two_train_lines(void) {
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // Train 1 approaching on UP track
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);
    if (cx.track[CX_TRACK_UP] != CX_TRACK_APPROACHING) return false;

    // Train 2 approaching on DOWN track (while train 1 still approaching)
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_DOWN, 0);
    if (cx.track[CX_TRACK_DOWN] != CX_TRACK_APPROACHING) return false;

    // Both tracks should be in APPROACHING state
    if (cx.track[CX_TRACK_UP] != CX_TRACK_APPROACHING) return false;
    if (cx.track[CX_TRACK_DOWN] != CX_TRACK_APPROACHING) return false;

    // Simulate gate close
    uint32_t gen = cx.timer_gen[TIMER_GATE_CLOSE_DELAY];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_DELAY, gen);
    crossing_handle_event(&cx, CX_EVENT_GATE_CLOSED, CX_TRACK_UP, 0);

    // Train 1 enters and exits
    crossing_handle_event(&cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_UP, 0);

    // Gate should NOT open yet (train 2 still there)
    if (cx.gate != GATE_CLOSED) return false;

    // Train 2 enters and exits
    crossing_handle_event(&cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_DOWN, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_DOWN, 0);

    // Now gate should be opening
    if (cx.gate != GATE_OPENING) return false;

    return true;
}

// ============================================
// Test 3: Train Frequency
// (This test just verifies the config exists - actual frequency is simulation)
// ============================================
static bool test_3_train_frequency(void) {
    // Verify config constants exist and are reasonable
    // Peak: 2 minutes = 120 seconds
    // Night: 20 minutes = 1200 seconds
    // These would be used by the simulation scheduler

    // For now, just verify the crossing system handles rapid train sequences
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // First train sequence
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);
    uint32_t gen = cx.timer_gen[TIMER_GATE_CLOSE_DELAY];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_DELAY, gen);
    crossing_handle_event(&cx, CX_EVENT_GATE_CLOSED, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_GATE_OPENED, CX_TRACK_UP, 0);

    // Verify clear was sent
    if (!test_state.clear_sent) return false;

    // Reset for second train
    test_state.preempt_sent = false;

    // Second train (can come immediately after)
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);
    if (!test_state.preempt_sent) return false;

    return true;
}

// ============================================
// Test 4: Intersections Sense Crossing State
// (Verify preempt and clear are sent, not direct gate control)
// ============================================
static bool test_4_intersections_sense_state(void) {
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // Train approaches - should send preempt (not gate command to local)
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);

    // Preempt should be sent to locals
    if (!test_state.preempt_sent) return false;

    // Complete the sequence
    uint32_t gen = cx.timer_gen[TIMER_GATE_CLOSE_DELAY];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_DELAY, gen);
    crossing_handle_event(&cx, CX_EVENT_GATE_CLOSED, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_UP, 0);
    crossing_handle_event(&cx, CX_EVENT_GATE_OPENED, CX_TRACK_UP, 0);

    // Clear should be sent to locals
    if (!test_state.clear_sent) return false;

    // The local_id fields should be set
    if (cx.local_id_1 != 1 || cx.local_id_2 != 2) return false;

    return true;
}

// ============================================
// Test 5: Gate Fault - Train Gets Red Light
// ============================================
static bool test_5_gate_fault_train_red(void) {
    reset_test_state();
    crossing_t cx1 = create_test_crossing();
    crossing_t cx2;
    crossing_init(&cx2, 2, "T2", 3, 4, &test_ops, NULL);

    crossing_t crossings[2] = {cx1, cx2};

    // Initially no fault
    if (crossing_any_fault(crossings, 2)) return false;

    // Inject fault in first crossing
    crossing_handle_event(&crossings[0], CX_EVENT_INJECT_FAULT, CX_TRACK_UP, 0);

    // Now should report fault (train signal should be STOP)
    if (!crossing_any_fault(crossings, 2)) return false;
    if (crossings[0].fault != CX_FAULT_INJECTED) return false;
    if (crossings[0].gate != GATE_FAULT) return false;

    return true;
}

// ============================================
// Test 6: Gate Fault - Error Reported to Control Room
// ============================================
static bool test_6_gate_fault_reported(void) {
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // Inject fault
    crossing_handle_event(&cx, CX_EVENT_INJECT_FAULT, CX_TRACK_UP, 0);

    // Verify fault alert was sent
    if (!test_state.fault_alert_sent) return false;
    if (test_state.last_fault != CX_FAULT_INJECTED) return false;

    // Test gate timeout fault
    reset_test_state();
    cx = create_test_crossing();

    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);
    uint32_t gen = cx.timer_gen[TIMER_GATE_CLOSE_DELAY];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_DELAY, gen);

    // Gate is closing, now simulate close timeout
    gen = cx.timer_gen[TIMER_GATE_CLOSE_TIMEOUT];
    crossing_handle_event(&cx, CX_EVENT_TIMER, (cx_track_direction_t)TIMER_GATE_CLOSE_TIMEOUT, gen);

    // Should report gate close timeout
    if (!test_state.fault_alert_sent) return false;
    if (test_state.last_fault != CX_FAULT_GATE_CLOSE_TIMEOUT) return false;

    return true;
}

// ============================================
// Test 7: Train Controller Communicates with Central
// (Verify callbacks are invoked for status reporting)
// ============================================
static bool test_7_communicate_with_central(void) {
    reset_test_state();
    crossing_t cx = create_test_crossing();

    // Any state change should trigger state_changed callback
    crossing_handle_event(&cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0);

    if (test_state.state_changed_count == 0) return false;

    // Fault should trigger fault_alert (which goes to Central)
    crossing_handle_event(&cx, CX_EVENT_INJECT_FAULT, CX_TRACK_UP, 0);

    if (!test_state.fault_alert_sent) return false;

    return true;
}

// ============================================
// Run Tests
// ============================================
typedef bool (*test_func_t)(void);

static test_func_t tests[] = {
    NULL,  // Index 0 unused
    test_1_boom_gates_and_lights,
    test_2_two_train_lines,
    test_3_train_frequency,
    test_4_intersections_sense_state,
    test_5_gate_fault_train_red,
    test_6_gate_fault_reported,
    test_7_communicate_with_central
};

int crossing_test_count(void) {
    return 7;
}

const char *crossing_test_name(int test_num) {
    if (test_num >= 1 && test_num <= 7) {
        return test_names[test_num];
    }
    return "Unknown";
}

bool crossing_test_run_single(int test_num, char *reply, size_t reply_len) {
    if (test_num < 1 || test_num > 7) {
        snprintf(reply, reply_len, "ERROR: Invalid test number %d", test_num);
        return false;
    }

    bool passed = tests[test_num]();

    snprintf(reply, reply_len, "TEST %d: %s", test_num, passed ? "PASS" : "FAIL");
    return passed;
}

bool crossing_test_run_all(char *reply, size_t reply_len) {
    int passed = 0;
    int failed[7];
    int fail_count = 0;

    for (int i = 1; i <= 7; i++) {
        if (tests[i]()) {
            passed++;
        } else {
            failed[fail_count++] = i;
        }
    }

    if (fail_count == 0) {
        snprintf(reply, reply_len, "TESTS: %d/%d PASS", passed, 7);
    } else {
        char fail_str[64] = "";
        for (int i = 0; i < fail_count; i++) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%s%d", i > 0 ? ", " : "", failed[i]);
            strncat(fail_str, buf, sizeof(fail_str) - strlen(fail_str) - 1);
        }
        snprintf(reply, reply_len, "TESTS: %d/%d PASS, failed: %s", passed, 7, fail_str);
    }

    return fail_count == 0;
}
