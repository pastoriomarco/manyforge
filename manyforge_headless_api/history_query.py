"""One bounded History query contract for Headless and thin transport adapters."""

from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .identity import parse_application_start_id, parse_id_filter
from .models import _bounded_nfc


HistoryTime = Annotated[int, Field(ge=0, le=2**63 - 1)]
HistoryIdentity = Annotated[str, Field(min_length=1, max_length=1024)]
CORRELATION_FIELDS = (
    "program_run_id", "operation_id", "action_span_id", "node_instance_id", "node_attempt_id",
    "intervention_session_id", "contact_policy_id", "incident_id", "condition_id", "alarm_condition_id",
    "activation_id", "stop_event_id", "resource_id", "channel", "phase", "parent_node_attempt_id",
)


# Filters naming an id minted inside one start (contract 1.5).
START_MINTED_ID_FILTERS = (
    "program_run_id",
    "operation_id",
    "node_attempt_id",
    "parent_node_attempt_id",
    "stop_event_id",
)


class HistorianQueryRequest(BaseModel):
    # HTTP query scalars arrive as strings. Conversion belongs only at this
    # transport boundary; unknown fields still fail instead of being ignored.
    model_config = ConfigDict(extra="forbid", frozen=True)

    start_ns: HistoryTime | None = None
    end_ns: HistoryTime | None = None
    start_utc_ns: HistoryTime | None = None
    end_utc_ns: HistoryTime | None = None
    recent_seconds: Annotated[int, Field(ge=1, le=31 * 86400)] | None = None
    clock_owner: HistoryIdentity | None = None
    components: Annotated[list[str], Field(max_length=256)] = []
    event_types: Annotated[list[str], Field(max_length=256)] = []
    severities: Annotated[list[Literal["DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"]], Field(max_length=5)] = []
    minimum_severity: Literal["DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"] | None = None
    before_sequence: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None = None
    limit: Annotated[int, Field(ge=1, le=32)] = 32
    search: Annotated[str, Field(max_length=256)] = ""
    alarm_phase: Literal["all", "activated", "returned", "acknowledged", "raw"] | None = None
    program_run_id: HistoryIdentity | None = None
    operation_id: HistoryIdentity | None = None
    action_span_id: HistoryIdentity | None = None
    node_instance_id: HistoryIdentity | None = None
    node_attempt_id: HistoryIdentity | None = None
    parent_node_attempt_id: HistoryIdentity | None = None
    intervention_session_id: HistoryIdentity | None = None
    contact_policy_id: HistoryIdentity | None = None
    incident_id: HistoryIdentity | None = None
    condition_id: HistoryIdentity | None = None
    alarm_condition_id: HistoryIdentity | None = None
    activation_id: HistoryIdentity | None = None
    stop_event_id: HistoryIdentity | None = None
    resource_id: HistoryIdentity | None = None
    channel: HistoryIdentity | None = None
    phase: HistoryIdentity | None = None
    # One retained failure capture's protected range; unknown or retired
    # captures are reported as not found, never widened to all history.
    capture_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None = None
    # One application start's rows (16 lowercase hex). Without it, a run,
    # operation, node attempt, Stop or action span filter means the current
    # start, and other filters span every retained start.
    application_start_id: Annotated[str, Field(max_length=16)] | None = None

    @field_validator("application_start_id")
    @classmethod
    def valid_application_start_id(cls, value: str | None) -> str | None:
        if value is not None:
            parse_application_start_id(value)
        return value

    @field_validator("components", "event_types")
    @classmethod
    def valid_names(cls, values: list[str]) -> list[str]:
        return [_bounded_nfc(value, label="history filter", maximum_bytes=1024) for value in values]

    @field_validator("clock_owner", *CORRELATION_FIELDS)
    @classmethod
    def valid_identity(cls, value: str | None) -> str | None:
        return None if value is None else _bounded_nfc(value, label="history identity", maximum_bytes=1024)

    @field_validator(*START_MINTED_ID_FILTERS)
    @classmethod
    def valid_start_minted_id(cls, value: str | None, info) -> str | None:
        # A number, its display form (``3f9a/op7``) or its copy form
        # (``<16 hex>/op7``) (BS 2.9); kept as given until the store query.
        if value is not None:
            parse_id_filter(info.field_name, value)
        return value

    @field_validator("search")
    @classmethod
    def valid_search(cls, value: str) -> str:
        value = value.strip()
        if value and len(value) < 3:
            raise ValueError("indexed search requires at least three characters")
        return _bounded_nfc(value, label="indexed search", maximum_bytes=256) if value else ""

    @model_validator(mode="after")
    def coherent_window(self) -> "HistorianQueryRequest":
        if self.minimum_severity is not None and self.severities:
            raise ValueError("minimum severity and explicit severity set are mutually exclusive")
        if (self.start_ns is not None or self.end_ns is not None) and self.clock_owner is None:
            raise ValueError("monotonic history windows require clock_owner; use recorded UTC across owners")
        for start, end in ((self.start_ns, self.end_ns), (self.start_utc_ns, self.end_utc_ns)):
            if start is not None and end is not None and start > end:
                raise ValueError("history start must not exceed end")
        if self.recent_seconds is not None and (self.start_utc_ns is not None or self.end_utc_ns is not None):
            raise ValueError("relative and exact UTC windows are mutually exclusive")
        full, prefixes = self.start_identities()
        if len(full) > 1:
            raise ValueError("the filters name two different application starts")
        if full and any(prefix != next(iter(full))[:4] for prefix in prefixes):
            raise ValueError("a display form names another application start than the filters")
        return self

    def native_arguments(self, *, now_utc_ns: int) -> dict:
        result = self.model_dump(
            exclude={
                "recent_seconds",
                "minimum_severity",
                *CORRELATION_FIELDS,
            }
        )
        named = self.start_identities()[0]
        result["application_start_id"] = next(iter(named)) if named else None
        if self.minimum_severity is not None:
            levels = ["DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"]
            result["severities"] = levels[levels.index(self.minimum_severity):]
        result["correlations"] = {
            key: (
                str(parse_id_filter(key, getattr(self, key))[0])
                if key in START_MINTED_ID_FILTERS
                else getattr(self, key)
            )
            for key in CORRELATION_FIELDS
            if getattr(self, key) is not None
        }
        if self.recent_seconds is not None:
            result["start_utc_ns"] = max(0, now_utc_ns - self.recent_seconds * 1_000_000_000)
            result["end_utc_ns"] = now_utc_ns
        return result

    def start_identities(self) -> tuple[set[str], set[str]]:
        """The full ``A`` values (``application_start_id`` and copy forms) and
        the 4-hex prefixes of display forms the query names."""

        full: set[str] = set() if self.application_start_id is None else {self.application_start_id}
        prefixes: set[str] = set()
        for name in START_MINTED_ID_FILTERS:
            value = getattr(self, name)
            if value is None:
                continue
            _, named, prefix = parse_id_filter(name, value)
            if named is not None:
                full.add(named)
            if prefix is not None:
                prefixes.add(prefix)
        return full, prefixes

    def query_parameters(self) -> list[tuple[str, str | int]]:
        return [(key, item) for key, value in self.model_dump(exclude_none=True).items()
                for item in (value if isinstance(value, list) else [value])]


class HistorianCaptureRequest(BaseModel):
    """One bounded newest-first page of retained failure captures."""

    model_config = ConfigDict(extra="forbid", frozen=True)
    before_capture_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None = None
    limit: Annotated[int, Field(ge=1, le=32)] = 32


class HistorianDiscoveryRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    kind: Literal["program_run_id", "component", "channel", "event_type", "application_start_id"]
    after: Annotated[str, Field(max_length=1024)] = ""
    limit: Annotated[int, Field(ge=1, le=32)] = 32
    # Program runs are listed for one start (default: the current start), in
    # number order; ``application_start_id`` lists the starts that have rows,
    # newest first; the other kinds span every retained start.
    application_start_id: Annotated[str, Field(max_length=16)] | None = None

    @field_validator("application_start_id")
    @classmethod
    def valid_application_start_id(cls, value: str | None) -> str | None:
        if value is not None:
            parse_application_start_id(value)
        return value

    @field_validator("after")
    @classmethod
    def valid_after(cls, value: str) -> str:
        return _bounded_nfc(value, label="history discovery cursor", maximum_bytes=1024) if value else ""


class HistorianDebugRequest(HistorianQueryRequest):
    run_id: HistoryIdentity | None = None

    @field_validator("run_id")
    @classmethod
    def valid_run_id(cls, value: str | None) -> str | None:
        if value is None:
            return None
        parse_id_filter("program_run_id", value)
        return value

    @model_validator(mode="after")
    def coherent_run(self) -> "HistorianDebugRequest":
        if self.run_id is not None and self.program_run_id not in {None, self.run_id}:
            raise ValueError("run_id and program_run_id must select the same exact Program run")
        return self

    def history_query(self) -> HistorianQueryRequest:
        return HistorianQueryRequest.model_validate({
            **self.model_dump(exclude={"run_id"}),
            "program_run_id": self.program_run_id or self.run_id,
        })
