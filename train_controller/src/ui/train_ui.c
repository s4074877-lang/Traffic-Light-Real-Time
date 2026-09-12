#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "train_ui.h"
#include "../../../common/common.h"

// String conversion helpers
static const char *track_state_str(track_state_t state)
{
    switch (state)
    {
    case TRACK_NONE:
        return "NONE";
    case TRACK_APPROACHING:
        return "APPROACHING";
    case TRACK_ON_CROSSING:
        return "ON_CROSSING";
    case TRACK_CLEARED:
        return "CLEARED";
    default:
        return "UNKNOWN";
    }
}

static const char *gate_state_str(gate_state_t state)
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

static const char *flash_state_str(flash_state_t state)
{
    return (state == FLASH_ON) ? "ON" : "OFF";
}

static const char *crossing_fault_str(crossing_fault_t fault)
{
    switch (fault)
    {
    case CROSSING_FAULT_NONE:
        return "NONE";
    case CROSSING_FAULT_GATE_CLOSE_TIMEOUT:
        return "GATE_CLOSE_TIMEOUT";
    case CROSSING_FAULT_GATE_OPEN_TIMEOUT:
        return "GATE_OPEN_TIMEOUT";
    case CROSSING_FAULT_SENSOR_FAILURE:
        return "SENSOR_FAILURE";
    case CROSSING_FAULT_COMM_LOSS:
        return "COMM_LOSS";
    default:
        return "UNKNOWN";
    }
}

static const char *connection_status_str(connection_status_t status)
{
    return (status == CONN_CONNECTED) ? "CONNECTED" : "LOST";
}

static const char *connection_status_color(connection_status_t status)
{
    return (status == CONN_CONNECTED) ? COLOR_GREEN : COLOR_RED;
}

static const char *frequency_str(train_frequency_t freq)
{
    return (freq == FREQ_PEAK) ? "PEAK 2 min" : "NIGHT 20 min";
}

static const char *signal_str(train_signal_t sig)
{
    return (sig == SIGNAL_PROCEED) ? "PROCEED" : "STOP";
}

static const char *signal_color(train_signal_t sig)
{
    return (sig == SIGNAL_PROCEED) ? COLOR_GREEN : COLOR_RED;
}

// Clear screen and move cursor to home
static void clear_screen(void)
{
    printf("\033[2J\033[H");
}

// Affected intersections for each crossing
static const char *crossing_affects(int crossing_id)
{
    switch (crossing_id)
    {
    case 0:
        return "I1, I2";
    case 1:
        return "I3, I4";
    case 2:
        return "I5, I6";
    default:
        return "N/A";
    }
}

void train_ui_init(train_ui_state_t *ui)
{
    memset(ui, 0, sizeof(train_ui_state_t));
    pthread_mutex_init(&ui->mutex, NULL);

    // Set defaults
    ui->central_status = CONN_LOST;
    ui->local_status = CONN_LOST;
    ui->frequency = FREQ_PEAK;
    ui->time_scale = 10;
    ui->signal = SIGNAL_PROCEED;
    ui->current_screen = UI_SCREEN_MENU;
    ui->needs_update = 1;

    // Initialize crossings
    for (int i = 0; i < NUM_CROSSINGS; i++)
    {
        ui->crossings[i].track_up = TRACK_NONE;
        ui->crossings[i].track_down = TRACK_NONE;
        ui->crossings[i].boom_gate = GATE_OPEN;
        ui->crossings[i].road_flash = FLASH_OFF;
        ui->crossings[i].fault = CROSSING_FAULT_NONE;
        strcpy(ui->crossings[i].preempt_time, "--:--:--");
        strcpy(ui->crossings[i].clear_time, "--:--:--");
    }

    // Initialize message info
    strcpy(ui->last_sent.timestamp, "--:--:--");
    strcpy(ui->last_sent.type, "NONE");
    strcpy(ui->last_sent.endpoint, "N/A");
    strcpy(ui->last_received.timestamp, "--:--:--");
    strcpy(ui->last_received.type, "NONE");
    strcpy(ui->last_received.endpoint, "N/A");
}

void train_ui_destroy(train_ui_state_t *ui)
{
    pthread_mutex_destroy(&ui->mutex);
}

// Internal: display status screen (must hold mutex)
static void display_status_internal(train_ui_state_t *ui)
{
    // Header
    printf("============================================================\n");
    printf("%s                 RAILWAY CONTROLLER STATUS%s\n", COLOR_BOLD, COLOR_RESET);
    printf("============================================================\n");

    // Connections section
    printf("%sCONNECTIONS%s\n", COLOR_BOLD, COLOR_RESET);
    printf("------------------------------------------------------------\n");

    printf("CENTRAL            : [%s%s%s]   Last Updated: [%s]\n",
           connection_status_color(ui->central_status),
           connection_status_str(ui->central_status),
           COLOR_RESET,
           ui->central_last_update[0] ? ui->central_last_update : "--:--:--");

    printf("LOCAL              : [%s%s%s]   Last Updated: [%s]\n",
           connection_status_color(ui->local_status),
           connection_status_str(ui->local_status),
           COLOR_RESET,
           ui->local_last_update[0] ? ui->local_last_update : "--:--:--");

    printf("Train frequency    : [%s]   Time scale: [x%d]\n",
           frequency_str(ui->frequency), ui->time_scale);

    printf("Train Signal       : [%s%s%s]\n",
           signal_color(ui->signal),
           signal_str(ui->signal),
           COLOR_RESET);

    printf("\n------------------------------------------------------------\n");

    // Crossings section
    for (int i = 0; i < NUM_CROSSINGS; i++)
    {
        crossing_state_t *c = &ui->crossings[i];

        printf("%sCROSSING P%d%s  (affects %s)\n",
               COLOR_BOLD, i + 1, COLOR_RESET, crossing_affects(i));

        printf("    Track UP (W->S)   : [%s%s%s]\n",
               (c->track_up != TRACK_NONE) ? COLOR_YELLOW : "",
               track_state_str(c->track_up),
               COLOR_RESET);

        printf("    Track DOWN (S->W) : [%s%s%s]\n",
               (c->track_down != TRACK_NONE) ? COLOR_YELLOW : "",
               track_state_str(c->track_down),
               COLOR_RESET);

        printf("    Boom Gate         : [%s%s%s]\n",
               (c->boom_gate == GATE_FAULT) ? COLOR_RED :
               (c->boom_gate == GATE_CLOSED) ? COLOR_GREEN :
               (c->boom_gate == GATE_CLOSING || c->boom_gate == GATE_OPENING) ? COLOR_YELLOW : "",
               gate_state_str(c->boom_gate),
               COLOR_RESET);

        printf("    Road Flash        : [%s%s%s]\n",
               (c->road_flash == FLASH_ON) ? COLOR_YELLOW : "",
               flash_state_str(c->road_flash),
               COLOR_RESET);

        printf("    Fault             : [%s%s%s]\n",
               (c->fault != CROSSING_FAULT_NONE) ? COLOR_RED : "",
               crossing_fault_str(c->fault),
               COLOR_RESET);

        printf("    Preempt           : [SENT %s]   Clear: [SENT %s]\n",
               c->preempt_time, c->clear_time);

        if (i < NUM_CROSSINGS - 1)
        {
            printf("\n");
        }
    }

    printf("\n------------------------------------------------------------\n");

    // Messages section
    printf("%sMESSAGES%s\n", COLOR_BOLD, COLOR_RESET);
    printf("------------------------------------------------------------\n");

    printf("Last Sent     : [%s] [%s] -> [%s]\n",
           ui->last_sent.timestamp,
           ui->last_sent.type,
           ui->last_sent.endpoint);

    printf("Last Received : [%s] [%s] <- [%s]\n",
           ui->last_received.timestamp,
           ui->last_received.type,
           ui->last_received.endpoint);

    printf("Active Faults : [%s%d%s]\n",
           (ui->active_faults > 0) ? COLOR_RED : COLOR_GREEN,
           ui->active_faults,
           COLOR_RESET);

    printf("============================================================\n");
    printf("\nPress %sq%s + Enter to return to main menu\n", COLOR_BOLD, COLOR_RESET);
    printf("%s> %s", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);
}

// Internal: display menu screen (must hold mutex)
static void display_menu_internal(train_ui_state_t *ui)
{
    (void)ui;

    printf("============================================================\n");
    printf("%s                  TRAIN CONTROLLER MENU%s\n", COLOR_BOLD, COLOR_RESET);
    printf("============================================================\n");
    printf("\n");
    printf("  %s1%s. View Status\n", COLOR_BOLD, COLOR_RESET);
    printf("  %s2%s. Send Command\n", COLOR_BOLD, COLOR_RESET);
    printf("  %s3%s. Exit\n", COLOR_BOLD, COLOR_RESET);
    printf("\n");
    printf("============================================================\n");
    printf("\n%sSelect option:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);
}

// Internal: display command screen (must hold mutex)
static void display_command_internal(train_ui_state_t *ui)
{
    (void)ui;

    printf("============================================================\n");
    printf("%s                   COMMAND INPUT%s\n", COLOR_BOLD, COLOR_RESET);
    printf("============================================================\n");
    printf("\n");
    printf("  %sConnection Commands:%s\n", COLOR_BOLD, COLOR_RESET);
    printf("    %ssend-central%s  - Send test message to Central\n", COLOR_CYAN, COLOR_RESET);
    printf("    %ssend-local%s    - Send test message to Local\n", COLOR_CYAN, COLOR_RESET);
    printf("\n");
    printf("  %sCrossing Simulation:%s\n", COLOR_BOLD, COLOR_RESET);
    printf("    %strain-up%s       - Train W->E through P3->P2->P1\n", COLOR_CYAN, COLOR_RESET);
    printf("    %strain-down%s     - Train E->W through P1->P2->P3\n", COLOR_CYAN, COLOR_RESET);
    printf("    %strain P# dir%s   - Single train (e.g., train P1 up)\n", COLOR_CYAN, COLOR_RESET);
    printf("\n");
    printf("  %sFault Injection:%s\n", COLOR_BOLD, COLOR_RESET);
    printf("    %sp#-fault%s       - Inject fault (e.g., p1-fault)\n", COLOR_CYAN, COLOR_RESET);
    printf("    %sreset P#%s       - Reset fault (e.g., reset P1)\n", COLOR_CYAN, COLOR_RESET);
    printf("    %sstuck P#%s       - Make gate stuck\n", COLOR_CYAN, COLOR_RESET);
    printf("\n");
    printf("  %sTesting:%s\n", COLOR_BOLD, COLOR_RESET);
    printf("    %stest%s           - Run all requirement tests\n", COLOR_CYAN, COLOR_RESET);
    printf("    %stest N%s         - Run single test (1-7)\n", COLOR_CYAN, COLOR_RESET);
    printf("    %sscale N%s        - Set time scale (1-100)\n", COLOR_CYAN, COLOR_RESET);
    printf("    %sstatus%s         - Show simulator status\n", COLOR_CYAN, COLOR_RESET);
    printf("    %shelp%s           - Show all simulator commands\n", COLOR_CYAN, COLOR_RESET);
    printf("\n");
    printf("  Press %sq%s + Enter to return to main menu\n", COLOR_BOLD, COLOR_RESET);
    printf("\n");
    printf("============================================================\n");
    printf("\n%sCommand:%s ", COLOR_BOLD, COLOR_RESET);
    fflush(stdout);
}

void train_ui_display(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);

    clear_screen();

    switch (ui->current_screen)
    {
    case UI_SCREEN_MENU:
        display_menu_internal(ui);
        break;
    case UI_SCREEN_STATUS:
        display_status_internal(ui);
        break;
    case UI_SCREEN_COMMAND:
        display_command_internal(ui);
        break;
    }

    ui->needs_update = 0;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_display_menu(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    clear_screen();
    display_menu_internal(ui);
    ui->needs_update = 0;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_display_status(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    clear_screen();
    display_status_internal(ui);
    ui->needs_update = 0;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_display_command(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    clear_screen();
    display_command_internal(ui);
    ui->needs_update = 0;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_screen(train_ui_state_t *ui, ui_screen_t screen)
{
    pthread_mutex_lock(&ui->mutex);
    ui->current_screen = screen;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

ui_screen_t train_ui_get_screen(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    ui_screen_t screen = ui->current_screen;
    pthread_mutex_unlock(&ui->mutex);
    return screen;
}

void train_ui_request_update(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

int train_ui_needs_update(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    int result = ui->needs_update;
    pthread_mutex_unlock(&ui->mutex);
    return result;
}

void train_ui_clear_update_flag(train_ui_state_t *ui)
{
    pthread_mutex_lock(&ui->mutex);
    ui->needs_update = 0;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_connection(train_ui_state_t *ui, controller_type_t controller,
                             connection_status_t status, const char *last_update)
{
    pthread_mutex_lock(&ui->mutex);

    if (controller == CONTROLLER_CENTRAL)
    {
        // Only update timestamp if state actually changed
        if (ui->central_status != status && last_update)
        {
            strncpy(ui->central_last_update, last_update, sizeof(ui->central_last_update) - 1);
            ui->central_last_update[sizeof(ui->central_last_update) - 1] = '\0';
            ui->needs_update = 1;
        }
        ui->central_status = status;
    }
    else if (controller == CONTROLLER_LOCAL)
    {
        // Only update timestamp if state actually changed
        if (ui->local_status != status && last_update)
        {
            strncpy(ui->local_last_update, last_update, sizeof(ui->local_last_update) - 1);
            ui->local_last_update[sizeof(ui->local_last_update) - 1] = '\0';
            ui->needs_update = 1;
        }
        ui->local_status = status;
    }

    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_crossing(train_ui_state_t *ui, int crossing_id,
                           const crossing_state_t *crossing)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS)
        return;

    pthread_mutex_lock(&ui->mutex);
    ui->crossings[crossing_id] = *crossing;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_track_state(train_ui_state_t *ui, int crossing_id,
                              track_direction_t direction, track_state_t state)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS)
        return;

    pthread_mutex_lock(&ui->mutex);
    if (direction == TRACK_UP)
    {
        ui->crossings[crossing_id].track_up = state;
    }
    else
    {
        ui->crossings[crossing_id].track_down = state;
    }
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_gate_state(train_ui_state_t *ui, int crossing_id,
                             gate_state_t state)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS)
        return;

    pthread_mutex_lock(&ui->mutex);
    ui->crossings[crossing_id].boom_gate = state;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_crossing_fault(train_ui_state_t *ui, int crossing_id,
                                 crossing_fault_t fault)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS)
        return;

    pthread_mutex_lock(&ui->mutex);
    ui->crossings[crossing_id].fault = fault;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_preempt_time(train_ui_state_t *ui, int crossing_id,
                               const char *timestamp)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS || !timestamp)
        return;

    pthread_mutex_lock(&ui->mutex);
    strncpy(ui->crossings[crossing_id].preempt_time, timestamp,
            sizeof(ui->crossings[crossing_id].preempt_time) - 1);
    ui->crossings[crossing_id].preempt_time[sizeof(ui->crossings[crossing_id].preempt_time) - 1] = '\0';
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_clear_time(train_ui_state_t *ui, int crossing_id,
                             const char *timestamp)
{
    if (crossing_id < 0 || crossing_id >= NUM_CROSSINGS || !timestamp)
        return;

    pthread_mutex_lock(&ui->mutex);
    strncpy(ui->crossings[crossing_id].clear_time, timestamp,
            sizeof(ui->crossings[crossing_id].clear_time) - 1);
    ui->crossings[crossing_id].clear_time[sizeof(ui->crossings[crossing_id].clear_time) - 1] = '\0';
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_frequency(train_ui_state_t *ui, train_frequency_t freq)
{
    pthread_mutex_lock(&ui->mutex);
    ui->frequency = freq;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_signal(train_ui_state_t *ui, train_signal_t signal)
{
    pthread_mutex_lock(&ui->mutex);
    ui->signal = signal;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_time_scale(train_ui_state_t *ui, int scale)
{
    pthread_mutex_lock(&ui->mutex);
    ui->time_scale = scale;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_last_sent(train_ui_state_t *ui, const char *timestamp,
                            const char *type, const char *dest)
{
    pthread_mutex_lock(&ui->mutex);

    if (timestamp)
    {
        strncpy(ui->last_sent.timestamp, timestamp,
                sizeof(ui->last_sent.timestamp) - 1);
        ui->last_sent.timestamp[sizeof(ui->last_sent.timestamp) - 1] = '\0';
    }
    if (type)
    {
        strncpy(ui->last_sent.type, type, sizeof(ui->last_sent.type) - 1);
        ui->last_sent.type[sizeof(ui->last_sent.type) - 1] = '\0';
    }
    if (dest)
    {
        strncpy(ui->last_sent.endpoint, dest,
                sizeof(ui->last_sent.endpoint) - 1);
        ui->last_sent.endpoint[sizeof(ui->last_sent.endpoint) - 1] = '\0';
    }

    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_last_received(train_ui_state_t *ui, const char *timestamp,
                                const char *type, const char *src)
{
    pthread_mutex_lock(&ui->mutex);

    if (timestamp)
    {
        strncpy(ui->last_received.timestamp, timestamp,
                sizeof(ui->last_received.timestamp) - 1);
        ui->last_received.timestamp[sizeof(ui->last_received.timestamp) - 1] = '\0';
    }
    if (type)
    {
        strncpy(ui->last_received.type, type,
                sizeof(ui->last_received.type) - 1);
        ui->last_received.type[sizeof(ui->last_received.type) - 1] = '\0';
    }
    if (src)
    {
        strncpy(ui->last_received.endpoint, src,
                sizeof(ui->last_received.endpoint) - 1);
        ui->last_received.endpoint[sizeof(ui->last_received.endpoint) - 1] = '\0';
    }

    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void train_ui_set_active_faults(train_ui_state_t *ui, int count)
{
    pthread_mutex_lock(&ui->mutex);
    ui->active_faults = count;
    ui->needs_update = 1;
    pthread_mutex_unlock(&ui->mutex);
}

void *train_ui_refresh_thread(void *arg)
{
    train_ui_state_t *ui = (train_ui_state_t *)arg;

    while (1)
    {
        if (train_ui_needs_update(ui))
        {
            train_ui_display(ui);
        }
        sleep(UI_CHECK_INTERVAL);
    }

    return NULL;
}
