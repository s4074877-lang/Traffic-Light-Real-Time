# Central validation, 2026-09-12

## Current integration revision

Base: `main` **7e5e933** (including the teammate's merged Train simulator).
Build label: `central-20260912-integration`. All changes are under
`central_controller/`; `common/`, `local_intersection_controller/` and
`train_controller/` have no source modifications. The existing
`central-full-20260911` backup remains at `e187cb6`.

Central core and separate UI debug/release binaries built with QNX SDP 7.1 for
x86_64, `-std=gnu11 -Wall -Wextra -Werror`. Central test drivers use `-O2` and the
same warning policy. The real Train fixture uses the teammate's unchanged source
and its existing `-Wall` policy; only a Central-owned wrapper selects test service
names. Binaries were executed on VM_x86_Target01, QNX 7.1.0 x86_64, via QCONN.
Unique temporary uploads and fixture-owned services/processes were cleaned up.

| Suite | Result | Evidence |
| --- | --- | --- |
| Command parser | 149 checks passed | Existing Local command syntax, ranges and typed payloads |
| Monitor | 218 checks passed | Freshness, per-ID health, recovery and legacy heartbeat interpretation |
| External IPC | 1,188 checks passed | Legacy and compact wire formats, P1-P3 mapping, faults, malformed/short frames and replies, stopped peer and no replay |
| Operator policy | 7,489 checks passed | Train allowlist, bounded/atomic schedule-file validation, daily minute boundaries, temporary/persistent precedence and overflow |
| Private UI IPC | 33 checks passed | Strict private framing, callback error/overflow, detach/reconnect and request timeouts |
| Existing Central integration | 213 checks passed | CLI/watch, six-target dispatch, mode/temp/revert/coordination, overload/expiry, heartbeat progress and stopped-peer cleanup |
| New Central features | 155 checks passed | Compact Train telemetry/faults, independent Train/Local queues, separate I2 route/NACK, headless EOF, UI lifecycle/whitespace/errors, delayed dispatch, operator/schedule precedence, blocked remote fault command |
| Real Train integration | 30 checks passed | Actual Train compact telemetry, reported CLOSED/gate fault/reset, unchanged peer source and receipt-only history |

Observed external stopped-peer timeout: **500.9 ms**. Private UI slow-callback and
stopped-service timeouts: **500.9 ms** each. Final normal core quit observation:
**202.0 ms**. These are observations on this VM, not worst-case response bounds.
Individual assertion totals can vary where a test also checks observed heartbeat
events; changes from an older count do not imply a new feature was removed.

An additional smoke run executed the actual `central_ui` binary against the
headless fixture core: `-c status` rendered its snapshot, `-c quit` left the core
alive, and `-c shutdown` stopped it. All four exit/liveness checks returned zero;
redirected output contained no ANSI escapes. This used no live production service.

The real Train scenario sent `scale 5`, `stuck P1`, then `train-up`. Central
displayed P1-P3 on the correct rows, observed CLOSED on unaffected crossings and
GATE FAULT on P1, and retained the descriptive critical alert. After the sequence,
`reset P1` produced reported OPEN/Fault NONE. The alert correctly stayed latched:
the actual Train does not emit an explicit fault-clear alert. These observations
demonstrate interoperability and simulator behavior, not physical gate protection.

The new feature suite verified that an immediately eligible mode request can pass
a future coordination request, while the latter is not sent before its requested
delay. A blocked Train send does not prevent Local receipt, and its uncertain
command is not replayed after the peer resumes. Schedule tests verify priority 1
versus operator priority 2, persistent holds, temporary expiry/revert, and explicit
schedule-resume. Dashboard values remain received states even when a new mode has
been requested.

### Current remaining limits

- VM `.101:8000` responded with a QCONN banner. `.102` and `.103` each timed out
  after approximately two seconds. No GNS services were started/stopped and no
  VM/network configuration was changed. `global_transport_test` builds but
  **two-node transport and full multi-node integration remain unverified**.
- The actual Local state machine remains a teammate dependency. Its safe phase
  transitions, independent operation, railway protection and temporary expiry
  with Central offline were not validated by these Central fixtures.
- Current Train discards simulator BUSY/error text and unconditionally ACKs its
  command handler. Central cannot confirm application from that reply. It does
  not receive train STOP, flashing phase or separate track fields in v1.
- Remote `p#-fault` can re-lock Train's state mutex through its fault callback.
  Central blocks that command before transmission; the original peer was not
  patched. Other Train callback blocking and shared simulator concurrency still
  require its owner's review. See [INTEGRATION.md](INTEGRATION.md).
- No shared activation epoch, applied-command acknowledgement, session/sequence
  protection or deduplication was added to teammate-owned protocol files.
  `coordinate-at` controls Central dispatch time only. Temporary Central policy
  suppression is receipt-relative; Local owns real application/expiry.
- Local-origin `MSG_OVERRIDE_REQUEST` and shared `MSG_DISPLAY_UPDATE` remain
  undefined extra flows. The separate Central UI uses a private, tested protocol.
- A stopped legacy peer may retain a kernel send and delay whole-process exit.
  A blocked log filesystem can delay logger shutdown. No hardware timing/WCET or
  complete system response-time analysis has been established.

Architecture, timing assumptions and primary QNX references are in
[IMPLEMENTATION_NOTE.md](IMPLEMENTATION_NOTE.md).

## Historical hardening revision (before merged Train)

Base: `main` commit `8189f11`. Changes retain the existing shared protocol and leave
`common`, Local and Train source unchanged. The full experimental branch remains
separate.

## Build and target results

Built with QNX SDP 7.1 for x86_64. Central debug and release builds succeeded with
`-std=gnu11 -Wall -Wextra -Werror`. Test binaries use `-O2` and the same warning policy.
Executed the four local suites on VM_x86_Target01, QNX 7.1.0 x86_64, through QCONN.
Deployment used unique `/tmp/traffic_validation_*` paths, and fixtures used their
own service names and child processes.

| Suite | Result | Relevant evidence |
| --- | --- | --- |
| Command parser | 149 checks passed | Targets, ranges, payload construction and invalid commands |
| Monitor | 218 checks passed | Configurable 1/2/5/60-second boundaries, stale/recovery, per-ID degraded health, legacy heartbeat compatibility |
| IPC | 749 checks passed | ACK/NACK, every truncated reply prefix before the complete command ID across six boundary IDs, no replay while busy, timeout and shutdown |
| Central integration | 214 checks passed | UI/partial input/watch, successful all, temporary/revert/coordination payloads, history, queue overload/expiry, heartbeat service under queued load, stopped peer and exit |

Observed stopped-peer caller timeout: **500.9 ms**. Observed normal quit: **202.0 ms**.
The integration test verified six ready targets received requests within its
**2-second** bound, both peers continued receiving heartbeats under command floods
with measured gaps below the test's **2-second** bound, and expired queued requests
were not transmitted. These are fixture observations/assertions on this VM;
they are not worst-case timing guarantees for the integrated system.

## Real-time design decisions checked

- Shared monitor timestamps are sampled after acquiring the state mutex so a
  concurrent heartbeat cannot overtake an earlier timestamp and falsely reset
  synchronization. Display age is sampled after copying its snapshot.
- Commands wake their dispatcher immediately. Heartbeat releases use an absolute
  monotonic schedule and take precedence over new commands when due; a send
  already in progress can still delay the next heartbeat.
- Queue/history/event storage is bounded. Dispatch rechecks freshness and a
  five-second queue lifetime. Uncertain delivery is recorded and never retried
  automatically; receipt is never represented as proof of application.
- IPC and file output happen outside the shared-state lock. QNX already supplies
  mutex priority inheritance by default. Process/thread priorities were not
  arbitrarily raised above the other controllers.
- Five-second freshness is configurable with `-s 1..60`. This supervisory policy
  requires an agreed telemetry period; it is not a certified safety deadline.

The rationale and primary QNX references are in [INTEGRATION.md](INTEGRATION.md).

## Explicit remaining limits

- The new two-node `global_transport_test` **compiled but has not run**. Qnet was
  observed between VM01 and VM02, but neither initially had GNS. A later attempt
  to begin isolated GNS testing could not reach VM02's QCONN service. The attempt
  stopped before starting GNS or changing VM services/network settings. Global
  transport and network timing remain to be validated using the README procedure.
- Local/Train fixtures are not the real application state machines or hardware.
  Applied-command acknowledgement, status sequence/session IDs, deduplication,
  temporary-mode baseline and coordinated activation timing require a group
  protocol/behavior agreement.
- Override/display behavior remains unimplemented pending agreement with Thao;
  Central does not invent automatic approval semantics.
- Legacy all-zero heartbeat health is ambiguous. Typed degraded I1/P1 senders
  must use a nonzero sequence; this does not provide restart/replay detection.
- A legacy peer stopped after receiving a message can keep the kernel sender
  and process exit pending until it replies or terminates. Logging to a blocked
  filesystem can also delay shutdown.
- No full-system worst-case execution/blocking analysis, hardware deadline
  validation or production-load measurement was performed. Passing these tests
  supports the tested supervisory behavior, not a hard real-time certification.
