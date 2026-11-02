#!/usr/bin/env python3
"""Validate the E1 external-delta ledger and its local patch identities."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Final


SCHEMA: Final = "manyforge.composable_cell.e1.external_delta_ledger.v1"
SELECTED_OUTCOME_SCHEMA: Final = (
    "manyforge.composable_cell.e1.selected_outcome_qualification.v1"
)
ALLOWED_DECISIONS: Final = {"undecided", "adopt_roboplan_0_6_0", "no_go_roboplan_0_6_0"}
ALLOWED_DISPOSITIONS: Final = {
    "delete_upstream_equivalent",
    "move_to_manyforge_boundary",
    "carry_minimal_patch",
    "submit_upstream_and_carry_until_released",
    "defer_to_owner_gate",
}
ALLOWED_STATES: Final = {"evidence_pending", "frozen"}
REQUIRED_LICENSE_SOURCE_IDS: Final = {
    "roboplan_0_5_1_control",
    "roboplan_0_6_0_candidate",
    "proxsuite_0_7_3_candidate_dependency",
    "proxsuite_cmake_module_candidate_dependency",
    "toppra_0_6_8_candidate_dependency",
}
REQUIRED_SELECTED_LANES: Final = {
    "native_amd64",
    "native_aarch64",
    "ros_isaac_amd64",
    "ros_isaac_aarch64",
}
REQUIRED_SOURCE_CLOSURES: Final = {
    "roboplan_0_5_1_control_closure",
    "roboplan_0_6_0_candidate_closure",
}
ALLOWED_QUALIFICATION_STATES: Final = {
    "pending_decision",
    "qualification_in_progress",
    "qualified",
}
ALLOWED_LANE_STATES: Final = {"pending_selected_outcome_replay", "passed"}
HEX40: Final = re.compile(r"^[0-9a-f]{40}$")
HEX64: Final = re.compile(r"^[0-9a-f]{64}$")
SHA256_IDENTITY: Final = re.compile(r"^sha256:[0-9a-f]{64}$")
REQUIRED_CAPABILITY_FIELDS: Final = {
    "id",
    "portable_semantic_owner",
    "adapter_responsibility",
    "planner_prerequisite",
    "restoration_gate",
    "acceptance_evidence",
}
REQUIRED_DELTA_FIELDS: Final = {
    "id",
    "dependency",
    "source_identity_refs",
    "local_sources",
    "guarantee",
    "consuming_surfaces",
    "owner_gate",
    "acceptance_evidence",
    "rollback_source",
    "removal_condition",
    "disposition",
}
REQUIRED_DOSSIER_FIELDS: Final = {
    "exact_upstream_source",
    "dependency_closure",
    "minimal_reproduction",
    "impact",
    "smallest_patch",
    "focused_regression",
    "sanitizer_evidence",
    "compatibility_risk",
    "intended_upstream_contract",
    "removal_predicate",
}


class LedgerError(ValueError):
    """The E1 ledger is structurally or cryptographically invalid."""


def _object(value: object, location: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise LedgerError(f"{location} must be an object")
    return value


def _list(value: object, location: str) -> list[object]:
    if not isinstance(value, list):
        raise LedgerError(f"{location} must be an array")
    return value


def _text(value: object, location: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise LedgerError(f"{location} must be a non-empty string")
    return value


def _text_list(value: object, location: str) -> list[str]:
    rows = _list(value, location)
    if not rows:
        raise LedgerError(f"{location} must not be empty")
    return [_text(row, f"{location}[{index}]") for index, row in enumerate(rows)]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _git_bytes(repository_root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = (
            (result.stderr or result.stdout).decode("utf-8", errors="replace").strip()
        )
        raise LedgerError(f"git {' '.join(arguments)} failed: {detail}")
    return result.stdout


def _validate_source_identities(ledger: dict[str, object]) -> set[str]:
    sources = _list(ledger.get("frozen_sources"), "frozen_sources")
    if not sources:
        raise LedgerError("frozen_sources must not be empty")
    source_ids: set[str] = set()
    for index, raw in enumerate(sources):
        location = f"frozen_sources[{index}]"
        source = _object(raw, location)
        source_id = _text(source.get("id"), f"{location}.id")
        if source_id in source_ids:
            raise LedgerError(f"duplicate frozen source id: {source_id}")
        source_ids.add(source_id)
        identity = _object(source.get("identity"), f"{location}.identity")
        for key, value in identity.items():
            if (
                key.endswith("commit")
                or key.endswith("tree")
                or key.endswith("tag_object")
            ):
                if not isinstance(value, str) or not HEX40.fullmatch(value):
                    raise LedgerError(
                        f"{location}.identity.{key} must be a lowercase Git object id"
                    )
            if key.endswith("sha256"):
                if not isinstance(value, str) or not HEX64.fullmatch(value):
                    raise LedgerError(
                        f"{location}.identity.{key} must be a lowercase SHA-256"
                    )
        if source_id in REQUIRED_LICENSE_SOURCE_IDS:
            source_url = _text(source.get("source_url"), f"{location}.source_url")
            if not source_url.startswith("https://"):
                raise LedgerError(f"{location}.source_url must be an HTTPS source URL")
            license_record = _object(source.get("license"), f"{location}.license")
            _text(license_record.get("path"), f"{location}.license.path")
            _text(license_record.get("spdx"), f"{location}.license.spdx")
            license_sha = license_record.get("sha256")
            if not isinstance(license_sha, str) or not HEX64.fullmatch(license_sha):
                raise LedgerError(
                    f"{location}.license.sha256 must be a lowercase SHA-256"
                )
            if not isinstance(identity.get("git_archive_sha256"), str):
                raise LedgerError(f"{location}.identity.git_archive_sha256 is required")
        _text(source.get("role"), f"{location}.role")
    return source_ids


def _validate_decision_record(ledger: dict[str, object]) -> None:
    decision = ledger.get("roboplan_decision")
    raw_record = ledger.get("roboplan_decision_record")
    if decision == "undecided":
        if raw_record is not None:
            raise LedgerError("an undecided RoboPlan outcome must not have a decision record")
        return

    record = _object(raw_record, "roboplan_decision_record")
    if record.get("decision") != decision:
        raise LedgerError("roboplan_decision_record disagrees with roboplan_decision")
    _text(record.get("authority"), "roboplan_decision_record.authority")
    date = _text(record.get("date"), "roboplan_decision_record.date")
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", date):
        raise LedgerError("roboplan_decision_record.date must be YYYY-MM-DD")
    _text(record.get("rationale"), "roboplan_decision_record.rationale")
    _text_list(
        record.get("accepted_tradeoffs"),
        "roboplan_decision_record.accepted_tradeoffs",
    )
    expected_sources = {
        "adopt_roboplan_0_6_0": (
            "roboplan_0_6_0_candidate",
            "roboplan_0_5_1_control",
        ),
        "no_go_roboplan_0_6_0": (
            "roboplan_0_5_1_control",
            "roboplan_0_6_0_candidate",
        ),
    }
    expected_selected, expected_rejected = expected_sources[str(decision)]
    if record.get("selected_source_id") != expected_selected:
        raise LedgerError("decision record names the wrong selected source")
    if record.get("rejected_source_id") != expected_rejected:
        raise LedgerError("decision record names the wrong rejected source")


def _validate_selected_outcome_qualification(
    ledger: dict[str, object],
    *,
    require_frozen: bool,
    repository_root: Path,
) -> None:
    qualification = _object(
        ledger.get("selected_outcome_qualification"),
        "selected_outcome_qualification",
    )
    state = qualification.get("state")
    if state not in ALLOWED_QUALIFICATION_STATES:
        raise LedgerError(
            "selected_outcome_qualification.state must be pending_decision, "
            "qualification_in_progress, or qualified"
        )
    _text(
        qualification.get("selection_rule"),
        "selected_outcome_qualification.selection_rule",
    )
    lanes = _list(
        qualification.get("required_lanes"),
        "selected_outcome_qualification.required_lanes",
    )
    observed_ids: set[str] = set()
    for index, raw in enumerate(lanes):
        location = f"selected_outcome_qualification.required_lanes[{index}]"
        lane = _object(raw, location)
        lane_id = _text(lane.get("id"), f"{location}.id")
        if lane_id in observed_ids:
            raise LedgerError(f"duplicate selected-outcome lane: {lane_id}")
        observed_ids.add(lane_id)
        lane_state = lane.get("status")
        if lane_state not in ALLOWED_LANE_STATES:
            raise LedgerError(f"{location}.status is unsupported")
        raw_evidence = _list(lane.get("evidence"), f"{location}.evidence")
        if lane_state == "passed":
            _text_list(raw_evidence, f"{location}.evidence")
        elif raw_evidence:
            raise LedgerError(f"{location}.evidence must be empty while pending")
    if observed_ids != REQUIRED_SELECTED_LANES:
        raise LedgerError("selected-outcome lane inventory is incomplete")

    metadata = _object(
        qualification.get("python_install_metadata"),
        "selected_outcome_qualification.python_install_metadata",
    )
    metadata_state = metadata.get("status")
    if metadata_state not in ALLOWED_LANE_STATES:
        raise LedgerError("selected-outcome Python metadata status is unsupported")
    metadata_evidence = _list(
        metadata.get("evidence"),
        "selected_outcome_qualification.python_install_metadata.evidence",
    )
    if metadata_state == "passed":
        _text_list(
            metadata_evidence,
            "selected_outcome_qualification.python_install_metadata.evidence",
        )
    elif metadata_evidence:
        raise LedgerError(
            "selected-outcome Python metadata evidence must be empty while pending"
        )

    decision = ledger.get("roboplan_decision")
    selected_source_id = qualification.get("selected_source_id")
    if state == "pending_decision":
        if decision != "undecided":
            raise LedgerError(
                "a decided RoboPlan outcome cannot remain pending_decision"
            )
        if selected_source_id is not None:
            raise LedgerError(
                "pending selected outcome must not name a selected source"
            )
    else:
        expected_source = {
            "adopt_roboplan_0_6_0": "roboplan_0_6_0_candidate",
            "no_go_roboplan_0_6_0": "roboplan_0_5_1_control",
        }.get(decision)
        if selected_source_id != expected_source:
            raise LedgerError(
                "selected source disagrees with the RoboPlan decision"
            )
    all_lanes_passed = all(
        lane.get("status") == "passed" for lane in lanes if isinstance(lane, dict)
    )
    if state == "qualified" and (not all_lanes_passed or metadata_state != "passed"):
        raise LedgerError("qualified selected outcome contains incomplete evidence")
    if state == "qualified":
        _validate_selected_outcome_result(
            ledger,
            qualification,
            repository_root=repository_root,
        )
    if require_frozen and state != "qualified":
        raise LedgerError("selected-outcome qualification remains incomplete")


def _validate_selected_outcome_result(
    ledger: dict[str, object],
    qualification: dict[str, object],
    *,
    repository_root: Path,
) -> None:
    """Bind the compact final replay record without retaining disposable logs."""

    reference = _object(
        qualification.get("result_record"),
        "selected_outcome_qualification.result_record",
    )
    path_text = _text(
        reference.get("path"),
        "selected_outcome_qualification.result_record.path",
    )
    expected_sha = reference.get("sha256")
    if not isinstance(expected_sha, str) or not HEX64.fullmatch(expected_sha):
        raise LedgerError(
            "selected_outcome_qualification.result_record.sha256 must be a "
            "lowercase SHA-256"
        )
    relative_path = Path(path_text)
    repository_root = repository_root.resolve()
    result_path = (repository_root / relative_path).resolve()
    if relative_path.is_absolute() or not result_path.is_relative_to(repository_root):
        raise LedgerError("selected-outcome result path must remain inside the repository")
    if not result_path.is_file():
        raise LedgerError(f"selected-outcome result does not exist: {path_text}")
    observed_sha = _sha256(result_path)
    if observed_sha != expected_sha:
        raise LedgerError(
            "selected-outcome result digest mismatch: "
            f"expected {expected_sha}, observed {observed_sha}"
        )
    try:
        result = _object(
            json.loads(result_path.read_text(encoding="utf-8")),
            "selected-outcome result",
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LedgerError(f"selected-outcome result is not valid JSON: {exc}") from exc
    if result.get("schema") != SELECTED_OUTCOME_SCHEMA:
        raise LedgerError(
            f"selected-outcome result schema must be {SELECTED_OUTCOME_SCHEMA}"
        )
    if result.get("status") != "passed":
        raise LedgerError("selected-outcome result status must be passed")
    if result.get("decision") != ledger.get("roboplan_decision"):
        raise LedgerError("selected-outcome result disagrees with the decision")
    if result.get("selected_source_id") != qualification.get("selected_source_id"):
        raise LedgerError("selected-outcome result names the wrong selected source")
    if result.get("raw_logs_committed") is not False:
        raise LedgerError("selected-outcome result must keep raw logs outside Git")

    expected_platforms = {
        "native_amd64": "linux/amd64",
        "native_aarch64": "linux/arm64",
        "ros_isaac_amd64": "linux/amd64",
        "ros_isaac_aarch64": "linux/arm64",
    }
    required_checks = {
        "image_build",
        "pip_check",
        "dependency_stack",
        "single_recorded_planner_abi",
    }
    result_lanes = _list(result.get("lanes"), "selected-outcome result.lanes")
    observed_lanes: set[str] = set()
    for index, raw_lane in enumerate(result_lanes):
        location = f"selected-outcome result.lanes[{index}]"
        lane = _object(raw_lane, location)
        lane_id = _text(lane.get("id"), f"{location}.id")
        if lane_id in observed_lanes:
            raise LedgerError(f"duplicate selected-outcome result lane: {lane_id}")
        observed_lanes.add(lane_id)
        if lane.get("platform") != expected_platforms.get(lane_id):
            raise LedgerError(f"{location}.platform is incorrect")
        image = _object(lane.get("image"), f"{location}.image")
        _text(image.get("tag"), f"{location}.image.tag")
        for field in ("descriptor_digest", "config_digest"):
            value = image.get(field)
            if not isinstance(value, str) or not SHA256_IDENTITY.fullmatch(value):
                raise LedgerError(f"{location}.image.{field} is not a SHA-256 identity")
        size_bytes = image.get("size_bytes")
        if type(size_bytes) is not int or size_bytes <= 0:
            raise LedgerError(f"{location}.image.size_bytes must be positive")
        checks = set(_text_list(lane.get("checks"), f"{location}.checks"))
        if not required_checks.issubset(checks):
            raise LedgerError(f"{location}.checks is incomplete")
    if observed_lanes != REQUIRED_SELECTED_LANES:
        raise LedgerError("selected-outcome result lane inventory is incomplete")
    _text_list(result.get("nonclaims"), "selected-outcome result.nonclaims")


def _validate_source_closures(ledger: dict[str, object], source_ids: set[str]) -> None:
    source_records = {
        str(source["id"]): source
        for source in _list(ledger.get("frozen_sources"), "frozen_sources")
        if isinstance(source, dict) and isinstance(source.get("id"), str)
    }
    closures = _list(ledger.get("planner_source_closures"), "planner_source_closures")
    observed_ids: set[str] = set()
    for index, raw in enumerate(closures):
        location = f"planner_source_closures[{index}]"
        closure = _object(raw, location)
        closure_id = _text(closure.get("id"), f"{location}.id")
        if closure_id in observed_ids:
            raise LedgerError(f"duplicate planner source closure: {closure_id}")
        observed_ids.add(closure_id)
        base_image = closure.get("base_image_sha256")
        identity = closure.get("identity_sha256")
        if not isinstance(base_image, str) or not HEX64.fullmatch(base_image):
            raise LedgerError(
                f"{location}.base_image_sha256 must be a lowercase SHA-256"
            )
        if not isinstance(identity, str) or not HEX64.fullmatch(identity):
            raise LedgerError(f"{location}.identity_sha256 must be a lowercase SHA-256")
        refs = _text_list(
            closure.get("source_identity_refs"),
            f"{location}.source_identity_refs",
        )
        if len(refs) != len(set(refs)):
            raise LedgerError(f"{location}.source_identity_refs contains duplicates")
        unknown = set(refs) - source_ids
        if unknown:
            raise LedgerError(
                f"{location} references unknown sources: {sorted(unknown)}"
            )
        payload = {
            "base_image_sha256": base_image,
            "sources": [
                {
                    key: source_records[source_id][key]
                    for key in ("id", "identity", "source_url", "license")
                }
                for source_id in refs
            ],
        }
        observed_identity = hashlib.sha256(
            json.dumps(
                payload,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            ).encode("utf-8")
        ).hexdigest()
        if identity != observed_identity:
            raise LedgerError(
                f"{location}.identity_sha256 mismatch: expected {identity}, "
                f"observed {observed_identity}"
            )
    if observed_ids != REQUIRED_SOURCE_CLOSURES:
        raise LedgerError("planner source closure inventory is incomplete")


def _validate_capabilities(ledger: dict[str, object]) -> None:
    capabilities = _list(
        ledger.get("planning_capability_responsibility"),
        "planning_capability_responsibility",
    )
    if not capabilities:
        raise LedgerError("planning_capability_responsibility must not be empty")
    ids: set[str] = set()
    for index, raw in enumerate(capabilities):
        location = f"planning_capability_responsibility[{index}]"
        capability = _object(raw, location)
        missing = REQUIRED_CAPABILITY_FIELDS - capability.keys()
        if missing:
            raise LedgerError(f"{location} is missing: {', '.join(sorted(missing))}")
        capability_id = _text(capability["id"], f"{location}.id")
        if capability_id in ids:
            raise LedgerError(f"duplicate planning capability id: {capability_id}")
        ids.add(capability_id)
        for field in (
            "portable_semantic_owner",
            "adapter_responsibility",
            "planner_prerequisite",
            "restoration_gate",
        ):
            _text(capability[field], f"{location}.{field}")
        _text_list(capability["acceptance_evidence"], f"{location}.acceptance_evidence")


def _validate_local_source(
    raw: object,
    location: str,
    *,
    repository_root: Path,
) -> None:
    source = _object(raw, location)
    kind = _text(source.get("kind"), f"{location}.kind")
    path_text = _text(source.get("path"), f"{location}.path")
    sha256 = source.get("sha256")
    if not isinstance(sha256, str) or not HEX64.fullmatch(sha256):
        raise LedgerError(f"{location}.sha256 must be a lowercase SHA-256")
    if kind == "repository_path":
        path = repository_root / path_text
        if not path.is_file():
            raise LedgerError(f"{location} does not exist: {path_text}")
        observed = _sha256(path)
        if observed != sha256:
            raise LedgerError(
                f"{location} digest mismatch for {path_text}: expected {sha256}, observed {observed}"
            )
    elif kind == "historical_git_path":
        commit = source.get("repository_commit")
        blob = source.get("blob")
        if not isinstance(commit, str) or not HEX40.fullmatch(commit):
            raise LedgerError(f"{location}.repository_commit must be a Git commit id")
        if not isinstance(blob, str) or not HEX40.fullmatch(blob):
            raise LedgerError(f"{location}.blob must be a Git blob id")
        observed_blob = (
            _git_bytes(repository_root, "rev-parse", f"{commit}:{path_text}")
            .decode("ascii")
            .strip()
        )
        if observed_blob != blob:
            raise LedgerError(
                f"{location}.blob mismatch for historical path {path_text}: "
                f"expected {blob}, observed {observed_blob}"
            )
        observed_content = _git_bytes(repository_root, "cat-file", "blob", blob)
        observed_sha256 = hashlib.sha256(observed_content).hexdigest()
        if observed_sha256 != sha256:
            raise LedgerError(
                f"{location} digest mismatch for historical path {path_text}: "
                f"expected {sha256}, observed {observed_sha256}"
            )
    elif kind != "external_git_range":
        raise LedgerError(f"{location}.kind is unsupported: {kind}")


def _validate_disposition(
    raw: object,
    location: str,
    *,
    require_frozen: bool,
    repository_root: Path,
) -> None:
    disposition = _object(raw, location)
    state = disposition.get("state")
    if state not in ALLOWED_STATES:
        raise LedgerError(f"{location}.state must be one of {sorted(ALLOWED_STATES)}")
    if state == "frozen":
        selection = disposition.get("selection")
        if selection not in ALLOWED_DISPOSITIONS:
            raise LedgerError(
                f"{location}.selection must be one of {sorted(ALLOWED_DISPOSITIONS)}"
            )
        _text(disposition.get("rationale"), f"{location}.rationale")
        if selection in {
            "carry_minimal_patch",
            "submit_upstream_and_carry_until_released",
        }:
            dossier_path = _text(
                disposition.get("upstream_dossier"), f"{location}.upstream_dossier"
            )
            dossier_file = repository_root / dossier_path
            if require_frozen and not dossier_file.is_file():
                raise LedgerError(f"{location} dossier does not exist: {dossier_path}")
            dossier_fields = set(
                _text_list(
                    disposition.get("dossier_fields"),
                    f"{location}.dossier_fields",
                )
            )
            missing = REQUIRED_DOSSIER_FIELDS - dossier_fields
            if missing:
                raise LedgerError(
                    f"{location}.dossier_fields is missing: {', '.join(sorted(missing))}"
                )
    else:
        candidates = set(
            _text_list(disposition.get("candidates"), f"{location}.candidates")
        )
        unknown = candidates - ALLOWED_DISPOSITIONS
        if unknown:
            raise LedgerError(
                f"{location}.candidates contains unsupported values: {sorted(unknown)}"
            )
        _text(disposition.get("decision_gate"), f"{location}.decision_gate")
        if require_frozen:
            raise LedgerError(f"{location} remains evidence_pending")


def _validate_builder_consumer_inventory(
    delta: dict[str, object], location: str
) -> None:
    if delta.get("id") != "xarm_fork_builder_surface":
        return
    inventory = _object(
        delta.get("consumer_inventory"), f"{location}.consumer_inventory"
    )
    single = _text_list(
        inventory.get("MoveItConfigsBuilder"),
        f"{location}.consumer_inventory.MoveItConfigsBuilder",
    )
    dual = _text_list(
        inventory.get("DualMoveItConfigsBuilder"),
        f"{location}.consumer_inventory.DualMoveItConfigsBuilder",
    )
    if len(single) != 16 or len(dual) != 7:
        raise LedgerError(
            f"{location}.consumer_inventory must contain exactly 16 single and 7 dual consumers"
        )
    combined = sorted([*single, *dual])
    if len(set(combined)) != 23:
        raise LedgerError(f"{location}.consumer_inventory contains duplicates")
    encoded = "".join(f"{path}\n" for path in combined).encode("utf-8")
    observed = hashlib.sha256(encoded).hexdigest()
    expected = delta.get("consumer_inventory_sha256")
    if expected != observed:
        raise LedgerError(
            f"{location}.consumer_inventory_sha256 mismatch: expected {expected}, observed {observed}"
        )


def validate(
    ledger: dict[str, object],
    *,
    repository_root: Path,
    require_frozen: bool,
) -> None:
    if ledger.get("schema") != SCHEMA:
        raise LedgerError(f"schema must be {SCHEMA}")
    status = ledger.get("status")
    if status not in {"evidence_in_progress", "frozen"}:
        raise LedgerError("status must be evidence_in_progress or frozen")
    decision = ledger.get("roboplan_decision")
    if decision not in ALLOWED_DECISIONS:
        raise LedgerError(
            f"roboplan_decision must be one of {sorted(ALLOWED_DECISIONS)}"
        )
    if require_frozen:
        if status != "frozen":
            raise LedgerError("ledger status is not frozen")
        if decision == "undecided":
            raise LedgerError("RoboPlan decision remains undecided")

    _validate_decision_record(ledger)
    source_ids = _validate_source_identities(ledger)
    _validate_source_closures(ledger, source_ids)
    _validate_selected_outcome_qualification(
        ledger,
        require_frozen=require_frozen,
        repository_root=repository_root,
    )
    _validate_capabilities(ledger)
    deltas = _list(ledger.get("deltas"), "deltas")
    if not deltas:
        raise LedgerError("deltas must not be empty")
    delta_ids: set[str] = set()
    for index, raw in enumerate(deltas):
        location = f"deltas[{index}]"
        delta = _object(raw, location)
        missing = REQUIRED_DELTA_FIELDS - delta.keys()
        if missing:
            raise LedgerError(f"{location} is missing: {', '.join(sorted(missing))}")
        delta_id = _text(delta["id"], f"{location}.id")
        if delta_id in delta_ids:
            raise LedgerError(f"duplicate delta id: {delta_id}")
        delta_ids.add(delta_id)
        _text(delta["dependency"], f"{location}.dependency")
        references = set(
            _text_list(
                delta["source_identity_refs"], f"{location}.source_identity_refs"
            )
        )
        unknown_sources = references - source_ids
        if unknown_sources:
            raise LedgerError(
                f"{location} references unknown frozen sources: {sorted(unknown_sources)}"
            )
        local_sources = _list(delta["local_sources"], f"{location}.local_sources")
        for source_index, source in enumerate(local_sources):
            _validate_local_source(
                source,
                f"{location}.local_sources[{source_index}]",
                repository_root=repository_root,
            )
        for field in (
            "guarantee",
            "owner_gate",
            "rollback_source",
            "removal_condition",
        ):
            _text(delta[field], f"{location}.{field}")
        _text_list(delta["consuming_surfaces"], f"{location}.consuming_surfaces")
        _text_list(delta["acceptance_evidence"], f"{location}.acceptance_evidence")
        _validate_builder_consumer_inventory(delta, location)
        _validate_disposition(
            delta["disposition"],
            f"{location}.disposition",
            require_frozen=require_frozen,
            repository_root=repository_root,
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ledger", type=Path)
    parser.add_argument("--repository-root", type=Path)
    parser.add_argument("--require-frozen", action="store_true")
    args = parser.parse_args(argv)
    repository_root = (
        args.repository_root.resolve()
        if args.repository_root is not None
        else Path(__file__).resolve().parents[2]
    )
    try:
        ledger = json.loads(args.ledger.read_text(encoding="utf-8"))
        validate(
            _object(ledger, "ledger"),
            repository_root=repository_root,
            require_frozen=args.require_frozen,
        )
    except (OSError, json.JSONDecodeError, LedgerError) as exc:
        print(f"E1 external-delta ledger verification failed: {exc}", file=sys.stderr)
        return 1
    print("E1 external-delta ledger verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
