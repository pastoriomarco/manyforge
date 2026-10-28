# ros2_medkit

> **Status:** nothing runnable. ManyForge does not report faults to
> [ros2_medkit](https://github.com/selfpatch/ros2_medkit) today. Both
> effectful entrypoints, `python3 -m manyforge_medkit_adapter.adapter` and
> `python3 -m manyforge_ros.diagnostics_bridge`, exit 78
> (`composable_cell_runtime_unavailable`) before they parse arguments or touch
> the network or ROS. No ManyForge image includes ros2_medkit. Medkit
> integration is planned after publication.

ManyForge core stays medkit-free. Only `manyforge_medkit_adapter` may depend
on medkit, one-way.

## What is in the tree

| Part | Today |
| --- | --- |
| `manyforge_medkit_adapter.forwarder` | Pure reducer, tested hermetically: one FAILED report per fault occurrence, then a PASSED stream on clear (`healing_passes`, default 5), severity mapping, bounded state. Its `FaultReport` fields match `ros2_medkit_msgs/srv/ReportFault` (`fault_code`, `event_type`, `severity`, `description`, `source_id`). |
| `manyforge_ros.diagnostics_bridge` | Pure mapping from ManyForge faults to `diagnostic_msgs` statuses plus an OK heartbeat. Its process entrypoint exits 78. |
| `manyforge_behavior.fault_taxonomy` | Fault classes and their external codes `MANYFORGE_<REASON_CODE>` (UPPER_SNAKE, ≤64 characters). Severities INFO/WARN/ERROR/CRITICAL map to medkit 0–3. |
| `GET /api/debug/export/ros2-medkit` | A ManyForge-native trace export (schema `manyforge.ros2_medkit_trace.v2`). No medkit tool reads it; medkit has no trace import. |

The planned adapter will read the alarm snapshot `GET /api/execution/alarms`
from the Headless API and report the changes to the medkit fault manager.

## Running ros2_medkit next to ManyForge

ros2_medkit 0.7.0 is released as apt binaries for ROS 2 Lyrical, the distro of
the ManyForge ROS lanes, so it installs from the ROS apt repository; no
source build is needed. Run medkit in a ROS container, never on the host.

Fault manager:

```bash
ros2 run ros2_medkit_fault_manager fault_manager_node --ros-args \
  -p database_path:=/tmp/medkit_faults.db \
  -p healing_enabled:=true
```

- Set `database_path` to a writable location. The default,
  `/var/lib/ros2_medkit/faults.db`, fails for non-root users. The database
  and any rosbag snapshots grow on disk, so bound them.
- Set `healing_enabled:=true`. The default is `false`, and then a PASSED
  stream never heals a confirmed fault.
- Check: `ros2 service list | grep fault_manager` shows `report_fault`,
  `list_faults` and `clear_fault`.
- Build the adapter against the same `ros2_medkit_msgs` as the running fault
  manager. The service hash changed after medkit 0.4.0.

> **Gateway security warning.** `ros2_medkit_gateway` has authentication
> **off** by default. With runtime discovery it exposes every ROS service,
> action and parameter it finds as REST operations and configurations,
> including trajectory, controller-manager and robot-driver services. A
> client of the gateway can then move or reconfigure the robot outside
> ManyForge's Stop, attribution and Apply checks. If you run the gateway,
> bind it to `127.0.0.1` or enable authentication with viewer-only clients.

## Notes for the planned integration

- Medkit aggregates faults by `fault_code` across sources. `source_id`
  should name the cell and the alarm subject, not a Composer.
- A `MedkitDiscoveryHint` on `/ros2_medkit/discovery` only enriches an entity
  that already exists. Its `stamp` drives a TTL, so the hint must be
  republished periodically, and the gateway needs the topic_beacon plugin.
- Medkit's own `diagnostic_bridge` maps `/diagnostics` ERROR and CRITICAL
  alike, so CRITICAL faults lose their debounce bypass on that path.
- Clearing a fault in medkit must never clear or acknowledge a ManyForge
  alarm.

## Tests

| Layer | Command | Needs |
| --- | --- | --- |
| Hermetic (mapping, reducer) | `./scripts/test-ros.sh` in the ROS Lyrical container | nothing extra; the real-medkit test skips with a reason |
| Real medkit lifecycle | `pytest manyforge_medkit_adapter/tests/test_medkit_integration.py` with ROS and medkit sourced | a running fault manager (above) |

The integration test drives the reducer against a real fault manager:
CRITICAL fault → FAILED → `CONFIRMED` → PASSED stream → the fault leaves
`CONFIRMED`.
