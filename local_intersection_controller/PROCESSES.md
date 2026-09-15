# Local process deployment

Build with `make -C local_intersection_controller all` using QNX SDP.
The same executable supports separate process roles, selected by `--role`:

| Role | Owns | Service |
| --- | --- | --- |
| core | Lamp state, timing, local event arbitration | `<service>_core`, local namespace only |
| comm | Central/Train network connections and forwarding | `<service>`, local or global namespace |
| io | Console sensor/pedestrian/demo input | Calls core using local QNX IPC |
| display | Terminal output from current snapshots | Calls core using local QNX IPC |
| ui | Interactive viewing and input with target selection | Calls core using local QNX IPC |

All roles for an intersection run on the same QNX node and use the same
`-i` and optional `-n` arguments. Central and Train can run on other nodes.
Default public service names remain `traffic_local_I1` through `traffic_local_I6`
when `-i` is supplied. Explicit roles without `-i` retain the legacy I1 name.

## One-command Local startup

The single-intersection screen keeps the original console layout: service and
connections, received-message timestamps, timing, vehicle lights, pedestrians,
sensors and train state. The all view uses six compact sections with the same
headings, bracketed values and separator lines. Both display and interactive UI
share `src/local_panels.c`. Only active WALK countdowns are shown; STOP and red
lamp future durations are not predicted across adaptive timing changes.
The private core reply now includes UI metadata; restart all Local roles using
the same rebuilt binary. The shared Central/Train wire protocol is unchanged.

```sh
/tmp/local_intersection_controller -l
```

With no role, ID or custom service, the launcher starts six cores, six comm
processes and the interactive UI. Central and Train are separate applications.
Use `view I3` to select I3, or `view all` to view all six.
In launcher mode, `q`, EOF or Ctrl+C stops the children started by this launcher.
Existing standard Local services cause startup to fail without stopping them.
Startup failures clean up children already started. The launcher does not restart
children that fail later; an unavailable core appears offline in the UI.
The launcher's implementation is in `src/local_launcher.c`.

Explicit `--role ui` still attaches without starting or stopping controllers.
Supplying `-i` or `-n` without a role still starts only one core.

From `local_intersection_controller/build/x86_64-debug`, use separate terminals:

```sh
./local_intersection_controller -l -i I1 --role core
./local_intersection_controller -l -i I1 --role comm
./local_intersection_controller -l -i I1 --role display
./local_intersection_controller -l -i I1 --role io
```

To view all six intersections in one terminal on the intersections node:

```sh
./local_intersection_controller --role display --all
```

The table reads each core directly, so Central and comm are not required for
local viewing. Start cores with `-i I1` through `-i I6` and the standard service
names. A missing or unresponsive core shows `OFFLINE`, never an old lamp value.
Custom `-n` service names remain available in the single-intersection view;
`--all` cannot be combined with `-i` or `-n`. Snapshots are read sequentially,
so the rows are a live overview, not an exactly simultaneous timing measurement.

## Interactive UI

With the cores already running, open one terminal:

```sh
./local_intersection_controller -l --role ui
```

It starts in the all-intersections overview. Enter `view I3` to select I3,
`view I5` to select I5, or `view all` to return to the overview. The selected
intersection is printed above the table. Commands `m n e x z 1 2 p o l c r`
are sent only to the selected core. The all view rejects control commands until
an intersection is selected. Offline or mismatched core IDs block input delivery.
Unacknowledged input is never automatically retried.

Use `--role ui -i I3` to start on I3. A custom `-n service` applies to that
intersection; other views use standard names. For legacy I1, start the UI with
`--role ui -n traffic_local_controller`. Without `-i`/`-n`, all six slots use
`traffic_local_I#_core` and do not automatically discover legacy/custom services.

The UI redraws the typed line during refresh, supports Backspace and Enter,
and restores terminal input settings on normal exit, Ctrl-C or SIGTERM.
`q`, `quit`, `exit`, or Ctrl-D on an empty line closes only the UI; switching
views neither starts nor restarts a core. The read-only display and separate
io roles remain available. A shared UI does not require Central or comm.

Repeat with I2 through I6. Alternatively, `sh local_intersection_controller/start_locals.sh -l`
starts all six core/comm pairs; attach display/input processes separately.
Use `-g` on comm for GNS deployment. No public Central protocol changes are needed.
The Train controller still needs its separate six-endpoint routing work.

Unlike the initial report's shared support processes, this implementation uses
one comm process per intersection. This keeps a comm process failure isolated to
one intersection. Input/display can be started or stopped independently.
Core never calls network send/open or writes periodic terminal output.
Its monotonic tick runs in the same event loop that applies incoming commands.
Communication calls have 250 ms send/reply timeouts. A failed input request is
not retried automatically because it may already have been applied.
Closing input with `q`, EOF, or closing a display does not stop the controller.
Use SIGINT/SIGTERM to stop each role. Display clears old lamp data if core is unavailable.
Core keeps a bounded 64-record status history for comm to drain after state changes.
Comm acknowledges a record only after Central accepts it. On reconnection it
starts with current state; overflow overwrites the oldest records rather than
blocking lamp control. Sequence numbers let Central observe discontinuities.

## Timing contract

The baseline is 20 + 2 + 20 + 2 = 44 seconds. Sensor demand is sampled once for
each complete pair of Green phases, anchored at the configured initial phase.
The resulting 25/15, 15/25 or 20/20 allocation is retained until the next pair.
Later sensor changes cannot independently extend the other half of that pair.

Pedestrian behavior remains automatic WALK, starting 1 second after compatible
Green and ending 5 seconds before Yellow. `ped_NS` is compatible with EW Green.
A button still caps the **compatible** Green and transfers removed seconds to
the next opposite Green; it does not switch to a different button interpretation.
Transfers reserve capacity under the 30-second Green limit instead of silently
discarding seconds. If there is insufficient receiving capacity, only the
transferable amount is removed. Pending requests do not add duplicate transfers.

As selected by the user, a transfer from the second phase may cross a cycle
boundary: individual cycles can be shorter/longer than 44 seconds. For example,
a 9-second transfer gives 35 seconds then 53 seconds, totaling 88 seconds.
It is incorrect to claim every pedestrian-adjusted cycle is exactly 44 seconds.
Railway preemption, reset and explicit coordination restart the timing plan;
they are not counted as uninterrupted normal cycles.

## QNX acceptance checks

1. Run all six pairs, attach Central and verify each endpoint reports its own ID.
2. Close io/display and stop comm for I1; I1 core and all other cores keep ticking.
3. Restart comm and verify Central receives a fresh snapshot before commanding it.
4. Stop I1 core: its display must show unavailable, comm must not report healthy
   heartbeats for it, and other intersections must continue.
5. Run `make -C local_intersection_controller/tests run` on QNX; capture sensor
   reversal, pedestrian transfer and railway recovery timing with monotonic logs.

Cross-compilation verifies buildability, not these runtime/IPC properties.

## Reading the code

Start with `local_intersection_controller.c` (arguments), then follow the role:

| File | Responsibility |
| --- | --- |
| local_core.c | Core event loop and bounded status history |
| local_comm.c | Network forwarding, status and heartbeat reporting |
| local_ui.c | Single/all-intersection display and console input |
| local_console.c | Interactive UI event loop and selected-target commands |
| local_ui_commands.c | Small view-command parser |
| local_process.c / .h | Shared QNX IPC helpers and private message format |
| local_commands.c | Small switch for demo inputs; no network or terminal I/O |
| local_ipc.c | Validate and apply messages received by core |
| local_traffic.c | Vehicle phase timing, cycle allocation and railway hold |
| local_pedestrian.c | WALK/STOP window and button time transfer |
| local_simulation.c | Simulated time, cars and demo resets |
| local_state.c / local_controller.h | State, initialization and telemetry |
| local_config.c | The six startup profiles |

The old `local_threads.c` and legacy in-core display path are removed. Only core
initializes lamp state. Functions ending in `_locked` operate on that state;
callers hold its mutex. Core also has exclusive ownership in its event loop.
