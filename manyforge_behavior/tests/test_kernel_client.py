"""Thin addressed Kernel client and behavior-owned action spans."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelClientResponseError,
    KernelOperationAdmissionRejectedError,
    KernelOperationNotYetPushedError,
    KernelOperationObservationUnavailableError,
    KernelOperationSettlementPendingError,
    KernelStopNotResidentError,
    LegacyKernelMockUnavailableError,
    MockKernelClient,
    PushedOperationTable,
)


@pytest.mark.parametrize(
    "field,value",
    [
        ("accepted", 1),
        ("accepted", "true"),
        ("resulting_assessment", None),
    ],
)
def test_recovery_result_must_have_its_shape(field, value) -> None:
    request = {"request_key": "c" + "0" * 30 + "1", "action": "acknowledge_and_continue"}
    result = {**request, "accepted": True, "resulting_assessment": {}, field: value}

    class Transport(FakeTransport):
        def apply_operation_recovery(self, operation_id, revision, body):
            assert (operation_id, revision, body) == (1, 3, request)
            return {"ok": True, "result": result}

    with pytest.raises(KernelClientResponseError, match="exact owner result"):
        KernelClient(Transport()).recover(1, 3, request)


def test_recovery_result_echo_is_not_compared() -> None:
    # Each exchange is its own connection: the echoed identity is not compared.
    request = {"request_key": "c" + "0" * 30 + "1", "action": "acknowledge_and_continue"}
    result = {"request_key": "c" + "0" * 30 + "2", "action": "reset_cycle",
              "accepted": True, "resulting_assessment": {}}

    class Transport(FakeTransport):
        def apply_operation_recovery(self, operation_id, revision, body):
            return {"ok": True, "result": result}

    assert KernelClient(Transport()).recover(1, 3, request) == result


class FakeTransport:
    def __init__(self) -> None:
        self.submitted: Mapping[str, Any] | None = None
        self.controls: list[tuple[str, int, Mapping[str, Any]]] = []

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {"graph_revision": "graph-1"}}

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {"ok": True, "accepted": {"operation_id": 1}}

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "planning_job": {"job_id": "pick-1", "kind": "pick", "state": "accepted"},
        }

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "planning_job": {"job_id": "place-1", "kind": "place", "state": "accepted"},
        }

    def read(self, operation_id: int) -> dict[str, Any]:
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
        }

    def _control(
        self, kind: str, operation_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.controls.append((kind, operation_id, body))
        return {"ok": True, "accepted": {"operation_id": operation_id}}

    def reset(self, operation_id, body):
        return self._control("reset", operation_id, body)

    def acknowledge_completed(self, operation_id, body):
        return self._control("acknowledge_completed", operation_id, body)

    def stop(self, body):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)


def test_execution_state_vocabulary_is_the_native_owner_vocabulary() -> None:
    assert {state.value for state in ExecutionState} == {
        "queued",
        "resolving",
        "planning",
        "validating",
        "prepared",
        "dispatching",
        "executing",
        "stopping",
        "completed",
        "stopped",
        "failed",
        "interrupted",
        "intervention_required",
        "cancelled_pre_dispatch",
    }


def test_program_action_span_requires_exact_attempt_identity() -> None:
    span = ActionSpan(
        action_source="program_node",
        action_span_id="1",
        program_run_id=1,
        node_instance_id="root/node[0]",
        node_attempt_id=1,
    )
    assert span.as_request()["action_span_id"] == str(span.node_attempt_id)
    # The host's action span keeps attempt ids as text (P-21 (a)).
    assert span.as_request()["node_attempt_id"] == "1"
    child = ActionSpan(
        action_source="program_node",
        action_span_id="1:pick",
        program_run_id=1,
        node_instance_id="root/node[0]/pick",
        node_attempt_id="1:pick",
        parent_node_attempt_id=1,
    ).as_request()
    assert (child["node_attempt_id"], child["parent_node_attempt_id"]) == ("1:pick", "1")
    with pytest.raises(ValueError, match="must equal"):
        ActionSpan(
            action_source="program_node",
            action_span_id="span-1",
            node_instance_id="root/node[0]",
            node_attempt_id=2,
        )


def test_kernel_client_only_forwards_bounded_headless_operations() -> None:
    transport = FakeTransport()
    client = KernelClient(transport)

    assert client.identity()["identity"]["graph_revision"] == "graph-1"
    assert client.execute({"program_run_id": 1})["accepted"][
        "operation_id"
    ] == 1
    assert transport.submitted == {"program_run_id": 1}
    assert client.plan_pick({"sequence_id": "pick-1"})["planning_job"][
        "kind"
    ] == "pick"
    assert client.plan_place({"sequence_id": "place-1"})["planning_job"][
        "kind"
    ] == "place"
    assert client.operation(1)["operation"]["state"] == "completed"
    request = {"expected_operation_revision": 1}
    stop_request = {
        "operations": [{"operation_id": 1}],
    }
    assert client.stop(stop_request)["accepted"]["operation_id"] == 1
    assert transport.controls == [("stop", 1, stop_request)]


def test_malformed_success_and_legacy_stateful_mock_fail_closed() -> None:
    class BadTransport(FakeTransport):
        def identity(self) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(BadTransport()).identity()
    with pytest.raises(LegacyKernelMockUnavailableError):
        MockKernelClient()


def test_execute_preserves_explicit_rejection_and_unknown_transport_failure() -> None:
    class RejectedTransport(FakeTransport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            return {
                "ok": False,
                "error": {
                    "code": "invalid_state_transition",
                    "message": "Program run is held",
                    "details": {"reason": "program_run_stop_fence_active"},
                },
            }

    with pytest.raises(KernelOperationAdmissionRejectedError) as rejected:
        KernelClient(RejectedTransport()).execute({"program_run_id": 1})
    assert rejected.value.code == "invalid_state_transition"
    assert rejected.value.details == {"reason": "program_run_stop_fence_active"}

    class LostTransport(FakeTransport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            raise TimeoutError("acceptance reply lost")

    with pytest.raises(TimeoutError, match="acceptance reply lost"):
        KernelClient(LostTransport()).execute({"program_run_id": 1})


def test_only_exact_operation_owner_unavailability_is_typed_for_retry() -> None:
    class OwnerUnavailableError(RuntimeError):
        code = "execution_owner_unavailable"

    class UnavailableObservationTransport(FakeTransport):
        def read(self, operation_id: int) -> dict[str, Any]:
            raise OwnerUnavailableError("HTTP 503")

    client = KernelClient(UnavailableObservationTransport())
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.operation(1)

    class MalformedObservationTransport(FakeTransport):
        def read(self, operation_id: int) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(MalformedObservationTransport()).operation(1)


def test_only_exact_in_flight_effect_is_typed_as_settlement_pending() -> None:
    class EffectInFlightError(RuntimeError):
        code = "invalid_state_transition"
        details = {"reason": "operation_effect_in_flight"}

    class SettlingTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise EffectInFlightError("subordinate effect is still settling")

    client = KernelClient(SettlingTransport())
    with pytest.raises(KernelOperationSettlementPendingError):
        client.acknowledge_completed(
            1, {"expected_operation_revision": 2}
        )

    class RejectedTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise RuntimeError("permanent rejection")

    with pytest.raises(RuntimeError, match="permanent rejection"):
        KernelClient(RejectedTransport()).acknowledge_completed(
            1, {"expected_operation_revision": 2}
        )


# Stop state model: cell Stop, Halt release, Stop event read -----------------


def _stop_status(event_id: int = 7, **patch: Any) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.execution_stop_status.v2",
        "event_id": event_id, "request_key": "0" * 32,
        "requested": "cell_stop", "applied": "cell_stop", "widen_reason": "none",
        "state": "complete", "result": "stopped", "covered": [],
        "accepted_at_ns": 1, "completed_at_ns": 2, **patch,
    }


class StopTransport(FakeTransport):
    def __init__(self, reply: Mapping[str, Any] | None = None) -> None:
        super().__init__()
        self.reply = reply
        self.calls: list[tuple[str, Any]] = []

    def read_stop(self, event_id: int) -> dict[str, Any]:
        self.calls.append(("read_stop", event_id))
        return dict(self.reply or {})

    def release_halt(self, program_run_id: int, expected_hold_revision: int) -> dict[str, Any]:
        self.calls.append(("release_halt", (program_run_id, expected_hold_revision)))
        return dict(self.reply or {})

    def cell_stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.calls.append(("cell_stop", body))
        return dict(self.reply or {})


@pytest.mark.parametrize("state", ["pending", "complete"])
def test_read_stop_returns_the_exact_event_record(state) -> None:
    status = _stop_status(state=state)
    transport = StopTransport({"ok": True, "command": "read_stop", "stop_status": status})
    assert KernelClient(transport).read_stop(7) == status
    assert transport.calls == [("read_stop", 7)]


def test_read_stop_of_an_overwritten_record_is_typed() -> None:
    transport = StopTransport({"ok": False, "error": {
        "code": "stop_status_not_resident", "message": "overwritten", "details": {}}})
    with pytest.raises(KernelStopNotResidentError) as caught:
        KernelClient(transport).read_stop(7)
    assert (caught.value.code, caught.value.event_id) == ("stop_status_not_resident", 7)
    other = StopTransport({"ok": False, "error": {"code": "execution_owner_unavailable"}})
    with pytest.raises(KernelClientResponseError):
        KernelClient(other).read_stop(7)


@pytest.mark.parametrize("patch", [
    {"event_id": 8},
    {"state": "succeeded"},
    {"schema_version": "manyforge.execution_stop_status.v1"},
])
def test_read_stop_refuses_another_event_or_shape(patch) -> None:
    transport = StopTransport(
        {"ok": True, "command": "read_stop", "stop_status": _stop_status(**patch)})
    with pytest.raises(KernelClientResponseError, match="event identity"):
        KernelClient(transport).read_stop(7)


def test_release_halt_names_the_hold_revision_and_returns_the_scope() -> None:
    scope = {"schema_version": "manyforge.program_execution_scope.v4",
             "program_run_id": 3, "hold_revision": 12, "frozen": "none"}
    transport = StopTransport({"ok": True, "command": "release_halt", "scope": scope})
    assert KernelClient(transport).release_halt(3, 11) == scope
    assert transport.calls == [("release_halt", (3, 11))]
    for reply in (
        {"ok": True, "command": "release_program_run_stop_fence", "scope": scope},
        {"ok": True, "command": "release_halt", "scope": {**scope, "hold_revision": None}},
        {"ok": True, "command": "release_halt"},
    ):
        with pytest.raises(KernelClientResponseError, match="Halt release"):
            KernelClient(StopTransport(reply)).release_halt(3, 11)


def test_cell_stop_sends_the_request_to_host_cell_stop() -> None:
    body = {"request_context": {"request_key": "0" * 32, "source_label": "manyforge_behavior"}}
    accepted = {"event_id": 4, "applied": "cell_stop", "requested": "cell_stop"}
    transport = StopTransport({"ok": True, "command": "cell_stop", "accepted": accepted})
    assert KernelClient(transport).cell_stop(body)["accepted"] == accepted
    assert transport.calls == [("cell_stop", body)]
    assert not hasattr(KernelClient, "full_stop")
    assert not hasattr(KernelClient, "stop_status")
    assert not hasattr(KernelClient, "release_program_run_stop_fence")


@pytest.mark.parametrize("verb", ["release", "start"])
def test_selection_release_is_fenced_on_the_hold_revision(verb) -> None:
    command = f"{verb}_program_selection"
    scope = {"hold_revision": 6}

    class Transport(FakeTransport):
        def __init__(self, reply):
            super().__init__()
            self.reply = reply
            self.seen = None

        def release_program_selection(self, body, expected_hold_revision):
            self.seen = expected_hold_revision
            return self.reply

        start_program_selection = release_program_selection

    transport = Transport({"ok": True, "command": command, "scope": scope})
    assert getattr(KernelClient(transport), command)({"program_run_id": 1}, 5) == scope
    assert transport.seen == 5
    stale = Transport({"ok": True, "command": command, "scope": {"selection_revision": 6}})
    with pytest.raises(KernelClientResponseError):
        getattr(KernelClient(stale), command)({"program_run_id": 1}, 5)


def test_selection_stop_carries_its_stop_acceptance() -> None:
    accepted = {"schema_version": "manyforge.program_execution_selection.v3",
                "state": {"hold_revision": 2}, "group_ids": ["all"],
                "selected_node_indices": [0], "widened_node_indices": [],
                "stop": {"event_id": 3, "replay_status": "accepted_new"}}

    class Transport(FakeTransport):
        def __init__(self, reply):
            super().__init__()
            self.reply = reply

        def stop_program_selection(self, body):
            return self.reply

    reply = {"ok": True, "command": "stop_program_selection", "accepted": accepted}
    assert KernelClient(Transport(reply)).stop_program_selection({}) == accepted
    without_stop = {**accepted, "stop": None, "replay_status": "accepted_new"}
    with pytest.raises(KernelClientResponseError, match="omitted its scope"):
        KernelClient(Transport({**reply, "accepted": without_stop})).stop_program_selection({})


def test_recovery_assessment_names_the_stop_event() -> None:
    class Transport(FakeTransport):
        def operation_recovery(self, operation_id):
            return {"ok": True, "operation_revision": 2, "participants": [{"ordinal": 1}],
                    "stop_event_id": 9,
                    "recovery_assessment": {"recovery_class": "ordinary_operator_stop",
                                            "stop_event_id": 9}}

    assessment = KernelClient(Transport()).recovery_assessment(1)
    assert assessment["stop_event_id"] == 9
    assert "stop_id" not in assessment and "stop_generation" not in assessment


# OD-213: the runner's copy of the event stream's pushed operations table.


def _pushed_entry(operation_id: int, state: str = "executing") -> dict[str, Any]:
    return {
        "control_pending": False,
        "last_control": None,
        "operation": {"operation_id": operation_id, "operation_revision": 1, "state": state},
        "operation_id": operation_id,
    }


def _pushed_state(change: int, *entries: dict[str, Any], total: int | None = None) -> dict[str, Any]:
    return {
        "active_tcp_baseline": [{"robot": "arm"}],
        "operations": {
            "change": change,
            "faulted": [],
            "faulted_total": 0,
            "resident": list(entries),
            "resident_total": len(entries) if total is None else total,
        },
        "planning_jobs": [],
    }


class CountingReadTransport(FakeTransport):
    def __init__(self) -> None:
        super().__init__()
        self.reads: list[int] = []

    def read(self, operation_id: int) -> dict[str, Any]:
        self.reads.append(operation_id)
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
            "resident": False,
        }


def test_pushed_table_answers_a_current_operation_without_a_read() -> None:
    transport, table = CountingReadTransport(), PushedOperationTable()
    client = KernelClient(transport, table)
    assert client.pushes_operations
    table.apply_state(_pushed_state(7, _pushed_entry(4)))

    envelope = client.observed_operation(4, 7)

    assert envelope == {
        "ok": True,
        "command": "read",
        "operation": {"operation_id": 4, "operation_revision": 1, "state": "executing"},
        "resident": True,
        "last_control": None,
        "control_pending": False,
        "active_tcp": [{"robot": "arm"}],
    }
    assert transport.reads == []
    # A private copy, like a fresh reply.
    envelope["operation"]["state"] = "changed"
    assert client.observed_operation(4, 7)["operation"]["state"] == "executing"


def test_pushed_table_older_than_the_acceptance_waits_or_reads_once() -> None:
    transport, table = CountingReadTransport(), PushedOperationTable()
    client = KernelClient(transport, table)
    table.apply_state(_pushed_state(6))

    # A per-tick observer stays running; it is an unavailable observation.
    with pytest.raises(KernelOperationNotYetPushedError):
        client.observed_operation(4, 7)
    assert issubclass(KernelOperationNotYetPushedError, KernelOperationObservationUnavailableError)
    assert transport.reads == []
    # A one-shot caller (a halt's cancel) reads instead.
    assert client.observed_operation(4, 7, wait=False)["resident"] is False
    assert transport.reads == [4]


def test_pushed_table_reads_once_what_it_cannot_show() -> None:
    transport, table = CountingReadTransport(), PushedOperationTable()
    client = KernelClient(transport, table)
    # Retired: absent from a current complete table.
    table.apply_state(_pushed_state(9, _pushed_entry(5)))
    assert client.observed_operation(4, 7)["resident"] is False
    # Beyond the list's bound, or listed without its state.
    table.apply_state(_pushed_state(9, total=1))
    assert client.observed_operation(4, 7)["resident"] is False
    table.apply_state(_pushed_state(9, {**_pushed_entry(4), "operation": None}))
    assert client.observed_operation(4, 7)["resident"] is False
    assert transport.reads == [4, 4, 4]


def test_pushed_table_is_unavailable_until_its_first_and_after_a_lost_stream() -> None:
    transport, table = CountingReadTransport(), PushedOperationTable()
    client = KernelClient(transport, table)
    for wait in (True, False):
        with pytest.raises(KernelOperationObservationUnavailableError):
            client.observed_operation(4, 0, wait=wait)
    table.apply_state(_pushed_state(7, _pushed_entry(4)))
    table.stream_lost()
    for wait in (True, False):
        with pytest.raises(KernelOperationObservationUnavailableError) as raised:
            client.observed_operation(4, 0, wait=wait)
        assert not isinstance(raised.value, KernelOperationNotYetPushedError)
    assert transport.reads == []
    table.apply_state(_pushed_state(8, _pushed_entry(4, "completed")))
    assert client.observed_operation(4, 7)["operation"]["state"] == "completed"


@pytest.mark.parametrize(
    "operations",
    [
        None,
        {"change": 1, "resident": [], "resident_total": -1},
        {"change": -1, "resident": [], "resident_total": 0},
        {"change": 1, "resident": None, "resident_total": 0},
        {"change": 1, "resident": [_pushed_entry(4)], "resident_total": 0},
        {"change": 1, "resident": [_pushed_entry(4), _pushed_entry(4)], "resident_total": 2},
        {"change": 1, "resident": [{**_pushed_entry(4), "operation_id": 5}], "resident_total": 1},
        {"change": 1, "resident": [{**_pushed_entry(4), "control_pending": None}],
         "resident_total": 1},
        {"change": 1, "resident": [{**_pushed_entry(4), "last_control": 3}], "resident_total": 1},
    ],
)
def test_malformed_pushed_table_raises_and_makes_the_copy_unavailable(operations) -> None:
    table = PushedOperationTable()
    client = KernelClient(CountingReadTransport(), table)
    table.apply_state(_pushed_state(7, _pushed_entry(4)))
    with pytest.raises(KernelClientResponseError):
        table.apply_state({"operations": operations})
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.observed_operation(4, 7)


def test_observed_operation_needs_the_acceptance_counter_and_reads_without_a_table() -> None:
    transport = CountingReadTransport()
    table = PushedOperationTable()
    table.apply_state(_pushed_state(7, _pushed_entry(4)))
    with pytest.raises(KernelClientResponseError, match="operations change counter"):
        KernelClient(transport, table).observed_operation(4, None)
    # A process without the event subscription reads.
    plain = KernelClient(transport)
    assert not plain.pushes_operations
    assert plain.observed_operation(4, None)["resident"] is False
    assert transport.reads == [4]


class PlanningJobTransport(FakeTransport):
    """The host's planning-job commands (OD-213 (1))."""

    def __init__(self) -> None:
        super().__init__()
        self.calls: list[tuple[str, Any]] = []
        self.refusal: str | None = None
        self.state = "planning"

    def _answer(self, name: str, argument: Any) -> dict[str, Any]:
        self.calls.append((name, argument))
        if self.refusal is not None:
            return {"ok": False, "error": {"code": self.refusal, "message": "refused"}}
        if name == "release_planning_job":
            return {"ok": True, "command": name}
        return {"ok": True, "command": name,
                "planning_job": {"job_id": "job-1", "kind": "pick", "state": self.state}}

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._answer("plan_pick", body)

    def read_planning_job(self, job_id: str) -> dict[str, Any]:
        return self._answer("read_planning_job", job_id)

    def cancel_planning_job(self, job_id: str) -> dict[str, Any]:
        return self._answer("cancel_planning_job", job_id)

    def release_planning_job(self, job_id: str) -> dict[str, Any]:
        return self._answer("release_planning_job", job_id)


def test_planning_job_commands_forward_and_raise_typed_refusals() -> None:
    from manyforge_behavior.kernel_client import KernelPlanningJobRefusedError

    transport = PlanningJobTransport()
    client = KernelClient(transport)
    assert client.plan_pick({"sequence_id": "job-1"})["planning_job"]["state"] == "planning"
    assert client.read_planning_job("job-1")["command"] == "read_planning_job"
    assert client.cancel_planning_job("job-1")["command"] == "cancel_planning_job"
    assert client.release_planning_job("job-1") == {"ok": True, "command": "release_planning_job"}
    transport.refusal = "planning_job_exists"
    with pytest.raises(KernelPlanningJobRefusedError) as refused:
        client.plan_pick({"sequence_id": "job-1"})
    assert refused.value.code == "planning_job_exists"


def test_planning_job_state_reads_the_pushed_copy_or_the_host_without_one() -> None:
    transport = PlanningJobTransport()
    table = PushedOperationTable()
    client = KernelClient(transport, table)
    # No snapshot yet: unavailable (the node stays RUNNING).
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.planning_job_state("job-1")
    state = _pushed_state(1)
    state["planning_jobs"] = [
        {"job_id": "job-1", "kind": "pick", "program_run_id": 3, "state": "done"},
    ]
    table.apply_state(state)
    assert client.planning_job_state("job-1") == "done"
    assert client.planning_job_state("job-2") is None  # Not pushed yet.
    assert transport.calls == []  # Memory only.
    table.stream_lost()
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.planning_job_state("job-1")
    # Without the event subscription the job is read.
    assert KernelClient(transport).planning_job_state("job-1") == "planning"
    assert transport.calls == [("read_planning_job", "job-1")]


@pytest.mark.parametrize(
    "rows",
    [
        None,
        [{"job_id": "", "kind": "pick", "program_run_id": None, "state": "done"}],
        [{"job_id": "a", "kind": "move", "program_run_id": None, "state": "done"}],
        [{"job_id": "a", "kind": "pick", "program_run_id": None, "state": "waiting"}],
        [{"job_id": "a", "kind": "pick", "program_run_id": None, "state": "done"}] * 2,
        [{"job_id": str(index), "kind": "pick", "program_run_id": None, "state": "done"}
         for index in range(9)],
    ],
)
def test_malformed_pushed_planning_jobs_make_both_copies_unavailable(rows) -> None:
    table = PushedOperationTable()
    client = KernelClient(PlanningJobTransport(), table)
    table.apply_state(_pushed_state(7, _pushed_entry(4)))
    state = _pushed_state(8, _pushed_entry(4))
    state["planning_jobs"] = rows
    with pytest.raises(KernelClientResponseError):
        table.apply_state(state)
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.planning_job_state("a")
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.observed_operation(4, 7)
