"""Thin addressed Kernel client and behavior-owned action spans."""

from __future__ import annotations

from copy import deepcopy
from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionCommandError,
    ExecutionState,
    KernelClient,
    KernelClientResponseError,
    KernelImmediateStepFailedError,
    KernelOperationAdmissionRejectedError,
    KernelOperationNotYetPushedError,
    KernelOperationObservationUnavailableError,
    KernelOperationSettlementPendingError,
    KernelPlanningJobRefusedError,
    KernelStopNotResidentError,
    LegacyKernelMockUnavailableError,
    MockKernelClient,
)
from .gateway_fakes import transport_gateway


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
            # The wire form carries the two optional texts.
            assert (operation_id, revision, body) == (
                1, 3, {**request, "principal_id": "", "reason": ""})
            return {"ok": True, "result": result}

    with pytest.raises(KernelClientResponseError, match="exact owner result"):
        KernelClient(transport_gateway(Transport())).recover(1, 3, request)


def test_recovery_result_echo_is_not_compared() -> None:
    # Each exchange is its own connection: the echoed identity is not compared.
    request = {"request_key": "c" + "0" * 30 + "1", "action": "acknowledge_and_continue"}
    result = {"request_key": "c" + "0" * 30 + "2", "action": "reset_cycle",
              "accepted": True, "resulting_assessment": {}}

    class Transport(FakeTransport):
        def apply_operation_recovery(self, operation_id, revision, body):
            return {"ok": True, "result": result}

    assert KernelClient(transport_gateway(Transport())).recover(1, 3, request) == result


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
    client = KernelClient(transport_gateway(transport))

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

    with pytest.raises(ExecutionCommandError) as refused:
        KernelClient(transport_gateway(BadTransport())).identity()
    # A refusal without its typed error proves nothing about the owner.
    assert refused.value.code == "execution_command_response_invalid"
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
        KernelClient(transport_gateway(RejectedTransport())).execute({"program_run_id": 1})
    assert rejected.value.code == "invalid_state_transition"
    assert rejected.value.details == {"reason": "program_run_stop_fence_active"}

    class LostTransport(FakeTransport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            raise TimeoutError("acceptance reply lost")

    with pytest.raises(TimeoutError, match="acceptance reply lost"):
        KernelClient(transport_gateway(LostTransport())).execute({"program_run_id": 1})


def test_only_exact_operation_owner_unavailability_is_typed_for_retry() -> None:
    class OwnerUnavailableError(RuntimeError):
        code = "execution_owner_unavailable"

    class UnavailableObservationTransport(FakeTransport):
        def read(self, operation_id: int) -> dict[str, Any]:
            raise OwnerUnavailableError("HTTP 503")

    client = KernelClient(transport_gateway(UnavailableObservationTransport()))
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.operation(1)

    class MalformedObservationTransport(FakeTransport):
        def read(self, operation_id: int) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(ExecutionCommandError) as refused:
        KernelClient(transport_gateway(MalformedObservationTransport())).operation(1)
    assert refused.value.code == "execution_command_response_invalid"


def test_only_exact_in_flight_effect_is_typed_as_settlement_pending() -> None:
    class EffectInFlightError(RuntimeError):
        code = "invalid_state_transition"
        details = {"reason": "operation_effect_in_flight"}

    class SettlingTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise EffectInFlightError("subordinate effect is still settling")

    client = KernelClient(transport_gateway(SettlingTransport()))
    with pytest.raises(KernelOperationSettlementPendingError):
        client.acknowledge_completed(
            1, {"operation_id": 1, "expected_operation_revision": 2}
        )

    class RejectedTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise RuntimeError("permanent rejection")

    with pytest.raises(RuntimeError, match="permanent rejection"):
        KernelClient(transport_gateway(RejectedTransport())).acknowledge_completed(
            1, {"operation_id": 1, "expected_operation_revision": 2}
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
    assert KernelClient(transport_gateway(transport)).read_stop(7) == status
    assert transport.calls == [("read_stop", 7)]


def test_read_stop_of_an_overwritten_record_is_typed() -> None:
    transport = StopTransport({"ok": False, "error": {
        "code": "stop_status_not_resident", "message": "overwritten", "details": {}}})
    with pytest.raises(KernelStopNotResidentError) as caught:
        KernelClient(transport_gateway(transport)).read_stop(7)
    assert (caught.value.code, caught.value.event_id) == ("stop_status_not_resident", 7)
    other = StopTransport({"ok": False, "error": {"code": "execution_owner_unavailable"}})
    with pytest.raises(ExecutionCommandError) as invalid:
        KernelClient(transport_gateway(other)).read_stop(7)
    assert invalid.value.code == "execution_command_response_invalid"


@pytest.mark.parametrize("patch", [
    {"event_id": 8},
    {"state": "succeeded"},
    {"schema_version": "manyforge.execution_stop_status.v1"},
])
def test_read_stop_refuses_another_event_or_shape(patch) -> None:
    transport = StopTransport(
        {"ok": True, "command": "read_stop", "stop_status": _stop_status(**patch)})
    with pytest.raises(KernelClientResponseError, match="event identity"):
        KernelClient(transport_gateway(transport)).read_stop(7)


def test_release_halt_names_the_hold_revision_and_returns_the_scope() -> None:
    scope = {"schema_version": "manyforge.program_execution_scope.v4",
             "program_run_id": 3, "hold_revision": 12, "frozen": "none"}
    transport = StopTransport({"ok": True, "command": "release_halt", "scope": scope})
    assert KernelClient(transport_gateway(transport)).release_halt(3, 11) == scope
    assert transport.calls == [("release_halt", (3, 11))]
    for reply in (
        {"ok": True, "command": "release_program_run_stop_fence", "scope": scope},
        {"ok": True, "command": "release_halt", "scope": {**scope, "hold_revision": None}},
        {"ok": True, "command": "release_halt"},
    ):
        with pytest.raises(KernelClientResponseError, match="Halt release"):
            KernelClient(transport_gateway(StopTransport(reply))).release_halt(3, 11)


def test_cell_stop_sends_the_request_to_host_cell_stop() -> None:
    body = {"request_context": {"request_key": "0" * 32, "source_label": "manyforge_behavior"}}
    accepted = {"event_id": 4, "applied": "cell_stop", "requested": "cell_stop"}
    transport = StopTransport({"ok": True, "command": "cell_stop", "accepted": accepted})
    assert KernelClient(transport_gateway(transport)).cell_stop(body)["accepted"] == accepted
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
    assert getattr(KernelClient(transport_gateway(transport)), command)({"program_run_id": 1}, 5) == scope
    assert transport.seen == 5
    stale = Transport({"ok": True, "command": command, "scope": {"selection_revision": 6}})
    with pytest.raises(KernelClientResponseError):
        getattr(KernelClient(transport_gateway(stale)), command)({"program_run_id": 1}, 5)


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
    assert KernelClient(transport_gateway(Transport(reply))).stop_program_selection({}) == accepted
    without_stop = {**accepted, "stop": None, "replay_status": "accepted_new"}
    with pytest.raises(KernelClientResponseError, match="omitted its scope"):
        KernelClient(transport_gateway(Transport({**reply, "accepted": without_stop}))).stop_program_selection({})


def test_recovery_assessment_names_the_stop_event() -> None:
    class Transport(FakeTransport):
        def operation_recovery(self, operation_id):
            return {"ok": True, "operation_revision": 2, "participants": [{"ordinal": 1}],
                    "stop_event_id": 9,
                    "recovery_assessment": {"recovery_class": "ordinary_operator_stop",
                                            "stop_event_id": 9}}

    assessment = KernelClient(transport_gateway(Transport())).recovery_assessment(1)
    assert assessment["stop_event_id"] == 9
    assert "stop_id" not in assessment and "stop_generation" not in assessment


# OD-213: the runner reads its operations from the core's pushed copy
# (``manyforge_headless_py.ExecutionEvents``). The copy's own semantics
# (replacement, unavailability, malformed tables) are the core's gtest
# (test_gateway_events.cpp); here the client's use of it.


class _CodedError(RuntimeError):
    """The core's GatewayError as the client sees it: a typed ``code``."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


class PushedEvents:
    """The core follower's two memory reads."""

    def __init__(self) -> None:
        self.envelopes: dict[int, dict[str, Any] | None] = {}
        self.jobs: dict[str, str] = {}
        self.error: str | None = "execution_event_mirror_unavailable"
        self.calls: list[tuple[Any, ...]] = []

    def operation_envelope(self, operation_id: int, after_change: int) -> dict[str, Any] | None:
        self.calls.append(("operation_envelope", operation_id, after_change))
        if self.error is not None:
            raise _CodedError(self.error)
        return deepcopy(self.envelopes.get(operation_id))

    def planning_job_state(self, job_id: str) -> str | None:
        self.calls.append(("planning_job_state", job_id))
        if self.error is not None:
            raise _CodedError(self.error)
        return self.jobs.get(job_id)


_PUSHED_ENVELOPE = {
    "ok": True,
    "command": "read",
    "operation": {"operation_id": 4, "operation_revision": 1, "state": "executing"},
    "resident": True,
    "last_control": None,
    "control_pending": False,
    "active_tcp": [{"robot": "arm"}],
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
    transport, events = CountingReadTransport(), PushedEvents()
    client = KernelClient(transport_gateway(transport), events)
    assert client.pushes_operations
    events.error, events.envelopes[4] = None, _PUSHED_ENVELOPE

    assert client.observed_operation(4, 7) == _PUSHED_ENVELOPE
    assert events.calls == [("operation_envelope", 4, 7)]
    assert transport.reads == []


def test_pushed_table_older_than_the_acceptance_waits_or_reads_once() -> None:
    transport, events = CountingReadTransport(), PushedEvents()
    client = KernelClient(transport_gateway(transport), events)
    events.error = "execution_event_mirror_behind"

    # A per-tick observer stays running; it is an unavailable observation.
    with pytest.raises(KernelOperationNotYetPushedError):
        client.observed_operation(4, 7)
    assert issubclass(KernelOperationNotYetPushedError, KernelOperationObservationUnavailableError)
    assert transport.reads == []
    # A one-shot caller (a halt's cancel) reads instead.
    assert client.observed_operation(4, 7, wait=False)["resident"] is False
    assert transport.reads == [4]


def test_pushed_table_reads_once_what_it_cannot_show() -> None:
    # Retired, beyond the list's bound, or listed without its state.
    transport, events = CountingReadTransport(), PushedEvents()
    client = KernelClient(transport_gateway(transport), events)
    events.error = None
    assert client.observed_operation(4, 7)["resident"] is False
    assert transport.reads == [4]


def test_pushed_table_unavailable_is_an_unavailable_observation() -> None:
    transport, events = CountingReadTransport(), PushedEvents()
    client = KernelClient(transport_gateway(transport), events)
    for wait in (True, False):
        with pytest.raises(KernelOperationObservationUnavailableError) as raised:
            client.observed_operation(4, 0, wait=wait)
        assert not isinstance(raised.value, KernelOperationNotYetPushedError)
        assert raised.value.__cause__.code == "execution_event_mirror_unavailable"
    assert transport.reads == []
    # Any other failure keeps its own type.
    events.error = "execution_event_stream_invalid"
    with pytest.raises(_CodedError):
        client.observed_operation(4, 0)


def test_observed_operation_needs_the_acceptance_counter_and_reads_without_a_table() -> None:
    transport, events = CountingReadTransport(), PushedEvents()
    events.error = None
    with pytest.raises(KernelClientResponseError, match="operations change counter"):
        KernelClient(transport_gateway(transport), events).observed_operation(4, None)
    # A process without the event subscription reads.
    plain = KernelClient(transport_gateway(transport))
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
    client = KernelClient(transport_gateway(transport))
    assert client.plan_pick({"sequence_id": "job-1"})["planning_job"]["state"] == "planning"
    assert client.read_planning_job("job-1")["command"] == "read_planning_job"
    assert client.cancel_planning_job("job-1")["command"] == "cancel_planning_job"
    assert client.release_planning_job("job-1") == {"ok": True, "command": "release_planning_job"}
    transport.refusal = "planning_job_exists"
    with pytest.raises(KernelPlanningJobRefusedError) as refused:
        client.plan_pick({"sequence_id": "job-1"})
    assert refused.value.code == "planning_job_exists"
    # Every coded refusal reply of a planning-job command is the typed
    # planning refusal, whatever its code (as before the core gateway).
    for code, command in (
        ("planning_job_unknown", client.cancel_planning_job),
        ("planning_job_unfinished", client.release_planning_job),
        ("execution_owner_unavailable", client.read_planning_job),
    ):
        transport.refusal = code
        with pytest.raises(KernelPlanningJobRefusedError) as refused:
            command("job-1")
        assert (refused.value.code, str(refused.value)) == (code, "refused")


def test_planning_job_state_reads_the_pushed_copy_or_the_host_without_one() -> None:
    transport, events = PlanningJobTransport(), PushedEvents()
    client = KernelClient(transport_gateway(transport), events)
    # No snapshot yet: unavailable (the node stays RUNNING).
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.planning_job_state("job-1")
    events.error, events.jobs = None, {"job-1": "done"}
    assert client.planning_job_state("job-1") == "done"
    assert client.planning_job_state("job-2") is None  # Not pushed yet.
    assert transport.calls == []  # Memory only.
    # Without the event subscription the job is read.
    assert KernelClient(transport_gateway(transport)).planning_job_state("job-1") == "planning"
    assert transport.calls == [("read_planning_job", "job-1")]


# The gateway members of every call -----------------------------------------


class _Gateway:
    """The core gateway as the client sees it: one method per command."""

    def __init__(self, reply: Mapping[str, Any] | None = None) -> None:
        self.sent: list[tuple[str, dict[str, Any]]] = []
        self.reply = dict(reply or {"ok": True})

    def __getattr__(self, command: str):
        if command.startswith("_"):
            raise AttributeError(command)

        def call(members: Mapping[str, Any] = {}, principal_id: str | None = None):
            assert principal_id is None
            self.sent.append((command, deepcopy(dict(members))))
            return deepcopy(self.reply)

        return call


_CONTEXT = {"request_key": "c" * 32, "source_label": "program"}


@pytest.mark.parametrize(
    ("call", "command", "members"),
    [
        (lambda c: c.identity(), "identity", {}),
        (lambda c: c.targets(), "targets", {}),
        (lambda c: c.read_active_tcp(), "read_active_tcp", {}),
        (lambda c: c.read_active_tcp(ordered=True), "read_active_tcp", {"ordered": True}),
        (lambda c: c.reset_active_tcp(), "reset_active_tcp", {}),
        (lambda c: c.register_program_run_capture({"program_run_id": 1}),
         "register_program_run_capture", {"capture": {"program_run_id": 1}}),
        (lambda c: c.query_target({"kind": "io_signal", "frame": None}),
         "query_target", {"query": {"kind": "io_signal"}}),
        (lambda c: c.query_operation({"program_run_id": 1}),
         "query_operation", {"query": {"program_run_id": 1}}),
        (lambda c: c.release_io_watch({"registration_id": "7"}),
         "release_io_watch", {"registration": {"registration_id": "7"}}),
        (lambda c: c.release_io_watch_scope({"node_attempt_id": 1}),
         "release_io_watch", {"scope_request": {"node_attempt_id": 1}}),
        (lambda c: c.end_mock_io_node_simulation({"x": 1}),
         "end_mock_io_node_simulation", {"request": {"x": 1}}),
        (lambda c: c.set_mock_io_run_paused({"x": 1}),
         "set_mock_io_run_paused", {"request": {"x": 1}}),
        (lambda c: c.read_interrupt_rule_state_events(9),
         "events", {"after_sequence": 9, "limit": 16}),
        (lambda c: c.reset_robot_position_capability(), "reset_robot_position_capability", {}),
        (lambda c: c.reset_robot_position({"authorization": {}, "expected_deployment_identity": {}}),
         "reset_robot_position", {"authorization": {}, "expected_deployment_identity": {}}),
        (lambda c: c.operation(4), "read", {"operation_id": 4}),
        (lambda c: c.cell_stop({"request_context": {"source_label": "program"}}),
         "cell_stop", {"request": {"request_context": {"request_key": None,
                                                         "source_label": "program"}}}),
        (lambda c: c.reset(4, {"request_context": _CONTEXT, "operation_id": 4,
                               "expected_operation_revision": 2}),
         "reset", {"request": {"request_context": _CONTEXT, "operation_id": 4,
                               "expected_operation_revision": 2, "reason": None}}),
        (lambda c: c.stop({"request_context": {**_CONTEXT, "application_start_id": "a" * 16},
                           "operations": [{"operation_id": 4}], "components": None,
                           "reason": ""}),
         "stop", {"request": {"request_context": _CONTEXT, "operations": [{"operation_id": 4}]}}),
        (lambda c: c.stop_program_run({"request_context": _CONTEXT, "program_run_id": 1}),
         "stop_program_run", {"request": {"request_context": _CONTEXT, "program_run_id": 1}}),
        (lambda c: c.stop_program_selection({"group_ids": ["g"]}),
         "stop_program_selection", {"request": {"group_ids": ["g"]}}),
        (lambda c: c.check_program_node_motion_attention({"node": "n"}),
         "check_program_node_motion_attention", {"request": {"node": "n"}}),
        (lambda c: c.release_program_selection({"group_ids": ["g"]}, 3),
         "release_program_selection", {"request": {"group_ids": ["g"]},
                                       "expected_hold_revision": 3}),
        (lambda c: c.start_program_selection({"group_ids": ["g"]}, 3),
         "start_program_selection", {"request": {"group_ids": ["g"]},
                                     "expected_hold_revision": 3}),
        (lambda c: c.release_halt(1, 3), "release_halt",
         {"expected_hold_revision": 3, "program_run_id": 1}),
        (lambda c: c.read_stop(7), "read_stop", {"event_id": 7}),
        (lambda c: c.set_rule_active("door", True), "set_rule_active",
         {"rule_id": "door", "active": True}),
        (lambda c: c.reset_rule_activation(), "reset_rule_activation", {}),
        (lambda c: c.clear_outside_change("io", "di_0"), "clear_outside_change",
         {"controllable_component_id": "io", "signal_name": "di_0"}),
        (lambda c: c.set_io_output("io", "do_0", True), "set_io_output",
         {"request": {"signal_id": "io/do_0", "value": True, "origin": "program"}}),
        (lambda c: c.read_planning_job("job-1"), "read_planning_job", {"job_id": "job-1"}),
        (lambda c: c.cancel_planning_job("job-1"), "cancel_planning_job", {"job_id": "job-1"}),
        (lambda c: c.release_planning_job("job-1"), "release_planning_job", {"job_id": "job-1"}),
        (lambda c: c.recovery_assessment(4), "get_operation_recovery", {"operation_id": 4}),
        (lambda c: c.recover(4, 2, {"request_key": "k", "action": "close_operation"}),
         "apply_operation_recovery",
         {"expected_operation_revision": 2, "operation_id": 4,
          "request": {"request_key": "k", "action": "close_operation",
                      "principal_id": "", "reason": ""}}),
        (lambda c: c.resolve_attach_active_tcp("b", {"component_id": "tool"}),
         "resolve_attach_active_tcp", {"program_binding_id": "b",
                                       "target": {"component_id": "tool"}}),
    ],
)
def test_each_call_sends_its_command_and_members(call, command, members) -> None:
    gateway = _Gateway()
    try:
        call(KernelClient(gateway))
    except (KernelClientResponseError, ExecutionCommandError, KeyError, TypeError):
        pass  # Only the request is under test; replies are covered above.
    assert gateway.sent == [(command, members)]


def test_an_operation_is_sent_in_the_hosts_wire_form() -> None:
    gateway = _Gateway({"ok": True, "accepted": {"operation_id": 1}})
    operation = {
        "application_start_id": "a" * 16,  # Checked at the edge, never sent.
        "request_context": {"source_label": "program"},
        "manual_handover": None,
        "participants": [{
            "action_span": {"action_source": "system", "action_span_id": "s"},
            "payload": {
                "kind": "motion",
                "speed_profile": {"velocity_scale": 1.0, "acceleration_scale": 1.0},
                "per_joint_dynamics_limits": {"j1": {"max_velocity_rad_s": 0.5}},
                "goal_frame_reference": None,
                "planner": {"search": "rrt"},
                "collision_rules": [{
                    "rule_id": "r", "a": [{"kind": "attached_object", "component_id": "t",
                                           "collision_body_role_id": None}],
                    "b": [{"kind": "scene_objects"}], "clearance_m": None,
                    "contact": {"max_penetration_m": 0.01, "spring_group": None},
                }],
                "motion": {"kind": "pose_goal", "goal": {}, "motion_type": None,
                           "speed_mode": None},
            },
        }],
    }
    KernelClient(gateway).execute(operation)
    assert gateway.sent == [("submit", {"operation": {
        "program_run_id": None,
        "request_context": {"request_key": None, "source_label": "program"},
        "participants": [{
            "action_span": {"action_source": "system", "action_span_id": "s",
                            "program_run_id": None, "node_instance_id": None,
                            "node_attempt_id": None, "parent_node_attempt_id": None},
            "payload": {
                "kind": "motion",
                "target_reference": None,
                "offset_reference": None,
                "speed_profile": {"velocity_scale": 1.0, "acceleration_scale": 1.0,
                                  "max_tcp_linear_speed_mps": None},
                "per_joint_dynamics_limits": {"j1": {
                    "max_velocity_rad_s": 0.5, "max_acceleration_rad_s2": None,
                    "max_jerk_rad_s3": None}},
                # Sent as built: the host reads absent members as the default.
                "planner": {"search": "rrt"},
                "collision_rules": [{
                    "rule_id": "r", "a": [{"kind": "attached_object", "component_id": "t"}],
                    "b": [{"kind": "scene_objects"}],
                    "contact": {"max_penetration_m": 0.01},
                }],
                "motion": {"kind": "pose_goal", "goal": {}},
            },
        }],
    }})]


def test_pick_and_place_phases_are_sent_in_the_wire_form() -> None:
    gateway = _Gateway({"ok": True, "planning_job": {"job_id": "pick-1"}})
    phase = {"request_context": _CONTEXT, "participants": []}
    KernelClient(gateway).plan_pick({
        "sequence_id": "pick-1",
        **{name: dict(phase) for name in
           ("approach_operation", "descend_operation", "tool_operation", "retreat_operation")},
    })
    (command, members), = gateway.sent
    assert command == "plan_pick"
    assert members["intent"]["sequence_id"] == "pick-1"
    assert members["intent"]["tool_operation"] == {**phase, "program_run_id": None}


def test_a_planning_job_refusal_is_typed() -> None:
    gateway = _Gateway({"ok": False, "error": {"code": "planning_job_unknown",
                                               "message": "no such job", "details": {}}})
    with pytest.raises(KernelPlanningJobRefusedError) as refused:
        KernelClient(gateway).release_planning_job("job-1")
    assert refused.value.code == "planning_job_unknown"


def test_a_selection_refusal_before_release_is_typed_and_others_are_not() -> None:
    from manyforge_behavior.orchestration_ports import ProgramSelectionReleaseRejectedError

    refusal = {"ok": False, "error": {"code": "invalid_state_transition", "message": "stale",
                                      "details": {"reason": "program_selection_stale"}}}
    with pytest.raises(ProgramSelectionReleaseRejectedError) as rejected:
        KernelClient(_Gateway(refusal)).release_program_selection({}, 1)
    assert (rejected.value.code, rejected.value.details) == (
        "invalid_state_transition", {"reason": "program_selection_stale"})
    other = {"ok": False, "error": {"code": "invalid_state_transition", "message": "x",
                                    "details": {"reason": "something_else"}}}
    with pytest.raises(ExecutionCommandError):
        KernelClient(_Gateway(other)).start_program_selection({}, 1)
    # The Stop of a selection is not a release: its refusal stays the owner's.
    with pytest.raises(ExecutionCommandError):
        KernelClient(_Gateway(refusal)).stop_program_selection({})


def test_an_admitted_immediate_step_failure_carries_the_replys_active_tcp() -> None:
    rows = [{"component_id": "robot", "registration_generation": 1, "sequence": 2,
             "entry": None}]
    reply = {"ok": False, "active_tcp": rows, "error": {
        "code": "set_tcp_failed", "message": "failed",
        "details": {"immediate_step": "admitted", "request_key": "c" * 32,
                    "operation_id": 5}}}
    intent = {"request_context": _CONTEXT,
              "participants": [{"payload": {"kind": "set_tcp"}}]}
    with pytest.raises(KernelImmediateStepFailedError) as failed:
        KernelClient(_Gateway(reply)).execute(intent)
    assert failed.value.active_tcp == rows


def test_gateway_failures_propagate_unchanged() -> None:
    class Lost(RuntimeError):
        code = "execution_owner_unavailable"
        details: dict[str, Any] = {}

    class Gateway(_Gateway):
        def __getattr__(self, command: str):
            def call(members: Mapping[str, Any] = {}, principal_id: str | None = None):
                raise Lost("lost")
            return call

    with pytest.raises(Lost):
        KernelClient(Gateway()).set_rule_active("door", True)
    with pytest.raises(Lost):
        KernelClient(Gateway()).execute({"participants": []})
