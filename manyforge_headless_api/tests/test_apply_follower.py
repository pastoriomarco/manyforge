"""Headless follows each Apply it forwarded and wakes the runner on commit."""

from __future__ import annotations

from threading import Event
from typing import Any

import pytest

from manyforge_headless_api.apply_follower import CommittedApplyFollower
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeManager,
    _committed_activation,
    _service,
)


def _committed(*apply_ids: str) -> list[dict[str, Any]]:
    return [
        {
            "apply_id": apply_id,
            "apply_generation": 1,
            "record_digest": "sha256:" + str(index) * 64,
            "scenario_id": "scenario.1",
        }
        for index, apply_id in enumerate(apply_ids)
    ]


class _Outcomes:
    """Manager's status answers for one Apply, then its projection."""

    def __init__(self, states: list[str], projected_after: int) -> None:
        self.states = list(states)
        self.projected_after = projected_after
        self.events: list[str] = []
        self.woken = Event()

    def observe(self, apply_id: str, apply_generation: int) -> dict[str, Any]:
        assert (apply_id, apply_generation) == ("apply.7", 1)
        state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
        self.events.append(f"status:{state}")
        return {"ok": True, "state": state}

    def activation(self) -> dict[str, Any]:
        self.events.append("projection")
        if self.projected_after > 0:
            self.projected_after -= 1
            # Manager publishes its source just after the terminal record.
            if self.projected_after % 2:
                raise HeadlessApiError(
                    "program_applied_selection_not_active", "lagging"
                )
            return {
                "apply_id": "apply.6",
                "apply_generation": 1,
                "committed_applies": _committed("apply.6"),
            }
        return {
            "apply_id": "apply.7",
            "apply_generation": 1,
            "committed_applies": _committed("apply.6", "apply.7"),
        }

    def wake(self) -> None:
        self.events.append("wake")
        self.woken.set()


def _follower(outcomes: _Outcomes) -> CommittedApplyFollower:
    return CommittedApplyFollower(
        observe_apply=outcomes.observe,
        committed_activation=outcomes.activation,
        wake=outcomes.wake,
        poll_seconds=0.001,
    )


def test_a_committed_apply_wakes_the_runner_once_it_is_projected() -> None:
    outcomes = _Outcomes(["pending", "pending", "succeeded"], projected_after=3)
    follower = _follower(outcomes)
    follower.follow("apply.7", 1)
    follower.follow("apply.7", 1)  # the same Apply is followed once
    assert outcomes.woken.wait(5)
    follower.close()
    # The projection still naming only the previous Apply is waited out.
    assert outcomes.events == [
        "status:pending",
        "status:pending",
        "status:succeeded",
        "projection",
        "projection",
        "projection",
        "projection",
        "wake",
    ]


@pytest.mark.parametrize("state", ["failed", "unknown_effect"])
def test_an_apply_that_never_published_wakes_nothing(state: str) -> None:
    outcomes = _Outcomes(["pending", state], projected_after=0)
    follower = _follower(outcomes)
    follower.follow("apply.7", 1)
    assert not outcomes.woken.wait(0.2)
    follower.close()
    assert outcomes.events == ["status:pending", f"status:{state}"]


def test_an_apply_already_superseded_by_a_later_one_wakes_at_once() -> None:
    outcomes = _Outcomes(["succeeded"], projected_after=0)

    def superseded() -> dict[str, Any]:
        # The very first read already holds the followed Apply and the later
        # one that superseded it; it is never current again.
        outcomes.events.append("projection")
        return {
            "apply_id": "apply.8",
            "apply_generation": 1,
            "committed_applies": _committed("apply.6", "apply.7", "apply.8"),
        }

    follower = CommittedApplyFollower(
        observe_apply=outcomes.observe,
        committed_activation=superseded,
        wake=outcomes.wake,
        poll_seconds=5,
    )
    follower.follow("apply.7", 1)
    # Polling until the deadline would take 30 s.
    assert outcomes.woken.wait(2)
    follower.close()
    assert outcomes.events == ["status:succeeded", "projection", "wake"]


class _CommittingManager(FakeManager):
    def apply(self, request) -> dict[str, Any]:
        self.requests.append(request)
        return {
            "ok": True,
            "state": "pending",
            "apply_id": "apply.forwarded",
            "apply_generation": 1,
            "request_id": request.request_id,
            "request_owner_epoch": request.request_owner_epoch,
        }

    def observe_apply(self, apply_id: str, apply_generation: int) -> dict[str, Any]:
        return {**super().observe_apply(apply_id, apply_generation), "state": "succeeded"}


class _Runner:
    def __init__(self) -> None:
        self.woken = Event()

    def reconcile_persistent_values(self) -> dict[str, Any]:
        self.woken.set()
        return {"outcome": "reconciled"}

    def close(self) -> None:
        return None


def test_a_forwarded_apply_wakes_the_runner_without_any_client_polling() -> None:
    core = FakeCore()
    core.committed_activation = (  # type: ignore[attr-defined]
        _committed_activation("apply.forwarded")
    )
    runner = _Runner()
    manager = _CommittingManager()
    service = _service(core, manager, program_runner=runner)
    assert service.apply_follower is not None
    service.apply_follower._poll = 0.001  # noqa: SLF001

    class _Request:
        request_id = "request.1"
        request_owner_epoch = "client.epoch.1"

    service.manager_apply(_Request())  # type: ignore[arg-type]
    try:
        assert runner.woken.wait(5)
    finally:
        service.close()
    assert manager.apply_observations == [("apply.forwarded", 1)]
