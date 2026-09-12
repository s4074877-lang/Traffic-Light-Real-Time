#include "rail_sim.h"
#include "crossing_test.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

// ============================================
// Event Queue Entry
// ============================================
typedef struct
{
    int64_t deliver_at_ms;    // When to deliver (in sim milliseconds)
    crossing_t *cx;           // Target crossing (NULL for global events)
    crossing_event_t event;   // Event type
    cx_track_direction_t dir; // Direction (for track events)
    uint32_t timer_gen;       // Timer generation (for TIMER events)
    timer_id_t timer_id;      // Timer ID (for TIMER events) - to prevent misinterpretation
    bool active;              // Is this slot in use?
} sim_event_t;

// ============================================
// Gate State (simulated gate mechanics)
// ============================================
typedef struct
{
    bool stuck;               // Gate is stuck (won't complete movement)
    int64_t move_complete_ms; // When current movement completes
} sim_gate_t;

// ============================================
// Simulator State
// ============================================
static struct
{
    crossing_t *crossings;
    int num_crossings;
    int time_scale;

    sim_event_t events[MAX_PENDING_EVENTS];
    sim_gate_t gates[3]; // Gate state for P1, P2, P3

    int64_t sim_time_ms; // Current simulation time (tick-based)
    uint32_t tick_count; // Number of ticks since start

    bool busy; // Command in progress
    bool initialized;
} sim;

// Gate movement duration (scaled simulation seconds to ms)
#define GATE_MOVE_DURATION_SEC 3

// ============================================
// Internal: Convert real seconds to sim ms
// ============================================
static int64_t sec_to_sim_ms(int seconds)
{
    // With time_scale, 1 real second = time_scale sim seconds
    // So sim_ms = seconds * 1000 / time_scale for real-time mapping
    // But we want the simulation to run faster, so:
    // sim_ms = seconds * 1000 (simulation time runs at normal speed internally)
    return (int64_t)seconds * 1000;
}

// ============================================
// Internal: Find crossing by ID
// ============================================
static crossing_t *find_crossing(int id)
{
    for (int i = 0; i < sim.num_crossings; i++)
    {
        if (sim.crossings[i].id == id)
        {
            return &sim.crossings[i];
        }
    }
    return NULL;
}

// ============================================
// Internal: Queue an event
// ============================================
static bool queue_event_ex(crossing_t *cx, crossing_event_t event,
                           cx_track_direction_t dir, uint32_t timer_gen,
                           timer_id_t timer_id, int delay_sec)
{
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        if (!sim.events[i].active)
        {
            sim.events[i].deliver_at_ms = sim.sim_time_ms + sec_to_sim_ms(delay_sec);
            sim.events[i].cx = cx;
            sim.events[i].event = event;
            sim.events[i].dir = dir;
            sim.events[i].timer_gen = timer_gen;
            sim.events[i].timer_id = timer_id;
            sim.events[i].active = true;
            return true;
        }
    }
    return false; // Queue full
}

// Wrapper for non-timer events (backwards compatible)
static bool queue_event(crossing_t *cx, crossing_event_t event,
                        cx_track_direction_t dir, uint32_t timer_gen, int delay_sec)
{
    return queue_event_ex(cx, event, dir, timer_gen, (timer_id_t)0, delay_sec);
}

// ============================================
// Initialize Simulator
// ============================================
void rail_sim_init(crossing_t *crossings, int num_crossings, int time_scale)
{
    memset(&sim, 0, sizeof(sim));

    sim.crossings = crossings;
    sim.num_crossings = num_crossings;
    sim.time_scale = time_scale > 0 ? time_scale : 1;
    sim.tick_count = 0;
    sim.sim_time_ms = 0;
    sim.initialized = true;

    for (int i = 0; i < 3; i++)
    {
        sim.gates[i].stuck = false;
        sim.gates[i].move_complete_ms = -1;
    }
}

// ============================================
// Destroy Simulator
// ============================================
void rail_sim_destroy(void)
{
    sim.initialized = false;
}

// ============================================
// Tick
// ============================================
int rail_sim_tick(void)
{
    if (!sim.initialized)
        return 0;

    // Update simulation time based on tick count and time scale
    // Each tick is SIM_TICK_MS real milliseconds
    // With time_scale, simulation runs faster
    sim.tick_count++;
    sim.sim_time_ms = (int64_t)sim.tick_count * SIM_TICK_MS * sim.time_scale;

    int processed = 0;

    // Check gate completions
    for (int i = 0; i < sim.num_crossings && i < 3; i++)
    {
        if (sim.gates[i].move_complete_ms >= 0 &&
            sim.sim_time_ms >= sim.gates[i].move_complete_ms &&
            !sim.gates[i].stuck)
        {

            crossing_t *cx = &sim.crossings[i];
            sim.gates[i].move_complete_ms = -1;

            if (cx->gate == GATE_CLOSING)
            {
                crossing_handle_event(cx, CX_EVENT_GATE_CLOSED, CX_TRACK_UP, 0);
                processed++;
            }
            else if (cx->gate == GATE_OPENING)
            {
                crossing_handle_event(cx, CX_EVENT_GATE_OPENED, CX_TRACK_UP, 0);
                processed++;
            }
        }
    }

    // Process pending events
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        if (sim.events[i].active && sim.sim_time_ms >= sim.events[i].deliver_at_ms)
        {
            sim_event_t *ev = &sim.events[i];

            if (ev->cx)
            {
                // For timer events, pass timer_id in the dir parameter
                // crossing_handle_event will extract it for CX_EVENT_TIMER
                cx_track_direction_t dir_or_timer_id = ev->dir;
                if (ev->event == CX_EVENT_TIMER)
                {
                    dir_or_timer_id = (cx_track_direction_t)ev->timer_id;
                }
                crossing_handle_event(ev->cx, ev->event, dir_or_timer_id, ev->timer_gen);
            }

            ev->active = false;
            processed++;
        }
    }

    // Check if any command sequence completed
    bool any_active = false;
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        if (sim.events[i].active)
        {
            any_active = true;
            break;
        }
    }
    if (!any_active)
    {
        sim.busy = false;
    }

    return processed;
}

// ============================================
// Gate Command (from crossing logic)
// ============================================
void rail_sim_gate_command(crossing_t *cx, gate_command_t cmd)
{
    if (!sim.initialized || cx->id < 1 || cx->id > 3)
        return;

    int idx = cx->id - 1;

    if (sim.gates[idx].stuck)
    {
        // Gate is stuck, won't complete movement
        return;
    }

    // Schedule gate movement completion
    sim.gates[idx].move_complete_ms = sim.sim_time_ms + sec_to_sim_ms(GATE_MOVE_DURATION_SEC);
}

// ============================================
// Timer Control
// ============================================
void rail_sim_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds)
{
    if (!sim.initialized)
        return;

    uint32_t gen = cx->timer_gen[timer_id];
    // Use queue_event_ex to include timer_id in the event
    // This prevents stale timers from being misinterpreted as different timer types
    queue_event_ex(cx, CX_EVENT_TIMER, CX_TRACK_UP, gen, timer_id, seconds);
}

void rail_sim_cancel_timer(crossing_t *cx, timer_id_t timer_id)
{
    // Timers are cancelled by incrementing generation in crossing.c
    // But we also need to deactivate the queued event to clear the busy flag
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        if (sim.events[i].active &&
            sim.events[i].cx == cx &&
            sim.events[i].event == CX_EVENT_TIMER &&
            sim.events[i].timer_id == timer_id)
        {
            // Deactivate this stale timer event
            sim.events[i].active = false;
        }
    }
}

// ============================================
// Internal: Parse crossing ID from string
// ============================================
static int parse_crossing_id(const char *s)
{
    // Accept "P1", "P2", "P3" or "1", "2", "3"
    if (s[0] == 'P' || s[0] == 'p')
        s++;
    if (s[0] >= '1' && s[0] <= '3')
    {
        return s[0] - '0';
    }
    return -1;
}

// ============================================
// Internal: Simulate train sequence for one crossing
// ============================================
static void simulate_train_at_crossing(crossing_t *cx, cx_track_direction_t dir, bool no_exit)
{
    // APPROACH -> ENTER (after crossing time) -> EXIT (after crossing time)
    queue_event(cx, CX_EVENT_TRAIN_APPROACH, dir, 0, 0);

    // Train enters after gate close delay + gate close time + small margin
    int enter_delay = GATE_CLOSE_DELAY_SEC + GATE_MOVE_DURATION_SEC + 2;
    queue_event(cx, CX_EVENT_TRAIN_ENTER, dir, 0, enter_delay);

    if (!no_exit)
    {
        // Train exits after spending time on crossing
        int exit_delay = enter_delay + TRAIN_CROSSING_TIME_SEC;
        queue_event(cx, CX_EVENT_TRAIN_EXIT, dir, 0, exit_delay);
    }
}

// ============================================
// Command Handler
// ============================================
bool rail_sim_command(const char *cmd, char *reply, size_t reply_len)
{
    if (!sim.initialized)
    {
        snprintf(reply, reply_len, "ERROR: Simulator not initialized");
        return false;
    }

    // Skip whitespace
    while (*cmd && isspace(*cmd))
        cmd++;

    // Check for empty command
    if (!*cmd)
    {
        snprintf(reply, reply_len, "ERROR: Empty command");
        return false;
    }

    // Check if busy
    if (sim.busy && strncmp(cmd, "test", 4) != 0 && strcmp(cmd, "help") != 0)
    {
        snprintf(reply, reply_len, "BUSY: Command in progress");
        return false;
    }

    // ===== train-up: simulate train W->E through P3->P2->P1 =====
    if (strcmp(cmd, "train-up") == 0)
    {
        sim.busy = true;

        for (int i = sim.num_crossings - 1; i >= 0; i--)
        {
            int delay = (sim.num_crossings - 1 - i) * TRAIN_TRAVEL_TIME_SEC;
            crossing_t *cx = &sim.crossings[i];

            // Queue approach
            queue_event(cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_UP, 0, delay);

            // Queue enter
            int enter_delay = delay + GATE_CLOSE_DELAY_SEC + GATE_MOVE_DURATION_SEC + 2;
            queue_event(cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_UP, 0, enter_delay);

            // Queue exit
            int exit_delay = enter_delay + TRAIN_CROSSING_TIME_SEC;
            queue_event(cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_UP, 0, exit_delay);
        }

        snprintf(reply, reply_len, "OK: Train UP (W->E) P3->P2->P1 started");
        return true;
    }

    // ===== train-down: simulate train E->W through P1->P2->P3 =====
    if (strcmp(cmd, "train-down") == 0)
    {
        sim.busy = true;

        for (int i = 0; i < sim.num_crossings; i++)
        {
            int delay = i * TRAIN_TRAVEL_TIME_SEC;
            crossing_t *cx = &sim.crossings[i];

            queue_event(cx, CX_EVENT_TRAIN_APPROACH, CX_TRACK_DOWN, 0, delay);

            int enter_delay = delay + GATE_CLOSE_DELAY_SEC + GATE_MOVE_DURATION_SEC + 2;
            queue_event(cx, CX_EVENT_TRAIN_ENTER, CX_TRACK_DOWN, 0, enter_delay);

            int exit_delay = enter_delay + TRAIN_CROSSING_TIME_SEC;
            queue_event(cx, CX_EVENT_TRAIN_EXIT, CX_TRACK_DOWN, 0, exit_delay);
        }

        snprintf(reply, reply_len, "OK: Train DOWN (E->W) P1->P2->P3 started");
        return true;
    }

    // ===== train P# dir: single crossing train =====
    if (strncmp(cmd, "train ", 6) == 0)
    {
        char arg1[16], arg2[16];
        if (sscanf(cmd + 6, "%15s %15s", arg1, arg2) == 2)
        {
            int id = parse_crossing_id(arg1);
            crossing_t *cx = find_crossing(id);
            if (!cx)
            {
                snprintf(reply, reply_len, "ERROR: Invalid crossing '%s'", arg1);
                return false;
            }

            cx_track_direction_t dir = CX_TRACK_UP;
            if (strcmp(arg2, "down") == 0 || strcmp(arg2, "DOWN") == 0)
            {
                dir = CX_TRACK_DOWN;
            }

            sim.busy = true;
            simulate_train_at_crossing(cx, dir, false);
            snprintf(reply, reply_len, "OK: Train at %s %s started",
                     cx->name, dir == CX_TRACK_UP ? "UP" : "DOWN");
            return true;
        }
    }

    // ===== noexit P# dir: train that never exits =====
    if (strncmp(cmd, "noexit ", 7) == 0)
    {
        char arg1[16], arg2[16];
        if (sscanf(cmd + 7, "%15s %15s", arg1, arg2) == 2)
        {
            int id = parse_crossing_id(arg1);
            crossing_t *cx = find_crossing(id);
            if (!cx)
            {
                snprintf(reply, reply_len, "ERROR: Invalid crossing '%s'", arg1);
                return false;
            }

            cx_track_direction_t dir = CX_TRACK_UP;
            if (strcmp(arg2, "down") == 0 || strcmp(arg2, "DOWN") == 0)
            {
                dir = CX_TRACK_DOWN;
            }

            sim.busy = true;
            simulate_train_at_crossing(cx, dir, true);
            snprintf(reply, reply_len, "OK: No-exit train at %s %s started",
                     cx->name, dir == CX_TRACK_UP ? "UP" : "DOWN");
            return true;
        }
    }

    // ===== stuck P#: gate gets stuck =====
    if (strncmp(cmd, "stuck ", 6) == 0)
    {
        int id = parse_crossing_id(cmd + 6);
        if (id >= 1 && id <= 3)
        {
            sim.gates[id - 1].stuck = true;
            snprintf(reply, reply_len, "OK: Gate %d stuck", id);
            return true;
        }
        snprintf(reply, reply_len, "ERROR: Invalid crossing");
        return false;
    }

    // ===== p#-fault: inject fault =====
    if ((cmd[0] == 'p' || cmd[0] == 'P') &&
        cmd[1] >= '1' && cmd[1] <= '3' &&
        strcmp(cmd + 2, "-fault") == 0)
    {

        int id = cmd[1] - '0';
        crossing_t *cx = find_crossing(id);
        if (cx)
        {
            crossing_handle_event(cx, CX_EVENT_INJECT_FAULT, CX_TRACK_UP, 0);
            snprintf(reply, reply_len, "OK: Fault injected at P%d", id);
            return true;
        }
        snprintf(reply, reply_len, "ERROR: Crossing not found");
        return false;
    }

    // ===== fault P#: inject fault (alternate syntax) =====
    if (strncmp(cmd, "fault ", 6) == 0)
    {
        int id = parse_crossing_id(cmd + 6);
        crossing_t *cx = find_crossing(id);
        if (cx)
        {
            crossing_handle_event(cx, CX_EVENT_INJECT_FAULT, CX_TRACK_UP, 0);
            snprintf(reply, reply_len, "OK: Fault injected at P%d", id);
            return true;
        }
        snprintf(reply, reply_len, "ERROR: Invalid crossing");
        return false;
    }

    // ===== reset P#: reset fault and unstick gate =====
    if (strncmp(cmd, "reset ", 6) == 0)
    {
        int id = parse_crossing_id(cmd + 6);
        if (id >= 1 && id <= 3)
        {
            sim.gates[id - 1].stuck = false;
            crossing_t *cx = find_crossing(id);
            if (cx)
            {
                crossing_handle_event(cx, CX_EVENT_RESET_FAULT, CX_TRACK_UP, 0);
                snprintf(reply, reply_len, "OK: Reset P%d", id);
                return true;
            }
        }
        snprintf(reply, reply_len, "ERROR: Invalid crossing");
        return false;
    }

    // ===== test: run all requirement tests =====
    if (strcmp(cmd, "test") == 0)
    {
        return crossing_test_run_all(reply, reply_len);
    }

    // ===== test N: run single test =====
    if (strncmp(cmd, "test ", 5) == 0)
    {
        int n = atoi(cmd + 5);
        if (n >= 1 && n <= 7)
        {
            return crossing_test_run_single(n, reply, reply_len);
        }
        snprintf(reply, reply_len, "ERROR: Test number must be 1-7");
        return false;
    }

    // ===== scale N: set time scale =====
    if (strncmp(cmd, "scale ", 6) == 0)
    {
        int n = atoi(cmd + 6);
        if (n >= 1 && n <= 100)
        {
            rail_sim_set_time_scale(n);
            snprintf(reply, reply_len, "OK: Time scale set to x%d", n);
            return true;
        }
        snprintf(reply, reply_len, "ERROR: Scale must be 1-100");
        return false;
    }

    // ===== status: show simulator status =====
    if (strcmp(cmd, "status") == 0)
    {
        snprintf(reply, reply_len,
                 "Sim: %s, Scale: x%d, Elapsed: %ds, Busy: %s",
                 sim.initialized ? "OK" : "NOT INIT",
                 sim.time_scale,
                 rail_sim_get_elapsed_sec(),
                 sim.busy ? "YES" : "NO");
        return true;
    }

    // ===== help: show available commands =====
    if (strcmp(cmd, "help") == 0)
    {
        snprintf(reply, reply_len,
                 "Commands:\n"
                 "  train-up       - Train W->E through P3->P2->P1\n"
                 "  train-down     - Train E->W through P1->P2->P3\n"
                 "  train P# dir   - Single train at crossing (dir=up/down)\n"
                 "  noexit P# dir  - Train that never exits\n"
                 "  stuck P#       - Make gate stuck\n"
                 "  p#-fault       - Inject fault (e.g., p1-fault)\n"
                 "  reset P#       - Reset fault and unstick gate\n"
                 "  test           - Run all requirement tests\n"
                 "  test N         - Run single test (1-7)\n"
                 "  scale N        - Set time scale (1-100)\n"
                 "  status         - Show simulator status\n"
                 "  help           - Show this help");
        return true;
    }

    snprintf(reply, reply_len, "ERROR: Unknown command '%s'. Type 'help' for commands.", cmd);
    return false;
}

// ============================================
// Time Scale
// ============================================
int rail_sim_get_time_scale(void)
{
    return sim.time_scale;
}

void rail_sim_set_time_scale(int scale)
{
    if (scale >= 1 && scale <= 100)
    {
        sim.time_scale = scale;
    }
}

// ============================================
// Status
// ============================================
bool rail_sim_is_busy(void)
{
    return sim.busy;
}

int rail_sim_get_elapsed_sec(void)
{
    return (int)(sim.sim_time_ms / 1000);
}
