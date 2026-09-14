#ifndef LOCAL_CONTROLLER_H
#define LOCAL_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "../../common/common.h"
#include "../../common/communication/connection.h"
#include "../../common/communication/receive.h"
#include "../../common/communication/send.h"

#define DEMO_HIGH_CAR_COUNT 6
#define PED_GREEN_CAP_SEC 10
#define SIM_SECONDS_PER_TICK 1
#define SECONDS_PER_DAY 86400
#define TRAIN_PASSING_SECONDS 10
#define MAX_SENSOR_CARS 12
#define STATUS_PERIOD_SEC 1
#define LOCAL_SERVICE_NAME_MAX 64

/*
 * Demo switches:
 * Keep demo commands on for local console test cases.
 */
#ifndef ENABLE_DEMO_COMMANDS
#define ENABLE_DEMO_COMMANDS 1
#endif

#ifndef ENABLE_TRAFFIC_SIMULATION
#define ENABLE_TRAFFIC_SIMULATION 1
#endif

typedef struct {
    connection_t central_conn;
    connection_t train_conn;
    connection_mode_t mode;
    char service_name[LOCAL_SERVICE_NAME_MAX];

    char last_recv_central[32];
    char last_recv_train[32];
    char last_send_central[32];
    char last_send_train[32];
    char last_central_update[32];
    char last_train_update[32];

    traffic_light_mode traffic_mode;
    phase_t phase;
    light_state_t ns_light;
    light_state_t ew_light;
    int time_remaining;
    int phase_duration;

    int sensor_ns_count;
    int sensor_ew_count;
    int next_ns_car_in_seconds;
    int next_ew_car_in_seconds;

    int ped_ns_request;
    int ped_ew_request;
    int ped_ns_walk;
    int ped_ew_walk;

    int sim_seconds;
    int sim_running;
    int next_train_in_seconds;
    int next_train_direction;
    int train_direction;
    int train_pass_remaining;
    int train_waiting_for_clear;
    int manual_mode_override;
    int manual_sensor_override;

    int temp_mode_remaining;
    traffic_light_mode temp_return_mode;
    int temp_return_manual_override;
    int coordination_pending;
    phase_t coordination_phase;
    int coordination_offset_sec;
    int ped_extra_ns_green;
    int ped_extra_ew_green;

    uint8_t intersection_id;
    int fault_active;
    int fault_pending;
    fault_type_t fault_type;
    fault_severity_t fault_severity;
    char fault_description[32];

    int train_pending;
    int train_active;
    int train_recovery_remaining;

    int ui_needs_update;
    int status_dirty;
    uint16_t status_sequence;
    uint16_t fault_sequence;
    uint16_t heartbeat_sequence;
    uint16_t last_applied_command_id;

    pthread_cond_t status_cond;
    pthread_mutex_t mutex;
    pthread_mutex_t central_send_mutex;
    pthread_mutex_t train_send_mutex;
} local_state_t;

extern local_state_t state;

void local_state_init(connection_mode_t mode, uint8_t intersection_id,
                      const char *service_name);
void local_state_destroy(void);

void mark_status_dirty_locked(void);
traffic_light_mode display_mode(void);
int target_matches_local(uint8_t target);
int railway_matches_local(uint8_t id);
int railway_display_line(uint8_t id);

void init_message(test_message_t *msg, msg_type_t type,
                  controller_type_t src, controller_type_t dst);
void fill_status_locked(status_msg_t *status);
uint16_t prepare_status_message_locked(test_message_t *msg);
uint16_t prepare_fault_message_locked(test_message_t *msg);
uint16_t prepare_heartbeat_message_locked(test_message_t *msg);
void set_fault_locked(fault_type_t type, fault_severity_t severity,
                      const char *description);
void clear_fault_locked(void);

int is_peak_time(int seconds);
int is_night_time(int seconds);
int random_train_gap_seconds(void);
int random_train_direction(void);
int random_car_gap_seconds(void);
int green_time_for_direction(direction_t direction);
int vehicle_signal_seconds_locked(direction_t direction);
int pedestrian_signal_seconds_locked(direction_t direction);

void traffic_tick_locked(void);
void update_temporary_mode_locked(void);
void apply_time_settings_locked(void);
void update_vehicle_counts_locked(void);
void update_time_of_day_locked(void);
void add_pedestrian_request_locked(direction_t direction);
void start_train_locked(int direction);
void start_train_message_locked(int direction, int eta_seconds);
void clear_train_locked(void);
void reset_demo_inputs_locked(void);

#if ENABLE_DEMO_COMMANDS
void toggle_sensor_locked(direction_t direction);
#endif

#if ENABLE_DEMO_COMMANDS && ENABLE_TRAFFIC_SIMULATION
void set_sim_hour_locked(int hour);
#endif

void display_ui(void);
int execute_command(const char *cmd);

void local_receive_init(receive_context_t *recv_ctx, name_attach_t *attach);
void* message_handler_thread(void *arg);
void* connection_thread(void *arg);
void* ui_refresh_thread(void *arg);
void* traffic_thread(void *arg);
void* status_thread(void *arg);
void* heartbeat_thread(void *arg);

#endif
