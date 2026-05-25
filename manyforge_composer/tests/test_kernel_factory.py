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
