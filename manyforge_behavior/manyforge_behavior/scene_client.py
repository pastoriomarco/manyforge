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
    """A submitted transaction has no trustworthy terminal owner result.

    A cause is separate from ``code``: callers use that attribute to retry
    proven owner refusals, which is not safe for an unknown outcome.
    """


class SceneTransactionRejectedError(SceneClientResponseError):
    def __init__(self, result: Mapping[str, Any]) -> None:
        self.code = result["error"]["code"]
        self.response = dict(result)
        super().__init__(result["error"]["message"])


def _unknown_with_cause(message: str, code: object, details: object) -> SceneOutcomeUnknownError:
    if not isinstance(code, str) or not code:
        return SceneOutcomeUnknownError(message)
    unknown = SceneOutcomeUnknownError(f"{message}: {code}")
    unknown.cause_code = code
    if isinstance(details, Mapping):
        unknown.cause_details = dict(details)
    return unknown


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
            code = getattr(error, "code", None)
            version = value.get("api_version") if isinstance(value, Mapping) else None
            claims_manager_response = isinstance(version, str) and version.startswith(
                "manyforge.manager_command_response."
            )
            if isinstance(code, str) and code and (
                value is None
                or isinstance(value, Mapping) and not claims_manager_response
            ):
                raise _unknown_with_cause(
                    f"Scene transaction delivery/outcome is unconfirmed: {error}",
                    code, getattr(error, "details", None),
                ) from error
            if value is None:
                raise SceneOutcomeUnknownError("Scene transaction delivery/outcome is unconfirmed") from error
        return self.result(value)

    @staticmethod
    def result(value: Any) -> dict[str, Any]:
        if (
            not isinstance(value, dict)
            or value.get("api_version")
            != "manyforge.manager_command_response.v2"
        ):
            raise SceneOutcomeUnknownError("Scene result is malformed")
        if (value.get("ok") is False
            and value.get("projection_status") in {"not_attempted", "rejected_rollback_proven"}
            and isinstance(value.get("error"), dict)
            and isinstance(value["error"].get("code"), str)
            and isinstance(value["error"].get("message"), str)):
            raise SceneTransactionRejectedError(value)
        if (value.get("ok") is not True or value.get("projection_status") != "committed"
            or not isinstance(value.get("committed_projection_revision"), dict)):
            owner_error = value.get("error")
            if isinstance(owner_error, Mapping):
                raise _unknown_with_cause(
                    "Scene effect has no proven terminal result",
                    owner_error.get("code"), owner_error.get("details"),
                )
            raise SceneOutcomeUnknownError("Scene effect has no proven terminal result")
        return value

    def receipt(self, request: Mapping[str, Any]) -> dict[str, Any] | None:
        """Read the exact result; absence never permits another submission."""
        value = self._transport.scene_transaction_status(request)
        if (not isinstance(value, dict) or set(value) != {"api_version", "ok", "result"}
            or value.get("api_version") != "manyforge.manager_command_response.v2"
            or value.get("ok") is not True):
            raise SceneOutcomeUnknownError("Scene result lookup is unavailable")
        return None if value["result"] is None else self.result(value["result"])


__all__ = [
    "HeadlessSceneTransport",
    "SceneClient",
    "SceneClientResponseError",
    "SceneOutcomeUnknownError",
    "SceneTransactionRejectedError",
]
