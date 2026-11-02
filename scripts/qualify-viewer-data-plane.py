#!/usr/bin/env python3
"""Pure viewer-qualification evidence helpers plus the I9/I runtime gate.

The executable collector is intentionally unavailable during the composable
cell cutover.  Its complete historical implementation remains at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``843c2fa6074f62c4e62c157555cef3c242bd0093``), with this disposition:

* lines 44-53 and 216-268 supplied value-only assignment, percentile, numeric
  delta, and observer-flattening helpers retained here with explicit bounds;
* lines 290-623 mixed reusable sample/metric/policy calculations with old
  viewer-v1 fanout, producer-epoch, manifest, and client-window vocabulary.
  The reusable calculations remain; the old wire-shaped extraction does not;
* lines 55-214 performed HTTP and ``/proc`` observation, lines 270-288 invoked
  Git, and lines 635-967 parsed arguments, sampled old Composer routes and
  processes, and wrote artifacts.  Those effectful paths return at I9/I; and
* line 624's profile selection remains only as a pure selection over an
  already-validated matrix.  This module never opens the matrix file.

The unchanged machine-readable value schema is blob
``96150651493f85ca9c82dbf60f6f4cd33be571fe``.  Validating or summarizing
already-captured values is not live qualification and does not imply that the
old routes, process topology, viewer-v1 protocol, or Composer runtime exist.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import math
import sys
from typing import TYPE_CHECKING, Any, Final, NoReturn

if TYPE_CHECKING:
    from scripts.qualification.viewer_qualification_contract import (
        ViewerQualificationArtifactV1,
        ViewerQualificationMatrixV1,
        ViewerQualificationSampleV1,
        ViewerQualificationScenarioV1,
    )


RUNTIME_UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
VIEWER_QUALIFICATION_RESTORATION_GATES: Final = ("I9", "I")

MAX_ASSIGNMENT_CHARS: Final = 4_096
MAX_QUALIFICATION_SAMPLES: Final = 1_000_000
MAX_OBSERVER_NUMERIC_FIELDS: Final = 4_096
MAX_OBSERVER_NESTING_DEPTH: Final = 16


class ViewerQualificationUnavailableError(RuntimeError):
    """Raised before network, process, clock, repository, or file access."""


class ViewerQualificationBoundsError(ValueError):
    """Raised when a pure qualification helper receives unbounded input."""


def _raise_viewer_qualification_unavailable() -> NoReturn:
    raise ViewerQualificationUnavailableError(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: the observer-owned viewer "
        "qualification collector returns at I9/I"
    )


def _contract_types() -> tuple[type[Any], type[Any], type[Any]]:
    """Load value-schema classes only when a pure validator is requested."""

    from scripts.qualification.viewer_qualification_contract import (
        ViewerQualificationArtifactV1,
        ViewerQualificationMatrixV1,
        ViewerQualificationSampleV1,
    )

    return (
        ViewerQualificationArtifactV1,
        ViewerQualificationMatrixV1,
        ViewerQualificationSampleV1,
    )


def _finite_number(value: object, *, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"{field_name} must be a finite number")
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError(f"{field_name} must be finite")
    return parsed


def _parse_assignment(value: str, *, value_type: type = str) -> tuple[str, Any]:
    """Parse one bounded ``LABEL=VALUE`` pair without touching CLI state."""

    if not isinstance(value, str):
        raise TypeError("assignment must be a string")
    if len(value) > MAX_ASSIGNMENT_CHARS:
        raise ViewerQualificationBoundsError(
            f"assignment exceeds {MAX_ASSIGNMENT_CHARS} characters"
        )
    label, separator, raw = value.partition("=")
    label = label.strip()
    raw = raw.strip()
    if not separator or not label or not raw:
        raise ValueError("expected LABEL=VALUE")
    try:
        parsed = value_type(raw)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"invalid {value_type.__name__}: {raw}") from exc
    if isinstance(parsed, (int, float)) and not isinstance(parsed, bool):
        _finite_number(parsed, field_name=label)
    return label, parsed


def _percentile(values: Sequence[float], percentile: float) -> float | None:
    """Return the historical nearest-rank percentile over bounded values."""

    percentile_value = _finite_number(percentile, field_name="percentile")
    if not 0.0 <= percentile_value <= 1.0:
        raise ValueError("percentile must be in [0, 1]")
    if len(values) > MAX_QUALIFICATION_SAMPLES:
        raise ViewerQualificationBoundsError(
            f"values exceeds {MAX_QUALIFICATION_SAMPLES} entries"
        )
    if not values:
        return None
    ordered = sorted(
        _finite_number(value, field_name="values entry") for value in values
    )
    index = max(
        0,
        min(
            len(ordered) - 1,
            math.ceil(len(ordered) * percentile_value) - 1,
        ),
    )
    return ordered[index]


def _numeric_counter_delta(
    first: Mapping[str, Any],
    last: Mapping[str, Any],
) -> dict[str, float]:
    """Subtract shared top-level finite numeric fields, excluding booleans."""

    delta: dict[str, float] = {}
    for key, end in last.items():
        start = first.get(key)
        if (
            isinstance(start, (int, float))
            and not isinstance(start, bool)
            and math.isfinite(float(start))
            and isinstance(end, (int, float))
            and not isinstance(end, bool)
            and math.isfinite(float(end))
        ):
            delta[str(key)] = float(end) - float(start)
    return delta


def _flatten_numeric_fields(
    value: Mapping[str, Any],
    *,
    prefix: str = "",
) -> dict[str, float]:
    """Flatten finite numeric observer leaves into bounded dotted paths."""

    flattened: dict[str, float] = {}

    def visit(mapping: Mapping[str, Any], path: str, depth: int) -> None:
        if depth > MAX_OBSERVER_NESTING_DEPTH:
            raise ViewerQualificationBoundsError(
                "observer metrics exceed "
                f"{MAX_OBSERVER_NESTING_DEPTH} nesting levels"
            )
        for key, item in mapping.items():
            name = f"{path}.{key}" if path else str(key)
            if isinstance(item, Mapping):
                visit(item, name, depth + 1)
                continue
            if (
                isinstance(item, (int, float))
                and not isinstance(item, bool)
                and math.isfinite(float(item))
            ):
                if name not in flattened and (
                    len(flattened) >= MAX_OBSERVER_NUMERIC_FIELDS
                ):
                    raise ViewerQualificationBoundsError(
                        "observer metrics exceed "
                        f"{MAX_OBSERVER_NUMERIC_FIELDS} numeric fields"
                    )
                flattened[name] = float(item)

    visit(value, prefix, 0)
    return flattened


def _observer_counter_delta(
    first: Mapping[str, Any],
    last: Mapping[str, Any],
) -> dict[str, float]:
    first_fields = _flatten_numeric_fields(first)
    last_fields = _flatten_numeric_fields(last)
    return {
        name: value - first_fields[name]
        for name, value in last_fields.items()
        if name in first_fields
    }


def validate_qualification_sample(
    payload: ViewerQualificationSampleV1 | Mapping[str, Any],
) -> ViewerQualificationSampleV1:
    """Validate one already-captured, effect-free sample value."""

    _artifact_type, _matrix_type, sample_type = _contract_types()
    if isinstance(payload, sample_type):
        return payload
    return sample_type.model_validate(payload)


def _validated_samples(
    samples: Sequence[ViewerQualificationSampleV1 | Mapping[str, Any]],
) -> list[ViewerQualificationSampleV1]:
    if isinstance(samples, (str, bytes)):
        raise TypeError("samples must be a sequence of sample values")
    if len(samples) > MAX_QUALIFICATION_SAMPLES:
        raise ViewerQualificationBoundsError(
            f"samples exceeds {MAX_QUALIFICATION_SAMPLES} entries"
        )
    return [validate_qualification_sample(sample) for sample in samples]


def _build_summary(
    samples: Sequence[ViewerQualificationSampleV1 | Mapping[str, Any]],
) -> dict[str, Any]:
    """Summarize transport-neutral metrics from normalized sample values.

    Client-window, producer-epoch, manifest and stage extraction used to be
    coupled to the deleted viewer-v1 status payload.  An I9 observer adapter
    may later add normalized client evidence to this summary; this helper does
    not infer it from arbitrary status dictionaries.
    """

    validated = _validated_samples(samples)
    viewer_latencies = [sample.viewer_route_latency_ms for sample in validated]
    kernel_latencies = [sample.kernel_route_latency_ms for sample in validated]
    process_labels = sorted(
        {label for sample in validated for label in sample.processes}
    )
    processes: dict[str, Any] = {}
    for label in process_labels:
        cpu_values: list[float] = []
        rss_values: list[int] = []
        for sample in validated:
            process = sample.processes.get(label)
            if not isinstance(process, Mapping):
                continue
            cpu = process.get("cpu_percent")
            if (
                isinstance(cpu, (int, float))
                and not isinstance(cpu, bool)
                and math.isfinite(float(cpu))
            ):
                cpu_values.append(float(cpu))
            rss = process.get("rss_bytes")
            if isinstance(rss, int) and not isinstance(rss, bool) and rss >= 0:
                rss_values.append(rss)
        processes[label] = {
            "cpu_percent": {
                "p50": _percentile(cpu_values, 0.50),
                "p95": _percentile(cpu_values, 0.95),
                "p99": _percentile(cpu_values, 0.99),
                "max": max(cpu_values, default=None),
            },
            "rss_bytes": {
                "max": max(rss_values, default=None),
                "last": rss_values[-1] if rss_values else None,
            },
        }

    first_viewer = validated[0].viewer_status if validated else {}
    last_viewer = validated[-1].viewer_status if validated else {}
    first_kernel = validated[0].kernel_metrics if validated else {}
    last_kernel = validated[-1].kernel_metrics if validated else {}
    kernel_state_samples: dict[str, int] = {}
    for sample in validated:
        state = sample.kernel_snapshot.get("state")
        if isinstance(state, str) and state:
            kernel_state_samples[state] = kernel_state_samples.get(state, 0) + 1

    observer_labels = sorted(
        {label for sample in validated for label in sample.observers}
    )
    observer_counter_deltas: dict[str, dict[str, float]] = {}
    for label in observer_labels:
        first_observer = next(
            (
                sample.observers[label]
                for sample in validated
                if label in sample.observers
            ),
            {},
        )
        last_observer = next(
            (
                sample.observers[label]
                for sample in reversed(validated)
                if label in sample.observers
            ),
            {},
        )
        observer_counter_deltas[label] = _observer_counter_delta(
            first_observer,
            last_observer,
        )

    return {
        "sample_count": len(validated),
        "sample_error_count": sum(bool(sample.errors) for sample in validated),
        "error_count": sum(len(sample.errors) for sample in validated),
        "viewer_route_latency_ms": {
            "p50": _percentile(viewer_latencies, 0.50),
            "p95": _percentile(viewer_latencies, 0.95),
            "p99": _percentile(viewer_latencies, 0.99),
            "max": max(viewer_latencies, default=None),
        },
        "kernel_route_latency_ms": {
            "p95": _percentile(kernel_latencies, 0.95),
            "max": max(kernel_latencies, default=None),
        },
        "viewer_counter_delta": _numeric_counter_delta(
            first_viewer,
            last_viewer,
        ),
        "kernel_counter_delta": _numeric_counter_delta(
            first_kernel,
            last_kernel,
        ),
        "kernel_state_samples": kernel_state_samples,
        "observer_counter_delta": observer_counter_deltas,
        "last_viewer_status": last_viewer,
        "last_kernel_metrics": last_kernel,
        "processes": processes,
    }


def _evaluate(
    summary: Mapping[str, Any],
    *,
    require_active_client: bool,
    require_no_client: bool,
    require_zero_builds: bool,
    client_count_min: int | None,
    client_count_max: int | None,
    active_client_count_min: int | None,
    max_viewer_route_p95_ms: float,
    max_process_cpu: Mapping[str, float],
    max_client_long_tasks: int | None,
    max_client_materialization_ms: float | None,
    max_client_react_commits_per_frame: float | None,
    max_client_unmatched_topology_rebuilds: int | None,
    max_build_failures: int,
    max_publication_sink_drops: int,
    required_layers: Sequence[str] | None = None,
    minimum_viewer_counter_deltas: Mapping[str, float] | None = None,
    minimum_kernel_counter_deltas: Mapping[str, float] | None = None,
    minimum_kernel_state_samples: Mapping[str, int] | None = None,
    minimum_client_totals: Mapping[str, float] | None = None,
    minimum_observer_counter_deltas: Mapping[str, Mapping[str, float]]
    | None = None,
) -> list[str]:
    """Apply value-only budgets to an already-normalized summary.

    Client-specific budgets deliberately fail for missing normalized evidence;
    this function never reverse-engineers the deleted viewer-v1 payload.
    """

    failures: list[str] = []
    last_status = summary.get("last_viewer_status", {})
    if not isinstance(last_status, Mapping):
        last_status = {}
    active_clients = int(last_status.get("active_client_count", 0))
    client_count = int(last_status.get("client_count", 0))
    if require_active_client and active_clients <= 0:
        failures.append("no active viewer client at the end of the capture")
    if require_no_client and client_count != 0:
        failures.append(f"viewer clients remained attached: {client_count}")
    if client_count_min is not None and client_count < client_count_min:
        failures.append(
            f"viewer client count {client_count} is below required {client_count_min}"
        )
    if client_count_max is not None and client_count > client_count_max:
        failures.append(
            f"viewer client count {client_count} exceeds allowed {client_count_max}"
        )
    if (
        active_client_count_min is not None
        and active_clients < active_client_count_min
    ):
        failures.append(
            "active viewer client count "
            f"{active_clients} is below required {active_client_count_min}"
        )

    layers = last_status.get("layers", [])
    observed_layers = (
        {str(layer) for layer in layers}
        if isinstance(layers, Sequence) and not isinstance(layers, (str, bytes))
        else set()
    )
    missing_layers = sorted(set(required_layers or ()) - observed_layers)
    if missing_layers:
        failures.append("missing required viewer layers: " + ", ".join(missing_layers))

    viewer_delta = summary.get("viewer_counter_delta", {})
    if not isinstance(viewer_delta, Mapping):
        viewer_delta = {}
    builds = viewer_delta.get("builds_started", 0.0)
    if require_zero_builds and builds != 0.0:
        failures.append(f"viewer builds advanced without demand: {builds:g}")

    for name, minimum in (minimum_viewer_counter_deltas or {}).items():
        observed = viewer_delta.get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no viewer counter-delta evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"viewer counter delta {name}={observed:g} is below required {minimum:g}"
            )

    kernel_delta = summary.get("kernel_counter_delta", {})
    if not isinstance(kernel_delta, Mapping):
        kernel_delta = {}
    for name, minimum in (minimum_kernel_counter_deltas or {}).items():
        observed = kernel_delta.get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no kernel counter-delta evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"kernel counter delta {name}={observed:g} is below required {minimum:g}"
            )

    kernel_states = summary.get("kernel_state_samples", {})
    if not isinstance(kernel_states, Mapping):
        kernel_states = {}
    for state, minimum in (minimum_kernel_state_samples or {}).items():
        observed = int(kernel_states.get(state, 0))
        if observed < minimum:
            failures.append(
                f"kernel state {state} sampled {observed} times; required {minimum}"
            )

    client_totals = summary.get("client_totals", {})
    if not isinstance(client_totals, Mapping):
        client_totals = {}
    for name, minimum in (minimum_client_totals or {}).items():
        observed = client_totals.get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no client workload evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"client total {name}={observed:g} is below required {minimum:g}"
            )

    observer_deltas = summary.get("observer_counter_delta", {})
    if not isinstance(observer_deltas, Mapping):
        observer_deltas = {}
    for observer, counters in (minimum_observer_counter_deltas or {}).items():
        observed_counters = observer_deltas.get(observer, {})
        if not isinstance(observed_counters, Mapping):
            observed_counters = {}
        for name, minimum in counters.items():
            observed = observed_counters.get(name)
            if not isinstance(observed, (int, float)) or isinstance(observed, bool):
                failures.append(
                    f"no observer counter-delta evidence for {observer}.{name}"
                )
            elif float(observed) < minimum:
                failures.append(
                    "observer counter delta "
                    f"{observer}.{name}={observed:g} is below required {minimum:g}"
                )

    viewer_latency = summary.get("viewer_route_latency_ms", {})
    p95 = viewer_latency.get("p95") if isinstance(viewer_latency, Mapping) else None
    if p95 is not None and p95 > max_viewer_route_p95_ms:
        failures.append(
            f"viewer observer p95 {p95:.3f} ms exceeds "
            f"{max_viewer_route_p95_ms:.3f} ms"
        )

    process_summary = summary.get("processes", {})
    if not isinstance(process_summary, Mapping):
        process_summary = {}
    for label, budget in max_process_cpu.items():
        process = process_summary.get(label, {})
        cpu = process.get("cpu_percent", {}) if isinstance(process, Mapping) else {}
        observed = cpu.get("p95") if isinstance(cpu, Mapping) else None
        if observed is None:
            failures.append(f"no process CPU evidence for {label}")
        elif observed > budget:
            failures.append(
                f"{label} CPU p95 {observed:.3f}% exceeds {budget:.3f}%"
            )

    if max_client_long_tasks is not None:
        observed = int(client_totals.get("long_tasks", 0))
        if observed > max_client_long_tasks:
            failures.append(
                f"client long tasks {observed} exceed {max_client_long_tasks}"
            )
    client_maxima = summary.get("client_maxima", {})
    if not isinstance(client_maxima, Mapping):
        client_maxima = {}
    if max_client_materialization_ms is not None:
        observed = client_maxima.get("materialization_max_ms")
        if observed is None:
            failures.append("no client materialization evidence")
        elif observed > max_client_materialization_ms:
            failures.append(
                "client materialization max "
                f"{observed:.3f} ms exceeds {max_client_materialization_ms:.3f} ms"
            )
    if max_client_react_commits_per_frame is not None:
        observed = summary.get("client_react_commits_per_frame")
        if observed is None:
            failures.append("no client frame/React-commit evidence")
        elif observed > max_client_react_commits_per_frame:
            failures.append(
                "client React commits/frame "
                f"{observed:.6f} exceeds {max_client_react_commits_per_frame:.6f}"
            )
    if max_client_unmatched_topology_rebuilds is not None:
        observed = summary.get("client_unmatched_topology_rebuilds")
        if observed is None:
            failures.append("no normalized client topology-rebuild evidence")
        elif int(observed) > max_client_unmatched_topology_rebuilds:
            failures.append(
                "client topology rebuilds without a matching manifest change "
                f"{int(observed)} exceed {max_client_unmatched_topology_rebuilds}"
            )

    build_failures = int(viewer_delta.get("build_failures", 0))
    if build_failures > max_build_failures:
        failures.append(
            f"viewer build failures {build_failures} exceed {max_build_failures}"
        )
    sink_drops = int(viewer_delta.get("publication_sink_drops", 0))
    if sink_drops > max_publication_sink_drops:
        failures.append(
            "viewer publication sink drops "
            f"{sink_drops} exceed {max_publication_sink_drops}"
        )
    return failures


def validate_qualification_artifact(
    payload: ViewerQualificationArtifactV1 | Mapping[str, Any],
) -> ViewerQualificationArtifactV1:
    """Validate schema and internal consistency without reading or writing."""

    artifact_type, _matrix_type, _sample_type = _contract_types()
    artifact = (
        payload
        if isinstance(payload, artifact_type)
        else artifact_type.model_validate(payload)
    )
    if len(artifact.samples) > MAX_QUALIFICATION_SAMPLES:
        raise ViewerQualificationBoundsError(
            f"artifact samples exceeds {MAX_QUALIFICATION_SAMPLES} entries"
        )
    if artifact.success != (not artifact.failures):
        raise ValueError("artifact success must equal not failures")
    summary_count = artifact.summary.get("sample_count")
    if summary_count is not None and summary_count != len(artifact.samples):
        raise ValueError("artifact summary sample_count does not match samples")
    indexes = [sample.index for sample in artifact.samples]
    if len(indexes) != len(set(indexes)):
        raise ValueError("artifact sample indexes must be unique")
    monotonic_values = [sample.monotonic_ns for sample in artifact.samples]
    if monotonic_values != sorted(monotonic_values):
        raise ValueError("artifact samples must be monotonic_ns ordered")
    return artifact


def validate_qualification_matrix(
    payload: ViewerQualificationMatrixV1 | Mapping[str, Any],
) -> ViewerQualificationMatrixV1:
    """Validate profile names and cross-references over an in-memory value."""

    _artifact_type, matrix_type, _sample_type = _contract_types()
    matrix = (
        payload
        if isinstance(payload, matrix_type)
        else matrix_type.model_validate(payload)
    )
    names = [scenario.name for scenario in matrix.scenarios]
    if len(names) != len(set(names)):
        raise ValueError("qualification scenario names must be unique")
    required_matrix = matrix.policy.get("required_matrix")
    if required_matrix is not None:
        if not isinstance(required_matrix, list) or not all(
            isinstance(name, str) for name in required_matrix
        ):
            raise ValueError("policy.required_matrix must be a list of names")
        if set(required_matrix) != set(names):
            raise ValueError(
                "policy.required_matrix must name every scenario exactly"
            )
    for scenario in matrix.scenarios:
        missing_observers = (
            set(scenario.requirements.observer_counter_delta_min)
            - set(scenario.observer_paths)
        )
        if missing_observers:
            raise ValueError(
                f"scenario {scenario.name!r} requires unknown observers: "
                + ", ".join(sorted(missing_observers))
            )
        if len(scenario.required_process_labels) != len(
            set(scenario.required_process_labels)
        ):
            raise ValueError(
                f"scenario {scenario.name!r} has duplicate process labels"
            )
    return matrix


def select_qualification_scenario(
    matrix: ViewerQualificationMatrixV1 | Mapping[str, Any],
    name: str,
) -> ViewerQualificationScenarioV1:
    """Select exactly one scenario from an already-provided matrix value."""

    if not isinstance(name, str) or not name:
        raise ValueError("scenario name must be non-empty")
    validated = validate_qualification_matrix(matrix)
    matches = [scenario for scenario in validated.scenarios if scenario.name == name]
    if len(matches) != 1:
        known = ", ".join(scenario.name for scenario in validated.scenarios)
        raise ValueError(f"unknown viewer qualification profile {name!r}; known: {known}")
    return matches[0]


def require_viewer_qualification(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Typed effect-free gate for callers that would start live collection."""

    _raise_viewer_qualification_unavailable()


def main(_argv: list[str] | None = None) -> int:
    print(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: historical viewer data-plane "
        "collection returns at I9/I",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "RUNTIME_UNAVAILABLE_REASON_CODE",
    "VIEWER_QUALIFICATION_RESTORATION_GATES",
    "ViewerQualificationBoundsError",
    "ViewerQualificationUnavailableError",
    "require_viewer_qualification",
    "select_qualification_scenario",
    "validate_qualification_artifact",
    "validate_qualification_matrix",
    "validate_qualification_sample",
]
