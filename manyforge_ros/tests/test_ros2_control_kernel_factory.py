"""Tests for the ROS 2 control kernel factory glue."""

from __future__ import annotations

from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace

from manyforge_ros.ros2_control_kernel_factory import _kernel_stop_profile
from manyforge_ros.ros2_control_kernel_factory import create_ros2_control_kernel
from manyforge_ros.ros2_control_kernel_factory import Ros2ControlRuntimeBundle
import pytest


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


def test_kernel_stop_profile_derives_same_total_deadline_as_ros_backend():
    execution_config = {
        "stop_completion_timeout_ms": 1500,
        "operator_stop_profile": {
            "profile_id": "operator-soft",
            "stop_mode": "soft_on_path",
            "deceleration_time_s": 1.0,
        },
    }

    resolved = _kernel_stop_profile(
        execution_config,
        "operator_stop_profile",
    )

    assert resolved["max_stop_time_s"] == pytest.approx(2.5)
    assert "max_stop_time_s" not in execution_config["operator_stop_profile"]


def test_kernel_stop_profile_preserves_explicit_total_deadline():
    execution_config = {
        "stop_completion_timeout_s": 1.5,
        "operator_stop_profile": {
            "profile_id": "operator-soft",
            "stop_mode": "soft_on_path",
            "deceleration_time_s": 1.0,
            "max_stop_time_s": 4.0,
        },
    }

    resolved = _kernel_stop_profile(
        execution_config,
        "operator_stop_profile",
    )

    assert resolved["max_stop_time_s"] == pytest.approx(4.0)


def _write_mimic_gripper_model(root: Path) -> tuple[Path, Path]:
    urdf = root / "mimic_gripper.urdf"
    srdf = root / "mimic_gripper.srdf"
    urdf.write_text(
        """
        <robot name="mimic_gripper">
          <link name="base"/>
          <link name="link1"/>
          <joint name="joint1" type="revolute">
            <parent link="base"/>
            <child link="link1"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
          <link name="gripper_base"/>
          <joint name="gripper_mount" type="fixed">
            <parent link="link1"/>
            <child link="gripper_base"/>
          </joint>
          <link name="left_finger"/>
          <joint name="left_finger_joint" type="prismatic">
            <parent link="gripper_base"/>
            <child link="left_finger"/>
            <axis xyz="0 1 0"/>
            <limit lower="0" upper="0.01" effort="10" velocity="1"/>
            <mimic joint="right_finger_joint" multiplier="1" offset="0"/>
          </joint>
          <link name="right_finger"/>
          <joint name="right_finger_joint" type="prismatic">
            <parent link="gripper_base"/>
            <child link="right_finger"/>
            <axis xyz="0 -1 0"/>
            <limit lower="0" upper="0.01" effort="10" velocity="1"/>
          </joint>
        </robot>
        """,
        encoding="utf-8",
    )
    srdf.write_text(
        """
        <robot name="mimic_gripper">
          <group name="arm">
            <joint name="joint1"/>
          </group>
          <group name="gripper">
            <joint name="left_finger_joint"/>
            <joint name="right_finger_joint"/>
          </group>
          <group_state name="open" group="gripper">
            <joint name="right_finger_joint" value="0.0081"/>
            <joint name="left_finger_joint" value="0.0081"/>
          </group_state>
          <group_state name="close" group="gripper">
            <joint name="right_finger_joint" value="0.0"/>
            <joint name="left_finger_joint" value="0.0"/>
          </group_state>
        </robot>
        """,
        encoding="utf-8",
    )
    return urdf, srdf


def test_runtime_bundle_resets_native_backend_when_ingress_shutdown_fails():
    calls: list[str] = []

    class FailingSubscription:
        def shutdown(self):
            calls.append("subscription")
            raise RuntimeError("subscriber failed")

    class NativeBundle:
        def reset(self):
            calls.append("native")

    bundle = Ros2ControlRuntimeBundle(
        native_bundle=NativeBundle(),
        group_id="arm",
        execution_admission_subscription=FailingSubscription(),
    )

    with pytest.raises(RuntimeError, match="subscriber failed"):
        bundle.shutdown()
    assert calls == ["subscription", "native"]
    assert bundle.execution_admission_subscription is None


def test_ros2_control_factory_uses_authority_runtime_model(monkeypatch, tmp_path):
    urdf, srdf = _write_one_joint_model(tmp_path)
    deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="logical_robot",
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
                    "executor_thread_count": 4,
                    "controller_binding": {
                        "schema": (
                            "manyforge.ros2_control."
                            "execution_binding.v0.experimental"
                        ),
                        "generation_id": "binding-generation",
                    },
                    "require_execution_admission_snapshot": True,
                    "execution_admission": {
                        "topic": "/manyforge_execution_admission",
                        "provider_instance_id": "provider-session-1",
                        "runtime_generation_id": "runtime-1",
                        "cell_supervisor_component_epoch": (
                            "supervisor-epoch-1"
                        ),
                        "maximum_lease_ms": 250,
                    },
                },
            ),
            "planning": SimpleNamespace(
                adapter_id="planning.roboplan",
                config={"rng_seed": 12345},
            ),
        },
        grippers={},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )
    recorded: dict[str, object] = {}

    class FakeNativeBundle:
        def __init__(self):
            self.configuration_completed = False

        def complete_ros2_control_configuration(self):
            self.configuration_completed = True

    fake_native_bundle = FakeNativeBundle()

    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_ros2_control_kernel_from_paths(
        model_name,
        group_name,
        urdf_path,
        srdf_path,
        package_paths,
        ros_config,
        kernel_config,
        logical_robot_id,
        defer_ros_executor_start,
    ):
        recorded["model_name"] = model_name
        recorded["group_name"] = group_name
        recorded["urdf_path"] = urdf_path
        recorded["srdf_path"] = srdf_path
        recorded["package_paths"] = package_paths
        recorded["ros_config"] = ros_config
        recorded["kernel_config"] = kernel_config
        recorded["logical_robot_id"] = logical_robot_id
        recorded["defer_ros_executor_start"] = defer_ros_executor_start
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

    fake_admission_module = ModuleType("manyforge_ros.execution_admission")

    class FakeIngressConfig:
        def __init__(self, **kwargs):
            self.__dict__.update(kwargs)

    class FakeAdmissionSubscription:
        def __init__(self, **kwargs):
            recorded["admission_subscription"] = kwargs

        def shutdown(self):
            recorded["admission_shutdown"] = True

    fake_admission_module.ExecutionAdmissionIngressConfig = FakeIngressConfig
    fake_admission_module.RosExecutionAdmissionSubscription = (
        FakeAdmissionSubscription
    )
    monkeypatch.setitem(
        sys.modules,
        "manyforge_ros.execution_admission",
        fake_admission_module,
    )

    kernel, bundle = create_ros2_control_kernel(deployment=deployment)

    assert isinstance(kernel, FakeNativeKernelClient)
    assert kernel.bundle is fake_native_bundle
    assert bundle.native_bundle is fake_native_bundle
    assert recorded["model_name"] == "robot"
    assert recorded["group_name"] == "arm"
    assert recorded["logical_robot_id"] == "logical_robot"
    assert recorded["defer_ros_executor_start"] is True
    assert fake_native_bundle.configuration_completed is True
    assert recorded["urdf_path"] == str(urdf)
    assert recorded["srdf_path"] == str(srdf)
    assert recorded["package_paths"] == [str(tmp_path)]
    assert recorded["ros_config"]["action_name"] == (
        "/arm_controller/follow_joint_trajectory"
    )
    assert recorded["ros_config"]["executor_thread_count"] == 4
    assert recorded["ros_config"]["controller_binding"] == {
        "schema": (
            "manyforge.ros2_control.execution_binding.v0.experimental"
        ),
        "generation_id": "binding-generation",
    }
    assert (
        recorded["ros_config"]["require_execution_admission_snapshot"]
        is True
    )
    admission = recorded["admission_subscription"]
    assert admission["native_bundle"] is fake_native_bundle
    assert admission["config"].topic == "/manyforge_execution_admission"
    assert admission["config"].provider_instance_id == "provider-session-1"
    assert admission["config"].robot_id == "logical_robot"
    assert admission["config"].binding_contract_generation_id == (
        "binding-generation"
    )
    assert admission["config"].runtime_generation_id == "runtime-1"
    assert admission["config"].cell_supervisor_component_epoch == (
        "supervisor-epoch-1"
    )
    assert admission["config"].maximum_lease_ns == 250_000_000
    assert recorded["kernel_config"]["rng_seed"] == 12345
    assert kernel.robot_model_authority_metadata["view_changed"] is False
    assert bundle.robot_model_authority_metadata["canonical_urdf_path"] == str(urdf)
    assert kernel.kwargs["robot_model_authority_metadata"] == (
        kernel.robot_model_authority_metadata
    )
    assert kernel.kwargs["robot_model_authority_temp_dir"] is None
    bundle.shutdown()
    assert recorded["admission_shutdown"] is True


def test_ros2_control_factory_forwards_validation_collision_timeout(
    monkeypatch, tmp_path
):
    urdf, srdf = _write_one_joint_model(tmp_path)
    deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="logical_robot",
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
            )
        },
        grippers={},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )
    recorded: dict[str, object] = {}
    fake_roboplan = ModuleType("manyforge_roboplan_py")

    class FakeNativeBundle:
        def complete_ros2_control_configuration(self):
            recorded["configuration_completed"] = True

    def create_ros2_control_kernel_from_paths(*args):
        recorded["kernel_config"] = args[6]
        recorded["defer_ros_executor_start"] = args[8]
        return FakeNativeBundle()

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

    create_ros2_control_kernel(
        deployment=deployment,
        validation_collision_query_timeout_ns=275_000_000,
        validation_collision_wall_timeout_ns=3_000_000_000,
        validation_collision_workers=3,
        runtime_collision_immediate_query_budget_ns=25_000_000,
        runtime_collision_lookahead_cycle_budget_ns=75_000_000,
        runtime_collision_lookahead_query_budget_ns=60_000_000,
        runtime_collision_lookahead_horizon_ns=1_500_000_000,
        runtime_collision_lookahead_max_samples=48,
        runtime_collision_lookahead_workers=4,
    )

    assert recorded["kernel_config"]["validation_collision_query_timeout_ns"] == (
        275_000_000
    )
    assert recorded["defer_ros_executor_start"] is True
    assert recorded["configuration_completed"] is True
    assert recorded["kernel_config"]["validation_collision_wall_timeout_ns"] == (
        3_000_000_000
    )
    assert recorded["kernel_config"]["validation_collision_workers"] == 3
    assert (
        recorded["kernel_config"]["runtime_collision_immediate_query_budget_ns"]
        == 25_000_000
    )
    assert (
        recorded["kernel_config"]["runtime_collision_lookahead_cycle_budget_ns"]
        == 75_000_000
    )
    assert (
        recorded["kernel_config"]["runtime_collision_lookahead_query_budget_ns"]
        == 60_000_000
    )
    assert (
        recorded["kernel_config"]["runtime_collision_lookahead_horizon_ns"]
        == 1_500_000_000
    )
    assert (
        recorded["kernel_config"]["runtime_collision_lookahead_max_samples"] == 48
    )
    assert recorded["kernel_config"]["runtime_collision_lookahead_workers"] == 4


def test_ros2_control_factory_keeps_ros_gripper_config_canonical_and_seeds_scene(
    monkeypatch, tmp_path
):
    urdf, srdf = _write_mimic_gripper_model(tmp_path)
    gripper = SimpleNamespace(
        command_mode="ros_gripper_command",
        group_id="gripper",
        joint_name="right_finger_joint",
        motion_time_s=0.8,
        srdf_presets={"open": 0.0081, "close": 0.0},
        completion_policy="driver_result",
        touch_links=[],
        mount_frame="",
        attach_frame="",
        tcp_frame="",
        auxiliary_frames=[],
    )
    deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="logical_robot",
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
                    "gripper": {
                        "action_name": "/gripper_controller/gripper_cmd",
                    },
                },
            ),
            "planning": SimpleNamespace(
                adapter_id="planning.roboplan",
                config={},
            ),
        },
        grippers={"primary": gripper},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )

    class FakeNativeBundle:
        def __init__(self):
            self.gripper_actions: dict[str, dict[str, object]] = {}
            self.seeded_positions: list[tuple[str, list[float]]] = []
            self.robot_model_authority_metadata: dict[str, object] = {}

        def get_group_commandable_joint_positions(self, group_id):
            assert group_id == "gripper"
            return [0.0]

        def set_group_joint_positions(self, group_id, positions):
            self.seeded_positions.append((group_id, list(positions)))

        def configure_ros_gripper_action(self, gripper_id, config):
            self.gripper_actions[gripper_id] = dict(config)

        def complete_ros2_control_configuration(self):
            self.configuration_completed = True

        def set_robot_model_authority_metadata(self, metadata):
            self.robot_model_authority_metadata = dict(metadata)

    fake_native_bundle = FakeNativeBundle()
    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_ros2_control_kernel_from_paths(*args):
        del args
        return fake_native_bundle

    fake_roboplan.create_ros2_control_kernel_from_paths = (
        create_ros2_control_kernel_from_paths
    )
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")

    class FakeNativeKernelClient:
        def __init__(self, bundle, **kwargs):
            self.bundle = bundle
            self.robot_model_authority_metadata = dict(
                kwargs["robot_model_authority_metadata"]
            )

    fake_client_module.NativeKernelClient = FakeNativeKernelClient
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    kernel, _bundle = create_ros2_control_kernel(deployment=deployment)

    view_joint = kernel.robot_model_authority_metadata["canonical_to_view_joint_names"][
        "right_finger_joint"
    ]
    assert view_joint != "right_finger_joint"
    assert fake_native_bundle.gripper_actions["primary"]["joint_name"] == (
        "right_finger_joint"
    )
    assert "__rma_mimic_" not in str(
        fake_native_bundle.gripper_actions["primary"]
    )
    assert fake_native_bundle.gripper_actions["primary"]["group_id"] == "gripper"
    assert fake_native_bundle.gripper_actions["primary"]["completion_policy"] == (
        "driver_result"
    )
    assert fake_native_bundle.seeded_positions == [("gripper", [0.0081])]
    assert fake_native_bundle.configuration_completed is True
    assert fake_native_bundle.robot_model_authority_metadata[
        "canonical_to_view_joint_names"
    ]["right_finger_joint"] == view_joint


def test_ros2_control_factory_configures_isaac_effort_gripper_transport(
    monkeypatch, tmp_path
):
    urdf, srdf = _write_mimic_gripper_model(tmp_path)
    gripper = SimpleNamespace(
        command_mode="ros_gripper_command",
        group_id="gripper",
        joint_name="right_finger_joint",
        motion_time_s=0.8,
        srdf_presets={"open": 0.0081, "closed": 0.0},
        actuation_kind="pneumatic",
        state_authority="sim_observed",
        completion_policy="dwell_elapsed",
        grasp_confirmation_policy="none_open_loop",
        transport={
            "kind": "isaac_joint_state_effort",
            "command_topic": "/isaac_joint_commands_gripper",
            "state_topic": "/isaac_joint_states",
            "joint_names": ["right_finger_joint"],
            "open_effort": 5.0,
            "close_effort": -5.0,
            "publish_nominal_position": True,
            "state_freshness_timeout_s": 1.5,
            "dwell_s": 0.8,
        },
        open_state_ref="open",
        closed_state_ref="closed",
        touch_links=[],
        mount_frame="",
        attach_frame="",
        tcp_frame="",
        auxiliary_frames=[],
    )
    deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="logical_robot",
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
        grippers={"primary": gripper},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )

    class FakeNativeBundle:
        def __init__(self):
            self.gripper_actions: dict[str, dict[str, object]] = {}
            self.seeded_positions: list[tuple[str, list[float]]] = []
            self.robot_model_authority_metadata: dict[str, object] = {}

        def get_group_commandable_joint_positions(self, group_id):
            assert group_id == "gripper"
            return [0.0]

        def set_group_joint_positions(self, group_id, positions):
            self.seeded_positions.append((group_id, list(positions)))

        def configure_ros_gripper_action(self, gripper_id, config):
            self.gripper_actions[gripper_id] = dict(config)

        def complete_ros2_control_configuration(self):
            self.configuration_completed = True

        def set_robot_model_authority_metadata(self, metadata):
            self.robot_model_authority_metadata = dict(metadata)

    fake_native_bundle = FakeNativeBundle()
    fake_roboplan = ModuleType("manyforge_roboplan_py")
    fake_roboplan.create_ros2_control_kernel_from_paths = (
        lambda *args: fake_native_bundle
    )
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")

    class FakeNativeKernelClient:
        def __init__(self, bundle, **kwargs):
            self.bundle = bundle
            self.robot_model_authority_metadata = dict(
                kwargs["robot_model_authority_metadata"]
            )

    fake_client_module.NativeKernelClient = FakeNativeKernelClient
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    create_ros2_control_kernel(deployment=deployment)

    config = fake_native_bundle.gripper_actions["primary"]
    assert config["transport_kind"] == "isaac_joint_state_effort"
    assert config["command_topic"] == "/isaac_joint_commands_gripper"
    assert config["state_topic"] == "/isaac_joint_states"
    assert config["command_joint_names"] == ["right_finger_joint"]
    assert config["open_position"] == 0.0081
    assert config["closed_position"] == 0.0
    assert config["open_effort"] == 5.0
    assert config["close_effort"] == -5.0
    assert config["publish_nominal_position"] is True
    assert config["state_freshness_timeout_s"] == 1.5
    assert fake_native_bundle.configuration_completed is True
