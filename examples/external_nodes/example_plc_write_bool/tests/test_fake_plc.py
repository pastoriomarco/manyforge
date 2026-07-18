"""Provider-neutral lifecycle tests for the copyable external-node example."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.runtime_resources import (
    RuntimeResourceFactoryRegistry,
    build_runtime_resource_registry,
)


SOURCE_PATH = Path(__file__).parents[1] / "node.py"
SPEC = spec_from_file_location("example_plc_write_bool_test", SOURCE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
ExamplePlcWriteBool = MODULE.ExamplePlcWriteBool


class FakeOperation:
    def __init__(self, *, done: bool = False, result=None):
        self.completed = done
        self.value = result
        self.cancelled = False

    def done(self) -> bool:
        return self.completed

    def result(self):
        return self.value

    def cancel(self) -> None:
        self.cancelled = True


class FakePlcClient:
    def __init__(self, operation: FakeOperation):
        self.operation = operation
        self.calls = []

    def write_bool_async(self, **kwargs):
        self.calls.append(kwargs)
        return self.operation


@pytest.fixture(autouse=True)
def _clear_blackboard():
    py_trees.blackboard.Blackboard.clear()
    yield
    py_trees.blackboard.Blackboard.clear()


def _behavior(operation: FakeOperation, *, timeout_s: float = 1.0):
    client = FakePlcClient(operation)
    registry = build_runtime_resource_registry(
        {"plc_clients": {"default": {"provider": "fake"}}},
        RuntimeResourceFactoryRegistry({"fake": lambda config: client}),
    )
    blackboard = py_trees.blackboard.Client(name="test-resources")
    blackboard.register_key(
        key="runtime_resources", access=py_trees.common.Access.WRITE
    )
    blackboard.runtime_resources = registry
    behavior = ExamplePlcWriteBool(
        name="write",
        plc_client_id="default",
        tag="fixture.ready",
        value=True,
        timeout_s=timeout_s,
    )
    behavior.setup()
    return behavior, client, registry


def test_success() -> None:
    operation = FakeOperation(done=False, result=SimpleNamespace(ok=True))
    behavior, client, registry = _behavior(operation)
    behavior.tick_once()
    operation.completed = True
    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.SUCCESS
    assert client.calls == [{"tag": "fixture.ready", "value": True}]
    registry.close()


def test_provider_failure() -> None:
    operation = FakeOperation(
        done=False,
        result=SimpleNamespace(ok=False, error="write_rejected"),
    )
    behavior, _, registry = _behavior(operation)
    behavior.tick_once()
    operation.completed = True
    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.FAILURE
    assert behavior.feedback_message == "write_rejected"
    registry.close()


def test_timeout_cancels(monkeypatch) -> None:
    clock = iter((1_000_000_000, 1_020_000_000))
    monkeypatch.setattr(MODULE.time, "monotonic_ns", lambda: next(clock))
    operation = FakeOperation()
    behavior, _, registry = _behavior(operation, timeout_s=0.01)
    behavior.tick_once()
    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.FAILURE
    assert behavior.feedback_message == "plc_write_timeout"
    assert operation.cancelled
    registry.close()


def test_stop_cancels_in_flight_operation() -> None:
    operation = FakeOperation()
    behavior, _, registry = _behavior(operation)
    behavior.tick_once()
    behavior.stop(py_trees.common.Status.INVALID)
    assert operation.cancelled
    registry.close()
