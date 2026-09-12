# Central controller

Central monitors reports for I1–I6 and P1–P3, records faults, and sends high-level
Local requests and Train simulator commands. Local owns lamp sequencing and safety
interlocks; Train owns gates. Integration changes stay in this directory; shared
protocol and teammate source files are unchanged.

## Build and open

In a shell configured for QNX SDP 7.1, from the repository root:

```sh
make -C central_controller all PLATFORM=x86_64
make -C central_controller all PLATFORM=x86_64 BUILD_PROFILE=release
```

Copy **both** `central_controller` and `central_ui` from
`central_controller/build/x86_64-debug/` (or `x86_64-release/`) to QNX. These
executables do not run directly on Windows. In Momentics, an existing local copy
with project metadata can be imported as a project. For a fresh clone without
that ignored IDE metadata, create a QNX C Makefile project using this directory
and its existing Makefile. Build for QNX x86_64 and create launch configurations
for both executables. The default make target builds both programs.

Open two terminals on the Central QNX node:

```sh
# Terminal 1: core stays alive when stdin closes.
./central_controller -l --headless

# Terminal 2: separate display and operator process.
./central_ui
```

`status` shows a snapshot; `watch` refreshes every second, and Enter leaves watch.
`quit` closes only the display. Start `central_ui` again to reconnect. `shutdown`
requests core shutdown. Use the group's normal process supervisor if the core
must survive terminal signals as well. Colors are automatic on terminals;
`--no-color`, `NO_COLOR` or redirected output disables them. Each dashboard shows
its build stamp. Scripted requests are supported:

```sh
./central_ui -c status
./central_ui -c 'mode-fixed I1'
```

Invalid/unavailable requests return a nonzero UI exit status. A successful
enqueue is still distinct from peer receipt and application; inspect `commands`.
For the previous embedded console, `./central_controller -l` still works; its
`quit` and stdin EOF stop the core.

For multiple QNX nodes, configure Qnet/GNS first, use `-g` on controllers and run
`central_ui` on the **same node as Central**. Its private IPC stays local. `-g`
is Central's default:

```sh
./central_controller -g --headless -s 5 -o /tmp/central_controller.log
```

`-s` selects freshness from 1–60 seconds, default 5, as a supervisory policy.
Agree periodic telemetry and state-change reporting with peers. The default log
is `/tmp/central_controller.log`; select a writable persistent path when needed.

## Operator commands

| Command | Purpose |
| --- | --- |
| `mode-fixed [I1..I6\|all]` | Request fixed timing; omitted target means all |
| `mode-sensor [I1..I6\|all]` | Request sensor-driven operation |
| `mode-temp I1 fixed 20` | Temporary mode, 1–65535 seconds |
| `mode-revert I1` | Cancel temporary mode, preserving persistent operator intent |
| `coordinate I1 NS 3` | Request NS/EW coordination with offset 0–43 seconds |
| `coordinate-at 10 all NS 3` | Dispatch coordination after 1–3600 seconds |
| `train-cmd train-up` | Simulate W→E through P3→P2→P1 |
| `train-cmd train-down` | Simulate E→W through P1→P2→P3 |
| `train-cmd train P1 up` | Simulate one crossing/direction; `down` also supported |
| `train-cmd noexit P1 up` | Simulate missing train exit |
| `train-cmd stuck P1` | Simulate a stuck gate on its next movement |
| `train-cmd reset P1` | Request the Train simulator's reset behavior |
| `train-cmd test` / `train-cmd test 1` | Invoke existing Train tests, number 1–7 |
| `train-cmd scale 5` | Simulator time scale, 1–100 |
| `train-cmd status` | Train processes status; current peer discards its text result |
| `status`, `commands`, `faults`, `events` | Reports, history, latched alerts, recent events |
| `schedule`, `schedule-resume I1` | Inspect daily policy or release operator hold |
| `version`, `help`, `watch` | Build stamp, help, live display |

Local targets also accept `all`; Train crossings are P1–P3. `train-up` simulates
a train movement, **not gate opening**. Strings are strictly parsed and use an
independent Train queue. Arbitrary strings and lamp/gate safety bypasses are absent.

Remote `p#-fault` is blocked: the current Train handler holds a mutex that its fault
callback locks again. Use direct fault injection in the Train console, or `stuck
P1` followed by `train P1 up` to trigger a gate fault through its tick thread.
See [INTEGRATION.md](INTEGRATION.md) for the exact peer issue.

`ACCEPTED` means a valid receipt reply, not successful application. Current Train
replies success even when its simulator says BUSY and discards the result string;
Central labels this limitation. A timeout/invalid reply is `UNCONFIRMED` and is
never automatically resent. Subsequent status remains the source of reported
operation; it has no applied-command ID.

Queues hold 32 requests **per endpoint**, with 64 shared history records. Broadcast
admission checks every target/capacity first; subsequent execution can differ.
Immediately eligible requests preserve order. Future coordination lets ready
requests pass; its five-second queue allowance starts at planned dispatch time.
Other requests get five seconds from enqueue. Eligibility/age are rechecked before
sending. V1 has no shared activation epoch, so `coordinate-at` does not guarantee
simultaneous Local application.

## Optional daily policy and endpoint routing

Copy [config/daily_schedule.example](config/daily_schedule.example) to QNX:

```sh
./central_controller -l --headless --schedule ./daily_schedule.example
```

Each entry is `HH:MM fixed|sensor I1..I6|all`; comments and blank lines are allowed.
At most 32 entries are validated once at startup. The latest applicable daily
entry is selected using the QNX node's local clock/timezone, wrapping to the
previous day. A separate monotonic 200 ms policy task does no periodic file I/O.

Operator mode requests use priority 2; automatic requests use priority 1. Operator
commands cancel queued automatic requests for their targets. Persistent operator
intent suppresses scheduling. Temporary suppression starts at receipt; Local
must enforce its own application/expiry. Rejected, expired or uncertain operator
requests hold automation until `schedule-resume`. Resuming requests the current
scheduled mode when fresh Local status is available. Reconnection alone never
automatically replays a scheduled request.

Default routing uses one Local service for all logical IDs. Separate Local
services can be mapped without changing their source here:

```sh
./central_controller -g --headless \
  --local-endpoint I1=intersection_1 \
  --local-endpoint I2=intersection_2
```

Unspecified targets keep the default service. Repeated names share a worker and
queue; each endpoint has its own probe state. Reports must contain correct logical
IDs. Routing support does not create six functioning Locals or authenticate IDs.

## Report and timing limits

Legacy 104-byte frames retain zero-based IDs. Exact compact Train status/fault
frames normalize its current one-based IDs inside Central. Compact Local
status/fault and typed heartbeat retain protocol IDs. Invalid sizes, routes,
enums and strings are rejected. No shared header changes are required.

OPEN/CLOSING/CLOSED/OPENING/FAULT and descriptive faults are received telemetry.
Approach signals, flashing lights and individual tracks are **NOT REPORTED** by
v1. Alerts remain latched until explicit `FAULT_NONE`; current status faults are
shown separately. Reset status alone does not erase a historical alert.

Contact, reported health and data age are distinct. Old data stays visible with
OFFLINE/WAITING UPDATE/STALE markers. Remaining times are snapshots; all-zero
heartbeats make no health assertion. Commands need a connected, recently probed
endpoint and fresh Local status without explicitly degraded health.

Absolute monotonic heartbeats run every second; three missed probes close a link.
IPC callers wait nominally up to 500 ms. Legacy peers that ignore QNX unblock
requests can retain a kernel send; an isolated worker contains it, but process
exit may wait for that peer to release it. A blocked filesystem can delay logger
shutdown. These policies and VM observations are not whole-system hard real-time
guarantees. [IMPLEMENTATION_NOTE.md](IMPLEMENTATION_NOTE.md) covers architecture,
assignment mapping and QNX references.

## Tests

Build on the host, then run generated binaries on QNX:

```sh
make -C central_controller/tests all PLATFORM=x86_64
# On QNX, with test binaries copied to one directory:
./commands_test
./monitor_test
./ipc_test
./operator_policy_test
./ui_ipc_test
./integration_test ./central_fixture
./central_features_test ./central_fixture
./real_train_test ./central_fixture ./real_train_fixture
```

Run application fixture suites sequentially: dedicated test service names are
shared between them. Fixtures clean up their own processes/tempfiles. Actual
results and limits are recorded in [VALIDATION.md](VALIDATION.md).
The real Train fixture compiles the current teammate sources unchanged, using
only a Central-owned wrapper to select isolated test service names.

After configuring GNS normally, test two-node transport separately using a unique
name; launch both ends within 20 seconds:

```sh
# QNX node A
./global_transport_test server traffic_test_global_unique
# QNX node B
./global_transport_test client traffic_test_global_unique
```

Both ends must report PASS. This transport check is separate from a complete
multi-node Local/Train demonstration.
