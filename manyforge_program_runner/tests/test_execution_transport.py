"""Program-owner direct execution transport boundary."""

from __future__ import annotations

from pathlib import Path
from copy import deepcopy
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


@pytest.mark.parametrize(
    "failure",
    ["command", "group", "missing_groups", "empty_groups", "release_revision"],
)
def test_selection_results_must_identify_the_requested_transition(
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
    elif failure == "group":
        response["accepted"]["group_ids"] = ["grippers"]
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
        response["scope"]["selection_revision"] = 2
    with pytest.raises(KernelClientResponseError):
        if failure == "release_revision":
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
        "api_version": "manyforge.execution_command.v1",
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
            "api_version": "manyforge.execution_command.v1",
            "command": "register_program_run_capture",
            "capture": capture,
        }
    ]
    response["claim_generation"] = 5
    with pytest.raises(HeadlessApiError, match="registered claim"):
        transport.register_program_run_capture(capture)


def test_control_origin_is_forwarded_privately_and_forbidden_publicly(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    request = {
        "request_context": _stop_body()["request_context"],
        "operation_id": "operation.1",
        "expected_operation_revision": 2,
        "reason": "Program Resume",
        "execution_scope": {
            "node_instance_id": "resume",
            "node_attempt_id": "resume.1",
        },
    }
    commands = []

    def exchange(_client, command, _socket_path):
        commands.append(command)
        return {
            "ok": True,
            "command": "resume",
            "accepted": {
                "request_id": request["request_context"]["request_id"],
                "operation_id": "operation.1",
                "control": "resume",
                "accepted_against_revision": 2,
            },
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    assert transport.resume("operation.1", request)["ok"] is True
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
                "component_role_id": "tool",
                "target_role_id": "gripper",
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
