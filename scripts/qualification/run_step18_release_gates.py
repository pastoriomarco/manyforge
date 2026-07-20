#!/usr/bin/env python3
"""Run or assemble the seven corrected-source Step 18 release gates.

Each ``lane`` invocation creates one isolated container, one unique build
directory, and a provenance-bound lane manifest.  ``assemble`` accepts the
seven independently produced manifests and invokes the strict aggregate
verifier.  It does not run the other Step 18 evidence families.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

from source_identity import (
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
)
from verify_step18_release_gates import (
    ARTIFACT_MANIFEST_SCHEMA,
    LANE_SCHEMA,
    LANES,
    VERIFICATION_SCOPE,
    ValidationError,
    _verify_lane_manifest,
    expected_outer_command,
    lane_environment,
    lane_input_paths,
    lane_inner_script,
    lane_labels,
    lane_profile,
    lane_security_override,
    lane_security_options,
    lane_service,
    normalize_mounts,
    publish_verification_summary,
    verify_artifact_manifest,
)


REPO_ROOT = Path(__file__).resolve().parents[2]


class RunnerError(RuntimeError):
    """A lane could not produce a complete retained artifact."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _write_exclusive(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def _run(
    argv: list[str],
    *,
    check: bool = True,
    text: bool = True,
) -> subprocess.CompletedProcess[Any]:
    try:
        return subprocess.run(
            argv,
            check=check,
            capture_output=True,
            text=text,
        )
    except FileNotFoundError as error:
        raise RunnerError(f"required command is unavailable: {argv[0]}") from error
    except subprocess.CalledProcessError as error:
        stderr = error.stderr if isinstance(error.stderr, str) else ""
        stdout = error.stdout if isinstance(error.stdout, str) else ""
        detail = (stderr or stdout).strip()
        raise RunnerError(
            f"command failed ({error.returncode}): {' '.join(argv)}"
            + (f": {detail}" if detail else "")
        ) from error


def _source_bundle(specs_root: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    try:
        return source_identity(specs_root), specs_contract_identity(specs_root)
    except SourceIdentityError as error:
        raise RunnerError(f"cannot collect Specs identity: {error}") from error


def _candidate_source(repo_root: Path) -> dict[str, Any]:
    try:
        return source_identity(repo_root)
    except SourceIdentityError as error:
        raise RunnerError(f"cannot collect ManyForge identity: {error}") from error


def _validate_roots(repo_root: Path, specs_root: Path) -> tuple[Path, Path]:
    repo = repo_root.resolve()
    specs = specs_root.resolve()
    for root, label in ((repo, "ManyForge"), (specs, "Specs")):
        if not root.is_dir() or not (root / ".git").exists():
            raise RunnerError(f"{label} checkout is unavailable: {root}")
    if not (repo / "docker-compose.yaml").is_file():
        raise RunnerError(f"ManyForge compose file is unavailable: {repo}")
    return repo, specs


def _validate_output_path(repo_root: Path, output_dir: Path) -> Path:
    output = output_dir.resolve()
    allowed = (repo_root / ".manyforge-var" / "qualification").resolve()
    try:
        output.relative_to(allowed)
    except ValueError as error:
        raise RunnerError(
            f"output must be below the ignored qualification root: {allowed}"
        ) from error
    if output == allowed:
        raise RunnerError("output must be a child of the qualification root")
    if output.exists():
        raise RunnerError(f"output already exists: {output}")
    ignored = subprocess.run(
        ["git", "-C", str(repo_root), "check-ignore", "--quiet", "--", str(output)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ignored.returncode != 0:
        raise RunnerError("output is not covered by the candidate Git ignore rules")
    return output


def _compose_config(repo_root: Path, lane: str) -> dict[str, Any]:
    argv = [
        "docker",
        "compose",
        "-f",
        str(repo_root / "docker-compose.yaml"),
    ]
    profile = lane_profile(lane)
    if profile is not None:
        argv.extend(["--profile", profile])
    argv.extend(["config", "--format", "json"])
    result = _run(argv)
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RunnerError(f"docker compose config did not return JSON: {error}") from error
    if not isinstance(value, dict):
        raise RunnerError("docker compose config did not return an object")
    return value


def _image_identity(repo_root: Path, lane: str) -> tuple[str, str]:
    config = _compose_config(repo_root, lane)
    services = config.get("services")
    service = lane_service(lane)
    if not isinstance(services, dict) or not isinstance(services.get(service), dict):
        raise RunnerError(f"compose config does not contain service {service}")
    reference = services[service].get("image")
    if not isinstance(reference, str) or not reference:
        # Compose omits ``image`` from ``config`` for a build-only service, but
        # gives the built service image an exact generated repository name.
        # Resolve that canonical service reference from configuration rather
        # than from ``compose images``, whose output depends on which service
        # containers happen to exist on the host.
        argv = [
            "docker",
            "compose",
            "-f",
            str(repo_root / "docker-compose.yaml"),
        ]
        profile = lane_profile(lane)
        if profile is not None:
            argv.extend(["--profile", profile])
        argv.extend(["config", "--images", service])
        result = _run(argv)
        references = [
            line.strip() for line in result.stdout.splitlines() if line.strip()
        ]
        if len(references) != 1:
            raise RunnerError(
                f"compose service {service} does not resolve to exactly one configured image"
            )
        reference = references[0]
        if reference == "<none>":
            raise RunnerError(
                f"compose service {service} has no configured built-image reference"
            )
    result = _run(
        ["docker", "image", "inspect", reference, "--format", "{{.Id}}"]
    )
    image_id = result.stdout.strip().removeprefix("sha256:")
    if len(image_id) != 64 or any(character not in "0123456789abcdef" for character in image_id):
        raise RunnerError(f"compose image is unavailable or has invalid id: {reference}")
    return reference, image_id


def _inspect(container_name: str) -> tuple[dict[str, Any], bytes]:
    result = _run(["docker", "inspect", container_name], text=False)
    raw = result.stdout
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RunnerError(f"docker inspect is invalid JSON: {error}") from error
    if not isinstance(value, list) or len(value) != 1 or not isinstance(value[0], dict):
        raise RunnerError("docker inspect did not identify exactly one container")
    return value[0], raw


def _script_hashes(repo_root: Path, lane: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for relative in lane_input_paths(lane):
        path = repo_root / relative
        if not path.is_file():
            raise RunnerError(f"required release-gate input is missing: {relative}")
        result[relative] = _sha256(path)
    return result


def _write_container_log(container_name: str, path: Path) -> None:
    with path.open("xb") as stream:
        try:
            result = subprocess.run(
                ["docker", "logs", container_name],
                stdout=stream,
                stderr=subprocess.STDOUT,
                check=False,
            )
        except FileNotFoundError as error:
            raise RunnerError("docker is unavailable while retaining logs") from error
    if result.returncode != 0:
        raise RunnerError(
            f"docker logs failed for {container_name}: status={result.returncode}"
        )


def run_lane(
    *,
    repo_root: Path,
    specs_root: Path,
    output_dir: Path,
    lane: str,
) -> Path:
    if lane not in LANES:
        raise RunnerError(f"unknown release-gate lane: {lane}")
    repo, specs = _validate_roots(repo_root, specs_root)
    output = _validate_output_path(repo, output_dir)
    manyforge_before = _candidate_source(repo)
    specs_before, contract_before = _source_bundle(specs)
    image_reference, declared_image_id = _image_identity(repo, lane)
    script_hashes_before = _script_hashes(repo, lane)

    run_id = str(uuid.uuid4())
    container_name = f"manyforge-step18-release-{lane.replace('_', '-')}-{run_id}"
    inner_script = lane_inner_script(lane)
    environment = lane_environment(
        lane,
        run_id,
        manyforge_before["fingerprint_sha256"],
        specs_before["fingerprint_sha256"],
        contract_before["contract_fingerprint_sha256"],
    )
    labels = lane_labels(
        lane,
        run_id,
        manyforge_before["fingerprint_sha256"],
        specs_before["fingerprint_sha256"],
        contract_before["contract_fingerprint_sha256"],
        _sha256_text(inner_script),
    )
    output.mkdir(parents=True)
    security_override_path: Path | None = None
    security_override_content = lane_security_override(lane)
    if security_override_content is not None:
        security_override_path = output / "compose-security.override.yaml"
        with security_override_path.open("x", encoding="utf-8") as stream:
            stream.write(security_override_content)
    outer_command = expected_outer_command(
        repo_root=repo,
        specs_root=specs,
        lane=lane,
        container_name=container_name,
        environment=environment,
        labels=labels,
        security_override_path=security_override_path,
    )

    inspect_path = output / "container-inspect.json"
    log_path = output / "container.log"
    manifest_path = output / "lane-manifest.json"
    container_created = False
    inspect_record: dict[str, Any] | None = None
    docker_wait_status = -1
    try:
        creation = _run(outer_command, check=False)
        if creation.returncode != 0:
            detail = (creation.stderr or creation.stdout).strip()
            raise RunnerError(
                f"Docker could not create {lane} container: status={creation.returncode}"
                + (f": {detail}" if detail else "")
            )
        container_created = True
        wait = _run(["docker", "wait", container_name], check=False)
        docker_wait_status = wait.returncode
        _write_container_log(container_name, log_path)
        inspect_record, inspect_raw = _inspect(container_name)
        inspect_path.write_bytes(inspect_raw)

        manyforge_after = _candidate_source(repo)
        specs_after, contract_after = _source_bundle(specs)
        script_hashes_after = _script_hashes(repo, lane)
        state = inspect_record.get("State")
        config = inspect_record.get("Config")
        host_config = inspect_record.get("HostConfig")
        if (
            not isinstance(state, dict)
            or not isinstance(config, dict)
            or not isinstance(host_config, dict)
        ):
            raise RunnerError("container inspect omitted State, Config, or HostConfig")
        container_id = inspect_record.get("Id")
        observed_image_id = str(inspect_record.get("Image", "")).removeprefix("sha256:")
        observed_name = str(inspect_record.get("Name", "")).removeprefix("/")
        if not isinstance(container_id, str) or not container_id:
            raise RunnerError("container inspect omitted its exact id")

        manifest = {
            "schema_version": LANE_SCHEMA,
            "verification_scope": VERIFICATION_SCOPE,
            "lane": lane,
            "run_id": run_id,
            "paths": {
                "manyforge_root": str(repo),
                "specs_root": str(specs),
            },
            "identities": {
                "manyforge_before": manyforge_before,
                "manyforge_after": manyforge_after,
                "specs_before": specs_before,
                "specs_after": specs_after,
                "specs_contract_before": contract_before,
                "specs_contract_after": contract_after,
            },
            "execution": {
                "compose_service": lane_service(lane),
                "compose_profile": lane_profile(lane),
                "container_id": container_id,
                "container_name": observed_name,
                "image_reference": image_reference,
                "declared_image_id": declared_image_id,
                "image_id": observed_image_id,
                "outer_command_argv": outer_command,
                "requested_container_argv": ["/bin/bash", "-lc", inner_script],
                "configured_process_argv": [
                    inspect_record.get("Path"),
                    *inspect_record.get("Args", []),
                ],
                "required_environment": environment,
                "configured_environment": config.get("Env"),
                "required_labels": labels,
                "security_override": (
                    {
                        "path": str(security_override_path),
                        "sha256": _sha256(security_override_path),
                        "bytes": security_override_path.stat().st_size,
                    }
                    if security_override_path is not None
                    else None
                ),
                "security_options": host_config.get("SecurityOpt") or [],
                "mounts": normalize_mounts(inspect_record.get("Mounts")),
                "terminal_status": {
                    "docker_wait_status": docker_wait_status,
                    "exit_code": state.get("ExitCode"),
                    "oom_killed": state.get("OOMKilled"),
                    "error": state.get("Error"),
                },
            },
            "artifacts": {
                "container_inspect": {
                    "path": str(inspect_path),
                    "sha256": _sha256(inspect_path),
                    "bytes": inspect_path.stat().st_size,
                },
                "container_log": {
                    "path": str(log_path),
                    "sha256": _sha256(log_path),
                    "bytes": log_path.stat().st_size,
                },
            },
            "script_hashes": {
                "before": script_hashes_before,
                "after": script_hashes_after,
            },
            "overall_step18_closure_claimed": False,
            "requires_common_candidate_aggregator": True,
        }
        _write_exclusive(manifest_path, manifest)

        if declared_image_id != observed_image_id or image_reference != config.get("Image"):
            raise RunnerError("qualification image changed between declaration and execution")
        _verify_lane_manifest(
            manifest_path=manifest_path,
            candidate_source=manyforge_before,
            candidate_specs_source=specs_before,
            candidate_specs_contract=contract_before,
            repo_root=repo,
            specs_root=specs,
        )
    finally:
        if container_created:
            subprocess.run(
                ["docker", "rm", "-f", container_name],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    print(f"Step 18 release-gate lane passed: {lane}")
    print(f"lane manifest: {manifest_path}")
    return manifest_path


def assemble(
    *,
    repo_root: Path,
    specs_root: Path,
    output_dir: Path,
    lane_manifests: list[Path],
) -> tuple[Path, Path]:
    repo, specs = _validate_roots(repo_root, specs_root)
    output = _validate_output_path(repo, output_dir)
    if len(lane_manifests) != len(LANES):
        raise RunnerError("assemble requires exactly seven --lane-manifest values")
    manyforge = _candidate_source(repo)
    specs_source, specs_contract = _source_bundle(specs)

    references: list[dict[str, Any]] = []
    observed_lanes: list[str] = []
    for raw in lane_manifests:
        path = raw.resolve()
        if not path.is_file():
            raise RunnerError(f"lane manifest is unavailable: {path}")
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise RunnerError(f"lane manifest is invalid JSON: {path}: {error}") from error
        if not isinstance(value, dict) or value.get("schema_version") != LANE_SCHEMA:
            raise RunnerError(f"lane manifest does not use {LANE_SCHEMA}: {path}")
        lane = value.get("lane")
        if lane not in LANES:
            raise RunnerError(f"lane manifest names an unknown lane: {path}")
        observed_lanes.append(lane)
        references.append(
            {
                "lane": lane,
                "manifest_path": str(path),
                "manifest_sha256": _sha256(path),
                "manifest_bytes": path.stat().st_size,
            }
        )
    if len(set(observed_lanes)) != len(LANES) or set(observed_lanes) != set(LANES):
        raise RunnerError("lane manifests are missing or duplicated")
    references.sort(key=lambda item: LANES.index(item["lane"]))

    output.mkdir(parents=True)
    artifact_manifest = output / "artifact-manifest.json"
    verification_summary = output / "verification-summary.json"
    _write_exclusive(
        artifact_manifest,
        {
            "schema_version": ARTIFACT_MANIFEST_SCHEMA,
            "verification_scope": VERIFICATION_SCOPE,
            "candidate": {
                "manyforge_root": str(repo),
                "specs_root": str(specs),
                "source_identity": manyforge,
                "specs_source_identity": specs_source,
                "specs_contract_identity": specs_contract,
            },
            "lanes": references,
            "overall_step18_closure_claimed": False,
            "requires_common_candidate_aggregator": True,
        },
    )
    result = verify_artifact_manifest(artifact_manifest)
    publish_verification_summary(verification_summary, result, artifact_manifest)
    print("Step 18 corrected-source release gates passed")
    print(f"artifact manifest: {artifact_manifest}")
    print(f"verification summary: {verification_summary}")
    return artifact_manifest, verification_summary


def describe_lane(lane: str) -> dict[str, Any]:
    inner = lane_inner_script(lane)
    return {
        "lane": lane,
        "compose_service": lane_service(lane),
        "compose_profile": lane_profile(lane),
        "security_options": lane_security_options(lane),
        "input_paths": list(lane_input_paths(lane)),
        "requested_container_argv": ["/bin/bash", "-lc", inner],
        "inner_script_sha256": _sha256_text(inner),
        "unique_build_root_template": (
            f"/workspace/build/step18-release-gates/<uuid>/{lane}"
        ),
        "overall_step18_closure_claimed": False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    lane_parser = subparsers.add_parser("lane", help="run one independently retained lane")
    lane_parser.add_argument("--lane", choices=LANES, required=True)
    lane_parser.add_argument("--specs-root", type=Path, required=True)
    lane_parser.add_argument("--output-dir", type=Path, required=True)
    lane_parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)

    assemble_parser = subparsers.add_parser("assemble", help="verify and aggregate seven lanes")
    assemble_parser.add_argument("--specs-root", type=Path, required=True)
    assemble_parser.add_argument("--output-dir", type=Path, required=True)
    assemble_parser.add_argument("--lane-manifest", type=Path, action="append", required=True)
    assemble_parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)

    describe_parser = subparsers.add_parser(
        "describe", help="print a lane's frozen Docker contract"
    )
    describe_parser.add_argument("--lane", choices=LANES, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "describe":
            print(json.dumps(describe_lane(args.lane), indent=2, sort_keys=True))
            return 0
        if args.command == "lane":
            run_lane(
                repo_root=args.repo_root,
                specs_root=args.specs_root,
                output_dir=args.output_dir,
                lane=args.lane,
            )
        else:
            assemble(
                repo_root=args.repo_root,
                specs_root=args.specs_root,
                output_dir=args.output_dir,
                lane_manifests=args.lane_manifest,
            )
    except (OSError, RunnerError, ValidationError, ValueError) as error:
        print(f"Step 18 release-gate runner failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
