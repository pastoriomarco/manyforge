from __future__ import annotations

from copy import deepcopy
import threading
import time
from typing import Any, Callable, Mapping

import pytest

from manyforge_composer.backend.robot_lifecycle import (
    AuthorityHealthEvidence,
    RobotLifecycleClient,
    RobotLifecycleConfig,
    RobotLifecycleTransitionRecorder,
)


class _ManualClock:
    def __init__(self) -> None:
        self.now_ns = time.monotonic_ns()

    def __call__(self) -> int:
        return self.now_ns

    def advance_s(self, seconds: float) -> None:
        self.now_ns += int(seconds * 1_000_000_000)


class _AuthorityProbe:
    def __init__(
        self,
        clock: Callable[[], int],
        *,
        component_epoch: str = "composer-control:test-epoch",
    ) -> None:
        self.clock = clock
        self.component_epoch = component_epoch
        self.sequence = 0
        self.advance_sequence = True
        self.healthy = True
        self.reason = ""

    def __call__(self) -> AuthorityHealthEvidence:
        if self.advance_sequence:
            self.sequence += 1
        return AuthorityHealthEvidence(
            component_epoch=self.component_epoch,
            sequence=self.sequence,
            observed_at_ns=self.clock(),
            healthy=self.healthy,
            reason=self.reason,
        )


def _capability(action_id: str) -> dict[str, Any]:
    return {
        "actionId": action_id,
        "label": action_id,
        "supported": True,
        "available": True,
        "availabilityReason": "",
        "confirmationClass": "explicit",
        "sideEffectSummary": "",
        "expectedPostcondition": "",
        "timeoutNs": 1_000_000_000,
    }


def _snapshot(clock: Callable[[], int] = time.monotonic_ns) -> dict[str, Any]:
    now_ns = clock()
    actions = (
        "robot.prepare_for_motion",
        "robot.make_noncommandable",
        "robot.enable",
        "robot.select_external_control",
        "robot.start_control",
        "robot.activate_hardware",
        "robot.activate_trajectory_controller",
        "robot.deactivate_trajectory_controller",
        "robot.deactivate_hardware",
        "robot.stop_control",
        "robot.disable",
    )
    return {
        "schemaVersion": "manyforge.robot_lifecycle.snapshot.v1",
        "providerInstanceId": "lite6-a:provider:test",
        "robotId": "lite6-a",
        "supervisorInstanceId": "lite6-a:supervisor",
        "authorityProfile": "local_operator",
        "runtimeGenerationId": "runtime-1",
        "providerComponentEpoch": "provider-epoch-1",
        "transportOwnerComponentEpoch": "owner-epoch-1",
        "authorityComponentEpoch": "",
        "cellSupervisorComponentEpoch": "supervisor-epoch-1",
        "providerPublicationSequence": 1,
        "providerPublishedSteadyTimeNs": now_ns,
        "transportOwnerProgressSequence": 1,
        "transportOwnerProgressReceivedSteadyTimeNs": now_ns,
        "transportOwnerProgressHealthy": True,
        "transportOwnerProgressReason": "",
        "snapshotGeneration": 1,
        "capabilityGeneration": 1,
        "observationGeneration": 1,
        "sourceSteadyTimeNs": now_ns,
        "receivedSteadyTimeNs": now_ns,
        "freshUntilSteadyTimeNs": now_ns + 10_000_000_000,
        "transportConnected": "true",
        "identityVerified": "true",
        "positionValid": "true",
        "robotEnabled": "false",
        "vendorControlReady": "false",
        "hardwareComponentActive": "false",
        "trajectoryControllerActive": "false",
        "faultPresent": "false",
        "warningPresent": "false",
        "recoveryRequired": "false",
        "stationary": "true",
        "observationFresh": True,
        "authorityFresh": False,
        "observeReady": True,
        "controllerReady": False,
        "motionAdmitted": False,
        "reconciliationRequired": False,
        "admissionReasonCodes": ["robot_disabled"],
        "adapterStatus": {},
        "authoritySessionId": "",
        "authoritySessionGeneration": 0,
        "commandSource": "",
        "authorityValidUntilSteadyTimeNs": 0,
        "capabilities": [_capability(action) for action in actions],
        "pendingTransaction": None,
        "lastTerminalTransaction": None,
    }


def _recordable_status() -> dict[str, Any]:
    status = _snapshot(lambda: 1_000_000)
    status.update(
        {
            "providerId": status["providerInstanceId"],
            "commandInFlight": None,
            "adapterStatus": {
                "vendor_mode": "0",
                "vendor_state": "4",
                "brake_mask": "0",
                "servo_enable_mask": "0",
                "error_code": "0",
                "warning_code": "0",
                "report_sample_count": "1",
                "joint_read_success_count": "1",
                "joint_write_attempt_count": "0",
                "lifecycle_command_attempt_count": "0",
            },
        }
    )
    return status


def _transition_recorder(
    *,
    material_adapter_status_keys: tuple[str, ...] = (),
) -> tuple[
    RobotLifecycleTransitionRecorder,
    list[tuple[str, dict[str, Any]]],
]:
    events: list[tuple[str, dict[str, Any]]] = []

    def sink(action: str, *, details: dict[str, Any]) -> None:
        events.append((action, details))

    return (
        RobotLifecycleTransitionRecorder(
            sink,
            material_adapter_status_keys=material_adapter_status_keys,
        ),
        events,
    )


def test_lifecycle_transition_recorder_waits_for_real_observation() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    status["providerComponentEpoch"] = ""

    recorder.observe(status)

    assert events == []


def test_lifecycle_transition_recorder_baselines_once_and_ignores_counters(
) -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()

    recorder.observe(status)
    recorder.observe(deepcopy(status))
    counter_only = deepcopy(status)
    counter_only["providerPublicationSequence"] = 2
    counter_only["snapshotGeneration"] = 2
    counter_only["observationGeneration"] = 2
    counter_only["sourceSteadyTimeNs"] = 2_000_000
    counter_only["adapterStatus"]["report_sample_count"] = "200"
    counter_only["adapterStatus"]["joint_read_success_count"] = "200"
    counter_only["adapterStatus"]["joint_write_attempt_count"] = "19"
    counter_only["adapterStatus"]["lifecycle_command_attempt_count"] = "7"
    recorder.observe(counter_only)

    assert [action for action, _details in events] == [
        "observation_baseline"
    ]
    baseline = events[0][1]
    assert baseline["correlation"] == {
        "classification": "observation_baseline"
    }
    assert baseline["facets"]["robot_enabled"] == "false"
    assert baseline["adapter_status"]["vendor_state"] == "4"
    assert baseline["adapter_status"]["report_sample_count"] == "1"


def test_lifecycle_transition_recorder_records_exact_external_delta() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    recorder.observe(status)

    enabled = deepcopy(status)
    enabled["providerPublicationSequence"] = 2
    enabled["snapshotGeneration"] = 2
    enabled["observationGeneration"] = 2
    enabled["robotEnabled"] = "true"
    enabled["adapterStatus"]["vendor_state"] = "2"
    enabled["adapterStatus"]["brake_mask"] = "63"
    enabled["adapterStatus"]["servo_enable_mask"] = "63"
    recorder.observe(enabled)

    assert [action for action, _details in events] == [
        "observation_baseline",
        "observed_transition",
    ]
    transition = events[1][1]
    assert transition["correlation"] == {
        "classification": "external_or_unknown"
    }
    assert transition["causal_attribution"] == "not_inferred"
    assert transition["changed_facets"] == ["robot_enabled"]
    assert transition["changes"]["robot_enabled"] == {
        "previous": "false",
        "current": "true",
    }
    assert transition["adapter_status"]["vendor_state"] == "2"
    assert transition["adapter_status"]["brake_mask"] == "63"
    assert transition["adapter_status"]["servo_enable_mask"] == "63"


def test_lifecycle_transition_recorder_records_declared_vendor_delta() -> None:
    recorder, events = _transition_recorder(
        material_adapter_status_keys=(
            "vendor_mode",
            "vendor_state",
        )
    )
    status = _recordable_status()
    status["adapterStatus"]["vendor_state"] = "2"
    recorder.observe(status)

    studio_stop = deepcopy(status)
    studio_stop["providerPublicationSequence"] = 2
    studio_stop["snapshotGeneration"] = 2
    studio_stop["observationGeneration"] = 2
    studio_stop["adapterStatus"]["vendor_state"] = "4"
    recorder.observe(studio_stop)

    assert [action for action, _details in events] == [
        "observation_baseline",
        "observed_transition",
    ]
    transition = events[-1][1]
    assert transition["changed_facets"] == ["adapter.vendor_state"]
    assert transition["changes"]["adapter.vendor_state"] == {
        "previous": "2",
        "current": "4",
    }
    assert transition["correlation"] == {
        "classification": "external_or_unknown"
    }
    assert transition["causal_attribution"] == "not_inferred"


def test_lifecycle_transition_recorder_rejects_stale_source_reversal() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    recorder.observe(status)

    enabled = deepcopy(status)
    enabled["providerPublicationSequence"] = 2
    enabled["robotEnabled"] = "true"
    recorder.observe(enabled)

    stale = deepcopy(status)
    stale["providerPublicationSequence"] = 1
    recorder.observe(stale)

    assert len(events) == 2
    assert events[-1][1]["changes"]["robot_enabled"]["current"] == "true"


def test_lifecycle_transition_recorder_correlates_without_claiming_cause(
) -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    recorder.observe(status)

    commanded = deepcopy(status)
    commanded["providerPublicationSequence"] = 2
    commanded["robotEnabled"] = "true"
    commanded["commandInFlight"] = {
        "operation": "enable",
        "stage": "robot.prepare_for_motion",
        "startedAtNs": 123,
    }
    recorder.observe(commanded)

    assert events[-1][1]["causal_attribution"] == "not_inferred"
    assert events[-1][1]["correlation"] == {
        "classification": "correlated_manyforge_command",
        "command": {
            "operation": "enable",
            "stage": "robot.prepare_for_motion",
            "startedAtNs": 123,
        },
    }


def test_lifecycle_transition_recorder_uses_new_terminal_only_once() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    recorder.observe(status)

    stopped = deepcopy(status)
    stopped["providerPublicationSequence"] = 2
    stopped["vendorControlReady"] = "true"
    stopped["lastTerminalTransaction"] = {
        "generation": 9,
        "requestId": "composer:request-9",
        "requestSequence": 3,
        "actionId": "robot.prepare_for_motion",
        "commandSource": "composer.local_operator",
        "authoritySessionGeneration": 1,
        "phase": "succeeded",
        "resultReason": "postcondition_observed",
    }
    stopped["commandInFlight"] = {
        "operation": "enable",
        "stage": "robot.prepare_for_motion",
        "startedAtNs": 123,
    }
    recorder.observe(stopped)

    external = deepcopy(stopped)
    external["providerPublicationSequence"] = 3
    external["vendorControlReady"] = "false"
    external["commandInFlight"] = None
    recorder.observe(external)

    assert [action for action, _details in events] == [
        "observation_baseline",
        "observed_transition",
        "transaction_terminal",
        "observed_transition",
    ]
    assert events[1][1]["correlation"]["classification"] == (
        "correlated_manyforge_terminal"
    )
    assert events[3][1]["correlation"] == {
        "classification": "external_or_unknown"
    }


def test_lifecycle_terminal_history_does_not_require_valid_observation() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    status["transportOwnerComponentEpoch"] = ""
    status["observationGeneration"] = 0
    status["lastTerminalTransaction"] = {
        "generation": 4,
        "requestId": "composer:request-4",
        "requestSequence": 2,
        "actionId": "robot.disable",
        "commandSource": "composer.local_operator",
        "authoritySessionGeneration": 1,
        "phase": "failed",
        "resultReason": "transport_unavailable",
    }

    recorder.observe(status)

    assert [action for action, _details in events] == [
        "transaction_terminal"
    ]
    assert events[0][1]["transaction"]["requestId"] == "composer:request-4"
    assert events[0][1]["transaction"]["phase"] == "failed"


def test_lifecycle_terminal_is_not_repeated_when_observation_recovers() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    status["transportOwnerComponentEpoch"] = ""
    status["observationGeneration"] = 0
    status["lastTerminalTransaction"] = {
        "generation": 4,
        "requestId": "composer:request-4",
        "requestSequence": 2,
        "actionId": "robot.disable",
        "commandSource": "composer.local_operator",
        "authoritySessionGeneration": 1,
        "phase": "failed",
        "resultReason": "transport_unavailable",
    }
    recorder.observe(status)

    recovered = deepcopy(status)
    recovered["transportOwnerComponentEpoch"] = "owner-epoch-1"
    recovered["observationGeneration"] = 1
    recovered["providerPublicationSequence"] = 2
    recorder.observe(recovered)

    assert [action for action, _details in events] == [
        "transaction_terminal",
        "observation_baseline",
    ]


def test_lifecycle_terminal_rejects_out_of_order_source_sample() -> None:
    recorder, events = _transition_recorder()
    newer = _recordable_status()
    newer["providerPublicationSequence"] = 3
    newer["lastTerminalTransaction"] = {
        "generation": 3,
        "requestId": "composer:request-3",
        "requestSequence": 3,
        "actionId": "robot.enable",
        "commandSource": "composer.local_operator",
        "authoritySessionGeneration": 1,
        "phase": "succeeded",
        "resultReason": "postcondition_observed",
    }
    recorder.observe(newer)

    stale = deepcopy(newer)
    stale["providerPublicationSequence"] = 2
    stale["lastTerminalTransaction"] = {
        "generation": 2,
        "requestId": "composer:request-2",
        "requestSequence": 2,
        "actionId": "robot.clear_fault",
        "commandSource": "composer.local_operator",
        "authoritySessionGeneration": 1,
        "phase": "succeeded",
        "resultReason": "postcondition_observed",
    }
    recorder.observe(stale)

    assert [action for action, _details in events] == [
        "observation_baseline",
        "transaction_terminal",
    ]
    assert events[-1][1]["transaction"]["requestId"] == "composer:request-3"


def test_lifecycle_history_sink_failure_is_contained() -> None:
    attempts: list[str] = []

    def failing_sink(action: str, *, details: dict[str, Any]) -> None:
        del details
        attempts.append(action)
        raise RuntimeError("history_sink_failed")

    recorder = RobotLifecycleTransitionRecorder(failing_sink)
    status = _recordable_status()
    recorder.observe(status)
    changed = deepcopy(status)
    changed["providerPublicationSequence"] = 2
    changed["robotEnabled"] = "true"
    recorder.observe(changed)

    assert attempts == [
        "observation_baseline",
        "observed_transition",
    ]


def test_lifecycle_transition_recorder_new_epoch_gets_new_baseline() -> None:
    recorder, events = _transition_recorder()
    status = _recordable_status()
    recorder.observe(status)

    restarted = deepcopy(status)
    restarted["providerComponentEpoch"] = "provider-epoch-2"
    restarted["transportOwnerComponentEpoch"] = "owner-epoch-2"
    restarted["providerPublicationSequence"] = 1
    recorder.observe(restarted)

    assert [action for action, _details in events] == [
        "observation_baseline",
        "observation_baseline",
    ]
    assert events[-1][1]["provider_component_epoch"] == "provider-epoch-2"


def test_lifecycle_transition_recorder_serializes_reduction_and_sink() -> None:
    entered_first_sink = threading.Event()
    release_first_sink = threading.Event()
    second_finished = threading.Event()
    events: list[tuple[str, int]] = []

    def sink(action: str, *, details: dict[str, Any]) -> None:
        provider_sequence = details["provider_publication_sequence"]
        if action == "observed_transition" and provider_sequence == 2:
            entered_first_sink.set()
            assert release_first_sink.wait(timeout=1.0)
        events.append((action, provider_sequence))

    recorder = RobotLifecycleTransitionRecorder(sink)
    status = _recordable_status()
    recorder.observe(status)
    sequence_two = deepcopy(status)
    sequence_two["providerPublicationSequence"] = 2
    sequence_two["robotEnabled"] = "true"
    sequence_three = deepcopy(sequence_two)
    sequence_three["providerPublicationSequence"] = 3
    sequence_three["robotEnabled"] = "false"

    first = threading.Thread(
        target=recorder.observe,
        args=(sequence_two,),
    )

    def record_second() -> None:
        recorder.observe(sequence_three)
        second_finished.set()

    second = threading.Thread(target=record_second)
    first.start()
    assert entered_first_sink.wait(timeout=1.0)
    second.start()
    assert second_finished.wait(timeout=0.05) is False
    release_first_sink.set()
    first.join(timeout=1.0)
    second.join(timeout=1.0)

    assert first.is_alive() is False
    assert second.is_alive() is False
    assert events == [
        ("observation_baseline", 1),
        ("observed_transition", 2),
        ("observed_transition", 3),
    ]


class _ScriptedLifecycleTransport:
    def __init__(
        self,
        snapshot_sink: Callable[[dict[str, Any]], None],
        clock: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        self.snapshot_sink = snapshot_sink
        self._clock = clock
        self.snapshot = _snapshot(clock)
        self.actions: list[str] = []
        self.command_sequences: list[tuple[int, int]] = []
        self.open_session_generations: list[int] = []
        self.closed = False
        self.snapshot_sink(deepcopy(self.snapshot))

    def _publish(self) -> None:
        self.snapshot["snapshotGeneration"] += 1
        self.snapshot["providerPublicationSequence"] += 1
        now_ns = self._clock()
        self.snapshot["providerPublishedSteadyTimeNs"] = now_ns
        self.snapshot["transportOwnerProgressSequence"] += 1
        self.snapshot[
            "transportOwnerProgressReceivedSteadyTimeNs"
        ] = now_ns
        self.snapshot["sourceSteadyTimeNs"] = now_ns
        self.snapshot["receivedSteadyTimeNs"] = now_ns
        self.snapshot["freshUntilSteadyTimeNs"] = (
            now_ns + 10_000_000_000
        )
        self.snapshot_sink(deepcopy(self.snapshot))

    def open_session(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        self.open_session_generations.append(
            int(payload["session_generation"])
        )
        self.snapshot.update(
            {
                "authorityFresh": True,
                "authoritySessionId": payload["session_id"],
                "authoritySessionGeneration": payload["session_generation"],
                "commandSource": payload["command_source"],
                "authorityComponentEpoch": payload[
                    "authority_component_epoch"
                ],
                "authorityValidUntilSteadyTimeNs": (
                    self._clock() + int(payload["lease_duration_ns"])
                ),
            }
        )
        self._publish()
        return {
            "accepted": True,
            "reason": "opened",
            "snapshotGeneration": self.snapshot["snapshotGeneration"],
        }

    def close_session(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        del payload
        self.snapshot.update(
            {
                "authorityFresh": False,
                "authoritySessionId": "",
                "authoritySessionGeneration": 0,
                "commandSource": "",
                "authorityComponentEpoch": "",
                "authorityValidUntilSteadyTimeNs": 0,
                "motionAdmitted": False,
                "admissionReasonCodes": ["authority_session_stale_or_missing"],
            }
        )
        self._publish()
        return {
            "accepted": True,
            "reason": "closed",
            "snapshotGeneration": self.snapshot["snapshotGeneration"],
        }

    def submit_command(
        self,
        payload: Mapping[str, Any],
    ) -> dict[str, Any]:
        action = str(payload["action_id"])
        self.actions.append(action)
        self.command_sequences.append(
            (
                int(payload["session_generation"]),
                int(payload["request_sequence"]),
            )
        )
        if action == "robot.prepare_for_motion":
            self.snapshot.update(
                {
                    "robotEnabled": "true",
                    "vendorControlReady": "true",
                    "hardwareComponentActive": "true",
                    "trajectoryControllerActive": "true",
                }
            )
        elif action == "robot.make_noncommandable":
            self.snapshot.update(
                {
                    "robotEnabled": "false",
                    "vendorControlReady": "false",
                    "hardwareComponentActive": "false",
                    "trajectoryControllerActive": "false",
                }
            )
        else:
            transitions = {
                "robot.enable": ("robotEnabled", "true"),
                "robot.select_external_control": (
                    "vendorControlReady",
                    "true",
                ),
                "robot.start_control": ("vendorControlReady", "true"),
                "robot.activate_hardware": (
                    "hardwareComponentActive",
                    "true",
                ),
                "robot.activate_trajectory_controller": (
                    "trajectoryControllerActive",
                    "true",
                ),
                "robot.deactivate_trajectory_controller": (
                    "trajectoryControllerActive",
                    "false",
                ),
                "robot.deactivate_hardware": (
                    "hardwareComponentActive",
                    "false",
                ),
                "robot.stop_control": ("vendorControlReady", "false"),
                "robot.disable": ("robotEnabled", "false"),
            }
            fact, value = transitions[action]
            self.snapshot[fact] = value
        self.snapshot["controllerReady"] = (
            self.snapshot["hardwareComponentActive"] == "true"
            and self.snapshot["trajectoryControllerActive"] == "true"
        )
        self.snapshot["motionAdmitted"] = bool(
            self.snapshot["robotEnabled"] == "true"
            and self.snapshot["vendorControlReady"] == "true"
            and self.snapshot["controllerReady"]
        )
        self.snapshot["admissionReasonCodes"] = (
            [] if self.snapshot["motionAdmitted"] else ["not_motion_ready"]
        )
        self.snapshot["lastTerminalTransaction"] = {
            "generation": len(self.actions),
            "requestId": payload["request_id"],
            "requestSequence": payload["request_sequence"],
            "actionId": action,
            "commandSource": payload["command_source"],
            "authoritySessionGeneration": payload["session_generation"],
            "phase": "succeeded",
            "acceptedAtSteadyTimeNs": self._clock(),
            "deadlineSteadyTimeNs": self._clock() + 1_000_000_000,
            "terminalAtSteadyTimeNs": self._clock(),
            "resultReason": "postcondition_observed",
        }
        self._publish()
        return {
            "accepted": True,
            "disposition": "accepted",
            "reason": "accepted",
            "transactionGeneration": len(self.actions),
            "transactionPhase": "succeeded",
            "deadlineSteadyTimeNs": self._clock() + 1_000_000_000,
            "snapshotGeneration": self.snapshot["snapshotGeneration"],
        }

    def reconcile_command(
        self,
        payload: Mapping[str, Any],
    ) -> dict[str, Any]:
        del payload
        return {
            "accepted": True,
            "reason": "reconciled",
            "snapshotGeneration": self.snapshot["snapshotGeneration"],
        }

    def close(self) -> None:
        self.closed = True


def _config() -> RobotLifecycleConfig:
    return RobotLifecycleConfig(
        robot_id="lite6-a",
        provider_instance_id="lite6-a:provider:test",
        runtime_generation_id="runtime-1",
        cell_supervisor_component_epoch="supervisor-epoch-1",
        status_topic="/lifecycle/status",
        open_session_service="/lifecycle/open",
        close_session_service="/lifecycle/close",
        submit_command_service="/lifecycle/submit",
        reconcile_command_service="/lifecycle/reconcile",
    )


def _client(
    *,
    clock: Callable[[], int] = time.monotonic_ns,
    probe: _AuthorityProbe | None = None,
) -> tuple[RobotLifecycleClient, _ScriptedLifecycleTransport]:
    transport: _ScriptedLifecycleTransport | None = None
    authority_probe = probe or _AuthorityProbe(clock)

    def factory(
        _config: RobotLifecycleConfig,
        sink: Callable[[dict[str, Any]], None],
    ) -> _ScriptedLifecycleTransport:
        nonlocal transport
        transport = _ScriptedLifecycleTransport(sink, clock)
        return transport

    client = RobotLifecycleClient(
        _config(),
        authority_component_epoch=authority_probe.component_epoch,
        authority_health_probe=authority_probe,
        transport_factory=factory,
        clock_ns=clock,
    )
    assert transport is not None
    return client, transport


def test_exact_snapshot_redelivery_does_not_renew_receiver_freshness() -> None:
    clock = _ManualClock()
    client, transport = _client(clock=clock)
    initial = client.status()
    assert initial["providerDeliverySequence"] == 1
    assert initial["fresh"] is True

    clock.advance_s(1.0)
    transport.snapshot_sink(deepcopy(transport.snapshot))
    redelivered = client.status()
    assert redelivered["providerDeliverySequence"] == 1
    assert redelivered["providerHealthy"] is False
    assert redelivered["fresh"] is False

    transport.snapshot["sourceSteadyTimeNs"] = clock()
    transport.snapshot["receivedSteadyTimeNs"] = clock()
    transport.snapshot["freshUntilSteadyTimeNs"] = (
        clock() + 10_000_000_000
    )
    transport.snapshot["providerPublicationSequence"] += 1
    transport.snapshot["providerPublishedSteadyTimeNs"] = clock()
    transport.snapshot_sink(deepcopy(transport.snapshot))
    progressed = client.status()
    assert progressed["snapshotGeneration"] == initial["snapshotGeneration"]
    assert progressed["providerDeliverySequence"] == 2
    assert progressed["providerHealthy"] is True
    assert progressed["fresh"] is True


def test_provider_delivery_health_is_separate_from_observation_freshness() -> None:
    clock = _ManualClock()
    client, transport = _client(clock=clock)

    clock.advance_s(0.1)
    transport.snapshot["sourceSteadyTimeNs"] = clock()
    transport.snapshot["receivedSteadyTimeNs"] = clock()
    transport.snapshot["freshUntilSteadyTimeNs"] = clock() - 1
    transport.snapshot["providerPublicationSequence"] += 1
    transport.snapshot["providerPublishedSteadyTimeNs"] = clock()
    transport.snapshot_sink(deepcopy(transport.snapshot))

    status = client.status()
    assert status["providerHealthy"] is True
    assert status["fresh"] is False
    assert status["readiness"] == "waiting"
    assert status["canExecute"] is False
    assert status["userMessage"] == (
        "Robot observation is not fresh; motion remains fenced."
    )
    runtime = client.runtime_status()
    assert runtime["providerHealthy"] is True
    assert runtime["canExecute"] is False
    assert runtime["components"][0]["status"] == "waiting"


def test_provider_publication_progresses_while_robot_observation_is_unchanged(
) -> None:
    clock = _ManualClock()
    client, transport = _client(clock=clock)
    initial = client.status()
    initial_source_ns = initial["sourceSteadyTimeNs"]

    clock.advance_s(0.1)
    transport.snapshot["providerPublicationSequence"] += 1
    transport.snapshot["providerPublishedSteadyTimeNs"] = clock()
    transport.snapshot["transportOwnerProgressSequence"] += 1
    transport.snapshot[
        "transportOwnerProgressReceivedSteadyTimeNs"
    ] = clock()
    transport.snapshot_sink(deepcopy(transport.snapshot))

    progressed = client.status()
    assert progressed["providerDeliverySequence"] == 2
    assert progressed["providerHealthy"] is True
    assert progressed["sourceSteadyTimeNs"] == initial_source_ns


@pytest.mark.parametrize(
    ("field", "reason"),
    [
        (
            "observationGeneration",
            "robot_lifecycle_observation_generation_regressed",
        ),
        (
            "sourceSteadyTimeNs",
            "robot_lifecycle_observation_time_regressed",
        ),
    ],
)
def test_provider_progress_cannot_hide_robot_observation_regression(
    field: str,
    reason: str,
) -> None:
    clock = _ManualClock()
    client, transport = _client(clock=clock)
    initial = client.status()

    clock.advance_s(0.1)
    transport.snapshot["providerPublicationSequence"] += 1
    transport.snapshot["providerPublishedSteadyTimeNs"] = clock()
    transport.snapshot[field] = int(initial[field]) - 1
    transport.snapshot_sink(deepcopy(transport.snapshot))

    status = client.status()
    assert status["providerDeliverySequence"] == 1
    assert status["providerHealthy"] is False
    assert status["fresh"] is False
    assert status["userMessage"] == reason


def test_enable_and_disable_submit_one_provider_owned_target_each() -> None:
    client, transport = _client()

    assert client.status()["canExecute"] is False
    initial_runtime = client.runtime_status()
    assert initial_runtime["canExecute"] is False
    assert initial_runtime["components"][0]["status"] == "waiting"
    enabled = client.enable()
    assert enabled["success"] is True
    assert transport.actions == ["robot.prepare_for_motion"]
    assert client.status()["canExecute"] is True
    assert client.runtime_status()["components"][0]["status"] == "ready"

    # An already-ready request still reaches the provider as one idempotent
    # target transaction. Composer never infers or repeats vendor primitives.
    client.enable()
    assert transport.actions == [
        "robot.prepare_for_motion",
        "robot.prepare_for_motion",
    ]

    disabled = client.disable()
    assert disabled["success"] is True
    assert transport.actions[-1] == "robot.make_noncommandable"
    assert client.status()["canExecute"] is False


def test_near_expiry_session_renews_same_generation_and_sequence() -> None:
    client, transport = _client()

    client.enable()
    first_session_generation = transport.command_sequences[0][0]
    assert transport.command_sequences == [(first_session_generation, 1)]

    transport.snapshot["authorityValidUntilSteadyTimeNs"] = (
        time.monotonic_ns() + 1_000_000_000
    )
    transport._publish()

    client.disable()
    assert transport.open_session_generations == [
        first_session_generation,
        first_session_generation,
    ]
    assert transport.command_sequences[-1] == (first_session_generation, 2)


def test_headless_heartbeat_keeps_one_epoch_healthy_past_old_limit() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    session_generation = transport.command_sequences[0][0]
    initial_session_id = transport.snapshot["authoritySessionId"]

    # Seventy seconds of enabled-idle operation is modeled without sleeps.
    # The backend-owned tick advances health while no lifecycle mutation,
    # browser poll, or repeated Enable occurs.
    for _ in range(70):
        clock.advance_s(1.0)
        transport._publish()
        result = client.authority_heartbeat_tick()
        assert result["status"] in {"healthy", "renewed"}
        assert transport.snapshot["authoritySessionId"] == initial_session_id
        assert (
            transport.snapshot["authoritySessionGeneration"]
            == session_generation
        )
        assert client.status()["canExecute"] is True

    assert len(transport.open_session_generations) > 2
    assert set(transport.open_session_generations) == {session_generation}
    assert transport.actions == ["robot.prepare_for_motion"]
    health = client.status()["authorityHealth"]
    assert health["renewalCount"] > 1
    assert health["healthy"] is True


def test_headless_heartbeat_renews_during_bounded_observation_pause() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    entered = threading.Event()
    release = threading.Event()
    transport: _ScriptedLifecycleTransport | None = None
    command_result: dict[str, Any] | None = None
    command_error: Exception | None = None

    class _BlockingSubmitTransport(_ScriptedLifecycleTransport):
        def submit_command(
            self,
            payload: Mapping[str, Any],
        ) -> dict[str, Any]:
            entered.set()
            if not release.wait(timeout=2.0):
                raise RuntimeError("test_command_release_timeout")
            return super().submit_command(payload)

    def factory(
        _config: RobotLifecycleConfig,
        sink: Callable[[dict[str, Any]], None],
    ) -> _ScriptedLifecycleTransport:
        nonlocal transport
        transport = _BlockingSubmitTransport(sink, clock)
        return transport

    client = RobotLifecycleClient(
        _config(),
        authority_component_epoch=probe.component_epoch,
        authority_health_probe=probe,
        transport_factory=factory,
        clock_ns=clock,
    )
    assert transport is not None

    def enable() -> None:
        nonlocal command_error, command_result
        try:
            command_result = client.enable()
        except Exception as exc:  # pragma: no cover - asserted below
            command_error = exc

    command = threading.Thread(target=enable)
    command.start()
    assert entered.wait(timeout=1.0)
    session_generation = transport.open_session_generations[0]
    session_id = transport.snapshot["authoritySessionId"]
    initial_open_count = len(transport.open_session_generations)

    # Model an admitted lifecycle primitive: the owner and provider callbacks
    # keep progressing while the serialized vendor call temporarily prevents
    # qualified robot telemetry from advancing.
    clock.advance_s(3.0)
    transport.snapshot.update(
        {
            "observationFresh": False,
            "freshUntilSteadyTimeNs": clock() - 1,
            "providerPublicationSequence": (
                transport.snapshot["providerPublicationSequence"] + 1
            ),
            "providerPublishedSteadyTimeNs": clock(),
            "transportOwnerProgressSequence": (
                transport.snapshot["transportOwnerProgressSequence"] + 1
            ),
            "transportOwnerProgressReceivedSteadyTimeNs": clock(),
            "transportOwnerProgressHealthy": True,
            "transportOwnerProgressReason": "",
        }
    )
    transport.snapshot_sink(deepcopy(transport.snapshot))

    paused = client.status()
    assert paused["providerHealthy"] is True
    assert paused["transportOwnerProgressHealthy"] is True
    assert paused["fresh"] is False
    assert paused["canExecute"] is False

    result = client.authority_heartbeat_tick()
    assert result == {"status": "renewed", "renewed": True}
    assert len(transport.open_session_generations) == initial_open_count + 1
    assert set(transport.open_session_generations) == {session_generation}
    assert transport.snapshot["authoritySessionId"] == session_id
    assert client.status()["authorityHealth"]["sessionEstablished"] is True
    # Authority liveness never substitutes for qualified robot telemetry.
    assert client.status()["fresh"] is False
    assert client.status()["canExecute"] is False

    release.set()
    command.join(timeout=2.0)
    assert not command.is_alive()
    assert command_error is None
    assert command_result is not None
    assert command_result["success"] is True


def test_headless_heartbeat_revokes_when_owner_progress_is_unhealthy() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    initial_open_count = len(transport.open_session_generations)
    clock.advance_s(3.0)
    transport.snapshot.update(
        {
            "providerPublicationSequence": (
                transport.snapshot["providerPublicationSequence"] + 1
            ),
            "providerPublishedSteadyTimeNs": clock(),
            "transportOwnerProgressSequence": (
                transport.snapshot["transportOwnerProgressSequence"] + 1
            ),
            "transportOwnerProgressReceivedSteadyTimeNs": clock(),
            "transportOwnerProgressHealthy": False,
            "transportOwnerProgressReason": (
                "transport_owner_command_deadline_expired"
            ),
        }
    )
    transport.snapshot_sink(deepcopy(transport.snapshot))

    assert client.status()["providerHealthy"] is True
    result = client.authority_heartbeat_tick()
    assert result == {
        "status": "transport_owner_progress_unhealthy",
        "reason": "transport_owner_command_deadline_expired",
        "renewed": False,
    }
    assert len(transport.open_session_generations) == initial_open_count
    assert transport.snapshot["authorityFresh"] is False
    assert client.status()["authorityHealth"]["sessionEstablished"] is False
    assert client.status()["canExecute"] is False


def test_headless_heartbeat_retains_epoch_for_one_bounded_owner_delivery_gap() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    initial_generation = transport.snapshot["authoritySessionGeneration"]
    initial_open_count = len(transport.open_session_generations)
    clock.advance_s(0.6)
    transport.snapshot.update(
        {
            "providerPublicationSequence": (
                transport.snapshot["providerPublicationSequence"] + 1
            ),
            "providerPublishedSteadyTimeNs": clock(),
            "transportOwnerProgressHealthy": False,
            "transportOwnerProgressReason": "xarm_owner_publication_stale",
            "transportOwnerProgressReceivedSteadyTimeNs": (
                clock() - 600_000_000
            ),
            "motionAdmitted": False,
        }
    )
    transport.snapshot_sink(deepcopy(transport.snapshot))

    pending = client.authority_heartbeat_tick()

    assert pending == {
        "status": "transport_owner_progress_pending",
        "reason": "xarm_owner_publication_stale",
        "progressAgeNs": 600_000_000,
        "renewed": False,
    }
    assert client.status()["authorityHealth"]["sessionEstablished"] is True
    assert transport.snapshot["authoritySessionGeneration"] == initial_generation
    assert len(transport.open_session_generations) == initial_open_count

    clock.advance_s(0.1)
    transport.snapshot.update(
        {
            "providerPublicationSequence": (
                transport.snapshot["providerPublicationSequence"] + 1
            ),
            "providerPublishedSteadyTimeNs": clock(),
            "transportOwnerProgressSequence": (
                transport.snapshot["transportOwnerProgressSequence"] + 1
            ),
            "transportOwnerProgressHealthy": True,
            "transportOwnerProgressReason": "",
            "transportOwnerProgressReceivedSteadyTimeNs": clock(),
        }
    )
    transport.snapshot_sink(deepcopy(transport.snapshot))

    assert client.authority_heartbeat_tick() == {
        "status": "healthy",
        "renewed": False,
    }
    assert transport.snapshot["authoritySessionGeneration"] == initial_generation
    assert len(transport.open_session_generations) == initial_open_count
    assert client.status()["authorityHealth"]["sessionEstablished"] is True


def test_headless_heartbeat_revokes_persistently_stale_owner_delivery() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    last_owner_receive_ns = clock()
    clock.advance_s(2.1)
    transport.snapshot.update(
        {
            "providerPublicationSequence": (
                transport.snapshot["providerPublicationSequence"] + 1
            ),
            "providerPublishedSteadyTimeNs": clock(),
            "transportOwnerProgressHealthy": False,
            "transportOwnerProgressReason": "xarm_owner_publication_stale",
            "transportOwnerProgressReceivedSteadyTimeNs": last_owner_receive_ns,
            "motionAdmitted": False,
        }
    )
    transport.snapshot_sink(deepcopy(transport.snapshot))

    failed = client.authority_heartbeat_tick()

    assert failed == {
        "status": "transport_owner_progress_unhealthy",
        "reason": "xarm_owner_publication_stale",
        "renewed": False,
    }
    assert client.status()["authorityHealth"]["sessionEstablished"] is False
    assert transport.snapshot["authorityFresh"] is False


def test_status_does_not_wait_behind_blocked_session_transport() -> None:
    entered = threading.Event()
    release = threading.Event()
    transport: _ScriptedLifecycleTransport | None = None
    command_error: Exception | None = None
    probe = _AuthorityProbe(time.monotonic_ns)

    class _BlockingOpenTransport(_ScriptedLifecycleTransport):
        def open_session(
            self,
            payload: Mapping[str, Any],
        ) -> dict[str, Any]:
            entered.set()
            if not release.wait(timeout=2.0):
                raise RuntimeError("test_session_release_timeout")
            return super().open_session(payload)

    def factory(
        _config: RobotLifecycleConfig,
        sink: Callable[[dict[str, Any]], None],
    ) -> _ScriptedLifecycleTransport:
        nonlocal transport
        transport = _BlockingOpenTransport(sink)
        return transport

    client = RobotLifecycleClient(
        _config(),
        authority_component_epoch=probe.component_epoch,
        authority_health_probe=probe,
        transport_factory=factory,
    )

    def enable() -> None:
        nonlocal command_error
        try:
            client.enable()
        except Exception as exc:  # pragma: no cover - asserted below
            command_error = exc

    command = threading.Thread(target=enable)
    command.start()
    assert entered.wait(timeout=1.0)
    started = time.monotonic()
    status = client.status()
    runtime = client.runtime_status()
    elapsed = time.monotonic() - started

    assert elapsed < 0.1
    assert status["providerHealthy"] is True
    assert status["authorityHealth"]["sessionEstablished"] is False
    assert runtime["providerHealthy"] is True
    release.set()
    command.join(timeout=2.0)
    assert not command.is_alive()
    assert command_error is None
    assert transport is not None
    assert client.status()["authorityHealth"]["sessionEstablished"] is True


def test_health_loss_revokes_and_recovery_does_not_auto_rearm() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    first_generation = transport.command_sequences[0][0]
    probe.healthy = False
    probe.reason = "composer_event_loop_unhealthy"
    clock.advance_s(1.0)

    failed = client.authority_heartbeat_tick()
    assert failed == {
        "status": "unhealthy",
        "reason": "composer_event_loop_unhealthy",
        "renewed": False,
    }
    assert transport.snapshot["authorityFresh"] is False
    assert client.status()["canExecute"] is False

    # Health returning makes the adapter observable again, but it cannot open
    # authority or restore motion without an explicit lifecycle request.
    probe.healthy = True
    probe.reason = ""
    clock.advance_s(1.0)
    transport._publish()
    assert client.authority_heartbeat_tick()["status"] == "inactive"
    assert transport.snapshot["authorityFresh"] is False

    client.enable()
    assert transport.command_sequences[-1] == (first_generation + 1, 1)
    assert client.status()["canExecute"] is True


def test_expired_session_cannot_be_renewed_or_rearmed_by_heartbeat() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)

    client.enable()
    first_generation = transport.command_sequences[0][0]
    assert transport.open_session_generations == [first_generation]

    # Model the provider retaining identity for diagnostics after its lease
    # deadline while closing admission. That retained identity is not a
    # renewable authority session.
    clock.advance_s(6.0)
    transport.snapshot.update(
        {
            "authorityFresh": False,
            "motionAdmitted": False,
            "admissionReasonCodes": ["authority_session_stale_or_missing"],
        }
    )
    transport._publish()

    result = client.authority_heartbeat_tick()
    assert result == {
        "status": "renewal_failed",
        "reason": "authority_session_lost",
        "renewed": False,
    }
    assert transport.open_session_generations == [first_generation]
    assert client.status()["canExecute"] is False

    # Explicit operator intent opens a fresh generation; health recovery alone
    # never revives the expired replay/admission domain.
    client.enable()
    assert transport.open_session_generations[-1] == first_generation + 1
    assert transport.command_sequences[-1] == (first_generation + 1, 1)


def test_replayed_health_evidence_revokes_current_authority() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)
    client.enable()

    # Reusing the last accepted sequence is stale/replayed evidence.
    probe.advance_sequence = False
    result = client.authority_heartbeat_tick()

    assert result["status"] == "unhealthy"
    assert result["reason"] == "authority_health_sequence_not_increasing"
    assert transport.snapshot["authorityFresh"] is False


def test_wrong_epoch_evidence_cannot_poison_expected_epoch_sequence() -> None:
    clock = _ManualClock()
    probe = _AuthorityProbe(clock)
    client, transport = _client(clock=clock, probe=probe)
    client.enable()

    probe.component_epoch = "composer-control:foreign-epoch"
    probe.sequence = 999
    clock.advance_s(1.0)
    rejected = client.authority_heartbeat_tick()
    assert rejected["status"] == "unhealthy"
    assert rejected["reason"] == "authority_component_epoch_mismatch"
    assert transport.snapshot["authorityFresh"] is False

    # A valid sample continues from the last accepted sequence in the expected
    # epoch; the foreign value was never admitted into its replay domain.
    probe.component_epoch = "composer-control:test-epoch"
    probe.sequence = 1
    clock.advance_s(1.0)
    transport._publish()
    assert client.authority_heartbeat_tick()["status"] == "inactive"
    assert client.status()["authorityHealth"]["sequence"] == 2


def test_critical_progress_probe_overrun_never_opens_authority() -> None:
    clock = _ManualClock()
    sequence = 0

    def slow_probe() -> AuthorityHealthEvidence:
        nonlocal sequence
        sequence += 1
        clock.advance_s(3.0)
        return AuthorityHealthEvidence(
            component_epoch="composer-control:test-epoch",
            sequence=sequence,
            observed_at_ns=clock(),
            healthy=True,
        )

    transport: _ScriptedLifecycleTransport | None = None

    def factory(
        _config: RobotLifecycleConfig,
        sink: Callable[[dict[str, Any]], None],
    ) -> _ScriptedLifecycleTransport:
        nonlocal transport
        transport = _ScriptedLifecycleTransport(sink, clock)
        return transport

    client = RobotLifecycleClient(
        _config(),
        authority_component_epoch="composer-control:test-epoch",
        authority_health_probe=slow_probe,
        transport_factory=factory,
        clock_ns=clock,
    )

    with pytest.raises(
        RuntimeError,
        match="authority_critical_progress_probe_overrun",
    ):
        client.enable()
    assert transport is not None
    assert transport.open_session_generations == []


def test_request_sequence_restarts_after_session_identity_loss() -> None:
    client, transport = _client()
    client.enable()
    first_session_generation = transport.command_sequences[0][0]

    transport.snapshot.update(
        {
            "authorityFresh": False,
            "authoritySessionId": "",
            "authoritySessionGeneration": 0,
            "authorityValidUntilSteadyTimeNs": 0,
        }
    )
    transport._publish()

    client.disable()
    assert transport.command_sequences[-1] == (
        first_session_generation + 1,
        1,
    )


def test_disable_fails_closed_without_stationary_evidence() -> None:
    client, transport = _client()
    client.enable()
    transport.snapshot["stationary"] = "unknown"
    next(
        capability
        for capability in transport.snapshot["capabilities"]
        if capability["actionId"] == "robot.make_noncommandable"
    ).update(
        {
            "available": False,
            "availabilityReason": "xarm_target_precondition_not_satisfied",
        }
    )
    transport._publish()

    with pytest.raises(
        RuntimeError,
        match="xarm_target_precondition_not_satisfied",
    ):
        client.disable()

    assert transport.snapshot["trajectoryControllerActive"] == "true"
    assert client.status()["canExecute"] is True


def test_config_rejects_a_fabricated_assurance_level() -> None:
    raw = {
        "schema": "manyforge.robot_lifecycle.ros_provider.v1",
        "robot_id": "lite6-a",
        "provider_instance_id": "provider",
        "status_topic": "/status",
        "open_session_service": "/open",
        "close_session_service": "/close",
        "submit_command_service": "/submit",
        "reconcile_command_service": "/reconcile",
        "caller_assurance": "trusted_localhost",
        "maximum_authority_lease_ms": 300_000,
    }

    with pytest.raises(ValueError, match="caller assurance"):
        RobotLifecycleConfig.from_mapping(raw, fallback_robot_id="lite6-a")


def test_config_accepts_bounded_provider_material_adapter_keys() -> None:
    raw = {
        "schema": "manyforge.robot_lifecycle.ros_provider.v1",
        "robot_id": "lite6-a",
        "provider_instance_id": "provider",
        "runtime_generation_id": "runtime-1",
        "cell_supervisor_component_epoch": "cell-epoch-1",
        "status_topic": "/status",
        "open_session_service": "/open",
        "close_session_service": "/close",
        "submit_command_service": "/submit",
        "reconcile_command_service": "/reconcile",
        "caller_assurance": "unverified",
        "maximum_authority_lease_ms": 300_000,
        "material_adapter_status_keys": [
            "vendor_mode",
            "vendor_state",
        ],
    }

    config = RobotLifecycleConfig.from_mapping(
        raw,
        fallback_robot_id="lite6-a",
    )

    assert config.material_adapter_status_keys == (
        "vendor_mode",
        "vendor_state",
    )
