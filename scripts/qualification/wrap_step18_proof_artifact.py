#!/usr/bin/env python3
"""Bind one Step 18 proof-cell payload to typed execution provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

from source_identity import (
    SourceIdentityError,
    validate_identity,
    validate_specs_contract_identity,
)


SCHEMA = "manyforge.step18.proof_cell_artifact.v2"
CELLS = (
    "lightweight_collision_only",
    "policy_support_contact",
    "precise_reference_parity",
    "contact_grasp",
    "free_space",
)
LEGACY_SOURCE_FIELDS = (
    "source_commit",
    "specs_commit",
    "specs_candidate_sha256",
    "working_tree_candidate_sha256",
)
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def sha256_string(value: str) -> bool:
    return len(value) == 64 and all(char in "0123456789abcdef" for char in value)


def nullable_sha256(value: str | None, option: str) -> str | None:
    if value is None:
        return None
    if not sha256_string(value):
        raise ValueError(f"{option} must be a lowercase SHA-256")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--cell", required=True, choices=CELLS)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--process-instance-id", required=True)
    parser.add_argument("--source-role", required=True)
    parser.add_argument("--source-identity-json", required=True)
    parser.add_argument("--specs-source-identity-json", required=True)
    parser.add_argument("--specs-contract-identity-json", required=True)
    parser.add_argument("--expected-source-fingerprint-sha256", required=True)
    parser.add_argument(
        "--expected-specs-source-fingerprint-sha256", required=True
    )
    parser.add_argument(
        "--expected-specs-contract-fingerprint-sha256", required=True
    )
    parser.add_argument("--image-id", required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--requested-cpu-affinity", required=True)
    parser.add_argument("--warmup-runs", required=True, type=int)
    parser.add_argument("--measured-runs", required=True, type=int)
    parser.add_argument(
        "--checkout-base-clean", required=True, choices=("0", "1")
    )
    parser.add_argument("--clean-checkout-manifest-sha256")
    parser.add_argument("--qualification-patch-sha256")
    parser.add_argument("--runner-script-sha256", required=True)
    parser.add_argument("--wrapper-script-sha256", required=True)
    parser.add_argument("--source-identity-helper-sha256", required=True)
    parser.add_argument("--runner-command-json", required=True)
    parser.add_argument("--build-command-json", required=True)
    parser.add_argument("--outer-command-json", required=True)
    parser.add_argument("--runner-environment-json", required=True)
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="exact benchmark argv after a -- separator",
    )
    return parser.parse_args()


def _argv(value: str, label: str) -> list[str]:
    parsed = json.loads(value)
    if not (
        isinstance(parsed, list)
        and parsed
        and all(isinstance(item, str) and item for item in parsed)
    ):
        raise ValueError(f"{label} must be a non-empty argv array")
    return parsed


def _source_identity(value: str, label: str) -> dict[str, Any]:
    try:
        return validate_identity(json.loads(value))
    except (json.JSONDecodeError, SourceIdentityError) as error:
        raise ValueError(f"{label} is invalid: {error}") from error


def _specs_contract_identity(value: str) -> dict[str, Any]:
    try:
        return validate_specs_contract_identity(json.loads(value))
    except (json.JSONDecodeError, SourceIdentityError) as error:
        raise ValueError(f"Specs contract identity is invalid: {error}") from error


def _payload(raw: bytes) -> dict[str, Any]:
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("proof payload must be one JSON object")
    return value


def main() -> int:
    args = parse_args()
    wrapper_path = Path(__file__).resolve()
    source_identity_helper_path = wrapper_path.with_name("source_identity.py")
    try:
        observed_wrapper_sha256 = sha256_file(wrapper_path)
        observed_source_identity_helper_sha256 = sha256_file(
            source_identity_helper_path
        )
    except OSError as error:
        raise SystemExit(f"unable to snapshot proof wrapper tools: {error}") from error
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command or not all(isinstance(value, str) and value for value in command):
        raise SystemExit("an exact non-empty command is required after --")
    if not args.run_id or UUID.fullmatch(args.process_instance_id) is None:
        raise SystemExit("run id and a canonical UUID process instance id are required")
    if args.warmup_runs != 5 or args.measured_runs != 50:
        raise SystemExit("Step 18 proof artifacts require exactly 5 warmup and 50 measured runs")
    for value, option in (
        (args.expected_source_fingerprint_sha256, "--expected-source-fingerprint-sha256"),
        (
            args.expected_specs_source_fingerprint_sha256,
            "--expected-specs-source-fingerprint-sha256",
        ),
        (
            args.expected_specs_contract_fingerprint_sha256,
            "--expected-specs-contract-fingerprint-sha256",
        ),
        (args.image_id, "--image-id"),
        (args.binary_sha256, "--binary-sha256"),
        (args.runner_script_sha256, "--runner-script-sha256"),
        (args.wrapper_script_sha256, "--wrapper-script-sha256"),
        (args.source_identity_helper_sha256, "--source-identity-helper-sha256"),
    ):
        if not sha256_string(value):
            raise SystemExit(f"{option} must be a lowercase SHA-256")
    if args.wrapper_script_sha256 != observed_wrapper_sha256:
        raise SystemExit("wrapper script hash does not match the executing wrapper")
    if (
        args.source_identity_helper_sha256
        != observed_source_identity_helper_sha256
    ):
        raise SystemExit(
            "source identity helper hash does not match the executing tool bundle"
        )
    try:
        clean_manifest = nullable_sha256(
            args.clean_checkout_manifest_sha256,
            "--clean-checkout-manifest-sha256",
        )
        qualification_patch = nullable_sha256(
            args.qualification_patch_sha256,
            "--qualification-patch-sha256",
        )
        raw = args.input.read_bytes()
        payload = _payload(raw)
        source_identity = _source_identity(
            args.source_identity_json, "source identity"
        )
        specs_source_identity = _source_identity(
            args.specs_source_identity_json, "Specs source identity"
        )
        specs_contract_identity = _specs_contract_identity(
            args.specs_contract_identity_json
        )
        runner_command = _argv(args.runner_command_json, "runner command")
        build_command = _argv(args.build_command_json, "build command")
        outer_command = _argv(args.outer_command_json, "outer command")
        runner_environment = json.loads(args.runner_environment_json)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(f"proof payload is invalid: {error}") from error
    if (
        source_identity["fingerprint_sha256"]
        != args.expected_source_fingerprint_sha256
        or specs_source_identity["fingerprint_sha256"]
        != args.expected_specs_source_fingerprint_sha256
        or specs_contract_identity["contract_fingerprint_sha256"]
        != args.expected_specs_contract_fingerprint_sha256
        or specs_contract_identity["head_commit"]
        != specs_source_identity["head_commit"]
        or specs_contract_identity["head_tree"]
        != specs_source_identity["head_tree"]
    ):
        raise SystemExit("observed source identity differs from the frozen expectation")
    if not isinstance(runner_environment, dict) or not runner_environment:
        raise SystemExit("runner environment provenance must be a non-empty object")
    if any(
        field in payload
        for field in (
            "source_identity",
            "specs_source_identity",
            "specs_contract_identity",
        )
    ):
        raise SystemExit("raw payload must not pre-populate wrapper-owned source identities")
    for field in LEGACY_SOURCE_FIELDS:
        payload.pop(field, None)
    payload["source_identity"] = source_identity
    payload["specs_source_identity"] = specs_source_identity
    payload["specs_contract_identity"] = specs_contract_identity
    payload["process_instance_id"] = args.process_instance_id
    canonical_payload = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")
    artifact = {
        "schema_version": SCHEMA,
        "cell": args.cell,
        "identity": {
            "run_id": args.run_id,
            "process_instance_id": args.process_instance_id,
            "source_role": args.source_role,
            "source_identity": source_identity,
            "specs_source_identity": specs_source_identity,
            "specs_contract_identity": specs_contract_identity,
            "image_id": args.image_id,
            "binary_sha256": args.binary_sha256,
            "requested_cpu_affinity": args.requested_cpu_affinity,
        },
        "provenance": {
            "expected_source_fingerprint_sha256": (
                args.expected_source_fingerprint_sha256
            ),
            "expected_specs_source_fingerprint_sha256": (
                args.expected_specs_source_fingerprint_sha256
            ),
            "expected_specs_contract_fingerprint_sha256": (
                args.expected_specs_contract_fingerprint_sha256
            ),
            "measurement_profile": {
                "warmup_runs": args.warmup_runs,
                "measured_runs": args.measured_runs,
            },
            "checkout_base_clean": args.checkout_base_clean == "1",
            "clean_checkout_manifest_sha256": clean_manifest,
            "qualification_patch_sha256": qualification_patch,
            "command_argv": command,
            "runner_command_argv": runner_command,
            "build_command_argv": build_command,
            "outer_command_argv": outer_command,
            "runner_environment": runner_environment,
            "runner_script_sha256": args.runner_script_sha256,
            "wrapper_script_sha256": args.wrapper_script_sha256,
            "source_identity_helper_sha256": args.source_identity_helper_sha256,
            "raw_payload_sha256": sha256_bytes(raw),
            "payload_canonical_sha256": sha256_bytes(canonical_payload),
        },
        "payload": payload,
    }
    rendered = json.dumps(artifact, indent=2, sort_keys=True, allow_nan=False)
    try:
        tools_unchanged = (
            sha256_file(wrapper_path) == observed_wrapper_sha256
            and sha256_file(source_identity_helper_path)
            == observed_source_identity_helper_sha256
        )
    except OSError:
        tools_unchanged = False
    if not tools_unchanged:
        raise SystemExit("proof wrapper tool bundle changed while wrapping")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
