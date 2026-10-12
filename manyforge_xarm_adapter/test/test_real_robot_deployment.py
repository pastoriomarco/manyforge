"""Disposition and fail-before-effects tests for the old Lite6 resolver.

Historical test blob ``bd97dc8bd64179f3db1fb299b97ea28a4064798e``
contains six acceptance families.  Their restoration assignments are kept
explicit here so removing the old runnable contract does not erase the work:

* template resolution plus complete artifact/runtime record: I3, I6-I8, I10/R;
* artifact-drift rejection: I3 identity plus I10/R product admission;
* authority-health-policy drift: I7/I8 plus I10/R;
* control-owner/Composer process separation: I4 plus I10/R;
* immutable-image-scoped build cache: I3 retention plus I10/R; and
* bounded lifecycle-history capture: I5/H0 observability plus I10/R.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

import pytest

from manyforge_xarm_adapter.real_robot_deployment import (
    HISTORICAL_SOURCE_BLOB,
    HISTORICAL_SOURCE_COMMIT,
    REAL_ROBOT_DEPLOYMENT_RESTORATION_GATES,
    REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE,
    RealRobotDeploymentUnavailableError,
    XARM_ROS2_REFERENCE_COMMIT,
    main,
    resolve_real_robot_deployment,
)


HISTORICAL_TEST_BLOB = "bd97dc8bd64179f3db1fb299b97ea28a4064798e"
HISTORICAL_CASE_DISPOSITION = {
    "resolve_template_and_runtime_record": ("I3", "I6", "I7", "I8", "I10", "R"),
    "reject_artifact_drift": ("I3", "I10", "R"),
    "reject_authority_health_policy_drift": ("I7", "I8", "I10", "R"),
    "separate_control_owner_from_composer": ("I4", "I10", "R"),
    "bind_build_cache_to_immutable_image": ("I3", "I10", "R"),
    "retain_bounded_lifecycle_history": ("I5", "H0", "I10", "R"),
}

_STATIC_VALIDATOR_PATH = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "models"
    / "validate_lite6_authority_runtime_contract.py"
)
_STATIC_VALIDATOR_SPEC = importlib.util.spec_from_file_location(
    "validate_lite6_static_runtime_artifacts",
    _STATIC_VALIDATOR_PATH,
)
assert (
    _STATIC_VALIDATOR_SPEC is not None
    and _STATIC_VALIDATOR_SPEC.loader is not None
)
_STATIC_VALIDATOR = importlib.util.module_from_spec(_STATIC_VALIDATOR_SPEC)
sys.modules[_STATIC_VALIDATOR_SPEC.name] = _STATIC_VALIDATOR
_STATIC_VALIDATOR_SPEC.loader.exec_module(_STATIC_VALIDATOR)


class _PoisonInput:
    def __getattribute__(self, _name: str) -> object:
        raise AssertionError("the unavailable gate inspected caller input")


def test_resolver_fails_before_inspecting_any_old_contract_input() -> None:
    poison = _PoisonInput()

    with pytest.raises(
        RealRobotDeploymentUnavailableError,
        match=REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE,
    ):
        resolve_real_robot_deployment(
            cell_path=poison,
            template_path=poison,
            program_path=poison,
            model_root=poison,
            runtime_binding_path=poison,
            output_deployment_path=poison,
            output_record_path=poison,
        )


def test_cli_reports_the_stable_i10_gate_without_argument_access(capsys) -> None:
    assert main() == 78
    assert REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE in capsys.readouterr().err


def test_historical_sources_and_all_six_case_families_remain_pinned() -> None:
    assert HISTORICAL_SOURCE_COMMIT == (
        "08222188293df8e12f90dc72e204c21f22f92ea1"
    )
    assert HISTORICAL_SOURCE_BLOB == (
        "20b4a9d4d7930dc550f6931a4f32c17c585e8212"
    )
    assert HISTORICAL_TEST_BLOB == (
        "bd97dc8bd64179f3db1fb299b97ea28a4064798e"
    )
    assert XARM_ROS2_REFERENCE_COMMIT == (
        "c3172e57c1c1bb2982fc008304ad6bc886357c87"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 6
    assigned_gates = {
        gate
        for gates in HISTORICAL_CASE_DISPOSITION.values()
        for gate in gates
    }
    assert assigned_gates <= set(REAL_ROBOT_DEPLOYMENT_RESTORATION_GATES) | {
        "I4",
        "I5",
        "H0",
    }


@pytest.fixture
def supplied_static_description(tmp_path: Path) -> Path:
    root = _STATIC_VALIDATOR.REPO_ROOT
    # Explicit static validator fixture, not the host generator or ROS proof.
    model = ET.parse(root / "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf")
    for control in model.getroot().findall("ros2_control"):
        model.getroot().remove(control)
    control = ET.SubElement(model.getroot(), "ros2_control", name="fixture", type="system")
    ET.SubElement(ET.SubElement(control, "hardware"), "plugin").text = "topic_based_ros2_control/TopicBasedSystem"
    for name in _STATIC_VALIDATOR.ARM_JOINTS:
        joint = ET.SubElement(control, "joint", name=name)
        ET.SubElement(joint, "command_interface", name="position")
        for state in ("position", "velocity"):
            ET.SubElement(joint, "state_interface", name=state)
    supplied_description = tmp_path / "supplied_description.urdf"
    model.write(supplied_description, encoding="utf-8", xml_declaration=True)

    return supplied_description


def test_owner_neutral_static_artifact_validation_survives(
    supplied_static_description: Path,
) -> None:
    root = _STATIC_VALIDATOR.REPO_ROOT
    report = _STATIC_VALIDATOR.validate_static_artifacts(
        ros2_control_urdf=supplied_static_description,
        controllers=(
            root
            / "manyforge_ros_bringup"
            / "config"
            / "isaac_lite6_topic_based_controllers.yaml"
        ),
        gripper_bridge=(
            root
            / "manyforge_ros_bringup"
            / "config"
            / "isaac_lite6_gripper_bridge.yaml"
        ),
    )

    assert report["arm_joints"] == [f"joint{index}" for index in range(1, 7)]
    assert report["gripper_command_joint"] == "right_finger_joint"
    assert report["gripper_derived_joint"] == "left_finger_joint"
    assert "schema" not in report
    assert "deployment" not in report


def test_old_deployment_authority_validator_fails_before_input_access() -> None:
    poison = _PoisonInput()
    reason_code = (
        _STATIC_VALIDATOR.LEGACY_DEPLOYMENT_AUTHORITY_UNAVAILABLE_REASON_CODE
    )

    with pytest.raises(
        _STATIC_VALIDATOR.LegacyDeploymentAuthorityUnavailableError,
        match=reason_code,
    ):
        _STATIC_VALIDATOR.validate_legacy_deployment_authority(poison)


@pytest.mark.parametrize(
    "arguments",
    (
        (),
        ("--deployment", "/must/not/be/read.yaml"),
        ("--deployment=/must/not/be/read.yaml",),
        ("--json",),
        ("--ros2-control-urdf", "/must/not/be/read.urdf"),
    ),
)
def test_historical_static_validator_cli_is_typed_unavailable_before_parse(
    arguments: tuple[str, ...],
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def reject_argument_parsing(*_args: object, **_kwargs: object) -> None:
        pytest.fail("historical Lite6 command reached argparse before its gate")

    monkeypatch.setattr(
        _STATIC_VALIDATOR.argparse.ArgumentParser,
        "parse_args",
        reject_argument_parsing,
    )

    assert _STATIC_VALIDATOR.main(arguments) == 78
    assert (
        _STATIC_VALIDATOR.LEGACY_DEPLOYMENT_AUTHORITY_UNAVAILABLE_REASON_CODE
        in capsys.readouterr().err
    )


def test_static_validator_cli_requires_explicit_static_artifact_mode(
    capsys: pytest.CaptureFixture[str],
    supplied_static_description: Path,
) -> None:
    assert _STATIC_VALIDATOR.main(
        (_STATIC_VALIDATOR.STATIC_ARTIFACT_MODE, "--ros2-control-urdf", str(supplied_static_description))
    ) == 0
    assert (
        "Lite6 static ROS/control/gripper artifacts passed."
        in capsys.readouterr().out
    )


def test_static_validator_requires_a_supplied_robot_description() -> None:
    with pytest.raises(SystemExit) as failure:
        _STATIC_VALIDATOR.main(["static-artifacts"])
    assert failure.value.code == 2
