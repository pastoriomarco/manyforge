# ros2_medkit integration — historical restoration reference

> **Cutover status (2026-07-31):** there is no runnable ManyForge Medkit or
> generic diagnostics process on the current composable-cell branch. The
> pure fault reducer, diagnostic mapping, and ROS message translation remain;
> both effectful entrypoints exit 78 before network or ROS effects. I5/H0 must
> restore them as thin clients of owner-issued headless diagnostics, not by
> reviving Composer runtime authority. The recipes and operational behavior
> below describe the implementation at
> `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`; they are acceptance
> inputs, not current commands or qualification claims.

The historical implementation integrated with
[ros2_medkit](https://github.com/selfpatch/ros2_medkit) through three
surfaces. ManyForge core remains **medkit-free**; only the future adapter
boundary may couple to medkit.

| Historical surface | Preserved requirement | Current disposition |
| --- | --- | --- |
| `manyforge_ros.diagnostics_bridge` | Taxonomy-mapped statuses plus an OK heartbeat on `/diagnostics`. | Pure mapping and ROS translation retained; process awaits H0 source/API. |
| `manyforge_medkit_adapter` | FAILED on latch, PASSED healing, archive reference, severity, and discovery mapping. | Pure reducer retained; ROS service process awaits H0 source/API. |
| `GET /api/debug/export/ros2-medkit` | Normalized, served trace export rather than an implicit push owner. | Historical API requirement; future owner/API assigned at H0. |

Fault codes are `MANYFORGE_<REASON_CODE>` (UPPER_SNAKE, ≤64 chars, from the
Phase 6A taxonomy in `manyforge_behavior.cycle_manager.
ROS2_CONTROL_BACKEND_FAULT_CLASSES`); severities map INFO/WARN/ERROR/CRITICAL
→ medkit 0–3. **CRITICAL confirms immediately (bypasses medkit debounce)** —
reserved for unconfirmed-stop / stuck-lifecycle classes.

## 1. Install / build ros2_medkit (once per environment)

In a ROS 2 Jazzy (or Humble) environment — e.g. inside the ManyForge
composer-ros container:

```bash
# dependency medkit needs that base images may lack (as root):
apt-get update && apt-get install -y nlohmann-json3-dev

git clone https://github.com/selfpatch/ros2_medkit /tmp/ros2_medkit
cd /tmp/ros2_medkit
source /opt/ros/jazzy/setup.bash
colcon build --packages-up-to ros2_medkit_fault_manager \
  --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
# -DBUILD_TESTING=OFF avoids a test_msgs dependency. ~30 s on Thor.
```

> **Version pinning (fail-closed):** medkit changed its service hash after
> 0.4.0 — the adapter and the fault_manager MUST be built against the same
> `ros2_medkit_msgs`. The adapter refuses to start (exit 2/3, clear log)
> rather than silently dropping faults when the contract is absent.

## 2. Run the fault manager

```bash
source /opt/ros/jazzy/setup.bash && source /tmp/ros2_medkit/install/setup.bash
ros2 run ros2_medkit_fault_manager fault_manager_node \
  --ros-args -p database_path:=/tmp/medkit_faults.db
```

> Gotcha: the default `database_path` is `/var/lib/ros2_medkit/faults.db`,
> which aborts with a filesystem permission error for non-root users —
> always set `database_path` to a writable location.

Sanity: `ros2 service list | grep fault_manager` must show `report_fault`,
`list_faults`, `clear_fault`.

## 3. Run the adapter (production path for core faults)

```bash
source /opt/ros/jazzy/setup.bash && source /tmp/ros2_medkit/install/setup.bash
cd /workspace   # manyforge repo root
export PYTHONPATH="/workspace/manyforge_medkit_adapter:/workspace/manyforge_ros:/workspace/manyforge_behavior:${PYTHONPATH}"
python3 -m manyforge_medkit_adapter.adapter \
  --composer-url http://127.0.0.1:9000 \
  --period 1.0 \
  --fault-manager-prefix /fault_manager
```

> Gotcha: **append** to `PYTHONPATH` (`:${PYTHONPATH}`); overwriting it
> drops the sourced medkit install paths and triggers the exit-2 fail-closed
> path.

Exit codes: `0` clean shutdown · `2` `ros2_medkit_msgs` not importable ·
`3` `report_fault` service unreachable within `--service-wait`.

Behavior to know when operating:

- One FAILED per fault occurrence (medkit aggregates `occurrence_count`).
- On clear the adapter emits a **PASSED stream** (`healing_passes`, default
  5 polls). A single PASSED never heals a CONFIRMED fault under the DEM
  debounce model — verified against a real fault_manager. While the Composer
  is unreachable the forwarder no-ops, so an in-flight healing stream pauses
  and resumes when the digest is observable again.
- `source_id` is `/manyforge/<deployment>/composer`; the description carries
  the ManyForge failure-archive id when one exists (freeze-frame parity for
  post-mortems — medkit snapshots only ROS topics).
- A transient-local `MedkitDiscoveryHint` is published once on
  `/ros2_medkit/discovery` so ManyForge appears in the SOVD hierarchy.

## 4. Run the generic diagnostics bridge (medkit-free alternative)

```bash
export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:${PYTHONPATH}"
python3 -m manyforge_ros.diagnostics_bridge --composer-url http://127.0.0.1:9000 --period 1.0
```

Publishes `/diagnostics`; pair with medkit's own
`ros2 run ros2_medkit_diagnostic_bridge ...` if you want medkit ingestion
without the direct adapter. The OK heartbeat is what heals faults — do not
filter it out.

## 5. Testing

| Layer | Command (composer-ros container) | Needs |
| --- | --- | --- |
| Hermetic (mapping + forwarder + bridge) | `./scripts/test-ros.sh` | nothing extra (29 passed; medkit integration test skips with a clear reason) |
| Real medkit lifecycle | `source` ROS + medkit install, then `PYTHONPATH=... python3 -m pytest manyforge_medkit_adapter/tests/test_medkit_integration.py` | built medkit (§1) + running fault_manager (§2) |
| Manual smoke | `ros2 service call /fault_manager/list_faults ros2_medkit_msgs/srv/ListFaults "{}"` after inducing any backend fault | §1 + §2 + adapter running |

The integration test drives the full lifecycle: CRITICAL fault → FAILED →
medkit `CONFIRMED` (source tracked, description carried) → healing stream →
fault leaves CONFIRMED.

## 6. Production checklist

1. Build medkit pinned alongside the adapter (§1); bake into the deployment
   image rather than building at runtime.
2. Start order: fault_manager → ManyForge stack → adapter (it waits
   `--service-wait`, then fails closed).
3. Supervise the adapter like any sidecar; restart-safe (re-reports active
   faults only on new occurrences; the discovery hint re-latches).
4. Run the diagnostics bridge too if other consumers (rqt_robot_monitor,
   logging pipelines) should see ManyForge health — it is independent.
5. Traces stay on the ManyForge API (`/api/debug/export/ros2-medkit`); point
   medkit gateway tooling at it rather than expecting pushes.
