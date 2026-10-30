"""E1 robots as data: the Mock controller rows (`manyforge component` tests:
test_component_descriptor.py)."""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from scripts.components import mock_controller_profiles

ROOT = Path(__file__).resolve().parents[1]

ROWS = ROOT / mock_controller_profiles.DEFAULT_PATH

def _write(tmp_path: Path, document: object) -> Path:
    path = tmp_path / "rows.yaml"
    path.write_text(yaml.safe_dump(document), encoding="utf-8")
    return path


def _lite6_row() -> dict[str, object]:
    return yaml.safe_load(ROWS.read_text(encoding="utf-8"))["mock_profiles"][0]


def test_rows_keep_the_values_of_the_former_cpp_table() -> None:
    # The C++ initializers of the four rows that predate the data file, as they
    # were in controller_launch_artifacts.cpp, plus (merge 6) the Mock arrival
    # tolerance; the vendor goal 0.01 moved into it. The generated rows match.
    former = {
        "lite6_lite_gripper": '{"lite6_lite_gripper", "MockLite6LiteGripperSystem", ArmTemplate::kUFactory, "lite6_traj_controller", VendorJointConstraints{"1.0"}, "lite_gripper_controller", true, 1.5, 5.0, 0.01},',
        "ur10e_robotiq_2f_140": '{"ur10e_robotiq_2f_140", "MockUR10eRobotiq2F140System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller", true, 0.5, 15.0, 0.01},',
        "ur5_roboplan": '{"ur5_roboplan", "MockUR5System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "", false, 0.5, 15.0, 0.01},',
        "ur5e_robotiq_2f_85": '{"ur5e_robotiq_2f_85", "MockUR5eRobotiq2F85System", ArmTemplate::kUr, "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller", true, 0.5, 15.0, 0.01},',
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
    assert uf850.vendor_joint_constraints == ("1.0",)
    assert uf850.arrival_tolerance_rad == 0.01
    assert uf850.gripper_controller == "xarm_gripper_controller"


def test_every_mock_row_has_the_decided_arrival_tolerance() -> None:
    # Merge 6 arrival (maintainer 2026-10-28): Mock 0.01 rad, as Real, on
    # every row with an arm; a gripper-only (tool) row has none.
    rows = mock_controller_profiles.load(ROWS)
    arms = [row for row in rows if row.arm_controller is not None]
    assert arms and all(row.arrival_tolerance_rad == 0.01 for row in arms)
    assert all(row.arrival_tolerance_rad is None for row in rows if row.arm_controller is None)


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"template": "kuka"}, "template must be one of"),
        ({"arm_controller": "two words"}, "one ROS name token"),
        ({"vendor_joint_constraints": None}, "copied from the vendor"),
        ({"hard_stop_completion_timeout_s": 0}, "positive number of seconds"),
        ({"arrival_tolerance_rad": 0}, "positive number of radians"),
        ({"arrival_tolerance_rad": None}, "missing arrival_tolerance_rad"),
        ({"vendor_joint_constraints": {"trajectory": "1.0", "goal": "0.01"}}, "copied from the vendor"),
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
    row = {**_lite6_row(), "vendor_joint_constraints": {"trajectory": value}}
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
            '{trajectory: "1.0"}', "{trajectory: 1.0}"
        ),
        encoding="utf-8",
    )
    assert mock_controller_profiles.load(path) == mock_controller_profiles.load(ROWS)
