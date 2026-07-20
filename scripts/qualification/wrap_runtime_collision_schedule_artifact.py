#!/usr/bin/env python3
"""Bind one native runtime-schedule record to its host runner provenance."""

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


RECORD_SCHEMA = "manyforge.runtime_collision_schedule_qualification.v2"
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)


def sha256_string(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_json(value: str, label: str, expected: type) -> Any:
    parsed = json.loads(value)
    if not isinstance(parsed, expected):
        raise ValueError(f"{label} must encode {expected.__name__}")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--process-instance-id", required=True)
    parser.add_argument("--source-identity-json", required=True)
    parser.add_argument("--specs-source-identity-json", required=True)
    parser.add_argument("--specs-contract-identity-json", required=True)
    parser.add_argument("--runner-command-json", required=True)
    parser.add_argument("--build-commands-json", required=True)
    parser.add_argument("--outer-command-json", required=True)
    parser.add_argument("--runner-environment-json", required=True)
    parser.add_argument("--runner-script-sha256", required=True)
    parser.add_argument("--source-identity-script-sha256", required=True)
    parser.add_argument("--prepare-script-sha256")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    wrapper_path = Path(__file__).resolve()
    source_identity_path = wrapper_path.with_name("source_identity.py")
    try:
        raw_record = args.input.read_bytes()
        record = json.loads(raw_record)
        source_identity = validate_identity(
            parse_json(args.source_identity_json, "source identity", dict)
        )
        specs_identity = validate_identity(
            parse_json(args.specs_source_identity_json, "Specs identity", dict)
        )
        specs_contract_identity = validate_specs_contract_identity(
            parse_json(
                args.specs_contract_identity_json,
                "Specs contract identity",
                dict,
            )
        )
        runner_command = parse_json(
            args.runner_command_json, "runner command", list
        )
        build_commands = parse_json(
            args.build_commands_json, "build commands", list
        )
        outer_command = parse_json(
            args.outer_command_json, "outer command", list
        )
        runner_environment = parse_json(
            args.runner_environment_json, "runner environment", dict
        )
        wrapper_sha256 = sha256_file(wrapper_path)
        source_identity_sha256 = sha256_file(source_identity_path)
    except (OSError, ValueError, json.JSONDecodeError, SourceIdentityError) as error:
        raise SystemExit(f"runtime artifact wrapping failed: {error}") from error
    if not isinstance(record, dict) or record.get("schema_version") != RECORD_SCHEMA:
        raise SystemExit("native runtime artifact has an unexpected schema")
    identity = record.get("identity")
    provenance = record.get("provenance")
    if not isinstance(identity, dict) or not isinstance(provenance, dict):
        raise SystemExit("native runtime artifact lacks identity/provenance")
    if (
        UUID.fullmatch(args.process_instance_id) is None
        or identity.get("process_instance_id") != args.process_instance_id
    ):
        raise SystemExit("native process instance does not match the host runner")
    if identity.get("source_identity") != source_identity:
        raise SystemExit("native source identity does not match the host runner")
    if identity.get("specs_source_identity") != specs_identity:
        raise SystemExit("native Specs identity does not match the host runner")
    if identity.get("specs_contract_identity") != specs_contract_identity:
        raise SystemExit(
            "native Specs contract identity does not match the host runner"
        )
    if (
        specs_contract_identity["head_commit"] != specs_identity["head_commit"]
        or specs_contract_identity["head_tree"] != specs_identity["head_tree"]
    ):
        raise SystemExit("full Specs and Specs-contract identities disagree")
    if not all(
        isinstance(command, list)
        and command
        and all(isinstance(item, str) and item for item in command)
        for command in (runner_command, outer_command)
    ):
        raise SystemExit("runner and outer commands must be non-empty argv arrays")
    if not (
        isinstance(build_commands, list)
        and build_commands
        and all(
            isinstance(command, list)
            and command
            and all(isinstance(item, str) and item for item in command)
            for command in build_commands
        )
    ):
        raise SystemExit("build commands must be a non-empty array of argv arrays")
    if not runner_environment:
        raise SystemExit("runner environment must be non-empty")
    for value, label in (
        (args.runner_script_sha256, "runner script"),
        (args.source_identity_script_sha256, "source identity script"),
    ):
        if not sha256_string(value):
            raise SystemExit(f"{label} hash must be a lowercase SHA-256")
    if args.prepare_script_sha256 is not None and not sha256_string(
        args.prepare_script_sha256
    ):
        raise SystemExit("prepare script hash must be a lowercase SHA-256")
    if args.source_identity_script_sha256 != source_identity_sha256:
        raise SystemExit("source identity script hash does not match the executing tool")
    provenance.update(
        {
            "runner_command_argv": runner_command,
            "build_commands_argv": build_commands,
            "outer_command_argv": outer_command,
            "runner_environment": runner_environment,
            "runner_script_sha256": args.runner_script_sha256,
            "wrapper_script_sha256": wrapper_sha256,
            "prepare_script_sha256": args.prepare_script_sha256,
            "source_identity_script_sha256": (
                args.source_identity_script_sha256
            ),
            "raw_record_sha256": hashlib.sha256(raw_record).hexdigest(),
        }
    )
    rendered = json.dumps(record, indent=2, sort_keys=True, allow_nan=False) + "\n"
    try:
        tools_unchanged = (
            sha256_file(wrapper_path) == wrapper_sha256
            and sha256_file(source_identity_path) == source_identity_sha256
        )
    except OSError:
        tools_unchanged = False
    if not tools_unchanged:
        raise SystemExit("runtime wrapper tool bundle changed while wrapping")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
