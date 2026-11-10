"""Closed read-only client surface for assistant and MCP adapters."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from types import MappingProxyType
from typing import Any, Final
import unicodedata

from .client import HeadlessClientError, HeadlessHttpClient


MAXIMUM_SAFE_INTEGER: Final = 9_007_199_254_740_991
MAXIMUM_FILTER_ITEMS: Final = 256
MAXIMUM_FILTER_IDENTITY_BYTES: Final = 256
MAXIMUM_ALARM_PAGE: Final = 128
MAXIMUM_HISTORY_PAGE: Final = 32

READ_OPERATION_SCHEMAS: Final = MappingProxyType(
    {
        "health": "manyforge.headless_health.v1",
        "runtime": "manyforge.headless_runtime_snapshot.v1",
        "manager_status": "manyforge.manager_status_projection.v1",
        "manager_graph": "manyforge.manager_graph_projection.v1",
        "manager_diff": "manyforge.manager_diff_projection.v1",
        "node_environment": "manyforge.node_environment_projection.v2",
        "source_model": "manyforge.source_model_projection.v3",
        "scene": "manyforge.scene_projection.v1",
        "alarms": "manyforge.operator_alarm_snapshot.v2",
        "alarm_metrics": "manyforge.operator_alarm_metrics.v1",
        "history": "manyforge.historian_summary_page.v2",
        "historian_health": "manyforge.async_historian_health.v1",
    }
)
ALARM_STATES: Final = frozenset(
    {
        "active_unacknowledged",
        "active_acknowledged",
        "returned_unacknowledged",
        "returned_acknowledged",
    }
)


class HeadlessReadInputError(ValueError):
    """One fixed read operation received malformed or unsupported arguments."""


def _arguments(value: Mapping[str, Any] | None) -> dict[str, Any]:
    if value is None:
        return {}
    if not isinstance(value, Mapping):
        raise HeadlessReadInputError("read arguments must be an object")
    if any(not isinstance(key, str) for key in value):
        raise HeadlessReadInputError("read argument names must be strings")
    return dict(value)


def _require_keys(arguments: dict[str, Any], allowed: set[str]) -> None:
    unknown = sorted(set(arguments) - allowed)
    if unknown:
        raise HeadlessReadInputError(
            "unsupported read arguments: " + ", ".join(unknown)
        )


def _integer(
    value: Any,
    *,
    label: str,
    minimum: int,
    maximum: int,
) -> int:
    if type(value) is not int or value < minimum or value > maximum:
        raise HeadlessReadInputError(
            f"{label} must be an integer in [{minimum}, {maximum}]"
        )
    return value


def _identities(value: Any, *, label: str) -> tuple[str, ...]:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes)):
        raise HeadlessReadInputError(f"{label} must be an array of strings")
    if len(value) > MAXIMUM_FILTER_ITEMS:
        raise HeadlessReadInputError(
            f"{label} exceeds {MAXIMUM_FILTER_ITEMS} entries"
        )
    result: list[str] = []
    for index, item in enumerate(value):
        if (
            not isinstance(item, str)
            or not item
            or item != item.strip()
            or unicodedata.normalize("NFC", item) != item
            or len(item.encode("utf-8")) > MAXIMUM_FILTER_IDENTITY_BYTES
        ):
            raise HeadlessReadInputError(
                f"{label}[{index}] must be one bounded NFC identity"
            )
        if item in result:
            raise HeadlessReadInputError(f"{label} contains duplicate identities")
        result.append(item)
    return tuple(result)


def _require_schema(
    operation: str,
    expected_schema: str,
    response: dict[str, Any],
) -> dict[str, Any]:
    if response.get("schema_version") != expected_schema:
        raise HeadlessClientError(
            "headless_read_response_invalid",
            f"{operation} returned an unsupported schema",
        )
    return response


class HeadlessReadClient:
    """Invoke a closed set of bounded reads without mutation or local truth."""

    def __init__(
        self,
        transport: HeadlessHttpClient,
        *,
        owns_transport: bool = False,
    ) -> None:
        if not isinstance(transport, HeadlessHttpClient):
            raise TypeError("transport must be a HeadlessHttpClient")
        self._transport = transport
        self._owns_transport = owns_transport

    @classmethod
    def connect(
        cls,
        base_url: str,
        *,
        timeout_seconds: float = 2.0,
    ) -> "HeadlessReadClient":
        return cls(
            HeadlessHttpClient(base_url, timeout_seconds=timeout_seconds),
            owns_transport=True,
        )

    @staticmethod
    def operations() -> tuple[str, ...]:
        return tuple(READ_OPERATION_SCHEMAS)

    def invoke(
        self,
        operation: str,
        arguments: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        if operation not in READ_OPERATION_SCHEMAS:
            raise HeadlessReadInputError(
                f"unsupported headless read operation: {operation}"
            )
        values = _arguments(arguments)
        response: dict[str, Any]
        if operation in {
            "health",
            "runtime",
            "manager_status",
            "manager_graph",
            "manager_diff",
            "node_environment",
            "source_model",
            "scene",
            "alarm_metrics",
            "historian_health",
        }:
            _require_keys(values, set())
            response = getattr(self._transport, operation)()
        elif operation == "alarms":
            _require_keys(values, {"limit", "states"})
            limit = (
                None
                if "limit" not in values
                else _integer(
                    values["limit"],
                    label="limit",
                    minimum=0,
                    maximum=MAXIMUM_ALARM_PAGE,
                )
            )
            states = _identities(values.get("states", ()), label="states")
            if any(state not in ALARM_STATES for state in states):
                raise HeadlessReadInputError("states contains an unsupported value")
            response = self._transport.alarms(limit=limit, states=states)
        else:
            from .history_query import HistorianQueryRequest

            try:
                query = HistorianQueryRequest.model_validate(values, strict=True)
            except ValueError as error:
                raise HeadlessReadInputError(str(error)) from error
            response = self._transport.history(params=query.query_parameters())
        return _require_schema(
            operation,
            READ_OPERATION_SCHEMAS[operation],
            response,
        )

    def close(self) -> None:
        if self._owns_transport:
            self._transport.close()
            self._owns_transport = False

    def __enter__(self) -> "HeadlessReadClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


__all__ = [
    "HeadlessReadClient",
    "HeadlessReadInputError",
    "READ_OPERATION_SCHEMAS",
]
