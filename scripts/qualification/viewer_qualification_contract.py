"""Machine-readable contract for viewer performance qualification artifacts."""
from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


class ViewerQualificationSampleV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    index: int = Field(ge=0)
    monotonic_ns: int = Field(ge=0)
    viewer_route_latency_ms: float = Field(ge=0.0)
    kernel_route_latency_ms: float = Field(ge=0.0)
    viewer_status: dict[str, Any]
    kernel_metrics: dict[str, Any]
    kernel_snapshot: dict[str, Any] = Field(default_factory=dict)
    observers: dict[str, dict[str, Any]] = Field(default_factory=dict)
    processes: dict[str, dict[str, Any]]
    errors: list[str]


class ViewerQualificationArtifactV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    schema_version: Literal["manyforge.viewer.qualification.v1"]
    scenario: str = Field(min_length=1)
    captured_at: str
    source: dict[str, Any]
    environment: dict[str, Any]
    configuration: dict[str, Any]
    samples: list[ViewerQualificationSampleV1]
    summary: dict[str, Any]
    failures: list[str]
    success: bool


class ViewerQualificationCaptureV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    duration_s: float = Field(gt=0.0)
    warmup_s: float = Field(ge=0.0)
    hz: float = Field(gt=0.0)
    timeout_s: float = Field(gt=0.0)


class ViewerQualificationRequirementsV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    active_client: bool = False
    no_client: bool = False
    zero_builds: bool = False
    client_count_min: int | None = Field(default=None, ge=0)
    client_count_max: int | None = Field(default=None, ge=0)
    active_client_count_min: int | None = Field(default=None, ge=0)
    required_layers: list[str] = Field(default_factory=list)
    viewer_counter_delta_min: dict[str, float] = Field(default_factory=dict)
    kernel_counter_delta_min: dict[str, float] = Field(default_factory=dict)
    kernel_state_sample_min: dict[str, int] = Field(default_factory=dict)
    client_total_min: dict[str, float] = Field(default_factory=dict)
    observer_counter_delta_min: dict[str, dict[str, float]] = Field(
        default_factory=dict
    )


class ViewerQualificationBudgetsV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    viewer_route_p95_ms: float = Field(gt=0.0)
    process_cpu_p95_percent: dict[str, float] = Field(default_factory=dict)
    client_long_tasks_max: int | None = Field(default=None, ge=0)
    client_materialization_max_ms: float | None = Field(default=None, ge=0.0)
    client_react_commits_per_frame_max: float | None = Field(default=None, ge=0.0)
    client_unmatched_topology_rebuilds_max: int | None = Field(default=None, ge=0)
    build_failures_max: int = Field(default=0, ge=0)
    publication_sink_drops_max: int = Field(default=0, ge=0)


class ViewerQualificationScenarioV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    name: str = Field(min_length=1)
    description: str = Field(min_length=1)
    capture: ViewerQualificationCaptureV1
    requirements: ViewerQualificationRequirementsV1
    budgets: ViewerQualificationBudgetsV1
    observer_paths: dict[str, str] = Field(default_factory=dict)
    required_process_labels: list[str] = Field(default_factory=list)
    preconditions: list[str] = Field(default_factory=list)


class ViewerQualificationMatrixV1(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    schema_version: Literal["manyforge.viewer.qualification_matrix.v1"]
    policy: dict[str, Any]
    scenarios: list[ViewerQualificationScenarioV1] = Field(min_length=1)
