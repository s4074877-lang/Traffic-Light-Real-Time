# Traffic Light Real-Time System

## Running the Controllers

Local now runs separate `--role core`, `--role comm`, `--role io`, and
`--role display` processes. See [Local process deployment](local_intersection_controller/PROCESSES.md)
for the complete six-intersection startup and failure checks.

### Single VM Testing (Local Mode)

```bash
# Terminal 1
/tmp/central_controller -l

# Terminal 2, legacy I1 service
/tmp/local_intersection_controller -l --role core

# Additional terminal: publishes the legacy I1 endpoint
/tmp/local_intersection_controller -l --role comm

# Separate terminals for display and input
/tmp/local_intersection_controller -l --role display
/tmp/local_intersection_controller -l --role io

# Or run a specific Local instance/service
/tmp/local_intersection_controller -l -i I2 -n traffic_local_I2 --role core
/tmp/local_intersection_controller -l -i I2 -n traffic_local_I2 --role comm

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
/tmp/local_intersection_controller -g -i I1 -n traffic_local_I1
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
its test executables. Run `make -C local_intersection_controller/tests run PLATFORM=x86_64`
on QNX for the Local core timing/telemetry harness.

Central uses the existing shared protocol. The merged Local now has a runtime
intersection ID (`-i I1..I6`) and service name (`-n service`), plus traffic
simulation, time-of-day policy, status/fault publication and mode/coordination
handlers. Local railway filtering maps P1 to I1/I2, P2 to I3/I4 and P3 to
I5/I6; Train still must deliver events to every affected Local service in the
final deployment. Central adds `sim-start I1`, `sim-stop I1` and
`sim-time I1 07:00`, with the Local `SIM1` handler still required. For this
demo, omit Central `--schedule` so Local's simulated clock controls time-of-day
policy. A connected peer alone does not enable commands; the default maximum
status age is five seconds, configurable with `-s 1..60`. Source review gaps and
unverified real-peer behavior are recorded in the integration contract;
historical fixture results are not validation of the updated Local.

See [Central instructions](central_controller/README.md) and the
[integration contract](central_controller/INTEGRATION.md) for test commands,
message formats, real-time assumptions, and the pending override/display decisions.
