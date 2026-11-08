"""Thin Behavior logical-scene client contract."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.scene_client import (
    SceneClient,
    SceneClientResponseError,
    SceneOutcomeUnknownError,
    SceneTransactionRejectedError,
)


class FakeTransport:
    def __init__(self) -> None:
        self.request: Mapping[str, Any] | None = None

    def scene(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "logical_scene": {},
        }

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.request = body
        return {
            "api_version": "manyforge.manager_command_response.v2",
            "request_id": body["request_context"]["request_id"],
            "request_owner_epoch": body["request_context"]["request_owner_epoch"],
            "ok": True,
            "projection_status": "committed",
            "committed_projection_revision": {"scene_revision": {}},
        }


def test_scene_client_forwards_without_owning_scene_state() -> None:
    transport = FakeTransport()
    client = SceneClient(transport)
    assert client.snapshot()["logical_scene"] == {}
    request = {"request_context": {"request_id": "request.1", "request_owner_epoch": "scene.1"}}
    assert client.commit(request)["projection_status"] == "committed"
    assert transport.request is request


def test_scene_client_rejects_malformed_owner_responses() -> None:
    class BadTransport(FakeTransport):
        def scene(self) -> dict[str, Any]:
            return {"schema_version": "legacy.scene"}

    with pytest.raises(SceneClientResponseError):
        SceneClient(BadTransport()).snapshot()

    class FailedTransport(FakeTransport):
        def commit_scene_transaction(
            self, body: Mapping[str, Any]
        ) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(SceneClientResponseError):
        SceneClient(FailedTransport()).commit({})


def test_lost_commit_response_uses_only_exact_result_lookup():
    class LostResponse(FakeTransport):
        def commit_scene_transaction(self, body):
            self.result = super().commit_scene_transaction(body)
            self.commits = getattr(self, "commits", 0) + 1
            raise OSError("reply lost after commit")

        def scene_transaction_status(self, body):
            assert body == self.request
            return {"api_version": "manyforge.manager_command_response.v2", "ok": True,
                    "result": self.result}

    transport = LostResponse()
    client = SceneClient(transport)
    request = {"request_context": {"request_id": "request.1", "request_owner_epoch": "scene.1"}}
    with pytest.raises(SceneOutcomeUnknownError):
        client.commit(request)
    assert client.receipt(request)["ok"]
    assert transport.commits == 1


@pytest.mark.parametrize("response", [None, {"detail": {"code": "manager_command_unavailable"}},
                                    {"api_version": "headless.error.v1", "error": {"code": "manager_command_unavailable"}}])
def test_typed_transport_error_preserves_cause_without_claiming_scene_outcome(response):
    class TypedFailure(RuntimeError):
        code = "manager_command_unavailable"
        details = {"reason": "timeout", "component_id": "manager"}

        def __init__(self):
            super().__init__("Manager command transport failed")
            self.response = response

    class FailedTransport(FakeTransport):
        def commit_scene_transaction(self, body):
            self.calls = getattr(self, "calls", 0) + 1
            raise TypedFailure()

    transport = FailedTransport()
    with pytest.raises(SceneOutcomeUnknownError) as caught:
        SceneClient(transport).commit({})
    assert not hasattr(caught.value, "code")
    assert caught.value.cause_code == "manager_command_unavailable"
    assert caught.value.cause_details == {"reason": "timeout", "component_id": "manager"}
    assert "manager_command_unavailable" in str(caught.value)
    assert "Scene result is malformed" not in str(caught.value)
    assert isinstance(caught.value.__cause__, TypedFailure)
    assert transport.calls == 1


def test_owner_rejection_still_preserves_exact_manager_receipt():
    receipt = {
        "api_version": "manyforge.manager_command_response.v2",
        "ok": False,
        "projection_status": "not_attempted",
        "request_id": "request.1",
        "request_owner_epoch": "scene.1",
        "error": {"code": "scene_refused", "message": "refused"},
    }

    class OwnerFailure(RuntimeError):
        code = "scene_refused"
        response = receipt

    class RejectedTransport(FakeTransport):
        def commit_scene_transaction(self, body):
            raise OwnerFailure("refused")

    with pytest.raises(SceneTransactionRejectedError) as caught:
        SceneClient(RejectedTransport()).commit({})
    assert caught.value.code == "scene_refused"
    assert caught.value.response == receipt


def test_malformed_manager_reply_still_reports_malformed():
    class FailedTransport(FakeTransport):
        def commit_scene_transaction(self, body):
            return {"api_version": "manyforge.manager_command_response.v0"}

    with pytest.raises(SceneOutcomeUnknownError, match="Scene result is malformed"):
        SceneClient(FailedTransport()).commit({})


def test_exception_with_unsupported_manager_response_version_remains_malformed():
    class BadManagerReply(RuntimeError):
        code = "authoring_not_ready"
        response = {"api_version": "manyforge.manager_command_response.v0"}

    class FailedTransport(FakeTransport):
        def commit_scene_transaction(self, body):
            self.calls = getattr(self, "calls", 0) + 1
            raise BadManagerReply("invalid reply")

    transport = FailedTransport()
    with pytest.raises(SceneOutcomeUnknownError, match="Scene result is malformed") as caught:
        SceneClient(transport).commit({})
    assert not hasattr(caught.value, "code")
    assert transport.calls == 1


def test_uncertain_manager_reply_retains_typed_cause_without_becoming_refusal():
    class UncertainTransport(FakeTransport):
        def commit_scene_transaction(self, body):
            return {
                "api_version": "manyforge.manager_command_response.v2",
                "ok": False, "projection_status": "failed_projection_uncertain",
                "error": {"code": "authoring_not_ready", "message": "uncertain",
                          "details": {"reason": "projection_unknown"}},
            }

    with pytest.raises(SceneOutcomeUnknownError) as caught:
        SceneClient(UncertainTransport()).commit({})
    assert not hasattr(caught.value, "code")
    assert caught.value.cause_code == "authoring_not_ready"
    assert caught.value.cause_details == {"reason": "projection_unknown"}
