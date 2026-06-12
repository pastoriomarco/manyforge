# manyforge_medkit_adapter

Forwards ManyForge core faults to a [ros2_medkit](https://github.com/selfpatch/ros2_medkit)
fault manager: FAILED on fault latch, PASSED healing stream on clear,
severities from the ManyForge fault taxonomy (CRITICAL bypasses medkit
debounce), SOVD entity presence via a `MedkitDiscoveryHint` beacon.
Fail-closed startup: exits 2 when `ros2_medkit_msgs` is not importable,
3 when the `report_fault` service is unreachable.

ManyForge core stays medkit-free — this package owns the coupling, one-way.

**Full runbook (install, test, operate, gotchas):**
[`docs/operations/ROS2_MEDKIT.md`](../docs/operations/ROS2_MEDKIT.md)

Quick start (medkit built and sourced, fault_manager running):

```bash
export PYTHONPATH="$PWD/manyforge_medkit_adapter:$PWD/manyforge_ros:$PWD/manyforge_behavior:${PYTHONPATH}"
python3 -m manyforge_medkit_adapter.adapter --composer-url http://127.0.0.1:9000
```

Tests: hermetic forwarder tests run in the ros lane (`./scripts/test-ros.sh`);
the real-lifecycle integration test requires a built medkit and a running
fault_manager, and skips with a clear reason otherwise.
