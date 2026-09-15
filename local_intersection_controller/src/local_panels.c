#include "local_process.h"

static const char *divider = "============================================================";

static const char *phase_name(unsigned phase) {
    const char *names[] = {"NS_GREEN", "NS_YELLOW", "EW_GREEN", "EW_YELLOW", "RAILWAY_HOLD"};
    return phase < 5 ? names[phase] : "UNKNOWN";
}

static const char *lamp_name(unsigned lamp) {
    const char *names[] = {"OFF", "RED", "YELLOW", "GREEN"};
    return lamp < 4 ? names[lamp] : "UNKNOWN";
}

static const char *traffic_mode(unsigned mode) {
    const char *names[] = {"FIXED", "SENSOR", "RAILWAY", "FAILSAFE"};
    return mode < 4 ? names[mode] : "UNKNOWN";
}

static const char *link_name(int connected) {
    return connected ? "\033[32mCONNECTED\033[0m" : "\033[31mDISCONNECTED\033[0m";
}

void local_print_heading(int selected) {
    printf("\033[2J\033[H%s\n", divider);
    if (selected == INTERSECTION_ALL) puts("                 LOCAL CONTROLLERS I1-I6");
    else printf("                    LOCAL CONTROLLER I%d\n", selected + 1);
    puts(divider);
}

static void print_pedestrian(const char *direction, int walk, int request, unsigned remaining) {
    printf("  %s [%s", direction, walk ? "WALK" : "STOP");
    if (walk && remaining > PED_WALK_END_SEC)
        printf(" %u sec", remaining - PED_WALK_END_SEC);
    printf("] request [%s]\n", request ? "PENDING" : "NONE");
}

void local_print_panel(const core_reply_t *reply, int compact) {
    const status_msg_t *s = &reply->status;
    if (compact) {
        printf("I%u | %s | %s [%u sec] | %s\n", s->intersection_id + 1,
               traffic_mode(s->mode), phase_name(s->phase), s->time_remaining,
               s->fault_active ? "FAULT" : "HEALTHY");
        printf("  Vehicle: NS [%s] EW [%s] | Cars NS [%u] EW [%u]\n",
               lamp_name(s->ns_state), lamp_name(s->ew_state), s->sensor_ns_count, s->sensor_ew_count);
        printf("  Pedestrian: NS [%s] EW [%s] | Train [%s]\n",
               s->pedestrian_ns ? "WALK" : "STOP", s->pedestrian_ew ? "WALK" : "STOP",
               s->train_active ? "ACTIVE" : s->train_pending ? "PENDING" :
               s->railway_preempt ? "RECOVERY" : "CLEAR");
        printf("  Central [%s] Train [%s]\n", link_name(reply->central_connected), link_name(reply->train_connected));
    } else {
        printf("Service [%s] mode [%s]\n", reply->service_name, reply->global_mode ? "GLOBAL" : "LOCAL");
        printf("Connected to central_controller [%s]\n", link_name(reply->central_connected));
        printf("Connected to train_controller [%s]\n", link_name(reply->train_connected));
        puts(divider);
        printf("Last message receive central [%s]\n", reply->last_recv_central[0] ? reply->last_recv_central : "N/A");
        printf("Last message receive train [%s]\n", reply->last_recv_train[0] ? reply->last_recv_train : "N/A");
        puts(divider);
        printf("Traffic mode [%s]\nPhase [%s] remaining [%u sec]\n",
               traffic_mode(s->mode), phase_name(s->phase), s->time_remaining);
        printf("Timing profile: initial [%s] baseline cycle [44 sec]\n", phase_name(reply->initial_phase));
        printf("  Baseline NS green [%d sec] EW green [%d sec]\n", reply->ns_green_sec, reply->ew_green_sec);
        printf("Telemetry seq [%u] last command [%u] health [%s]\n",
               s->status_sequence, s->last_command_id, s->fault_active ? "FAULT" : "HEALTHY");
        printf("Sim time [%02d:%02d:%02d]\n", reply->sim_seconds / 3600,
               reply->sim_seconds / 60 % 60, reply->sim_seconds % 60);
        puts("Vehicle lights:");
        printf("  NS [%s]\n  EW [%s]\n", lamp_name(s->ns_state), lamp_name(s->ew_state));
        puts("Pedestrian:");
        print_pedestrian("NS", s->pedestrian_ns, s->pedestrian_ns_request, s->time_remaining);
        print_pedestrian("EW", s->pedestrian_ew, s->pedestrian_ew_request, s->time_remaining);
        printf("Sensors:\n  NS cars [%u]\n  EW cars [%u]\n", s->sensor_ns_count, s->sensor_ew_count);
        printf("Train: %s\n", s->train_active ? "active" : s->train_pending ? "pending" :
               s->railway_preempt ? "recovery" : "none active");
        if (s->fault_active) printf("Fault type [%u] severity [%u]\n", s->fault_type, s->fault_severity);
    }
    puts(divider);
}

void local_print_commands(void) {
    puts("View: view I1..I6 | view all");
    puts("Commands: m mode | n ped-NS | e ped-EW | x sensor-NS");
    puts("          z sensor-EW | 1 train-line1 | 2 train-line2");
    puts("          p peak | o offpeak | l light | c train-clear");
    puts("          r reset | q quit");
    puts(divider);
}
