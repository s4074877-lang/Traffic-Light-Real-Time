#include "crossing.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// ============================================
// Helper: Get current timestamp string
// ============================================
static void get_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%H:%M:%S", tm_info);
}

// ============================================
// Initialize Crossing
// ============================================
void crossing_init(crossing_t *cx, uint8_t id, const char *name,
                   uint8_t local_id_1, uint8_t local_id_2,
                   const cx_ops_t *ops, void *user_data)
{
    memset(cx, 0, sizeof(*cx));

    cx->id = id;
    strncpy(cx->name, name, sizeof(cx->name) - 1);
    cx->local_id_1 = local_id_1;
    cx->local_id_2 = local_id_2;

    cx->track[CX_TRACK_UP] = CX_TRACK_NONE;
    cx->track[CX_TRACK_DOWN] = CX_TRACK_NONE;
    cx->gate = GATE_OPEN;
    cx->flash_on = false;
    cx->fault = CX_FAULT_NONE;
    cx->reset_pending = false;

    for (int i = 0; i < NUM_TIMERS; i++)
    {
        cx->timer_gen[i] = 0;
    }

    cx->preempt_time[0] = '\0';
    cx->clear_time[0] = '\0';

    cx->ops = ops;
    cx->user_data = user_data;
}

// ============================================
// Check if both tracks are clear
// ============================================
bool crossing_both_tracks_clear(const crossing_t *cx)
{
    return (cx->track[CX_TRACK_UP] == CX_TRACK_NONE || cx->track[CX_TRACK_UP] == CX_TRACK_CLEARED) &&
           (cx->track[CX_TRACK_DOWN] == CX_TRACK_NONE || cx->track[CX_TRACK_DOWN] == CX_TRACK_CLEARED);
}

// ============================================
// Check if any crossing has a fault
// ============================================
bool crossing_any_fault(const crossing_t *crossings, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (crossings[i].fault != CX_FAULT_NONE)
        {
            return true;
        }
    }
    return false;
}

// ============================================
// Internal: Enter fault state
// ============================================
static void enter_fault(crossing_t *cx, cx_fault_t fault)
{
    cx->fault = fault;
    cx->gate = GATE_FAULT;

    // Keep flash on
    if (!cx->flash_on)
    {
        cx->flash_on = true;
        if (cx->ops->set_flash)
        {
            cx->ops->set_flash(cx, true);
        }
    }

    // Cancel all timers
    for (int i = 0; i < NUM_TIMERS; i++)
    {
        cx->timer_gen[i]++;
        if (cx->ops->cancel_timer)
        {
            cx->ops->cancel_timer(cx, (timer_id_t)i);
        }
    }

    // Send fault alert
    if (cx->ops->fault_alert)
    {
        cx->ops->fault_alert(cx, fault);
    }

    // Notify state changed
    if (cx->ops->state_changed)
    {
        cx->ops->state_changed(cx);
    }
}

// ============================================
// Internal: Start gate close sequence
// ============================================
static void start_gate_close(crossing_t *cx)
{
    if (cx->gate == GATE_OPEN || cx->gate == GATE_OPENING)
    {
        cx->gate = GATE_CLOSING;

        if (cx->ops->gate_command)
        {
            cx->ops->gate_command(cx, GATE_CMD_CLOSE);
        }

        // Start close timeout timer
        cx->timer_gen[TIMER_GATE_CLOSE_TIMEOUT]++;
        if (cx->ops->start_timer)
        {
            cx->ops->start_timer(cx, TIMER_GATE_CLOSE_TIMEOUT, GATE_CLOSE_TIMEOUT_SEC);
        }

        if (cx->ops->state_changed)
        {
            cx->ops->state_changed(cx);
        }
    }
}

// ============================================
// Internal: Start gate open sequence
// ============================================
static void start_gate_open(crossing_t *cx)
{
    if (cx->gate == GATE_CLOSED || cx->gate == GATE_CLOSING)
    {
        cx->gate = GATE_OPENING;

        // Cancel any pending close delay timer to prevent interference
        cx->timer_gen[TIMER_GATE_CLOSE_DELAY]++;
        cx->timer_gen[TIMER_GATE_CLOSE_TIMEOUT]++;
        if (cx->ops->cancel_timer)
        {
            cx->ops->cancel_timer(cx, TIMER_GATE_CLOSE_DELAY);
            cx->ops->cancel_timer(cx, TIMER_GATE_CLOSE_TIMEOUT);
        }

        if (cx->ops->gate_command)
        {
            cx->ops->gate_command(cx, GATE_CMD_OPEN);
        }

        // Start open timeout timer
        cx->timer_gen[TIMER_GATE_OPEN_TIMEOUT]++;
        if (cx->ops->start_timer)
        {
            cx->ops->start_timer(cx, TIMER_GATE_OPEN_TIMEOUT, GATE_OPEN_TIMEOUT_SEC);
        }

        if (cx->ops->state_changed)
        {
            cx->ops->state_changed(cx);
        }
    }
}

// ============================================
// Handle Event
// ============================================
void crossing_handle_event(crossing_t *cx, crossing_event_t event,
                           cx_track_direction_t dir, uint32_t timer_gen)
{
    // If in fault state, only RESET_FAULT is accepted
    if (cx->fault != CX_FAULT_NONE && event != CX_EVENT_RESET_FAULT)
    {
        return;
    }

    switch (event)
    {
    case CX_EVENT_TRAIN_APPROACH:
    {
        // Train approaching on specified track
        if (cx->track[dir] == CX_TRACK_NONE)
        {
            cx->track[dir] = CX_TRACK_APPROACHING;

            // Turn on flashing lights
            if (!cx->flash_on)
            {
                cx->flash_on = true;
                if (cx->ops->set_flash)
                {
                    cx->ops->set_flash(cx, true);
                }
            }

            // Send preempt to local controllers
            if (cx->ops->send_preempt)
            {
                cx->ops->send_preempt(cx);
            }
            get_timestamp(cx->preempt_time, sizeof(cx->preempt_time));

            // Start gate close delay timer (if gate not already closing/closed)
            if (cx->gate == GATE_OPEN)
            {
                cx->timer_gen[TIMER_GATE_CLOSE_DELAY]++;
                if (cx->ops->start_timer)
                {
                    cx->ops->start_timer(cx, TIMER_GATE_CLOSE_DELAY, GATE_CLOSE_DELAY_SEC);
                }
            }

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }
        }
        break;
    }

    case CX_EVENT_TRAIN_ENTER:
    {
        // Train entered crossing
        if (cx->track[dir] == CX_TRACK_APPROACHING)
        {
            // Check if gate is closed - if not, it's a fault
            if (cx->gate != GATE_CLOSED)
            {
                enter_fault(cx, CX_FAULT_TRAIN_EARLY);
                return;
            }

            cx->track[dir] = CX_TRACK_ON_CROSSING;

            // Start train timeout timer
            cx->timer_gen[TIMER_TRAIN_TIMEOUT]++;
            if (cx->ops->start_timer)
            {
                cx->ops->start_timer(cx, TIMER_TRAIN_TIMEOUT, TRAIN_TIMEOUT_SEC);
            }

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }
        }
        break;
    }

    case CX_EVENT_TRAIN_EXIT:
    {
        // Train exited crossing
        if (cx->track[dir] == CX_TRACK_ON_CROSSING)
        {
            cx->track[dir] = CX_TRACK_CLEARED;

            // Cancel train timeout for this direction
            cx->timer_gen[TIMER_TRAIN_TIMEOUT]++;
            if (cx->ops->cancel_timer)
            {
                cx->ops->cancel_timer(cx, TIMER_TRAIN_TIMEOUT);
            }

            // If both tracks clear, start opening gate
            if (crossing_both_tracks_clear(cx))
            {
                // Reset cleared tracks to none
                if (cx->track[CX_TRACK_UP] == CX_TRACK_CLEARED)
                {
                    cx->track[CX_TRACK_UP] = CX_TRACK_NONE;
                }
                if (cx->track[CX_TRACK_DOWN] == CX_TRACK_CLEARED)
                {
                    cx->track[CX_TRACK_DOWN] = CX_TRACK_NONE;
                }

                start_gate_open(cx);
            }

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }
        }
        break;
    }

    case CX_EVENT_GATE_CLOSED:
    {
        // Gate finished closing
        if (cx->gate == GATE_CLOSING)
        {
            cx->gate = GATE_CLOSED;

            // Cancel close timeout
            cx->timer_gen[TIMER_GATE_CLOSE_TIMEOUT]++;
            if (cx->ops->cancel_timer)
            {
                cx->ops->cancel_timer(cx, TIMER_GATE_CLOSE_TIMEOUT);
            }

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }

            // If reset pending, open gate now that close verified
            if (cx->reset_pending)
            {
                cx->reset_pending = false;
                start_gate_open(cx);
            }
        }
        break;
    }

    case CX_EVENT_GATE_OPENED:
    {
        // Gate finished opening
        if (cx->gate == GATE_OPENING)
        {
            cx->gate = GATE_OPEN;

            // Cancel open timeout
            cx->timer_gen[TIMER_GATE_OPEN_TIMEOUT]++;
            if (cx->ops->cancel_timer)
            {
                cx->ops->cancel_timer(cx, TIMER_GATE_OPEN_TIMEOUT);
            }

            // Turn off flashing lights
            cx->flash_on = false;
            if (cx->ops->set_flash)
            {
                cx->ops->set_flash(cx, false);
            }

            // Send clear to local controllers
            if (cx->ops->send_clear)
            {
                cx->ops->send_clear(cx);
            }
            get_timestamp(cx->clear_time, sizeof(cx->clear_time));

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }
        }
        break;
    }

    case CX_EVENT_TIMER:
    {
        // For timer events, 'dir' contains the timer_id
        // This ensures we only check the specific timer type that fired,
        // preventing stale timers from being misinterpreted as different types
        timer_id_t timer_id = (timer_id_t)dir;

        // Verify the generation matches for this specific timer
        if (timer_gen != cx->timer_gen[timer_id])
        {
            // Stale timer, ignore
            break;
        }

        // Handle the specific timer that fired
        switch (timer_id)
        {
        case TIMER_GATE_CLOSE_DELAY:
            // Gate close delay expired - start closing
            start_gate_close(cx);
            break;
        case TIMER_GATE_CLOSE_TIMEOUT:
            // Gate took too long to close
            enter_fault(cx, CX_FAULT_GATE_CLOSE_TIMEOUT);
            break;
        case TIMER_GATE_OPEN_TIMEOUT:
            // Gate took too long to open
            enter_fault(cx, CX_FAULT_GATE_OPEN_TIMEOUT);
            break;
        case TIMER_TRAIN_TIMEOUT:
            // Train never exited
            enter_fault(cx, CX_FAULT_TRAIN_TIMEOUT);
            break;
        default:
            // Unknown timer, ignore
            break;
        }
        break;
    }

    case CX_EVENT_INJECT_FAULT:
    {
        // Test-injected fault
        enter_fault(cx, CX_FAULT_INJECTED);
        break;
    }

    case CX_EVENT_RESET_FAULT:
    {
        // Reset fault from Central
        if (cx->fault != CX_FAULT_NONE)
        {
            cx->fault = CX_FAULT_NONE;

            // Reset tracks
            cx->track[CX_TRACK_UP] = CX_TRACK_NONE;
            cx->track[CX_TRACK_DOWN] = CX_TRACK_NONE;

            // Close gate as verification, then open after close completes
            cx->gate = GATE_CLOSING;
            cx->reset_pending = true; // Signal to open after close
            if (cx->ops->gate_command)
            {
                cx->ops->gate_command(cx, GATE_CMD_CLOSE);
            }

            // Start close timeout
            cx->timer_gen[TIMER_GATE_CLOSE_TIMEOUT]++;
            if (cx->ops->start_timer)
            {
                cx->ops->start_timer(cx, TIMER_GATE_CLOSE_TIMEOUT, GATE_CLOSE_TIMEOUT_SEC);
            }

            if (cx->ops->state_changed)
            {
                cx->ops->state_changed(cx);
            }
        }
        break;
    }
    }
}

// ============================================
// String Conversions
// ============================================
const char *cx_track_state_str(cx_track_state_t state)
{
    switch (state)
    {
    case CX_TRACK_NONE:
        return "NONE";
    case CX_TRACK_APPROACHING:
        return "APPROACHING";
    case CX_TRACK_ON_CROSSING:
        return "ON_CROSSING";
    case CX_TRACK_CLEARED:
        return "CLEARED";
    default:
        return "UNKNOWN";
    }
}

const char *gate_state_str(gate_state_t state)
{
    switch (state)
    {
    case GATE_OPEN:
        return "OPEN";
    case GATE_CLOSING:
        return "CLOSING";
    case GATE_CLOSED:
        return "CLOSED";
    case GATE_OPENING:
        return "OPENING";
    case GATE_FAULT:
        return "FAULT";
    default:
        return "UNKNOWN";
    }
}

const char *cx_fault_str(cx_fault_t fault)
{
    switch (fault)
    {
    case CX_FAULT_NONE:
        return "NONE";
    case CX_FAULT_GATE_CLOSE_TIMEOUT:
        return "GATE_CLOSE_TIMEOUT";
    case CX_FAULT_GATE_OPEN_TIMEOUT:
        return "GATE_OPEN_TIMEOUT";
    case CX_FAULT_GATE_UNEXPECTED:
        return "GATE_UNEXPECTED";
    case CX_FAULT_TRAIN_TIMEOUT:
        return "TRAIN_TIMEOUT";
    case CX_FAULT_TRAIN_EARLY:
        return "TRAIN_EARLY";
    case CX_FAULT_INJECTED:
        return "INJECTED";
    default:
        return "UNKNOWN";
    }
}
