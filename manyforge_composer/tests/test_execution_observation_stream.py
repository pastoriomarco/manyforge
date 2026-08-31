from __future__ import annotations

import asyncio
from typing import Any

import pytest

from manyforge_composer.backend import execution_observation_stream as stream


def _status(*, sequence: int = 7, position: float = 0.25) -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "observation",
        "ok": True,
        "observation": {
            "schema_version": "manyforge.execution_observation.v1",
            "owner_epoch": "execution-owner.1",
            "cell_id": "cell-1",
            "retained_bundle_id": "bundle-1",
            "graph_revision": "graph-1",
            "controllable_component_id": "robot-1",
            "command_target_id": "arm",
            "source_epoch": "joint-source.1",
            "source_sequence": sequence,
            "source_joint_names": ["joint1", "joint2"],
            "compiled_joint_names": ["robot__joint1", "robot__joint2"],
            "positions_rad": [position, -position],
        },
    }


def test_project_execution_observation_is_exact_and_compact() -> None:
    frame = stream.project_execution_observation(
        _status(), received_at_monotonic_us=123_456
    )
    assert frame == {
        "schema_version": "manyforge.execution_observation_frame.v1",
        "owner_epoch": "execution-owner.1",
        "cell_id": "cell-1",
        "retained_bundle_id": "bundle-1",
        "graph_revision": "graph-1",
        "controllable_component_id": "robot-1",
        "command_target_id": "arm",
        "source_epoch": "joint-source.1",
        "source_sequence": 7,
        "received_at_monotonic_us": 123_456,
        "source_joint_names": ["joint1", "joint2"],
        "compiled_joint_names": ["robot__joint1", "robot__joint2"],
        "positions_rad": [0.25, -0.25],
    }

    crossed = _status()
    crossed["observation"]["compiled_joint_names"] = ["robot__joint1"]
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="joint orders differ",
    ):
        stream.project_execution_observation(
            crossed, received_at_monotonic_us=123_457
        )

    inflated = _status()
    inflated["observation"]["diagnostics"] = {"unexpected": "cold data"}
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="schema is unsupported",
    ):
        stream.project_execution_observation(
            inflated, received_at_monotonic_us=123_458
        )


def test_source_is_demand_bound_change_only_and_capacity_one(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Client:
        def __init__(self) -> None:
            self.calls = 0

        def execution_observation(self) -> dict[str, Any]:
            self.calls += 1
            if self.calls <= 2:
                return _status(sequence=self.calls, position=0.1)
            return _status(sequence=self.calls, position=float(self.calls))

    async def exercise() -> None:
        monkeypatch.setattr(stream, "_OBSERVATION_PERIOD_S", 0.0)
        client = Client()
        source = stream.ExecutionObservationSource(client)  # type: ignore[arg-type]
        assert client.calls == 0
        subscription = source.subscribe()
        for _ in range(200):
            if source.status()["fanout"]["replaced_frames_total"] > 0:
                break
            # The source performs the synchronous owner read through
            # ``to_thread``. Give that worker a bounded scheduling opportunity
            # instead of exhausting this loop before it can publish.
            await asyncio.sleep(0.001)
        status = source.status()
        assert status["source"]["poll_count"] >= 3
        assert status["source"]["published_count"] < status["source"]["poll_count"]
        assert status["source"]["last_poll_duration_us"] >= 0
        assert status["source"]["max_poll_duration_us"] >= (
            status["source"]["last_poll_duration_us"]
        )
        assert status["fanout"]["replaced_frames_total"] > 0
        assert subscription.queue.qsize() == 1
        latest = subscription.queue.get_nowait()
        # The poll counter is incremented before the owner read and may include
        # one in-flight call. In this fixture the second completed poll is the
        # sole unchanged frame, so the latest completed source sequence is one
        # greater than the number of published frames.
        assert latest["source_sequence"] == (
            status["source"]["published_count"] + 1
        )

        second = source.subscribe()
        assert second.queue.get_nowait()["source_sequence"] == latest["source_sequence"]
        source.unsubscribe(second)
        source.unsubscribe(subscription)
        await asyncio.sleep(0)
        status = source.status()
        assert status["fanout"]["client_count"] == 0
        assert status["source"]["active"] is False

    asyncio.run(exercise())


def test_source_rejects_changed_state_at_the_same_source_sequence(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Client:
        def __init__(self) -> None:
            self.calls = 0

        def execution_observation(self) -> dict[str, Any]:
            self.calls += 1
            return _status(sequence=7, position=float(self.calls))

    async def exercise() -> None:
        monkeypatch.setattr(stream, "_OBSERVATION_PERIOD_S", 0.0)
        monkeypatch.setattr(stream, "_OBSERVATION_ERROR_PERIOD_S", 0.0)
        source = stream.ExecutionObservationSource(Client())  # type: ignore[arg-type]
        subscription = source.subscribe()
        first = await subscription.queue.get()
        assert first["positions_rad"] == [1.0, -1.0]
        for _ in range(200):
            if source.status()["source"]["error_count"] > 0:
                break
            # The source uses ``to_thread`` around the synchronous owner
            # client. Give that worker a bounded scheduling opportunity rather
            # than spinning the event loop faster than the thread can run.
            await asyncio.sleep(0.001)
        status = source.status()["source"]
        assert status["published_count"] == 1
        assert status["error_count"] > 0
        assert status["last_error"]["message"] == (
            "execution observation changed without advancing its source sequence"
        )
        source.unsubscribe(subscription)

    asyncio.run(exercise())


def test_source_accepts_a_fresh_restart_epoch_with_a_lower_sequence(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Client:
        def __init__(self) -> None:
            self.calls = 0

        def execution_observation(self) -> dict[str, Any]:
            self.calls += 1
            response = _status(
                sequence=50 if self.calls == 1 else 1,
                position=0.1 if self.calls == 1 else 0.2,
            )
            if self.calls > 1:
                response["observation"]["owner_epoch"] = "execution-owner.2"
                response["observation"]["source_epoch"] = "joint-source.2"
            return response

    async def exercise() -> None:
        monkeypatch.setattr(stream, "_OBSERVATION_PERIOD_S", 0.0)
        source = stream.ExecutionObservationSource(Client())  # type: ignore[arg-type]
        subscription = source.subscribe()
        for _ in range(200):
            if source.status()["source"]["published_count"] >= 2:
                break
            await asyncio.sleep(0.001)
        status = source.status()["source"]
        assert status["published_count"] == 2
        assert status["error_count"] == 0
        latest = subscription.queue.get_nowait()
        assert latest["owner_epoch"] == "execution-owner.2"
        assert latest["source_epoch"] == "joint-source.2"
        assert latest["source_sequence"] == 1
        assert latest["positions_rad"] == [0.2, -0.2]
        source.unsubscribe(subscription)

    asyncio.run(exercise())
