"""Tests for the ROS 2 control kernel factory glue."""

from __future__ import annotations

from pathlib import Path
from types import ModuleType, SimpleNamespace
import sys

from manyforge_ros.ros2_control_kernel_factory import create_ros2_control_kernel


def _write_one_joint_model(root: Path) -> tuple[Path, Path]:
    urdf = root / "robot.urdf"
    srdf = root / "robot.srdf"
    urdf.write_text(
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
        encoding="utf-8",
    )
    srdf.write_text(
        """
        <robot name="one_joint">
          <group name="arm">
            <joint name="joint1"/>
          </group>
        </robot>
        """,
        encoding="utf-8",
    )
    return urdf, srdf


def test_ros2_control_factory_uses_authority_runtime_model(monkeypatch, tmp_path):
    urdf, srdf = _write_one_joint_model(tmp_path)
    deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            group_id="arm",
            urdf_path=str(urdf),
            srdf_path=str(srdf),
            package_paths=[str(tmp_path)],
        ),
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.ros2_control_follow_joint_trajectory",
                config={
                    "required_joints": ["joint1"],
                    "arm_action": "/arm_controller/follow_joint_trajectory",
                },
            ),
            "planning": SimpleNamespace(
                adapter_id="planning.roboplan",
                config={},
            ),
        },
        grippers={},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )
    recorded: dict[str, object] = {}
    fake_native_bundle = object()

    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_ros2_control_kernel_from_paths(
        model_name,
        group_name,
        urdf_path,
        srdf_path,
        package_paths,
        ros_config,
        kernel_config,
    ):
        recorded["model_name"] = model_name
        recorded["group_name"] = group_name
        recorded["urdf_path"] = urdf_path
        recorded["srdf_path"] = srdf_path
        recorded["package_paths"] = package_paths
        recorded["ros_config"] = ros_config
        recorded["kernel_config"] = kernel_config
        return fake_native_bundle

    fake_roboplan.create_ros2_control_kernel_from_paths = (
        create_ros2_control_kernel_from_paths
    )
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")

    class FakeNativeKernelClient:
        def __init__(self, bundle, **kwargs):
            self.bundle = bundle
            self.kwargs = kwargs

    fake_client_module.NativeKernelClient = FakeNativeKernelClient
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    kernel, bundle = create_ros2_control_kernel(deployment=deployment)

    assert isinstance(kernel, FakeNativeKernelClient)
    assert kernel.bundle is fake_native_bundle
    assert bundle.native_bundle is fake_native_bundle
    assert recorded["model_name"] == "robot"
    assert recorded["group_name"] == "arm"
    assert recorded["urdf_path"] == str(urdf)
    assert recorded["srdf_path"] == str(srdf)
    assert recorded["package_paths"][0] == str(tmp_path)
    assert recorded["ros_config"]["action_name"] == (
        "/arm_controller/follow_joint_trajectory"
    )
    assert kernel.robot_model_authority_metadata["view_changed"] is False
    assert bundle.robot_model_authority_metadata["canonical_urdf_path"] == str(urdf)
    assert kernel.kwargs["robot_model_authority_metadata"] == (
        kernel.robot_model_authority_metadata
    )
    assert kernel.kwargs["robot_model_authority_temp_dir"] is None
