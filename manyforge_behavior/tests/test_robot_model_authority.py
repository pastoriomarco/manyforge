from __future__ import annotations

from pathlib import Path
import time
import xml.etree.ElementTree as ET

import pytest

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    GripperCommandRequest,
    SceneEditRequest,
)
from manyforge_behavior.model_paths import ResolvedRobotModel
from manyforge_behavior.robot_model_authority import (
    MimicLaw,
    RobotModelAuthorityError,
    RobotModelGripperConfig,
    build_roboplan_import_view,
    load_robot_model_authority,
    robot_model_asset_descriptor,
    robot_model_instance_view,
)
from manyforge_behavior.robot_model_runtime import prepare_roboplan_runtime_model_view


REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = REPO_ROOT / "manyforge_example_models" / "models"


def _write_model(
    tmp_path: Path,
    urdf_body: str,
    srdf_body: str = "",
) -> tuple[Path, Path | None]:
    urdf = tmp_path / "robot.urdf"
    urdf.write_text(urdf_body, encoding="utf-8")
    if not srdf_body:
        return urdf, None
    srdf = tmp_path / "robot.srdf"
    srdf.write_text(srdf_body, encoding="utf-8")
    return urdf, srdf


def test_lite6_canonical_mimic_and_gripper_metadata():
    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "lite6_lite_gripper.urdf",
        srdf_path=model_dir / "lite6_lite_gripper.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="lite6_lite_gripper",
        grippers={
            "lite_gripper": RobotModelGripperConfig(
                group_id="lite_gripper",
                joint_name="right_finger_joint",
                touch_links=(
                    "lite_gripper_left_finger",
                    "lite_gripper_right_finger",
                ),
            )
        },
    )

    assert authority.joints["left_finger_joint"].mimic == MimicLaw(
        master="right_finger_joint",
        multiplier=1.0,
        offset=0.0,
    )
    assert authority.joints["left_finger_joint"].is_derived
    assert not authority.joints["left_finger_joint"].is_commandable
    assert authority.joints["right_finger_joint"].is_commandable
    assert authority.joints["right_finger_joint"].is_mimic_master
    assert authority.group_commandable_joints("lite_gripper") == (
        "right_finger_joint",
    )
    assert authority.grippers["lite_gripper"].derived_joints == (
        "left_finger_joint",
    )
    assert authority.grippers["lite_gripper"].presets == {
        "open": 0.0081,
        "close": 0.0,
        "closed": 0.0,
    }
    assert any(
        pair.link1 == "lite_gripper_left_finger"
        and pair.link2 == "lite_gripper_right_finger"
        for pair in authority.disabled_collisions
    )


def test_lite6_roboplan_import_view_renames_order_sensitive_mimic_pair(tmp_path):
    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "lite6_lite_gripper.urdf",
        srdf_path=model_dir / "lite6_lite_gripper.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="lite6_lite_gripper",
    )

    view = build_roboplan_import_view(authority)
    repeat = build_roboplan_import_view(authority)

    right_view = view.view_joint_name("right_finger_joint")
    left_view = view.view_joint_name("left_finger_joint")
    assert view.changed
    assert right_view != "right_finger_joint"
    assert left_view != "left_finger_joint"
    assert right_view < left_view
    assert view.canonical_joint_name(right_view) == "right_finger_joint"
    assert view.canonical_joint_name(left_view) == "left_finger_joint"
    assert view.view_fingerprint == repeat.view_fingerprint
    assert view.canonical_fingerprint == authority.canonical_fingerprint

    urdf_root = ET.fromstring(view.urdf_xml)
    left_joint = next(
        joint
        for joint in urdf_root.findall(".//joint")
        if joint.attrib.get("name") == left_view
    )
    mimic = left_joint.find("mimic")
    assert mimic is not None
    assert mimic.attrib["joint"] == right_view

    srdf_root = ET.fromstring(view.srdf_xml)
    gripper_joint_names = [
        joint.attrib["name"]
        for group in srdf_root.findall("group")
        if group.attrib.get("name") == "lite_gripper"
        for joint in group.findall("joint")
    ]
    assert gripper_joint_names == [left_view, right_view]

    urdf_path, srdf_path = view.write_to_directory(tmp_path, stem="lite6_rma_view")
    assert urdf_path.read_text(encoding="utf-8") == view.urdf_xml
    assert srdf_path.read_text(encoding="utf-8") == view.srdf_xml


def test_robotiq_gripper_mimics_are_model_derived():
    model_dir = MODEL_ROOT / "ur10e_robotiq_2f_140"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "ur10e_robotiq_2f_140.urdf",
        srdf_path=model_dir / "ur10e_robotiq_2f_140.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="ur10e_robotiq_2f_140",
        grippers={
            "robotiq": {
                "group_id": "gripper",
                "joint_name": "finger_joint",
            }
        },
    )

    assert authority.joints["finger_joint"].is_commandable
    assert authority.joints["finger_joint"].is_mimic_master
    assert "finger_joint" not in authority.derived_joint_names
    assert authority.grippers["robotiq"].presets == {
        "open": 0.0,
        "closed": 0.66,
    }
    assert authority.grippers["robotiq"].derived_joints == ()
    assert authority.joints["left_inner_finger_joint"].mimic == MimicLaw(
        master="finger_joint",
        multiplier=1.0,
        offset=0.0,
    )
    assert authority.joints["right_outer_knuckle_joint"].mimic == MimicLaw(
        master="finger_joint",
        multiplier=-1.0,
        offset=0.0,
    )
    assert authority.joints["right_inner_knuckle_joint"].mimic == MimicLaw(
        master="finger_joint",
        multiplier=-1.0,
        offset=0.0,
    )


def test_robotiq_roboplan_import_view_keeps_safe_canonical_names():
    model_dir = MODEL_ROOT / "ur10e_robotiq_2f_140"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "ur10e_robotiq_2f_140.urdf",
        srdf_path=model_dir / "ur10e_robotiq_2f_140.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="ur10e_robotiq_2f_140",
    )

    view = build_roboplan_import_view(authority)

    assert not view.changed
    assert view.view_joint_name("finger_joint") == "finger_joint"
    assert view.view_joint_name("left_inner_finger_joint") == "left_inner_finger_joint"


def test_lite6_import_view_loads_native_kernel_when_binding_available(tmp_path):
    try:
        import manyforge_roboplan_py
    except ImportError:
        pytest.skip("manyforge_roboplan_py not available")

    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "lite6_lite_gripper.urdf",
        srdf_path=model_dir / "lite6_lite_gripper.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="lite6_lite_gripper",
    )
    view = build_roboplan_import_view(authority)
    urdf_path, srdf_path = view.write_to_directory(tmp_path, stem="lite6_rma_view")

    bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
        "lite6_lite_gripper",
        "lite6",
        str(urdf_path),
        str(srdf_path),
        [str(REPO_ROOT)],
    )

    assert bundle is not None


def test_lite6_canonical_gripper_command_uses_roboplan_view_mapping_when_binding_available():
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient
    except ImportError:
        pytest.skip("manyforge_roboplan_py/native client not available")

    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    runtime_model = prepare_roboplan_runtime_model_view(
        resolved_model=ResolvedRobotModel(
            model_name="lite6_lite_gripper",
            urdf_path=str(model_dir / "lite6_lite_gripper.urdf"),
            srdf_path=str(model_dir / "lite6_lite_gripper.srdf"),
            package_paths=[str(REPO_ROOT)],
        ),
        grippers={
            "lite_gripper": {
                "group_id": "lite_gripper",
                "joint_name": "right_finger_joint",
            }
        },
    )
    bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
        runtime_model.model_name,
        "lite6",
        runtime_model.urdf_path,
        runtime_model.srdf_path,
        runtime_model.package_paths,
    )
    kernel = NativeKernelClient(
        bundle,
        robot_model_authority_metadata=runtime_model.metadata,
        robot_model_authority_temp_dir=runtime_model._temp_dir,
    )

    assert runtime_model.metadata["grippers"]["lite_gripper"] == {
        "group_id": "lite_gripper",
        "command_joint": "right_finger_joint",
        "derived_joints": ["left_finger_joint"],
        "touch_links": [],
        "mount_frame": "",
        "attach_frame": "",
        "tcp_frame": "",
        "auxiliary_frames": [],
        "presets": {"open": 0.0081, "close": 0.0, "closed": 0.0},
    }
    assert runtime_model.import_view.view_joint_name("right_finger_joint") != (
        "right_finger_joint"
    )
    assert bundle.get_group_joint_names("lite_gripper") == [
        "left_finger_joint",
        "right_finger_joint",
    ]
    assert bundle.get_group_joint_positions("lite_gripper") == pytest.approx(
        [0.0, 0.0]
    )
    assert bundle.get_group_commandable_joint_positions(
        "lite_gripper"
    ) == pytest.approx([0.0])

    result = kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="lite_gripper",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0081,
            duration_s=0.0,
        )
    )

    assert result.accepted is True, result.error
    assert bundle.get_group_joint_positions("lite_gripper") == pytest.approx(
        [0.0081, 0.0081],
        abs=1e-4,
    )
    assert bundle.get_group_commandable_joint_positions(
        "lite_gripper"
    ) == pytest.approx([0.0081], abs=1e-4)

    bundle.set_group_joint_positions("lite_gripper", [0.0, 0.0])
    assert bundle.get_group_joint_positions("lite_gripper") == pytest.approx(
        [0.0, 0.0],
        abs=1e-4,
    )
    assert bundle.get_group_commandable_joint_positions(
        "lite_gripper"
    ) == pytest.approx([0.0], abs=1e-4)

    comparison = kernel.collision_compare_positions([0.0] * 6)
    assert comparison["ok"] is True, comparison
    assert comparison["manyforge_collision"]["engine_status"] == "ok"
    assert comparison["manyforge_collision"]["decision"] == "pass"


def test_lite6_arm_pick_validation_preserves_open_gripper_state_when_binding_available():
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient
    except ImportError:
        pytest.skip("manyforge_roboplan_py/native client not available")

    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    runtime_model = prepare_roboplan_runtime_model_view(
        resolved_model=ResolvedRobotModel(
            model_name="lite6_lite_gripper",
            urdf_path=str(model_dir / "lite6_lite_gripper.urdf"),
            srdf_path=str(model_dir / "lite6_lite_gripper.srdf"),
            package_paths=[str(REPO_ROOT)],
        ),
        grippers={
            "primary": {
                "group_id": "lite_gripper",
                "joint_name": "right_finger_joint",
            }
        },
    )
    bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
        runtime_model.model_name,
        "lite6",
        runtime_model.urdf_path,
        runtime_model.srdf_path,
        runtime_model.package_paths,
    )
    kernel = NativeKernelClient(
        bundle,
        robot_model_authority_metadata=runtime_model.metadata,
        robot_model_authority_temp_dir=runtime_model._temp_dir,
    )

    def wait_for_terminal(timeout_s: float = 30.0):
        deadline = time.monotonic() + timeout_s
        last_status = kernel.get_status()
        while time.monotonic() < deadline:
            last_status = kernel.get_status()
            if last_status.state in {
                ExecutionState.COMPLETED,
                ExecutionState.FAILED,
                ExecutionState.STOPPED,
            }:
                return last_status
            time.sleep(0.01)
        pytest.fail(f"timed out waiting for terminal state: {last_status.state}")

    def add_box(object_id: str, dimensions: list[float], position: list[float]) -> None:
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id=object_id,
                shape_type="box",
                box_dimensions_m=dimensions,
                pose_position_m=position,
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        )
        assert result.success, result.error

    def execute_relative(move_id: str, z_offset_m: float) -> None:
        request = ExecuteRequest(
            robot_id="lite6",
            group_id="lite6",
            move_id=move_id,
            base_frame="link_base",
            tcp_frame="link_tcp",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset_reference": {"type": "frame", "id": "link_base"},
                "offset": {
                    "position_m": [0.0, 0.0, z_offset_m],
                    "orientation_rpy_deg": [180.0, 0.0, 0.0],
                },
            },
            velocity_scale=0.4,
            planning_timeout_s=10.0,
            execution_timeout_s=30.0,
            cache_mode="disable",
        )
        result = kernel.execute(request)
        assert result.ok, result.error
        terminal = wait_for_terminal()
        snapshot = kernel.get_execution_snapshot()
        assert terminal.state == ExecutionState.COMPLETED, snapshot.error_details
        ack = kernel.acknowledge_completed(terminal.execution_id)
        assert ack.accepted, ack.error

    bundle.set_joint_positions([0.0, 0.0, 1.57, 0.0, 0.0, 0.0])
    add_box("ground", [1.0, 1.0, 0.1], [0.0, 0.0, -0.051])
    add_box("wall", [1.0, 0.02, 0.2], [0.0, -0.15, 0.1])
    add_box("graspable", [0.06, 0.015, 0.015], [0.18, -0.20, 0.08])

    gripper = kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="primary",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0081,
            max_effort=10.0,
            duration_s=0.0,
        )
    )
    assert gripper.accepted, gripper.error
    assert bundle.get_group_joint_positions("lite_gripper") == pytest.approx(
        [0.0081, 0.0081],
        abs=1e-4,
    )

    execute_relative("lite6_pick_approach", 0.08)
    execute_relative("lite6_pick_down", 0.035)


def test_lite6_manyforge_collision_matches_legacy_for_open_gripper_pick_waypoint():
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient
    except ImportError:
        pytest.skip("manyforge_roboplan_py/native client not available")

    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    runtime_model = prepare_roboplan_runtime_model_view(
        resolved_model=ResolvedRobotModel(
            model_name="lite6_lite_gripper",
            urdf_path=str(model_dir / "lite6_lite_gripper.urdf"),
            srdf_path=str(model_dir / "lite6_lite_gripper.srdf"),
            package_paths=[str(REPO_ROOT)],
        ),
        grippers={
            "primary": {
                "group_id": "lite_gripper",
                "joint_name": "right_finger_joint",
            }
        },
    )
    bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
        runtime_model.model_name,
        "lite6",
        runtime_model.urdf_path,
        runtime_model.srdf_path,
        runtime_model.package_paths,
    )
    kernel = NativeKernelClient(
        bundle,
        robot_model_authority_metadata=runtime_model.metadata,
        robot_model_authority_temp_dir=runtime_model._temp_dir,
    )

    for request in (
        SceneEditRequest(
            operation="add",
            object_id="ground",
            shape_type="box",
            box_dimensions_m=[1.0, 1.0, 0.1],
            pose_position_m=[0.0, 0.0, -0.051],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ),
        SceneEditRequest(
            operation="add",
            object_id="wall",
            shape_type="box",
            box_dimensions_m=[1.0, 0.02, 0.2],
            pose_position_m=[0.0, -0.15, 0.1],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ),
        SceneEditRequest(
            operation="add",
            object_id="graspable",
            shape_type="box",
            box_dimensions_m=[0.06, 0.015, 0.015],
            pose_position_m=[0.18, -0.25, 0.08],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ),
    ):
        result = kernel.scene_edit(request)
        assert result.success, result.error

    result = kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="primary",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0081,
            max_effort=10.0,
            duration_s=0.0,
        )
    )
    assert result.accepted, result.error
    assert bundle.get_group_joint_positions("lite_gripper") == pytest.approx(
        [0.0081, 0.0081],
        abs=1e-4,
    )

    # Regression for the live Lite6 false positive seen on lite6_pick_down:
    # the legacy RoboPlan scene accepted this open-gripper waypoint while the
    # snapshot-backed ManyForge collision engine used a conservative robot-mesh
    # AABB proxy and reported graspable <-> finger collision.
    failing_waypoint = [
        -0.94676919298494788,
        0.70804426974298251,
        1.1681676704489288,
        4.6425930722353571e-06,
        0.46012187095654034,
        -0.9467541851191329,
        0.0081,
    ]
    comparison = bundle.collision_compare_positions(failing_waypoint, 66)
    assert comparison["ok"] is True, comparison
    assert comparison["legacy"]["decision"] == "pass"
    assert comparison["manyforge_collision"]["decision"] == "pass", comparison
    assert comparison["match"] is True, comparison


def test_lite6_asset_descriptor_and_instance_view_preserve_mimic_gripper_maps():
    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "lite6_lite_gripper.urdf",
        srdf_path=model_dir / "lite6_lite_gripper.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="lite6_lite_gripper",
        grippers={
            "lite_gripper": RobotModelGripperConfig(
                group_id="lite_gripper",
                joint_name="right_finger_joint",
                touch_links=(
                    "lite_gripper_left_finger",
                    "lite_gripper_right_finger",
                ),
                mount_frame="lite_gripper_base_link",
                attach_frame="lite_gripper_base_link",
                tcp_frame="link_tcp",
            )
        },
    )

    descriptor = robot_model_asset_descriptor(authority, asset_id="lite6_asset")
    view = robot_model_instance_view(
        authority,
        asset_id="lite6_asset",
        instance_id="left_lite6",
        prefix="L_",
        mount_parent_frame="cell",
        mount_xyz=(0.1, 0.2, 0.3),
        controller_bindings={"lite6": "left_arm_controller"},
        state_sources={"joint_states": "/left/joint_states"},
    )

    assert descriptor.asset_id == "lite6_asset"
    assert descriptor.canonical_fingerprint == authority.canonical_fingerprint
    assert "lite_gripper" in descriptor.end_effectors
    assert ("lite_gripper", "open") in descriptor.group_states
    assert view.links.generated("link_tcp") == "L_link_tcp"
    assert view.joints.generated("right_finger_joint") == "L_right_finger_joint"
    assert view.groups.generated("lite6") == "L_lite6"
    assert view.group_states.generated("lite_gripper/open") == "L_lite_gripper/L_open"
    assert view.end_effectors.generated("lite_gripper") == "L_lite_gripper"
    assert view.spec.mount_parent_frame == "cell"
    assert view.spec.mount_xyz == pytest.approx((0.1, 0.2, 0.3))
    assert view.spec.controller_bindings["lite6"] == "left_arm_controller"
    assert view.spec.state_sources["joint_states"] == "/left/joint_states"

    mimic = next(
        law
        for law in view.generated_mimic_laws
        if law.source_joint == "left_finger_joint"
    )
    assert mimic.generated_joint == "L_left_finger_joint"
    assert mimic.generated_master == "L_right_finger_joint"
    assert mimic.multiplier == pytest.approx(1.0)
    assert mimic.offset == pytest.approx(0.0)

    gripper = view.grippers["lite_gripper"]
    assert gripper.generated_gripper_id == "L_lite_gripper"
    assert gripper.generated_group_id == "L_lite_gripper"
    assert gripper.generated_command_joint == "L_right_finger_joint"
    assert gripper.generated_derived_joints == ("L_left_finger_joint",)
    assert gripper.generated_touch_links == (
        "L_lite_gripper_left_finger",
        "L_lite_gripper_right_finger",
    )
    assert gripper.generated_tcp_frame == "L_link_tcp"
    assert gripper.presets["open"] == pytest.approx(0.0081)

    disabled = {
        (pair.generated_link1, pair.generated_link2)
        for pair in view.disabled_collision_pairs
    }
    assert ("L_lite_gripper_left_finger", "L_lite_gripper_right_finger") in disabled
    assert any(
        record.kind == "joint"
        and record.source_name == "right_finger_joint"
        and record.generated_name == "L_right_finger_joint"
        and record.source_asset_id == "lite6_asset"
        and record.mount_parent_frame == "cell"
        for record in view.traceability
    )


def test_two_instance_same_asset_views_are_globally_unique_and_deterministic():
    model_dir = MODEL_ROOT / "lite6_lite_gripper"
    authority = load_robot_model_authority(
        urdf_path=model_dir / "lite6_lite_gripper.urdf",
        srdf_path=model_dir / "lite6_lite_gripper.srdf",
        package_paths=(str(REPO_ROOT),),
        model_name="lite6_lite_gripper",
        grippers={
            "lite_gripper": {
                "group_id": "lite_gripper",
                "joint_name": "right_finger_joint",
            }
        },
    )

    left = robot_model_instance_view(
        authority,
        asset_id="lite6_asset",
        instance_id="left",
        prefix="L_",
        mount_parent_frame="cell",
    )
    left_repeat = robot_model_instance_view(
        authority,
        asset_id="lite6_asset",
        instance_id="left",
        prefix="L_",
        mount_parent_frame="cell",
    )
    right = robot_model_instance_view(
        authority,
        asset_id="lite6_asset",
        instance_id="right",
        prefix="R_",
        mount_parent_frame="cell",
    )

    left_names = set(left.links.generated_to_canonical) | set(
        left.joints.generated_to_canonical
    ) | set(left.groups.generated_to_canonical)
    right_names = set(right.links.generated_to_canonical) | set(
        right.joints.generated_to_canonical
    ) | set(right.groups.generated_to_canonical)

    assert left_names.isdisjoint(right_names)
    assert left.instance_fingerprint == left_repeat.instance_fingerprint
    assert left.instance_fingerprint != right.instance_fingerprint
    assert left.joints.canonical("L_right_finger_joint") == "right_finger_joint"
    assert right.joints.generated("right_finger_joint") == "R_right_finger_joint"
    assert right.grippers["lite_gripper"].generated_command_joint == (
        "R_right_finger_joint"
    )


def test_instance_view_rejects_empty_or_reserved_prefix(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="one_joint">
          <link name="base"/>
          <link name="tool"/>
          <joint name="joint1" type="revolute">
            <parent link="base"/>
            <child link="tool"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
        </robot>
        """,
    )
    authority = load_robot_model_authority(urdf_path=urdf)

    with pytest.raises(RobotModelAuthorityError, match="prefix is required"):
        robot_model_instance_view(authority, instance_id="one", prefix="")
    with pytest.raises(RobotModelAuthorityError, match="reserved"):
        robot_model_instance_view(
            authority,
            instance_id="one",
            prefix="__rma_mimic_bad_",
        )


def test_no_mimic_arm_has_stable_fingerprint(tmp_path):
    urdf, srdf = _write_model(
        tmp_path,
        """
        <robot name="one_joint">
          <link name="base"/>
          <link name="tool"/>
          <joint name="joint1" type="revolute">
            <parent link="base"/>
            <child link="tool"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
        </robot>
        """,
        """
        <robot name="one_joint">
          <group name="arm">
            <joint name="joint1"/>
          </group>
        </robot>
        """,
    )

    first = load_robot_model_authority(urdf_path=urdf, srdf_path=srdf)
    second = load_robot_model_authority(urdf_path=urdf, srdf_path=srdf)

    assert first.commandable_joint_names == ("joint1",)
    assert first.derived_joint_names == ()
    assert first.group_commandable_joints("arm") == ("joint1",)
    assert first.canonical_fingerprint == second.canonical_fingerprint


def test_unknown_mimic_master_fails_before_runtime(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="bad">
          <link name="base"/>
          <link name="left"/>
          <joint name="left_joint" type="prismatic">
            <parent link="base"/>
            <child link="left"/>
            <mimic joint="missing_master"/>
          </joint>
        </robot>
        """,
    )

    with pytest.raises(RobotModelAuthorityError, match="unknown master joint"):
        load_robot_model_authority(urdf_path=urdf)


def test_reserved_rma_joint_prefix_fails_before_runtime(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="bad">
          <link name="base"/>
          <link name="tool"/>
          <joint name="__rma_mimic_0000_joint1" type="revolute">
            <parent link="base"/>
            <child link="tool"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
        </robot>
        """,
    )

    with pytest.raises(RobotModelAuthorityError, match="reserved"):
        load_robot_model_authority(urdf_path=urdf)


def test_mimic_law_parses_non_default_multiplier_and_offset(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="offset_mimic">
          <link name="base"/>
          <link name="master_link"/>
          <link name="child_link"/>
          <joint name="master_joint" type="revolute">
            <parent link="base"/>
            <child link="master_link"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
          <joint name="child_joint" type="revolute">
            <parent link="base"/>
            <child link="child_link"/>
            <axis xyz="0 0 1"/>
            <mimic joint="master_joint" multiplier="-2.5" offset="0.125"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
        </robot>
        """,
    )

    authority = load_robot_model_authority(urdf_path=urdf)

    assert authority.joints["child_joint"].mimic == MimicLaw(
        master="master_joint",
        multiplier=-2.5,
        offset=0.125,
    )


def test_import_view_rewrites_srdf_passive_joint_names(tmp_path):
    urdf, srdf = _write_model(
        tmp_path,
        """
        <robot name="passive_mimic">
          <link name="base"/>
          <link name="right"/>
          <link name="left"/>
          <joint name="right_master" type="prismatic">
            <parent link="base"/>
            <child link="right"/>
            <axis xyz="1 0 0"/>
            <limit lower="0" upper="1" effort="10" velocity="1"/>
          </joint>
          <joint name="left_child" type="prismatic">
            <parent link="base"/>
            <child link="left"/>
            <axis xyz="1 0 0"/>
            <mimic joint="right_master"/>
            <limit lower="0" upper="1" effort="10" velocity="1"/>
          </joint>
        </robot>
        """,
        """
        <robot name="passive_mimic">
          <group name="gripper">
            <joint name="left_child"/>
            <joint name="right_master"/>
          </group>
          <passive_joint name="left_child"/>
        </robot>
        """,
    )
    authority = load_robot_model_authority(urdf_path=urdf, srdf_path=srdf)

    view = build_roboplan_import_view(authority)
    srdf_root = ET.fromstring(view.srdf_xml)
    passive = srdf_root.find("passive_joint")

    assert passive is not None
    assert passive.attrib["name"] == view.view_joint_name("left_child")
    assert passive.attrib["name"] != "left_child"


def test_duplicate_joint_fails_before_runtime(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="bad">
          <link name="base"/>
          <link name="one"/>
          <link name="two"/>
          <joint name="joint1" type="fixed">
            <parent link="base"/>
            <child link="one"/>
          </joint>
          <joint name="joint1" type="fixed">
            <parent link="base"/>
            <child link="two"/>
          </joint>
        </robot>
        """,
    )

    with pytest.raises(RobotModelAuthorityError, match="duplicate URDF joint"):
        load_robot_model_authority(urdf_path=urdf)


def test_unknown_joint_link_fails_before_runtime(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="bad">
          <link name="base"/>
          <joint name="joint1" type="fixed">
            <parent link="base"/>
            <child link="missing_child"/>
          </joint>
        </robot>
        """,
    )

    with pytest.raises(RobotModelAuthorityError, match="unknown child link"):
        load_robot_model_authority(urdf_path=urdf)


def test_cyclic_mimic_graph_fails_before_runtime(tmp_path):
    urdf, _ = _write_model(
        tmp_path,
        """
        <robot name="bad">
          <link name="base"/>
          <link name="a"/>
          <link name="b"/>
          <joint name="joint_a" type="prismatic">
            <parent link="base"/>
            <child link="a"/>
            <mimic joint="joint_b"/>
          </joint>
          <joint name="joint_b" type="prismatic">
            <parent link="base"/>
            <child link="b"/>
            <mimic joint="joint_a"/>
          </joint>
        </robot>
        """,
    )

    with pytest.raises(RobotModelAuthorityError, match="cyclic mimic graph"):
        load_robot_model_authority(urdf_path=urdf)
