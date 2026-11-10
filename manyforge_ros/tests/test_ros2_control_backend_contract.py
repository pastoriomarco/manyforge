"""Focused disposition for the removed ROS2-control bundle test corpus.

The historical 1,702-line suite constructed the deleted Python
create_ros2_control_kernel_from_paths factory and its singleton bundle. It
cannot remain as a permanently skipped compatibility test. Its exact source is
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
2db2411ab47a78eb24300711e799f6c157cd877e.

Every historical case family is assigned below to the addressed C++ runtime,
Kernel Stop, headless scene, lifecycle, Isaac, real-robot, or tool restoration
gate.
"""

from __future__ import annotations

import ast
from pathlib import Path


HISTORICAL_CASE_GATES: dict[str, tuple[str, ...]] = {
    "test_ros2_control_gripper_nonfinite_action_sample_fails_closed": (
        "addressed-runtime",
        "tools",
    ),
    "test_ros2_control_backend_requires_fresh_state_before_planning": ("addressed-runtime",),
    "test_ros2_control_backend_rejects_sim_tolerance_override_outside_commanded_sim": (
        "addressed-runtime",
        "isaac",
    ),
    "test_ros2_control_backend_rejects_negative_executor_thread_count": ("addressed-runtime",),
    "test_ros2_control_commanded_sim_uses_backend_initial_state_without_observed_feed": (
        "addressed-runtime",
        "isaac",
    ),
    "test_ros2_control_backend_preserves_fjt_abort_diagnostics": (
        "addressed-runtime",
        "kernel-stop",
    ),
    "test_ros2_control_backend_preserves_monitor_failure_after_late_result": (
        "addressed-runtime",
        "kernel-stop",
    ),
    "test_ros2_control_backend_resyncs_after_fault_before_next_run": (
        "addressed-runtime",
        "kernel-stop",
    ),
    "test_ros2_control_backend_reset_clears_inflight_gripper_command": (
        "addressed-runtime",
        "kernel-stop",
        "tools",
    ),
    "test_ros2_control_gripper_updates_roboplan_scene_group_state": (
        "headless-scene",
        "addressed-runtime",
        "tools",
    ),
    "test_ros2_control_gripper_driver_result_accepts_success_without_reached_goal": (
        "addressed-runtime",
        "tools",
    ),
    "test_ros2_control_isaac_effort_gripper_publishes_joint_state_and_dwell_completes": (
        "addressed-runtime",
        "isaac",
        "tools",
    ),
    "test_ros2_control_backend_documents_lock_order_contract": ("retained-now",),
    "test_ros2_control_admission_surface_accepts_exact_generic_binding": (
        "addressed-runtime",
        "lifecycle",
        "real-robot",
    ),
    "test_ros2_control_monitor_exits_promptly_after_stop_confirmation": (
        "addressed-runtime",
        "kernel-stop",
    ),
    "test_ros2_control_stop_observes_cancel_for_held_active_goal": (
        "addressed-runtime",
        "kernel-stop",
    ),
}

HISTORICAL_BLOB = "2db2411ab47a78eb24300711e799f6c157cd877e"
REMOVED_FACTORY_SYMBOLS = (
    "create_ros2_control_kernel_from_paths",
    "NativeKernelBundle",
)


def test_all_historical_ros2_control_case_families_remain_assigned() -> None:
    assert len(HISTORICAL_CASE_GATES) == 16
    assert HISTORICAL_BLOB == "2db2411ab47a78eb24300711e799f6c157cd877e"
    assert all(gates for gates in HISTORICAL_CASE_GATES.values())

    assigned = {
        gate
        for gates in HISTORICAL_CASE_GATES.values()
        for gate in gates
    }
    assert assigned == {
        "retained-now",
        "headless-scene",
        "addressed-runtime",
        "lifecycle",
        "kernel-stop",
        "isaac",
        "real-robot",
        "tools",
    }


def test_removed_bundle_factory_is_not_a_current_test_dependency() -> None:
    tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
    executable_symbols = {
        node.id
        for node in ast.walk(tree)
        if isinstance(node, ast.Name)
    } | {
        node.attr
        for node in ast.walk(tree)
        if isinstance(node, ast.Attribute)
    }
    called_symbols = {
        node.func.id
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    } | {
        node.func.attr
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    }

    assert {
        "import_module",
        "skip",
        "importorskip",
        "_create_bundle",
    }.isdisjoint(called_symbols)
    for symbol in REMOVED_FACTORY_SYMBOLS:
        assert symbol not in executable_symbols
