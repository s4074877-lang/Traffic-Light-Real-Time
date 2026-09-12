#ifndef CROSSING_TEST_H
#define CROSSING_TEST_H

#include <stdbool.h>
#include <stddef.h>

// ============================================
// Requirement Tests
// ============================================
// Tests run on private crossing instances (not live P1-P3)
// so they don't affect the running system.
//
// Test List:
// 1. Train Crossing - Boom Gates Close and Flashing Lights On
// 2. Two Train Lines - One Each Direction
// 3. Train Frequency - 2 Minutes Apart (Peak) and 20 Minutes (Night)
// 4. Intersections Sense Crossing State (No Control Over Gate)
// 5. Gate Fault - Train Gets Red Light
// 6. Gate Fault - Error Reported to Control Room
// 7. Train Line Controller Communicates with Central

// Run all tests
// reply: buffer for result message
// reply_len: size of reply buffer
// Returns: true if all tests passed
bool crossing_test_run_all(char *reply, size_t reply_len);

// Run a single test
// test_num: test number (1-7)
// reply: buffer for result message
// reply_len: size of reply buffer
// Returns: true if test passed
bool crossing_test_run_single(int test_num, char *reply, size_t reply_len);

// Get total number of tests
int crossing_test_count(void);

// Get test name by number
const char *crossing_test_name(int test_num);

#endif // CROSSING_TEST_H
