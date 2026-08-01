#!/usr/bin/env python3
"""Relocation gate and pure reducers for the historical RP5 offline producer.

The executable producer at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1 built the removed Python
runtime-model/factory composition and launched legacy benchmark binaries. The
corpus validator remains in roboplan_05_qualification.py; only source-neutral
evidence projection is retained here.
"""

from __future__ import annotations

import json
import math
import sys
from itertools import islice
from numbers import Real
from typing import Any, Iterable, NamedTuple


UNAVAILABLE_EXIT_CODE = 78
HISTORICAL_SOURCE = "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
HISTORICAL_SOURCE_BLOB = "1f07b69faea96950fbbdbc35a4d8e3e9913cc2b1"
HISTORICAL_TEST_BLOB = "0935aa3246f28dc7457706ae37db386759cf415d"
MAX_FIXTURE_ROWS = 4096
MAX_DETAIL_NODES = 65_536
MAX_DETAIL_DEPTH = 16
MAX_TEXT_CHARS = 4096
MAX_IDENTIFIER_CHARS = 256
_EXACT_BOOLEAN_DETAIL_KEYS = frozenset(
    {
        "decision_match",
        "enabled",
        "ordered_contact_summary_used",
        "support_contact_active",
        "support_contact_ordered_summary_verified",
    }
)
_NONNEGATIVE_INTEGER_DETAIL_KEYS = frozenset(
    {
        "allocation_bound_per_query",
        "allocations",
        "runs",
        "waypoints",
        "workers",
        "workers_requested",
    }
)


class RestorationAssignment(NamedTuple):
    historical_case: str
    historical_source_lines: str
    restoration_gate: str
    required_behavior: str


RESTORATION_ASSIGNMENTS = (
    RestorationAssignment(
        "corpus_capture_and_evidence_validation",
        "historical source blob lines 365-371; retained in the standalone validator",
        "retained/roboplan_05_qualification.py",
        "validate serialized RP5 artifacts without constructing a product runtime",
    ),
    RestorationAssignment(
        "runtime_model_view_and_kernel_fixture_construction",
        "historical source blob lines 154-300 and 365-398",
        "I6/N",
        "resolve addressed model/program/deployment products through the native owner",
    ),
    RestorationAssignment(
        "neutral_engine_matrix_and_trajectory_validation",
        "historical source blob lines 399-518",
        "I6/N+I8/P5",
        "run direct collision evidence and final ordered multi-domain validation",
    ),
    RestorationAssignment(
        "planner_policy_and_collision_attribution",
        "historical source blob lines 450-540",
        "I6/N",
        "observe native planner facts without a Python scene or factory authority",
    ),
    RestorationAssignment(
        "benchmark_binary_execution_and_evidence_publication",
        "historical source blob lines 51-77, 349-363, and 365-618",
        "I6/N+I8/P5",
        "qualify restored native targets in the supported container lane",
    ),
    RestorationAssignment(
        "source_bound_evidence_projection",
        "retained source blob lines 79-151; old envelope lines 303-347 and "
        "541-560 removed",
        "retained/current",
        "retain unit conversion and engine-record projection for captured artifacts",
    ),
)


def _latency_us_to_ns(value: dict[str, Any]) -> dict[str, Any]:
    """Project a captured latency distribution without running a benchmark."""

    if not isinstance(value, dict):
        raise ValueError("latency distribution must be an object")
    projected: dict[str, Any] = {}
    for key in ("p50", "p95", "p99", "max", "mad"):
        if key not in value:
            continue
        projected[key] = (
            _nonnegative_real(value[key], label=f"latency.{key}") * 1000.0
        )
    return projected


def _engine_matrix_records(raw: dict[str, Any]) -> list[dict[str, Any]]:
    """Retained source-neutral projection of an already-captured engine matrix."""

    if not isinstance(raw, dict):
        raise ValueError("engine matrix must be an object")
    fixtures = _bounded_items(raw.get("fixtures", ()), label="fixtures")
    records: list[dict[str, Any]] = []
    seen_fixture_names: set[str] = set()
    for index, fixture_value in enumerate(fixtures):
        fixture = _object(fixture_value, label=f"fixtures[{index}]")
        fixture_name = _bounded_text(
            fixture.get("fixture"),
            label=f"fixtures[{index}].fixture",
            limit=MAX_IDENTIFIER_CHARS,
        )
        fixture_passed = _exact_bool(
            fixture.get("passed"), label=f"fixtures[{index}].passed"
        )
        if fixture_name in seen_fixture_names:
            raise ValueError(f"duplicate fixture identifier: {fixture_name}")
        seen_fixture_names.add(fixture_name)
        fixture_id = f"neutral_engine/{fixture_name}"
        runtime = _optional_object(
            fixture,
            "runtime_query",
            label=f"fixtures[{index}].runtime_query",
        )
        revision = _optional_object(
            runtime,
            "revision_change",
            label=f"fixtures[{index}].runtime_query.revision_change",
        )
        revision_max_us = 0.0
        if revision:
            if "max_us" not in revision:
                raise ValueError(
                    f"fixtures[{index}].runtime_query.revision_change.max_us is required"
                )
            revision_max_us = _nonnegative_real(
                revision["max_us"],
                label=f"fixtures[{index}].runtime_query.revision_change.max_us",
            )
        if revision_max_us > 0.0:
            revision_latency = {
                key: _required_nonnegative_real(
                    revision,
                    f"{key}_us",
                    label=(
                        f"fixtures[{index}].runtime_query.revision_change.{key}_us"
                    ),
                )
                for key in ("p50", "p95", "p99", "max")
            }
            revision_details = _validated_revision_details(
                revision,
                label=f"fixtures[{index}].runtime_query.revision_change",
            )
            records.append(
                {
                    "record_type": "collision_revision",
                    "fixture_id": fixture_id,
                    "passed": fixture_passed,
                    "latency_ns": _latency_us_to_ns(revision_latency),
                    "details": revision_details,
                }
            )
        validation = _optional_object(
            fixture,
            "validation",
            label=f"fixtures[{index}].validation",
        )
        noncollision = _optional_object(
            fixture,
            "noncollision_validation",
            label=f"fixtures[{index}].noncollision_validation",
        )
        validation_enabled = _enabled_flag(
            validation, label=f"fixtures[{index}].validation"
        )
        noncollision_enabled = _enabled_flag(
            noncollision, label=f"fixtures[{index}].noncollision_validation"
        )
        if validation_enabled or noncollision_enabled:
            _validate_bounded_detail(
                validation, label=f"fixtures[{index}].validation"
            )
            _validate_bounded_detail(
                noncollision,
                label=f"fixtures[{index}].noncollision_validation",
            )
            decision_match = True
            validation_runs = 0
            if validation_enabled:
                decision_match = _exact_bool(
                    validation.get("decision_match"),
                    label=f"fixtures[{index}].validation.decision_match",
                )
                validation_runs = _required_nonnegative_int(
                    validation,
                    "runs",
                    label=f"fixtures[{index}].validation.runs",
                )
            elif "decision_match" in validation:
                _exact_bool(
                    validation["decision_match"],
                    label=f"fixtures[{index}].validation.decision_match",
                )
            noncollision_runs = 0
            if noncollision_enabled:
                noncollision_runs = _required_nonnegative_int(
                    noncollision,
                    "runs",
                    label=f"fixtures[{index}].noncollision_validation.runs",
                )
            support_verified = False
            if "support_contact_ordered_summary_verified" in fixture:
                support_verified = _exact_bool(
                    fixture["support_contact_ordered_summary_verified"],
                    label=(
                        f"fixtures[{index}].support_contact_ordered_summary_verified"
                    ),
                )
            details = {
                "collision_validation": validation,
                "noncollision_validation": noncollision,
                "support_contact_ordered_summary_verified": support_verified,
            }
            record: dict[str, Any] = {
                "record_type": "trajectory_validation",
                "fixture_id": fixture_id,
                "passed": fixture_passed and decision_match,
                "sample_count": max(validation_runs, noncollision_runs),
                "details": details,
            }
            noncollision_max_us = 0.0
            if "max_us" in noncollision:
                noncollision_max_us = _nonnegative_real(
                    noncollision["max_us"],
                    label=f"fixtures[{index}].noncollision_validation.max_us",
                )
            if noncollision_max_us > 0.0:
                record["latency_ns"] = _latency_us_to_ns(
                    {
                        key: _required_nonnegative_real(
                            noncollision,
                            f"{key}_us",
                            label=(
                                f"fixtures[{index}].noncollision_validation.{key}_us"
                            ),
                        )
                        for key in ("p50", "p95", "p99", "max")
                    }
                )
            records.append(record)
    return records


def _bounded_items(values: Iterable[Any], *, label: str) -> tuple[Any, ...]:
    if isinstance(values, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be an array")
    try:
        items = tuple(islice(iter(values), MAX_FIXTURE_ROWS + 1))
    except TypeError as exc:
        raise ValueError(f"{label} must be an array") from exc
    if len(items) > MAX_FIXTURE_ROWS:
        raise ValueError(f"{label} exceeds the {MAX_FIXTURE_ROWS}-row bound")
    return items


def _object(value: Any, *, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def _optional_object(
    parent: dict[str, Any], key: str, *, label: str
) -> dict[str, Any]:
    if key not in parent:
        return {}
    return _object(parent[key], label=label)


def _bounded_text(value: Any, *, label: str, limit: int) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be a non-empty string")
    if len(value) > limit:
        raise ValueError(f"{label} exceeds the {limit}-character bound")
    return value


def _exact_bool(value: Any, *, label: str) -> bool:
    if type(value) is not bool:
        raise ValueError(f"{label} must be a boolean")
    return value


def _nonnegative_real(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a real number")
    result = float(value)
    if not math.isfinite(result) or result < 0.0:
        raise ValueError(f"{label} must be finite and non-negative")
    return result


def _required_nonnegative_real(
    parent: dict[str, Any], key: str, *, label: str
) -> float:
    if key not in parent:
        raise ValueError(f"{label} is required")
    return _nonnegative_real(parent[key], label=label)


def _required_nonnegative_int(
    parent: dict[str, Any], key: str, *, label: str
) -> int:
    if key not in parent:
        raise ValueError(f"{label} is required")
    value = parent[key]
    if type(value) is not int or value < 0:
        raise ValueError(f"{label} must be a non-negative integer")
    return value


def _enabled_flag(value: dict[str, Any], *, label: str) -> bool:
    if not value:
        return False
    if "enabled" not in value:
        raise ValueError(f"{label}.enabled is required")
    return _exact_bool(value["enabled"], label=f"{label}.enabled")


def _validated_revision_details(
    revision: dict[str, Any], *, label: str
) -> dict[str, Any]:
    details: dict[str, Any] = {}
    for key in (
        "allocations",
        "allocation_bound_per_query",
        "wcet_budget_ns",
    ):
        if key in revision:
            details[key] = _required_nonnegative_int(
                revision, key, label=f"{label}.{key}"
            )
    if "allocations_per_query" in revision:
        details["allocations_per_query"] = _nonnegative_real(
            revision["allocations_per_query"],
            label=f"{label}.allocations_per_query",
        )
    return details


def _validate_bounded_detail(value: Any, *, label: str) -> None:
    node_count = 0
    active_containers: set[int] = set()

    def visit(
        item: Any, *, path: str, depth: int, field_name: str | None = None
    ) -> None:
        nonlocal node_count
        node_count += 1
        if node_count > MAX_DETAIL_NODES:
            raise ValueError(f"{label} exceeds the {MAX_DETAIL_NODES}-node bound")
        if depth > MAX_DETAIL_DEPTH:
            raise ValueError(f"{path} exceeds the {MAX_DETAIL_DEPTH}-level bound")
        if item is None:
            return
        if field_name in _EXACT_BOOLEAN_DETAIL_KEYS:
            if type(item) is not bool:
                raise ValueError(f"{path} must be a boolean")
            return
        if type(item) is bool:
            return
        if isinstance(item, Real):
            numeric = float(item)
            if not math.isfinite(numeric):
                raise ValueError(f"{path} must be finite")
            if field_name in _NONNEGATIVE_INTEGER_DETAIL_KEYS:
                if type(item) is not int or item < 0:
                    raise ValueError(f"{path} must be a non-negative integer")
            elif field_name is not None and (
                field_name.endswith(("_us", "_ns", "_count"))
                or "allocation" in field_name
            ) and numeric < 0.0:
                raise ValueError(f"{path} must be non-negative")
            return
        if isinstance(item, str):
            if len(item) > MAX_TEXT_CHARS:
                raise ValueError(
                    f"{path} exceeds the {MAX_TEXT_CHARS}-character bound"
                )
            return
        if not isinstance(item, (dict, list, tuple)):
            raise ValueError(f"{path} is not JSON-compatible evidence")
        identity = id(item)
        if identity in active_containers:
            raise ValueError(f"{path} contains a recursive container")
        active_containers.add(identity)
        try:
            if isinstance(item, dict):
                if len(item) > MAX_FIXTURE_ROWS:
                    raise ValueError(
                        f"{path} exceeds the {MAX_FIXTURE_ROWS}-entry bound"
                    )
                for key, child in item.items():
                    if not isinstance(key, str) or len(key) > MAX_IDENTIFIER_CHARS:
                        raise ValueError(f"{path} contains an invalid key")
                    visit(
                        child,
                        path=f"{path}.{key}",
                        depth=depth + 1,
                        field_name=key,
                    )
            else:
                if len(item) > MAX_FIXTURE_ROWS:
                    raise ValueError(
                        f"{path} exceeds the {MAX_FIXTURE_ROWS}-row bound"
                    )
                for index, child in enumerate(item):
                    visit(
                        child,
                        path=f"{path}[{index}]",
                        depth=depth + 1,
                    )
        finally:
            active_containers.remove(identity)

    visit(value, path=label, depth=0)


def unavailable_payload() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "script": "run_roboplan_05_offline_baseline.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
    }


def main(argv: list[str] | None = None) -> int:
    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
