# Train–Local Integration Review

> **Status:** Not fully completed · **Method:** Code review only (not run/tested) · **Files modified:** None

## Summary

The review found **6 issues**, mostly in the **Train controller** and its integration with the **Local controllers**. Issues 1–4 are **P1** and should be fixed first, because they directly break Train ↔ Local coordination.

| # | Priority | Area | Issue | Location |
|---|----------|------|-------|----------|
| 1 | P1 | Train → Local IPC | Train sends to only one Local controller, not all 6 | [train_controller.c:675](train_controller/src/train_controller.c#L675) |
| 2 | P1 | Gate state machine | CLEAR can be sent while a train is approaching | [crossing.c:218](train_controller/src/crossing/crossing.c#L218) |
| 3 | P1 | Gate state machine | Two inconsistent timeout paths when a train leaves | [crossing.c:272](train_controller/src/crossing/crossing.c#L272) |
| 4 | P1 | IPC robustness | Blocking `MsgSend()` can freeze the simulator | [train_controller.c:365](train_controller/src/train_controller.c#L365) |
| 5 | P2 | Concurrency | Shared crossing state is not fully synchronized | [rail_sim.c:140](train_controller/src/crossing/rail_sim.c#L140) |
| 6 | P2 | Central config | Offsets limited to a 44 s cycle (`0…43`) | [commands.c:226](central_controller/src/commands.c#L226), [ipc.c:143](central_controller/src/ipc.c#L143) |

---

## P1 — Must Fix (Train ↔ Local Coordination)

### 1. Train does not send to all 6 Local controllers

- **Problem:** There is only one `local_conn`, connected to the old service name.
- **Impact:** With six `traffic_local_I1 … I6` services running, preempt/clear messages do not reach the correct intersections.
- **Expected mapping:**

  | Crossing | Local controllers |
  |----------|-------------------|
  | P1 | I1, I2 |
  | P2 | I3, I4 |
  | P3 | I5, I6 |

- **Location:** [train_controller.c:675](train_controller/src/train_controller.c#L675)

### 2. Approach detection may send CLEAR incorrectly

- **Problem:** The `TRAIN_APPROACH` branch only starts closing the gate when it is `GATE_OPEN`, and ignores `GATE_OPENING`. When the gate then reaches `GATE_OPENED`, CLEAR is still sent even though a train may be approaching.
- **Required change:**
  1. Cancel the opening sequence when a train approaches during `GATE_OPENING`.
  2. Close the gate.
  3. Check **both** railway directions before sending CLEAR.
- **Location:** [crossing.c:218](train_controller/src/crossing/crossing.c#L218)

### 3. Inconsistent timeout paths when a train leaves the crossing

- **Problem:** There are two different timeout paths. One uses `TIMER_TRAIN_TIMEOUT` only if the train keeps moving in the same direction.
- **Impact:** If a train changes direction while on the crossing, the timeout may not be detected correctly.
- **Location:** [crossing.c:272](train_controller/src/crossing/crossing.c#L272)

### 4. Blocking IPC can freeze the Train simulator

- **Problem:** `MsgSend()` is called directly inside callbacks and loops with no timeout.
- **Impact:** If a Local or Central controller does not reply, the whole Train simulator blocks and simulation time stops.
- **Required change:** Add a timeout to `MsgSend()` calls, or move IPC to an asynchronous path so communication never blocks simulation timing.
- **Location:** [train_controller.c:365](train_controller/src/train_controller.c#L365)

---

## P2 — Should Fix

### 5. Crossing state accessed concurrently without full synchronization

- **Problem:** The tick thread, input thread, and IPC receive thread all read/modify the same crossing state, but the mutex is only used in the handler.
- **Impact:** Race conditions can cause lost or incorrect state updates.
- **Location:** [rail_sim.c:140](train_controller/src/crossing/rail_sim.c#L140)

### 6. Central still limited to a 44-second coordination cycle

- **Problem:** Local controllers now have their own timing durations, but Central's parser and IPC validation still only accept offsets `0…43`.
- **Impact:** Valid offsets greater than 43 are rejected.
- **Locations:** [commands.c:226](central_controller/src/commands.c#L226), [ipc.c:143](central_controller/src/ipc.c#L143)

---

## Priority

Fix **issues 1–4 first**, because they directly affect **Train ↔ Local coordination**.

## Limitations

- Findings are based on **code review only**.
- No tests were run: the QNX compiler/toolchain was not available in `PATH`.
- No source files have been modified yet.
