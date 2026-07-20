#!/usr/bin/env python3
"""Generate a typed decision/pair parity artifact for baseline probe records."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import uuid
from pathlib import Path
from typing import Any

from source_identity import (
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity as validate_source_identity,
    validate_specs_contract_identity,
)


SCHEMA = "manyforge.runtime_collision_baseline_decision_parity.v2"
RECORD_SCHEMA = "manyforge.runtime_collision_schedule_qualification.v2"
BASELINE_SOURCE = "643fe9c399182419d38d4478750d7e6f692dc9ad"
BASELINE_SPECS = "5e50a4872bbb9a2167c6bc14b9b5963061f2f09e"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_one(payload: bytes, path: Path) -> dict[str, Any]:
    text = payload.decode("utf-8").strip()
    parsed = json.loads(text)
    if isinstance(parsed, list):
        if len(parsed) != 1:
            raise ValueError(f"expected one probe record in {path}")
        parsed = parsed[0]
    if not isinstance(parsed, dict):
        raise ValueError(f"probe record must be an object: {path}")
    return parsed


def path_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def git_ignored(path: Path, repository_root: Path) -> bool:
    repository_root = repository_root.resolve()
    try:
        relative = path.resolve(strict=False).relative_to(repository_root)
    except ValueError:
        return True
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "check-ignore",
            "-q",
            "--",
            str(relative),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def validated_output(
    output: Path, records: tuple[dict[str, Any], dict[str, Any]]
) -> Path:
    destination = output.expanduser().resolve(strict=False)
    repository = Path(__file__).resolve().parents[2]
    if destination.exists():
        raise ValueError(f"refusing to overwrite parity artifact: {destination}")
    protected: set[Path] = set()
    for record in records:
        for field in ("source_root", "specs_root"):
            value = nested(
                record, "provenance", "runner_environment", field
            )
            if isinstance(value, str) and value:
                protected.add(Path(value).expanduser().resolve(strict=False))
    for root in protected:
        if path_within(destination, root):
            raise ValueError(
                f"parity output must not be inside qualification checkout {root}"
            )
    if path_within(destination, repository):
        allowed = (
            repository / "qualification",
            repository / ".manyforge-var" / "qualification",
            repository / "build",
        )
        if not any(path_within(destination, root) for root in allowed):
            raise ValueError(
                "parity output inside ManyForge must be below an ignored "
                "qualification or build evidence root"
            )
        if not git_ignored(destination, repository):
            raise ValueError(
                "parity output inside ManyForge must be actually ignored by git"
            )
    return destination


def stage_output(path: Path, payload: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.pending-", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        Path(temporary_name).unlink(missing_ok=True)
        raise
    return Path(temporary_name)


def publish_staged_exclusive(staged: Path, destination: Path) -> None:
    try:
        os.link(staged, destination)
    except FileExistsError as error:
        raise ValueError(
            f"refusing to overwrite parity artifact: {destination}"
        ) from error
    finally:
        try:
            staged.unlink(missing_ok=True)
        except OSError:
            pass


def nested(value: dict[str, Any], *path: str) -> Any:
    current: Any = value
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current


def require_probe_checkout_identities(
    records: tuple[dict[str, Any], dict[str, Any]]
) -> None:
    for index, record in enumerate(records):
        source_root = nested(
            record, "provenance", "runner_environment", "source_root"
        )
        specs_root = nested(
            record, "provenance", "runner_environment", "specs_root"
        )
        if not isinstance(source_root, str) or not isinstance(specs_root, str):
            raise ValueError(f"probe {index} lacks checkout roots")
        if source_identity(Path(source_root)) != nested(
            record, "identity", "source_identity"
        ):
            raise ValueError(f"probe {index} source checkout identity changed")
        if source_identity(Path(specs_root)) != nested(
            record, "identity", "specs_source_identity"
        ):
            raise ValueError(f"probe {index} full Specs identity changed")
        if specs_contract_identity(Path(specs_root)) != nested(
            record, "identity", "specs_contract_identity"
        ):
            raise ValueError(f"probe {index} Specs contract identity changed")


def fixture_identity(record: dict[str, Any]) -> dict[str, Any]:
    inputs = record.get("inputs", {})
    return {
        field: inputs.get(field)
        for field in (
            "fixture_id",
            "robot_model",
            "group",
            "geometry_fingerprint_fnv1a64",
            "pair_fingerprint_fnv1a64",
            "policy_fingerprint_fnv1a64",
            "trajectory_fingerprint_fnv1a64",
            "world_content_hash",
            "allowed_contact_pair_count",
            "trajectory_waypoint_count",
        )
    }


def normalized_pair_output(record: dict[str, Any]) -> dict[str, Any]:
    probe = record.get("parity_probe", {})
    pairs = probe.get("normalized_pairs")
    if (
        probe.get("evidence_kind") != "unmeasured_output"
        or not isinstance(pairs, list)
        or not pairs
        or not all(
            isinstance(pair, dict)
            and isinstance(pair.get("body_a"), str)
            and isinstance(pair.get("body_b"), str)
            and pair["body_a"] < pair["body_b"]
            for pair in pairs
        )
    ):
        raise ValueError("probe lacks canonical unmeasured output pairs")
    canonical_pairs = [
        {"body_a": pair["body_a"], "body_b": pair["body_b"]}
        for pair in pairs
    ]
    if canonical_pairs != sorted(
        canonical_pairs, key=lambda pair: (pair["body_a"], pair["body_b"])
    ) or len({(pair["body_a"], pair["body_b"]) for pair in canonical_pairs}) != len(
        canonical_pairs
    ):
        raise ValueError("probe output pairs are not sorted and unique")
    rendered = json.dumps(
        canonical_pairs, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return {
        "engine_status_code": probe.get("engine_status_code"),
        "decision_code": probe.get("decision_code"),
        "normalized_pair_output_fingerprint_fnv1a64": probe.get(
            "normalized_pair_output_fingerprint_fnv1a64"
        ),
        "normalized_pairs": canonical_pairs,
        "normalized_pairs_sha256": hashlib.sha256(rendered).hexdigest(),
    }


def validate_identity(record: dict[str, Any], kind: str, role: str) -> None:
    if record.get("schema_version") != RECORD_SCHEMA:
        raise ValueError(f"{kind} probe uses an unknown record schema")
    if nested(record, "inputs", "arm") != "current_all_active_distance":
        raise ValueError(f"{kind} probe is not the historical baseline arm")
    if nested(record, "identity", "measurement_kind") != kind:
        raise ValueError(f"{kind} probe measurement kind is inconsistent")
    if nested(record, "provenance", "runner_environment", "purpose") != (
        "parity-probe"
    ):
        raise ValueError(f"{kind} input is not labeled as a parity probe")
    try:
        source = validate_source_identity(
            nested(record, "identity", "source_identity")
        )
        specs = validate_source_identity(
            nested(record, "identity", "specs_source_identity")
        )
        contract = validate_specs_contract_identity(
            nested(record, "identity", "specs_contract_identity")
        )
    except SourceIdentityError as error:
        raise ValueError(f"{kind} probe source identity is invalid: {error}") from error
    if source["head_commit"] != BASELINE_SOURCE:
        raise ValueError(f"{kind} probe source is not the pinned baseline")
    if specs["head_commit"] != BASELINE_SPECS:
        raise ValueError(f"{kind} probe Specs source is not pinned")
    if (
        contract["head_commit"] != BASELINE_SPECS
        or contract["contract_dirty"] is not False
        or contract["head_tree"] != specs["head_tree"]
    ):
        raise ValueError(f"{kind} probe Specs contract is not pristine")
    if nested(record, "provenance", "source_role") != role:
        raise ValueError(f"{kind} probe source role is inconsistent")
    run_id = nested(record, "identity", "run_id")
    process_id = nested(record, "identity", "process_instance_id")
    if not isinstance(run_id, str) or not run_id:
        raise ValueError(f"{kind} probe lacks a run identity")
    try:
        process_uuid_valid = str(uuid.UUID(str(process_id))) == process_id
    except (AttributeError, ValueError):
        process_uuid_valid = False
    if not process_uuid_valid:
        raise ValueError(f"{kind} probe lacks a UUID process identity")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timing_probe", type=Path)
    parser.add_argument("observation_probe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    generator_path = Path(__file__).resolve()
    timing_path = args.timing_probe.resolve()
    observation_path = args.observation_probe.resolve()
    if timing_path == observation_path:
        raise ValueError("timing and observation probes must be distinct files")
    timing_payload = timing_path.read_bytes()
    observation_payload = observation_path.read_bytes()
    generator_sha256 = sha256_file(generator_path)
    timing_sha256 = hashlib.sha256(timing_payload).hexdigest()
    observation_sha256 = hashlib.sha256(observation_payload).hexdigest()
    timing = load_one(timing_payload, timing_path)
    observation = load_one(observation_payload, observation_path)
    validate_identity(timing, "timing", "pristine_baseline_timing")
    validate_identity(
        observation, "observation", "pristine_baseline_observation"
    )
    if nested(timing, "identity", "specs_source_identity") != nested(
        observation, "identity", "specs_source_identity"
    ):
        raise ValueError("baseline probes use different Specs identities")
    if nested(timing, "identity", "specs_contract_identity") != nested(
        observation, "identity", "specs_contract_identity"
    ):
        raise ValueError("baseline probes use different Specs contract identities")
    output = validated_output(args.output, (timing, observation))
    if nested(timing, "identity", "run_id") == nested(
        observation, "identity", "run_id"
    ) or nested(timing, "identity", "process_instance_id") == nested(
        observation, "identity", "process_instance_id"
    ):
        raise ValueError("baseline parity probes are not independent processes")

    timing_immediate = nested(timing, "raw", "immediate_samples") or []
    observation_immediate = (
        nested(observation, "raw", "immediate_samples") or []
    )
    timing_lookahead = nested(timing, "raw", "lookahead_cycles") or []
    observation_lookahead = (
        nested(observation, "raw", "lookahead_cycles") or []
    )
    timing_pair_output = normalized_pair_output(timing)
    observation_pair_output = normalized_pair_output(observation)
    comparisons = {
        "fixture_identity_equal": (
            fixture_identity(timing) == fixture_identity(observation)
        ),
        "normalized_logical_pairs_equal": (
            timing_pair_output == observation_pair_output
        ),
        "immediate_status_equal": (
            [sample.get("status") for sample in timing_immediate]
            == [sample.get("status") for sample in observation_immediate]
        ),
        "immediate_reason_equal": (
            [sample.get("reason_code") for sample in timing_immediate]
            == [
                sample.get("reason_code")
                for sample in observation_immediate
            ]
        ),
        "lookahead_status_equal": (
            [sample.get("status") for sample in timing_lookahead]
            == [sample.get("status") for sample in observation_lookahead]
        ),
        "lookahead_exact_32_equal": (
            [sample.get("checked_samples") for sample in timing_lookahead]
            == [
                sample.get("checked_samples")
                for sample in observation_lookahead
            ]
            and bool(timing_lookahead)
            and all(
                sample.get("checked_samples") == 32
                for sample in (*timing_lookahead, *observation_lookahead)
            )
        ),
    }
    parity_passed = bool(comparisons) and all(comparisons.values())
    artifact = {
        "schema_version": SCHEMA,
        "generator_script_sha256": generator_sha256,
        "input_manifest": [
            {
                "role": "baseline_timing_parity_probe",
                "path": str(timing_path),
                "sha256": timing_sha256,
            },
            {
                "role": "baseline_observation_parity_probe",
                "path": str(observation_path),
                "sha256": observation_sha256,
            },
        ],
        "timing_source_identity": nested(
            timing, "identity", "source_identity"
        ),
        "observation_source_identity": nested(
            observation, "identity", "source_identity"
        ),
        "specs_source_identity": nested(
            timing, "identity", "specs_source_identity"
        ),
        "specs_contract_identity": nested(
            timing, "identity", "specs_contract_identity"
        ),
        "timing_probe_sha256": timing_sha256,
        "observation_probe_sha256": observation_sha256,
        "timing_probe_run_id": nested(timing, "identity", "run_id"),
        "observation_probe_run_id": nested(
            observation, "identity", "run_id"
        ),
        "timing_probe_process_instance_id": nested(
            timing, "identity", "process_instance_id"
        ),
        "observation_probe_process_instance_id": nested(
            observation, "identity", "process_instance_id"
        ),
        "timing_binary_sha256": nested(
            timing, "identity", "binary_sha256"
        ),
        "observation_binary_sha256": nested(
            observation, "identity", "binary_sha256"
        ),
        "fixture_identity": fixture_identity(timing),
        "timing_output": timing_pair_output,
        "observation_output": observation_pair_output,
        "comparisons": comparisons,
        "parity_passed": parity_passed,
    }
    rendered = json.dumps(artifact, indent=2, sort_keys=True, allow_nan=False)
    payload = (rendered + "\n").encode("utf-8")
    try:
        print(rendered)
    except BrokenPipeError:
        return 2
    staged = stage_output(output, payload)
    try:
        if (
            sha256_file(generator_path) != generator_sha256
            or sha256_file(timing_path) != timing_sha256
            or sha256_file(observation_path) != observation_sha256
        ):
            raise ValueError(
                "parity generator or retained probe input changed during generation"
            )
        require_probe_checkout_identities((timing, observation))
        if hashlib.sha256(staged.read_bytes()).digest() != hashlib.sha256(
            payload
        ).digest():
            raise ValueError("staged parity artifact changed before publication")
        if validated_output(args.output, (timing, observation)) != output:
            raise ValueError("parity output destination changed")
        publish_staged_exclusive(staged, output)
    finally:
        staged.unlink(missing_ok=True)
    return 0 if parity_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
