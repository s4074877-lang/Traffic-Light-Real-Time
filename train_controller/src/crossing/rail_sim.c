#include "rail_sim.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

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

    bool initialized;

    // Track layout in use (compile-time config after validation)
    int distance_sec[2];    // [0] P1-P2, [1] P2-P3 travel, front to front
    int train_length_sec;   // Time the train occupies a crossing
    int warning_sec;        // PREEMPT lead time before the train arrives
    char config_notice[256]; // Refused config values, empty if none
} sim;

// ============================================
// Simulator Lock
// ============================================
// Serializes the tick, console and IPC threads. Recursive because crossing
// callbacks re-enter the simulator (gate commands and timers).
static pthread_mutex_t sim_mutex;
static pthread_once_t sim_mutex_once = PTHREAD_ONCE_INIT;

static void create_sim_mutex(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&sim_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void lock_sim(void)
{
    pthread_once(&sim_mutex_once, create_sim_mutex);
    pthread_mutex_lock(&sim_mutex);
}

static void unlock_sim(void)
{
    pthread_mutex_unlock(&sim_mutex);
}

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
// Internal: Track Layout Config
// ============================================
// Use a configured value when it is in range; otherwise refuse it, note why
// and fall back to the default
static int config_value(const char *name, int value, int min, int max, int fallback)
{
    if (value >= min && value <= max)
    {
        return value;
    }

    size_t used = strlen(sim.config_notice);
    snprintf(sim.config_notice + used, sizeof(sim.config_notice) - used,
             "%s%s=%d refused (allowed %d-%d), using default %d",
             used ? "; " : "", name, value, min, max, fallback);
    return fallback;
}

static void apply_config_locked(void)
{
    sim.config_notice[0] = '\0';

    sim.distance_sec[0] = DISTANCE_UNIT_SEC *
        config_value("DISTANCE_P1_P2_UNITS", DISTANCE_P1_P2_UNITS,
                     DISTANCE_MIN_UNITS, DISTANCE_MAX_UNITS, DEFAULT_DISTANCE_UNITS);
    sim.distance_sec[1] = DISTANCE_UNIT_SEC *
        config_value("DISTANCE_P2_P3_UNITS", DISTANCE_P2_P3_UNITS,
                     DISTANCE_MIN_UNITS, DISTANCE_MAX_UNITS, DEFAULT_DISTANCE_UNITS);
    // Length must stay below the train timeout or every train would fault
    sim.train_length_sec = config_value("TRAIN_LENGTH_SEC", TRAIN_LENGTH_SEC,
                                        1, TRAIN_TIMEOUT_SEC - 1, DEFAULT_TRAIN_LENGTH_SEC);
    // Warning must leave time for the gate to close before the train arrives
    sim.warning_sec = config_value("TRAIN_WARNING_SEC", TRAIN_WARNING_SEC,
                                   TRAIN_WARNING_MIN_SEC, TRAIN_WARNING_MAX_SEC,
                                   DEFAULT_TRAIN_WARNING_SEC);
}

// ============================================
// Initialize Simulator
// ============================================
void rail_sim_init(crossing_t *crossings, int num_crossings, int time_scale)
{
    lock_sim();

    memset(&sim, 0, sizeof(sim));

    sim.crossings = crossings;
    sim.num_crossings = num_crossings;
    sim.time_scale = time_scale > 0 ? time_scale : 1;
    sim.tick_count = 0;
    sim.sim_time_ms = 0;
    sim.initialized = true;
    apply_config_locked();

    for (int i = 0; i < 3; i++)
    {
        sim.gates[i].move_complete_ms = -1;
    }

    unlock_sim();
}

// ============================================
// Destroy Simulator
// ============================================
void rail_sim_destroy(void)
{
    lock_sim();
    sim.initialized = false;
    unlock_sim();
}

// ============================================
// Tick
// ============================================
static int tick_locked(void)
{
    if (!sim.initialized)
        return 0;

    // Update simulation time based on tick count and time scale
    // Each tick is SIM_TICK_MS real milliseconds
    // With time_scale, simulation runs faster
    // Accumulate per tick so changing the scale doesn't rescale elapsed time
    sim.tick_count++;
    sim.sim_time_ms += (int64_t)SIM_TICK_MS * sim.time_scale;

    int processed = 0;

    // Check gate completions
    for (int i = 0; i < sim.num_crossings && i < 3; i++)
    {
        if (sim.gates[i].move_complete_ms >= 0 &&
            sim.sim_time_ms >= sim.gates[i].move_complete_ms)
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

    return processed;
}

int rail_sim_tick(void)
{
    lock_sim();
    int processed = tick_locked();
    unlock_sim();
    return processed;
}

// ============================================
// Gate Command (from crossing logic)
// ============================================
void rail_sim_gate_command(crossing_t *cx, gate_command_t cmd)
{
    (void)cmd;
    lock_sim();

    if (sim.initialized && cx->id >= 1 && cx->id <= 3)
    {
        // Schedule gate movement completion
        sim.gates[cx->id - 1].move_complete_ms = sim.sim_time_ms + sec_to_sim_ms(GATE_MOVE_SEC);
    }

    unlock_sim();
}

// ============================================
// Timer Control
// ============================================
void rail_sim_start_timer(crossing_t *cx, timer_id_t timer_id, int seconds)
{
    lock_sim();

    if (sim.initialized)
    {
        uint32_t gen = cx->timer_gen[timer_id];
        // Use queue_event_ex to include timer_id in the event
        // This prevents stale timers from being misinterpreted as different timer types
        queue_event_ex(cx, CX_EVENT_TIMER, CX_TRACK_UP, gen, timer_id, seconds);
    }

    unlock_sim();
}

void rail_sim_cancel_timer(crossing_t *cx, timer_id_t timer_id)
{
    lock_sim();

    // Timers are cancelled by incrementing generation in crossing.c
    // But we also deactivate the queued event so its slot is freed
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

    unlock_sim();
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
// Internal: Train Track Occupancy
// ============================================
// Queued trains are tracked per crossing and track, so trains on the UP and
// DOWN tracks can run at the same time. Only two trains on the same track at
// the same crossing are prevented from overlapping.

// Worst-case timer events the crossings may queue while trains run
#define TIMER_EVENT_RESERVE (NUM_CROSSINGS * NUM_TIMERS)

static bool is_train_event(crossing_event_t event)
{
    return event == CX_EVENT_TRAIN_APPROACH ||
           event == CX_EVENT_TRAIN_ENTER ||
           event == CX_EVENT_TRAIN_EXIT;
}

// True if a queued train event targets this crossing's track
static bool train_pending(const crossing_t *cx, cx_track_direction_t dir)
{
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        const sim_event_t *ev = &sim.events[i];
        if (ev->active && is_train_event(ev->event) && ev->cx == cx && ev->dir == dir)
        {
            return true;
        }
    }
    return false;
}

// True if a train is still running anywhere on this track
static bool track_pending(cx_track_direction_t dir)
{
    for (int i = 0; i < sim.num_crossings; i++)
    {
        if (train_pending(&sim.crossings[i], dir))
        {
            return true;
        }
    }
    return false;
}

static int free_event_slots(void)
{
    int free_slots = 0;
    for (int i = 0; i < MAX_PENDING_EVENTS; i++)
    {
        if (!sim.events[i].active)
        {
            free_slots++;
        }
    }
    return free_slots;
}

static const char *dir_name(cx_track_direction_t dir)
{
    return dir == CX_TRACK_UP ? "UP" : "DOWN";
}

// Check a new train can start on this track at one crossing (or every
// crossing when cx is NULL) and that its events plus timers fit in the queue
static bool can_start_train(const crossing_t *cx, cx_track_direction_t dir, int events,
                            char *reply, size_t reply_len)
{
    for (int i = 0; i < sim.num_crossings; i++)
    {
        const crossing_t *target = &sim.crossings[i];
        if ((cx == NULL || cx == target) && train_pending(target, dir))
        {
            snprintf(reply, reply_len, "BUSY: Train already running on %s %s track",
                     target->name, dir_name(dir));
            return false;
        }
    }

    if (free_event_slots() < events + TIMER_EVENT_RESERVE)
    {
        snprintf(reply, reply_len, "BUSY: Simulation event queue full");
        return false;
    }

    return true;
}

// ============================================
// Internal: Simulate train along the whole line
// ============================================
// UP runs W->E through P3->P2->P1, DOWN runs E->W through P1->P2->P3.
// Each crossing is warned warning_sec before the front arrives, entered on
// arrival and cleared train_length_sec later, so a long train can still be on
// one crossing when the next crossing is warned.
static void simulate_train_on_line(cx_track_direction_t dir)
{
    int arrive = sim.warning_sec; // Seconds until the front reaches the first crossing

    for (int step = 0; step < sim.num_crossings; step++)
    {
        int i = dir == CX_TRACK_UP ? sim.num_crossings - 1 - step : step;
        crossing_t *cx = &sim.crossings[i];

        if (step > 0)
        {
            // Travel from the previous crossing: index 0/1 is P1-P2, 1/2 is P2-P3
            int previous = dir == CX_TRACK_UP ? i + 1 : i - 1;
            arrive += sim.distance_sec[i < previous ? i : previous];
        }

        queue_event(cx, CX_EVENT_TRAIN_APPROACH, dir, 0, arrive - sim.warning_sec);
        queue_event(cx, CX_EVENT_TRAIN_ENTER, dir, 0, arrive);
        queue_event(cx, CX_EVENT_TRAIN_EXIT, dir, 0, arrive + sim.train_length_sec);
    }
}

// ============================================
// Internal: Simulate train sequence for one crossing
// ============================================
static void simulate_train_at_crossing(crossing_t *cx, cx_track_direction_t dir, bool no_exit)
{
    // Warn now, enter when the front arrives, clear after the train's length
    queue_event(cx, CX_EVENT_TRAIN_APPROACH, dir, 0, 0);
    queue_event(cx, CX_EVENT_TRAIN_ENTER, dir, 0, sim.warning_sec);

    if (!no_exit)
    {
        queue_event(cx, CX_EVENT_TRAIN_EXIT, dir, 0, sim.warning_sec + sim.train_length_sec);
    }
}

// ============================================
// Command Handler
// ============================================
static bool command_locked(const char *cmd, char *reply, size_t reply_len)
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

    // ===== train-both: start UP and DOWN trains at the same time =====
    if (strcmp(cmd, "train-both") == 0)
    {
        // Check both tracks and room for both runs before queuing either
        int events = 6 * sim.num_crossings;
        if (!can_start_train(NULL, CX_TRACK_UP, events, reply, reply_len) ||
            !can_start_train(NULL, CX_TRACK_DOWN, events, reply, reply_len))
            return false;

        simulate_train_on_line(CX_TRACK_UP);
        simulate_train_on_line(CX_TRACK_DOWN);
        snprintf(reply, reply_len, "OK: Train UP (P3->P2->P1) and DOWN (P1->P2->P3) started");
        return true;
    }

    // ===== train-up: simulate train W->E through P3->P2->P1 =====
    // Runs alongside a train on the DOWN track
    if (strcmp(cmd, "train-up") == 0)
    {
        if (!can_start_train(NULL, CX_TRACK_UP, 3 * sim.num_crossings, reply, reply_len))
            return false;

        simulate_train_on_line(CX_TRACK_UP);
        snprintf(reply, reply_len, "OK: Train UP (W->E) P3->P2->P1 started");
        return true;
    }

    // ===== train-down: simulate train E->W through P1->P2->P3 =====
    // Runs alongside a train on the UP track
    if (strcmp(cmd, "train-down") == 0)
    {
        if (!can_start_train(NULL, CX_TRACK_DOWN, 3 * sim.num_crossings, reply, reply_len))
            return false;

        simulate_train_on_line(CX_TRACK_DOWN);
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

            if (!can_start_train(cx, dir, 3, reply, reply_len))
                return false;

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

            if (!can_start_train(cx, dir, 2, reply, reply_len))
                return false;

            simulate_train_at_crossing(cx, dir, true);
            snprintf(reply, reply_len, "OK: No-exit train at %s %s started",
                     cx->name, dir == CX_TRACK_UP ? "UP" : "DOWN");
            return true;
        }
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

    // ===== reset P#: reset fault =====
    if (strncmp(cmd, "reset ", 6) == 0)
    {
        int id = parse_crossing_id(cmd + 6);
        if (id >= 1 && id <= 3)
        {
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
                 "Sim: %s, Scale: x%d, Elapsed: %ds, Train UP: %s, Train DOWN: %s\n"
                 "Layout: P1-P2 %ds, P2-P3 %ds, train length %ds, warning %ds%s",
                 sim.initialized ? "OK" : "NOT INIT",
                 sim.time_scale,
                 rail_sim_get_elapsed_sec(),
                 track_pending(CX_TRACK_UP) ? "RUNNING" : "IDLE",
                 track_pending(CX_TRACK_DOWN) ? "RUNNING" : "IDLE",
                 sim.distance_sec[0], sim.distance_sec[1],
                 sim.train_length_sec, sim.warning_sec,
                 sim.config_notice[0] ? " (refused config values replaced by defaults)" : "");
        return true;
    }

    // ===== help: show available commands =====
    if (strcmp(cmd, "help") == 0)
    {
        snprintf(reply, reply_len,
                 "Commands:\n"
                 "  train-up       - Train W->E through P3->P2->P1\n"
                 "  train-down     - Train E->W through P1->P2->P3\n"
                 "  train-both     - Start train-up and train-down together\n"
                 "                   (UP and DOWN trains can run at the same time)\n"
                 "  train P# dir   - Single train at crossing (dir=up/down)\n"
                 "  noexit P# dir  - Train that never exits\n"
                 "  p#-fault       - Inject fault (e.g., p1-fault)\n"
                 "  reset P#       - Reset fault\n"
                 "  scale N        - Set time scale (1-100)\n"
                 "  status         - Show simulator status\n"
                 "  help           - Show this help");
        return true;
    }

    snprintf(reply, reply_len, "ERROR: Unknown command '%s'. Type 'help' for commands.", cmd);
    return false;
}

bool rail_sim_command(const char *cmd, char *reply, size_t reply_len)
{
    lock_sim();
    bool result = command_locked(cmd, reply, reply_len);
    unlock_sim();
    return result;
}

// ============================================
// Time Scale
// ============================================
int rail_sim_get_time_scale(void)
{
    lock_sim();
    int scale = sim.time_scale;
    unlock_sim();
    return scale;
}

void rail_sim_set_time_scale(int scale)
{
    if (scale >= 1 && scale <= 100)
    {
        lock_sim();
        sim.time_scale = scale;
        unlock_sim();
    }
}

// ============================================
// Status
// ============================================
bool rail_sim_is_busy(void)
{
    lock_sim();
    bool busy = track_pending(CX_TRACK_UP) || track_pending(CX_TRACK_DOWN);
    unlock_sim();
    return busy;
}

int rail_sim_get_elapsed_sec(void)
{
    lock_sim();
    int elapsed = (int)(sim.sim_time_ms / 1000);
    unlock_sim();
    return elapsed;
}

int rail_sim_snapshot(crossing_t *out, int max)
{
    int count = 0;

    lock_sim();
    if (sim.initialized)
    {
        count = sim.num_crossings < max ? sim.num_crossings : max;
        memcpy(out, sim.crossings, (size_t)count * sizeof(*out));
    }
    unlock_sim();

    return count;
}

// ============================================
// Track Layout
// ============================================
int rail_sim_get_warning_sec(void)
{
    lock_sim();
    int warning = sim.warning_sec;
    unlock_sim();
    return warning;
}

const char *rail_sim_config_notice(void)
{
    // Written only by rail_sim_init, before other threads start
    return sim.config_notice;
}
