#include "monitor.h"

int central_peer_index(controller_type_t source) {
    return source == CONTROLLER_LOCAL ? 0 : source == CONTROLLER_TRAIN ? 1 : -1;
}

int central_peer_online(const central_monitor_t *m, controller_type_t source, uint64_t now) {
    int index = central_peer_index(source);
    const central_peer_status_t *peer;
    if (index < 0) return 0;
    peer = &m->peers[index];
    return peer->seen && now >= peer->last_seen &&
           now - peer->last_seen < HEARTBEAT_MISS_LIMIT * CENTRAL_NSEC;
}

void central_peer_disconnected(central_monitor_t *m, controller_type_t source) {
    int index = central_peer_index(source);
    unsigned i;
    if (index < 0) return;
    m->peers[index].connected = 0;
    m->peers[index].seen = 0;
    if (source == CONTROLLER_LOCAL) {
        for (i = 0; i < NUM_INTERSECTIONS; ++i) m->intersections[i].synchronized = 0;
    } else {
        for (i = 0; i < NUM_CROSSINGS; ++i) m->crossings[i].synchronized = 0;
    }
}

int central_observe_peer(central_monitor_t *m, controller_type_t source, uint64_t now) {
    int index = central_peer_index(source);
    int restored;
    if (index < 0) return 0;
    restored = !central_peer_online(m, source, now);
    if (restored) {
        int connected = m->peers[index].connected;
        central_peer_disconnected(m, source);
        m->peers[index].connected = connected;
    }
    m->peers[index].seen = 1;
    m->peers[index].last_seen = now;
    return restored;
}

int central_monitor_status(central_monitor_t *m, const status_msg_t *s, uint64_t now) {
    central_intersection_status_t *entry;
    if (s->intersection_id >= NUM_INTERSECTIONS || s->mode > MODE_FAILSAFE ||
        s->phase > PHASE_RAILWAY_HOLD || s->ns_state > LIGHT_GREEN ||
        s->ew_state > LIGHT_GREEN || s->pedestrian_ns > 1 || s->pedestrian_ew > 1 ||
        s->railway_preempt > 1) return 0;
    central_observe_peer(m, CONTROLLER_LOCAL, now);
    entry = &m->intersections[s->intersection_id];
    entry->valid = 1;
    entry->synchronized = 1;
    entry->received_at = now;
    entry->status = *s;
    return 1;
}

int central_monitor_railway(central_monitor_t *m, const railway_status_msg_t *s, uint64_t now) {
    central_crossing_status_t *entry;
    if (s->crossing_id >= NUM_CROSSINGS || s->train_state > TRAIN_CLEAR ||
        s->gate_state > GATE_FAULT || s->fault > FAULT_NOT_WORKING) return 0;
    central_observe_peer(m, CONTROLLER_TRAIN, now);
    entry = &m->crossings[s->crossing_id];
    entry->valid = 1;
    entry->synchronized = 1;
    entry->received_at = now;
    entry->status = *s;
    return 1;
}

int central_can_command(const central_monitor_t *m, unsigned target, uint64_t now) {
    const central_intersection_status_t *entry;
    if (target >= NUM_INTERSECTIONS || !m->peers[0].connected ||
        !central_peer_online(m, CONTROLLER_LOCAL, now)) return 0;
    entry = &m->intersections[target];
    return entry->valid && entry->synchronized && now >= entry->received_at &&
           now - entry->received_at < CENTRAL_STATUS_STALE_SEC * CENTRAL_NSEC;
}
