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
```

`-l` uses local service names on one machine. `-g` is the default and requires GNS to be configured between the machines. The event log defaults to `/tmp/central_controller.log`.

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
help
quit
```

An omitted target means all six intersections. Temporary durations are 1–65535 seconds. The coordination offset is within the 44-second fixed cycle defined by the shared timing constants. Local is responsible for applying requests through its state machine, enforcing railway preemption, and reverting temporary modes.

Commands require a connected Local link, recent contact, and a status report less than 60 seconds old for each target. An `all` request is queued only when every target is ready; each intersection then receives its own command ID and reply. Execution across intersections is not atomic.

`commands` shows QUEUED, SENDING, ACCEPTED, REJECTED, or UNCONFIRMED. ACCEPTED means Local returned success with the matching command ID. It does not confirm that the requested mode has been applied. A timeout or invalid reply leaves the command UNCONFIRMED; Central does not automatically resend it. Inspect subsequent status reports before deciding whether to issue another request.

## Monitoring and communication

Local and Train have separate communication workers. Heartbeats run every second; three failed probes close a link. Contact becomes offline after three seconds without a successful heartbeat or status report. Old reports remain visible with OFFLINE, WAITING UPDATE, or STALE markers. A recovered peer must send a fresh report before its cached intersection state can authorize commands. Remaining times are the values reported by the peer, not a live countdown.

The application waits at most 500 ms for each connection or send operation. Legacy QNX receivers can leave a sender blocked after receiving its message. A private worker contains that outstanding operation, so the other peer and the operator interface can continue. That link cannot start another operation until the kernel releases the pending request.

If a legacy receiver remains stopped while holding a request, Central can finish its application cleanup but QNX can postpone process termination until that receiver replies or terminates. Central prints a pending-IPC message when this happens. Resuming or restarting the affected peer releases the request. This limitation cannot be removed by a Central-side timeout; the peer receiver must handle unblock requests correctly or use a channel that permits sender timeouts.

`faults` retains the latest alert for each intersection or crossing until a `FAULT_NONE` clear alert arrives. Railway status reports show the crossing's currently reported fault separately.

Messages carry the existing typed payload inside `test_message_t.data`; receivers reply with `reply_t` and QNX reply status zero. Mode and coordination acknowledgements must echo the command ID. Central validates frame sizes, routes, IDs, enums, and replies. Version 1 has no session identifier, status sequence number, status-query message, or applied-command acknowledgement, so it cannot distinguish a delayed valid report from a new report after a peer restart.

The existing Local and Train demonstration programs provide heartbeat/test traffic. Their application handlers must publish the typed status reports and implement the command behavior before end-to-end traffic operation can be exercised. Central displays unavailable data as waiting for status.

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
