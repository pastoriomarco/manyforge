"""Audit 6H: standard diagnostics surface mapping tests (hermetic)."""

from __future__ import annotations

import pytest

from manyforge_ros.diagnostics_bridge import (
    LEVEL_ERROR,
    LEVEL_OK,
    LEVEL_WARN,
    MAX_DETAIL_CHARS,
    MAX_DETAIL_ROWS,
    MAX_ID_CHARS,
    MAX_STATUS_ROWS,
    MAX_STATUS_VALUE_ROWS,
    UNAVAILABLE_MESSAGE,
    UNAVAILABLE_REASON_CODE,
    build_diagnostic_statuses,
    external_fault_code,
    fault_severity,
    main,
    statuses_to_msg,
)


class _StringTrap:
    def __str__(self) -> str:
        raise AssertionError("an arbitrary object's __str__ must not be called")


def _valid_status() -> dict:
    return {
        "name": "manyforge/execution",
        "level": LEVEL_OK,
        "message": "ok",
        "hardware_id": "deployment",
        "values": [("state", "running")],
    }


def test_effectful_bridge_entrypoint_is_fenced(capsys: pytest.CaptureFixture[str]):
    assert main() == 78
    assert capsys.readouterr().err.strip() == UNAVAILABLE_MESSAGE
    assert UNAVAILABLE_REASON_CODE in UNAVAILABLE_MESSAGE


@pytest.mark.parametrize(
    "deployment_name",
    [None, 7, "", " deployment", "deployment ", "x" * (MAX_ID_CHARS + 1)],
)
def test_deployment_name_is_an_exact_bounded_identifier(deployment_name) -> None:
    with pytest.raises(ValueError):
        build_diagnostic_statuses(None, deployment_name=deployment_name)


@pytest.mark.parametrize(
    "reason_code",
    [None, 7, True, "", " reason", "reason ", "x" * (MAX_ID_CHARS + 1)],
)
def test_fault_helpers_reject_coercible_or_unbounded_reasons(reason_code) -> None:
    with pytest.raises(ValueError):
        external_fault_code(reason_code)
    with pytest.raises(ValueError):
        fault_severity(reason_code)


def test_fault_helpers_do_not_stringify_arbitrary_objects() -> None:
    value = _StringTrap()
    with pytest.raises(ValueError, match="must be a non-empty string"):
        external_fault_code(value)
    with pytest.raises(ValueError, match="must be a non-empty string"):
        fault_severity(value)


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


def test_json_details_use_bounded_deterministic_projection() -> None:
    digest = {
        "state": "stopped",
        "error": "fault",
        "errorCode": "custom_fault",
        "errorDetails": {
            "reason_code": "custom_fault",
            "boolean": True,
            "count": 7,
            "nested": {"b": [2, None], "a": 1},
        },
    }
    values = dict(
        build_diagnostic_statuses(digest, deployment_name="d")[0]["values"]
    )
    assert values["boolean"] == "true"
    assert values["count"] == "7"
    assert values["nested"] == '{"a":1,"b":[2,null]}'


@pytest.mark.parametrize(
    "digest",
    [
        [],
        {},
        {"state": "idle"},
        {"state": "", "error": None},
        {"state": "idle", "error": "fault"},
        {"state": "idle", "error": None, "errorCode": "fault"},
        {"state": True, "error": None},
        {"state": "running", "error": _StringTrap()},
        {"state": "stopped", "error": "fault", "errorCode": _StringTrap()},
        {"state": "stopped", "error": "fault", "errorDetails": []},
        {
            "state": "stopped",
            "error": "fault",
            "errorDetails": {"reason_code": _StringTrap()},
        },
        {
            "state": "stopped",
            "error": "fault",
            "errorDetails": {"reason_code": "fault", "detail": _StringTrap()},
        },
        {
            "state": "stopped",
            "error": "fault",
            "errorDetails": {"reason_code": "fault", "state": "spoofed"},
        },
        {
            "state": "stopped",
            "error": "fault",
            "errorDetails": {"reason_code": "fault"},
            "error_details": {"reason_code": "other"},
        },
        {
            "state": "stopped",
            "error": "fault",
            "errorCode": "fault",
            "error_code": "other",
        },
    ],
)
def test_malformed_digest_facts_fail_instead_of_becoming_healthy(digest) -> None:
    with pytest.raises(ValueError):
        build_diagnostic_statuses(digest, deployment_name="d")


def test_digest_detail_bounds_are_enforced_before_projection() -> None:
    too_many_details = {
        f"detail_{index}": "value" for index in range(MAX_DETAIL_ROWS + 1)
    }
    with pytest.raises(ValueError, match="error details exceeds"):
        build_diagnostic_statuses(
            {
                "state": "stopped",
                "error": "fault",
                "errorCode": "fault",
                "errorDetails": too_many_details,
            },
            deployment_name="d",
        )

    with pytest.raises(ValueError, match="detail key"):
        build_diagnostic_statuses(
            {
                "state": "stopped",
                "error": "fault",
                "errorCode": "fault",
                "errorDetails": {"x" * (MAX_ID_CHARS + 1): "value"},
            },
            deployment_name="d",
        )

    with pytest.raises(ValueError, match="2048-character bound"):
        build_diagnostic_statuses(
            {
                "state": "stopped",
                "error": "fault",
                "errorCode": "fault",
                "errorDetails": {"detail": "x" * (MAX_DETAIL_CHARS + 1)},
            },
            deployment_name="d",
        )


def test_diagnostic_value_rows_are_bounded_without_silent_detail_loss() -> None:
    allowed_details = {"reason_code": "fault"}
    allowed_details.update({f"detail_{index}": "value" for index in range(60)})
    statuses = build_diagnostic_statuses(
        {
            "state": "stopped",
            "error": "fault",
            "errorCode": "fault",
            "errorDetails": allowed_details,
        },
        deployment_name="d",
    )
    assert len(statuses[0]["values"]) == MAX_STATUS_VALUE_ROWS

    overflow_details = dict(allowed_details)
    overflow_details["detail_60"] = "value"
    with pytest.raises(ValueError, match="diagnostic values exceeds"):
        build_diagnostic_statuses(
            {
                "state": "stopped",
                "error": "fault",
                "errorCode": "fault",
                "errorDetails": overflow_details,
            },
            deployment_name="d",
        )


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


def test_unavailable_diagnostic_source_reports_warn_not_silence():
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


@pytest.mark.parametrize(
    "statuses",
    [
        (),
        [{**_valid_status(), "level": True}],
        [{**_valid_status(), "level": 4}],
        [{**_valid_status(), "name": "x" * (MAX_ID_CHARS + 1)}],
        [{**_valid_status(), "message": "x" * (MAX_DETAIL_CHARS + 1)}],
        [{**_valid_status(), "values": [["state", "running"]]}],
        [{**_valid_status(), "values": [("state",)]}],
        [{**_valid_status(), "values": [("state", 7)]}],
        [{**_valid_status(), "values": [("state", "a"), ("state", "b")]}],
        [{**_valid_status(), "extra": "unexpected"}],
    ],
)
def test_status_translation_rejects_malformed_rows_before_ros_import(statuses) -> None:
    with pytest.raises(ValueError):
        statuses_to_msg(statuses, object())


def test_status_translation_bounds_statuses_and_values_before_ros_import() -> None:
    with pytest.raises(ValueError, match="statuses exceeds"):
        statuses_to_msg(
            [_valid_status() for _ in range(MAX_STATUS_ROWS + 1)], object()
        )

    status = _valid_status()
    status["values"] = [
        (f"key_{index}", "value")
        for index in range(MAX_STATUS_VALUE_ROWS + 1)
    ]
    with pytest.raises(ValueError, match="values exceeds"):
        statuses_to_msg([status], object())


def test_status_translation_rejects_duplicate_status_names_before_ros_import() -> None:
    with pytest.raises(ValueError, match="duplicate name"):
        statuses_to_msg([_valid_status(), _valid_status()], object())


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
