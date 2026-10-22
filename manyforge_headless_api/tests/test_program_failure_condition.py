"""The ordinary Program-failure condition follows the accepted Run only."""

from __future__ import annotations

import asyncio
import hashlib
import json
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_headless_api.component_overview import ComponentOverviewFacts
from manyforge_headless_api.control_protocol import ControlFreshnessAuthority
from manyforge_headless_api.control_state import HeadlessControlStateSource
from manyforge_headless_api.execution_control import ExecutionControlIngress
from manyforge_headless_api.native import HeadlessApiError


def _failure_row(
    operation_id: str = "operation.1",
    *,
    node: str = "move",
    attempt: str = "move.attempt.1",
    resources: list[str] | None = None,
    recovery_class: str = "failed_validation",
    facts: list[dict[str, str]] | None = None,
) -> dict[str, Any]:
    return {
        "node_instance_id": node,
        "node_attempt_id": attempt,
        "operator_facing_run_revision": 2,
        "operation": {
            "source_owner_epoch": "kernel.1",
            "operation_id": operation_id,
            "operation_revision": 1,
        },
        "resource_ids": resources or ["robot.arm"],
        "failure": {"code": "planning_failed", "message": "original failure"},
        "recovery_class": recovery_class,
        "terminal_facts": (
            [{"kind": "local_failure", "effect": "no_effect"}]
            if facts is None
            else facts
        ),
    }


def _cycle(
    *,
    control: int,
    run_state: str | None,
    run_revision: int = 2,
    rows: list[dict[str, Any]] | None = None,
    epoch: str = "runner.1",
    logic_only: bool = False,
    run_id: str = "run.1",
    omit_failures: bool = False,
) -> dict[str, Any]:
    cycle: dict[str, Any] = {
        "program_runner_owner_epoch": epoch,
        "execution_control_revision": control,
        "cycle_generation": 2,
        "state": "idle_after_reset",
        "program_run_id": None,
        "run_revision": None,
        "node_status_revision": None,
        "runtime_control_revision": None,
        "run": None,
        "runtime_controls": None,
        "recovery_offer": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
        "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
        "current_command_kind": None,
        "allowed_commands": [],
        "causal_correlation_id": None,
        "causal_session_id": None,
        "causal_client_sequence": None,
        "failure": None,
    }
    if run_state is None:
        return cycle
    digest = "sha256:" + "a" * 64
    capture = {
        key: digest
        for key in (
            "applied_record_digest",
            "source_document_digest",
            "program_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
            "node_environment_digest",
        )
    }
    capture.update(
        {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": run_id,
            "scenario_id": "pick",
            "program_id": "program.pick",
            "node_policies": [],
            "node_policy_digest": "sha256:" + hashlib.sha256(b"[]").hexdigest(),
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "runner",
                    "owner_epoch": epoch,
                }
            ],
        }
    )
    failed = run_state != "running"
    run = {
        "schema_version": "manyforge.program_run_status.v3",
        "owner_id": "runner",
        "owner_epoch": epoch,
        "run_revision": run_revision,
        "capture": capture,
        "state": run_state,
        "tree_status": "failure" if run_state == "failed" else "running",
        "node_status_revision": run_revision,
        "node_statuses": {"move": "held" if failed else "running"},
        "active_operation_count": 0,
        "reason_code": "planning_failed" if failed else None,
        "message": "original failure" if failed else "",
        "details": None,
        "origin": (
            {"kind": "program_node", "node_instance_id": "move", "owner_api": None}
            if failed
            else None
        ),
    }
    if not omit_failures:
        # The Runner always publishes the key, empty when there are no rows.
        run["operation_failures"] = [] if rows is None else rows
    terminal = run_state in {"failed", "stopped"}
    cycle.update(
        {
            "state": "terminal_awaiting_reset" if terminal else "running",
            "program_run_id": run_id,
            "cycle_claim_generation": 1,
            "terminal_or_settlement_reference": digest if terminal else None,
            "run": run,
            "run_revision": run_revision,
            "node_status_revision": run_revision,
        }
    )
    if logic_only:
        run["origin"] = {
            "kind": "program_node",
            "node_instance_id": "condition",
            "owner_api": None,
        }
    return cycle


class _Observer:
    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []
        self.called: asyncio.Event | None = None
        self.fail = False

    def __call__(self, condition_json: str) -> dict[str, Any]:
        condition = json.loads(condition_json)
        # The native binding requires canonical JSON.
        assert condition_json == json.dumps(
            condition, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        )
        if self.fail:
            raise HeadlessApiError("headless_api_closed", "core is closed")
        self.calls.append(condition)
        if self.called is not None:
            self.called.set()
        return {
            "schema_version": "manyforge.program_failure_observation.v1",
            "status": "observed",
            "changed": True,
        }

    def states(self) -> list[tuple[str, str, int]]:
        return [
            (
                call["state"],
                call["operation"]["operation_id"],
                call["execution_control_revision"],
            )
            for call in self.calls
        ]


def _ingress() -> ExecutionControlIngress:
    return ExecutionControlIngress(
        projection=lambda: ("runner.1", 4),
        dispatch=lambda _request, _stamp: None,
        authority=ControlFreshnessAuthority(
            receiver_incarnation="receiver.1", secret=b"x" * 32
        ),
    )


@pytest.fixture
def source_and_observer():
    ingress = _ingress()
    observer = _Observer()
    source = HeadlessControlStateSource(
        SimpleNamespace(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
        program_failure_observer=observer,
    )
    try:
        yield source, observer
    finally:
        ingress.close()


def _accept(source: HeadlessControlStateSource, cycle: dict[str, Any]) -> None:
    source._accept_cycle(cycle, "critical")  # noqa: SLF001


def test_zero_client_demand_keeps_the_one_program_reader_running() -> None:
    held = _cycle(control=5, run_state="intervention_required", rows=[_failure_row()])

    async def exercise() -> None:
        ingress = _ingress()
        observer = _Observer()
        observer.called = asyncio.Event()
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=lambda: held,
                component_overview_facts=lambda *, program_cycle, cancelled=None: (
                    ComponentOverviewFacts(
                        runtime={},
                        alarms={},
                        authoring_admission={},
                        program_cycle=program_cycle,
                        applied_program={},
                        source_model={},
                        execution_identity_response={},
                        execution_targets_response={},
                        execution_control_response={},
                        errors={},
                    )
                ),
            ),
            ingress,
            program_runner_websocket_url=None,
            program_failure_observer=observer,
        )
        try:
            await source.start_program_demand()
            await asyncio.wait_for(observer.called.wait(), 1)
            reader = source._runner_task  # noqa: SLF001
            assert source.active_clients == 0
            assert reader is not None and not reader.done()
            assert source._cold_task is None  # noqa: SLF001
            assert observer.states() == [("active", "operation.1", 5)]
            # A browser shares the same reader; leaving keeps it running.
            subscription = await source.subscribe()
            assert source._runner_task is reader  # noqa: SLF001
            await source.unsubscribe(subscription)
            assert source._runner_task is reader and not reader.done()  # noqa: SLF001
            assert source._cold_task is None  # noqa: SLF001
            assert source._cycle == held  # noqa: SLF001
        finally:
            await source.close()
            ingress.close()
        assert source._runner_task is None  # noqa: SLF001
        assert reader.done()

    asyncio.run(exercise())


def test_held_operation_failure_is_active_once(source_and_observer) -> None:
    source, observer = source_and_observer
    held = _cycle(control=5, run_state="intervention_required", rows=[_failure_row()])
    _accept(source, held)
    _accept(source, held)
    # Node progress of the same hold restates nothing.
    _accept(
        source,
        _cycle(
            control=5,
            run_state="intervention_required",
            run_revision=3,
            rows=[_failure_row()],
        ),
    )
    assert observer.calls == [
        {
            "schema_version": "manyforge.program_failure_condition.v1",
            "program_runner_owner_epoch": "runner.1",
            "execution_control_revision": 5,
            "program_run_id": "run.1",
            "run_revision": 2,
            "state": "active",
            "occurrence": {
                "node_instance_id": "move",
                "node_attempt_id": "move.attempt.1",
                "operator_facing_run_revision": 2,
            },
            "operation": {
                "source_owner_epoch": "kernel.1",
                "operation_id": "operation.1",
                "operation_revision": 1,
            },
            "resource_ids": ["robot.arm"],
            "failure": {"code": "planning_failed", "message": "original failure"},
        }
    ]


@pytest.mark.parametrize(
    "rows,logic_only",
    [
        (None, True),
        ([], True),
        ([_failure_row(recovery_class="runtime_collision")], False),
        ([_failure_row(recovery_class="stop_fault_intervention")], False),
        (
            [_failure_row(facts=[{"kind": "local_failure", "effect": "effect_uncertain"}])],
            False,
        ),
        (
            [
                _failure_row(
                    facts=[
                        {"kind": "local_failure", "effect": "no_effect"},
                        {"kind": "local_failure", "effect": "effect_uncertain"},
                    ]
                )
            ],
            False,
        ),
        ([_failure_row(facts=[])], False),
    ],
)
def test_logic_collision_stop_and_uncertain_failures_never_activate(
    source_and_observer, rows, logic_only
) -> None:
    source, observer = source_and_observer
    _accept(
        source,
        _cycle(control=5, run_state="failed", rows=rows, logic_only=logic_only),
    )
    assert observer.calls == []


def test_accepted_retry_leaving_the_hold_clears_with_original_provenance(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="intervention_required", rows=[_failure_row()]))
    # Running again without a later accepted control revision is no release.
    _accept(source, _cycle(control=5, run_state="running", run_revision=3))
    assert observer.states() == [("active", "operation.1", 5)]
    # The Runner's accepted Retry advances the control revision exactly once
    # (ProgramRuntime.apply_recovery_action); the cycle stays running.
    _accept(source, _cycle(control=6, run_state="running", run_revision=4))
    assert observer.states() == [
        ("active", "operation.1", 5),
        ("clear", "operation.1", 6),
    ]
    cleared = observer.calls[-1]
    assert cleared["run_revision"] == 4
    assert {key: cleared[key] for key in ("occurrence", "operation", "resource_ids", "failure")} == {
        key: observer.calls[0][key]
        for key in ("occurrence", "operation", "resource_ids", "failure")
    }
    _accept(source, _cycle(control=7, run_state="running", run_revision=5))
    assert len(observer.calls) == 2


def test_failed_run_stays_active_until_reset_completes(source_and_observer) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="failed", rows=[_failure_row()]))
    _accept(
        source,
        _cycle(control=6, run_state="failed", run_revision=3, rows=[_failure_row()]),
    )
    # The Runner keeps a stopped/failed run's rows until Reset retires it.
    _accept(
        source,
        _cycle(control=7, run_state="failed", run_revision=4, rows=[_failure_row()]),
    )
    assert observer.states() == [("active", "operation.1", 5)]
    _accept(source, _cycle(control=8, run_state=None))
    assert observer.states() == [
        ("active", "operation.1", 5),
        ("clear", "operation.1", 8),
    ]
    # Completed Reset retires the run; its last Run revision is retained.
    assert observer.calls[-1]["program_run_id"] == "run.1"
    assert observer.calls[-1]["run_revision"] == 4


def test_stopped_run_with_unresolved_failure_is_active(source_and_observer) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="stopped", rows=[_failure_row()]))
    assert observer.states() == [("active", "operation.1", 5)]


def test_source_invalidation_reports_unknown_and_recovery_restates(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    held = _cycle(control=5, run_state="intervention_required", rows=[_failure_row()])
    _accept(source, held)
    source._invalidate_source("program")  # noqa: SLF001
    source._invalidate_source("program")  # noqa: SLF001
    assert observer.states() == [
        ("active", "operation.1", 5),
        ("unknown", "operation.1", 5),
    ]
    # Recovery with the same current cycle restates the condition as good.
    _accept(source, held)
    assert observer.states()[-1] == ("active", "operation.1", 5)
    assert len(observer.calls) == 3


def test_owner_replacement_never_clears(source_and_observer) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="failed", rows=[_failure_row()]))
    _accept(source, _cycle(control=1, run_state=None, epoch="runner.2"))
    _accept(source, _cycle(control=9, run_state="running", epoch="runner.2"))
    assert observer.states() == [("active", "operation.1", 5)]


def test_multiple_origins_are_observed_once_each_with_own_resources(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    rows = [
        _failure_row("operation.1", resources=["robot.arm"]),
        _failure_row(
            "operation.2",
            node="grip",
            attempt="grip.attempt.1",
            resources=["gripper.a", "gripper.b"],
        ),
    ]
    _accept(source, _cycle(control=5, run_state="intervention_required", rows=rows))
    _accept(source, _cycle(control=5, run_state="intervention_required", rows=rows))
    assert [
        (call["operation"]["operation_id"], call["resource_ids"])
        for call in observer.calls
    ] == [
        ("operation.1", ["robot.arm"]),
        ("operation.2", ["gripper.a", "gripper.b"]),
    ]
    # Both holds were released before this projection was read: the Runner
    # advanced the control revision once per accepted release.
    _accept(source, _cycle(control=7, run_state="running", run_revision=3))
    assert sorted(observer.states()[2:]) == [
        ("clear", "operation.1", 7),
        ("clear", "operation.2", 7),
    ]


def _two_holds() -> list[dict[str, Any]]:
    return [
        _failure_row("operation.1"),
        _failure_row(
            "operation.2", node="grip", attempt="grip.attempt.1", resources=["gripper.a"]
        ),
    ]


def test_retry_of_one_hold_clears_only_its_alarm(source_and_observer) -> None:
    source, observer = source_and_observer
    first, second = _two_holds()
    _accept(
        source, _cycle(control=5, run_state="intervention_required", rows=[first, second])
    )
    # Retry released the first hold; the tree ticks with the second still held.
    # The Runner advanced the control revision once for that release.
    _accept(source, _cycle(control=6, run_state="running", run_revision=3, rows=[second]))
    assert observer.states() == [
        ("active", "operation.1", 5),
        ("active", "operation.2", 5),
        ("clear", "operation.1", 6),
    ]
    # A later revision that releases nothing (e.g. another control) keeps it.
    _accept(source, _cycle(control=7, run_state="running", run_revision=4, rows=[second]))
    assert len(observer.calls) == 3
    # The second hold's release is its own single Runner advance.
    _accept(source, _cycle(control=8, run_state="running", run_revision=5))
    assert observer.states()[-1] == ("clear", "operation.2", 8)


def test_continue_of_one_hold_clears_its_alarm(source_and_observer) -> None:
    source, observer = source_and_observer
    first, second = _two_holds()
    _accept(
        source, _cycle(control=5, run_state="intervention_required", rows=[first, second])
    )
    # Continue closed the second hold's operation; the first remains held.
    # The Runner advanced the control revision once for that release.
    _accept(
        source,
        _cycle(control=6, run_state="intervention_required", run_revision=3, rows=[first]),
    )
    assert observer.states()[2:] == [("clear", "operation.2", 6)]


def test_a_dropped_row_clears_only_under_a_later_control_revision(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    first, second = _two_holds()
    _accept(
        source, _cycle(control=5, run_state="intervention_required", rows=[first, second])
    )
    _accept(
        source,
        _cycle(control=5, run_state="intervention_required", run_revision=3, rows=[first]),
    )
    assert len(observer.calls) == 2
    _accept(
        source,
        _cycle(control=6, run_state="intervention_required", run_revision=4, rows=[first]),
    )
    assert observer.states()[2:] == [("clear", "operation.2", 6)]


def test_observer_failure_never_stops_the_reader_and_is_restated(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    observer.fail = True
    _accept(source, _cycle(control=5, run_state="intervention_required", rows=[_failure_row()]))
    assert source._cycle is not None  # noqa: SLF001
    observer.fail = False
    _accept(
        source,
        _cycle(
            control=5,
            run_state="intervention_required",
            run_revision=3,
            rows=[_failure_row()],
        ),
    )
    assert observer.states() == [("active", "operation.1", 5)]


@pytest.mark.parametrize(
    "row",
    [
        {**_failure_row(), "resource_ids": ["b", "a"]},
        {**_failure_row(), "resource_ids": []},
        {**_failure_row(), "unexpected": 1},
        {**_failure_row(), "operation": {"operation_id": "operation.1"}},
        {**_failure_row(), "operator_facing_run_revision": 0},
    ],
)
def test_malformed_operation_provenance_is_refused_as_a_bad_source(
    source_and_observer, row
) -> None:
    source, observer = source_and_observer
    with pytest.raises(HeadlessApiError):
        _accept(source, _cycle(control=5, run_state="failed", rows=[row]))
    assert observer.calls == []
    assert source._cycle is None  # noqa: SLF001


@pytest.mark.parametrize(
    "facts",
    [
        [
            {"kind": "local_failure", "effect": "no_effect"},
            {"kind": "computed_completion", "effect": "effect_settled"},
        ],
        [
            {"kind": "local_failure", "effect": "no_effect"},
            {"kind": "cancelled_before_dispatch", "effect": "no_effect"},
            {"kind": "preempted_by_addressed_stop", "effect": "effect_settled"},
        ],
    ],
)
def test_raw_co_participant_facts_never_suppress_the_selected_failure(
    source_and_observer, facts
) -> None:
    # SX-1985: every participant's raw terminal fact is kept; a co-participant
    # that succeeded or ended differently does not hide the selected cause.
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="failed", rows=[_failure_row(facts=facts)]))
    assert observer.states() == [("active", "operation.1", 5)]


def test_a_run_without_its_operation_failures_key_is_unknown_never_no_failure(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="intervention_required", rows=[_failure_row()]))
    with pytest.raises(HeadlessApiError):
        _accept(
            source,
            _cycle(control=6, run_state="running", run_revision=3, omit_failures=True),
        )
    assert observer.states() == [("active", "operation.1", 5)]  # Never cleared.


def test_a_new_run_of_the_same_owner_clears_the_previous_runs_failures(
    source_and_observer,
) -> None:
    source, observer = source_and_observer
    _accept(source, _cycle(control=5, run_state="failed", rows=[_failure_row()]))
    # No idle_after_reset was observed in between; the earlier Run is gone.
    _accept(source, _cycle(control=7, run_state="running", run_id="run.2"))
    assert observer.states() == [
        ("active", "operation.1", 5),
        ("clear", "operation.1", 7),
    ]
    assert observer.calls[-1]["program_run_id"] == "run.1"
