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


def _committed(*apply_ids: int) -> list[dict[str, Any]]:
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

    def observe(self, apply_id: int, apply_generation: int) -> dict[str, Any]:
        assert (apply_id, apply_generation) == (7, 1)
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
                "apply_id": 6,
                "apply_generation": 1,
                "committed_applies": _committed(6),
            }
        return {
            "apply_id": 7,
            "apply_generation": 1,
            "committed_applies": _committed(6, 7),
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
    follower.follow(7, 1)
    follower.follow(7, 1)  # the same Apply is followed once
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
    follower.follow(7, 1)
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
            "apply_id": 8,
            "apply_generation": 1,
            "committed_applies": _committed(6, 7, 8),
        }

    follower = CommittedApplyFollower(
        observe_apply=outcomes.observe,
        committed_activation=superseded,
        wake=outcomes.wake,
        poll_seconds=5,
    )
    follower.follow(7, 1)
    # Found on the first read: no poll period passes before the wake.
    assert outcomes.woken.wait(2)
    follower.close()
    assert outcomes.events == ["status:succeeded", "projection", "wake"]


def test_a_long_apply_and_a_late_publication_are_followed_to_their_end() -> None:
    # OD-214 (T36): no limit of the follower's own decides; the Apply's
    # terminal status and then its publication end the waits.
    outcomes = _Outcomes(["pending"] * 200 + ["succeeded"], projected_after=200)
    follower = _follower(outcomes)
    follower.follow(7, 1)
    assert outcomes.woken.wait(10)
    follower.close()
    assert outcomes.events.count("status:pending") == 200
    assert outcomes.events.count("projection") == 201
    assert outcomes.events[-1] == "wake"


def test_unreadable_apply_status_is_followed_until_it_is_final() -> None:
    # A status read that fails (Manager not answering for a moment) is not an
    # outcome: the follower reads again until Manager answers terminal.
    outcomes = _Outcomes(["succeeded"], projected_after=0)
    failures = [3]

    def observe(apply_id: int, apply_generation: int) -> dict[str, Any]:
        if failures[0] > 0:
            failures[0] -= 1
            outcomes.events.append("status:unavailable")
            raise HeadlessApiError("manager_unavailable", "Manager is not reachable")
        return outcomes.observe(apply_id, apply_generation)

    follower = CommittedApplyFollower(
        observe_apply=observe,
        committed_activation=outcomes.activation,
        wake=outcomes.wake,
        poll_seconds=0.001,
    )
    follower.follow(7, 1)
    assert outcomes.woken.wait(5)
    follower.close()
    assert outcomes.events == ["status:unavailable"] * 3 + [
        "status:succeeded", "projection", "wake",
    ]


@pytest.mark.parametrize("stage", ["status", "publication"])
def test_closing_the_service_ends_a_follow_that_has_no_end_yet(stage: str) -> None:
    # Application shutdown ends both waits; nothing is woken.
    read = Event()
    woken = Event()

    def observe(_apply_id: int, _apply_generation: int) -> dict[str, Any]:
        read.set()
        return {"ok": True, "state": "pending" if stage == "status" else "succeeded"}

    def activation() -> dict[str, Any]:
        read.set()
        return {"apply_id": 6, "apply_generation": 1, "committed_applies": _committed(6)}

    follower = CommittedApplyFollower(
        observe_apply=observe,
        committed_activation=activation,
        wake=woken.set,
        poll_seconds=0.001,
    )
    follower.follow(7, 1)
    assert read.wait(5)
    follower.close(timeout_seconds=5)
    assert not follower._followed  # noqa: SLF001 - the thread has ended
    assert not woken.is_set()


class _CommittingManager(FakeManager):
    def apply(self, request) -> dict[str, Any]:
        self.requests.append(request)
        return {
            "ok": True,
            "state": "pending",
            "apply_id": 9,
            "apply_generation": 1,
            "request_key": request.request_key,
        }

    def observe_apply(self, apply_id: int, apply_generation: int) -> dict[str, Any]:
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
        _committed_activation(9)
    )
    runner = _Runner()
    manager = _CommittingManager()
    service = _service(core, manager, program_runner=runner)
    assert service.apply_follower is not None
    service.apply_follower._poll = 0.001  # noqa: SLF001

    class _Request:
        request_key = "c0" + "00" * 7 + "00" * 7 + "01"

    service.manager_apply(_Request())  # type: ignore[arg-type]
    try:
        assert runner.woken.wait(5)
    finally:
        service.close()
    assert manager.apply_observations == [(9, 1)]
