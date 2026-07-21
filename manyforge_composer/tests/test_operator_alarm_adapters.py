"""Explicit authoritative adapter coverage for every initial alarm definition."""
from __future__ import annotations

import itertools
from typing import Any

import pytest

from manyforge_composer.backend import operator_alarm_adapters as adapter_module
from manyforge_composer.backend.event_recorder import EventRecorder
from manyforge_composer.backend.operator_alarm_adapters import OperatorAlarmAdapters
from manyforge_composer.backend.operator_alarm_catalog import load_operator_alarm_catalog
from manyforge_composer.backend.operator_alarms import OperatorAlarmManager


class _Clock:
    def __init__(self) -> None:
        self.now_ns = 1_000_000_000

    def monotonic(self) -> int:
        return self.now_ns

    def wall(self) -> str:
        return f"2026-07-21T00:00:{self.now_ns / 1e9:09.6f}Z"

    def advance(self, seconds: float) -> None:
        self.now_ns += int(seconds * 1_000_000_000)


def _system(
    *,
    providers: tuple[str, ...] = (),
    activation_prefix: str = "adapter-activation",
) -> tuple[
    _Clock,
    OperatorAlarmManager,
    OperatorAlarmAdapters,
    list[Any],
]:
    clock = _Clock()
    activations = itertools.count(1)
    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
        activation_factory=lambda: f"{activation_prefix}-{next(activations)}",
    )
    lifecycle: list[Any] = []
    adapters = OperatorAlarmAdapters(
        manager,
        lifecycle_sink=lifecycle.extend,
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
        provider_freshness_s=6.0,
    )
    adapters.begin_runtime_epoch(
        epoch="runtime-1",
        runtime_lane="isaac-sim",
        robot_resource="robot0",
        pose_scopes=(
            {
                "pose_source_id": "isaac-scene",
                "object_id": "box",
                "apply_policy_id": "continuous",
            },
            {
                "pose_source_id": "isaac-scene",
                "object_id": "fixture",
                "apply_policy_id": "continuous",
            },
        ),
        required_providers=providers,
    )
    return clock, manager, adapters, lifecycle


def _by_definition(manager: OperatorAlarmManager, definition_id: str) -> list[dict[str, Any]]:
    return [
        alarm
        for alarm in manager.snapshot()["alarms"]
        if alarm["definitionId"] == definition_id
    ]


def _collision_offer(*, revision: int = 1) -> dict[str, Any]:
    return {
        "offerId": "offer-collision",
        "offerRevision": revision,
        "recoveryClass": "runtime_collision",
        "status": "ready",
        "reasonCode": "runtime_collision",
        "evidenceRefs": ["kernel-event:7"],
        "latchId": "latch-7",
        "executionId": 41,
        "programGeneration": 3,
        "deploymentGeneration": 4,
    }


def _collision_details() -> dict[str, Any]:
    return {
        "collision_result_first_failure_pair": (
            "robot/robot0/link/wrist_3 <-> object/wall"
        ),
        "collision_result_waypoint_index": "0",
    }


def test_runtime_reconciliation_requires_fresh_motion_scene_and_runtime_snapshots() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    assert manager.snapshot()["reconciled"] is False

    adapters.scene_initialization_opened(initialization_token="runtime-1")
    snapshot = manager.snapshot()
    assert snapshot["domains"]["scene"]["reconciled"] is True
    assert snapshot["domains"]["runtime_health"]["reconciled"] is True
    assert snapshot["domains"]["motion"]["reconciled"] is False

    adapters.motion_recovery_snapshot(
        offer=None,
        error_code=None,
        error_details=None,
    )
    assert manager.snapshot()["reconciled"] is True


def test_scene_initialization_failure_sets_repeats_and_matching_success_clears() -> None:
    _clock, manager, adapters, lifecycle = _system()

    adapters.scene_initialization_failed(
        error="seed failed",
        initialization_token="runtime-1",
    )
    alarm = _by_definition(
        manager,
        "manyforge.runtime.scene_initialization_fault",
    )[0]
    assert alarm["lifecycleState"] == "active_unacknowledged"
    assert alarm["occurrenceCount"] == 1

    adapters.scene_initialization_failed(
        error="seed still failed",
        initialization_token="runtime-1",
    )
    assert _by_definition(
        manager,
        "manyforge.runtime.scene_initialization_fault",
    )[0]["occurrenceCount"] == 2

    adapters.scene_initialization_opened(initialization_token="runtime-1")
    returned = _by_definition(
        manager,
        "manyforge.runtime.scene_initialization_fault",
    )[0]
    assert returned["lifecycleState"] == "returned_unacknowledged"
    assert [fact.event_type for fact in lifecycle] == [
        "operator_alarm_activated",
        "operator_alarm_returned",
    ]


def test_pose_apply_is_ignored_before_open_then_coalesces_repeats_and_clears() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    failed = {
        "generation": 9,
        "current_generation": 9,
        "generation_invalidated": False,
        "policy": "continuous",
        "reason": "provider_snapshot",
        "required_missing": [],
        "details": [
            {
                "source_id": "isaac-scene",
                "object_id": "box",
                "status": "rejected",
                "reject_reason": "scene_edit_failed",
                "error": "bad transform",
            }
        ],
    }
    adapters.observe_pose_apply(failed)
    assert _by_definition(manager, "manyforge.scene.pose_apply_fault") == []

    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.observe_pose_apply(failed)
    adapters.observe_pose_apply(failed)
    alarm = _by_definition(manager, "manyforge.scene.pose_apply_fault")[0]
    assert alarm["scope"]["object_id"] == "box"
    assert alarm["occurrenceCount"] == 2

    succeeded = {
        **failed,
        "generation": 10,
        "current_generation": 10,
        "details": [
            {
                "source_id": "isaac-scene",
                "object_id": "box",
                "status": "applied",
            }
        ],
    }
    adapters.observe_pose_apply(succeeded)
    assert _by_definition(manager, "manyforge.scene.pose_apply_fault")[0][
        "lifecycleState"
    ] == "returned_unacknowledged"


def test_pose_missing_required_object_uses_its_configured_scope() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.observe_pose_apply(
        {
            "generation": 2,
            "current_generation": 2,
            "generation_invalidated": False,
            "policy": "continuous",
            "reason": "before_planning",
            "required_missing": ["fixture"],
            "error": "no fresh update",
            "details": [],
        }
    )
    alarms = _by_definition(manager, "manyforge.scene.pose_apply_fault")
    assert len(alarms) == 1
    assert alarms[0]["scope"]["object_id"] == "fixture"


def test_collision_condition_uses_normalized_pair_counts_native_repeats_and_clears() -> None:
    _clock, manager, adapters, lifecycle = _system()
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    alarm = _by_definition(manager, "manyforge.motion.runtime_collision")[0]
    assert alarm["scope"] == {
        "robot_resource": "robot0",
        "contact_pair": "object/wall <-> robot/robot0/link/wrist_3",
    }
    assert alarm["occurrenceCount"] == 1

    adapters.motion_occurrence(
        error_details=_collision_details(),
        evidence_ref="kernel-native:8",
        correlations={"native_event_index": 8, "execution_id": 41},
    )
    assert _by_definition(manager, "manyforge.motion.runtime_collision")[0][
        "occurrenceCount"
    ] == 2

    adapters.motion_recovery_snapshot(
        offer=None,
        error_code=None,
        error_details=None,
    )
    returned = _by_definition(manager, "manyforge.motion.runtime_collision")[0]
    assert returned["lifecycleState"] == "returned_unacknowledged"
    assert [fact.event_type for fact in lifecycle] == [
        "operator_alarm_activated",
        "operator_alarm_returned",
    ]


def test_non_collision_motion_fault_replaces_collision_and_uses_fault_identity() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    adapters.motion_recovery_snapshot(
        offer={
            "offerId": "offer-fault",
            "offerRevision": 2,
            "recoveryClass": "unknown_fault",
            "status": "waiting",
            "reasonCode": "controller_fault",
            "evidenceRefs": [],
        },
        error_code="controller_fault",
        error_details={"controller": "arm"},
    )

    collision = _by_definition(manager, "manyforge.motion.runtime_collision")[0]
    fault = _by_definition(manager, "manyforge.motion.execution_fault")[0]
    assert collision["active"] is False
    assert fault["active"] is True
    assert fault["scope"]["fault_code"] == "controller_fault"


def test_collision_without_pair_fails_source_quality_closed() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details={},
    )
    snapshot = manager.snapshot()
    assert snapshot["reconciled"] is False
    assert snapshot["health"]["state"] == "unavailable"
    assert "operator_alarm_motion_identity_unavailable" in snapshot["health"][
        "details"
    ]


def test_provider_waiting_is_not_an_alarm_until_freshness_qualified_then_ready_clears() -> None:
    clock, manager, adapters, _lifecycle = _system(
        providers=("isaac_demo_supervisor",)
    )
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.motion_recovery_snapshot(
        offer=None,
        error_code=None,
        error_details=None,
    )
    assert _by_definition(manager, "manyforge.runtime.provider_unavailable") == []
    assert manager.snapshot()["reconciled"] is False

    clock.advance(5.9)
    adapters.check_provider_freshness()
    assert _by_definition(manager, "manyforge.runtime.provider_unavailable") == []

    clock.advance(0.2)
    adapters.check_provider_freshness()
    alarm = _by_definition(
        manager,
        "manyforge.runtime.provider_unavailable",
    )[0]
    assert alarm["active"] is True
    assert manager.snapshot()["reconciled"] is True

    adapters.observe_provider_status(
        provider_id="isaac_demo_supervisor",
        payload={
            "readiness": "ready",
            "canExecute": True,
            "userMessage": "ready",
        },
        received_at_ns=clock.now_ns,
    )
    returned = _by_definition(
        manager,
        "manyforge.runtime.provider_unavailable",
    )[0]
    assert returned["lifecycleState"] == "returned_unacknowledged"


def test_explicit_provider_fault_activates_immediately_and_large_payload_is_bounded() -> None:
    clock, manager, adapters, _lifecycle = _system(
        providers=("isaac_demo_supervisor",)
    )
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    adapters.observe_provider_status(
        provider_id="isaac_demo_supervisor",
        payload={
            "readiness": "fault",
            "canExecute": False,
            "userMessage": "Isaac unavailable",
            "components": [
                {"id": f"component-{index}", "detail": "x" * 4_000}
                for index in range(100)
            ],
        },
        received_at_ns=clock.now_ns,
    )
    alarm = _by_definition(
        manager,
        "manyforge.runtime.provider_unavailable",
    )[0]
    assert alarm["active"] is True
    assert alarm["details"]["reason"] == "provider_fault"


def test_new_runtime_epoch_retires_old_conditions_and_does_not_reuse_activation() -> None:
    _clock, manager, adapters, _lifecycle = _system()
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    first = _by_definition(manager, "manyforge.motion.runtime_collision")[0]

    adapters.begin_runtime_epoch(
        epoch="runtime-2",
        runtime_lane="isaac-sim",
        robot_resource="robot0",
        pose_scopes=(),
        required_providers=(),
    )
    retired = _by_definition(manager, "manyforge.motion.runtime_collision")[0]
    assert retired["lifecycleState"] == "returned_unacknowledged"

    adapters.motion_recovery_snapshot(
        offer=_collision_offer(revision=2),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    recurrence = _by_definition(manager, "manyforge.motion.runtime_collision")[0]
    assert recurrence["active"] is True
    assert recurrence["activationId"] != first["activationId"]


def test_lifecycle_sink_failure_does_not_rollback_alarm_and_marks_health() -> None:
    clock = _Clock()
    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )

    def fail_sink(_facts: tuple[Any, ...]) -> None:
        raise OSError("disk unavailable")

    adapters = OperatorAlarmAdapters(
        manager,
        lifecycle_sink=fail_sink,
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    snapshot = manager.snapshot()
    assert snapshot["alarms"][0]["active"] is True
    assert "operator_alarm_lifecycle_persistence_failed" in snapshot["health"][
        "details"
    ]
    assert adapters.persistence_status()["metrics"][
        "lifecycle_persistence_failures"
    ] == 1


def test_lifecycle_outbox_overflow_is_bounded_and_fails_health_closed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(adapter_module, "_MAX_PENDING_LIFECYCLE_FACTS", 1)
    clock = _Clock()
    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )

    def fail_sink(_facts: tuple[Any, ...]) -> None:
        raise OSError("disk unavailable")

    adapters = OperatorAlarmAdapters(
        manager,
        lifecycle_sink=fail_sink,
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    adapters.scene_initialization_failed(
        error="seed failed",
        initialization_token="runtime-1",
    )
    status = adapters.persistence_status()
    assert status["pendingLifecycleFacts"] == 1
    assert status["capacity"] == 1
    assert status["metrics"]["lifecycle_persistence_overflow_facts"] >= 1
    assert "operator_alarm_lifecycle_outbox_overflow" in manager.snapshot()[
        "health"
    ]["details"]


def test_expected_duplicate_adapter_state_is_suppressed_and_counted() -> None:
    _clock, _manager, adapters, _lifecycle = _system()
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    before = adapters.persistence_status()["metrics"].get(
        "expected_lifecycle_suppressions", 0
    )
    adapters.scene_initialization_opened(initialization_token="runtime-1")
    after = adapters.persistence_status()["metrics"][
        "expected_lifecycle_suppressions"
    ]
    assert after > before


@pytest.mark.parametrize("restart_kind", ["orderly", "crash"])
def test_restart_reconciles_from_fresh_authority_and_reannunciates_active_condition(
    restart_kind: str,
) -> None:
    _clock, first_manager, first_adapters, _lifecycle = _system(
        activation_prefix="before-restart"
    )
    first_adapters.scene_initialization_opened(initialization_token="runtime-1")
    first_adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    first = _by_definition(
        first_manager, "manyforge.motion.runtime_collision"
    )[0]
    first_manager.acknowledge(
        condition_id=first["conditionId"],
        activation_id=first["activationId"],
        expected_record_revision=first["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    assert first_manager.snapshot()["activeAcknowledgedCount"] == 1
    if restart_kind == "orderly":
        assert first_adapters.flush_lifecycle_facts() == []
        assert first_adapters.persistence_status()["pendingLifecycleFacts"] == 0
    # The crash case intentionally discards the first manager without a
    # shutdown callback. Both policies reconstruct only from fresh authority.

    _clock, restarted_manager, restarted_adapters, _lifecycle = _system(
        activation_prefix="after-restart"
    )
    assert restarted_manager.snapshot()["reconciled"] is False
    restarted_adapters.scene_initialization_opened(
        initialization_token="runtime-1"
    )
    restarted_adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    restarted = _by_definition(
        restarted_manager, "manyforge.motion.runtime_collision"
    )[0]
    assert restarted_manager.snapshot()["reconciled"] is True
    assert restarted["lifecycleState"] == "active_unacknowledged"
    assert restarted["activationId"] != first["activationId"]
    assert restarted["acknowledgementPrincipal"] is None


def test_repeated_condition_keeps_one_live_alarm_without_losing_raw_history() -> None:
    clock = _Clock()
    recorder = EventRecorder(max_events=512)
    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )
    adapters = OperatorAlarmAdapters(
        manager,
        lifecycle_sink=recorder.record_operator_alarm_lifecycle_facts,
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
    )
    adapters.begin_runtime_epoch(
        epoch="runtime-raw-history",
        runtime_lane="isaac-sim",
        robot_resource="robot0",
        pose_scopes=(),
        required_providers=(),
    )
    adapters.motion_recovery_snapshot(
        offer=_collision_offer(),
        error_code="runtime_collision",
        error_details=_collision_details(),
    )
    for index in range(128):
        event_index = recorder._record_event(  # noqa: SLF001 - raw-history proof
            event_type="collision_detected",
            execution_id=41,
            details={
                "reason": "runtime_collision",
                **_collision_details(),
            },
        )
        adapters.motion_occurrence(
            error_details=_collision_details(),
            evidence_ref=f"event-index:{event_index}",
            correlations={"execution_id": 41},
        )

    raw = recorder.history(
        limit=512,
        event_types=["collision_detected"],
    )["events"]
    alarms = _by_definition(manager, "manyforge.motion.runtime_collision")
    assert len(raw) == 128
    assert len(alarms) == 1
    assert alarms[0]["occurrenceCount"] == 129
    assert manager.metrics()["gauges"]["liveAlarms"] == 1
