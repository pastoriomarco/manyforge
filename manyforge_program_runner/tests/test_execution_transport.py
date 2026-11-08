"""Program-owner direct execution transport boundary."""

from __future__ import annotations

from pathlib import Path
from copy import deepcopy
import importlib.util
import sys
from types import ModuleType
from typing import Any

import pytest
from pydantic import ValidationError

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.models import (
    OperationControlRequest,
    OperationIntentRequest,
)
from manyforge_program_runner.execution_transport import (
    ProgramRunnerExecutionTransport,
)


def _stop_body() -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "program-stop.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "manyforge_behavior",
            "correlation_id": "program-run.1",
        },
        "program_run_id": "program-run.1",
        "reason": "operator Stop",
    }


@pytest.mark.parametrize("endpoint_owner", ["arm", "gripper"])
def test_behavior_motion_endpoint_crosses_the_runner_strict_transport(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, endpoint_owner: str
) -> None:
    # Reuse the owner tests' captured context/transport; the operation itself
    # is emitted by the real production port, not assembled for this parser.
    source = (
        Path(__file__).resolve().parents[2]
        / "manyforge_behavior/tests/test_production_ports.py"
    )
    package_name = "_runner_endpoint_production_port_fixture"
    package = ModuleType(package_name)
    package.__path__ = [str(source.parent)]
    monkeypatch.setitem(sys.modules, package_name, package)
    name = f"{package_name}.test_production_ports"
    spec = importlib.util.spec_from_file_location(name, source)
    assert spec is not None and spec.loader is not None
    fixture = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, name, fixture)
    spec.loader.exec_module(fixture)
    recorder = fixture._Transport()
    port = fixture.NativeKernelProgramPort(fixture.KernelClient(recorder))
    port.bind(fixture._context())
    parameters = dict(fixture._motion_inputs().parameters)
    selected_target = (
        {"component_id": "robot.1", "command_target_id": "arm.command"}
        if endpoint_owner == "arm"
        else {"component_id": "tool.1", "command_target_id": "gripper.command"}
    )
    expected_endpoint = {"kind": "default_of_target", "target": selected_target}
    parameters["tcp_endpoint"] = expected_endpoint
    port.operations.start(
        "motion.move_to_pose",
        fixture._identity("runner-endpoint"),
        fixture.OwnerNodeInputs(parameters),
    )
    operation = recorder.submissions[0]
    commands: list[dict[str, Any]] = []

    def exchange(_client, command, _socket_path):
        commands.append(command)
        return {
            "ok": True,
            "command": "submit",
            "accepted": {
                "request_id": command["operation"]["request_context"]["request_id"]
            },
            "active_tcp": [{"component_id": "robot.1", "entry": None,
                            "owner_epoch": "owner.1", "registration_generation": 1,
                            "sequence": 0}],
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    result = transport.submit(operation)
    assert result["ok"] is True
    assert result["active_tcp"][0]["component_id"] == "robot.1"
    sent = commands[0]["operation"]["participants"][0]
    assert sent["payload"]["tcp_endpoint"] == expected_endpoint
    assert "tcp_frame_role_id" not in sent["payload"]
    assert sent["execution_scope"] == operation["participants"][0]["execution_scope"]
    legacy = deepcopy(operation)
    payload = legacy["participants"][0]["payload"]
    payload["tcp_endpoint"] = {"kind": "default_of_role", "role": endpoint_owner}
    with pytest.raises(HeadlessApiError) as refused:
        transport.submit(legacy)
    assert refused.value.code == "execution_command_invalid"
    assert len(commands) == 1  # refusal occurs before the socket exchange


@pytest.mark.parametrize(
    "failure",
    ["command", "missing_groups", "empty_groups", "release_command"],
)
def test_selection_results_must_have_their_kind_and_shape(
    monkeypatch, tmp_path, failure
):
    from manyforge_behavior.kernel_client import KernelClient, KernelClientResponseError

    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    kernel = KernelClient(transport)
    request = {"group_ids": ["arm", "grippers"]}
    response = {
        "ok": True,
        "command": "stop_program_selection",
        "accepted": {
            "state": {},
            "group_ids": ["arm", "grippers"],
            "replay_status": "accepted_new",
        },
    }
    monkeypatch.setattr(transport, "_exchange_on", lambda *_: response)
    assert kernel.stop_program_selection(request)["group_ids"] == ["arm", "grippers"]
    if failure == "command":
        response["command"] = "unrelated_command"
    elif failure == "missing_groups":
        del request["group_ids"]
        del response["accepted"]["group_ids"]
    elif failure == "empty_groups":
        request["group_ids"] = []
        response["accepted"]["group_ids"] = []
    else:
        response = {
            "ok": True,
            "command": "release_program_selection",
            "scope": {"selection_revision": 3},
        }
        assert kernel.release_program_selection(request, 2)["selection_revision"] == 3
        response["command"] = "unrelated_command"
    with pytest.raises(KernelClientResponseError):
        if failure == "release_command":
            kernel.release_program_selection(request, 2)
        else:
            kernel.stop_program_selection(request)


@pytest.mark.parametrize(
    "reason",
    [
        "program_selection_revision_stale",
        "program_children_unsettled",
        "program_run_admission_in_flight",
        "unexpected_exception",
    ],
)
def test_selection_release_distinguishes_no_effect_refusal_from_unknown(
    monkeypatch, tmp_path, reason
):
    from manyforge_behavior.orchestration_ports import (
        ProgramSelectionReleaseRejectedError,
    )

    monkeypatch.setattr(
        ExecutionCommandClient,
        "_exchange_on",
        lambda *_: {
            "ok": False,
            "error": {
                "code": "invalid_state_transition",
                "message": "release refused",
                "details": {"reason": reason},
            },
        },
    )
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    expected = (
        HeadlessApiError
        if reason == "unexpected_exception"
        else ProgramSelectionReleaseRejectedError
    )
    with pytest.raises(expected, match="release refused"):
        transport.release_program_selection({"group_ids": ["all"]}, 2)


def test_transport_reuses_the_owner_client_with_internal_attribution(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    commands: list[dict[str, Any]] = []

    def exchange(
        _client: ExecutionCommandClient,
        command: dict[str, Any],
        _socket_path: Path,
    ) -> dict[str, Any]:
        commands.append(command)
        kind = command["command"]
        if kind == "identity":
            return {"api_version": "test", "command": kind, "ok": True}
        if kind == "stop_program_run":
            return {
                "api_version": "test",
                "accepted": {"request_id": "program-stop.1"},
                "command": kind,
                "ok": True,
            }
        if kind == "release_program_run_stop_fence":
            return {
                "api_version": "test",
                "command": kind,
                "ok": True,
            }
        assert kind == "read_stop"
        return {
            "api_version": "test",
            "command": kind,
            "ok": True,
            "stop_status": {
                "request_id": "program-stop.1",
                "schema_version": "manyforge.execution_stop_status.v1",
                "state": "succeeded",
                "stop_generation": 1,
                "stop_id": "stop.1",
            },
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=3.5)

    assert transport.identity()["ok"] is True
    assert transport.stop_program_run(_stop_body())["ok"] is True
    assert transport.stop_status("stop.1")["stop_status"]["state"] == "succeeded"
    assert (
        transport.release_program_run_stop_fence("program-run.1", "stop.1", 1)["ok"]
        is True
    )
    assert commands[1]["attribution"] == {
        "transport": "internal",
        "assurance": "unverified",
        "principal_id": None,
    }
    assert commands[1]["request"] == _stop_body()
    assert commands[3] == {
        "api_version": "manyforge.execution_command.v3",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "release_program_run_stop_fence",
        "program_run_id": "program-run.1",
        "stop_generation": 1,
        "stop_id": "stop.1",
    }


def test_transport_preserves_validation_and_owner_error_codes(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    def unavailable(
        _client: ExecutionCommandClient,
        command: dict[str, Any],
        _socket_path: Path,
    ) -> dict[str, Any]:
        return {
            "api_version": "test",
            "command": command["command"],
            "error": {
                "code": "execution_owner_unavailable",
                "message": "owner is restarting",
                "details": {"reason": "owner_epoch_mismatch"},
            },
            "ok": False,
        }

    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=3.5)
    invalid = _stop_body()
    invalid["unexpected"] = True
    with pytest.raises(HeadlessApiError) as validation:
        transport.stop_program_run(invalid)
    assert validation.value.code == "execution_command_invalid"

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", unavailable)
    with pytest.raises(HeadlessApiError) as owner:
        transport.stop_program_run(_stop_body())
    assert owner.value.code == "execution_owner_unavailable"
    assert owner.value.details == {"reason": "owner_epoch_mismatch"}


@pytest.mark.parametrize("details", [None, [], "owner_epoch_mismatch"])
def test_transport_rejects_malformed_owner_error_details(details: Any) -> None:
    with pytest.raises(HeadlessApiError) as invalid:
        ExecutionCommandClient.require_success(
            {
                "ok": False,
                "error": {
                    "code": "rejected",
                    "message": "Rejected",
                    "details": details,
                },
            }
        )
    assert invalid.value.code == "execution_command_response_invalid"


def test_capture_registration_sends_identities_and_checks_the_returned_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    capture = {
        "schema_version": "manyforge.program_run_execution_capture.v1",
        "request_owner_epoch": "kernel.1",
        "program_runner_owner_epoch": "runner.1",
        "claim_generation": 4,
        "program_run_id": "run.1",
        "program_id": "pick",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "retained_bundle_id": "sha256:" + "c" * 64,
        "graph_revision": "sha256:" + "d" * 64,
    }
    commands = []
    response = {
        "ok": True,
        "command": "register_program_run_capture",
        "program_run_id": "run.1",
        "claim_generation": 4,
    }

    def exchange(_client, command, _socket_path):
        commands.append(command)
        return deepcopy(response)

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    assert transport.register_program_run_capture(capture) == response
    assert commands == [
        {
            "api_version": "manyforge.execution_command.v3",
            "command": "register_program_run_capture",
            "capture": capture,
        }
    ]
    response["claim_generation"] = 5  # The echo is not compared.
    assert transport.register_program_run_capture(capture) == response
    response["command"] = "register_io_watch"
    with pytest.raises(HeadlessApiError) as invalid:
        transport.register_program_run_capture(capture)
    assert invalid.value.code == "execution_command_response_invalid"


def test_control_origin_is_forwarded_privately_and_forbidden_publicly(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    request = {
        "request_context": _stop_body()["request_context"],
        "operation_id": "operation.1",
        "expected_operation_revision": 2,
        "reason": "Program Reset",
        "execution_scope": {
            "node_instance_id": "reset",
            "node_attempt_id": "reset.1",
        },
    }
    commands = []

    def exchange(_client, command, _socket_path):
        commands.append(command)
        return {
            "ok": True,
            "command": "reset",
            "accepted": {
                "request_id": request["request_context"]["request_id"],
                "operation_id": "operation.1",
                "control": "reset",
                "accepted_against_revision": 2,
            },
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    assert transport.reset("operation.1", request)["ok"] is True
    assert commands[0]["request"]["execution_scope"] == request["execution_scope"]
    with pytest.raises(ValidationError, match="execution_scope"):
        OperationControlRequest.model_validate(request)


def test_program_origin_is_required_privately_and_forbidden_publicly(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    operation = {
        "request_context": _stop_body()["request_context"],
        "program_id": "pick",
        "program_revision": "revision",
        "program_binding_id": "binding",
        "program_run_id": "program-run.1",
        "expected_graph_revision": "graph",
        "expected_retained_bundle_id": "bundle",
        "expected_scene_revision": None,
        "coordination_policy": "independent",
        "participants": [
            {
                "target": {"component_id": "tool", "command_target_id": "gripper"},
                "required_capability_id": "tool.gripper.command",
                "action_span": None,
                "execution_scope": {
                    "node_instance_id": "close",
                    "node_attempt_id": "close.1",
                },
                "payload": {
                    "kind": "capability_command",
                    "command_schema_id": "gripper.position",
                    "command_schema_revision": "revision",
                    "completion_timeout_s": 1,
                    "caller_options": [],
                    "command_value": {
                        "kind": "gripper_position",
                        "position": {"kind": "angular_rad", "value_rad": 0.3},
                        "max_effort": None,
                    },
                },
            }
        ],
    }
    commands = []

    def exchange(_client, command, _socket_path):
        commands.append(command)
        return {
            "ok": True,
            "command": "submit",
            "accepted": {
                "request_id": operation["request_context"]["request_id"],
            },
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    assert transport.submit(operation)["ok"] is True
    assert commands[0]["operation"]["participants"][0]["execution_scope"] == (
        operation["participants"][0]["execution_scope"]
    )
    with pytest.raises(ValidationError, match="execution_scope"):
        OperationIntentRequest.model_validate(operation)
    for scope in (None, {}, {"node_instance_id": "close", "node_attempt_id": ""}):
        malformed = deepcopy(operation)
        if scope is None:
            malformed["participants"][0].pop("execution_scope")
        else:
            malformed["participants"][0]["execution_scope"] = scope
        with pytest.raises(HeadlessApiError, match="invalid"):
            transport.submit(malformed)
    assert len(commands) == 1


def _position_capability_response():
    digest = "sha256:" + "a" * 64
    return {"api_version": "manyforge.execution_command_response.v4", "command": "reset_robot_position_capability",
            "ok": True, "reset_robot_position": {"available": True, "reason": None},
            "identity": {"schema_version": "manyforge.execution_identity.v2", "owner_epoch": "kernel.1",
                         "active_publication_generation": 3, "retained_bundle_id": digest, "graph_revision": digest,
                         "cell_id": "cell.1", "control_management_mode": "component_lifecycle"}}


def _position_request():
    digest = "sha256:" + "a" * 64
    return {"authorization": {"program_runner_owner_epoch": "runner.1", "cycle_generation": 1,
            "request_owner_epoch": "caller.1", "request_id": "reset.1", "request_payload_digest": digest,
            "program_run_id": None, "cycle_claim_generation": None, "terminal_or_settlement_reference": None,
            "reset_claim_generation": 7}, "expected_deployment_identity": {"owner_epoch": "kernel.1",
            "active_publication_generation": 3, "retained_bundle_id": digest, "graph_revision": digest}}


@pytest.mark.parametrize("mutation", ["extra", "wrong_kind", "boolean_int", "bad_reason", "missing_identity"])
def test_position_capability_response_is_strict_and_pins_deployment(monkeypatch, tmp_path, mutation):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    response = _position_capability_response()
    packets = []
    monkeypatch.setattr(transport, "_exchange", lambda packet: packets.append(packet) or response)
    assert transport.reset_robot_position_capability() == response
    assert packets == [{"api_version": "manyforge.execution_command.v3", "command": "reset_robot_position_capability"}]
    if mutation == "extra": response["arbitrary"] = True
    elif mutation == "wrong_kind": response["command"] = "identity"
    elif mutation == "boolean_int": response["ok"] = 1
    elif mutation == "bad_reason": response["reset_robot_position"]["reason"] = "unsupported"
    else: del response["identity"]["owner_epoch"]
    with pytest.raises(HeadlessApiError, match="response is invalid"):
        transport.reset_robot_position_capability()


@pytest.mark.parametrize("mutation", ["joints", "extra_authority", "claim_union", "reply_kind", "reply_extra", "reply_bool"])
def test_position_effect_rejects_arbitrary_targets_and_mismatched_success(monkeypatch, tmp_path, mutation):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    packets = []
    response = {"api_version": "manyforge.execution_command_response.v4", "command": "reset_robot_position", "ok": True}
    monkeypatch.setattr(transport, "_exchange", lambda packet: packets.append(packet) or response)
    request = _position_request()
    transport.reset_robot_position(request)
    assert packets[-1] == {"api_version": "manyforge.execution_command.v3", "command": "reset_robot_position", **request}
    if mutation == "joints": request["initial_joint_positions_rad"] = [1.0]
    elif mutation == "extra_authority": request["authorization"]["reload_initial_scene"] = True
    elif mutation == "claim_union": request["authorization"]["cycle_claim_generation"] = 9
    elif mutation == "reply_kind": response["command"] = "reset"
    elif mutation == "reply_bool": response["ok"] = 1
    else: response["position"] = "assumed"
    with pytest.raises(HeadlessApiError):
        transport.reset_robot_position(request)
    assert len(packets) == (1 if mutation in {"joints", "extra_authority", "claim_union"} else 2)


@pytest.mark.parametrize("command", ["read_active_tcp", "reset_active_tcp"])
def test_active_tcp_commands_keep_rows_and_internal_reset_attribution(monkeypatch, tmp_path, command):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    commands = []
    rows = [{"component_id": "robot", "entry": {
                "target": {"component_id": "tool", "command_target_id": "gripper"},
                "frame": {"component_id": "tool", "frame_id": "tcp"}},
             "owner_epoch": "owner.1", "registration_generation": 2, "sequence": 3}]
    response = {"api_version": "manyforge.execution_command_response.v4", "command": command,
                "ok": True, "robots": rows}
    def exchange(_client, request, _socket_path):
        commands.append(request)
        return response
    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    assert getattr(transport, command)()["robots"] == rows
    assert commands[0]["command"] == command
    if command == "reset_active_tcp":
        assert commands[0]["attribution"] == {"transport": "internal", "assurance": "unverified", "principal_id": None}
    else:
        assert "attribution" not in commands[0]
    response["robots"][0]["registration_generation"] = True
    with pytest.raises(HeadlessApiError, match="Active TCP response is invalid"):
        getattr(transport, command)()


def test_runner_set_tcp_participant_strictly_round_trips():
    from manyforge_program_runner.execution_transport import _ProgramParticipant
    body = {"target": {"component_id": "robot", "command_target_id": "arm"},
            "required_capability_id": "execution.trajectory.execute", "action_span": None,
            "execution_scope": {"node_instance_id": "set", "node_attempt_id": "attempt.1"},
            "payload": {"kind": "set_tcp", "tcp": {
                "kind": "default_of_target",
                "target": {"component_id": "tool", "command_target_id": "gripper"}}}}
    assert _ProgramParticipant.model_validate(body).model_dump(mode="json") == body
    body["payload"]["tcp"] = {"target_role_id": "tool", "frame_role_id": "tcp"}
    with pytest.raises(ValidationError):
        _ProgramParticipant.model_validate(body)


def test_admitted_immediate_failure_preserves_confirmed_active_tcp(monkeypatch, tmp_path):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    rows = [{"component_id": "robot", "entry": None, "owner_epoch": "owner.1",
             "registration_generation": 1, "sequence": 2}]
    response = {"ok": False, "error": {"code": "tcp_offset_not_applied", "message": "failed",
        "producer": "kernel", "details": {"immediate_step": "admitted", "request_id": "request.1", "operation_id": "operation.1"}},
        "active_tcp": rows}
    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", lambda *_: response)
    with pytest.raises(HeadlessApiError) as caught:
        transport._exchange({"command": "submit"})
    assert caught.value.code == "tcp_offset_not_applied"
    assert caught.value.producer == "kernel"
    assert caught.value.details["request_id"] == "request.1"
    assert caught.value.active_tcp == rows
    del response["active_tcp"]
    with pytest.raises(HeadlessApiError) as invalid:
        transport._exchange({"command": "submit"})
    assert invalid.value.code == "execution_command_response_invalid"


@pytest.mark.parametrize("outcome", ["succeeded", "failed", "unknown", "refused"])
def test_program_output_write_is_the_manual_set_io_output_command(monkeypatch, tmp_path, outcome):
    """One write path. The Program sends the same command as a
    manual write, once; refusals and failed/unknown results keep their code."""

    from manyforge_behavior.kernel_client import IoOutputWriteFailedError, KernelClient

    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    kernel = KernelClient(transport)
    packets: list[dict[str, Any]] = []
    if outcome == "refused":
        response = {"ok": False, "error": {"code": "cell_latched", "message": "latched"}}
    else:
        result: dict[str, Any] = {"outcome": outcome}
        if outcome != "succeeded":
            result["error"] = {"code": "io_output_acceptance_unknown", "message": "no reply"}
        response = {"ok": True, "command": "set_io_output", "result": result}
    monkeypatch.setattr(
        ExecutionCommandClient, "_exchange_on",
        lambda self, packet, socket: packets.append(packet) or deepcopy(response),
    )
    if outcome == "succeeded":
        assert kernel.set_io_output("io.1", "do_0", True) == {"outcome": "succeeded"}
    elif outcome == "refused":
        with pytest.raises(HeadlessApiError) as refused:
            kernel.set_io_output("io.1", "do_0", True)
        assert refused.value.code == "cell_latched"
    else:
        with pytest.raises(IoOutputWriteFailedError) as failed:
            kernel.set_io_output("io.1", "do_0", True)
        assert failed.value.code == "io_output_acceptance_unknown"
        assert failed.value.outcome == outcome
    assert [packet["command"] for packet in packets] == ["set_io_output"]
    assert packets[0]["request"] == {"signal_id": "io.1/do_0", "value": True, "origin": "program"}
