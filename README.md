# Traffic Light Real-Time System

## Running the Controllers

### Single VM Testing (Local Mode)

```bash
# Terminal 1
/tmp/central_controller -l

# Terminal 2
/tmp/local_intersection_controller -l

# Terminal 3
/tmp/train_controller -l
```

### Multi-VM with GNS (Global Mode)

```bash
# On Central VM - start GNS server first
/system/xbin/gns -s &
/tmp/central_controller -g

# On other VMs - start GNS client first
gns -c /net/vm3-central-controller/dev/name/gns &
/tmp/local_intersection_controller -g
/tmp/train_controller -g
```

## Building

Run with the QNX SDP environment loaded:

```bash
make -C central_controller
make -C local_intersection_controller
make -C train_controller
```

The executables are written to each controller's `build/x86_64-debug` directory.
Build and deploy all controllers with the same `common/protocol.h` version.

## Commands (Central Controller)

Targets are `I1` through `I6`, or `all`. Omitting the target for `mode-fixed`
or `mode-sensor` selects all six intersections.

| Command | Description |
|---------|-------------|
| `mode-fixed [target]` | Request fixed timing |
| `mode-sensor [target]` | Request sensor timing |
| `mode-temp target fixed\|sensor seconds` | Request a temporary mode, with expiry handled by Local |
| `mode-revert target` | Cancel a temporary mode |
| `coordinate target NS\|EW offset` | Request fixed-cycle coordination with an offset from 0 to 43 seconds |
| `override target NS\|EW seconds` | Request a temporary legal movement, subject to Local safety checks |
| `release target` | Release a movement override |
| `status` | Display six intersections and three railway crossings |
| `commands` | Display command IDs and their outcomes |
| `faults` | Display active fault reports |
| `help` | Display command usage |
| `quit` | Stop workers and exit |

Central requires a current Local snapshot and a healthy link before queuing
commands. Each target receives its own command ID. `ACCEPTED` means that Local
accepted the request; `APPLIED` requires an explicit reply or status report.
`UNCONFIRMED` means the outcome is unknown after a communication error; Central
does not resend that command automatically.

Event logs default to `/tmp/central_controller.log`. Use `-o path` to select a
different file. The display retains the last reported state with its age when
a controller becomes unavailable.

## Controller Interfaces

Messages use a versioned header followed by a typed payload. Initialize them with
`protocol_init_message()` and send them with `send_message()`. The transport
validates the message length, routing, payload fields and matching reply ID.

Local supplies `MSG_STATUS_UPDATE` for each intersection and handles
`MSG_STATUS_REQUEST`. A snapshot reply uses `REPLY_APPLIED`, type
`MSG_STATUS_UPDATE`, the requested ID, and the current session/sequence.
Train supplies the corresponding `MSG_RAILWAY_STATUS` snapshot for each crossing.
Sequences increase within a controller session; a restart changes the session ID.
Local command reports echo both `command_id` and the issuing Central's
`command_session_id`.

The Local and Train executables currently support test messages and heartbeats.
Their traffic-state and railway-state handlers remain to be implemented; unsupported
requests receive a rejection. Central displays unavailable snapshots explicitly.

## Tests

```bash
make -C tests
```

Run the test executables on a QNX target. Protocol/monitor tests cover framing,
health, stale state, reconnection and command outcomes. Transport tests use their
own services to check ACK/NACK, malformed messages, timeout and shutdown behavior.
