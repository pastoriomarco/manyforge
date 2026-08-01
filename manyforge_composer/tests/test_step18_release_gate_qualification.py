from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_QUALIFICATION = _REPO_ROOT / "scripts/qualification"
if str(_QUALIFICATION) not in sys.path:
    sys.path.insert(0, str(_QUALIFICATION))


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


_SOURCE = _load("step18_release_source_identity", _QUALIFICATION / "source_identity.py")
sys.modules.setdefault("source_identity", _SOURCE)
_VERIFY = _load(
    "step18_release_verifier",
    _QUALIFICATION / "verify_step18_release_gates.py",
)
sys.modules.setdefault("verify_step18_release_gates", _VERIFY)
_RUNNER = _load(
    "step18_release_runner",
    _QUALIFICATION / "run_step18_release_gates.py",
)
_CLOSURE = _load(
    "step18_closure_verifier_for_release_integration",
    _QUALIFICATION / "verify_step18_closure.py",
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write(path: Path, value: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(value, bytes):
        path.write_bytes(value)
    else:
        path.write_text(value, encoding="utf-8")


def _json_write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


_DISTANCE_CAPABILITY_SOURCES = (
    "manyforge_core/include/manyforge_core/types/collision.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
    "manyforge_collision/src/manyforge_collision_engine.cpp",
    "manyforge_core/src/collision_validation_coordinator.cpp",
    "manyforge_core/src/runtime_collision_query_session.cpp",
)


def _copy_distance_capability_sources(destination: Path) -> Path:
    for relative in _DISTANCE_CAPABILITY_SOURCES:
        _write(destination / relative, (_REPO_ROOT / relative).read_text(encoding="utf-8"))
    return destination


def _commit_repo(root: Path, files: dict[str, str]) -> None:
    root.mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(
        ["git", "-C", str(root), "config", "user.email", "fixture@example.com"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(root), "config", "user.name", "Fixture"],
        check=True,
    )
    for relative, content in files.items():
        _write(root / relative, content)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(
        ["git", "-C", str(root), "commit", "-q", "-m", "fixture"],
        check=True,
    )


def _lane_log(lane: str) -> str:
    lines = [f"STEP18_RELEASE_GATE_BEGIN lane={lane}"]
    if lane in _VERIFY.ROS_FULL_LANES:
        lines.append("STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4")
    if lane == "full_dev_regression":
        lines.append("All 11 test suite(s) passed.")
    elif lane == "full_ros_regression":
        lines.extend(
            [
                "STEP18_GXX_VERSION=13.3.0",
                "STEP18_ROS_CTEST_BEGIN",
                "100% tests passed, 0 tests failed out of 59",
                "STEP18_ROS_CTEST_END status=0",
                "STEP18_ROS_PYTEST_BEGIN",
                "137 passed in 4.20s",
                "STEP18_ROS_PYTEST_END status=0",
            ]
        )
    elif lane == "contract_architecture_hygiene":
        for check_id, _ in _VERIFY.HYGIENE_CHECKS:
            lines.extend(
                [
                    f"STEP18_HYGIENE_CHECK_BEGIN id={check_id}",
                    "check output",
                    (
                        "STEP18_SPECIFIC_HYGIENE passed checks=12"
                        if check_id == "step18_removed_field_and_distance_scope"
                        else "check passed"
                    ),
                    f"STEP18_HYGIENE_CHECK_END id={check_id} status=0",
                ]
            )
    else:
        prefix = _VERIFY._SANITIZER_PREFIX[lane]  # noqa: SLF001
        lines.extend(
            [
                "STEP18_GXX_VERSION=13.3.0",
                f"{prefix}_REGISTRY registered_tests=12 enabled_tests=12 "
                "disabled_tests=0 unique_binaries=1",
                f"{prefix}_GROUP_SUMMARY binaries=1 registered_tests=12 "
                "enabled_tests=12 disabled_tests=0 ran=12 passed=12 skipped=0 "
                "fallback_binaries=0",
            ]
        )
    lines.append(f"STEP18_RELEASE_GATE_END lane={lane} status=0")
    return "\n".join(lines) + "\n"


def _mounts(manyforge: Path, specs: Path) -> list[dict[str, Any]]:
    return [
        {
            "Type": "bind",
            "Source": str(manyforge),
            "Destination": "/workspace",
            "Mode": "rw",
            "RW": True,
            "Propagation": "rprivate",
        },
        {
            "Type": "bind",
            "Source": str(specs),
            "Destination": "/step18-specs",
            "Mode": "ro",
            "RW": False,
            "Propagation": "rprivate",
        },
    ]


def _fixture(tmp_path: Path) -> dict[str, Any]:
    manyforge = tmp_path / "candidate"
    specs = tmp_path / "specs"
    inputs = sorted(
        {
            path
            for lane in _VERIFY.LANES
            for path in _VERIFY.lane_input_paths(lane)
        }
    )
    _commit_repo(
        manyforge,
        {
            ".gitignore": ".manyforge-var/\nbuild/\n",
            **{
                relative: f"fixture input: {relative}\n"
                for relative in inputs
            },
        },
    )
    _commit_repo(specs, {"docs/spec/contract.md": "fixture contract\n"})
    source = _SOURCE.source_identity(manyforge)
    specs_source = _SOURCE.source_identity(specs)
    contract = _SOURCE.specs_contract_identity(specs)
    evidence = tmp_path / "evidence"
    lane_paths: dict[str, Path] = {}

    for index, lane in enumerate(_VERIFY.LANES, start=1):
        lane_root = evidence / lane
        lane_root.mkdir(parents=True)
        run_id = str(uuid.UUID(int=index, version=4))
        inner = _VERIFY.lane_inner_script(lane)
        environment = _VERIFY.lane_environment(
            lane,
            run_id,
            source["fingerprint_sha256"],
            specs_source["fingerprint_sha256"],
            contract["contract_fingerprint_sha256"],
        )
        labels = _VERIFY.lane_labels(
            lane,
            run_id,
            source["fingerprint_sha256"],
            specs_source["fingerprint_sha256"],
            contract["contract_fingerprint_sha256"],
            hashlib.sha256(inner.encode()).hexdigest(),
        )
        container_name = f"fixture-{lane}-{index}"
        container_id = f"{index:064x}"
        image_id = "b" * 64 if lane in _VERIFY.ROS_FULL_LANES else "a" * 64
        image_reference = (
            "fixture-ros-full:latest"
            if lane in _VERIFY.ROS_FULL_LANES
            else "fixture-dev:latest"
        )
        configured_environment = [
            "FIXTURE_BASE_IMAGE=1",
            *(f"{key}={value}" for key, value in sorted(environment.items())),
        ]
        raw_mounts = _mounts(manyforge, specs)
        security_override_path: Path | None = None
        security_override_content = _VERIFY.lane_security_override(lane)
        if security_override_content is not None:
            security_override_path = lane_root / "compose-security.override.yaml"
            _write(security_override_path, security_override_content)
        inspect = [
            {
                "Id": container_id,
                "Image": f"sha256:{image_id}",
                "Name": f"/{container_name}",
                "Path": "/bin/bash",
                "Args": ["-lc", inner],
                "Config": {
                    "Image": image_reference,
                    "Entrypoint": ["/bin/bash"],
                    "Cmd": ["-lc", inner],
                    "Env": configured_environment,
                    "Labels": {"fixture": "1", **labels},
                },
                "HostConfig": {
                    "SecurityOpt": _VERIFY.lane_security_options(lane),
                },
                "Mounts": raw_mounts,
                "State": {
                    "ExitCode": 0,
                    "OOMKilled": False,
                    "Error": "",
                },
            }
        ]
        inspect_path = lane_root / "container-inspect.json"
        log_path = lane_root / "container.log"
        _json_write(inspect_path, inspect)
        _write(log_path, _lane_log(lane))
        script_hashes = {
            relative: _sha256(manyforge / relative)
            for relative in _VERIFY.lane_input_paths(lane)
        }
        manifest = {
            "schema_version": _VERIFY.LANE_SCHEMA,
            "verification_scope": _VERIFY.VERIFICATION_SCOPE,
            "lane": lane,
            "run_id": run_id,
            "paths": {
                "manyforge_root": str(manyforge),
                "specs_root": str(specs),
            },
            "identities": {
                "manyforge_before": source,
                "manyforge_after": source,
                "specs_before": specs_source,
                "specs_after": specs_source,
                "specs_contract_before": contract,
                "specs_contract_after": contract,
            },
            "execution": {
                "compose_service": _VERIFY.lane_service(lane),
                "compose_profile": _VERIFY.lane_profile(lane),
                "container_id": container_id,
                "container_name": container_name,
                "image_reference": image_reference,
                "declared_image_id": image_id,
                "image_id": image_id,
                "outer_command_argv": _VERIFY.expected_outer_command(
                    repo_root=manyforge,
                    specs_root=specs,
                    lane=lane,
                    container_name=container_name,
                    environment=environment,
                    labels=labels,
                    security_override_path=security_override_path,
                ),
                "requested_container_argv": ["/bin/bash", "-lc", inner],
                "configured_process_argv": ["/bin/bash", "-lc", inner],
                "required_environment": environment,
                "configured_environment": configured_environment,
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
                "security_options": _VERIFY.lane_security_options(lane),
                "mounts": _VERIFY.normalize_mounts(raw_mounts),
                "terminal_status": {
                    "docker_wait_status": 0,
                    "exit_code": 0,
                    "oom_killed": False,
                    "error": "",
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
            "script_hashes": {"before": script_hashes, "after": script_hashes},
            "overall_step18_closure_claimed": False,
            "requires_common_candidate_aggregator": True,
        }
        manifest_path = lane_root / "lane-manifest.json"
        _json_write(manifest_path, manifest)
        lane_paths[lane] = manifest_path

    artifact_manifest = evidence / "artifact-manifest.json"
    _json_write(
        artifact_manifest,
        {
            "schema_version": _VERIFY.ARTIFACT_MANIFEST_SCHEMA,
            "verification_scope": _VERIFY.VERIFICATION_SCOPE,
            "candidate": {
                "manyforge_root": str(manyforge),
                "specs_root": str(specs),
                "source_identity": source,
                "specs_source_identity": specs_source,
                "specs_contract_identity": contract,
            },
            "lanes": [
                {
                    "lane": lane,
                    "manifest_path": str(lane_paths[lane]),
                    "manifest_sha256": _sha256(lane_paths[lane]),
                    "manifest_bytes": lane_paths[lane].stat().st_size,
                }
                for lane in _VERIFY.LANES
            ],
            "overall_step18_closure_claimed": False,
            "requires_common_candidate_aggregator": True,
        },
    )
    return {
        "manifest": artifact_manifest,
        "manyforge": manyforge,
        "specs": specs,
        "source": source,
        "specs_source": specs_source,
        "contract": contract,
        "lanes": lane_paths,
    }


def _rebind_lane(fixture: dict[str, Any], lane: str) -> None:
    manifest_path = fixture["lanes"][lane]
    campaign = json.loads(fixture["manifest"].read_text(encoding="utf-8"))
    reference = next(item for item in campaign["lanes"] if item["lane"] == lane)
    reference["manifest_sha256"] = _sha256(manifest_path)
    reference["manifest_bytes"] = manifest_path.stat().st_size
    _json_write(fixture["manifest"], campaign)


def _rebind_artifact(
    fixture: dict[str, Any], lane: str, artifact_name: str
) -> dict[str, Any]:
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    path = Path(manifest["artifacts"][artifact_name]["path"])
    manifest["artifacts"][artifact_name]["sha256"] = _sha256(path)
    manifest["artifacts"][artifact_name]["bytes"] = path.stat().st_size
    _json_write(manifest_path, manifest)
    _rebind_lane(fixture, lane)
    return manifest


def test_release_gate_schema_and_exact_lane_names_are_frozen() -> None:
    assert _VERIFY.SUMMARY_SCHEMA == "manyforge.step18.release_gate_verification.v1"
    assert _VERIFY.LANES == (
        "full_dev_regression",
        "full_ros_regression",
        "contract_architecture_hygiene",
        "core_asan_ubsan",
        "core_tsan",
        "collision_asan_ubsan",
        "collision_tsan",
    )


def test_complete_release_gate_campaign_passes_without_claiming_step18(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    assert set(result) == {
        "schema_version",
        "verification_scope",
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
        "lanes",
        "artifact_manifest",
        "retained_artifact_manifest",
        "runner",
        "verifier",
        "failure_count",
        "release_gates_passed",
        "overall_step18_closure_claimed",
        "requires_common_candidate_aggregator",
    }
    assert result["schema_version"] == _VERIFY.SUMMARY_SCHEMA
    assert result["verification_scope"] == "corrected_source_release_gates"
    assert [lane["name"] for lane in result["lanes"]] == list(_VERIFY.LANES)
    assert all(lane["passed"] is True for lane in result["lanes"])
    assert result["release_gates_passed"] is True
    assert result["failure_count"] == 0
    assert result["overall_step18_closure_claimed"] is False
    assert result["requires_common_candidate_aggregator"] is True
    assert len(result["retained_artifact_manifest"]) == 28
    assert sum(
        item["role"] == "release_security_override"
        for item in result["retained_artifact_manifest"]
    ) == 4
    assert result["verifier"] == {
        "path": str(
            fixture["manyforge"]
            / "scripts/qualification/verify_step18_release_gates.py"
        ),
        "sha256": _sha256(
            fixture["manyforge"]
            / "scripts/qualification/verify_step18_release_gates.py"
        ),
    }


def test_actual_release_summary_is_consumed_by_common_closure_verifier(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    _CLOSURE._verify_release(  # noqa: SLF001
        result,
        fixture["source"],
        fixture["specs_source"],
        fixture["contract"],
    )


def test_release_retained_manifest_detects_postverification_override_mutation(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    override = next(
        item
        for item in result["retained_artifact_manifest"]
        if item["role"] == "release_security_override"
    )
    _write(Path(override["path"]), "services: {}\n")
    with pytest.raises(_VERIFY.ValidationError, match="retained artifact changed"):
        _VERIFY._assert_retained_artifact_manifest_current(  # noqa: SLF001
            result["retained_artifact_manifest"]
        )


def test_release_publication_is_atomic_when_retained_input_drifts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    override = next(
        item
        for item in result["retained_artifact_manifest"]
        if item["role"] == "release_security_override"
    )
    original_assertion = _VERIFY._assert_retained_artifact_manifest_current  # noqa: SLF001

    def drift_then_assert(value: Any) -> None:
        _write(Path(override["path"]), "services: {}\n")
        original_assertion(value)

    monkeypatch.setattr(
        _VERIFY,
        "_assert_retained_artifact_manifest_current",
        drift_then_assert,
    )
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/release-publication-fixture"
        / "verification-summary.json"
    )
    with pytest.raises(_VERIFY.ValidationError, match="retained artifact changed"):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()
    assert not list(output.parent.glob(f".{output.name}.tmp-*"))


def test_release_publication_never_overwrites_a_racing_publisher(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/release-publication-race"
        / "verification-summary.json"
    )
    racing_payload = "concurrent publisher won\n"
    real_link = _VERIFY.os.link

    def publish_racer_then_link(source, destination, *args, **kwargs) -> None:
        destination_descriptor = kwargs["dst_dir_fd"]
        descriptor = _VERIFY.os.open(
            destination,
            _VERIFY.os.O_WRONLY
            | _VERIFY.os.O_CREAT
            | _VERIFY.os.O_EXCL
            | _VERIFY.os.O_CLOEXEC
            | _VERIFY.os.O_NOFOLLOW,
            0o600,
            dir_fd=destination_descriptor,
        )
        try:
            _VERIFY.os.write(descriptor, racing_payload.encode("utf-8"))
        finally:
            _VERIFY.os.close(descriptor)
        real_link(source, destination, *args, **kwargs)

    monkeypatch.setattr(_VERIFY.os, "link", publish_racer_then_link)
    with pytest.raises(FileExistsError):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert output.read_text(encoding="utf-8") == racing_payload
    assert not list(output.parent.glob(f".{output.name}.tmp-*"))


def test_release_verifier_rejects_linked_and_unbounded_evidence(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    linked_manifest = tmp_path / "artifact-manifest-link.json"
    linked_manifest.symlink_to(fixture["manifest"])
    with pytest.raises(_VERIFY.ValidationError, match="symbolic-link traversal|open"):
        _VERIFY.verify_artifact_manifest(linked_manifest)

    lane = _VERIFY.LANES[0]
    lane_manifest_path = fixture["lanes"][lane]
    lane_manifest = json.loads(lane_manifest_path.read_text(encoding="utf-8"))
    lane_manifest["artifacts"]["container_log"]["bytes"] = (
        _VERIFY.MAX_CONTAINER_LOG_BYTES + 1
    )
    _json_write(lane_manifest_path, lane_manifest)
    _rebind_lane(fixture, lane)
    with pytest.raises(_VERIFY.ValidationError, match="invalid or unbounded"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])

    oversized = tmp_path / "oversized-artifact-manifest.json"
    _write(oversized, b" " * (_VERIFY.MAX_ARTIFACT_MANIFEST_BYTES + 1))
    with pytest.raises(_VERIFY.ValidationError, match="byte bound"):
        _VERIFY.verify_artifact_manifest(oversized)


def test_release_verifier_rejects_hardlinked_and_ambiguous_json_inputs(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    alias = tmp_path / "artifact-manifest-alias.json"
    alias.hardlink_to(fixture["manifest"])
    with pytest.raises(_VERIFY.ValidationError, match="non-hardlinked"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])

    duplicate = tmp_path / "duplicate-artifact-manifest.json"
    _write(
        duplicate,
        '{"schema_version":"first","schema_version":"second"}\n',
    )
    with pytest.raises(_VERIFY.ValidationError, match="repeats a field"):
        _VERIFY.verify_artifact_manifest(duplicate)


def test_release_publication_rejects_ignore_timeout(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    timeout_fixture = _fixture(tmp_path)
    timeout_result = _VERIFY.verify_artifact_manifest(timeout_fixture["manifest"])
    timeout_output = (
        timeout_fixture["manyforge"]
        / ".manyforge-var/qualification/timeout/verification-summary.json"
    )

    def time_out(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["git", "check-ignore"], timeout=5)

    monkeypatch.setattr(_VERIFY.subprocess, "run", time_out)
    with pytest.raises(_VERIFY.ValidationError, match="ignore provenance"):
        _VERIFY.publish_verification_summary(
            timeout_output,
            timeout_result,
            timeout_fixture["manifest"],
        )
    assert not timeout_output.exists()


def test_release_publication_requires_committed_current_ignore_rule(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/ignore-provenance/verification-summary.json"
    )
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    _write(fixture["manyforge"] / ".gitignore", "build/\n")
    with pytest.raises(
        _VERIFY.ValidationError,
        match="source-controlled version",
    ):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()


def test_release_publication_rejects_a_later_committed_ignore_negation(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    repo = fixture["manyforge"]
    _write(
        repo / ".gitignore",
        (
            ".manyforge-var/\n"
            "!.manyforge-var/\n"
            "!.manyforge-var/qualification/\n"
            "!.manyforge-var/qualification/**\n"
            "build/\n"
        ),
    )
    subprocess.run(["git", "-C", str(repo), "add", ".gitignore"], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "commit", "-qm", "negate ignore"],
        check=True,
    )
    head = _SOURCE.source_identity(repo)["head_commit"]
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "ignore-negation repository",
    )
    try:
        assert _VERIFY._publication_path_is_ignored(  # noqa: SLF001
            binding,
            Path(".manyforge-var/qualification/result.json"),
            head,
        ) is False
    finally:
        binding.close()


def test_release_publication_detects_postlink_payload_mutation(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/postlink/verification-summary.json"
    )
    real_link = _VERIFY.os.link

    def link_then_mutate(source, destination, *args, **kwargs) -> None:
        real_link(source, destination, *args, **kwargs)
        descriptor = _VERIFY.os.open(
            source,
            _VERIFY.os.O_WRONLY | _VERIFY.os.O_CLOEXEC | _VERIFY.os.O_NOFOLLOW,
            dir_fd=kwargs["src_dir_fd"],
        )
        try:
            _VERIFY.os.pwrite(descriptor, b"X", 0)
            _VERIFY.os.fsync(descriptor)
        finally:
            _VERIFY.os.close(descriptor)

    monkeypatch.setattr(_VERIFY.os, "link", link_then_mutate)
    with pytest.raises(_VERIFY.ValidationError, match="content changed"):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()


def test_release_publication_detects_postlink_candidate_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/postlink-source/verification-summary.json"
    )
    real_link = _VERIFY.os.link

    def link_then_drift(source, destination, *args, **kwargs) -> None:
        real_link(source, destination, *args, **kwargs)
        _write(fixture["manyforge"] / "docker-compose.yaml", "source drift\n")

    monkeypatch.setattr(_VERIFY.os, "link", link_then_drift)
    with pytest.raises(
        _VERIFY.ValidationError,
        match="ManyForge source changed during",
    ):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()


def test_release_publication_retains_artifact_descriptors_through_link(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/retained-race/verification-summary.json"
    )
    original = _VERIFY._assert_retained_handles_current  # noqa: SLF001
    payload_passes = 0

    def mutate_after_prelink_check(handles, *, payload: bool) -> None:
        nonlocal payload_passes
        original(handles, payload=payload)
        if payload:
            payload_passes += 1
            if payload_passes == 2:
                _write(handles[0].path, handles[0].path.read_bytes() + b"drift\n")

    monkeypatch.setattr(
        _VERIFY,
        "_assert_retained_handles_current",
        mutate_after_prelink_check,
    )
    with pytest.raises(_VERIFY.ValidationError, match="retained artifact changed"):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()


def test_release_publication_rejects_byte_identical_retained_path_replacement(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _fixture(tmp_path)
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/retained-replacement/verification-summary.json"
    )
    retained_path = Path(
        next(
            item["path"]
            for item in result["retained_artifact_manifest"]
            if item["role"] == "release_container_log"
        )
    )
    displaced = retained_path.with_name(f"{retained_path.name}.displaced")
    original_assertion = _VERIFY._assert_publication_candidate_current  # noqa: SLF001
    replaced = False

    def replace_after_link(*, phase: str, **kwargs: Any) -> None:
        nonlocal replaced
        original_assertion(phase=phase, **kwargs)
        if phase == "during" and not replaced:
            replaced = True
            payload = retained_path.read_bytes()
            retained_path.rename(displaced)
            _write(retained_path, payload)

    monkeypatch.setattr(
        _VERIFY,
        "_assert_publication_candidate_current",
        replace_after_link,
    )
    with pytest.raises(_VERIFY.ValidationError, match="retained artifact changed"):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert not output.exists()
    assert retained_path.read_bytes() == displaced.read_bytes()


def test_release_publication_cleanup_surfaces_failure_and_syncs_parent(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    owned = tmp_path / "owned.tmp"
    _write(owned, "owned\n")
    identity = (owned.stat().st_dev, owned.stat().st_ino)
    directory_descriptor = os.open(
        tmp_path,
        os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
    )
    real_unlink = _VERIFY.os.unlink
    real_fsync = _VERIFY.os.fsync
    try:
        def refuse_unlink(*_args, **_kwargs) -> None:
            raise PermissionError("fixture cleanup refusal")

        with monkeypatch.context() as patch:
            patch.setattr(_VERIFY.os, "unlink", refuse_unlink)
            with pytest.raises(
                _VERIFY.ValidationError,
                match="cannot durably remove owned publication rollback file",
            ):
                _VERIFY._unlink_matching_file(  # noqa: SLF001
                    directory_descriptor,
                    owned.name,
                    identity,
                )
        assert owned.exists()

        synced: list[int] = []

        def record_sync(descriptor: int) -> None:
            synced.append(descriptor)
            real_fsync(descriptor)

        with monkeypatch.context() as patch:
            patch.setattr(_VERIFY.os, "unlink", real_unlink)
            patch.setattr(_VERIFY.os, "fsync", record_sync)
            assert _VERIFY._unlink_matching_file(  # noqa: SLF001
                directory_descriptor,
                owned.name,
                identity,
            ) is True
        assert synced == [directory_descriptor]
        assert not owned.exists()
    finally:
        os.close(directory_descriptor)


def test_release_publication_rejects_parent_swap(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    swap_fixture = _fixture(tmp_path)
    swap_result = _VERIFY.verify_artifact_manifest(swap_fixture["manifest"])
    swap_output = (
        swap_fixture["manyforge"]
        / ".manyforge-var/qualification/swap/verification-summary.json"
    )
    qualification = swap_fixture["manyforge"] / ".manyforge-var/qualification"
    displaced = qualification.with_name("qualification-displaced")
    real_source_identity = _VERIFY.source_identity
    swapped = False

    def collect_then_swap(path: Path):
        nonlocal swapped
        value = real_source_identity(path)
        if not swapped and path == swap_fixture["manyforge"]:
            swapped = True
            qualification.rename(displaced)
            swap_output.parent.mkdir(parents=True)
        return value

    monkeypatch.setattr(_VERIFY, "source_identity", collect_then_swap)
    with pytest.raises(_VERIFY.ValidationError, match="path changed"):
        _VERIFY.publish_verification_summary(
            swap_output,
            swap_result,
            swap_fixture["manifest"],
        )
    assert not swap_output.exists()
    assert not (displaced / "swap/verification-summary.json").exists()
    assert not list(displaced.glob("swap/.verification-summary.json.tmp-*"))


def test_runner_assembles_independent_lane_manifests(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/release-aggregate-fixture"
    )
    artifact_path, summary_path = _RUNNER.assemble(
        repo_root=fixture["manyforge"],
        specs_root=fixture["specs"],
        output_dir=output,
        lane_manifests=[fixture["lanes"][lane] for lane in _VERIFY.LANES],
    )
    assert artifact_path.is_file()
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    assert summary["release_gates_passed"] is True
    assert [lane["name"] for lane in summary["lanes"]] == list(_VERIFY.LANES)


def test_release_runner_rejects_unignored_qualification_output(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    _write(fixture["manyforge"] / ".gitignore", "build/\n")
    output = fixture["manyforge"] / ".manyforge-var/qualification/release"
    with pytest.raises(_RUNNER.RunnerError, match="Git ignore"):
        _RUNNER._validate_output_path(fixture["manyforge"], output)  # noqa: SLF001


def test_release_runner_fails_closed_when_ignore_check_times_out(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = _fixture(tmp_path)
    output = fixture["manyforge"] / ".manyforge-var/qualification/release"

    def time_out(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["git", "check-ignore"], timeout=5)

    monkeypatch.setattr(_RUNNER.subprocess, "run", time_out)
    with pytest.raises(_RUNNER.RunnerError, match="cannot check"):
        _RUNNER._validate_output_path(fixture["manyforge"], output)  # noqa: SLF001


def test_release_runner_rejects_unsafe_lane_manifest_inputs(tmp_path: Path) -> None:
    duplicate_fields = tmp_path / "duplicate.json"
    _write(
        duplicate_fields,
        '{"schema_version":"manyforge.step18.release_gate_lane.v1",'
        '"lane":"core_tsan","lane":"core_tsan"}\n',
    )
    with pytest.raises(_RUNNER.RunnerError, match="repeats a JSON field"):
        _RUNNER._read_lane_manifest(duplicate_fields)  # noqa: SLF001

    oversized = tmp_path / "oversized.json"
    _write(oversized, b" " * (_RUNNER.MAX_LANE_MANIFEST_BYTES + 1))
    with pytest.raises(_RUNNER.RunnerError, match="exceeds"):
        _RUNNER._read_lane_manifest(oversized)  # noqa: SLF001

    target = tmp_path / "target.json"
    _json_write(
        target,
        {
            "schema_version": _RUNNER.LANE_SCHEMA,
            "lane": "core_tsan",
        },
    )
    link = tmp_path / "lane-link.json"
    link.symlink_to(target)
    with pytest.raises(_RUNNER.RunnerError, match="symbolic link"):
        _RUNNER._read_lane_manifest(link)  # noqa: SLF001

    invalid_unicode = tmp_path / "invalid-unicode.json"
    _write(invalid_unicode, '{"value":"\\ud800"}\n')
    with pytest.raises(_RUNNER.RunnerError, match="valid Unicode"):
        _RUNNER._read_lane_manifest(invalid_unicode)  # noqa: SLF001


def test_release_runner_preflights_referenced_artifact_bounds_and_links(
    tmp_path: Path,
) -> None:
    manifest = tmp_path / "lane-manifest.json"
    inspect = tmp_path / "container-inspect.json"
    log = tmp_path / "container.log"
    _write(inspect, "{}\n")
    _write(log, "bounded log\n")
    value = {
        "artifacts": {
            "container_inspect": {
                "path": str(inspect),
                "sha256": _sha256(inspect),
                "bytes": inspect.stat().st_size,
            },
            "container_log": {
                "path": str(log),
                "sha256": _sha256(log),
                "bytes": _RUNNER.MAX_CONTAINER_LOG_BYTES + 1,
            },
        },
        "execution": {"security_override": None},
    }
    with pytest.raises(_RUNNER.RunnerError, match="retained-artifact bound"):
        _RUNNER._preflight_lane_artifacts(  # noqa: SLF001
            manifest,
            value,
            "core_tsan",
        )
    link = tmp_path / "container-inspect-link.json"
    link.symlink_to(inspect)
    value["artifacts"]["container_inspect"]["path"] = str(link)
    value["artifacts"]["container_inspect"]["bytes"] = inspect.stat().st_size
    value["artifacts"]["container_log"]["bytes"] = log.stat().st_size
    with pytest.raises(_RUNNER.RunnerError, match="symbolic link"):
        _RUNNER._preflight_lane_artifacts(  # noqa: SLF001
            manifest,
            value,
            "core_tsan",
        )

    linked_parent = tmp_path / "linked-parent"
    linked_parent.symlink_to(tmp_path, target_is_directory=True)
    value["artifacts"]["container_inspect"]["path"] = str(
        linked_parent / inspect.name
    )
    with pytest.raises(_RUNNER.RunnerError, match="traverse symbolic links"):
        _RUNNER._preflight_lane_artifacts(  # noqa: SLF001
            manifest,
            value,
            "core_tsan",
        )


def test_release_runner_bounds_serialized_aggregate_outputs() -> None:
    with pytest.raises(_RUNNER.RunnerError, match="output bound"):
        _RUNNER._render_bounded_json(  # noqa: SLF001
            "verification-summary.json",
            {"value": "x" * _RUNNER.MAX_OUTPUT_ARTIFACT_BYTES},
        )


def test_release_runner_rejects_lane_manifest_symlink_swap_at_open(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = tmp_path / "lane.json"
    replacement = tmp_path / "replacement.json"
    _json_write(manifest, {"schema_version": _RUNNER.LANE_SCHEMA})
    _json_write(replacement, {"schema_version": _RUNNER.LANE_SCHEMA})
    real_open = _RUNNER.os.open
    swapped = False

    def swap_then_open(path, flags, *args, **kwargs):
        nonlocal swapped
        if not swapped and Path(path) == manifest:
            swapped = True
            manifest.unlink()
            manifest.symlink_to(replacement)
        return real_open(path, flags, *args, **kwargs)

    monkeypatch.setattr(_RUNNER.os, "open", swap_then_open)
    with pytest.raises(_RUNNER.RunnerError, match="symbolic link"):
        _RUNNER._read_lane_manifest(manifest)  # noqa: SLF001


def test_release_runner_rejects_manifest_count_before_checkout_or_git_work(
    tmp_path: Path,
) -> None:
    with pytest.raises(_RUNNER.RunnerError, match="exactly seven"):
        _RUNNER.assemble(
            repo_root=tmp_path / "missing-manyforge",
            specs_root=tmp_path / "missing-specs",
            output_dir=tmp_path / "untrusted-output",
            lane_manifests=[],
        )


def test_release_runner_rejects_cross_lane_hardlink_aliases(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    first_lane, second_lane = _VERIFY.LANES[:2]
    first_manifest = json.loads(
        fixture["lanes"][first_lane].read_text(encoding="utf-8")
    )
    second_manifest_path = fixture["lanes"][second_lane]
    second_manifest = json.loads(second_manifest_path.read_text(encoding="utf-8"))
    first_log = Path(first_manifest["artifacts"]["container_log"]["path"])
    second_log = Path(second_manifest["artifacts"]["container_log"]["path"])
    second_log.unlink()
    second_log.hardlink_to(first_log)
    second_manifest["artifacts"]["container_log"] = {
        "path": str(second_log),
        "sha256": _sha256(second_log),
        "bytes": second_log.stat().st_size,
    }
    _json_write(second_manifest_path, second_manifest)

    output = fixture["manyforge"] / ".manyforge-var/qualification/alias"
    with pytest.raises(_RUNNER.RunnerError, match="alias"):
        _RUNNER.assemble(
            repo_root=fixture["manyforge"],
            specs_root=fixture["specs"],
            output_dir=output,
            lane_manifests=[
                fixture["lanes"][lane] for lane in _VERIFY.LANES
            ],
        )
    assert not output.exists()


def test_release_runner_rejects_lane_cardinality_before_artifact_traversal(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    last_lane = _VERIFY.LANES[-1]
    manifest_path = fixture["lanes"][last_lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["lane"] = _VERIFY.LANES[0]
    manifest["artifacts"]["container_log"]["path"] = str(
        tmp_path / "must-not-be-opened"
    )
    _json_write(manifest_path, manifest)

    output = fixture["manyforge"] / ".manyforge-var/qualification/duplicate"
    with pytest.raises(_RUNNER.RunnerError, match="missing or duplicated"):
        _RUNNER.assemble(
            repo_root=fixture["manyforge"],
            specs_root=fixture["specs"],
            output_dir=output,
            lane_manifests=[
                fixture["lanes"][lane] for lane in _VERIFY.LANES
            ],
        )
    assert not output.exists()


def test_release_runner_removes_owned_partial_output_after_verifier_failure(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    broken_lane = fixture["lanes"][_VERIFY.LANES[0]]
    manifest = json.loads(broken_lane.read_text(encoding="utf-8"))
    manifest["execution"]["terminal_status"]["exit_code"] = 1
    _json_write(broken_lane, manifest)
    output = (
        fixture["manyforge"]
        / ".manyforge-var/qualification/release-failed-fixture"
    )

    with pytest.raises(_RUNNER.RunnerError, match="verification failed"):
        _RUNNER.assemble(
            repo_root=fixture["manyforge"],
            specs_root=fixture["specs"],
            output_dir=output,
            lane_manifests=[
                fixture["lanes"][lane] for lane in _VERIFY.LANES
            ],
        )

    assert not output.exists()


def test_release_runner_rejects_replaced_qualification_directory_chain(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = _fixture(tmp_path)
    output = fixture["manyforge"] / ".manyforge-var/qualification/path-swap"
    qualification = output.parent
    displaced = qualification.with_name("qualification-displaced")
    real_verify = _VERIFY.verify_artifact_manifest

    def verify_then_replace(path: Path) -> dict[str, Any]:
        result = real_verify(path)
        qualification.rename(displaced)
        output.mkdir(parents=True)
        return result

    monkeypatch.setattr(_VERIFY, "verify_artifact_manifest", verify_then_replace)
    with pytest.raises(_RUNNER.RunnerError, match="paths changed"):
        _RUNNER.assemble(
            repo_root=fixture["manyforge"],
            specs_root=fixture["specs"],
            output_dir=output,
            lane_manifests=[
                fixture["lanes"][lane] for lane in _VERIFY.LANES
            ],
        )
    assert output.is_dir()
    assert not (output / "verification-summary.json").exists()
    assert not (displaced / output.name).exists()


def test_release_runner_rejects_a_noncanonical_published_summary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = _fixture(tmp_path)
    output = fixture["manyforge"] / ".manyforge-var/qualification/wrong-summary"

    def publish_wrong_summary(
        destination: Path,
        _result: dict[str, Any],
        _manifest: Path,
    ) -> None:
        _write(destination, '{"release_gates_passed":true}\n')

    monkeypatch.setattr(
        _VERIFY,
        "publish_verification_summary",
        publish_wrong_summary,
    )
    with pytest.raises(_RUNNER.RunnerError, match="summary content differs"):
        _RUNNER.assemble(
            repo_root=fixture["manyforge"],
            specs_root=fixture["specs"],
            output_dir=output,
            lane_manifests=[
                fixture["lanes"][lane] for lane in _VERIFY.LANES
            ],
        )
    assert not output.exists()


def test_release_runner_cleanup_preserves_an_unowned_summary_name(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    output = fixture["manyforge"] / ".manyforge-var/qualification/cleanup-race"
    repo_identity = _RUNNER._directory_identity(  # noqa: SLF001
        fixture["manyforge"],
        "ManyForge",
    )
    handles = _RUNNER._create_output_directory(  # noqa: SLF001
        fixture["manyforge"],
        output,
        repo_identity,
    )
    summary = output / "verification-summary.json"
    try:
        _write(summary, "racing publisher\n")
        _RUNNER._cleanup_failed_output(  # noqa: SLF001
            handles,
            output.name,
            None,
            None,
        )
        assert summary.read_text(encoding="utf-8") == "racing publisher\n"
    finally:
        summary.unlink(missing_ok=True)
        if output.exists():
            output.rmdir()
        _RUNNER._close_output_handles(handles)  # noqa: SLF001


@pytest.mark.parametrize("mutation", ["missing", "duplicate"])
def test_aggregate_rejects_missing_or_duplicate_lanes(
    tmp_path: Path, mutation: str
) -> None:
    fixture = _fixture(tmp_path)
    campaign = json.loads(fixture["manifest"].read_text(encoding="utf-8"))
    if mutation == "missing":
        campaign["lanes"].pop()
    else:
        campaign["lanes"][-1] = dict(campaign["lanes"][0])
    _json_write(fixture["manifest"], campaign)
    with pytest.raises(_VERIFY.ValidationError, match="seven|missing or duplicated"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_rehashed_mixed_source_lane(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    other = tmp_path / "other"
    _commit_repo(other, {"candidate.txt": "other source\n"})
    other_identity = _SOURCE.source_identity(other)
    lane = "core_tsan"
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["identities"]["manyforge_before"] = other_identity
    manifest["identities"]["manyforge_after"] = other_identity
    _json_write(manifest_path, manifest)
    _rebind_lane(fixture, lane)
    with pytest.raises(_VERIFY.ValidationError, match="mixed ManyForge source"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_rehashed_cross_lane_log_substitution(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    lane = "core_asan_ubsan"
    target_manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    source_manifest = json.loads(
        fixture["lanes"]["collision_asan_ubsan"].read_text(encoding="utf-8")
    )
    target_log = Path(target_manifest["artifacts"]["container_log"]["path"])
    source_log = Path(source_manifest["artifacts"]["container_log"]["path"])
    target_log.write_bytes(source_log.read_bytes())
    _rebind_artifact(fixture, lane, "container_log")
    with pytest.raises(_VERIFY.ValidationError, match="missing or ambiguous"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize(
    ("marker_kind", "replacement"),
    [
        ("begin", "prefix STEP18_RELEASE_GATE_BEGIN lane=full_dev_regression"),
        ("begin", "STEP18_RELEASE_GATE_BEGIN lane=full_dev_regression suffix"),
        ("end", "prefix STEP18_RELEASE_GATE_END lane=full_dev_regression status=0"),
        ("end", "STEP18_RELEASE_GATE_END lane=full_dev_regression status=0 suffix"),
    ],
)
def test_aggregate_rejects_nonexact_release_boundary_lines(
    tmp_path: Path, marker_kind: str, replacement: str
) -> None:
    fixture = _fixture(tmp_path)
    lane = "full_dev_regression"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    log_path = Path(manifest["artifacts"]["container_log"]["path"])
    marker = (
        f"STEP18_RELEASE_GATE_BEGIN lane={lane}"
        if marker_kind == "begin"
        else f"STEP18_RELEASE_GATE_END lane={lane} status=0"
    )
    log = log_path.read_text(encoding="utf-8")
    assert log.count(marker) == 1
    _write(log_path, log.replace(marker, replacement, 1))
    _rebind_artifact(fixture, lane, "container_log")

    with pytest.raises(_VERIFY.ValidationError, match="missing or ambiguous"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize(
    ("lane", "needle", "replacement", "message"),
    [
        (
            "core_asan_ubsan",
            "fallback_binaries=0",
            "fallback_binaries=1",
            "fallback/skip",
        ),
        (
            "core_tsan",
            "CORE_TSAN_GROUP_SUMMARY",
            "WARNING: ThreadSanitizer\nCORE_TSAN_GROUP_SUMMARY",
            "sanitizer signature",
        ),
        (
            "full_dev_regression",
            "All 11 test suite(s) passed.",
            "  SKIP: native capability unavailable\nAll 11 test suite(s) passed.",
            "skipped a capability lane",
        ),
        (
            "full_dev_regression",
            "All 11 test suite(s) passed.",
            "All 11 test suite(s) passed.\nAll 11 test suite(s) passed.",
            "11-suite passing summary",
        ),
    ],
)
def test_aggregate_rejects_failed_or_ambiguous_test_evidence(
    tmp_path: Path,
    lane: str,
    needle: str,
    replacement: str,
    message: str,
) -> None:
    fixture = _fixture(tmp_path)
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    log_path = Path(manifest["artifacts"]["container_log"]["path"])
    text = log_path.read_text(encoding="utf-8")
    assert needle in text
    _write(log_path, text.replace(needle, replacement, 1))
    _rebind_artifact(fixture, lane, "container_log")
    with pytest.raises(_VERIFY.ValidationError, match=message):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_empty_log_even_when_rehashed(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    lane = "collision_tsan"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    log_path = Path(manifest["artifacts"]["container_log"]["path"])
    _write(log_path, b"")
    _rebind_artifact(fixture, lane, "container_log")
    with pytest.raises(_VERIFY.ValidationError, match="log is empty"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_docker_inspect_rejects_rehashed_missing_sanitizer_seccomp(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    lane = "collision_asan_ubsan"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    inspect_path = Path(manifest["artifacts"]["container_inspect"]["path"])
    inspect = json.loads(inspect_path.read_text(encoding="utf-8"))
    inspect[0]["HostConfig"]["SecurityOpt"] = []
    _json_write(inspect_path, inspect)
    _rebind_artifact(fixture, lane, "container_inspect")
    with pytest.raises(_VERIFY.ValidationError, match="security options differ"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize(
    "lane", ["full_dev_regression", "collision_asan_ubsan"]
)
def test_docker_inspect_rejects_coordinated_noncanonical_security_profile(
    tmp_path: Path, lane: str
) -> None:
    fixture = _fixture(tmp_path)
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    inspect_path = Path(manifest["artifacts"]["container_inspect"]["path"])
    inspect = json.loads(inspect_path.read_text(encoding="utf-8"))
    inspect[0]["HostConfig"]["SecurityOpt"] = []
    manifest["execution"]["security_options"] = []
    _json_write(inspect_path, inspect)
    _json_write(manifest_path, manifest)
    _rebind_artifact(fixture, lane, "container_inspect")

    with pytest.raises(_VERIFY.ValidationError, match="security profile is not canonical"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_ros_lane_rejects_bare_ambient_passthrough_even_when_manifest_matches(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    lane = "full_ros_regression"
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    inspect_path = Path(manifest["artifacts"]["container_inspect"]["path"])
    inspect = json.loads(inspect_path.read_text(encoding="utf-8"))
    key = _VERIFY.ROS_AMBIENT_PASSTHROUGH_VARIABLES[0]
    configured = inspect[0]["Config"]["Env"]
    configured[configured.index(f"{key}=")] = key
    manifest["execution"]["configured_environment"] = configured
    _json_write(inspect_path, inspect)
    _json_write(manifest_path, manifest)
    _rebind_artifact(fixture, lane, "container_inspect")

    with pytest.raises(_VERIFY.ValidationError, match="environment is not exact"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize(
    "replacement",
    [
        "STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=40",
        "prefix STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4",
        "STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4 suffix",
    ],
)
def test_ros_lane_rejects_nonexact_neutralization_marker(
    tmp_path: Path, replacement: str
) -> None:
    fixture = _fixture(tmp_path)
    lane = "full_ros_regression"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    log_path = Path(manifest["artifacts"]["container_log"]["path"])
    marker = "STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4"
    log = log_path.read_text(encoding="utf-8")
    assert log.count(marker) == 1
    _write(log_path, log.replace(marker, replacement, 1))
    _rebind_artifact(fixture, lane, "container_log")

    with pytest.raises(_VERIFY.ValidationError, match="exact ROS ambient"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize("position", ["before", "after"])
def test_ros_lane_rejects_extra_neutralization_marker_outside_release_section(
    tmp_path: Path, position: str
) -> None:
    fixture = _fixture(tmp_path)
    lane = "full_ros_regression"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    log_path = Path(manifest["artifacts"]["container_log"]["path"])
    marker = "STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4\n"
    log = log_path.read_text(encoding="utf-8")
    assert log.count(marker) == 1
    _write(log_path, marker + log if position == "before" else log + marker)
    _rebind_artifact(fixture, lane, "container_log")

    with pytest.raises(_VERIFY.ValidationError, match="exact ROS ambient"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("image", "container id/name/image differs"),
        ("mount", "source or Specs mount is not exact"),
        ("command", "configured process argv differs"),
        ("entrypoint", "configured entrypoint is not canonical"),
        ("environment", "environment .* differs or is duplicated"),
    ],
)
def test_docker_inspect_rejects_rehashed_identity_or_runtime_substitution(
    tmp_path: Path,
    mutation: str,
    message: str,
) -> None:
    fixture = _fixture(tmp_path)
    lane = "full_ros_regression"
    manifest = json.loads(fixture["lanes"][lane].read_text(encoding="utf-8"))
    inspect_path = Path(manifest["artifacts"]["container_inspect"]["path"])
    inspect = json.loads(inspect_path.read_text(encoding="utf-8"))
    if mutation == "image":
        inspect[0]["Image"] = "sha256:" + "c" * 64
    elif mutation == "mount":
        workspace = next(
            item for item in inspect[0]["Mounts"] if item["Destination"] == "/workspace"
        )
        workspace["Source"] = str(tmp_path / "substituted-source")
        manifest["execution"]["mounts"] = _VERIFY.normalize_mounts(
            inspect[0]["Mounts"]
        )
    elif mutation == "command":
        inspect[0]["Args"][-1] += "\ntrue"
    elif mutation == "entrypoint":
        inspect[0]["Config"]["Entrypoint"] = ["/usr/local/bin/docker-entrypoint.sh"]
    else:
        key = "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256"
        inspect[0]["Config"]["Env"] = [
            f"{key}={'0' * 64}" if item.startswith(f"{key}=") else item
            for item in inspect[0]["Config"]["Env"]
        ]
        manifest["execution"]["configured_environment"] = inspect[0]["Config"][
            "Env"
        ]
    _json_write(inspect_path, inspect)
    _json_write(fixture["lanes"][lane], manifest)
    _rebind_artifact(fixture, lane, "container_inspect")
    with pytest.raises(_VERIFY.ValidationError, match=message):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_script_hash_that_changed_during_lane(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    lane = "contract_architecture_hygiene"
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    first = next(iter(manifest["script_hashes"]["before"]))
    manifest["script_hashes"]["before"][first] = "0" * 64
    _json_write(manifest_path, manifest)
    _rebind_lane(fixture, lane)
    with pytest.raises(_VERIFY.ValidationError, match="changed during execution"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_rehashed_script_substitution(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    lane = "core_asan_ubsan"
    manifest_path = fixture["lanes"][lane]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    script = "scripts/ci/run-core-asan.sh"
    manifest["script_hashes"]["before"][script] = "0" * 64
    manifest["script_hashes"]["after"][script] = "0" * 64
    _json_write(manifest_path, manifest)
    _rebind_lane(fixture, lane)
    with pytest.raises(_VERIFY.ValidationError, match="input hash changed"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_rejects_current_candidate_drift(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    _write(fixture["manyforge"] / "post-campaign.txt", "drift\n")
    with pytest.raises(_VERIFY.ValidationError, match="current ManyForge source differs"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_aggregate_allows_only_the_excluded_evidence_record_to_change(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    evidence_record = (
        fixture["specs"] / _SOURCE.SPECS_CONTRACT_EXCLUDED_PATH
    )
    _write(evidence_record, "post-campaign release-summary hash\n")
    result = _VERIFY.verify_artifact_manifest(fixture["manifest"])
    assert result["candidate_specs_source_identity"] == fixture["specs_source"]
    assert _SOURCE.source_identity(fixture["specs"]) != fixture["specs_source"]


def test_aggregate_rejects_nonexcluded_specs_drift(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    _write(fixture["specs"] / "docs/spec/drift.md", "contract drift\n")
    with pytest.raises(_VERIFY.ValidationError, match="current Specs contract differs"):
        _VERIFY.verify_artifact_manifest(fixture["manifest"])


def test_step18_hygiene_rejects_production_all_active_distance(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    producer = fixture["manyforge"] / "manyforge_core/src/new_producer.cpp"
    _write(producer, "auto mode = kAllActivePairs;\n")
    with pytest.raises(_VERIFY.ValidationError, match="production selects"):
        _VERIFY._step18_distance_scope_hygiene(fixture["manyforge"])  # noqa: SLF001


def test_step18_removed_field_hygiene_allows_only_pinned_boundary_uses(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    removed = "min_separation_" + "clearance_m"
    _write(
        repo / "scripts/qualification/capture_isaac_drop_approach.py",
        "\n".join(
            (
                "The historical baseline field is accepted only at this boundary and",
                "never preserves or silently emulates it",
                f'if "{removed}" in normalized:',
                f'clearance = normalized.pop("{removed}")',
                f'"Step 18 capture rejects nonzero {removed}"',
                f'contact.pop("{removed}", None)',
            )
        ),
    )
    _write(
        repo / "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp",
        "\n".join(
            (
                "#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE",
                "baseline mechanism replay requires captured contact_release policy",
                f"entry.{removed} = source.get(\"{removed}\");",
                "#endif",
                "#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE",
                f"contact.{removed} = source.get(\"{removed}\");",
                "#endif",
            )
        ),
    )

    assert _VERIFY._step18_removed_field_hygiene(repo) == [  # noqa: SLF001
        "removed_field_absent_from_production",
        "removed_field_occurrences_restricted_to_rejection_or_baseline_evidence",
    ]

    with (repo / "scripts/qualification/capture_isaac_drop_approach.py").open(
        "a", encoding="utf-8"
    ) as stream:
        stream.write(f"\nproduction.{removed} = 0.0\n")
    with pytest.raises(_VERIFY.ValidationError, match="outside strict"):
        _VERIFY._step18_removed_field_hygiene(repo)  # noqa: SLF001


def test_step18_removed_field_hygiene_rejects_symlink_special_and_invalid_utf8(
    tmp_path: Path,
) -> None:
    removed = ("min_separation_" + "clearance_m").encode("ascii")

    symlink_repo = tmp_path / "symlink-source"
    _write(symlink_repo / "target.py", "safe\n")
    (symlink_repo / "producer.py").symlink_to("target.py")
    with pytest.raises(_VERIFY.ValidationError, match="symbolic link"):
        _VERIFY._step18_removed_field_hygiene(symlink_repo)  # noqa: SLF001

    special_repo = tmp_path / "special-source"
    special_repo.mkdir()
    os.mkfifo(special_repo / "producer.fifo")
    with pytest.raises(_VERIFY.ValidationError, match="special file"):
        _VERIFY._step18_removed_field_hygiene(special_repo)  # noqa: SLF001

    invalid_repo = tmp_path / "invalid-source"
    _write(invalid_repo / "producer.bin", removed + b"\xff")
    with pytest.raises(_VERIFY.ValidationError, match="not valid UTF-8"):
        _VERIFY._step18_removed_field_hygiene(invalid_repo)  # noqa: SLF001


def test_step18_removed_field_hygiene_enforces_tree_bounds(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo = tmp_path / "source"
    _write(repo / "one.py", "one\n")
    _write(repo / "two.py", "two\n")
    monkeypatch.setattr(_VERIFY, "MAX_HYGIENE_FILES", 1)
    with pytest.raises(_VERIFY.ValidationError, match="file-count bound"):
        _VERIFY._step18_removed_field_hygiene(repo)  # noqa: SLF001


def test_step18_hygiene_consumes_only_candidate_identity_paths(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(
        repo,
        {
            ".gitignore": "ignored/\n",
            "tracked.py": "tracked source\n",
        },
    )
    removed = "min_separation_" + "clearance_m"
    _write(repo / "ignored/outside-candidate.py", f"production.{removed} = 1\n")

    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "test hygiene repository",
    )
    try:
        attested = _VERIFY._step18_attested_source_paths(binding)  # noqa: SLF001
        assert "ignored/outside-candidate.py" not in attested
        assert _VERIFY._step18_removed_field_hygiene(  # noqa: SLF001
            binding,
            attested_paths=attested,
        ) == [
            "removed_field_absent_from_production",
            "removed_field_occurrences_restricted_to_rejection_or_baseline_evidence",
        ]
    finally:
        binding.close()


def test_step18_attested_source_paths_decodes_bounded_git_buffer(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(repo, {"tracked.py": "tracked source\n"})
    _write(repo / "untracked.py", "untracked source\n")

    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "bytearray inventory repository",
    )
    try:
        assert _VERIFY._step18_attested_source_paths(binding) == frozenset(  # noqa: SLF001
            {"tracked.py", "untracked.py"}
        )
    finally:
        binding.close()


def test_step18_global_hygiene_scans_attested_build_prefixed_file(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(repo, {"build-hidden.py": "tracked source\n"})
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "build-prefixed source repository",
    )
    try:
        attested = _VERIFY._step18_attested_source_paths(binding)  # noqa: SLF001
        observed = {
            relative
            for relative, _payload in _VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=attested,
            )
        }
        assert "build-hidden.py" in observed
    finally:
        binding.close()


def test_step18_global_hygiene_scans_attested_source_below_build_directory(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(repo, {"build/kept.py": "tracked source\n"})
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "attested build-directory repository",
    )
    try:
        attested = _VERIFY._step18_attested_source_paths(binding)  # noqa: SLF001
        observed = {
            relative
            for relative, _payload in _VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=attested,
            )
        }
        assert "build/kept.py" in observed
    finally:
        binding.close()


@pytest.mark.parametrize(
    "relative",
    (
        "producer/ignored.py",
        "producer/build/ignored.py",
    ),
)
def test_step18_producer_hygiene_rejects_unattested_ignored_source(
    tmp_path: Path,
    relative: str,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(
        repo,
        {
            ".gitignore": f"{relative}\n",
            "tracked.py": "tracked source\n",
            "producer/tracked.py": "tracked producer source\n",
        },
    )
    _write(repo / relative, "producer source\n")

    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "test producer repository",
    )
    try:
        attested = _VERIFY._step18_attested_source_paths(binding)  # noqa: SLF001
        with pytest.raises(_VERIFY.ValidationError, match="absent from the candidate"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                relative_root="producer",
                suffixes=frozenset({".py"}),
                attested_paths=attested,
                reject_unattested=True,
            ))
    finally:
        binding.close()


def test_step18_hygiene_walker_revalidates_root_directory_and_binding(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _write(repo / "producer/one.py", "one\n")
    _write(repo / "producer/two.py", "two\n")
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "test hygiene repository",
    )
    try:
        root_iterator = _VERIFY._iter_hygiene_files(binding)  # noqa: SLF001
        assert next(root_iterator)[0] == "producer/one.py"
        _write(repo / "late.py", "late\n")
        with pytest.raises(_VERIFY.ValidationError, match="directory changed"):
            list(root_iterator)

        relative_iterator = _VERIFY._iter_hygiene_files(  # noqa: SLF001
            binding,
            relative_root="producer",
        )
        assert next(relative_iterator)[0] == "producer/one.py"
        displaced = repo / "producer-displaced"
        (repo / "producer").rename(displaced)
        _write(repo / "producer/one.py", "one\n")
        _write(repo / "producer/two.py", "two\n")
        with pytest.raises(
            _VERIFY.ValidationError,
            match="directory changed|path changed",
        ):
            list(relative_iterator)
    finally:
        binding.close()


def test_step18_hygiene_rejects_deep_attested_inventory_before_expansion(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _write(repo / "source.py", "source\n")
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "deep-inventory repository",
    )
    deep = "/".join(
        [f"depth-{index}" for index in range(_VERIFY.MAX_HYGIENE_DEPTH + 1)]
        + ["source.py"]
    )
    try:
        with pytest.raises(_VERIFY.ValidationError, match="directory-depth bound"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=frozenset({deep}),
            ))
    finally:
        binding.close()


def test_step18_hygiene_rejects_wide_attested_directory_inventory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo = tmp_path / "source"
    _write(repo / "source.py", "source\n")
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "wide-inventory repository",
    )
    monkeypatch.setattr(_VERIFY, "MAX_HYGIENE_DIRECTORIES", 2)
    try:
        with pytest.raises(_VERIFY.ValidationError, match="directory-count bound"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=frozenset(
                    {
                        "first/source.py",
                        "second/source.py",
                        "third/source.py",
                    }
                ),
            ))
    finally:
        binding.close()


def test_step18_hygiene_bounds_attested_path_count_and_input_bytes(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo = tmp_path / "source"
    _write(repo / "source.py", "source\n")
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "bounded-inventory repository",
    )
    original_file_bound = _VERIFY.MAX_HYGIENE_FILES
    try:
        monkeypatch.setattr(_VERIFY, "MAX_HYGIENE_FILES", 1)
        with pytest.raises(_VERIFY.ValidationError, match="file-count bound"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=frozenset({"one.py", "two.py"}),
            ))

        monkeypatch.setattr(_VERIFY, "MAX_HYGIENE_FILES", original_file_bound)
        monkeypatch.setattr(_VERIFY, "MAX_HYGIENE_GIT_INVENTORY_BYTES", 4)
        with pytest.raises(_VERIFY.ValidationError, match="byte bound"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                attested_paths=frozenset({"source.py"}),
            ))
    finally:
        binding.close()


def _allowed_hygiene_symlink_fixture(root: Path, *, target: bool = True) -> Path:
    target_path = root / "scripts/manyforge-mcp-bridge.py"
    if target:
        _write(target_path, "bridge source\n")
    _write(root / "middle.py", "middle source\n")
    link = root / "agent-skills/manyforge-composer/manyforge-mcp-bridge.py"
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to("../../scripts/manyforge-mcp-bridge.py")
    return link


def test_step18_hygiene_allows_unchanged_bound_symlink_and_scans_target(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    link = _allowed_hygiene_symlink_fixture(repo)
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "unchanged allowed-symlink repository",
    )
    try:
        observed = {
            relative
            for relative, _payload in _VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
            )
        }
    finally:
        binding.close()
    assert link.relative_to(repo).as_posix() not in observed
    assert "scripts/manyforge-mcp-bridge.py" in observed
    assert "middle.py" in observed


def test_step18_hygiene_allowed_symlink_requires_existing_scanned_target(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _allowed_hygiene_symlink_fixture(repo, target=False)
    with pytest.raises(
        _VERIFY.ValidationError,
        match="symbolic link target",
    ):
        _VERIFY._step18_removed_field_hygiene(repo)  # noqa: SLF001

    unscanned_repo = tmp_path / "unscanned-source"
    _allowed_hygiene_symlink_fixture(unscanned_repo)
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        unscanned_repo,
        "unscanned symlink repository",
    )
    try:
        with pytest.raises(_VERIFY.ValidationError, match="was not scanned as source"):
            list(_VERIFY._iter_hygiene_files(  # noqa: SLF001
                binding,
                suffixes=frozenset({".cpp"}),
            ))
    finally:
        binding.close()


@pytest.mark.parametrize("replacement", ("link", "target"))
def test_step18_hygiene_allowed_symlink_revalidates_link_and_target(
    tmp_path: Path,
    replacement: str,
) -> None:
    repo = tmp_path / "source"
    link = _allowed_hygiene_symlink_fixture(repo)
    target = repo / "scripts/manyforge-mcp-bridge.py"
    binding = _VERIFY._open_absolute_directory(  # noqa: SLF001
        repo,
        "allowed symlink repository",
    )
    try:
        iterator = _VERIFY._iter_hygiene_files(binding)  # noqa: SLF001
        assert next(iterator)[0] == "middle.py"
        if replacement == "link":
            link.unlink()
            link.symlink_to("../../scripts/manyforge-mcp-bridge.py")
        else:
            displaced = target.with_name("manyforge-mcp-bridge.displaced.py")
            payload = target.read_bytes()
            target.rename(displaced)
            _write(target, payload)
        with pytest.raises(
            _VERIFY.ValidationError,
            match="symbolic link.*changed|directory changed",
        ):
            list(iterator)
    finally:
        binding.close()


def test_step18_hygiene_rejects_persistent_mutation_after_source_was_read(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    repo = tmp_path / "source"
    _commit_repo(repo, {"tracked.py": "initial source\n"})

    def read_then_mutate(_root, *, attested_paths=None):
        assert attested_paths == frozenset({"tracked.py"})
        assert (repo / "tracked.py").read_text(encoding="utf-8") == "initial source\n"
        _write(repo / "tracked.py", "persistent mutation\n")
        return ["removed_field_absent_from_production"]

    monkeypatch.setattr(
        _VERIFY,
        "_step18_removed_field_hygiene",
        read_then_mutate,
    )
    monkeypatch.setattr(
        _VERIFY,
        "_step18_distance_scope_hygiene",
        lambda _root, *, attested_paths=None: [],
    )
    with pytest.raises(
        _VERIFY.ValidationError,
        match="source identity changed during Step 18 hygiene",
    ):
        _VERIFY.run_step18_hygiene(repo)


def test_step18_hygiene_binds_exact_distance_capability_contract(
    tmp_path: Path,
) -> None:
    repo = _copy_distance_capability_sources(tmp_path / "source")
    checks = _VERIFY._step18_distance_capability_hygiene(repo)  # noqa: SLF001
    assert checks == [
        "distance_scope_capability_descriptor_is_explicit",
        "distance_scope_declaration_is_sorted_unique_and_exactly_admitted",
        "manyforge_engine_publishes_all_four_exact_distance_scopes",
        "validation_uses_exact_scope_admission_on_both_query_paths",
        "runtime_uses_exact_scope_admission",
        "distance_capabilities_are_mode_scoped_without_broader_fallback",
    ]


@pytest.mark.parametrize(
    ("relative", "old", "new", "message"),
    (
        (
            "manyforge_core/include/manyforge_core/types/collision.hpp",
            "supported_distance_request_modes;",
            "legacy_distance_request_modes;",
            "descriptor",
        ),
        (
            "manyforge_core/include/manyforge_core/types/collision.hpp",
            "return std::binary_search(",
            "return std::find(",
            "exact declared-mode lookup",
        ),
        (
            "manyforge_collision/src/manyforge_collision_engine.cpp",
            "      core::CollisionDistanceRequestMode::kSelectedCollidingPairs,\n",
            "",
            "exact sorted",
        ),
        (
            "manyforge_core/src/collision_validation_coordinator.cpp",
            "collision_engine_supports_distance_request_mode(\n          capability, distance_request_mode)",
            "capability.supports_distance_query",
            "validation",
        ),
        (
            "manyforge_core/src/runtime_collision_query_session.cpp",
            "collision_engine_supports_distance_request_mode(*capability_,",
            "legacy_broad_distance_admission(*capability_,",
            "runtime",
        ),
    ),
)
def test_step18_hygiene_rejects_distance_capability_regression(
    tmp_path: Path, relative: str, old: str, new: str, message: str
) -> None:
    repo = _copy_distance_capability_sources(tmp_path / "source")
    path = repo / relative
    text = path.read_text(encoding="utf-8")
    assert old in text
    _write(path, text.replace(old, new, 1))
    with pytest.raises(_VERIFY.ValidationError, match=message):
        _VERIFY._step18_distance_capability_hygiene(repo)  # noqa: SLF001

# Historical producer source blob: 398a61263e5418242b313e713d632370c8342e1a.
# Historical test blob: b39353a830e4158e5369953a42427ceb81fa63fa.
# The verifier/assembler cases above remain active. Only tests that asserted
# the deleted Docker lane producer and its image-resolution implementation
# move to the future acceptance gates below.
REMOVED_RELEASE_PRODUCER_CASES = (
    {
        "historical_test_blob": "b39353a830e4158e5369953a42427ceb81fa63fa",
        "historical_lines": "419-510",
        "case_names": (
            "test_docker_lane_contracts_are_exact_and_independently_rerunnable",
            "test_release_lane_environment_bypasses_entrypoint_without_losing_source_packages",
            "test_ros_environment_neutralization_uses_unshadowable_shell_builtins",
        ),
        "future_gates": ("I5/H0", "I6/N", "I8/P5", "I9/I", "I10/R"),
        "acceptance": (
            "Define new lane-native container commands, package visibility, ROS "
            "environment neutralization, and security from the restored owners; "
            "do not revive the frozen Step-18 command strings."
        ),
    },
    {
        "historical_test_blob": "b39353a830e4158e5369953a42427ceb81fa63fa",
        "historical_lines": "671-731",
        "case_names": (
            "test_release_runner_resolves_build_only_compose_service_image",
            "test_release_runner_rejects_ambiguous_build_only_image_configuration",
        ),
        "future_gates": ("I5/H0", "I6/N", "I8/P5", "I9/I", "I10/R"),
        "acceptance": (
            "Bind each restored release lane to one immutable current image and "
            "reject absent or ambiguous image resolution before execution."
        ),
    },
)


def test_release_lane_selector_gates_before_argument_parsing(
    capsys,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    def forbidden(*_args, **_kwargs):
        raise AssertionError("unavailable lane touched an offline helper")

    for name in (
        "_source_identity_module",
        "_verifier_module",
        "_validate_roots",
        "_validate_output_path",
        "_create_output_directory",
    ):
        monkeypatch.setattr(_RUNNER, name, forbidden)
    output = tmp_path / "must-not-exist"
    assert _RUNNER.main(
        ["lane", "--output-dir", str(output), "--not-a-real-lane-option"]
    ) == 78
    assert not output.exists()
    payload = json.loads(capsys.readouterr().err)
    assert payload["historical_source_blob"] == (
        "398a61263e5418242b313e713d632370c8342e1a"
    )
    assert payload["historical_test_blob"] == (
        "b39353a830e4158e5369953a42427ceb81fa63fa"
    )
    assert payload["exit_code"] == 78
    assert {
        assignment["historical_source_lines"]
        for assignment in payload["assignments"].values()
    } == {_RUNNER.HISTORICAL_PRODUCER_SOURCE_LINES}


def test_release_lane_selector_does_not_require_offline_helper_modules(
    tmp_path: Path,
) -> None:
    standalone = tmp_path / "run_step18_release_gates.py"
    _write(
        standalone,
        (_QUALIFICATION / "run_step18_release_gates.py").read_text(
            encoding="utf-8"
        ),
    )
    result = subprocess.run(
        [sys.executable, str(standalone), "lane", "--malformed"],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert result.returncode == 78
    payload = json.loads(result.stderr)
    assert payload["execution_available"] is False


def test_removed_release_producer_cases_have_future_acceptance_owners() -> None:
    cases = [
        case
        for cohort in REMOVED_RELEASE_PRODUCER_CASES
        for case in cohort["case_names"]
    ]
    assert len(cases) == 5
    assert len(set(cases)) == len(cases)
    assert {
        cohort["historical_test_blob"] for cohort in REMOVED_RELEASE_PRODUCER_CASES
    } == {"b39353a830e4158e5369953a42427ceb81fa63fa"}
    assert all(cohort["future_gates"] for cohort in REMOVED_RELEASE_PRODUCER_CASES)
    assert all(cohort["acceptance"] for cohort in REMOVED_RELEASE_PRODUCER_CASES)
