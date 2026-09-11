#include "monitor.h"

static int older_sequence(uint32_t incoming, uint32_t current) {
    return incoming != current && (uint32_t)(incoming - current) >= UINT32_C(0x80000000);
}

int monitor_peer_index(controller_type_t source) {
    if (source == CONTROLLER_LOCAL) return 0;
    if (source == CONTROLLER_TRAIN) return 1;
    return -1;
}

void monitor_heartbeat(monitor_t *m, controller_type_t source, int healthy, uint64_t now) {
    int index = monitor_peer_index(source);
    if (index < 0) return;
    if (m->peers[index].seen && now >= m->peers[index].last_heartbeat &&
        now - m->peers[index].last_heartbeat >= HEARTBEAT_MISS_LIMIT * NANOSECONDS_PER_SEC) {
        int connected = m->peers[index].connected;
        monitor_disconnect(m, source);
        m->peers[index].connected = connected;
    }
    m->peers[index].seen = 1;
    m->peers[index].healthy = healthy;
    m->peers[index].last_heartbeat = now;
}

int monitor_peer_online(const monitor_t *m, controller_type_t source, uint64_t now) {
    int index = monitor_peer_index(source);
    const peer_status_t *peer;
    if (index < 0) return 0;
    peer = &m->peers[index];
    return peer->seen && now >= peer->last_heartbeat &&
           now - peer->last_heartbeat < HEARTBEAT_MISS_LIMIT * NANOSECONDS_PER_SEC;
}

void monitor_device_heartbeat(monitor_t *m, unsigned id, int healthy, uint64_t now) {
    intersection_status_t *entry;
    if (id >= NUM_INTERSECTIONS) return;
    entry = &m->intersections[id];
    if (entry->heartbeat_seen && now >= entry->last_heartbeat &&
        now - entry->last_heartbeat >= HEARTBEAT_MISS_LIMIT * NANOSECONDS_PER_SEC)
        entry->synchronized = 0;
    entry->heartbeat_seen = 1;
    entry->healthy = healthy;
    entry->last_heartbeat = now;
}

void monitor_disconnect(monitor_t *m, controller_type_t source) {
    int index = monitor_peer_index(source);
    unsigned i;
    if (index < 0) return;
    m->peers[index].connected = 0;
    if (source == CONTROLLER_TRAIN) {
        for (i = 0; i < NUM_CROSSINGS; ++i) m->crossings[i].synchronized = 0;
        return;
    }
    for (i = 0; i < NUM_INTERSECTIONS; ++i) m->intersections[i].synchronized = 0;
    for (i = 0; i < CENTRAL_COMMAND_HISTORY; ++i) {
        command_record_t *record = &m->commands[i];
        if (record->id && record->state == REQUEST_QUEUED)
            record->state = REQUEST_REJECTED;
        else if (record->id && (record->state == REQUEST_SENDING || record->state == REQUEST_ACCEPTED))
            record->state = REQUEST_UNCERTAIN;
    }
}

command_record_t *monitor_find_command(monitor_t *m, uint16_t id) {
    unsigned i;
    if (!id) return NULL;
    for (i = 0; i < CENTRAL_COMMAND_HISTORY; ++i)
        if (m->commands[i].id == id) return &m->commands[i];
    return NULL;
}

int monitor_add_command(monitor_t *m, uint16_t id, uint16_t type, uint8_t target) {
    unsigned i;
    if (!id || target >= NUM_INTERSECTIONS || monitor_find_command(m, id)) return 0;
    for (i = 0; i < CENTRAL_COMMAND_HISTORY; ++i) {
        unsigned slot = (m->command_next + i) % CENTRAL_COMMAND_HISTORY;
        command_record_t *record = &m->commands[slot];
        if (record->id && (record->state == REQUEST_QUEUED ||
            record->state == REQUEST_SENDING || record->state == REQUEST_ACCEPTED ||
            record->state == REQUEST_UNCERTAIN)) continue;
        record->id = id;
        record->type = type;
        record->target = target;
        record->state = REQUEST_QUEUED;
        m->command_next = (slot + 1) % CENTRAL_COMMAND_HISTORY;
        if (m->command_count < CENTRAL_COMMAND_HISTORY) ++m->command_count;
        return 1;
    }
    return 0;
}

int monitor_status(monitor_t *m, const status_msg_t *s, uint32_t session,
                   uint32_t sequence, uint64_t now, int snapshot) {
    intersection_status_t *entry;
    command_record_t *record;
    if (!protocol_valid_status(s)) return 0;
    entry = &m->intersections[s->intersection_id];
    if (entry->valid) {
        if (entry->session_id != session && !snapshot) {
            entry->synchronized = 0;
            return 0;
        }
        if (entry->session_id == session && (older_sequence(sequence, entry->sequence) ||
            (!snapshot && sequence == entry->sequence))) return 0;
    }
    entry->valid = 1;
    if (snapshot) entry->synchronized = 1;
    entry->status = *s;
    entry->session_id = session;
    entry->sequence = sequence;
    entry->received_at = now;
    record = monitor_find_command(m, s->command_id);
    if (record && s->command_session_id == m->session_id &&
        record->target == s->intersection_id &&
        record->state != REQUEST_QUEUED && record->state != REQUEST_REJECTED) {
        switch (s->command_state) {
            case COMMAND_QUEUED:
                if (record->state == REQUEST_SENDING || record->state == REQUEST_UNCERTAIN)
                    record->state = REQUEST_ACCEPTED;
                break;
            case COMMAND_APPLIED:
                if (record->state != REQUEST_EXPIRED) record->state = REQUEST_APPLIED;
                break;
            case COMMAND_REJECTED:
                if (record->state != REQUEST_APPLIED && record->state != REQUEST_EXPIRED)
                    record->state = REQUEST_REJECTED;
                break;
            case COMMAND_EXPIRED: record->state = REQUEST_EXPIRED; break;
            default: break;
        }
    }
    return 1;
}

int monitor_railway_status(monitor_t *m, const railway_status_msg_t *s,
                           uint32_t session, uint32_t sequence, uint64_t now, int snapshot) {
    crossing_status_t *entry;
    if (!protocol_valid_railway_status(s)) return 0;
    entry = &m->crossings[s->crossing_id];
    if (entry->valid && entry->session_id == session &&
        (older_sequence(sequence, entry->sequence) || (!snapshot && sequence == entry->sequence)))
        return 0;
    if (entry->valid && entry->session_id != session && !snapshot) {
        entry->synchronized = 0;
        return 0;
    }
    entry->valid = 1;
    if (snapshot) entry->synchronized = 1;
    entry->status = *s;
    entry->session_id = session;
    entry->sequence = sequence;
    entry->received_at = now;
    return 1;
}

int monitor_can_command(const monitor_t *m, unsigned target, uint64_t now) {
    const intersection_status_t *entry;
    if (target >= NUM_INTERSECTIONS || !m->peers[0].connected ||
        !monitor_peer_online(m, CONTROLLER_LOCAL, now) || !m->peers[0].healthy) return 0;
    entry = &m->intersections[target];
    if (entry->heartbeat_seen && (!entry->healthy || now < entry->last_heartbeat ||
        now - entry->last_heartbeat >= HEARTBEAT_MISS_LIMIT * NANOSECONDS_PER_SEC)) return 0;
    return entry->valid && entry->synchronized && now >= entry->received_at &&
           now - entry->received_at < STATUS_STALE_SEC * NANOSECONDS_PER_SEC;
}
