#!/usr/bin/env python3
"""Generate the deterministic Step 18 live closure manifest and parity artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification import verify_step18_live_contact_modes as live  # noqa: E402
from scripts.qualification.source_identity import (  # noqa: E402
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
)


class _InputTracker:
    """Retain the exact bytes used to generate the manifest."""

    def __init__(self) -> None:
        self._payloads: dict[Path, bytes] = {}
        self._digests: dict[Path, str] = {}

    def read(self, path: Path, field: str) -> tuple[bytes, str]:
        resolved = path.resolve()
        if resolved in self._payloads:
            return self._payloads[resolved], self._digests[resolved]
        try:
            payload = resolved.read_bytes()
        except OSError as exc:
            raise RuntimeError(f"cannot read {field} {resolved}: {exc}") from exc
        digest = hashlib.sha256(payload).hexdigest()
        self._payloads[resolved] = payload
        self._digests[resolved] = digest
        return payload, digest

    def assert_unchanged(self) -> None:
        for path, expected in self._digests.items():
            try:
                observed = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError as exc:
                raise RuntimeError(f"retained input disappeared: {path}: {exc}") from exc
            if observed != expected:
                raise RuntimeError(f"retained input changed during generation: {path}")


def _json_payload(payload: bytes, field: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot parse {field}: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{field}: top-level JSON must be an object")
    return value


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _same(values: list[Any], field: str) -> Any:
    if not values or any(value != values[0] for value in values[1:]):
        raise RuntimeError(f"row metadata disagrees on {field}")
    return values[0]


def _contained_relative(path: Path, root: Path, field: str) -> str:
    resolved = path.resolve()
    try:
        return str(resolved.relative_to(root.resolve()))
    except ValueError as exc:
        raise RuntimeError(f"{field} must remain below manifest directory {root}") from exc


def _metadata_path(metadata: dict[str, Any], field: str) -> Path:
    value = metadata.get(field)
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"run metadata {field} is missing")
    path = Path(value).resolve()
    if not path.is_file():
        raise RuntimeError(f"run metadata {field} does not exist: {path}")
    return path


def _runtime_profile(rows: list[dict[str, Any]], row_kind: str) -> dict[str, Any]:
    selected = [row["metadata"] for row in rows if row["row_kind"] == row_kind]
    composer = _same(
        [metadata.get("composer_runtime") for metadata in selected],
        f"{row_kind}.composer_runtime",
    )
    controller = _same(
        [metadata.get("controller_runtime") for metadata in selected],
        f"{row_kind}.controller_runtime",
    )
    host_root = _same(
        [metadata.get("host_repo_root") for metadata in selected],
        f"{row_kind}.host_repo_root",
    )
    if not isinstance(composer, dict) or not isinstance(controller, dict):
        raise RuntimeError(f"{row_kind}: runtime identity is missing")
    return {
        "host_repo_root": host_root,
        "composer_runtime_fingerprint_sha256": composer.get("fingerprint_sha256"),
        "controller_runtime_fingerprint_sha256": controller.get("fingerprint_sha256"),
        "composer_image_id": composer.get("image_id"),
        "controller_image_id": controller.get("image_id"),
        "composer_native_artifacts_sha256": _canonical_sha256(
            (composer.get("process") or {}).get("mapped_native_artifacts")
        ),
        "controller_native_artifacts_sha256": _canonical_sha256(
            (controller.get("process") or {}).get("mapped_native_artifacts")
        ),
    }


def _row(
    metadata_path: Path, output_root: Path, tracker: _InputTracker
) -> dict[str, Any]:
    metadata_payload, metadata_digest = tracker.read(
        metadata_path, "row metadata"
    )
    metadata = _json_payload(metadata_payload, f"row metadata {metadata_path}")
    if metadata.get("schema_version") != "manyforge.step18_live_row_run.v2":
        raise RuntimeError(f"{metadata_path}: unsupported row metadata schema")
    row_uuid = metadata.get("row_uuid")
    row_kind = metadata.get("row_kind")
    mode = metadata.get("support_contact_mode")
    stratum = metadata.get("viewer_stratum")
    if row_kind not in live.ROW_KINDS:
        raise RuntimeError(f"{metadata_path}: invalid row kind")
    evidence = _metadata_path(metadata, "evidence_path")
    controller_log = _metadata_path(metadata, "controller_log_path")
    evidence_payload, evidence_sha = tracker.read(evidence, "row evidence")
    _, log_sha = tracker.read(
        controller_log, "row controller log"
    )
    if metadata.get("evidence_sha256") != evidence_sha:
        raise RuntimeError(f"{metadata_path}: evidence hash is stale")
    if metadata.get("controller_log_sha256") != log_sha:
        raise RuntimeError(f"{metadata_path}: controller log hash is stale")
    return {
        "id": f"{row_kind}-{mode}-{stratum}",
        "row_uuid": row_uuid,
        "row_kind": row_kind,
        "mode": mode,
        "viewer_stratum": stratum,
        "evidence": _contained_relative(evidence, output_root, "evidence"),
        "evidence_sha256": evidence_sha,
        "controller_log": _contained_relative(
            controller_log, output_root, "controller log"
        ),
        "controller_log_sha256": log_sha,
        "run_metadata": _contained_relative(
            metadata_path, output_root, "run metadata"
        ),
        "run_metadata_sha256": metadata_digest,
        "metadata": metadata,
        "evidence_value": _json_payload(
            evidence_payload, f"row evidence {evidence}"
        ),
    }


def _parity(rows: list[dict[str, Any]]) -> dict[str, Any]:
    bindings: list[dict[str, Any]] = []
    for stratum in live.VIEWER_STRATA:
        pristine = next(
            row
            for row in rows
            if row["row_kind"] == "baseline_pristine_timing"
            and row["viewer_stratum"] == stratum
        )
        observation = next(
            row
            for row in rows
            if row["row_kind"] == "baseline_observation"
            and row["viewer_stratum"] == stratum
        )
        pristine_evidence = pristine["evidence_value"]
        observation_evidence = observation["evidence_value"]
        pristine_output = live.retained_live_output_sha256(
            pristine_evidence, f"parity.{stratum}.pristine"
        )
        observation_output = live.retained_live_output_sha256(
            observation_evidence, f"parity.{stratum}.observation"
        )
        if pristine_output != observation_output:
            raise RuntimeError(f"{stratum}: retained live outputs differ")
        bindings.append(
            {
                "viewer_stratum": stratum,
                "pristine_evidence_sha256": pristine["evidence_sha256"],
                "observation_evidence_sha256": observation["evidence_sha256"],
                "pristine_retained_live_output_sha256": pristine_output,
                "observation_retained_live_output_sha256": observation_output,
            }
        )
    pristine_source = _same(
        [
            row["metadata"].get("observed_source_identity")
            for row in rows
            if row["row_kind"] == "baseline_pristine_timing"
        ],
        "pristine source identity",
    )
    observation_source = _same(
        [
            row["metadata"].get("observed_source_identity")
            for row in rows
            if row["row_kind"] == "baseline_observation"
        ],
        "observation source identity",
    )
    return {
        "schema_version": live.PARITY_SCHEMA,
        "passed": True,
        "scope": "retained_live_decision_trajectory_outputs",
        "pristine_source_fingerprint_sha256": pristine_source.get(
            "fingerprint_sha256"
        ),
        "observation_source_fingerprint_sha256": observation_source.get(
            "fingerprint_sha256"
        ),
        "rows": bindings,
    }


def _write_exclusive(path: Path, value: dict[str, Any]) -> str:
    payload = (
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    ).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(payload)
    return hashlib.sha256(payload).hexdigest()


def generate(args: argparse.Namespace) -> dict[str, Any]:
    output = args.output.resolve()
    parity_output = args.parity_output.resolve()
    candidate_root = Path(
        getattr(args, "current_candidate_root", REPO_ROOT)
    ).resolve()
    specs_root_raw = getattr(args, "current_specs_root", None)
    if specs_root_raw is None:
        raise RuntimeError("current Specs root is required")
    specs_root = Path(specs_root_raw).resolve()
    if candidate_root != REPO_ROOT.resolve():
        raise RuntimeError("current candidate root is not the generator checkout")
    if not specs_root.is_dir():
        raise RuntimeError("current Specs root does not exist")
    if output == parity_output:
        raise RuntimeError("manifest and parity outputs must be distinct files")
    if output.parent != parity_output.parent:
        raise RuntimeError("manifest and parity artifact must share one evidence root")
    if output.exists() or parity_output.exists():
        raise RuntimeError("refusing to overwrite manifest or parity artifact")
    if len(args.row_metadata) != 16:
        raise RuntimeError("exactly sixteen --row-metadata inputs are required")
    tracker = _InputTracker()
    verifier_path = (candidate_root / live._VERIFIER_RELATIVE_PATH).resolve()  # noqa: SLF001
    _, verifier_sha256 = tracker.read(verifier_path, "live verifier")
    try:
        initial_candidate_identity = source_identity(candidate_root)
        initial_specs_contract_identity = specs_contract_identity(specs_root)
    except SourceIdentityError as exc:
        raise RuntimeError(f"cannot bind current source identity: {exc}") from exc
    output_root = output.parent
    rows = [
        _row(path.resolve(), output_root, tracker) for path in args.row_metadata
    ]
    expected_rows = {
        (kind, live.BASELINE_MODE, stratum)
        for kind in ("baseline_pristine_timing", "baseline_observation")
        for stratum in live.VIEWER_STRATA
    } | {
        (kind, mode, stratum)
        for kind in ("candidate_timing", "candidate_observation")
        for mode in live.CANDIDATE_MODES
        for stratum in live.VIEWER_STRATA
    }
    actual_rows = {
        (row["row_kind"], row["mode"], row["viewer_stratum"]) for row in rows
    }
    if len(actual_rows) != len(rows) or actual_rows != expected_rows:
        raise RuntimeError("row metadata does not form the exact sixteen-row matrix")
    rows.sort(
        key=lambda row: (
            live.ROW_KINDS.index(row["row_kind"]),
            row["mode"],
            live.VIEWER_STRATA.index(row["viewer_stratum"]),
        )
    )
    patch = args.baseline_observation_patch.resolve()
    _contained_relative(patch, output_root, "baseline observation patch")
    if not patch.is_file():
        raise RuntimeError("baseline observation patch does not exist")
    _, patch_sha256 = tracker.read(patch, "baseline observation patch")
    parity = _parity(rows)

    def role_value(row_kind: str, field: str) -> Any:
        return _same(
            [
                row["metadata"].get(field)
                for row in rows
                if row["row_kind"] == row_kind
            ],
            f"{row_kind}.{field}",
        )

    candidate_identity = _same(
        [
            role_value(kind, "observed_source_identity")
            for kind in ("candidate_timing", "candidate_observation")
        ],
        "candidate timing/observation source identity",
    )
    candidate_specs_contract = _same(
        [
            role_value(kind, "specs_contract_identity")
            for kind in ("candidate_timing", "candidate_observation")
        ],
        "candidate timing/observation Specs contract identity",
    )
    if candidate_identity != initial_candidate_identity:
        raise RuntimeError("candidate rows do not bind the current ManyForge source")
    if candidate_specs_contract != initial_specs_contract_identity:
        raise RuntimeError("candidate rows do not bind the current Specs contract")
    baseline_specs = _same(
        [
            role_value("baseline_pristine_timing", "specs_source_identity"),
            role_value("baseline_observation", "specs_source_identity"),
        ],
        "baseline Specs source identity",
    )
    baseline_specs_contract = _same(
        [
            role_value("baseline_pristine_timing", "specs_contract_identity"),
            role_value("baseline_observation", "specs_contract_identity"),
        ],
        "baseline Specs contract identity",
    )
    runner_sha = _same(
        [row["metadata"].get("host_runner") for row in rows], "host runner"
    ).get("sha256")
    tool_bundle = _same(
        [row["metadata"].get("qualification_tool_bundle") for row in rows],
        "qualification tool bundle",
    )
    thor_host_identity = _same(
        [row["metadata"].get("thor_host_identity") for row in rows],
        "Thor host identity",
    )
    mechanism_path = args.mechanism_replay_manifest.resolve()
    _contained_relative(mechanism_path, output_root, "mechanism replay manifest")
    mechanism_payload, mechanism_sha256 = tracker.read(
        mechanism_path, "mechanism replay manifest"
    )
    mechanism = _json_payload(mechanism_payload, "mechanism replay manifest")
    if mechanism.get("schema_version") != live.MECHANISM_REPLAY_MANIFEST_SCHEMA:
        raise RuntimeError("mechanism replay manifest schema is invalid")
    expected_observation_rows = sorted(
        row["id"]
        for row in rows
        if row["row_kind"] in live.OBSERVATION_ROW_KINDS
    )
    if sorted(mechanism.get("observation_row_ids") or []) != expected_observation_rows:
        raise RuntimeError("mechanism replay manifest does not own every observation row")
    manifest = {
        "schema_version": live.MANIFEST_SCHEMA,
        "expected_baseline_commit": live.PINNED_BASELINE_COMMIT,
        "expected_specs_commit": live.PINNED_SPECS_COMMIT,
        "expected_candidate_source_identity": candidate_identity,
        "expected_baseline_specs_source_identity": baseline_specs,
        "expected_candidate_specs_source_identity": role_value(
            "candidate_observation", "specs_source_identity"
        ),
        "expected_baseline_specs_contract_identity": baseline_specs_contract,
        "expected_candidate_specs_contract_identity": candidate_specs_contract,
        "current_candidate_root": str(candidate_root),
        "current_specs_root": str(specs_root),
        "expected_verifier": {
            "path": str(verifier_path),
            "sha256": verifier_sha256,
        },
        "baseline_observation_patch": _contained_relative(
            patch, output_root, "baseline observation patch"
        ),
        "baseline_observation_patch_sha256": patch_sha256,
        "baseline_observation_parity": _contained_relative(
            parity_output, output_root, "baseline retained-output parity"
        ),
        "baseline_observation_parity_sha256": "",
        "mechanism_replay_manifest": _contained_relative(
            mechanism_path, output_root, "mechanism replay manifest"
        ),
        "mechanism_replay_manifest_sha256": mechanism_sha256,
        "expected_host_runner_sha256": runner_sha,
        "expected_qualification_tool_bundle_fingerprint_sha256": tool_bundle.get(
            "fingerprint_sha256"
        ),
        "expected_thor_host_identity": thor_host_identity,
        "runtime_profiles": {
            row_kind: _runtime_profile(rows, row_kind)
            for row_kind in live.ROW_KINDS
        },
        "bounded_penetration_m": args.bounded_penetration_m,
        "maximum_controller_progress_regression_ratio": args.maximum_ratio,
        "rows": [
            {
                key: value
                for key, value in row.items()
                if key not in {"metadata", "evidence_value"}
            }
            for row in rows
        ],
    }
    tracker.assert_unchanged()
    try:
        if source_identity(candidate_root) != initial_candidate_identity:
            raise RuntimeError("current ManyForge source changed during generation")
        if specs_contract_identity(specs_root) != initial_specs_contract_identity:
            raise RuntimeError("current Specs contract changed during generation")
    except SourceIdentityError as exc:
        raise RuntimeError(f"final source identity cannot be read: {exc}") from exc
    manifest["baseline_observation_parity_sha256"] = _write_exclusive(
        parity_output, parity
    )
    _write_exclusive(output, manifest)
    tracker.assert_unchanged()
    try:
        if source_identity(candidate_root) != initial_candidate_identity:
            raise RuntimeError("ignored outputs invalidated the ManyForge source")
        if specs_contract_identity(specs_root) != initial_specs_contract_identity:
            raise RuntimeError("ignored outputs invalidated the Specs contract")
    except SourceIdentityError as exc:
        raise RuntimeError(f"post-output source identity cannot be read: {exc}") from exc
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--row-metadata", type=Path, action="append", required=True)
    parser.add_argument("--baseline-observation-patch", type=Path, required=True)
    parser.add_argument("--mechanism-replay-manifest", type=Path, required=True)
    parser.add_argument("--parity-output", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--current-candidate-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--current-specs-root", type=Path, required=True)
    parser.add_argument("--bounded-penetration-m", type=float, default=0.03)
    parser.add_argument("--maximum-ratio", type=float, default=1.10)
    args = parser.parse_args(argv)
    if (
        not math.isfinite(args.bounded_penetration_m)
        or args.bounded_penetration_m < 0.0
        or not math.isfinite(args.maximum_ratio)
        or args.maximum_ratio < 1.0
    ):
        parser.error("thresholds must be finite; maximum ratio must be at least one")
    try:
        live.ensure_ignored_qualification_output(
            args.output, args.current_candidate_root
        )
        live.ensure_ignored_qualification_output(
            args.parity_output, args.current_candidate_root
        )
        manifest = generate(args)
    except (OSError, RuntimeError, live.ValidationError, ValueError) as exc:
        print(f"Step 18 live manifest generation failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"rows": len(manifest["rows"]), "generated": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
