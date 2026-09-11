# Central integration contract and open decisions

This document describes the existing version 1 interface in `../common/protocol.h` and the Central-only integration boundary. It does not define new override/display behavior or change the shared ABI. Local and Train still need their real application handlers and target integration tests.

## What can be integrated now

| Route | Message | Existing meaning |
| --- | --- | --- |
| Local -> Central | `MSG_STATUS_UPDATE` | Latest observed state of one intersection |
| Local/Train -> Central | `MSG_HEARTBEAT` | Contact, with typed health information when supplied |
| Local/Train -> Central | `MSG_FAULT_ALERT` | Report a fault; `FAULT_NONE` clears the retained alert |
| Train -> Central | `MSG_RAILWAY_STATUS` | Latest crossing/train/gate state |
| Central -> Local | `MSG_MODE_COMMAND` | Request fixed/sensor mode, temporary mode, or revert |
| Central -> Local | `MSG_COORDINATION_COMMAND` | Request fixed-mode phase and cycle offset |
| Local -> Central | `MSG_OVERRIDE_REQUEST` | Declared in v1; behavior pending agreement with Thao |
| Central -> Local | `MSG_DISPLAY_UPDATE` | Declared in v1; behavior pending agreement with Thao |

Central also probes the existing Local and Train services with heartbeat messages for compatibility with their demonstration receivers. This is contact checking; Central does not send train or lamp-control commands. The route comments in `protocol.h` do not fully describe this existing heartbeat exchange.

Service names are `traffic_central_controller`, `traffic_local_controller`, and `traffic_train_controller`. Use local name lookup when all processes run on one QNX VM (`-l`); global lookup requires the group's GNS deployment. The current deployment has one Local service responsible for routing I1-I6 internally. Six independent Local processes cannot all register this same name on one node without an agreed addressing change.

## Exact version 1 wire format

Every application request is one complete `test_message_t`, including its `msg_header_t` and 64-byte `data` array. Copy the appropriate typed payload into `data`. Zero-initialize the envelope and payload; set `header.type`, `header.src`, `header.dst`, and a terminated timestamp. IDs use enum values: `I1 == 0`, `I6 == 5`, `P1 == 0`, `P3 == 2`.

Do not send only `status_msg_t`, or a header followed by a shorter payload. The existing `any_msg_t` is a union: its `header` and `status` members overlap, so setting both does not construct a valid header-plus-payload frame. The v1 transport uses the native C layouts and has no wire version field or portable serialization; all peers must use compatible definitions, layout, and byte order.

Every normal application reply is a full `reply_t` sent with **QNX reply status zero**:

```c
MsgReply(rcvid, 0, &reply, sizeof(reply));
```

`reply.status == 0` means the receiver accepted the request; `-1` means rejection. A successful mode or coordination reply must echo that request's nonzero `command_id`. Other supported messages use zero. Central tolerates a legacy rejection with a zero ID, but a new Local handler should echo the rejected command ID too. Do not put `sizeof(reply)` in the second `MsgReply` argument: that argument becomes the return value of `MsgSend`, not the reply length.

### Local status and heartbeat examples

These fragments illustrate message construction, not a complete Local controller. `snapshot` must come from the Local state machine under its synchronization rules; do not fabricate operational state to make Central appear ready.

```c
#include "common/common.h"  /* adjust include path for your target */

static void local_frame(test_message_t *frame, msg_type_t type) {
    time_t now = time(NULL);
    struct tm wall;
    memset(frame, 0, sizeof(*frame));
    frame->header.type = type;
    frame->header.src = CONTROLLER_LOCAL;
    frame->header.dst = CONTROLLER_CENTRAL;
    if (localtime_r(&now, &wall) != NULL)
        strftime(frame->header.timestamp, sizeof(frame->header.timestamp),
                 "%H:%M:%S", &wall);
}

static void make_status(test_message_t *frame,
                        const status_msg_t *snapshot) {
    local_frame(frame, MSG_STATUS_UPDATE);
    memcpy(frame->data, snapshot, sizeof(*snapshot));
}

static void make_heartbeat(test_message_t *frame, uint8_t intersection,
                           uint8_t healthy, uint16_t sequence) {
    heartbeat_msg_t heartbeat = {0};
    local_frame(frame, MSG_HEARTBEAT);
    heartbeat.sender_id = intersection; /* I1..I6 */
    heartbeat.healthy = healthy;        /* 0 degraded, 1 healthy */
    heartbeat.sequence = sequence;
    memcpy(frame->data, &heartbeat, sizeof(heartbeat));
}
```

A telemetry worker can send the complete frame to an already opened Central connection:

```c
test_message_t frame;
reply_t reply;
uint64_t timeout_ns = UINT64_C(500000000);
make_status(&frame, &snapshot);
memset(&reply, 0xa5, sizeof(reply)); /* a short reply must not look successful */
if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                 NULL, &timeout_ns, NULL) == -1) {
    /* Record error; do not issue an unbounded send as fallback. */
} else {
    int result = MsgSend(central_coid, &frame, sizeof(frame),
                         &reply, sizeof(reply));
    if (result != 0 || reply.status != 0 || reply.command_id != 0 ||
        memchr(reply.timestamp, '\0', sizeof(reply.timestamp)) == NULL) {
        /* Treat this attempt as rejected/unconfirmed; inspect errno when -1. */
    }
}
```

Keep telemetry IPC outside the Local lamp/state-machine thread. Use a bounded handoff or latest-state snapshot so a missing Central cannot stall phase transitions. Keep memory owned by the sending worker valid until its actual `MsgSend` returns; a caller-side timeout must not free memory still referenced by an outstanding IPC operation.

Legacy `common/communication/send.c` sends an all-zero heartbeat payload. Central treats `{sender_id=0, healthy=0, sequence=0}` as unspecified legacy health, not an explicit degradation of I1 or P1. It does not clear previously reported typed degradation. A typed degraded I1/P1 heartbeat must use a nonzero sequence to avoid this ambiguity, including when the 16-bit sequence wraps. This compatibility convention is not a session or replay-protection mechanism.

### Local mode and coordination receipt

After checking the full received byte count, route, timestamp termination, enum values, target, duration/offset, and nonzero command ID, copy the payload out of `data`:

```c
mode_cmd_msg_t mode;
coordination_command_msg_t coordination;
reply_t reply = {0};
/* `frame` is a complete, validated test_message_t received from Central. */
if (frame.header.type == MSG_MODE_COMMAND) {
    memcpy(&mode, frame.data, sizeof(mode));
    reply.command_id = mode.command_id;
    reply.status = local_try_enqueue_mode(&mode) == 0 ? 0 : -1;
} else if (frame.header.type == MSG_COORDINATION_COMMAND) {
    memcpy(&coordination, frame.data, sizeof(coordination));
    reply.command_id = coordination.command_id;
    reply.status = local_try_enqueue_coordination(&coordination) == 0 ? 0 : -1;
} else {
    reply.status = -1;
}
/* Fill reply.timestamp with a terminated HH:MM:SS string, if available. */
MsgReply(rcvid, 0, &reply, sizeof(reply));
```

The two `local_try_enqueue_*` names above are illustrative Local-owned functions, not existing APIs. They must make a bounded copy and reject when the Local queue cannot accept the request. The raw-receiver example replies exactly once. With the existing `receive_loop` callback API, fill its supplied `reply_t` and return zero instead; that wrapper issues `MsgReply` itself. Returning an error through the old wrapper replaces the supplied reply with a generic rejection and loses the command ID.

The Local state machine applies an accepted request later at a safe transition. Publish the actual resulting `status_msg_t` after it applies; never change telemetry immediately to the requested state unless the real state has changed. `ACCEPTED` in Central's history confirms receipt/acceptance only. Version 1 has no applied-command ID in status, so observing the requested mode later cannot prove which request caused it. Central leaves uncertain delivery `UNCONFIRMED` and does not automatically retry a command that may already have executed.

Central sends per-intersection requests for `all`. Each gets a separate ID; the group is not atomic or simultaneous. Coordination v1 contains a phase and relative offset but no common activation timestamp, timebase, or late-arrival rule. Agree those semantics before claiming synchronized intersections.

## Local owns real-time actuation

Central provides supervision and operator requests. Local owns safe phase transitions, pedestrian timing, railway preemption, output validation, and behavior during communication loss. A Central request cannot authorize conflicting movements or bypass railway/failsafe behavior. Local also owns a temporary mode's timer and reversion, so disconnecting Central cannot leave the timer dependent on further messages.

The group must agree when a temporary duration starts (receipt or safe application), which baseline is restored, and how replacement/revert behaves during railway preemption. These details are not fully specified by the v1 fields. Likewise, `CMD_PRIO_OPERATOR` and `CMD_PRIO_SCHEDULE` are application request priorities, not QNX thread priorities or permission to override safety.

Proposed telemetry contract for discussion: publish state changes promptly and repeat each owned intersection's current state at least once per second. Heartbeats alone show contact, not that phase/status generation is progressing. Central uses a provisional five-second status-age limit for commands, configurable with `-s 1..60`, and rechecks readiness before transmission. Its five-second queue lifetime discards unsent old operator intent. These are supervisory settings, not proven safety deadlines; agree the reporting period, worst-case latency, and allowed age with Local before changing them.

## Override and display: decisions for Thao

The declarations establish routes and fields; they do not require automatic approval or define a usable end-to-end behavior. Central currently rejects unsupported override requests and does not produce display updates. Continue developing against the existing types, but agree the following before adding application behavior:

| Decision | What to settle |
| --- | --- |
| Route and scope | Does only Local originate overrides? Does `source_id` always mean an intersection? Which Local display consumes a Central update? |
| Meaning | Is override a notification, operator approval request, or automatic mode request? What are the valid `reason` values? What information must the display show? |
| Timing | When does a request expire? How often are display updates sent? Which timebase and safe activation point apply? |
| Ownership | Who decides acceptance? Local retains railway/failsafe and temporary-expiry authority. Is display informational or used by another controller? |
| Reply and result | How is `request_id` correlated with approval/rejection and later application? Can existing messages express this, or does the group need an explicit protocol revision? |
| Failure and replacement | What happens on a duplicate, new request, restart, communication loss, delayed reply, or stale display data? |

Suggested message to Thao:

> T dang hoan thien Central theo protocol hien tai. Phan override/display m dang lam, gui t giup flow du kien nhe: Local gui override khi nao, Central chi ghi nhan hay can duyet/gui mode lai; display can hien gi va cap nhat bao lau; request_id/ACK va timeout xu ly sao? T da chuan bi interface va test phia Central, nhung can chot y nghia truoc khi ghep de khoi sua qua lai. Railway/failsafe va hen gio revert van do Local quan ly.

## QNX real-time review

These are implementation criteria and limits; running on an RTOS does not prove the whole application meets its deadlines.

- Use `CLOCK_MONOTONIC` for elapsed time, freshness, queue expiry, and periodic release times. Wall-clock timestamps are for human logs. If a condition-variable deadline is monotonic, initialize its clock to monotonic too; the default is the system clock. [QNX monotonic clock](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.getting_started/topic/s1_timer_CLOCK_MONOTONIC.html), [condition-variable clock](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_condattr_setclock.html).
- Retain absolute heartbeat due times and skip missed releases without a burst of catch-up sends. Check heartbeat work between commands. This avoids cumulative work-plus-sleep drift, but a send already in progress can delay the next probe. QNX sleep expiry makes a thread ready; clock resolution and scheduling can delay actual execution. [QNX clock_nanosleep](https://www.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.lib_ref/topic/c/clock_nanosleep.html).
- Keep shared-state critical sections short. QNX's default mutex attributes already use priority inheritance; this helps priority inversion but does not make an unbounded I/O operation inside a lock bounded. Choose process/thread priorities with the whole Local/Train workload, then measure them; raising Central above safety-critical Local work is not an automatic improvement. [QNX mutexes](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.sys_arch/topic/kernel_Mutexes.html), [QNX scheduling](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.prog/topic/overview_SCHEDS.html).
- Bound queues and application waits, and define overload behavior. Call `TimerTimeout` immediately before the intended blocking IPC. A 500 ms caller-side wait limits that application's wait under scheduling assumptions; it is not a universal upper bound on kernel IPC completion. [QNX TimerTimeout](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html).

With `_NTO_CHF_UNBLOCK`, a receiver that already accepted a message gets an unblock pulse when the sender tries to time out. The receiver must resolve the pending transaction with a reply/error; a stopped or unresponsive receiver can keep the sender blocked. Central isolates one pending operation per link so the other peer and operator processing can continue. It cannot force a legacy peer to release that request. Shutdown may remain pending until that peer resumes, replies, or exits. Correcting this needs peer/channel design and tests, not another Central-only timeout. [QNX channel unblock behavior](https://qdn.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html).

## Integration evidence still required

The Central tests use peer fixtures. Before calling the integrated system complete, test with Thao's real Local and the real Train processes on the target QNX setup:

1. Start in either order, exchange all per-target reports, disconnect/reconnect, and verify stale data cannot authorize a command until fresh status arrives.
2. Accept, reject, queue, expire, and safely apply mode/temporary/revert/coordination requests; verify matching IDs, actual state reports, temporary expiry with Central offline, and railway precedence.
3. Flood operator commands while delaying one peer's replies. Verify bounded memory, heartbeat service between sends, queue expiry, and continued operation of the other peer. Stop a peer after it receives a message; verify continued Central responsiveness and the documented pending-shutdown behavior, then resume it.
4. Restart peers, wrap heartbeat/command IDs, and inject delayed/duplicate messages. Version 1 has no session ID, status sequence, explicit apply acknowledgement, or idempotence contract; record these limits rather than treating timestamps as proof of freshness.
5. Measure worst observed release jitter, receive-to-ACK latency, command queue age, status delivery latency, lock hold times, CPU use, and UI/log backpressure under target load and the actual GNS/network path. Agree deadlines, analyze worst-case execution/blocking and task priorities, and record whether the system meets them. Fixture passes or average timings alone do not establish a hard real-time guarantee.
