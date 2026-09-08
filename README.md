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

## Commands (Central Controller)

| Command         | Description              |
|-----------------|--------------------------|
| `local-RED`     | Set light to RED         |
| `local-YELLOW`  | Set light to YELLOW      |
| `local-GREEN`   | Set light to GREEN       |
| `train-BLOCK`   | Block train crossing     |
| `train-OPEN`    | Open train crossing      |
| `quit`          | Exit                     |
