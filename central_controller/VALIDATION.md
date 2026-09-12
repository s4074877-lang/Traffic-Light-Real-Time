# Central validation, 2026-09-12

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
