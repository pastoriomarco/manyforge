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
        orchestrator=SimpleNamespace(config={"collision_monitor_period_ms": 10.0}),
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

    def create_ros2_control_kernel(*, deployment, collision_monitor_period_ns):
        calls.append(
            {
                "deployment": deployment,
                "collision_monitor_period_ns": collision_monitor_period_ns,
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
            "collision_monitor_period_ns": 10_000_000,
        }
    ]
