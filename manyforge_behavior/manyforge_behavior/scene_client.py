"""Strict stateless Behavior client for the logical-scene authority."""

from __future__ import annotations

from typing import Any, Mapping, Protocol


class HeadlessSceneTransport(Protocol):
    """The bounded logical-scene HTTP calls used by Behavior."""

    def scene(self) -> dict[str, Any]: ...

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def scene_transaction_status(self, body: Mapping[str, Any]) -> dict[str, Any]: ...


class SceneClientResponseError(RuntimeError):
    """Raised when a nominally successful transport response is malformed."""


class SceneOutcomeUnknownError(SceneClientResponseError):
    """A submitted transaction has no trustworthy terminal owner result."""


class SceneTransactionRejectedError(SceneClientResponseError):
    def __init__(self, result: Mapping[str, Any]) -> None:
        self.code = result["error"]["code"]
        self.response = dict(result)
        super().__init__(result["error"]["message"])


class SceneClient:
    """Forward scene reads/intents without caching or reducing owner truth."""

    def __init__(self, transport: HeadlessSceneTransport) -> None:
        for method in ("scene", "commit_scene_transaction"):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"scene transport has no callable {method}")
        self._transport = transport

    def snapshot(self) -> dict[str, Any]:
        value = self._transport.scene()
        if (
            not isinstance(value, dict)
            or value.get("schema_version") != "manyforge.scene_projection.v1"
        ):
            raise SceneClientResponseError(
                "headless scene read returned an unsupported projection"
            )
        return value

    def commit(self, request: Mapping[str, Any]) -> dict[str, Any]:
        try:
            value = self._transport.commit_scene_transaction(request)
        except Exception as error:
            value = getattr(error, "response", None)
            if value is None:
                raise SceneOutcomeUnknownError("Scene transaction delivery/outcome is unconfirmed") from error
        return self.result(request, value)

    @staticmethod
    def result(request: Mapping[str, Any], value: Any) -> dict[str, Any]:
        context = request.get("request_context", {})
        if (
            not isinstance(value, dict)
            or value.get("api_version")
            != "manyforge.manager_command_response.v1"
            or not context.get("request_id")
            or value.get("request_id") != context["request_id"]
            or not context.get("request_owner_epoch")
            or value.get("request_owner_epoch") != context["request_owner_epoch"]
        ):
            raise SceneOutcomeUnknownError("Scene result is malformed or names another request")
        if (value.get("ok") is False
            and value.get("projection_status") in {"not_attempted", "rejected_rollback_proven"}
            and isinstance(value.get("error"), dict)
            and isinstance(value["error"].get("code"), str)
            and isinstance(value["error"].get("message"), str)):
            raise SceneTransactionRejectedError(value)
        if (value.get("ok") is not True or value.get("projection_status") != "committed"
            or not isinstance(value.get("committed_projection_revision"), dict)):
            raise SceneOutcomeUnknownError("Scene effect has no proven terminal result")
        return value

    def receipt(self, request: Mapping[str, Any]) -> dict[str, Any] | None:
        """Read the exact result; absence never permits another submission."""
        value = self._transport.scene_transaction_status(request)
        if (not isinstance(value, dict) or set(value) != {"api_version", "ok", "result"}
            or value.get("api_version") != "manyforge.manager_command_response.v1"
            or value.get("ok") is not True):
            raise SceneOutcomeUnknownError("Scene result lookup is unavailable")
        return None if value["result"] is None else self.result(request, value["result"])


__all__ = [
    "HeadlessSceneTransport",
    "SceneClient",
    "SceneClientResponseError",
    "SceneOutcomeUnknownError",
    "SceneTransactionRejectedError",
]
