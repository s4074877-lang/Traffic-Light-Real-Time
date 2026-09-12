# Central integration contract

This revision changes Central-owned code and documents only. It adapts to the existing shared protocol, Local skeleton and merged Train simulator; it does not change those teammates' files or establish that their state machines are complete. See [IMPLEMENTATION_NOTE.md](IMPLEMENTATION_NOTE.md) for architecture and assignment coverage, and [VALIDATION.md](VALIDATION.md) for measured evidence.

## Ownership and assignment interpretation

Central displays reported intersection and railway states and requests high-level operating patterns. Local owns individual lamp outputs, safe phase transitions, pedestrians, railway preemption, temporary-mode expiry and continued operation when Central disappears. Train owns crossing gates, train approach signals and their safety decisions. Central requests cannot bypass those safety rules.

The assignment's page 2 says traffic control senses railway gates rather than directly controlling them; page 3 allows operator requests to the boom-gate/train-approach system. The implemented boundary is a Train-owned request handler. `train-cmd train-up` simulates a train travelling through crossings; it does not raise a gate. Open/closing/closed/opening/fault display concerns boom gates, not passenger-train doors.

Central-origin operator overrides are described by the assignment. The repository's additional Local-origin `MSG_OVERRIDE_REQUEST` and Central-to-Local `MSG_DISPLAY_UPDATE` are optional design choices with undefined behavior. Central rejects unsupported override requests and does not produce those display messages. The separate Central display process uses a private interface and does not assign new meaning to either shared message.

## Messages and current peer support

| Route | Message | Current Central behavior and integration condition |
| --- | --- | --- |
| Local -> Central | `MSG_STATUS_UPDATE` | Retains actual mode, phase, vehicle/pedestrian states, railway preemption, reported time remaining and local receive age for I1-I6. Local must publish them. |
| Local/Train -> Central | `MSG_HEARTBEAT` | Records contact; typed payloads can additionally report per-target health. |
| Local/Train -> Central | `MSG_FAULT_ALERT` | Retains source, type, severity, description and age. An explicit `FAULT_NONE` alert clears the retained alert. |
| Train -> Central | `MSG_RAILWAY_STATUS` | Displays reported aggregate train state, gate state and fault for P1-P3. |
| Central -> Local | `MSG_MODE_COMMAND` | Requests fixed/sensor, temporary mode, or cancellation of temporary mode. Local validates and safely applies it. |
| Central -> Local | `MSG_COORDINATION_COMMAND` | Requests a fixed-mode phase and relative cycle offset; there is no shared activation epoch. |
| Central -> Train | `MSG_TEST` | Carries an allowlisted simulator command in the existing string payload. Train implements this route despite the older shared-header comment that Central never sends to Train. |
| Central -> Local/Train | `MSG_HEARTBEAT` | Compatibility contact probes, not proof of sensor or actuator health. |
| Local -> Central | `MSG_OVERRIDE_REQUEST` | Declared in the shared header; application contract pending. |
| Central -> Local | `MSG_DISPLAY_UPDATE` | Declared in the shared header; application contract pending. |
| `central_ui` <-> Central core | Private local request/reply | Bounded commands and rendered status/history text, independent of the shared Local/Train ABI. |

The current Local source primarily implements communication demonstrations. A successful heartbeat/test reply does not establish support for real status generation, mode application, coordination, pedestrians or railway preemption. The Local owner must supply and demonstrate those behaviors.

## Wire formats and Central compatibility adapter

Outgoing Local commands and Train simulator requests remain complete `test_message_t` envelopes: `msg_header_t` followed by a 64-byte `data` array. Copy typed payloads into `data` and zero unused bytes. On the current x86_64 build the envelope is 104 bytes; use `sizeof` the declared structures rather than hard-coded counts.

Central accepts these incoming forms after validating exact length, route, field ranges and timestamp termination:

| Incoming form | ID convention |
| --- | --- |
| Original full `test_message_t` envelope | Protocol enum IDs: I1=0 through I6=5; P1=0 through P3=2. |
| `status_full_msg_t` / `heartbeat_full_msg_t` | Protocol enum IDs. Compact framing does not change Local or heartbeat IDs. |
| Train `railway_status_full_msg_t` / `fault_full_msg_t` | Current Train sends crossing-object IDs 1 through 3. Central converts these exact compact Train frames to P1=0 through P3=2 internally. |
| Local `fault_full_msg_t` | Protocol intersection enum IDs, unchanged. |

The adapter selects a dialect using message type, source and exact structure size. It does not guess whether an overlapping ID such as `1` means P1 or P2. An envelope crossing ID `1` still means P2. A future Train compact producer must not switch to zero-based IDs without updating this agreement. This compatibility rule does not alter the shared header.

Bare payloads, truncated/oversized frames, unrelated compact types and malformed routes are rejected. `any_msg_t` is a union: its header and payload members overlap, so assigning both does not construct a combined frame. Native C layout and byte order remain part of the ABI. `PROTOCOL_VERSION` is a compile-time macro, not an on-wire negotiation field.

Every external Local/Train application reply is a complete `reply_t`, with QNX reply status zero:

```c
MsgReply(rcvid, 0, &reply, sizeof(reply));
```

The second argument becomes the return value of `MsgSend`, not its reply byte count. Set `reply.status` to zero for acceptance or `-1` for rejection, supply a terminated timestamp, and echo the nonzero mode/coordination `command_id`. Other current messages use ID zero. Central tolerates an older rejection with ID zero; new Local handlers should echo the rejected ID too. With the existing shared receive callback wrapper, fill its supplied reply and return zero; the wrapper performs `MsgReply`.

`ACCEPTED` means a receipt/acceptance reply, not application of the requested state. Status v1 has no applied-command ID. Train is weaker: its current `MSG_TEST` handler discards the simulator's Boolean result and text, then returns success. Central therefore cannot distinguish successful simulation from BUSY or application rejection. `train-cmd status` reaches that handler, but its textual simulator result is not returned to Central. Inspect actual crossing telemetry and the Train console; an ACK is insufficient evidence of success.

## Names, processes and deployment

Default external services are `traffic_central_controller`, `traffic_local_controller` and `traffic_train_controller`. `-l` uses same-node lookup; `-g` requires functioning GNS/Qnet. Global configuration alone is not proof of tested multi-node operation.

Central defaults to one Local service routing six logical IDs. For separately named Local services, repeated mappings configure Central's destinations:

```sh
./central_controller -g --headless \
  --local-endpoint I1=traffic_local_I1 \
  --local-endpoint I2=traffic_local_I2
```

Unmapped IDs retain the default route. The Local owner must publish the selected services and correct IDs. Central does not create six Local state machines or rename another person's services. Agree a complete node/service map before the demonstration. The v1 header does not authenticate which process owns an intersection ID.

For a separate display, start the core on its QNX node:

```sh
./central_controller -l --headless --schedule config/daily_schedule.example
```

In another terminal on that node:

```sh
./central_ui
```

The private default UI service is `traffic_central_controller_ui`. `central_ui -n name` selects another local UI service; `--no-color` disables color. `quit` closes the separate UI, while `shutdown` asks the core to stop. Closing the UI or its terminal leaves a headless core running. The embedded console remains available without `--headless`. Build/transfer both executables and the chosen configuration file using [README.md](README.md).

## Operator requests and schedules

| Operator input | Meaning |
| --- | --- |
| `mode-fixed I1`, `mode-sensor all` | Persistent high-level operator mode request. |
| `mode-temp I1 sensor 30` | Temporary request; Local owns activation, timing and reversion. |
| `mode-revert I1` | Cancels temporary mode; preserves persistent operator intent. |
| `coordinate all NS 0` | Per-target phase/offset requests with separate receipts. |
| `coordinate-at 10 all NS 0` | Becomes eligible for dispatch at Central after 10 monotonic seconds; it does not synchronize Local application. |
| `schedule` | Shows loaded entries and Central operator-policy information. |
| `schedule-resume I1` | Explicitly releases Central's hold so the current schedule may be requested once the target is ready. |
| `version` | Shows build label and compilation stamp for deployment checks. |

Daily files are loaded once at startup with `--schedule path`. A row is `HH:MM fixed|sensor I1..I6|all`, using the QNX target's local clock/timezone. The parser accepts up to 32 entries and 1024 physical lines and rejects overlapping same-time rows affecting the same target. File order is irrelevant: each target selects its most recent applicable daily row, wrapping to the previous day before the first row. A target with no row gets no invented default. See [daily_schedule.example](config/daily_schedule.example).

Persistent operator intent takes precedence over scheduling. A temporary operator mode suppresses scheduling using a monotonic deadline; an earlier persistent override remains after that temporary interval. Ordinary `mode-revert` preserves that persistent intent. Rejected or uncertain operator requests conservatively hold automatic changes until `schedule-resume`. Receipt-based Central suppression does not determine Local's actual temporary-mode start/end; the group must agree that contract.

Scheduling requests a new desired mode instead of replaying a command every tick. Uncertain requests are not automatically retried after reconnect. If an unchanged scheduled intent must be requested again after investigation, use `schedule-resume` deliberately. Wall-clock changes may change the selected daily row. Temporary intervals, data age, queue lifetime and dispatch delays use the monotonic clock; no cross-node clock accuracy is guaranteed.

`all` expands to individual messages with separate IDs and readiness checks, so receipt is neither atomic nor simultaneous. A request's five-second queue allowance starts when it becomes eligible for dispatch: `coordinate-at 10 ...` waits ten intentional seconds and then has five seconds to begin transmission. Late unsent intent expires.

Train simulation commands have a separate queue and strict allowlist:

```text
train-cmd train-up
train-cmd train-down
train-cmd train P1 up
train-cmd noexit P2 down
train-cmd stuck P3
train-cmd reset P1
train-cmd test
train-cmd test 1
train-cmd scale 10
train-cmd status
```

Valid crossings are P1-P3, directions `up`/`down`, tests 1-7 and scale 1-100. These are simulator event/test requests. Arbitrary raw strings are rejected. Train retains responsibility for safe reset, simulator events and command results.

The parser recognizes the existing `p#-fault` syntax, but Central deliberately refuses to send it to the current Train peer. That remote handler holds Train's state mutex before invoking fault handling, whose connection check tries to lock the same mutex again. The resulting deadlock must be fixed by the Train owner. Until then, use the Train console for direct `p1-fault`, or request `train-cmd stuck P1` followed by `train-cmd train P1 up` to exercise a stuck-gate scenario through the simulator. Central prints `Not sent` for the blocked direct remote fault command.

## Telemetry and timing agreement

Local should publish a snapshot whenever a light changes, as required by the assignment. The proposed integration contract also repeats every owned intersection's current snapshot at least once per second. That repeat period is a design assumption to agree and measure, not a number supplied by the assignment. Heartbeats alone cannot show that phase or status generation is progressing.

Central uses one-second heartbeat releases, three consecutive missed probes for link-down, a 500 ms caller wait per peer operation, a five-second queue allowance and a default five-second status-age limit configurable with `-s 1..60`. Scheduling/network delay means these are settings, not measured worst-case bounds. Readiness is checked again before transmission. Missing data remains waiting/unknown, and old snapshots retain an age and stale/offline indication.

The shared sender's all-zero heartbeat is ambiguous: `{sender_id=0, healthy=0, sequence=0}` means unspecified legacy health to Central, not an explicit I1/P1 degradation. It cannot clear an earlier typed degradation. Typed degraded I1/P1 reports need a nonzero sequence, including at wrap. This convention provides no session or replay protection.

Current Train status does not publish independent up/down track occupancy, the train STOP signal or flashing-light phase. Central labels unavailable details unreported rather than inferring them from gates or ACKs. Retained fault alerts and current status snapshots are separate observations; agree explicit fault-clear reporting with Train.

## Remaining peer-owned work

| Owner | Handoff or review |
| --- | --- |
| Local | Publish real status after changes and agreed refresh; safely handle mode/temp/revert/coordination; echo IDs; continue phases, railway protection and temporary expiry while Central is offline. |
| Train | Return simulator results/BUSY instead of unconditional success. Add train STOP, track and flash reports only via an agreed protocol change if required. |
| Train | Move blocking outbound `MsgSend` work away from simulator/state-machine callbacks using a bounded handoff. Current callbacks can wait on Central/Local. |
| Train | Fix the remote `p#-fault` self-deadlock caused by re-locking the same state mutex in a callback. Synchronize simulator state shared by tick, console and remote-command paths; Central's separate queue cannot repair internal races. |
| Train + Local | Map P1 to I1/I2, P2 to I3/I4 and P3 to I5/I6. Current Train preemption uses its crossing ID in an intersection-ID field, which does not establish delivery to both affected intersections. |
| Group | Agree node/service ownership, frame dialects and ID bases, telemetry timing, temporary baseline/activation, recovery and demonstration scenarios. |
| Thao, for optional override/display | Define request meaning/reasons, target display, expiry, reply/application correlation, duplicates, restart and stale-display behavior before enabling those routes. |

A future protocol should negotiate version/capabilities, identify sender sessions and per-target status sequences, and distinguish RECEIVED/QUEUED/APPLIED/REJECTED/BUSY with command IDs. True synchronized coordination also needs a common activation epoch/timebase, clock accuracy, late-arrival behavior and cancellation. Existing fields must not be silently repurposed to imply these guarantees.

Suggested message to Thao:

> T đã làm phía Central: hiển thị trạng thái thật, lệnh fixed/sensor/temp/revert, coordination, lịch giờ và UI riêng. M gửi t phần Local đang hỗ trợ, tên service/ID và format status nhé. Local cần gửi trạng thái mỗi khi đèn đổi, tự chuyển pha an toàn và tự hết hạn temp kể cả Central mất kết nối; ACK phải echo command_id. Phần Local gửi override hoặc Central gửi display xuống Local không bắt buộc riêng trong đề, nên nếu m đang làm thì chốt giúp t flow, ý nghĩa và timeout trước khi ghép. T giữ nguyên code của m.

Suggested message to the Train owner:

> Central đã nhận frame compact bên Train và đổi ID P1-P3 ở phía Central; có train-cmd để chuyển các lệnh simulator đã kiểm tra. T đang chặn gửi p#-fault từ Central vì remote handler khóa mutex rồi callback khóa lại chính mutex đó, dễ deadlock; tạm dùng console Train hoặc test stuck gate. Bên m giúp sửa chỗ này và trả đúng kết quả/BUSY thay vì luôn ACK thành công; chốt fault-clear và train STOP/track/flash nếu cần hiển thị. Cần kiểm tra callback IPC bị chặn, đồng bộ simulator và mapping một crossing tới hai intersection. T không sửa code Train, sẽ ghép/test theo contract mình thống nhất.

## Real-time evidence required

Elapsed time and timed condition waits must use the same monotonic clock; QNX condition variables otherwise default to the system clock. Central uses absolute heartbeat releases, skips missed periods and keeps blocking peer/file I/O outside monitor critical sections. These decisions reduce timing interference but do not establish a worst-case response time. [QNX condition-variable clocks](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_condattr_setclock.html).

QNX timeouts must cover SEND-blocked and REPLY-blocked states. With `_NTO_CHF_UNBLOCK`, a server that already accepted a message may need to reply after an unblock pulse before the kernel sender is released. Central isolates outstanding operations and retains their buffers, but cannot force a stopped peer to finish. A blocked peer or filesystem can still delay process termination. [QNX kernel message timeouts](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.getting_started/topic/s1_timer_Kernel_timeouts_with_messages.html).

Priority inheritance helps with inversion, but long critical sections and unsuitable task priorities still need analysis. Choose priorities across Central, Local and Train using deadlines and measured workloads. Raising Central's priority alone is not a schedulability argument. [QNX priority-inversion guidance](https://qnx.com/developers/docs/7.1/com.qnx.doc.ide.userguide/topic/detecting_priority_inversion.html).

After fixtures, test actual peers on intended nodes: both startup orders, stale/recovery, one failed endpoint, flood/slow replies, independent UI exit, temporary expiry with Central offline, trains on both tracks, gate fault/clear and red train signal, and correct affected intersection pairs. Record receive-to-ACK latency, queue delay, heartbeat jitter, status age, lock hold times, CPU load and real network behavior. [VALIDATION.md](VALIDATION.md) distinguishes completed observations from pending scenarios. Fixture passes do not complete peer state machines or prove a hard real-time guarantee.
