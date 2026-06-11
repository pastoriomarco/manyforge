"""Tests for Composer kernel factory adapter dispatch."""

from __future__ import annotations

from types import ModuleType, SimpleNamespace
import sys

import pytest

from manyforge_composer import __main__ as composer_main


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


def test_ros2_control_adapter_fails_closed_when_runtime_factory_missing():
    deployment = _deployment_with_execution_adapter(
        "execution.ros2_control_follow_joint_trajectory"
    )

    sys.modules.pop("manyforge_ros.ros2_control_kernel_factory", None)
    with pytest.raises(RuntimeError, match="ROS 2 controller execution"):
        composer_main._make_kernel(deployment)


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
        runtime_dynamic_body_max_age_cap_ns,
    ):
        calls.append(
            {
                "deployment": deployment,
                "collision_monitor_period_ns": collision_monitor_period_ns,
                "runtime_collision_query_timeout_ns": (
                    runtime_collision_query_timeout_ns
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
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config["planner_max_planning_time_s"] == 12.0


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
        runtime_dynamic_body_max_age_cap_ns=None,
    )
    assert config is not None
    assert "single_shot_ik_timeout_s" not in config
