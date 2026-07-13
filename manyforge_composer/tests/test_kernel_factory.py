"""Tests for Composer kernel factory adapter dispatch."""

from __future__ import annotations

from pathlib import Path
from types import ModuleType, SimpleNamespace
import sys

import pytest

from manyforge_composer import __main__ as composer_main
from manyforge_composer.backend.runtime_requirements import (
    RuntimeFactoryRequirementsError,
)


def _deployment_with_execution_adapter(adapter_id: str):
    return SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id=adapter_id,
                config={},
            )
        },
        orchestrator=SimpleNamespace(config={"collision_monitor_period_ms": 50.0}),
    )


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


def _write_invalid_mimic_model(root: Path) -> tuple[Path, Path]:
    urdf = root / "invalid.urdf"
    srdf = root / "invalid.srdf"
    urdf.write_text(
        """
        <robot name="invalid">
          <link name="base"/>
          <link name="left"/>
          <joint name="left_joint" type="prismatic">
            <parent link="base"/>
            <child link="left"/>
            <mimic joint="missing_master"/>
            <limit lower="0" upper="1" effort="10" velocity="1"/>
          </joint>
        </robot>
        """,
        encoding="utf-8",
    )
    srdf.write_text(
        """
        <robot name="invalid">
          <group name="arm">
            <joint name="left_joint"/>
          </group>
        </robot>
        """,
        encoding="utf-8",
    )
    return urdf, srdf


def test_ros2_control_adapter_fails_closed_when_runtime_factory_missing():
    deployment = _deployment_with_execution_adapter(
        "execution.ros2_control_follow_joint_trajectory"
    )

    sys.modules.pop("manyforge_ros.ros2_control_kernel_factory", None)
    with pytest.raises(RuntimeError, match="ROS 2 controller execution"):
        composer_main._make_kernel(deployment)


def test_native_adapter_fails_closed_when_runtime_factory_missing(monkeypatch):
    deployment = _deployment_with_execution_adapter("execution.roboplan_native")

    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", None)
    with pytest.raises(RuntimeFactoryRequirementsError) as exc_info:
        composer_main._make_kernel(deployment)

    detail = exc_info.value.to_detail()
    assert detail["reason_code"] == "native_factory_runtime_failed"
    assert detail["missing"] == ["native_roboplan_runtime"]
    assert "MockKernelClient" not in str(exc_info.value)


def test_ros2_control_adapter_dispatches_to_runtime_factory(monkeypatch):
    deployment = _deployment_with_execution_adapter("execution.ros2_control_fjt")
    fake_kernel = object()
    fake_bundle = object()
    calls: list[dict] = []

    fake_factory = ModuleType("manyforge_ros.ros2_control_kernel_factory")

    def create_ros2_control_kernel(
        *,
        deployment,
        collision_monitor_period_ns,
        runtime_collision_query_timeout_ns,
        validation_collision_query_timeout_ns,
        validation_collision_wall_timeout_ns,
        runtime_dynamic_body_max_age_cap_ns,
    ):
        calls.append(
            {
                "deployment": deployment,
                "collision_monitor_period_ns": collision_monitor_period_ns,
                "runtime_collision_query_timeout_ns": (
                    runtime_collision_query_timeout_ns
                ),
                "validation_collision_query_timeout_ns": (
                    validation_collision_query_timeout_ns
                ),
                "validation_collision_wall_timeout_ns": (
                    validation_collision_wall_timeout_ns
                ),
                "runtime_dynamic_body_max_age_cap_ns": (
                    runtime_dynamic_body_max_age_cap_ns
                ),
            }
        )
        return fake_kernel, fake_bundle

    fake_factory.create_ros2_control_kernel = create_ros2_control_kernel
    monkeypatch.setitem(
        sys.modules,
        "manyforge_ros.ros2_control_kernel_factory",
        fake_factory,
    )

    kernel, bundle = composer_main._make_kernel(deployment)

    assert kernel is fake_kernel
    assert bundle is fake_bundle
    assert calls == [
        {
            "deployment": deployment,
            "collision_monitor_period_ns": 50_000_000,
            "runtime_collision_query_timeout_ns": None,
            "validation_collision_query_timeout_ns": None,
            "validation_collision_wall_timeout_ns": None,
            "runtime_dynamic_body_max_age_cap_ns": None,
        }
    ]


def test_ros2_control_adapter_passes_dynamic_body_max_age_cap(monkeypatch):
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.ros2_control_fjt",
                config={},
            )
        },
        orchestrator=SimpleNamespace(
            config={
                "collision_monitor_period_ms": 50.0,
                "runtime_dynamic_body_max_age_cap_ms": 250.0,
                "validation_collision_wall_timeout_ms": 1250.0,
            }
        ),
    )
    calls: list[dict] = []
    fake_factory = ModuleType("manyforge_ros.ros2_control_kernel_factory")

    def create_ros2_control_kernel(**kwargs):
        calls.append(kwargs)
        return object(), object()

    fake_factory.create_ros2_control_kernel = create_ros2_control_kernel
    monkeypatch.setitem(
        sys.modules,
        "manyforge_ros.ros2_control_kernel_factory",
        fake_factory,
    )

    composer_main._make_kernel(deployment)

    assert calls[0]["runtime_dynamic_body_max_age_cap_ns"] == 250_000_000
    assert calls[0]["validation_collision_wall_timeout_ns"] == 1_250_000_000


def test_native_kernel_factory_passes_validation_collision_timeouts(monkeypatch):
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.roboplan_native",
                config={},
            )
        },
        orchestrator=SimpleNamespace(
            config={
                "validation_collision_query_timeout_ms": 275.0,
                "validation_collision_wall_timeout_ms": 3250.0,
            }
        ),
    )
    recorded: dict[str, object] = {}
    fake_bundle = object()

    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_native_kernel(model_name, group_name, config, logical_robot_id):
        recorded["model_name"] = model_name
        recorded["group_name"] = group_name
        recorded["config"] = config
        recorded["logical_robot_id"] = logical_robot_id
        return fake_bundle

    fake_roboplan.create_native_kernel = create_native_kernel
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")

    class FakeNativeKernelClient:
        def __init__(self, bundle, **kwargs):
            self.bundle = bundle
            self.kwargs = kwargs

    fake_client_module.NativeKernelClient = FakeNativeKernelClient
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    kernel, bundle = composer_main._make_kernel(deployment)

    assert isinstance(kernel, FakeNativeKernelClient)
    assert bundle is fake_bundle
    assert recorded["logical_robot_id"] == "ur5_gripper"
    assert (
        recorded["config"]["validation_collision_query_timeout_ns"]
        == 275_000_000
    )
    assert (
        recorded["config"]["validation_collision_wall_timeout_ns"]
        == 3_250_000_000
    )


def test_runtime_dynamic_body_max_age_cap_zero_is_passed_through():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(
            config={"runtime_dynamic_body_max_age_cap_ms": 0}
        ),
    )
    assert (
        composer_main._runtime_dynamic_body_max_age_cap_ns_from_deployment(deployment)
        == 0
    )


def test_validation_collision_query_timeout_is_parsed_from_deployment():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(
            config={"validation_collision_query_timeout_ms": 275.5}
        ),
    )
    assert (
        composer_main._validation_collision_query_timeout_ns_from_deployment(
            deployment
        )
        == 275_500_000
    )


def test_validation_collision_wall_timeout_is_parsed_from_deployment():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(
            config={"validation_collision_wall_timeout_ms": 3500.25}
        ),
    )
    assert (
        composer_main._validation_collision_wall_timeout_ns_from_deployment(
            deployment
        )
        == 3_500_250_000
    )


def test_validation_collision_wall_timeout_rejects_nonpositive():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(
            config={"validation_collision_wall_timeout_ms": 0}
        ),
    )
    with pytest.raises(ValueError, match="validation_collision_wall_timeout_ms"):
        composer_main._validation_collision_wall_timeout_ns_from_deployment(
            deployment
        )


def test_runtime_dynamic_body_max_age_cap_rejects_negative():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(
            config={"runtime_dynamic_body_max_age_cap_ms": -1.0}
        ),
    )
    with pytest.raises(ValueError, match="must be >= 0"):
        composer_main._runtime_dynamic_body_max_age_cap_ns_from_deployment(deployment)


def test_runtime_dynamic_body_max_age_cap_absent_returns_none():
    deployment = SimpleNamespace(
        adapters={},
        orchestrator=SimpleNamespace(config={}),
    )
    assert (
        composer_main._runtime_dynamic_body_max_age_cap_ns_from_deployment(deployment)
        is None
    )


def test_kernel_config_forwards_single_shot_ik_timeout():
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.native",
                config={"single_shot_ik_timeout_s": 0.08},
            )
        },
        orchestrator=SimpleNamespace(config={}),
    )
    config = composer_main._kernel_config_from_deployment(
        deployment,
        collision_monitor_period_ns=None,
        runtime_collision_query_timeout_ns=None,
        validation_collision_query_timeout_ns=None,
        validation_collision_wall_timeout_ns=None,
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config["single_shot_ik_timeout_s"] == 0.08


def test_kernel_config_forwards_planner_budget_from_planning_adapter():
    deployment = SimpleNamespace(
        adapters={
            "planning": SimpleNamespace(
                adapter_id="planning.roboplan",
                config={"planner_max_planning_time_s": 12.0},
            ),
            "execution": SimpleNamespace(
                adapter_id="execution.native",
                config={"planner_max_planning_time_s": 3.0},
            ),
        },
        orchestrator=SimpleNamespace(config={}),
    )
    config = composer_main._kernel_config_from_deployment(
        deployment,
        collision_monitor_period_ns=None,
        runtime_collision_query_timeout_ns=None,
        validation_collision_query_timeout_ns=None,
        validation_collision_wall_timeout_ns=None,
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config["planner_max_planning_time_s"] == 12.0


def test_kernel_config_forwards_planner_rng_seed_from_planning_adapter():
    deployment = SimpleNamespace(
        adapters={
            "planning": SimpleNamespace(
                adapter_id="planning.roboplan",
                config={"rng_seed": 12345},
            ),
            "execution": SimpleNamespace(adapter_id="execution.native", config={}),
        },
        orchestrator=SimpleNamespace(config={}),
    )
    config = composer_main._kernel_config_from_deployment(
        deployment,
        collision_monitor_period_ns=None,
        runtime_collision_query_timeout_ns=None,
        validation_collision_query_timeout_ns=None,
        validation_collision_wall_timeout_ns=None,
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config["rng_seed"] == 12345


def test_kernel_config_forwards_baseline_collision_policy_snapshot():
    snapshot = {
        "policy_id": "deployment_baseline",
        "revision_id": 1,
        "content_hash": "abc123",
        "require_full_diagnostics": True,
    }
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(adapter_id="execution.native", config={})
        },
        orchestrator=SimpleNamespace(config={}),
        baseline_collision_policy_snapshot=lambda: snapshot,
    )
    config = composer_main._kernel_config_from_deployment(
        deployment,
        collision_monitor_period_ns=None,
        runtime_collision_query_timeout_ns=None,
        validation_collision_query_timeout_ns=None,
        validation_collision_wall_timeout_ns=None,
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config["baseline_collision_policy_snapshot"] == snapshot


def test_kernel_config_omits_single_shot_ik_timeout_when_absent():
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(adapter_id="execution.native", config={})
        },
        orchestrator=SimpleNamespace(config={}),
    )
    # Pass one non-None override so the builder returns a dict (it collapses to
    # None only when nothing at all is configured).
    config = composer_main._kernel_config_from_deployment(
        deployment,
        collision_monitor_period_ns=50_000_000,
        runtime_collision_query_timeout_ns=None,
        validation_collision_query_timeout_ns=None,
        validation_collision_wall_timeout_ns=None,
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config is not None
    assert "single_shot_ik_timeout_s" not in config


def test_native_kernel_factory_attaches_robot_model_authority_metadata(
    monkeypatch,
    tmp_path,
):
    urdf, srdf = _write_one_joint_model(tmp_path)
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.roboplan_native",
                config={},
            )
        },
        orchestrator=SimpleNamespace(config={}),
        robot_config=SimpleNamespace(
            robot_id="logical_robot",
            group_id="arm",
            urdf_path=str(urdf),
            srdf_path=str(srdf),
            package_paths=[str(tmp_path)],
        ),
        grippers={},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )
    recorded: dict[str, object] = {}
    fake_bundle = object()

    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_native_kernel_from_paths(
        model_name,
        group_name,
        urdf_path,
        srdf_path,
        package_paths,
        config,
        logical_robot_id,
    ):
        recorded["model_name"] = model_name
        recorded["group_name"] = group_name
        recorded["urdf_path"] = urdf_path
        recorded["srdf_path"] = srdf_path
        recorded["package_paths"] = package_paths
        recorded["config"] = config
        recorded["logical_robot_id"] = logical_robot_id
        return fake_bundle

    fake_roboplan.create_native_kernel_from_paths = create_native_kernel_from_paths
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")

    class FakeNativeKernelClient:
        def __init__(self, bundle, **kwargs):
            self.bundle = bundle
            self.kwargs = kwargs

    fake_client_module.NativeKernelClient = FakeNativeKernelClient
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    kernel, bundle = composer_main._make_kernel(deployment)

    assert isinstance(kernel, FakeNativeKernelClient)
    assert bundle is fake_bundle
    assert recorded["model_name"] == "robot"
    assert recorded["group_name"] == "arm"
    assert recorded["logical_robot_id"] == "logical_robot"
    assert recorded["urdf_path"] == str(urdf)
    assert recorded["srdf_path"] == str(srdf)
    assert recorded["package_paths"] == [str(tmp_path)]
    assert recorded["config"] is None
    assert kernel.robot_model_authority_metadata["view_changed"] is False
    assert kernel.robot_model_authority_metadata["canonical_urdf_path"] == str(urdf)
    assert kernel.kwargs["robot_model_authority_metadata"] == (
        kernel.robot_model_authority_metadata
    )
    assert kernel.kwargs["robot_model_authority_temp_dir"] is None


def test_native_kernel_factory_fails_closed_on_invalid_authority_model(
    monkeypatch,
    tmp_path,
):
    urdf, srdf = _write_invalid_mimic_model(tmp_path)
    deployment = SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id="execution.roboplan_native",
                config={},
            )
        },
        orchestrator=SimpleNamespace(config={}),
        robot_config=SimpleNamespace(
            group_id="arm",
            urdf_path=str(urdf),
            srdf_path=str(srdf),
            package_paths=[str(tmp_path)],
        ),
        grippers={},
        source_path=str(tmp_path / "deployment.yaml"),
        baseline_collision_policy_snapshot=lambda: None,
    )
    fake_roboplan = ModuleType("manyforge_roboplan_py")

    def create_native_kernel_from_paths(*_args, **_kwargs):
        raise AssertionError("invalid authority model reached RoboPlan factory")

    fake_roboplan.create_native_kernel_from_paths = create_native_kernel_from_paths
    monkeypatch.setitem(sys.modules, "manyforge_roboplan_py", fake_roboplan)

    fake_client_module = ModuleType("native_kernel_client")
    fake_client_module.NativeKernelClient = object
    monkeypatch.setitem(sys.modules, "native_kernel_client", fake_client_module)

    with pytest.raises(RuntimeFactoryRequirementsError) as exc_info:
        composer_main._make_kernel(deployment)

    detail = exc_info.value.to_detail()
    assert detail["reason_code"] == "native_factory_runtime_failed"
    assert "unknown master joint" in detail["original_error"]
