#!/usr/bin/env python3
"""Relocation gate and pure matrix reducer for Step 18 mechanism replay.

The producer at manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
configured and built legacy qualification binaries, then replayed captures
through the pre-cutover scene/Kernel composition. Only the owner-neutral row
matrix remains here; execution returns a typed restoration gate before any
filesystem, subprocess, container, or runtime effect.
"""

from __future__ import annotations

import json
import math
import sys
from numbers import Real
from typing import Any, NamedTuple, Sequence


UNAVAILABLE_EXIT_CODE = 78
HISTORICAL_SOURCE = "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
HISTORICAL_SOURCE_BLOB = "43fc8c7ed2ef1b51cb34744319f05da821d3689d"
HISTORICAL_TEST_BLOB = "8e79f5f0ad6e92e368500a59dbd2c83acf8913d1"
ROW_METADATA_SCHEMA = "manyforge.step18_live_row_run.v2"
BASELINE_LANE_SCHEMA = "manyforge.runtime_collision_baseline_lane.v2"
MODE_TO_ARM = {
    "signed_penetration": "signed_penetration_selective",
    "binary_contact": "binary_contact",
    "bounded_penetration": "bounded_penetration",
}
BASELINE_MODE = "current_all_active_distance"
OBSERVATION_ROW_KINDS = ("baseline_observation", "candidate_observation")
VIEWER_STRATA = ("disconnected", "focused")
MAX_OBSERVATION_ROWS = 8
MAX_ROW_FIELDS = 64
MAX_ROW_TEXT_BYTES = 128


class RestorationAssignment(NamedTuple):
    historical_case: str
    historical_source_lines: str
    restoration_gate: str
    required_behavior: str


RESTORATION_ASSIGNMENTS = (
    RestorationAssignment(
        "eight_row_capture_identity_and_matrix",
        "retained source blob lines 262-289; row/capture binding from lines "
        "203-260 remains in offline verifiers",
        "retained/generate+verify_step18_live_contact_modes",
        "retain exact row/capture bindings and baseline/candidate role projection",
    ),
    RestorationAssignment(
        "baseline_and_candidate_binary_binding",
        "historical source blob lines 109-201 and 333-492",
        "I6/N+I8/P5",
        "bind restored native collision/validation executables by source identity",
    ),
    RestorationAssignment(
        "candidate_build_and_container_replay",
        "historical source blob lines 75-201 and 291-492",
        "I6/N+I8/P5",
        "build and execute only supported resource-addressed qualification targets",
    ),
    RestorationAssignment(
        "isaac_live_capture_replay",
        "historical source blob lines 203-260 and 333-492",
        "I9/I",
        "consume captures produced by the registered Isaac manager-owned product",
    ),
    RestorationAssignment(
        "scene_policy_and_stop_mechanism_comparison",
        "historical source blob lines 262-492",
        "I8/P5+I9/I",
        "compare final Stop/policy mechanisms without reconstructing old scene owners",
    ),
    RestorationAssignment(
        "immutable_replay_manifest",
        "historical source blob lines 36-74 and 333-492; retained in the offline "
        "verifier",
        "retained/offline-manifest-verifiers",
        "retain exact manifest schema and artifact validation independently of "
        "execution",
    ),
)


def _bounded_text(value: object, *, label: str) -> str:
    if type(value) is not str:
        raise TypeError(f"{label} must be an exact string")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} must be valid UTF-8 text") from exc
    if len(encoded) > MAX_ROW_TEXT_BYTES:
        raise ValueError(
            f"{label} exceeds the {MAX_ROW_TEXT_BYTES}-byte text bound"
        )
    return value


def _finite_real(value: object, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise TypeError(f"{label} must be a real number, not bool")
    try:
        result = float(value)
    except (OverflowError, ValueError) as exc:
        raise ValueError(f"{label} must be finite") from exc
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _row_selector(row: object) -> tuple[str, str, str | None]:
    if type(row) is not dict:
        raise TypeError("observation row must be an exact object")
    if len(row) > MAX_ROW_FIELDS:
        raise ValueError(
            f"observation row exceeds the {MAX_ROW_FIELDS}-field bound"
        )
    for key in row:
        _bounded_text(key, label="observation row key")

    try:
        row_kind_value = row["row_kind"]
        mode_value = row["mode"]
    except KeyError as exc:
        raise ValueError(f"observation row is missing {exc.args[0]!r}") from exc
    row_kind = _bounded_text(row_kind_value, label="row kind")
    mode = _bounded_text(mode_value, label="support-contact mode")
    if row_kind not in OBSERVATION_ROW_KINDS:
        raise ValueError(f"unknown observation row kind: {row_kind}")
    if row_kind == "baseline_observation":
        if mode != BASELINE_MODE:
            raise ValueError("baseline observation rows require the baseline mode")
    elif mode not in MODE_TO_ARM:
        raise ValueError(f"unknown candidate support-contact mode: {mode}")

    stratum_value = row.get("viewer_stratum")
    if stratum_value is None:
        return row_kind, mode, None
    stratum = _bounded_text(stratum_value, label="viewer stratum")
    if stratum not in VIEWER_STRATA:
        raise ValueError(f"unknown viewer stratum: {stratum}")
    return row_kind, mode, stratum


def _execution_specs(
    row: dict[str, Any],
) -> list[tuple[str, str, str, str]]:
    """Project one captured row into the historical comparison-role matrix."""

    row_kind, mode, _ = _row_selector(row)

    baseline = [
        (
            "baseline_decision",
            "baseline_timing",
            "signed_penetration_selective",
            "baseline_pristine",
        ),
        (
            "baseline_observation",
            "baseline_observation",
            "signed_penetration_selective",
            "baseline_observation",
        ),
    ]
    if row_kind == "baseline_observation":
        return baseline
    arm = MODE_TO_ARM[mode]
    return baseline + [
        (
            "candidate_decision",
            "candidate_timing",
            arm,
            "current_candidate",
        ),
        (
            "candidate_observation",
            "candidate_observation",
            arm,
            "current_candidate",
        ),
    ]


def validate_observation_matrix(rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """Validate the exact, duplicate-free historical eight-row selector matrix."""

    if isinstance(rows, (str, bytes, bytearray)) or not isinstance(rows, Sequence):
        raise TypeError("observation rows must be a sequence")
    if len(rows) != MAX_OBSERVATION_ROWS:
        raise ValueError("exactly eight observation rows are required")

    observed: list[tuple[str, str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    for index, row in enumerate(rows):
        row_kind, mode, stratum = _row_selector(row)
        if stratum is None:
            raise ValueError(f"observation row {index} is missing 'viewer_stratum'")
        selector = (row_kind, mode, stratum)
        if selector in seen:
            raise ValueError(f"duplicate observation row selector at index {index}")
        seen.add(selector)
        observed.append(selector)

    expected = {
        ("baseline_observation", BASELINE_MODE, stratum)
        for stratum in VIEWER_STRATA
    } | {
        ("candidate_observation", mode, stratum)
        for mode in MODE_TO_ARM
        for stratum in VIEWER_STRATA
    }
    if seen != expected:
        raise ValueError("observation rows do not form the exact eight-row matrix")
    return {
        "observation_row_count": len(observed),
        "selectors": sorted(observed),
    }


def validate_replay_request(
    *,
    observation_row_count: int,
    bounded_penetration_m: float,
) -> dict[str, Any]:
    """Retain the pure cardinality and numeric contract for reconstruction."""

    if type(observation_row_count) is not int:
        raise TypeError("observation row count must be an exact integer")
    if observation_row_count != MAX_OBSERVATION_ROWS:
        raise ValueError("exactly eight observation row inputs are required")
    bounded_penetration = _finite_real(
        bounded_penetration_m,
        label="bounded penetration",
    )
    if bounded_penetration < 0.0:
        raise ValueError(
            "bounded penetration must be finite and non-negative"
        )
    return {
        "observation_row_count": observation_row_count,
        "bounded_penetration_m": bounded_penetration,
    }


def unavailable_payload() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "script": "run_step18_live_mechanism_replay.py",
        "historical_source": HISTORICAL_SOURCE,
        "historical_row_schema": ROW_METADATA_SCHEMA,
        "historical_baseline_lane_schema": BASELINE_LANE_SCHEMA,
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
