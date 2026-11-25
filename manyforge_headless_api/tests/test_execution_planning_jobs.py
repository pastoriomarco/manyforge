"""Pick/place planning jobs on the execution client (OD-213 (1)).

A plan is answered at acceptance with its job; the job's state, and its result
once done, are read by name; cancel and release by name too. Nothing waits
for the plan. The core sends the job commands on the host's control socket
(``manyforge_headless/test/test_gateway_core.cpp``).
"""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.tests.fake_gateway import fake_client

_RESPONSE = "manyforge.execution_command_response.v6"


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


def test_plan_is_answered_with_the_accepted_job() -> None:
    sent: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        sent.append(command)
        return {"api_version": _RESPONSE, "command": command["command"], "ok": True,
                "planning_job": _job("accepted")}

    client = fake_client(exchange)
    assert client.plan_pick(_Request(), trusted_principal_id="operator.1")[
        "planning_job"]["state"] == "accepted"
    client.plan_place(_Request(), trusted_principal_id=None)
    assert [command["command"] for command in sent] == ["plan_pick", "plan_place"]
    assert sent[0]["intent"] == {"sequence_id": "job-1"}
    assert [call[2] for call in client._gateway.calls] == ["operator.1", None]  # noqa: SLF001


def test_plan_refusal_passes_through() -> None:
    refusal = {"api_version": _RESPONSE, "ok": False,
               "error": {"code": "execution_planning_jobs_busy", "message": "full"}}
    client = fake_client(lambda _command: refusal)
    assert client.plan_pick(_Request(), trusted_principal_id=None) == refusal


def test_read_cancel_and_release_name_the_job() -> None:
    sent: list[dict[str, Any]] = []
    job = _job("cancelled")

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        sent.append(command)
        return {"api_version": _RESPONSE, "command": command["command"], "ok": True,
                "planning_job": job}

    client = fake_client(exchange)
    assert client.read_planning_job("job-1")["planning_job"] == job
    assert client.cancel_planning_job("job-1")["planning_job"] == job
    client.release_planning_job("job-1")
    assert sent == [
        {"api_version": "manyforge.execution_command.v4", "command": command,
         "job_id": "job-1"}
        for command in ("read_planning_job", "cancel_planning_job", "release_planning_job")
    ]


def test_a_typed_release_refusal_passes_through() -> None:
    refusal = {"api_version": _RESPONSE, "ok": False,
               "error": {"code": "planning_job_unfinished", "message": "planning"}}
    client = fake_client(lambda _command: refusal)
    assert client.release_planning_job("job-1")["error"]["code"] == "planning_job_unfinished"


@pytest.mark.parametrize("job_id", ["", "a" * 97, "é" * 49, 7, None])
def test_job_names_are_bounded_before_any_exchange(job_id) -> None:
    client = fake_client(lambda _command: pytest.fail("sent"))
    with pytest.raises(ValueError):
        client.read_planning_job(job_id)
