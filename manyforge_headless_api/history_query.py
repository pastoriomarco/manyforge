"""One bounded History query contract for Headless and thin transport adapters."""

from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .models import _bounded_nfc


HistoryTime = Annotated[int, Field(ge=0, le=2**63 - 1)]
HistoryIdentity = Annotated[str, Field(min_length=1, max_length=1024)]
CORRELATION_FIELDS = (
    "program_run_id", "operation_id", "action_span_id", "node_instance_id", "node_attempt_id",
    "intervention_session_id", "contact_policy_id", "incident_id", "condition_id", "alarm_condition_id",
    "activation_id", "stop_id", "resource_id", "source_owner_epoch", "channel", "phase", "parent_node_attempt_id",
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
    stop_id: HistoryIdentity | None = None
    resource_id: HistoryIdentity | None = None
    source_owner_epoch: HistoryIdentity | None = None
    channel: HistoryIdentity | None = None
    phase: HistoryIdentity | None = None

    @field_validator("components", "event_types")
    @classmethod
    def valid_names(cls, values: list[str]) -> list[str]:
        return [_bounded_nfc(value, label="history filter", maximum_bytes=1024) for value in values]

    @field_validator("clock_owner", *CORRELATION_FIELDS)
    @classmethod
    def valid_identity(cls, value: str | None) -> str | None:
        return None if value is None else _bounded_nfc(value, label="history identity", maximum_bytes=1024)

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
        return self

    def native_arguments(self, *, now_utc_ns: int) -> dict:
        result = self.model_dump(exclude={"recent_seconds", "minimum_severity", *CORRELATION_FIELDS})
        if self.minimum_severity is not None:
            levels = ["DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"]
            result["severities"] = levels[levels.index(self.minimum_severity):]
        result["correlations"] = {key: getattr(self, key) for key in CORRELATION_FIELDS if getattr(self, key) is not None}
        if self.recent_seconds is not None:
            result["start_utc_ns"] = max(0, now_utc_ns - self.recent_seconds * 1_000_000_000)
            result["end_utc_ns"] = now_utc_ns
        return result

    def query_parameters(self) -> list[tuple[str, str | int]]:
        return [(key, item) for key, value in self.model_dump(exclude_none=True).items()
                for item in (value if isinstance(value, list) else [value])]


class HistorianDiscoveryRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    kind: Literal["program_run_id", "component", "channel", "event_type"]
    after: Annotated[str, Field(max_length=1024)] = ""
    limit: Annotated[int, Field(ge=1, le=32)] = 32

    @field_validator("after")
    @classmethod
    def valid_after(cls, value: str) -> str:
        return _bounded_nfc(value, label="history discovery cursor", maximum_bytes=1024) if value else ""


class HistorianDebugRequest(HistorianQueryRequest):
    run_id: HistoryIdentity | None = None

    @field_validator("run_id")
    @classmethod
    def valid_run_id(cls, value: str | None) -> str | None:
        return None if value is None else _bounded_nfc(value, label="history run", maximum_bytes=1024)

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
