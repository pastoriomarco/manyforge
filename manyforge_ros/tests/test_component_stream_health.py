"""Component stream health: sample sink (hermetic, no ROS graph)."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from manyforge_ros import component_stream_health as csh
from manyforge_ros.component_stream_health import (
    COMPONENT_STREAM_SAMPLES_TOPIC,
    ComponentStreamSink,
)


class _Status(SimpleNamespace):
    pass


def _key_value(*, key: str, value: str) -> tuple[str, str]:
    return (key, value)


class _Publisher:
    def __init__(self) -> None:
        self.messages: list[_Status] = []

    def publish(self, message: _Status) -> None:
        self.messages.append(message)


class _Node:
    def __init__(self) -> None:
        self.publisher = _Publisher()
        self.created: list[tuple[object, str, object]] = []

    def create_publisher(self, message_type, topic, qos):
        self.created.append((message_type, topic, qos))
        return self.publisher


class _Clock:
    def __init__(self) -> None:
        self.now = 1_000

    def __call__(self) -> int:
        return self.now


@pytest.fixture
def sink_env(monkeypatch):
    monkeypatch.setattr(csh, "_ros_transport", lambda: (_Status, _key_value, "qos64"))
    node, clock = _Node(), _Clock()
    sink = ComponentStreamSink(
        node, "isaac_sim", "sim_clock", heartbeat_period_ns=100, clock_ns=clock
    )
    return sink, node, clock


def _stamps(node: _Node) -> list[str]:
    return [dict(m.values)["stamp_ns"] for m in node.publisher.messages]


# --- sink ---------------------------------------------------------------------


def test_sink_publishes_on_the_samples_topic(sink_env) -> None:
    sink, node, _ = sink_env
    assert node.created == [(_Status, COMPONENT_STREAM_SAMPLES_TOPIC, "qos64")]
    sink.record(42)
    (message,) = node.publisher.messages
    assert message.hardware_id == "isaac_sim"
    assert message.name == "sim_clock"
    assert message.level == bytes([0])
    assert message.message == ""
    assert message.values == [("stamp_ns", "42")]


def test_sink_throttles_and_flushes_newest_advancing_stamp(sink_env) -> None:
    sink, node, clock = sink_env
    sink.record(10)
    clock.now += 50
    sink.record(11)
    sink.record(12)
    sink.record(12)  # equal stamp: not advancing
    assert _stamps(node) == ["10"]
    clock.now += 50
    sink.record(12)  # not advancing, but flushes the pending sample
    assert _stamps(node) == ["10", "12"]
    clock.now += 500
    sink.record(12)  # nothing pending: a frozen stamp is never re-sent
    assert _stamps(node) == ["10", "12"]
    sink.record(13)
    assert _stamps(node) == ["10", "12", "13"]


def test_sink_forwards_a_stamp_reset(sink_env) -> None:
    sink, node, clock = sink_env
    sink.record(5_000)
    clock.now += 200
    sink.record(5)  # simulator time reset: a different stamp advances
    assert _stamps(node) == ["5000", "5"]
    clock.now += 200
    sink.record(5)  # paused: equal stamp is not re-sent
    assert _stamps(node) == ["5000", "5"]


def test_sink_arrival_samples_send_zero_stamp(sink_env) -> None:
    sink, node, clock = sink_env
    sink.record()
    sink.record()
    clock.now += 100
    sink.record()
    assert _stamps(node) == ["0", "0"]


def test_sink_zero_stamp_samples_are_arrival_samples(sink_env) -> None:
    # Live r10-E: Isaac entity-state poses carry stamp 0; the pose streams
    # stayed stale because only the first zero stamp ever advanced.
    sink, node, clock = sink_env
    sink.record(0)
    clock.now += 100
    sink.record(0)
    clock.now += 100
    sink.record(0)
    assert _stamps(node) == ["0", "0", "0"]


def test_sink_rejects_invalid_stamps(sink_env) -> None:
    sink, _, _ = sink_env
    for stamp in (-1, 1.5, True, 2**63):
        with pytest.raises(ValueError):
            sink.record(stamp)


def test_sink_reports_fault_at_once_with_bounded_detail(sink_env) -> None:
    sink, node, _ = sink_env
    sink.record(10)
    sink.report_fault("isaac_simulation_stopped", "é" * 200)
    fault = node.publisher.messages[-1]
    assert fault.level == bytes([2])
    assert fault.message == "isaac_simulation_stopped"
    values = dict(fault.values)
    assert values["stamp_ns"] == "0"
    assert values["detail"] == "é" * 128
    assert len(values["detail"].encode("utf-8")) == 256
    sink.report_fault("isaac_simulation_stopped", "x" * 3 + "é" * 200)
    detail = dict(node.publisher.messages[-1].values)["detail"]
    assert detail == "xxx" + "é" * 126  # never splits a UTF-8 sequence


def test_sink_rejects_invalid_fault_codes(sink_env) -> None:
    sink, node, _ = sink_env
    for code in ("", "Upper", "a-b", "x" * 65, None):
        with pytest.raises(ValueError):
            sink.report_fault(code, "")
    assert node.publisher.messages == []


def test_sink_rejects_invalid_identity(monkeypatch) -> None:
    monkeypatch.setattr(csh, "_ros_transport", lambda: (_Status, _key_value, "qos64"))
    with pytest.raises(ValueError):
        ComponentStreamSink(_Node(), "", "sim_clock")
    with pytest.raises(ValueError):
        ComponentStreamSink(_Node(), "isaac_sim", "Sim Clock")
