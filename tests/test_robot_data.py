"""E1 robots as data: the Mock controller rows and `check_robot.py`."""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "robots"))

import check_robot  # noqa: E402
import mock_controller_profiles  # noqa: E402

ROWS = ROOT / mock_controller_profiles.DEFAULT_PATH
SHIPPED = ("lite6_lite_gripper", "uf850_xarm_gripper", "ur10e_robotiq_2f_140", "ur5e_robotiq_2f_85")


def _bundle(family: str) -> Path:
    return ROOT / "manyforge_example_models" / "models" / family


def _write(tmp_path: Path, document: object) -> Path:
    path = tmp_path / "rows.yaml"
    path.write_text(yaml.safe_dump(document), encoding="utf-8")
    return path


def _lite6_row() -> dict[str, object]:
    return yaml.safe_load(ROWS.read_text(encoding="utf-8"))["mock_profiles"][0]


def test_rows_keep_the_values_of_the_former_cpp_table() -> None:
    # The C++ initializers of the four rows that predate the data file, as they
    # were in controller_launch_artifacts.cpp; the generated rows must match.
    former = {
        "lite6_lite_gripper": '{"lite6_lite_gripper", "MockLite6LiteGripperSystem", ArmTemplate::kUFactory, "lite6_traj_controller", VendorJointConstraints{"1.0", "0.01"}, "lite_gripper_controller", true, 1.5, 5.0},',
        "ur10e_robotiq_2f_140": '{"ur10e_robotiq_2f_140", "MockUR10eRobotiq2F140System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller", true, 0.5, 15.0},',
        "ur5_roboplan": '{"ur5_roboplan", "MockUR5System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "", false, 0.5, 15.0},',
        "ur5e_robotiq_2f_85": '{"ur5e_robotiq_2f_85", "MockUR5eRobotiq2F85System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller", true, 0.5, 15.0},',
    }
    rows = mock_controller_profiles.load(ROWS)
    lines = mock_controller_profiles.cpp_rows(rows, "x").splitlines()[1:]
    generated = {row.mock_profile: line for row, line in zip(rows, lines)}
    for profile, line in former.items():
        assert generated[profile] == line


def test_uf850_row_loads_from_the_data_file() -> None:
    rows = {row.mock_profile: row for row in mock_controller_profiles.load(ROWS)}
    uf850 = rows["uf850_xarm_gripper"]
    assert uf850.template == "ufactory"
    assert uf850.arm_controller == "uf850_traj_controller"
    assert uf850.vendor_joint_constraints == ("1.0", "0.01")
    assert uf850.gripper_controller == "xarm_gripper_controller"


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"template": "kuka"}, "template must be one of"),
        ({"arm_controller": "two words"}, "one ROS name token"),
        ({"vendor_joint_constraints": None}, "copied from the vendor"),
        ({"hard_stop_completion_timeout_s": 0}, "positive number of seconds"),
        ({"extra": 1}, "unknown fields extra"),
        ({"mock_profile": "Lite6"}, "lower-case portable id"),
    ],
)
def test_invalid_rows_name_the_field(tmp_path: Path, change: dict, message: str) -> None:
    row = {**_lite6_row(), **change}
    row = {key: value for key, value in row.items() if value is not None}
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match=message):
        mock_controller_profiles.load(_write(tmp_path, {"mock_profiles": [row]}))


def test_ur_template_rejects_vendor_constraints_and_duplicates_are_refused(tmp_path: Path) -> None:
    ur = {**_lite6_row(), "template": "ur"}
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match="must be absent"):
        mock_controller_profiles.load(_write(tmp_path, {"mock_profiles": [ur]}))
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match="more than once"):
        mock_controller_profiles.load(
            _write(tmp_path, {"mock_profiles": [_lite6_row(), _lite6_row()]})
        )


@pytest.mark.parametrize("value", ["1.0e+999", "1e400", 1e400])
def test_vendor_constraints_must_be_finite(tmp_path: Path, value: object) -> None:
    row = {**_lite6_row(), "vendor_joint_constraints": {"trajectory": value, "goal": "0.01"}}
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match="finite"):
        mock_controller_profiles.load(_write(tmp_path, {"mock_profiles": [row]}))


def test_duplicate_keys_are_refused(tmp_path: Path) -> None:
    path = tmp_path / "rows.yaml"
    path.write_text(
        ROWS.read_text(encoding="utf-8").replace(
            "    arm_controller: lite6_traj_controller\n",
            "    arm_controller: lite6_traj_controller\n    arm_controller: other\n",
            1,
        ),
        encoding="utf-8",
    )
    with pytest.raises(
        mock_controller_profiles.MockControllerProfileError,
        match=r"line \d+: duplicate key 'arm_controller'",
    ):
        mock_controller_profiles.load(path)
    assert mock_controller_profiles.main(["--input", str(path)]) == 1


def test_malformed_types_raise_the_typed_error(tmp_path: Path) -> None:
    for row, message in (
        ({**_lite6_row(), "template": []}, r"\(lite6_lite_gripper\)\.template must be one of"),
        ({**_lite6_row(), "template": {"a": 1}}, r"\.template must be one of"),
        ({**_lite6_row(), 7: "x"}, r"mock_profiles\[0\] has non-text field names \[7\]"),
    ):
        with pytest.raises(mock_controller_profiles.MockControllerProfileError, match=message):
            mock_controller_profiles.load(_write(tmp_path, {"mock_profiles": [row]}))


def test_unquoted_vendor_constraints_keep_their_text(tmp_path: Path) -> None:
    path = tmp_path / "rows.yaml"
    path.write_text(
        ROWS.read_text(encoding="utf-8").replace(
            '{trajectory: "1.0", goal: "0.01"}', "{trajectory: 1.0, goal: 0.01}"
        ),
        encoding="utf-8",
    )
    assert mock_controller_profiles.load(path) == mock_controller_profiles.load(ROWS)


@pytest.mark.parametrize("family", ["lite6_lite_gripper", "ur10e_robotiq_2f_140", "ur5e_robotiq_2f_85"])
def test_shipped_robots_pass(family: str) -> None:
    report = check_robot.check(ROOT, _bundle(family))
    assert report.errors == []


def test_shipped_scenarios_pass() -> None:
    for family, scenario in (
        ("lite6_lite_gripper", "lite6_pick_and_place"),
        ("ur10e_robotiq_2f_140", "ur10e_pick_and_place"),
        ("ur5e_robotiq_2f_85", "ur5e_lite6_wall_grouped"),
    ):
        report = check_robot.check(
            ROOT, _bundle(family), ROOT / "examples" / f"{scenario}.scenario"
        )
        assert report.errors == [], (family, scenario)


def test_uf850_data_loads() -> None:
    # The family, its Mock profile row, controller row and generated component
    # types load and join. Until the closed schema accepts the id, Apply still
    # refuses it, and the checker says so.
    report = check_robot.check(ROOT, _bundle("uf850_xarm_gripper"))
    remaining = [error for error in report.errors if "mock_profile enum" not in error]
    assert remaining == []


def _copy_workspace(tmp_path: Path, family: str) -> Path:
    """A minimal workspace with one bundle, the description and the rows."""
    root = tmp_path / "ws"
    for relative in (
        check_robot.DESCRIPTION,
        mock_controller_profiles.DEFAULT_PATH,
        check_robot.RUNTIME_SCHEMA,
        f"{check_robot.COMPONENT_TYPES}/{family}.component.json",
        f"{check_robot.COMPONENT_TYPES}/{family}.fixed_scale.component.json",
    ):
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / relative, root / relative)
    shutil.copytree(_bundle(family), root / "manyforge_example_models/models" / family)
    return root


def test_broken_data_gives_clear_errors(tmp_path: Path) -> None:
    root = _copy_workspace(tmp_path, "lite6_lite_gripper")
    description = json.loads((root / check_robot.DESCRIPTION).read_text())
    family = next(
        row for row in description["model_families"]
        if row["model_family_id"] == "lite6_lite_gripper"
    )
    for binding in family["semantics"]["frame_role_bindings"]:
        if binding["frame_role_id"] == "tcp":
            binding["source_link_name"] = "link_tcpx"
    for binding in family["semantics"]["resource_role_bindings"]:
        if binding["resource_role_id"] == "arm_joints":
            binding["source_joint_names"][0] = "joint_eef"
    (root / check_robot.DESCRIPTION).write_text(json.dumps(description))
    rows = root / mock_controller_profiles.DEFAULT_PATH
    rows.write_text(rows.read_text().replace("    gripper_controller: lite_gripper_controller\n", "", 1))
    errors = check_robot.check(root, root / "manyforge_example_models/models/lite6_lite_gripper").errors
    assert any("frame role 'tcp': link 'link_tcpx' is not in" in error for error in errors)
    assert any("joint 'joint_eef' is fixed, not a commanded joint" in error for error in errors)
    assert any("needs gripper_controller" in error for error in errors)


def test_unknown_folder_and_missing_mock_row(tmp_path: Path) -> None:
    root = _copy_workspace(tmp_path, "lite6_lite_gripper")
    errors = check_robot.check(root, tmp_path).errors
    assert errors and "0 model family rows" in errors[0]
    rows = root / mock_controller_profiles.DEFAULT_PATH
    rows.write_text(rows.read_text().replace("mock_profile: lite6_lite_gripper", "mock_profile: lite6_other"))
    errors = check_robot.check(root, root / "manyforge_example_models/models/lite6_lite_gripper").errors
    assert any("has no row in" in error for error in errors)


def test_command_line_exit_status() -> None:
    assert check_robot.main([str(_bundle("lite6_lite_gripper"))]) == 0
