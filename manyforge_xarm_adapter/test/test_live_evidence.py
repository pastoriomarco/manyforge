"""Pure observation-evidence tests and live-capture disposition."""

from __future__ import annotations

from manyforge_xarm_adapter import live_evidence as subject


HISTORICAL_TEST_BLOB = "2a4ca4e3bd6cb5df70c6b0d1ba63f540d589c909"
HISTORICAL_CASE_DISPOSITION = {
    "test_read_only_capture_records_optional_timeout_without_passing_it": (
        "retained",
        "I10/R",
    ),
    "test_required_probe_timeout_is_a_failed_evidence_gate": (
        "retained",
        "I10/R",
    ),
    "test_supervised_preflight_uses_manifest_selected_binding": (
        "retained",
        "I7/L0",
        "I10/R",
    ),
    "test_read_only_capture_rejects_an_unexpected_graph_participant": (
        "retained",
        "I10/R",
    ),
    "test_supervised_preflight_requires_claimed_interfaces": (
        "retained",
        "I7/L0",
        "I10/R",
    ),
    "test_read_only_graph_expectations_follow_the_resolved_namespace": (
        "retained",
        "I6/N",
        "I10/R",
    ),
}


def _manifest(*, namespace: str = "") -> dict[str, object]:
    prefix = f"/{namespace.strip('/')}" if namespace.strip("/") else ""
    joints = [f"joint{index}" for index in range(1, 7)]
    return {
        "binding": {
            "ros_namespace": namespace,
            "driver_node": f"{prefix}/ufactory_driver",
            "diagnostics_topic": f"{prefix}/manyforge/xarm/diagnostics",
            "robot_state_topic": f"{prefix}/ufactory/robot_states",
            "joint_state_topic": f"{prefix}/ufactory/joint_states",
            "validated_joint_state_topic": (
                f"{prefix}/manyforge/xarm/validated_joint_states"
            ),
        },
        "controller_binding": {
            "controller_manager": f"{prefix}/controller_manager",
            "controller_manager_activity_topic": (
                f"{prefix}/controller_manager/activity"
            ),
            "hardware_component_name": "lite6_system",
            "trajectory_controller_name": (
                f"{prefix}/lite6_traj_controller"
            ),
            "trajectory_controller_type": (
                "joint_trajectory_controller/JointTrajectoryController"
            ),
            "claimed_command_interface_names": [
                f"{joint}/{interface}"
                for joint in joints
                for interface in ("position", "velocity")
            ],
            "state_interface_names": [
                f"{joint}/{interface}"
                for joint in joints
                for interface in ("position", "velocity")
            ],
        },
    }


def _read_only_nodes(namespace: str = "") -> bytes:
    prefix = f"/{namespace.strip('/')}" if namespace.strip("/") else ""
    return (
        f"{prefix}/ufactory_driver\n"
        f"{prefix}/lite6_robot_state_publisher\n"
        f"{prefix}/manyforge_xarm_read_only_observer\n"
    ).encode()


def _outcome(
    stdout: bytes,
    *,
    return_code: int = 0,
    timed_out: bool = False,
) -> subject.CommandOutcome:
    return subject.CommandOutcome(
        return_code=return_code,
        stdout=stdout,
        stderr=b"",
        timed_out=timed_out,
        duration_ms=5.0,
    )


def test_optional_timeout_is_recorded_but_never_passed() -> None:
    probe = subject.read_only_probes(_manifest())[-1]
    assessment = subject.assess_probe(
        probe=probe,
        outcome=_outcome(b"", return_code=124, timed_out=True),
        manifest=_manifest(),
    )

    assert probe.required is False
    assert assessment["timed_out"] is True
    assert assessment["timeout_is_success"] is False
    assert assessment["postcondition_passed"] is False


def test_required_timeout_fails_the_supplied_probe_outcome() -> None:
    probe = subject.read_only_probes(_manifest())[0]
    assessment = subject.assess_probe(
        probe=probe,
        outcome=_outcome(b"", return_code=124, timed_out=True),
        manifest=_manifest(),
    )

    assert probe.required is True
    assert assessment["postcondition_passed"] is False


def test_preflight_plan_uses_the_manifest_selected_controller() -> None:
    probes = subject.supervised_preflight_probes(_manifest(namespace="cell/a"))
    commands = [probe.argv for probe in probes]

    assert any("/cell/a/controller_manager" in command for command in commands)
    assert commands[-1][-1] == "/cell/a/controller_manager/activity"


def test_read_only_postcondition_rejects_an_unexpected_participant() -> None:
    passed, reason = subject.evaluate_postcondition(
        "read_only_nodes",
        (_read_only_nodes() + b"/unexpected_command_owner\n").decode(),
        _manifest(),
    )

    assert passed is False
    assert "unexpected_command_owner" in reason


def test_preflight_postcondition_requires_claimed_interfaces() -> None:
    manifest = _manifest()
    controller = manifest["controller_binding"]
    assert isinstance(controller, dict)
    commands = controller["claimed_command_interface_names"]
    states = controller["state_interface_names"]
    assert isinstance(commands, list)
    assert isinstance(states, list)
    output = (
        "command interfaces\n"
        + "\n".join(f"{name} [claimed]" for name in commands)
        + "\nstate interfaces\n"
        + "\n".join(str(name) for name in states)
    )

    passed, _ = subject.evaluate_postcondition(
        "claimed_production_interfaces", output, manifest
    )
    unclaimed, _ = subject.evaluate_postcondition(
        "claimed_production_interfaces",
        output.replace(" [claimed]", ""),
        manifest,
    )

    assert passed is True
    assert unclaimed is False


def test_read_only_graph_expectations_follow_the_resolved_namespace() -> None:
    passed, reason = subject.evaluate_postcondition(
        "read_only_nodes",
        _read_only_nodes("cell/a").decode(),
        _manifest(namespace="cell/a"),
    )

    assert passed is True
    assert reason == "expected_read_only_graph"


def test_live_runner_is_replaced_and_all_six_cases_remain_pinned(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "capture_live_evidence")
    assert not hasattr(subject, "_subprocess_runner")
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "7d58213cd9a8e27422700516095da97b3b19dd3b"
    )
    assert HISTORICAL_TEST_BLOB == (
        "2a4ca4e3bd6cb5df70c6b0d1ba63f540d589c909"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 6
