# Central controller

Central displays the latest reports for intersections I1–I6 and crossings P1–P3, records fault events, and sends mode and coordination requests to Local. It uses the message layouts in `../common/protocol.h` (version 1).

## Build and run

With a QNX SDP 7.1 environment configured, run from the repository root:

```sh
make -C central_controller all PLATFORM=x86_64
```

On QNX:

```sh
./central_controller -l
./central_controller -g -o /tmp/central_controller.log
./central_controller -l -s 5
```

`-l` uses local service names on one machine. `-g` is the default and requires GNS to be configured between the machines. The event log defaults to `/tmp/central_controller.log`. `-s` sets the maximum status age from 1 to 60 seconds (default 5). This is a configurable supervisory policy, not a proven hard real-time deadline. Agree the reporting interval with Local/Train; a proposed interval is one report per intersection/crossing per second, plus state changes.

## Operator commands

```text
mode-fixed [I1..I6|all]
mode-sensor [I1..I6|all]
mode-temp <I1..I6|all> <fixed|sensor> <seconds>
mode-revert <I1..I6|all>
coordinate <I1..I6|all> <NS|EW> <offset 0..43>
status
commands
faults
events
watch
help
quit
```

An omitted target means all six intersections. Temporary durations are 1–65535 seconds. The coordination offset is within the 44-second fixed cycle defined by the shared timing constants. Local is responsible for applying requests through its state machine, enforcing railway preemption, and reverting temporary modes.

The default interface keeps command results on screen. `status` prints one snapshot; `watch` refreshes a live view once per second until Enter returns to the prompt. While watching, the next input line exits the live view and is not executed as a command. `events` shows the last eight events and logging failures/dropped records.

Commands require a connected Local link, recent contact, a status report younger than the configured age, and no explicit degraded-health report for that intersection. An `all` request is queued only when every target is ready; each intersection then receives its own command ID and reply. Execution across intersections is not atomic or simultaneous.

The queue holds 32 requests and history retains 64. Unsent requests expire after five seconds; readiness and age are checked again before dispatch. Expiration is recorded when the dispatcher examines the request, not by a separate timer exactly at the deadline. A lost link or shutdown cancels queued requests. New requests preserve FIFO order; neither a newer operator request nor `mode-revert` silently replaces earlier queued work.

`commands` shows QUEUED, SENDING, ACCEPTED, REJECTED, UNCONFIRMED, CANCELED, or EXPIRED, together with the requested mode/duration/coordination, queue wait, IPC duration, and outcome detail. Logs include the request description and lifecycle timestamps. ACCEPTED means Local returned success with the matching command ID. It does not confirm that the requested mode has been applied. A timeout or invalid reply leaves the command UNCONFIRMED; Central does not automatically resend it. Version 1 supplies no rejection reason or applied-command acknowledgement. Inspect subsequent status reports before deciding whether to issue another request.

## Monitoring and communication

Local and Train have separate communication workers. Heartbeats use absolute monotonic one-second release times; missed releases are skipped without a catch-up burst. Pending commands wake the Local worker immediately instead of waiting for the next heartbeat. A due heartbeat takes precedence over a new command, although an already-started send can delay it by its application wait (nominally at most 500 ms, plus scheduling delay). Three failed probes close a link. Contact becomes offline after three seconds without a successful heartbeat or status report. Old reports remain visible with OFFLINE, WAITING UPDATE, or STALE markers. A recovered peer must send a fresh report before its cached intersection state can authorize commands. Remaining times are the values reported by the peer, not a live countdown.

Typed heartbeat health is tracked per intersection/crossing. A reported degradation persists until an explicit healthy heartbeat for that ID; probe replies and new status alone do not clear it. Existing demo peers send all-zero heartbeat payloads, which Central treats as unspecified health for compatibility. A typed degraded I1/P1 heartbeat must use a nonzero sequence to distinguish it from the legacy empty payload, including at sequence wrap. ONLINE indicates contact, not proof that the peer's state machine is healthy.

The application waits at most 500 ms for each connection or send operation. Legacy QNX receivers can leave a sender blocked after receiving its message. A private worker contains that outstanding operation, so the other peer and the operator interface can continue. That link cannot start another operation until the kernel releases the pending request.

If a legacy receiver remains stopped while holding a request, Central can finish its application cleanup but QNX can postpone process termination until that receiver replies or terminates. Central prints a pending-IPC message when this happens. Resuming or restarting the affected peer releases the request. This limitation cannot be removed by a Central-side timeout; the peer receiver must handle unblock requests correctly or use a channel that permits sender timeouts.

Logging runs outside the shared-state lock on a separate worker. A blocked filesystem can still delay logger shutdown; a finite whole-process exit time is not guaranteed for arbitrary `-o` destinations. Target timing measurements and the Local/Train task schedule are required before claiming hard real-time deadlines.

`faults` retains the latest alert for each intersection or crossing until a `FAULT_NONE` clear alert arrives. Railway status reports show the crossing's currently reported fault separately.

Messages carry the existing typed payload inside `test_message_t.data`; receivers reply with `reply_t` and QNX reply status zero. Mode and coordination acknowledgements must echo the command ID. Central validates frame sizes, routes, IDs, enums, and replies. Version 1 has no session identifier, status sequence number, status-query message, or applied-command acknowledgement, so it cannot distinguish a delayed valid report from a new report after a peer restart.

The existing Local and Train demonstration programs provide heartbeat/test traffic. Their application handlers must publish the typed status reports and implement the command behavior before end-to-end traffic operation can be exercised. Central displays unavailable data as waiting for status.

The declared `MSG_OVERRIDE_REQUEST` and `MSG_DISPLAY_UPDATE` flows remain pending agreement with their implementer. Central does not automatically approve an unspecified override. See [INTEGRATION.md](INTEGRATION.md) for exact wire examples, Local ownership of safety and temporary expiry, QNX scheduling/IPC references, and the decisions to settle with Thao.

## Tests

Build with QNX SDP, then run the generated binaries on QNX:

```sh
make -C central_controller/tests all PLATFORM=x86_64
cd central_controller/build/tests-x86_64
./commands_test
./monitor_test
./ipc_test
./integration_test ./central_fixture
```

The integration fixture uses dedicated test service names. Tests exercise parsing, report freshness and recovery, legacy message exchanges, command acknowledgements, malformed frames, timeouts, and shutdown. Stopped-peer scenarios verify continued operator/Train responsiveness and the pending-IPC notification, then resume the test peer to verify process exit. They use simulated peer behavior and do not validate the Local or Train state machines or physical hardware.

Integration regressions also cover stable CLI input, opt-in live view, successful six-target dispatch, complete temporary/revert/coordination payloads, request descriptions, queue overload and expiry, and heartbeat service during command floods. See [VALIDATION.md](VALIDATION.md) for the recorded target results and remaining limits.

An additional `global_transport_test` executable provides a manual two-node GNS transport smoke test. Configure GNS using the group's normal procedure first (`gns -s` on one node, `gns -c <server-Qnet-node-name>` on the other). Copy this executable to both QNX nodes, choose a unique test service, then start the server and client within 20 seconds:

```sh
# QNX node A
./global_transport_test server traffic_test_global_demo_20260912
# QNX node B, in a separate terminal
./global_transport_test client traffic_test_global_demo_20260912
```

Both must report PASS. This exercises global registration/lookup, heartbeat, matching command ACKs and rejection across nodes. It does not exercise the complete Central application or real Local/Train behavior. The `run` make target runs only the four local suites; it does not configure GNS or launch this two-node test.
