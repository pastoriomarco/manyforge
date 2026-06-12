"""Audit 6H: standard diagnostics surface mapping tests (hermetic)."""

from __future__ import annotations

import pytest

from manyforge_ros.diagnostics_bridge import (
    LEVEL_ERROR,
    LEVEL_OK,
    LEVEL_WARN,
    build_diagnostic_statuses,
    external_fault_code,
    fault_severity,
    statuses_to_msg,
)


def test_healthy_digest_emits_ok_heartbeat():
    statuses = build_diagnostic_statuses(
        {"state": "running", "error": None},
        deployment_name="ur10e_preview",
    )
    assert len(statuses) == 1
    status = statuses[0]
    assert status["name"] == "manyforge/execution"
    assert status["level"] == LEVEL_OK
    assert status["hardware_id"] == "ur10e_preview"


def test_tracking_fault_maps_taxonomy_measurements_and_alias():
    digest = {
        "state": "stopped",
        "error": "Controller tracking error exceeded tolerance: 0.078 rad",
        "errorCode": "tracking_error_exceeded",
        "errorDetails": {
            "reason_code": "tracking_error_exceeded",
            "backend": "ros2_control",
            "surface": "ros2_control_execution_backend",
            "joint_name": "wrist_3_joint",
            "tracking_error_rad": "0.078",
            "tracking_error_tolerance_rad": "0.05",
        },
    }
    statuses = build_diagnostic_statuses(digest, deployment_name="isaac_preview")
    assert len(statuses) == 1
    status = statuses[0]
    assert status["name"] == "manyforge/execution"
    assert status["level"] == LEVEL_ERROR
    assert "tracking error exceeded" in status["message"]
    values = dict(status["values"])
    assert values["fault_code"] == "MANYFORGE_TRACKING_ERROR_EXCEEDED"
    assert values["reason_code"] == "tracking_error_exceeded"
    assert values["manyforge_severity"] == "ERROR"
    assert values["joint_name"] == "wrist_3_joint"
    assert values["tracking_error_rad"] == "0.078"


def test_critical_class_maps_to_error_level_with_severity_value():
    digest = {
        "state": "stopped",
        "error": "Timed out waiting for FollowJointTrajectory cancel confirmation",
        "errorCode": "stop_failed",
        "errorDetails": {
            "reason_code": "controller_cancel_timeout",
            "backend": "ros2_control",
        },
    }
    statuses = build_diagnostic_statuses(digest, deployment_name="d")
    status = statuses[0]
    # diagnostic_msgs has no CRITICAL level: ERROR level + preserved severity.
    assert status["level"] == LEVEL_ERROR
    assert dict(status["values"])["manyforge_severity"] == "CRITICAL"
    assert (
        dict(status["values"])["fault_code"]
        == "MANYFORGE_CONTROLLER_CANCEL_TIMEOUT"
    )


def test_unknown_code_is_sanitized_to_fault_code_charset():
    assert external_fault_code("weird-code.42!") == "MANYFORGE_WEIRD_CODE_42"
    assert fault_severity("weird-code.42!") == "ERROR"
    code = external_fault_code("x" * 100)
    assert len(code) <= 64
    digest = {
        "state": "stopped",
        "error": "boom",
        "errorCode": "tree_failed",
        "errorDetails": {},
    }
    values = dict(
        build_diagnostic_statuses(digest, deployment_name="d")[0]["values"]
    )
    assert values["fault_code"] == "MANYFORGE_TREE_FAILED"


def test_unreachable_composer_reports_warn_not_silence():
    statuses = build_diagnostic_statuses(None, deployment_name="d")
    assert len(statuses) == 1
    assert statuses[0]["level"] == LEVEL_WARN
    assert statuses[0]["name"] == "manyforge/bridge"


def test_statuses_to_msg_builds_diagnostic_array():
    pytest.importorskip("diagnostic_msgs")
    from builtin_interfaces.msg import Time

    statuses = build_diagnostic_statuses(
        {"state": "running", "error": None}, deployment_name="d"
    )
    msg = statuses_to_msg(statuses, Time())
    assert len(msg.status) == 1
    assert msg.status[0].name == "manyforge/execution"
    assert msg.status[0].level == bytes([LEVEL_OK])


def test_running_state_with_stale_error_text_emits_ok_heartbeat():
    """The digest can carry the previous run's error prose while a new run
    is active; the bridge must emit the OK heartbeat (same guard as the
    medkit forwarder), not a stale ERROR that would block DEM healing."""
    digest = {
        "state": "running",
        "error": "Controller tracking error exceeded tolerance: 0.078 rad",
        "errorCode": "tracking_error_exceeded",
        "errorDetails": {"reason_code": "tracking_error_exceeded"},
    }
    statuses = build_diagnostic_statuses(digest, deployment_name="d")
    assert len(statuses) == 1
    assert statuses[0]["level"] == LEVEL_OK
    assert statuses[0]["name"] == "manyforge/execution"


def test_bridge_import_chain_is_py_trees_free():
    """The canonical ros-jazzy lane image deliberately ships without
    py_trees; the bridge must import through the dependency-light
    fault_taxonomy module, never through cycle_manager. Subprocess with a
    meta-path blocker simulates the lane image."""
    import os
    import subprocess
    import sys
    from pathlib import Path

    repo = Path(__file__).resolve().parents[2]
    probe = (
        "import sys\n"
        "class _Block:\n"
        "    def find_spec(self, name, *a, **k):\n"
        "        if name == 'py_trees' or name.startswith('py_trees.'):\n"
        "            raise ImportError('blocked: py_trees (lane-image simulation)')\n"
        "sys.meta_path.insert(0, _Block())\n"
        "import manyforge_ros.diagnostics_bridge\n"
        "import manyforge_medkit_adapter.forwarder\n"
        "print('IMPORT_OK')\n"
    )
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [
            str(repo / "manyforge_ros"),
            str(repo / "manyforge_behavior"),
            str(repo / "manyforge_medkit_adapter"),
            env.get("PYTHONPATH", ""),
        ]
    )
    result = subprocess.run(
        [sys.executable, "-c", probe],
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr
    assert "IMPORT_OK" in result.stdout


def test_gripper_fault_maps_to_gripper_component_with_alias():
    digest = {
        "state": "idle",
        "error": "Gripper action goal was rejected",
        "errorCode": "gripper_goal_rejected",
        "errorDetails": {
            "reason_code": "gripper_goal_rejected",
            "surface": "command_gripper",
            "gripper_id": "primary",
            "message": "Gripper action goal was rejected",
        },
    }
    # Tree-terminal failures rest in idle with the error latched: idle must
    # latch (proven by the 2026-06-12 live acceptance, where the guard's old
    # idle exclusion made a real gripper rejection invisible externally).
    statuses = build_diagnostic_statuses(digest, deployment_name="isaac_preview")
    assert len(statuses) == 1
    status = statuses[0]
    assert status["name"] == "manyforge/gripper"
    assert status["level"] == LEVEL_ERROR
    values = dict(status["values"])
    assert values["fault_code"] == "MANYFORGE_GRIPPER_GOAL_REJECTED"
    assert values["reason_code"] == "gripper_goal_rejected"
    assert values["manyforge_severity"] == "ERROR"
    assert values["gripper_id"] == "primary"
