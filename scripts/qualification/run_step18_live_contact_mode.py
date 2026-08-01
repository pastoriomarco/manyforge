#!/usr/bin/env python3
"""Relocation gate for the historical Step 18 live contact-row producer.

The full host/container runner is pinned at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1. It actively inspected
Composer, controller, scene, viewer, and process state from the pre-cutover
product. Those observations must be recreated against the restored owners,
not kept as an unreachable legacy body.
"""

from __future__ import annotations

import hashlib
import json
import math
import sys
from numbers import Real
from typing import Any, NamedTuple


UNAVAILABLE_EXIT_CODE = 78
HISTORICAL_SOURCE = "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
HISTORICAL_SOURCE_BLOB = "48cbabff75bfba9a8a2a920cc2422df813fb4494"
HISTORICAL_TEST_BLOB = "8e79f5f0ad6e92e368500a59dbd2c83acf8913d1"
ROW_METADATA_SCHEMA = "manyforge.step18_live_row_run.v2"
ROW_KINDS = (
    "baseline_pristine_timing",
    "baseline_observation",
    "candidate_timing",
    "candidate_observation",
)
VIEWER_STRATA = ("disconnected", "focused")
SUPPORT_CONTACT_MODES = (
    "current_all_active_distance",
    "signed_penetration",
    "binary_contact",
    "bounded_penetration",
)
MAX_ROW_TEXT_BYTES = 128
MAX_SUCCESSFUL_OBSERVATIONS = 1_000_000
MAX_JSON_DEPTH = 32
MAX_JSON_NODES = 4_096
MAX_JSON_CONTAINER_ITEMS = 1_024
MAX_JSON_TEXT_BYTES = 65_536
MAX_JSON_DOCUMENT_BYTES = 1_048_576
MIN_JSON_INTEGER = -(1 << 63)
MAX_JSON_INTEGER = (1 << 63) - 1


class RestorationAssignment(NamedTuple):
    historical_case: str
    historical_source_lines: str
    restoration_gate: str
    required_behavior: str


RESTORATION_ASSIGNMENTS = (
    RestorationAssignment(
        "candidate_host_process_and_tool_bundle_identity",
        "historical source blob lines 66-242 and 338-467",
        "retained/offline-manifest-verifiers",
        "validate source, tool, and evidence bindings without inspecting live "
        "processes",
    ),
    RestorationAssignment(
        "composer_container_runtime_and_cycle_observation",
        "historical source blob lines 123-263 and 329-515",
        "I5/H0+I6/N+I8/P5",
        "consume client-neutral status and final Kernel facts from their headless "
        "owners",
    ),
    RestorationAssignment(
        "support_contact_scene_and_policy_application",
        "historical source blob lines 316-326 and 329-515",
        "I6/N+I8/P5",
        "address scene resources and apply Stop policy through the restored "
        "public contract",
    ),
    RestorationAssignment(
        "viewer_disconnected_and_focused_rows",
        "historical source blob lines 316-326 and 329-515",
        "I9/I",
        "observe the restored Isaac viewer strictly as a client-side stratum",
    ),
    RestorationAssignment(
        "isaac_controller_identity_progress_and_logs",
        "historical source blob lines 123-263 and 329-515",
        "I9/I",
        "bind evidence to the manager-owned Isaac controller and simulator lifecycle",
    ),
    RestorationAssignment(
        "live_contact_mode_execution_campaign",
        "historical source blob lines 279-515 and 517-569",
        "I8/P5+I9/I",
        "run all contact modes on one final Stop core and registered Isaac product",
    ),
    RestorationAssignment(
        "immutable_row_evidence_and_metadata",
        "retained source blob lines 60-64 and 316-326; file binding remains in "
        "the offline verifier; producer lines 272-308 and 329-515 removed",
        "retained/generate+verify_step18_live_contact_modes",
        "retain artifact hashing, schema validation, and closure aggregation offline",
    ),
)


def _bounded_text(value: object, *, label: str, limit: int) -> str:
    if type(value) is not str:
        raise TypeError(f"{label} must be an exact string")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} must be valid UTF-8 text") from exc
    if len(encoded) > limit:
        raise ValueError(f"{label} exceeds the {limit}-byte text bound")
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


def _validate_json_value(
    value: object, *, depth: int, remaining_nodes: list[int]
) -> None:
    remaining_nodes[0] -= 1
    if remaining_nodes[0] < 0:
        raise ValueError(
            f"JSON value exceeds the {MAX_JSON_NODES}-node bound"
        )
    if depth > MAX_JSON_DEPTH:
        raise ValueError(
            f"JSON value exceeds the {MAX_JSON_DEPTH}-level depth bound"
        )
    if value is None or type(value) is bool:
        return
    if type(value) is str:
        _bounded_text(value, label="JSON string", limit=MAX_JSON_TEXT_BYTES)
        return
    if type(value) is int:
        if not MIN_JSON_INTEGER <= value <= MAX_JSON_INTEGER:
            raise ValueError("JSON integer exceeds the signed 64-bit bound")
        return
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError("JSON numbers must be finite")
        return
    if type(value) is list:
        if len(value) > MAX_JSON_CONTAINER_ITEMS:
            raise ValueError(
                "JSON array exceeds the "
                f"{MAX_JSON_CONTAINER_ITEMS}-row bound"
            )
        for item in value:
            _validate_json_value(
                item,
                depth=depth + 1,
                remaining_nodes=remaining_nodes,
            )
        return
    if type(value) is dict:
        if len(value) > MAX_JSON_CONTAINER_ITEMS:
            raise ValueError(
                "JSON object exceeds the "
                f"{MAX_JSON_CONTAINER_ITEMS}-row bound"
            )
        for key, item in value.items():
            _bounded_text(
                key,
                label="JSON object key",
                limit=MAX_JSON_TEXT_BYTES,
            )
            _validate_json_value(
                item,
                depth=depth + 1,
                remaining_nodes=remaining_nodes,
            )
        return
    raise TypeError("canonical evidence must contain only exact JSON value types")


def canonical_sha256(value: Any) -> str:
    """Hash one bounded JSON value using the historical canonical projection."""

    _validate_json_value(value, depth=0, remaining_nodes=[MAX_JSON_NODES])
    encoder = json.JSONEncoder(
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    digest = hashlib.sha256()
    total_bytes = 0
    for chunk in encoder.iterencode(value):
        encoded = chunk.encode("utf-8")
        total_bytes += len(encoded)
        if total_bytes > MAX_JSON_DOCUMENT_BYTES:
            raise ValueError(
                "canonical JSON exceeds the "
                f"{MAX_JSON_DOCUMENT_BYTES}-byte document bound"
            )
        digest.update(encoded)
    return digest.hexdigest()


def validate_row_shape(
    *,
    row_kind: str,
    viewer_stratum: str,
    support_contact_mode: str,
    max_contact_penetration_m: float,
    successful_observations: int,
    operator_attests_viewer_focused: bool,
) -> dict[str, Any]:
    """Pure argument validation retained for future producer reconstruction."""

    row_kind = _bounded_text(
        row_kind, label="row kind", limit=MAX_ROW_TEXT_BYTES
    )
    viewer_stratum = _bounded_text(
        viewer_stratum, label="viewer stratum", limit=MAX_ROW_TEXT_BYTES
    )
    support_contact_mode = _bounded_text(
        support_contact_mode,
        label="support-contact mode",
        limit=MAX_ROW_TEXT_BYTES,
    )
    if row_kind not in ROW_KINDS:
        raise ValueError(f"unknown row kind: {row_kind}")
    if viewer_stratum not in VIEWER_STRATA:
        raise ValueError(f"unknown viewer stratum: {viewer_stratum}")
    if support_contact_mode not in SUPPORT_CONTACT_MODES:
        raise ValueError(
            f"unknown support-contact mode: {support_contact_mode}"
        )
    baseline_row = row_kind in {
        "baseline_pristine_timing",
        "baseline_observation",
    }
    if baseline_row and support_contact_mode != "current_all_active_distance":
        raise ValueError(
            "baseline rows require current_all_active_distance"
        )
    if not baseline_row and support_contact_mode == "current_all_active_distance":
        raise ValueError(
            "candidate rows cannot claim the historical baseline mode"
        )
    if type(successful_observations) is not int:
        raise TypeError("successful observations must be an exact integer")
    if not 30 <= successful_observations <= MAX_SUCCESSFUL_OBSERVATIONS:
        raise ValueError(
            "formal live rows require between 30 and "
            f"{MAX_SUCCESSFUL_OBSERVATIONS} observations"
        )
    max_contact_penetration = _finite_real(
        max_contact_penetration_m,
        label="maximum contact penetration",
    )
    if max_contact_penetration < 0.0:
        raise ValueError(
            "maximum contact penetration must be finite and non-negative"
        )
    if type(operator_attests_viewer_focused) is not bool:
        raise TypeError("operator attestation must be an exact bool")
    if viewer_stratum == "focused" and not operator_attests_viewer_focused:
        raise ValueError("focused rows require an operator attestation")
    return {
        "row_kind": row_kind,
        "viewer_stratum": viewer_stratum,
        "support_contact_mode": support_contact_mode,
        "max_contact_penetration_m": max_contact_penetration,
        "successful_observations": successful_observations,
        "controller_progress_observer_mode": (
            "quiescent_repeated"
            if row_kind == "baseline_pristine_timing"
            else "coherent_native"
        ),
    }


def unavailable_payload() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "script": "run_step18_live_contact_mode.py",
        "historical_source": HISTORICAL_SOURCE,
        "historical_row_schema": ROW_METADATA_SCHEMA,
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
