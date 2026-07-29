from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

import pytest
import yaml


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


def test_docker_lane_contracts_are_exact_and_independently_rerunnable() -> None:
    compose = yaml.safe_load((_REPO_ROOT / "docker-compose.yaml").read_text())
    assert compose["services"]["dev"]["security_opt"] == ["seccomp=unconfined"]
    for lane in _VERIFY.LANES:
        description = _RUNNER.describe_lane(lane)
        assert description["unique_build_root_template"].endswith(f"/<uuid>/{lane}")
        assert description["requested_container_argv"][:2] == ["/bin/bash", "-lc"]
        if lane in _VERIFY.SANITIZER_LANES:
            assert description["compose_service"] == "ros-jazzy-full"
            assert description["compose_profile"] == "ros-full"
            assert description["security_options"] == ["seccomp=unconfined"]
            assert "run-ctest-gtest-binaries-grouped.py" in " ".join(
                description["input_paths"]
            )
        elif description["compose_service"] == "dev":
            assert description["security_options"] == ["seccomp=unconfined"]
        else:
            assert description["security_options"] == []
    assert "./scripts/test-all.sh all" in _VERIFY.lane_inner_script(
        "full_dev_regression"
    )
    ros = _VERIFY.lane_inner_script("full_ros_regression")
    assert "-DCMAKE_BUILD_TYPE=Release" in ros
    assert "-DCMAKE_CXX_FLAGS=-Werror" in ros
    assert "ctest --test-dir" in ros
    assert "pytest /workspace/manyforge_ros/tests" in ros


def test_release_lane_environment_bypasses_entrypoint_without_losing_source_packages() -> None:
    environment = _VERIFY.lane_environment(
        "full_dev_regression",
        str(uuid.UUID(int=100, version=4)),
        "a" * 64,
        "b" * 64,
        "c" * 64,
    )
    assert environment["MANYFORGE_BUILD_DIR"].startswith(
        "/workspace/build/step18-release-gates/"
    )
    assert environment["PYTHONPATH"] == (
        "/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace"
    )
    assert environment["PYTHONPYCACHEPREFIX"].startswith(
        "/tmp/manyforge-step18-release-gate-pycache/"
    )
    ros_environment = _VERIFY.lane_environment(
        "full_ros_regression",
        str(uuid.UUID(int=101, version=4)),
        "a" * 64,
        "b" * 64,
        "c" * 64,
    )
    assert {
        key: ros_environment[key]
        for key in _VERIFY.ROS_AMBIENT_PASSTHROUGH_VARIABLES
    } == {key: "" for key in _VERIFY.ROS_AMBIENT_PASSTHROUGH_VARIABLES}
    ros_script = _VERIFY.lane_inner_script("full_ros_regression")
    assert (
        "builtin unset -v -- "
        + " ".join(_VERIFY.ROS_AMBIENT_PASSTHROUGH_VARIABLES)
        in ros_script
    )
    assert "env | grep" not in ros_script
    assert ros_script.count("STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4") == 1
    assert "STEP18_ROS_AMBIENT_ENV_NEUTRALIZED" not in _VERIFY.lane_inner_script(
        "full_dev_regression"
    )


def test_ros_environment_neutralization_uses_unshadowable_shell_builtins() -> None:
    keys = " ".join(_VERIFY.ROS_AMBIENT_PASSTHROUGH_VARIABLES)
    poisoned_shell = f'''set -euo pipefail
unset() {{ return 0; }}
env() {{ return 2; }}
export -f unset env
for key in {keys}; do
  builtin export "${{key}}=poisoned"
done
{_VERIFY._ros_ambient_environment_preamble()}
for key in {keys}; do
  if [[ -v "${{key}}" ]]; then
    exit 9
  fi
done
'''
    result = subprocess.run(
        ["bash", "-c", poisoned_shell],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr


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

    def publish_racer_then_link(source: Path, destination: Path) -> None:
        _write(Path(destination), racing_payload)
        real_link(source, destination)

    monkeypatch.setattr(_VERIFY.os, "link", publish_racer_then_link)
    with pytest.raises(FileExistsError):
        _VERIFY.publish_verification_summary(output, result, fixture["manifest"])
    assert output.read_text(encoding="utf-8") == racing_payload
    assert not list(output.parent.glob(f".{output.name}.tmp-*"))


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


def test_release_runner_resolves_build_only_compose_service_image(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    image_id = "a" * 64
    calls: list[list[str]] = []
    monkeypatch.setattr(
        _RUNNER,
        "_compose_config",
        lambda _repo_root, _lane: {"services": {"dev": {"build": {}}}},
    )

    def run(argv: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        calls.append(argv)
        if "config" in argv:
            return subprocess.CompletedProcess(
                argv,
                0,
                stdout="manyforge-dev\n",
                stderr="",
            )
        return subprocess.CompletedProcess(
            argv, 0, stdout=f"sha256:{image_id}\n", stderr=""
        )

    monkeypatch.setattr(_RUNNER, "_run", run)

    assert _RUNNER._image_identity(  # noqa: SLF001
        tmp_path, "contract_architecture_hygiene"
    ) == ("manyforge-dev", image_id)
    assert calls[0][-3:] == ["config", "--images", "dev"]
    assert calls[1] == [
        "docker",
        "image",
        "inspect",
        "manyforge-dev",
        "--format",
        "{{.Id}}",
    ]


@pytest.mark.parametrize("configured_images", ["", "first\nsecond\n"])
def test_release_runner_rejects_ambiguous_build_only_image_configuration(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    configured_images: str,
) -> None:
    monkeypatch.setattr(
        _RUNNER,
        "_compose_config",
        lambda _repo_root, _lane: {"services": {"dev": {"build": {}}}},
    )

    def run(argv: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(argv, 0, stdout=configured_images, stderr="")

    monkeypatch.setattr(_RUNNER, "_run", run)

    with pytest.raises(_RUNNER.RunnerError, match="exactly one configured image"):
        _RUNNER._image_identity(  # noqa: SLF001
            tmp_path, "contract_architecture_hygiene"
        )


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
