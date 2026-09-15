# Local validation

This file records Local-controller checks that are owned by the Local module.
It separates source-level harness evidence from full QNX integration evidence.

## Local core harness

QNX launcher acceptance checks (not executed by Windows cross-compilation):
- Run the binary with only `-l`; verify six cores, six comm processes and the UI.
- Switch between `view I3` and `view all`; verify timers continue independently.
- Exit using `q`, EOF and Ctrl+C; verify all launcher-owned children exit.
- Start a second launcher; verify refusal leaves the first session intact.
- Start a standard core separately; verify launcher refusal leaves it running.
- Force a child startup failure; verify previously started children are cleaned up.

Deployment now uses separate core/comm/io/display processes; see `PROCESSES.md`.
The harness remains an in-process test of core logic, not a process isolation test.

Run on a QNX target with the QNX toolchain available:

```sh
make -C local_intersection_controller/tests run
```

The `local_logic_test` harness exercises the Local state-machine functions
directly without starting Central or Train:

- status telemetry fits the existing `test_message_t.data` envelope and reports
  sequence, sensor counts, pedestrian requests, simulated time, train state,
  fault state and last accepted command ID;
- the configured staggered startup profile matches I1 EW, I2 NS, I3 NS, I4 EW,
  I5 EW and I6 NS, with per-intersection green timings inside protocol bounds;
- Local heartbeat payload reports `sender_id`, `healthy` and a nonzero sequence;
- runtime intersection matching accepts I1-I6 targets and railway filtering maps
  P1 to I1/I2, P2 to I3/I4 and P3 to I5/I6;
- the fixed 20s green / 2s yellow phase cycle advances while Central is offline;
- sensor mode adjusts green time using the configured threshold and bounds;
- sensor reversal between the two Green phases keeps the previously committed
  44-second baseline allocation;
- pedestrian transfers preserve seconds at the 30-second limit, including
  a cross-cycle 35s/53s pair totaling 88s (individual cycles need not be 44s);
- a `ped_NS` request during `EW_GREEN` caps that compatible green to 10 seconds
  and transfers the removed time to the next NS vehicle green;
- railway preemption safely terminates green through yellow, holds all-red until
  `TRAIN_CLEAR`, then runs the recovery hold before normal traffic resumes.
- repeated railway preempt messages preserve the remaining Yellow;
- conflicting stored Green outputs enter fail-safe before phase recomputation.

`local_input_test` separately checks invalid console commands, sensor count bounds,
same-phase coordination offsets, and simulation command IDs in telemetry.
It also checks `view I1..I6` / `view all` parsing and that invalid view commands
leave the previous selection unchanged.

Both test executables and the application were cross-compiled with QNX SDP 7.1,
using `-Wall -Wextra -Werror`. Application builds were checked with demo/simulation
enabled and disabled. No QNX runtime test result is claimed by these build checks.

## Integration proof still required

The harness is not a replacement for the final real-time demonstration. On the
target system, still record an end-to-end run with actual QNX processes:

- Local status and typed heartbeat received by Central, including degraded
  health when Local raises a fault;
- phase/status refresh timing from Local state change to Central display;
- Local continuing fixed/sensor/pedestrian/railway logic while Central is killed;
- railway preemption and clear using the real Train process, including delivery
  to both Local services affected by the crossing;
- CPU/load notes and `CLOCK_MONOTONIC` timestamps for the timing claims in the
  final report.
- `--role display --all` showing I1-I6, then marking a stopped core OFFLINE while
  the other rows keep updating; restarting that core restores its row.
- `--role ui`: switch I3 -> I5 -> all without restarting any core; verify `n`
  affects only the selected target, is rejected in all view, and does not reach
  any other target when the selected core is offline. Type while the table
  refreshes, then verify terminal echo is restored after q/Ctrl-C.
