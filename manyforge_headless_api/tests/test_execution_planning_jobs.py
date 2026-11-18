"""Pick/place planning jobs on the execution client (OD-213 (1)).

A plan is answered at acceptance with its job; the job's state, and its result
once done, are read by name on the host's control socket; cancel and release
go there too. Nothing waits for the plan.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.native import HeadlessApiError

_RESPONSE = "manyforge.execution_command_response.v6"
_RESULT = {
    "attempted_candidate_count": 1,
    "kind": "pick",
    "last_attempted_candidate": {},
    "plan": {},
    "reports": [],
    "schema_version": "manyforge.pick_place_planning_result.v1",
    "status": "planned",
}


def _job(state: str, **members: Any) -> dict[str, Any]:
    return {
        "error": None,
        "job_id": "job-1",
        "kind": "pick",
        "planning_result": None,
        "schema_version": "manyforge.planning_job.v1",
        "state": state,
        **members,
    }


class _Request:
    sequence_id = "job-1"

    @staticmethod
    def model_dump(mode: str) -> dict[str, Any]:
        assert mode == "json"
        return {"sequence_id": "job-1"}


def _client(tmp_path: Path) -> ExecutionCommandClient:
    return ExecutionCommandClient(tmp_path)


def test_plan_is_answered_with_the_accepted_job(monkeypatch, tmp_path) -> None:
    client = _client(tmp_path)
    sent: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        sent.append(command)
        return {"api_version": _RESPONSE, "command": "plan_pick", "ok": True,
                "planning_job": _job("accepted")}

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client._plan_pick_place("plan_pick", _Request(), trusted_principal_id=None)
    assert response["planning_job"]["state"] == "accepted"
    assert sent[0]["command"] == "plan_pick"
    assert sent[0]["intent"] == {"sequence_id": "job-1"}


@pytest.mark.parametrize(
    "job",
    [
        _job("planning"),  # not answered at acceptance
        _job("accepted", job_id="another"),
        _job("accepted", kind="place"),
        {**_job("accepted"), "extra": True},
    ],
)
def test_plan_refuses_a_reply_that_is_not_this_accepted_job(monkeypatch, tmp_path, job) -> None:
    client = _client(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: {
        "api_version": _RESPONSE, "command": "plan_pick", "ok": True, "planning_job": job})
    with pytest.raises(HeadlessApiError) as error:
        client._plan_pick_place("plan_pick", _Request(), trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_plan_refusal_passes_through(monkeypatch, tmp_path) -> None:
    client = _client(tmp_path)
    refusal = {"api_version": _RESPONSE, "ok": False,
               "error": {"code": "execution_planning_jobs_busy", "message": "full"}}
    monkeypatch.setattr(client, "_exchange", lambda _command: refusal)
    assert client._plan_pick_place("plan_pick", _Request(), trusted_principal_id=None) == refusal


@pytest.mark.parametrize(
    "job",
    [
        _job("accepted"),
        _job("planning"),
        _job("done", planning_result=_RESULT),
        _job("failed", error={"code": "pick_planning_cancelled", "message": "cancelled"}),
        _job("cancelled"),
    ],
)
def test_read_and_cancel_go_to_the_control_socket_in_every_state(monkeypatch, tmp_path, job) -> None:
    client = _client(tmp_path)
    control: list[dict[str, Any]] = []

    def exchange_control(command: dict[str, Any]) -> dict[str, Any]:
        control.append(command)
        return {"api_version": _RESPONSE, "command": command["command"], "ok": True,
                "planning_job": job}

    monkeypatch.setattr(client, "_exchange_control", exchange_control)
    monkeypatch.setattr(client, "_exchange", lambda _command: pytest.fail("ordinary socket"))
    assert client.read_planning_job("job-1")["planning_job"] == job
    assert client.cancel_planning_job("job-1")["planning_job"] == job
    assert control == [
        {"api_version": "manyforge.execution_command.v4", "command": "read_planning_job",
         "job_id": "job-1"},
        {"api_version": "manyforge.execution_command.v4", "command": "cancel_planning_job",
         "job_id": "job-1"},
    ]


@pytest.mark.parametrize(
    "job",
    [
        _job("done"),  # done without a result
        _job("planning", planning_result=_RESULT),  # a result before done
        _job("failed"),  # failed without an error
        _job("cancelled", error={"code": "x_y", "message": "m"}),
        _job("done", planning_result={**_RESULT, "kind": "place"}),
        _job("waiting"),
        {**_job("accepted"), "schema_version": "manyforge.planning_job.v0"},
    ],
)
def test_read_refuses_a_malformed_job(monkeypatch, tmp_path, job) -> None:
    client = _client(tmp_path)
    monkeypatch.setattr(client, "_exchange_control", lambda command: {
        "api_version": _RESPONSE, "command": command["command"], "ok": True, "planning_job": job})
    with pytest.raises(HeadlessApiError) as error:
        client.read_planning_job("job-1")
    assert error.value.code == "execution_command_response_invalid"


def test_release_and_its_typed_refusals(monkeypatch, tmp_path) -> None:
    client = _client(tmp_path)
    replies = iter([
        {"api_version": _RESPONSE, "command": "release_planning_job", "ok": True},
        {"api_version": _RESPONSE, "ok": False,
         "error": {"code": "planning_job_unfinished", "message": "planning"}},
        {"api_version": _RESPONSE, "command": "release_planning_job", "ok": True,
         "planning_job": _job("done", planning_result=_RESULT)},
    ])
    monkeypatch.setattr(client, "_exchange_control", lambda _command: next(replies))
    assert client.release_planning_job("job-1")["ok"] is True
    assert client.release_planning_job("job-1")["error"]["code"] == "planning_job_unfinished"
    with pytest.raises(HeadlessApiError):
        client.release_planning_job("job-1")


@pytest.mark.parametrize("job_id", ["", "a" * 97, 7, None])
def test_job_names_are_bounded_before_any_exchange(monkeypatch, tmp_path, job_id) -> None:
    client = _client(tmp_path)
    monkeypatch.setattr(client, "_exchange_control", lambda _command: pytest.fail("sent"))
    with pytest.raises(ValueError):
        client.read_planning_job(job_id)
