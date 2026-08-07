"""Validated adapter over the pybind11-owned H0 semantic core."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Protocol, Sequence
import unicodedata


MAX_NATIVE_RESPONSE_BYTES = 64 * 1024 * 1024


class HeadlessApiError(RuntimeError):
    """Stable failure returned by a headless owner or projection adapter."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class HeadlessApiConfig:
    publication_root: Path
    package_root: Path
    node_catalog_path: Path
    alarm_catalog_path: Path
    historian_database_path: Path
    manager_state_root: Path
    execution_state_root: Path
    cell_id: str
    resource_ids: tuple[str, ...] = ()
    owner_id: str = "headless-api"
    # Deterministic override for in-process qualification only. Production
    # launchers leave this unset so every owner restart mints a new epoch.
    owner_epoch: str | None = None
    freshness_milliseconds: int = 2_000
    historian_queue_capacity: int = 1_024
    historian_record_retention: int = 100_000
    mutation_replay_capacity: int = 4_096
    manager_timeout_seconds: float = 0.25
    execution_timeout_seconds: float = 10.0
    maximum_request_body_bytes: int = 1024 * 1024

    def __post_init__(self) -> None:
        for label, value in (("cell_id", self.cell_id), ("owner_id", self.owner_id)):
            if (
                not value
                or unicodedata.normalize("NFC", value) != value
                or len(value.encode("utf-8")) > 256
            ):
                raise ValueError(f"{label} must be bounded non-empty NFC text")
        if self.owner_epoch is not None and (
            not self.owner_epoch
            or unicodedata.normalize("NFC", self.owner_epoch) != self.owner_epoch
            or len(self.owner_epoch.encode("utf-8")) > 256
        ):
            raise ValueError("owner_epoch must be bounded non-empty NFC text")
        if tuple(sorted(set(self.resource_ids))) != self.resource_ids:
            raise ValueError("resource_ids must be strictly sorted and unique")
        if len(self.resource_ids) > 4_096:
            raise ValueError("resource_ids exceeds the 4096-resource bound")
        for resource_id in self.resource_ids:
            if (
                not resource_id
                or unicodedata.normalize("NFC", resource_id) != resource_id
                or len(resource_id.encode("utf-8")) > 1_024
            ):
                raise ValueError(
                    "resource_ids must contain bounded non-empty NFC text"
                )
        if self.freshness_milliseconds < 1:
            raise ValueError("freshness_milliseconds must be positive")
        if not 0 < self.manager_timeout_seconds <= 5:
            raise ValueError("manager_timeout_seconds must be in (0, 5]")
        if not 0 < self.execution_timeout_seconds <= 30:
            raise ValueError("execution_timeout_seconds must be in (0, 30]")
        if not 1_024 <= self.maximum_request_body_bytes <= 1024 * 1024:
            raise ValueError(
                "maximum_request_body_bytes must be between 1024 and 1048576"
            )


class HeadlessCorePort(Protocol):
    def runtime_snapshot(self) -> dict[str, Any]: ...

    def node_environment_projection(self) -> dict[str, Any]: ...

    def alarm_snapshot(
        self,
        limit: int | None = 32,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]: ...

    def alarm_metrics(self) -> dict[str, Any]: ...

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]: ...

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]: ...

    def history(self, **kwargs: Any) -> dict[str, Any]: ...

    def historian_health(self) -> dict[str, Any]: ...

    def reconcile_historian_health(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]: ...

    def shutdown(self) -> None: ...


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value} is forbidden")


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def strict_json_object(payload: str, *, label: str) -> dict[str, Any]:
    if len(payload.encode("utf-8")) > MAX_NATIVE_RESPONSE_BYTES:
        raise HeadlessApiError(
            "headless_projection_response_oversized",
            f"{label} exceeds the native response byte bound",
        )
    try:
        value = json.loads(
            payload,
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (UnicodeError, ValueError, TypeError) as exc:
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"{label} is not strict JSON: {exc}",
        ) from exc
    if not isinstance(value, dict):
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"{label} root must be an object",
        )
    return value


class NativeHeadlessCore:
    """Decode the canonical bridge envelope without recreating owner state."""

    def __init__(self, config: HeadlessApiConfig) -> None:
        try:
            import manyforge_headless_py  # type: ignore[import-not-found]
        except ImportError as exc:
            raise HeadlessApiError(
                "headless_native_binding_unavailable",
                "manyforge_headless_py is not importable",
            ) from exc
        try:
            self._native = manyforge_headless_py.HeadlessApiCore(
                str(config.publication_root),
                str(config.package_root),
                str(config.node_catalog_path),
                str(config.alarm_catalog_path),
                str(config.historian_database_path),
                config.cell_id,
                list(config.resource_ids),
                config.owner_id,
                config.owner_epoch,
                config.freshness_milliseconds,
                config.historian_queue_capacity,
                config.historian_record_retention,
                config.mutation_replay_capacity,
            )
        except Exception as exc:
            raise HeadlessApiError(
                "headless_native_initialization_failed",
                f"headless native core initialization failed: {exc}",
            ) from exc

    def _invoke(self, method: str, *args: Any, **kwargs: Any) -> Any:
        payload = getattr(self._native, method)(*args, **kwargs)
        if not isinstance(payload, str):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned a non-text envelope",
            )
        envelope = strict_json_object(payload, label=f"native {method} response")
        ok = envelope.get("ok")
        if ok is True and set(envelope) == {"ok", "value"}:
            return envelope["value"]
        if ok is False and set(envelope) == {"error", "ok"}:
            error = envelope["error"]
            if (
                isinstance(error, dict)
                and set(error) == {"code", "message"}
                and isinstance(error["code"], str)
                and error["code"]
                and isinstance(error["message"], str)
                and error["message"]
            ):
                raise HeadlessApiError(error["code"], error["message"])
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"native {method} returned an invalid envelope",
        )

    @staticmethod
    def _require_object(value: Any, *, method: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} value must be an object",
            )
        return value

    def runtime_snapshot(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("runtime_snapshot"),
            method="runtime_snapshot",
        )

    def node_environment_projection(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("node_environment_projection"),
            method="node_environment_projection",
        )

    def alarm_snapshot(
        self,
        limit: int | None = 32,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke("alarm_snapshot", limit, list(states), ordering),
            method="alarm_snapshot",
        )

    def alarm_metrics(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("alarm_metrics"),
            method="alarm_metrics",
        )

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("acknowledge_alarm", **kwargs),
            method="acknowledge_alarm",
        )

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("acknowledge_all", **kwargs),
            method="acknowledge_all",
        )

    def history(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("history", **kwargs),
            method="history",
        )

    def historian_health(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("historian_health"),
            method="historian_health",
        )

    def reconcile_historian_health(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("reconcile_historian_health", **kwargs),
            method="reconcile_historian_health",
        )

    def shutdown(self) -> None:
        self._native.shutdown()
