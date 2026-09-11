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
# Replace VM_x86_Target01 with the Central VM's Qnet node name
gns -c VM_x86_Target01 &
/tmp/local_intersection_controller -g
/tmp/train_controller -g
```

## Commands (Central Controller)

| Command | Description |
|---------|-------------|
| `mode-fixed [I1..I6\|all]` | Request fixed timing |
| `mode-sensor [I1..I6\|all]` | Request sensor timing |
| `mode-temp I1 sensor 30` | Request a temporary mode |
| `mode-revert I1` | Request cancellation of a temporary mode |
| `coordinate I1 NS 0` | Request fixed-cycle coordination |
| `status` / `watch` | Inspect a snapshot / enter live view (Enter exits) |
| `commands` / `faults` / `events` | Inspect requests, fault alerts and recent events |
| `help` / `quit` | Command usage / exit |

Build Central with `make -C central_controller all PLATFORM=x86_64` in a configured
QNX SDP environment, then copy `central_controller/build/x86_64-debug/central_controller`
to the QNX target. Run `make -C central_controller/tests all PLATFORM=x86_64` to build
its test executables.

Central on `main` uses the existing shared protocol. Local and Train currently
provide demonstration heartbeat/test traffic; real status publication and command
application still need integration. A connected peer alone does not enable commands.
The default maximum status age is five seconds, configurable with `-s 1..60`.

See [Central instructions](central_controller/README.md) and the
[integration contract](central_controller/INTEGRATION.md) for test commands,
message formats, real-time assumptions, and the pending override/display decisions.
