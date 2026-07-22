#!/usr/bin/env python3
"""Replay exact Step 18 UR10/Isaac live inputs through production validation."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
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


ROW_METADATA_SCHEMA = "manyforge.step18_live_row_run.v2"
BASELINE_LANE_SCHEMA = "manyforge.runtime_collision_baseline_lane.v2"
MODE_TO_ARM = {
    "signed_penetration": "signed_penetration_selective",
    "binary_contact": "binary_contact",
    "bounded_penetration": "bounded_penetration",
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json(path: Path, field: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read {field} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{field} must be a JSON object")
    return value


def _write_exclusive(path: Path, value: dict[str, Any]) -> str:
    payload = (
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    ).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(payload)
    return hashlib.sha256(payload).hexdigest()


def _write_capture_exclusive(path: Path, value: dict[str, Any]) -> str:
    payload = (
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
        + "\n"
    ).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(payload)
    return hashlib.sha256(payload).hexdigest()


def _run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _checked(command: list[str], field: str) -> subprocess.CompletedProcess[str]:
    result = _run(command)
    if result.returncode != 0:
        raise RuntimeError(
            f"{field} failed ({result.returncode}): "
            f"{result.stdout}\n{result.stderr}"
        )
    return result


def _below(path: Path, root: Path, field: str) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise RuntimeError(f"{field} must remain below {root}") from exc
    return resolved


def _container_path(path: Path, repo_root: Path, field: str) -> str:
    relative = _below(path, repo_root, field).relative_to(repo_root.resolve())
    return str(Path("/workspace") / relative)


def _binding(path: Path) -> dict[str, str]:
    return {"path": str(path.resolve()), "sha256": _sha256(path.resolve())}


def _compose_prefix(repo_root: Path) -> list[str]:
    return [
        "docker",
        "compose",
        "-f",
        str(repo_root / "docker-compose.yaml"),
        "run",
        "--rm",
        "-T",
        "--no-deps",
    ]


def _dev_image(repo_root: Path) -> dict[str, str]:
    reference = _checked(
        [
            "docker",
            "compose",
            "-f",
            str(repo_root / "docker-compose.yaml"),
            "images",
            "-q",
            "dev",
        ],
        "resolve dev image",
    ).stdout.strip()
    if not reference or "\n" in reference:
        raise RuntimeError("Compose dev service does not resolve to one image")
    image_id = _checked(
        ["docker", "image", "inspect", reference, "--format", "{{.Id}}"],
        "inspect dev image",
    ).stdout.strip()
    if not image_id.startswith("sha256:") or len(image_id) != 71:
        raise RuntimeError("dev image is not content-addressed")
    return {"reference": reference, "image_id": image_id}


def _build_candidate(
    *, repo_root: Path, build_dir: Path
) -> tuple[list[list[str]], Path, Path]:
    if build_dir.exists():
        raise RuntimeError(
            f"candidate mechanism build directory already exists: {build_dir}"
        )
    build_root = (repo_root / "build").resolve()
    _below(build_dir, build_root, "candidate build directory")
    if build_dir.resolve() == build_root:
        raise RuntimeError(
            "candidate mechanism build directory must be a dedicated path below build"
        )
    source_container = "/workspace/manyforge_roboplan"
    build_container = _container_path(build_dir, repo_root, "candidate build")
    configure = _compose_prefix(repo_root) + [
        "--entrypoint",
        "cmake",
        "dev",
        "-S",
        source_container,
        "-B",
        build_container,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
    ]
    build = _compose_prefix(repo_root) + [
        "--entrypoint",
        "cmake",
        "dev",
        "--build",
        build_container,
        "--target",
        "perf_runtime_collision_schedule_timing",
        "perf_runtime_collision_schedule_observation",
        "--parallel",
        "1",
    ]
    _checked(configure, "candidate replay configure")
    _checked(build, "candidate replay build")
    timing = (
        build_dir
        / "manyforge_integration_tests/perf_runtime_collision_schedule_timing"
    )
    observation = (
        build_dir
        / "manyforge_integration_tests/perf_runtime_collision_schedule_observation"
    )
    if not timing.is_file() or not observation.is_file():
        raise RuntimeError("candidate replay build omitted a required binary")
    return [configure, build], timing, observation


def _row(
    metadata_path: Path,
    *,
    candidate_identity: dict[str, Any],
    candidate_specs_contract: dict[str, Any],
    baseline_observation_identity: dict[str, Any],
) -> dict[str, Any]:
    metadata = _json(metadata_path, "observation row metadata")
    if metadata.get("schema_version") != ROW_METADATA_SCHEMA:
        raise RuntimeError(f"{metadata_path}: unsupported row metadata schema")
    kind = metadata.get("row_kind")
    mode = metadata.get("support_contact_mode")
    stratum = metadata.get("viewer_stratum")
    if kind not in live.OBSERVATION_ROW_KINDS or stratum not in live.VIEWER_STRATA:
        raise RuntimeError(f"{metadata_path}: not a Step 18 observation row")
    if kind == "baseline_observation":
        if mode != live.BASELINE_MODE:
            raise RuntimeError(f"{metadata_path}: baseline mode changed")
        if metadata.get("observed_source_identity") != baseline_observation_identity:
            raise RuntimeError(f"{metadata_path}: baseline observation source changed")
    else:
        if mode not in live.CANDIDATE_MODES:
            raise RuntimeError(f"{metadata_path}: candidate mode changed")
        if metadata.get("observed_source_identity") != candidate_identity:
            raise RuntimeError(f"{metadata_path}: candidate source changed")
        if metadata.get("specs_contract_identity") != candidate_specs_contract:
            raise RuntimeError(f"{metadata_path}: candidate Specs contract changed")
    evidence_path = Path(str(metadata.get("evidence_path") or "")).resolve()
    if not evidence_path.is_file() or metadata.get("evidence_sha256") != _sha256(
        evidence_path
    ):
        raise RuntimeError(f"{metadata_path}: observation evidence binding changed")
    evidence = _json(evidence_path, "observation row evidence")
    records = evidence.get("records")
    if not isinstance(records, list) or len(records) != 1:
        raise RuntimeError(f"{metadata_path}: expected one live record")
    details = records[0].get("details") if isinstance(records[0], dict) else None
    if not isinstance(details, dict):
        raise RuntimeError(f"{metadata_path}: live record details are missing")
    capture = details.get("mechanism_replay_input")
    capture_identity = live._mechanism_capture_identity(  # noqa: SLF001
        capture,
        details=details,
        field=f"{metadata_path}.mechanism_replay_input",
    )
    return {
        "id": f"{kind}-{mode}-{stratum}",
        "row_uuid": metadata.get("row_uuid"),
        "row_kind": kind,
        "mode": mode,
        "viewer_stratum": stratum,
        "metadata": metadata,
        "metadata_binding": _binding(metadata_path),
        "evidence_binding": _binding(evidence_path),
        "capture": capture,
        "capture_identity": capture_identity,
    }


def _execution_specs(row: dict[str, Any]) -> list[tuple[str, str, str, str]]:
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
    if row["row_kind"] == "baseline_observation":
        return baseline
    arm = MODE_TO_ARM[row["mode"]]
    return baseline + [
        ("candidate_decision", "candidate_timing", arm, "current_candidate"),
        (
            "candidate_observation",
            "candidate_observation",
            arm,
            "current_candidate",
        ),
    ]


def _run_replay(
    *,
    repo_root: Path,
    binary: Path,
    capture_path: Path,
    output_path: Path,
    run_id: str,
    arm: str,
    source_role: str,
    bounded_penetration_m: float,
) -> tuple[list[str], dict[str, Any], str, str]:
    command = _compose_prefix(repo_root) + [
        "--entrypoint",
        _container_path(binary, repo_root, "replay binary"),
        "dev",
        "--mechanism-replay-input",
        _container_path(capture_path, repo_root, "replay capture"),
        "--repo-root",
        "/workspace",
        "--run-id",
        run_id,
        "--source-role",
        source_role,
        "--arm",
        arm,
        "--bounded-penetration-m",
        str(bounded_penetration_m),
    ]
    result = _checked(command, f"mechanism replay {run_id}")
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"mechanism replay {run_id} did not emit one JSON record")
    try:
        value = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"mechanism replay {run_id} emitted invalid JSON") from exc
    if not isinstance(value, dict) or value.get("schema_version") != live.MECHANISM_REPLAY_SCHEMA:
        raise RuntimeError(f"mechanism replay {run_id} emitted the wrong schema")
    output_sha256 = _write_exclusive(output_path, value)
    return command, value, output_sha256, result.stderr


def run(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    specs_root = args.specs_root.resolve()
    output = args.output.resolve()
    output_dir = args.output_dir.resolve()
    live.ensure_ignored_qualification_output(output, repo_root)
    live.ensure_ignored_qualification_output(output_dir / "probe.json", repo_root)
    if output.exists() or output_dir.exists():
        raise RuntimeError("refusing to overwrite mechanism replay evidence")
    if output.parent != output_dir.parent:
        raise RuntimeError("mechanism manifest and artifact directory must share a root")
    build_dir = args.candidate_build_dir.resolve()

    try:
        initial_candidate = source_identity(repo_root)
        initial_specs_contract = specs_contract_identity(specs_root)
    except SourceIdentityError as exc:
        raise RuntimeError(f"cannot bind current source identity: {exc}") from exc
    if initial_candidate.get("head_commit") != live.PINNED_BASELINE_COMMIT or not initial_candidate.get("dirty"):
        raise RuntimeError("candidate must be a frozen delta over pinned 2bfe30e")
    if initial_specs_contract.get("head_commit") != live.PINNED_SPECS_COMMIT or not initial_specs_contract.get("contract_dirty"):
        raise RuntimeError("candidate Specs contract must be a frozen Step 18 delta")

    baseline_metadata_path = args.baseline_lane_metadata.resolve()
    baseline = _json(baseline_metadata_path, "baseline lane metadata")
    if baseline.get("schema_version") != BASELINE_LANE_SCHEMA:
        raise RuntimeError("baseline lane metadata schema is invalid")
    baseline_timing = args.baseline_timing_binary.resolve()
    baseline_observation = args.baseline_observation_binary.resolve()
    if baseline.get("timing_binary_sha256") != _sha256(baseline_timing):
        raise RuntimeError("baseline timing binary binding changed")
    if baseline.get("observation_binary_sha256") != _sha256(baseline_observation):
        raise RuntimeError("baseline observation binary binding changed")

    rows = [
        _row(
            path.resolve(),
            candidate_identity=initial_candidate,
            candidate_specs_contract=initial_specs_contract,
            baseline_observation_identity=baseline.get("observation_source_identity"),
        )
        for path in args.observation_row_metadata
    ]
    expected = {
        ("baseline_observation", live.BASELINE_MODE, stratum)
        for stratum in live.VIEWER_STRATA
    } | {
        ("candidate_observation", mode, stratum)
        for mode in live.CANDIDATE_MODES
        for stratum in live.VIEWER_STRATA
    }
    actual = {(row["row_kind"], row["mode"], row["viewer_stratum"]) for row in rows}
    if len(rows) != 8 or actual != expected:
        raise RuntimeError("observation rows do not form the exact eight-row owner matrix")
    rows.sort(key=lambda row: row["id"])

    image = _dev_image(repo_root)
    build_commands, candidate_timing, candidate_observation = _build_candidate(
        repo_root=repo_root, build_dir=build_dir
    )
    if _dev_image(repo_root) != image:
        raise RuntimeError("dev image changed during candidate replay build")
    binaries = {
        "baseline_timing": baseline_timing,
        "baseline_observation": baseline_observation,
        "candidate_timing": candidate_timing,
        "candidate_observation": candidate_observation,
    }

    output_dir.mkdir(parents=True)
    records: list[dict[str, Any]] = []
    for row in rows:
        capture_path = output_dir / "captures" / f"{row['id']}.json"
        capture_sha256 = _write_capture_exclusive(capture_path, row["capture"])
        if capture_sha256 != row["capture_identity"]["capture_payload_sha256"]:
            raise RuntimeError(f"{row['id']}: retained capture bytes changed")
        executions: list[dict[str, Any]] = []
        for role, binary_role, arm, source_role in _execution_specs(row):
            artifact_path = output_dir / "replays" / f"{row['id']}--{role}.json"
            run_id = f"{row['row_uuid']}--{role}"
            command, value, artifact_sha256, stderr = _run_replay(
                repo_root=repo_root,
                binary=binaries[binary_role],
                capture_path=capture_path,
                output_path=artifact_path,
                run_id=run_id,
                arm=arm,
                source_role=source_role,
                bounded_penetration_m=args.bounded_penetration_m,
            )
            executions.append(
                {
                    "role": role,
                    "binary_role": binary_role,
                    "binary": _binding(binaries[binary_role]),
                    "arm": arm,
                    "source_role": source_role,
                    "run_id": run_id,
                    "capture_container_path": _container_path(
                        capture_path, repo_root, "replay capture"
                    ),
                    "command_argv": command,
                    "stderr_sha256": hashlib.sha256(stderr.encode()).hexdigest(),
                    "artifact": {
                        "path": str(artifact_path),
                        "sha256": artifact_sha256,
                    },
                    "result_summary": {
                        "measurement_kind": value.get("measurement_kind"),
                        "pair_transcript_fingerprint": value.get(
                            "pair_transcript_fingerprint"
                        ),
                    },
                }
            )
        records.append(
            {
                "observation_row_id": row["id"],
                "row_uuid": row["row_uuid"],
                "row_kind": row["row_kind"],
                "mode": row["mode"],
                "viewer_stratum": row["viewer_stratum"],
                "row_metadata": row["metadata_binding"],
                "row_evidence": row["evidence_binding"],
                "capture": {"path": str(capture_path), "sha256": capture_sha256},
                "capture_identity": row["capture_identity"],
                "executions": executions,
            }
        )

    if source_identity(repo_root) != initial_candidate:
        raise RuntimeError("candidate source changed during mechanism replay")
    if specs_contract_identity(specs_root) != initial_specs_contract:
        raise RuntimeError("Specs contract changed during mechanism replay")
    if _dev_image(repo_root) != image:
        raise RuntimeError("dev image changed during mechanism replay")
    manifest = {
        "schema_version": live.MECHANISM_REPLAY_MANIFEST_SCHEMA,
        "scope": "exact_ur10e_isaac_live_validation_mechanism_owner",
        "runner": _binding(Path(__file__)),
        "candidate_source_identity": initial_candidate,
        "candidate_specs_contract_identity": initial_specs_contract,
        "baseline_lane_metadata": _binding(baseline_metadata_path),
        "baseline_timing_source_identity": baseline.get("timing_source_identity"),
        "baseline_observation_source_identity": baseline.get(
            "observation_source_identity"
        ),
        "dev_image": image,
        "candidate_build_commands_argv": build_commands,
        "candidate_binaries": {
            "timing": _binding(candidate_timing),
            "observation": _binding(candidate_observation),
        },
        "bounded_penetration_m": args.bounded_penetration_m,
        "observation_row_ids": [record["observation_row_id"] for record in records],
        "records": records,
    }
    _write_exclusive(output, manifest)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--observation-row-metadata", type=Path, action="append", required=True
    )
    parser.add_argument("--baseline-lane-metadata", type=Path, required=True)
    parser.add_argument("--baseline-timing-binary", type=Path, required=True)
    parser.add_argument("--baseline-observation-binary", type=Path, required=True)
    parser.add_argument("--candidate-build-dir", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--specs-root", type=Path, required=True)
    parser.add_argument("--bounded-penetration-m", type=float, default=0.03)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if len(args.observation_row_metadata) != 8:
        parser.error("exactly eight observation row metadata inputs are required")
    if (
        not math.isfinite(args.bounded_penetration_m)
        or args.bounded_penetration_m < 0.0
    ):
        parser.error("--bounded-penetration-m must be finite and non-negative")
    try:
        manifest = run(args)
    except (OSError, RuntimeError, live.ValidationError, ValueError) as exc:
        print(f"Step 18 live mechanism replay failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"records": len(manifest["records"]), "passed": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
