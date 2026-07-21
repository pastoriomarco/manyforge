"""Four-state operator-alarm reducer and manager qualification."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import itertools
import threading
from typing import Any

import pytest

from manyforge_composer.backend.operator_alarm_catalog import (
    load_operator_alarm_catalog,
)
from manyforge_composer.backend.operator_alarms import (
    AlarmAcknowledgementMember,
    AlarmConditionUpdate,
    AlarmPointState,
    OperatorAlarmError,
    OperatorAlarmManager,
    canonical_condition_id,
    reduce_acknowledgement,
    reduce_condition_update,
)


_DEFINITION_ID = "manyforge.scene.pose_apply_fault"
_SOURCE_ID = "scene-apply-worker"
_ADAPTER_ID = "manyforge.adapter.scene_condition"


class _FakeClock:
    def __init__(self) -> None:
        self.monotonic_ns = 10_000

    def monotonic(self) -> int:
        self.monotonic_ns += 1
        return self.monotonic_ns

    def wall(self) -> str:
        return f"2026-07-21T00:00:00.{self.monotonic_ns:09d}Z"


def _manager(
    *,
    terminal_retention: int = 4096,
    condition_capacity: int = 8192,
    required_domains: tuple[str, ...] = ("scene",),
) -> OperatorAlarmManager:
    clock = _FakeClock()
    activation_ids = itertools.count(1)
    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        required_domains=required_domains,
        terminal_retention=terminal_retention,
        condition_capacity=condition_capacity,
        monotonic_clock=clock.monotonic,
        wall_clock=clock.wall,
        activation_factory=lambda: f"activation-{next(activation_ids)}",
    )
    manager.register_source(
        domain="scene",
        source_id=_SOURCE_ID,
        adapter_id=_ADAPTER_ID,
        source_epoch="scene-epoch-1",
        source_epoch_order=1,
    )
    return manager


def _update(
    sequence: int,
    *,
    active: bool,
    object_id: str = "box",
    epoch: str = "scene-epoch-1",
    quality: str = "good",
    evidence_refs: tuple[str, ...] = (),
    details: dict[str, Any] | None = None,
) -> AlarmConditionUpdate:
    return AlarmConditionUpdate(
        definition_id=_DEFINITION_ID,
        scope={
            "pose_source_id": "isaac-scene",
            "object_id": object_id,
            "apply_policy_id": "continuous",
        },
        active=active,
        source_id=_SOURCE_ID,
        source_epoch=epoch,
        source_sequence=sequence,
        observed_at_ns=sequence * 100,
        observed_at_wall=f"2026-07-21T00:00:{sequence:02d}Z",
        quality=quality,
        details=details or {},
        evidence_refs=evidence_refs,
        correlations={"run_id": "run-20c"},
    )


def _only_alarm(manager: OperatorAlarmManager) -> dict[str, Any]:
    alarms = manager.snapshot()["alarms"]
    assert len(alarms) == 1
    return alarms[0]


def _acknowledge_current(manager: OperatorAlarmManager) -> dict[str, Any]:
    record = _only_alarm(manager)
    result = manager.acknowledge(
        condition_id=record["conditionId"],
        activation_id=record["activationId"],
        expected_record_revision=record["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    assert result.changed is True
    return result.records[0].to_dict()


def _pure_states() -> tuple[
    Any,
    Any,
    dict[str, AlarmPointState],
]:
    catalog = load_operator_alarm_catalog()
    definition = catalog.definition(_DEFINITION_ID)
    condition_id = canonical_condition_id(catalog, definition, _update(1, active=True).scope)
    absent = AlarmPointState(condition_id=condition_id)
    active = reduce_condition_update(
        catalog,
        definition,
        absent,
        _update(1, active=True),
        source_epoch_order=1,
        activation_id="pure-activation-1",
    ).point
    returned_unacknowledged = reduce_condition_update(
        catalog,
        definition,
        active,
        _update(2, active=False),
        source_epoch_order=1,
        activation_id="unused",
    ).point
    active_acknowledged = reduce_acknowledgement(
        definition,
        active,
        activation_id="pure-activation-1",
        expected_record_revision=active.record.record_revision,
        principal_id="operator_ui",
        reason=None,
        acknowledged_at_ns=150,
        acknowledged_at_wall="2026-07-21T00:00:01Z",
    ).point
    returned_acknowledged = reduce_condition_update(
        catalog,
        definition,
        active_acknowledged,
        _update(2, active=False),
        source_epoch_order=1,
        activation_id="unused",
    ).point
    return catalog, definition, {
        "absent": absent,
        "active_unacknowledged": active,
        "active_acknowledged": active_acknowledged,
        "returned_unacknowledged": returned_unacknowledged,
        "returned_acknowledged": returned_acknowledged,
    }


@pytest.mark.parametrize(
    ("current_state", "active", "sequence", "expected_state", "status"),
    [
        ("absent", True, 1, "active_unacknowledged", "activated"),
        (
            "active_unacknowledged",
            True,
            2,
            "active_unacknowledged",
            "active_repeat",
        ),
        (
            "active_unacknowledged",
            False,
            2,
            "returned_unacknowledged",
            "returned",
        ),
        (
            "active_acknowledged",
            True,
            2,
            "active_acknowledged",
            "active_repeat",
        ),
        (
            "active_acknowledged",
            False,
            2,
            "returned_acknowledged",
            "returned",
        ),
        (
            "returned_unacknowledged",
            True,
            3,
            "active_unacknowledged",
            "recurred",
        ),
        (
            "returned_acknowledged",
            True,
            3,
            "active_unacknowledged",
            "recurred",
        ),
        (
            "returned_acknowledged",
            False,
            3,
            "returned_acknowledged",
            "normal_observed",
        ),
    ],
)
def test_pure_condition_reducer_transition_table(
    current_state: str,
    active: bool,
    sequence: int,
    expected_state: str,
    status: str,
) -> None:
    catalog, definition, states = _pure_states()
    original = states[current_state]

    first = reduce_condition_update(
        catalog,
        definition,
        original,
        _update(sequence, active=active),
        source_epoch_order=1,
        activation_id="pure-next-activation",
    )
    second = reduce_condition_update(
        catalog,
        definition,
        original,
        _update(sequence, active=active),
        source_epoch_order=1,
        activation_id="pure-next-activation",
    )

    assert first == second
    assert first.status == status
    assert first.point.record.lifecycle_state == expected_state
    assert states[current_state] == original


@pytest.mark.parametrize(
    ("current_state", "expected_state"),
    [
        ("active_unacknowledged", "active_acknowledged"),
        ("returned_unacknowledged", "returned_acknowledged"),
    ],
)
def test_pure_acknowledgement_reducer_transition_table(
    current_state: str,
    expected_state: str,
) -> None:
    _, definition, states = _pure_states()
    original = states[current_state]
    assert original.record is not None

    result = reduce_acknowledgement(
        definition,
        original,
        activation_id=original.record.activation_id,
        expected_record_revision=original.record.record_revision,
        principal_id="operator_ui",
        reason=None,
        acknowledged_at_ns=999,
        acknowledged_at_wall="2026-07-21T00:00:09Z",
    )

    assert result.point.record.lifecycle_state == expected_state
    assert result.lifecycle_facts[0].event_type == "operator_alarm_acknowledged"
    assert states[current_state] == original


def test_absent_normal_then_activation_repeat_acknowledge_return_and_recur() -> None:
    manager = _manager()
    normal = manager.submit(_update(0, active=False))
    assert normal.status == "normal_observed"
    assert manager.snapshot()["liveCount"] == 0

    activated = manager.submit(
        _update(1, active=True, evidence_refs=("event-1",))
    )
    first = activated.records[0].to_dict()
    assert activated.status == "activated"
    assert first["lifecycleState"] == "active_unacknowledged"
    assert first["active"] is True
    assert first["acknowledged"] is False
    assert first["occurrenceCount"] == 1
    assert first["activationCount"] == 1
    assert activated.lifecycle_facts[0].event_type == "operator_alarm_activated"

    repeated = manager.submit(
        _update(2, active=True, evidence_refs=("event-2",))
    )
    repeat_record = repeated.records[0].to_dict()
    assert repeated.status == "active_repeat"
    assert repeat_record["activationId"] == first["activationId"]
    assert repeat_record["occurrenceCount"] == 2
    assert repeat_record["evidenceRefs"] == ["event-1", "event-2"]
    assert repeated.lifecycle_facts == ()

    standing = _acknowledge_current(manager)
    assert standing["lifecycleState"] == "active_acknowledged"
    assert standing["active"] is True
    assert standing["acknowledged"] is True
    assert manager.snapshot()["unacknowledgedCount"] == 0

    standing_repeat = manager.submit(_update(3, active=True)).records[0].to_dict()
    assert standing_repeat["lifecycleState"] == "active_acknowledged"
    assert standing_repeat["occurrenceCount"] == 3

    returned = manager.submit(_update(4, active=False))
    assert returned.records[0].lifecycle_state == "returned_acknowledged"
    assert returned.lifecycle_facts[0].event_type == "operator_alarm_returned"
    assert manager.snapshot()["liveCount"] == 0
    assert len(manager.terminal_records()) == 1

    recurrence = manager.submit(_update(5, active=True))
    recurring_record = recurrence.records[0].to_dict()
    assert recurrence.status == "recurred"
    assert recurring_record["activationId"] != first["activationId"]
    assert recurring_record["activationCount"] == 2
    assert recurring_record["occurrenceCount"] == 1
    assert recurring_record["lifecycleState"] == "active_unacknowledged"


def test_clear_before_ack_remains_attention_required_and_recurrence_reannunciates() -> None:
    manager = _manager()
    first = manager.submit(_update(1, active=True)).records[0]
    returned = manager.submit(_update(2, active=False)).records[0]

    assert returned.lifecycle_state == "returned_unacknowledged"
    snapshot = manager.snapshot()
    assert snapshot["unacknowledgedCount"] == 1
    assert snapshot["activeCount"] == 0

    recurrence = manager.submit(_update(3, active=True)).records[0]
    assert recurrence.lifecycle_state == "active_unacknowledged"
    assert recurrence.activation_id != first.activation_id
    assert recurrence.activation_count == 2
    assert manager.snapshot()["generation"] == 2


def test_clear_then_ack_and_ack_then_clear_converge_with_serialized_audit_order() -> None:
    clear_first = _manager()
    clear_first.submit(_update(1, active=True))
    returned = clear_first.submit(_update(2, active=False))
    acknowledged = _acknowledge_current(clear_first)
    assert acknowledged["lifecycleState"] == "returned_acknowledged"
    assert returned.lifecycle_facts[0].event_type == "operator_alarm_returned"

    ack_first = _manager()
    ack_first.submit(_update(1, active=True))
    acknowledgement = ack_first.acknowledge(
        condition_id=_only_alarm(ack_first)["conditionId"],
        activation_id=_only_alarm(ack_first)["activationId"],
        expected_record_revision=_only_alarm(ack_first)["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    returned_after_ack = ack_first.submit(_update(2, active=False))
    assert returned_after_ack.records[0].lifecycle_state == "returned_acknowledged"
    assert acknowledgement.lifecycle_facts[0].event_type == (
        "operator_alarm_acknowledged"
    )
    assert returned_after_ack.lifecycle_facts[0].event_type == (
        "operator_alarm_returned"
    )


def test_duplicate_is_idempotent_and_same_sequence_conflict_is_rejected() -> None:
    manager = _manager()
    update = _update(1, active=True)
    original = manager.submit(update).records[0]

    duplicate = manager.submit(update)
    assert duplicate.status == "duplicate"
    assert duplicate.changed is False
    assert duplicate.records[0].record_revision == original.record_revision

    with pytest.raises(OperatorAlarmError) as conflict:
        manager.submit(_update(1, active=False))
    assert conflict.value.code == "operator_alarm_source_sequence_conflict"
    assert _only_alarm(manager)["lifecycleState"] == "active_unacknowledged"


def test_lower_sequence_and_retired_epoch_are_rejected_but_new_epoch_can_clear() -> None:
    manager = _manager()
    manager.submit(_update(10, active=True))

    with pytest.raises(OperatorAlarmError) as stale_sequence:
        manager.submit(_update(9, active=True))
    assert stale_sequence.value.code == "operator_alarm_source_sequence_stale"

    manager.register_source(
        domain="scene",
        source_id=_SOURCE_ID,
        adapter_id=_ADAPTER_ID,
        source_epoch="scene-epoch-2",
        source_epoch_order=2,
    )
    with pytest.raises(OperatorAlarmError) as stale_epoch:
        manager.submit(_update(11, active=True, epoch="scene-epoch-1"))
    assert stale_epoch.value.code == "operator_alarm_source_epoch_stale"

    cleared = manager.submit(_update(0, active=False, epoch="scene-epoch-2"))
    assert cleared.records[0].lifecycle_state == "returned_unacknowledged"


def test_pure_reducer_rejects_epoch_order_regression() -> None:
    catalog, definition, states = _pure_states()
    active = states["active_unacknowledged"]

    with pytest.raises(OperatorAlarmError) as stale:
        reduce_condition_update(
            catalog,
            definition,
            active,
            _update(0, active=False, epoch="retired-epoch"),
            source_epoch_order=0,
            activation_id="unused",
        )
    assert stale.value.code == "operator_alarm_source_epoch_stale"


def test_different_registered_source_cannot_clear_existing_condition() -> None:
    manager = _manager()
    manager.submit(_update(1, active=True))
    manager.register_source(
        domain="scene",
        source_id="other-scene-worker",
        adapter_id=_ADAPTER_ID,
        source_epoch="other-scene-epoch-1",
        source_epoch_order=1,
    )
    foreign_clear = AlarmConditionUpdate(
        definition_id=_DEFINITION_ID,
        scope=dict(_update(2, active=False).scope),
        active=False,
        source_id="other-scene-worker",
        source_epoch="other-scene-epoch-1",
        source_sequence=2,
        observed_at_ns=200,
        observed_at_wall="2026-07-21T00:00:02Z",
    )

    with pytest.raises(OperatorAlarmError) as authority:
        manager.submit(foreign_clear)
    assert authority.value.code == "operator_alarm_source_authority_conflict"
    assert _only_alarm(manager)["active"] is True


def test_unknown_quality_cannot_clear_and_marks_source_unreconciled() -> None:
    manager = _manager()
    manager.complete_source_snapshot(
        source_id=_SOURCE_ID,
        source_epoch="scene-epoch-1",
    )
    assert manager.snapshot()["reconciled"] is True
    manager.submit(_update(1, active=True))

    with pytest.raises(OperatorAlarmError) as rejected:
        manager.submit(_update(2, active=False, quality="unknown"))
    assert rejected.value.code == "operator_alarm_clear_quality_unknown"
    assert _only_alarm(manager)["active"] is True

    manager.mark_source_quality_unknown(
        source_id=_SOURCE_ID,
        source_epoch="scene-epoch-1",
    )
    assert manager.snapshot()["reconciled"] is False


def test_acknowledgement_is_activation_and_revision_fenced_but_replay_is_idempotent() -> None:
    manager = _manager()
    manager.submit(_update(1, active=True))
    stale = _only_alarm(manager)
    manager.submit(_update(2, active=True))
    current = _only_alarm(manager)

    with pytest.raises(OperatorAlarmError) as revision:
        manager.acknowledge(
            condition_id=stale["conditionId"],
            activation_id=stale["activationId"],
            expected_record_revision=stale["recordRevision"],
            principal_id="operator_ui",
            confirmed=True,
        )
    assert revision.value.code == "operator_alarm_record_revision_stale"

    acknowledged = manager.acknowledge(
        condition_id=current["conditionId"],
        activation_id=current["activationId"],
        expected_record_revision=current["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    replay = manager.acknowledge(
        condition_id=current["conditionId"],
        activation_id=current["activationId"],
        expected_record_revision=current["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    assert acknowledged.changed is True
    assert replay.status == "already_acknowledged"
    assert replay.changed is False

    manager.submit(_update(3, active=False))
    recurrence = manager.submit(_update(4, active=True)).records[0]
    with pytest.raises(OperatorAlarmError) as activation:
        manager.acknowledge(
            condition_id=recurrence.condition_id,
            activation_id=current["activationId"],
            expected_record_revision=recurrence.record_revision,
            principal_id="operator_ui",
            confirmed=True,
        )
    assert activation.value.code == "operator_alarm_activation_stale"


def test_acknowledge_all_is_exact_membership_revision_and_generation_fenced() -> None:
    manager = _manager()
    manager.submit(_update(1, active=True, object_id="box-a"))
    manager.submit(_update(1, active=True, object_id="box-b"))
    original = manager.snapshot()
    members = tuple(
        AlarmAcknowledgementMember(
            condition_id=item["conditionId"],
            activation_id=item["activationId"],
            record_revision=item["recordRevision"],
        )
        for item in original["unacknowledgedMembership"]
    )

    with pytest.raises(OperatorAlarmError) as confirmation:
        manager.acknowledge_all(
            members=members,
            expected_generation=original["generation"],
            principal_id="operator_ui",
            confirmed=False,
        )
    assert confirmation.value.code == (
        "operator_alarm_acknowledge_all_confirmation_required"
    )

    manager.submit(_update(2, active=True, object_id="box-a"))
    with pytest.raises(OperatorAlarmError) as membership:
        manager.acknowledge_all(
            members=members,
            expected_generation=original["generation"],
            principal_id="operator_ui",
            confirmed=True,
        )
    assert membership.value.code == "operator_alarm_membership_mismatch"
    assert manager.snapshot()["unacknowledgedCount"] == 2

    refreshed = manager.snapshot()
    refreshed_members = tuple(
        AlarmAcknowledgementMember(
            condition_id=item["conditionId"],
            activation_id=item["activationId"],
            record_revision=item["recordRevision"],
        )
        for item in refreshed["unacknowledgedMembership"]
    )
    manager.submit(_update(1, active=True, object_id="box-c"))
    with pytest.raises(OperatorAlarmError) as generation:
        manager.acknowledge_all(
            members=refreshed_members,
            expected_generation=refreshed["generation"],
            principal_id="operator_ui",
            confirmed=True,
        )
    assert generation.value.code == "operator_alarm_manager_generation_stale"
    assert manager.snapshot()["unacknowledgedCount"] == 3

    current = manager.snapshot()
    exact = tuple(
        AlarmAcknowledgementMember(
            condition_id=item["conditionId"],
            activation_id=item["activationId"],
            record_revision=item["recordRevision"],
        )
        for item in current["unacknowledgedMembership"]
    )
    result = manager.acknowledge_all(
        members=exact,
        expected_generation=current["generation"],
        principal_id="operator_ui",
        confirmed=True,
    )
    assert result.status == "acknowledged_all"
    assert len(result.records) == 3
    assert result.lifecycle_facts[-1].event_type == "operator_alarms_acknowledged"
    assert manager.snapshot()["unacknowledgedCount"] == 0


def test_snapshot_orders_attention_then_priority_and_keeps_complete_membership() -> None:
    manager = _manager()
    for object_id in ("box-a", "box-b", "box-c", "box-d"):
        manager.submit(_update(1, active=True, object_id=object_id))
    snapshot = manager.snapshot(limit=3)

    assert snapshot["liveCount"] == 4
    assert snapshot["unacknowledgedCount"] == 4
    assert len(snapshot["unacknowledgedMembership"]) == 4
    assert len(snapshot["alarms"]) == 3
    assert snapshot["truncated"] is True


def test_terminal_retention_and_condition_capacity_never_evict_live_attention() -> None:
    manager = _manager(terminal_retention=2, condition_capacity=2)
    manager.submit(_update(1, active=True, object_id="box-a"))
    manager.submit(_update(1, active=True, object_id="box-b"))

    with pytest.raises(OperatorAlarmError) as capacity:
        manager.submit(_update(1, active=True, object_id="box-c"))
    assert capacity.value.code == "operator_alarm_condition_capacity_exhausted"
    snapshot = manager.snapshot()
    assert snapshot["liveCount"] == 2
    assert snapshot["health"]["state"] == "unavailable"

    manager.reconcile_health(code="operator_alarm_condition_capacity_exhausted")
    first = snapshot["alarms"][0]
    manager.acknowledge(
        condition_id=first["conditionId"],
        activation_id=first["activationId"],
        expected_record_revision=first["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    object_id = first["scope"]["object_id"]
    manager.submit(_update(2, active=False, object_id=object_id))
    manager.submit(_update(1, active=True, object_id="box-c"))
    assert manager.snapshot()["liveCount"] == 2


def test_terminal_chronology_is_bounded_to_newest_records() -> None:
    manager = _manager(terminal_retention=2, condition_capacity=8)
    for object_id in ("box-a", "box-b", "box-c"):
        activated = manager.submit(
            _update(1, active=True, object_id=object_id)
        ).records[0]
        manager.acknowledge(
            condition_id=activated.condition_id,
            activation_id=activated.activation_id,
            expected_record_revision=activated.record_revision,
            principal_id="operator_ui",
            confirmed=True,
        )
        manager.submit(_update(2, active=False, object_id=object_id))

    terminal = manager.terminal_records()
    assert len(terminal) == 2
    assert [record["scope"]["object_id"] for record in terminal] == [
        "box-c",
        "box-b",
    ]


def test_evidence_is_deduplicated_and_bounded_to_definition_limit() -> None:
    manager = _manager()
    first = tuple(f"evidence-{index}" for index in range(16))
    manager.submit(_update(1, active=True, evidence_refs=first))
    repeated = manager.submit(
        _update(
            2,
            active=True,
            evidence_refs=("evidence-15", "evidence-16"),
        )
    ).records[0]
    assert len(repeated.evidence_refs) == 16
    assert repeated.evidence_refs[0] == "evidence-1"
    assert repeated.evidence_refs[-1] == "evidence-16"


def test_required_domains_reconcile_only_after_each_good_complete_snapshot() -> None:
    manager = _manager(required_domains=("scene", "motion"))
    manager.complete_source_snapshot(
        source_id=_SOURCE_ID,
        source_epoch="scene-epoch-1",
    )
    assert manager.snapshot()["reconciled"] is False

    manager.register_source(
        domain="motion",
        source_id="motion-source",
        adapter_id="manyforge.adapter.motion_condition",
        source_epoch="motion-epoch-1",
        source_epoch_order=1,
    )
    manager.complete_source_snapshot(
        source_id="motion-source",
        source_epoch="motion-epoch-1",
    )
    assert manager.snapshot()["reconciled"] is True


def test_concurrent_duplicate_delivery_has_one_activation_and_constant_cardinality() -> None:
    manager = _manager()
    update = _update(1, active=True)
    with ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(lambda _: manager.submit(update), range(64)))

    assert sum(result.status == "activated" for result in results) == 1
    assert sum(result.status == "duplicate" for result in results) == 63
    snapshot = manager.snapshot()
    assert snapshot["liveCount"] == 1
    assert snapshot["alarms"][0]["occurrenceCount"] == 1


def test_metrics_are_bounded_and_report_flood_chatter_latency_and_ordering() -> None:
    manager = _manager()
    for activation_index in range(11):
        sequence = activation_index * 2 + 1
        manager.submit(_update(sequence, active=True))
        if activation_index < 10:
            manager.submit(_update(sequence + 1, active=False))

    active = _only_alarm(manager)
    before_ack = manager.metrics()
    assert before_ack["totals"]["activations"] == 11
    assert before_ack["totals"]["recurrences"] == 10
    assert before_ack["gauges"]["liveAlarms"] == 1
    assert before_ack["gauges"]["unacknowledgedAttention"] == 1
    assert before_ack["assessment"]["alarmFloodCandidate"] is True
    assert before_ack["assessment"]["chatteringCandidateCount"] == 1
    assert before_ack["assessment"]["chatteringCandidates"][0][
        "conditionId"
    ] == active["conditionId"]
    assert before_ack["distributions"]["ingestLatencyNs"]["count"] == 21
    assert before_ack["retention"]["durationSamples"] == 4096

    manager.acknowledge(
        condition_id=active["conditionId"],
        activation_id=active["activationId"],
        expected_record_revision=active["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )
    manager.submit(_update(200, active=False))
    manager.mark_source_quality_unknown(
        source_id=_SOURCE_ID,
        source_epoch="scene-epoch-1",
    )
    with pytest.raises(OperatorAlarmError):
        manager.submit(_update(199, active=True))

    after = manager.metrics()
    assert after["distributions"]["acknowledgementLatencyNs"]["count"] == 1
    assert after["distributions"]["completedStandingDurationNs"]["count"] == 1
    assert after["totals"]["source_quality_unknown_observations"] == 1
    assert after["totals"]["out_of_order_sequence_updates"] == 1
    assert after["totals"]["stale_updates"] == 1
    assert after["totals"]["peak_live_condition_points"] == 1


def test_simultaneous_clear_and_acknowledgement_converge_without_lost_state() -> None:
    manager = _manager()
    manager.submit(_update(1, active=True))
    original = _only_alarm(manager)
    start = threading.Barrier(2)

    def acknowledge() -> str:
        start.wait()
        try:
            return manager.acknowledge(
                condition_id=original["conditionId"],
                activation_id=original["activationId"],
                expected_record_revision=original["recordRevision"],
                principal_id="operator_ui",
                confirmed=True,
            ).status
        except OperatorAlarmError as exc:
            return exc.code

    def clear() -> str:
        start.wait()
        return manager.submit(_update(2, active=False)).status

    with ThreadPoolExecutor(max_workers=2) as executor:
        ack_future = executor.submit(acknowledge)
        clear_future = executor.submit(clear)
        results = {ack_future.result(), clear_future.result()}

    assert "returned" in results
    if manager.snapshot()["unacknowledgedCount"]:
        _acknowledge_current(manager)
    assert manager.snapshot()["liveCount"] == 0
    assert manager.terminal_records(limit=1)[0]["lifecycleState"] == (
        "returned_acknowledged"
    )


def test_injected_factory_and_clocks_are_not_called_under_manager_lock() -> None:
    catalog = load_operator_alarm_catalog()
    manager: OperatorAlarmManager

    def assert_unlocked_activation() -> str:
        assert manager._lock.locked() is False
        return "activation-unlocked"

    def assert_unlocked_clock() -> int:
        assert manager._lock.locked() is False
        return 123

    manager = OperatorAlarmManager(
        catalog,
        required_domains=("scene",),
        activation_factory=assert_unlocked_activation,
        monotonic_clock=assert_unlocked_clock,
        wall_clock=lambda: "2026-07-21T00:00:00Z",
    )
    manager.register_source(
        domain="scene",
        source_id=_SOURCE_ID,
        adapter_id=_ADAPTER_ID,
        source_epoch="scene-epoch-1",
        source_epoch_order=1,
    )
    manager.submit(_update(1, active=True))
    record = _only_alarm(manager)
    manager.acknowledge(
        condition_id=record["conditionId"],
        activation_id=record["activationId"],
        expected_record_revision=record["recordRevision"],
        principal_id="operator_ui",
        confirmed=True,
    )


def test_oversized_or_malformed_updates_are_rejected_without_state_change() -> None:
    manager = _manager()
    malformed = AlarmConditionUpdate(
        definition_id=_DEFINITION_ID,
        scope={"object_id": "box"},
        active=True,
        source_id=_SOURCE_ID,
        source_epoch="scene-epoch-1",
        source_sequence=1,
        observed_at_ns=1,
        observed_at_wall="2026-07-21T00:00:00Z",
    )
    with pytest.raises(OperatorAlarmError) as scope:
        manager.submit(malformed)
    assert scope.value.code == "operator_alarm_scope_invalid"

    oversized = _update(1, active=True, details={"blob": "x" * (33 * 1024)})
    with pytest.raises(OperatorAlarmError) as details:
        manager.submit(oversized)
    assert details.value.code == "operator_alarm_condition_update_invalid"
    assert manager.snapshot()["liveCount"] == 0


def test_condition_identity_is_length_framed_and_scope_requires_nfc() -> None:
    catalog = load_operator_alarm_catalog()
    definition = catalog.definition("manyforge.motion.runtime_collision")
    first = canonical_condition_id(
        catalog,
        definition,
        {"robot_resource": "ab", "contact_pair": "c"},
    )
    second = canonical_condition_id(
        catalog,
        definition,
        {"robot_resource": "a", "contact_pair": "bc"},
    )
    assert first != second

    manager = _manager()
    decomposed = _update(1, active=True, object_id="e\u0301")
    with pytest.raises(OperatorAlarmError) as normalization:
        manager.submit(decomposed)
    assert normalization.value.code == "operator_alarm_condition_update_invalid"
    with pytest.raises(TypeError):
        decomposed.scope["object_id"] = "box"
