from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_VERIFY_PATH = (
    _REPO_ROOT / "scripts/qualification/verify_step18_loaded_kernel.py"
)
_SOURCE_IDENTITY_PATH = _REPO_ROOT / "scripts/qualification/source_identity.py"


def _load_module(name: str, path: Path):  # type: ignore[no-untyped-def]
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


_SOURCE_IDENTITY = sys.modules.get("source_identity") or _load_module(
    "source_identity", _SOURCE_IDENTITY_PATH
)
_VERIFY = _load_module("verify_step18_loaded_kernel", _VERIFY_PATH)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def _git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _commit_repo(repo: Path, files: dict[str, str]) -> None:
    repo.mkdir(parents=True)
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "qualification@example.invalid")
    _git(repo, "config", "user.name", "Qualification Test")
    for name, value in files.items():
        _write(repo / name, value)
    _git(repo, "add", "--all")
    _git(repo, "commit", "-qm", "fixture")


def _json_write(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def _fixture(tmp_path: Path) -> tuple[Path, dict[str, Path]]:
    manyforge = tmp_path / "manyforge"
    specs = tmp_path / "manyforge_specs"
    _commit_repo(
        manyforge,
        {
            ".gitignore": "build/\n.manyforge-var/\n",
            "scripts/run-step18-loaded-kernel-qualification.sh": "runner-v1\n",
            "scripts/qualification/source_identity.py": "identity-v1\n",
            "scripts/qualification/verify_step18_loaded_kernel.py": "verifier-v1\n",
            "manyforge_core/tools/stability_gate.sh": "gate-v1\n",
            "docker-compose.yaml": "services: {}\n",
        },
    )
    _commit_repo(specs, {"docs/spec/250-contact-policy.md": "contract-v1\n"})

    binary = manyforge / "build/step18-loaded-kernel/test_execution_kernel"
    _write(binary, "native-test-binary\n")
    evidence = manyforge / ".manyforge-var/qualification/loaded-kernel"
    raw_dir = evidence / "raw"
    raw_dir.mkdir(parents=True)

    ledger_lines = [
        "iteration\tstatus\tduration_ns\tlog_name\tlog_sha256\tlog_bytes\tbefore_workers_json\tafter_workers_json"
    ]
    iterations = []
    worker_identities = [
        {
            "ordinal": ordinal,
            "pid": 41000 + ordinal,
            "proc_starttime_ticks": 900000 + ordinal,
            "state": "R",
            "state_observations": ["R", "R"],
            "command_argv": _VERIFY.EXPECTED_LOAD_WORKER_COMMAND,
        }
        for ordinal in range(1, 4)
    ]
    for index in range(1, 31):
        log_name = f"test_execution_kernel.{index}.log"
        log = raw_dir / log_name
        _write(
            log,
            "[==========] 291 tests from 44 test suites ran. (123 ms total)\n"
            "[  PASSED  ] 291 tests.\n",
        )
        digest = _sha256(log)
        size = log.stat().st_size
        ledger_lines.append(
            f"{index}\t0\t{1000 + index}\t{log_name}\t{digest}\t{size}\t"
            + json.dumps(worker_identities, sort_keys=True, separators=(",", ":"))
            + "\t"
            + json.dumps(worker_identities, sort_keys=True, separators=(",", ":"))
        )
        iterations.append(
            {
                "iteration": index,
                "exit_status": 0,
                "duration_ns": 1000 + index,
                "log_name": log_name,
                "log_sha256": digest,
                "log_bytes": size,
                "load_worker_evidence_before": worker_identities,
                "load_worker_evidence_after": worker_identities,
            }
        )
    ledger = raw_dir / "stability-gate-ledger.tsv"
    _write(ledger, "\n".join(ledger_lines) + "\n")
    inventory = raw_dir / "test_execution_kernel.inventory.txt"
    required_inventory = sorted(_VERIFY.REQUIRED_LOADED_PREDICATE_TESTS)
    required_test_names = [name.split(".", 1)[1] for name in required_inventory]
    inventory_names = required_test_names + [
        f"LoadedFixtureDummy{index:03d}"
        for index in range(291 - len(required_test_names))
    ]
    _write(
        inventory,
        "ExecutionKernelTest.\n"
        + "".join(f"  {name}\n" for name in inventory_names),
    )

    process_id = "123e4567-e89b-42d3-a456-426614174000"
    run_id = f"20260720T000000000000000Z-step18-loaded-kernel-{process_id}"
    gate = manyforge / "manyforge_core/tools/stability_gate.sh"
    raw_manifest = raw_dir / "stability-gate-manifest.json"
    raw = {
        "schema_version": _VERIFY.RAW_SCHEMA,
        "generated_at_utc": "2026-07-20T00:00:00Z",
        "run_id": run_id,
        "process_instance_id": process_id,
        "configuration": {
            "iterations": 30,
            "load_workers": 3,
            "timeout_seconds": 30,
            "cpus_allowed_list": "10-13",
        },
        "command_argv": [
            "/workspace/build/step18-loaded-kernel/test_execution_kernel",
            "--gtest_filter=*",
            "--gtest_repeat=1",
            "--gtest_brief=1",
        ],
        "test_binary": {
            "path": "/workspace/build/step18-loaded-kernel/test_execution_kernel",
            "sha256": _sha256(binary),
        },
        "gate_script": {
            "path": "/workspace/manyforge_core/tools/stability_gate.sh",
            "sha256": _sha256(gate),
        },
        "test_inventory": {
            "name": inventory.name,
            "sha256": _sha256(inventory),
            "bytes": inventory.stat().st_size,
        },
        "environment": {
            "ld_preload": "",
            "segfault_signals": "",
            "gtest_overrides_present": [],
        },
        "ledger": {
            "name": ledger.name,
            "sha256": _sha256(ledger),
            "bytes": ledger.stat().st_size,
        },
        "iterations": iterations,
        "passed": True,
    }
    _json_write(raw_manifest, raw)

    container_id = "a" * 64
    image_id = "b" * 64
    container_name = f"manyforge-step18-loaded-{process_id}"
    compose_project_name = "fixtureproject"
    image_reference = f"{compose_project_name}-dev"
    manyforge_identity = _SOURCE_IDENTITY.source_identity(manyforge)
    specs_identity = _SOURCE_IDENTITY.source_identity(specs)
    specs_contract = _SOURCE_IDENTITY.specs_contract_identity(specs)
    container_command = [
        "taskset",
        "-c",
        "10-13",
        "bash",
        "-lc",
        _VERIFY.EXPECTED_INNER_SCRIPT,
    ]
    binary_container_path = (
        "/workspace/build/step18-loaded-kernel/test_execution_kernel"
    )
    raw_container_path = (
        "/workspace/.manyforge-var/qualification/loaded-kernel/raw/"
        "stability-gate-manifest.json"
    )
    environment = {
        "LD_PRELOAD": "",
        "MANYFORGE_BUILD_JOBS": "2",
        "MANYFORGE_CORE_BUILD_DIR": str(Path(binary_container_path).parent),
        "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256": manyforge_identity[
            "fingerprint_sha256"
        ],
        "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256": specs_contract[
            "contract_fingerprint_sha256"
        ],
        "MANYFORGE_STABILITY_ITERATIONS": "30",
        "MANYFORGE_STABILITY_LOAD_WORKERS": "3",
        "MANYFORGE_STABILITY_LOG_DIR": str(Path(raw_container_path).parent),
        "MANYFORGE_STABILITY_MANIFEST": raw_container_path,
        "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID": process_id,
        "MANYFORGE_STABILITY_RUN_ID": run_id,
        "MANYFORGE_STABILITY_TEST_BINARY": binary_container_path,
        "MANYFORGE_STABILITY_TIMEOUT_SECONDS": "30",
        "SEGFAULT_SIGNALS": "",
    }
    labels = {
        "io.manyforge.qualification.kind": "step18-loaded-kernel",
        "io.manyforge.qualification.run_id": run_id,
        "io.manyforge.qualification.process_instance_id": process_id,
        "io.manyforge.qualification.source_fingerprint_sha256": manyforge_identity[
            "fingerprint_sha256"
        ],
        "io.manyforge.qualification.specs_contract_fingerprint_sha256": specs_contract[
            "contract_fingerprint_sha256"
        ],
        "io.manyforge.qualification.compose_project": compose_project_name,
    }
    expected_mounts = [
        {
            "type": "volume",
            "source": f"{compose_project_name}_build-cache",
            "destination": "/tmp/manyforge-build",
            "rw": True,
        },
        {
            "type": "bind",
            "source": str(manyforge),
            "destination": "/workspace",
            "rw": True,
        },
    ]
    inspect_path = evidence / "container-inspect.json"
    inspect = [
        {
            "Id": container_id,
            "Image": f"sha256:{image_id}",
            "Name": f"/{container_name}",
            "Path": container_command[0],
            "Args": container_command[1:],
            "State": {"ExitCode": 0, "OOMKilled": False, "Error": ""},
            "Config": {
                "Image": image_reference,
                "Env": [f"{key}={value}" for key, value in environment.items()],
                "Labels": {
                    **labels,
                    "com.docker.compose.project": compose_project_name,
                    "com.docker.compose.service": "dev",
                    "com.docker.compose.oneoff": "True",
                },
            },
            "Mounts": [
                {
                    "Type": "volume",
                    "Name": f"{compose_project_name}_build-cache",
                    "Source": (
                        "/var/lib/docker/volumes/"
                        f"{compose_project_name}_build-cache/_data"
                    ),
                    "Destination": "/tmp/manyforge-build",
                    "RW": True,
                },
                {
                    "Type": "bind",
                    "Source": str(manyforge),
                    "Destination": "/workspace",
                    "RW": True,
                }
            ],
        }
    ]
    _json_write(inspect_path, inspect)
    container_log = evidence / "container.log"
    _write(container_log, "stability gate passed\n")

    outer_command = [
        "docker",
        "compose",
        "--project-name",
        compose_project_name,
        "-f",
        str(manyforge / "docker-compose.yaml"),
        "run",
        "--detach",
        "--no-deps",
        "-T",
        "--name",
        container_name,
    ]
    for key, value in sorted(environment.items()):
        outer_command.extend(("-e", f"{key}={value}"))
    for key, value in sorted(labels.items()):
        outer_command.extend(("--label", f"{key}={value}"))
    outer_command.extend(("dev", *container_command))
    manifest_path = evidence / "run-manifest.json"
    manifest = {
        "schema_version": _VERIFY.RUN_SCHEMA,
        "run_id": run_id,
        "process_instance_id": process_id,
        "paths": {
            "manyforge_root": str(manyforge),
            "specs_root": str(specs),
            "raw_manifest": str(raw_manifest),
            "container_inspect": str(inspect_path),
            "container_log": str(container_log),
        },
        "identities": {
            "manyforge_before": manyforge_identity,
            "manyforge_after": manyforge_identity,
            "specs_before": specs_identity,
            "specs_after": specs_identity,
            "specs_contract_before": specs_contract,
            "specs_contract_after": specs_contract,
        },
        "execution": {
            "requested_cpu_affinity": "10-13",
            "host_kernel_id": "Linux qualification",
            "cpu_governor_set": "performance",
            "cpu_frequency_driver_set": "fixture",
            "compose_service": "dev",
            "container_id": container_id,
            "container_name": container_name,
            "image_id": image_id,
            "image_reference": image_reference,
            "compose_project_name": compose_project_name,
            "container_labels": labels,
            "expected_mounts": expected_mounts,
            "outer_command_argv": outer_command,
            "container_command_argv": container_command,
            "container_environment": environment,
        },
        "hashes": {
            "runner_script_sha256": _sha256(
                manyforge / "scripts/run-step18-loaded-kernel-qualification.sh"
            ),
            "source_identity_helper_sha256": _sha256(
                manyforge / "scripts/qualification/source_identity.py"
            ),
            "verifier_script_sha256": _sha256(
                manyforge
                / "scripts/qualification/verify_step18_loaded_kernel.py"
            ),
            "gate_script_sha256": _sha256(gate),
            "compose_file_sha256": _sha256(manyforge / "docker-compose.yaml"),
            "raw_manifest_sha256": _sha256(raw_manifest),
            "container_inspect_sha256": _sha256(inspect_path),
            "container_log_sha256": _sha256(container_log),
        },
    }
    _json_write(manifest_path, manifest)
    return manifest_path, {
        "manyforge": manyforge,
        "specs": specs,
        "raw": raw_manifest,
        "inspect": inspect_path,
        "binary": binary,
        "ledger": ledger,
        "inventory": inventory,
        "verifier": (
            manyforge / "scripts/qualification/verify_step18_loaded_kernel.py"
        ),
    }


def _rehash_run_input(manifest_path: Path, field: str, path: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["hashes"][field] = _sha256(path)
    _json_write(manifest_path, manifest)


def test_loaded_kernel_verifier_accepts_bound_30_by_3_gate(tmp_path: Path) -> None:
    manifest, paths = _fixture(tmp_path)
    result = _VERIFY.verify(manifest)
    assert result["loaded_kernel_gate_passed"] is True
    assert result["configuration"]["iterations"] == 30
    assert result["configuration"]["tests_per_iteration"] == 291
    assert len(result["iteration_logs"]) == 30
    assert len(result["retained_artifact_manifest"]) == 39
    assert result["test_inventory"]["required_predicates"] == sorted(
        _VERIFY.REQUIRED_LOADED_PREDICATE_TESTS
    )
    assert sum(
        item["role"] == "loaded_iteration_log"
        for item in result["retained_artifact_manifest"]
    ) == 30
    assert all(
        row["load_workers_alive_before"] == 3
        and row["load_workers_alive_after"] == 3
        and [worker["pid"] for worker in row["load_worker_evidence_before"]]
        == [41001, 41002, 41003]
        for row in result["iteration_logs"]
    )
    assert result["verifier_script"]["sha256"] == _sha256(paths["verifier"])
    assert Path(result["verifier_script"]["path"]) == paths["verifier"]
    assert result["step18_closed"] is False


def test_loaded_kernel_verifier_rejects_missing_cancel_predicate_inventory(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inventory = paths["inventory"]
    inventory.write_text(
        inventory.read_text(encoding="utf-8").replace(
            "  Cancel_StopDuringValidationRequestsValidationCancel\n", ""
        ),
        encoding="utf-8",
    )
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["test_inventory"]["sha256"] = _sha256(inventory)
    raw["test_inventory"]["bytes"] = inventory.stat().st_size
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])

    with pytest.raises(_VERIFY.ValidationError, match="omits required cancellation"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_missing_worker(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["iterations"][12]["load_worker_evidence_after"] = raw["iterations"][12][
        "load_worker_evidence_after"
    ][:2]
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="exactly three workers"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_container_image_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inspect = json.loads(paths["inspect"].read_text(encoding="utf-8"))
    inspect[0]["Image"] = "sha256:" + "c" * 64
    _json_write(paths["inspect"], inspect)
    _rehash_run_input(manifest, "container_inspect_sha256", paths["inspect"])
    with pytest.raises(_VERIFY.ValidationError, match="container identity"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_native_binary_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    _write(paths["binary"], "different native binary\n")
    with pytest.raises(_VERIFY.ValidationError, match="test binary hash changed"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_evidence_log_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    log = paths["raw"].parent / "test_execution_kernel.7.log"
    _write(log, "substituted log\n")
    with pytest.raises(_VERIFY.ValidationError, match="iteration 7 GTest log hash"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_retained_manifest_detects_postverification_log_mutation(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    result = _VERIFY.verify(manifest)
    log = paths["raw"].parent / "test_execution_kernel.7.log"
    _write(log, "changed after verification\n")
    with pytest.raises(_VERIFY.ValidationError, match="retained artifact changed"):
        _VERIFY._assert_retained_artifact_manifest_current(  # noqa: SLF001
            result["retained_artifact_manifest"]
        )


def test_loaded_kernel_verifier_rejects_fully_rehashed_partial_gtest_log(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    log = paths["raw"].parent / "test_execution_kernel.7.log"
    _write(
        log,
        "[==========] 1 test from 1 test suite ran. (1 ms total)\n"
        "[  PASSED  ] 1 test.\n",
    )
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    record = raw["iterations"][6]
    record["log_sha256"] = _sha256(log)
    record["log_bytes"] = log.stat().st_size
    ledger_lines = paths["ledger"].read_text(encoding="utf-8").splitlines()
    fields = ledger_lines[7].split("\t")
    fields[4] = record["log_sha256"]
    fields[5] = str(record["log_bytes"])
    ledger_lines[7] = "\t".join(fields)
    _write(paths["ledger"], "\n".join(ledger_lines) + "\n")
    raw["ledger"]["sha256"] = _sha256(paths["ledger"])
    raw["ledger"]["bytes"] = paths["ledger"].stat().st_size
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="count changed"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_fully_rehashed_partial_suite(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    ledger_lines = paths["ledger"].read_text(encoding="utf-8").splitlines()
    for index, record in enumerate(raw["iterations"], start=1):
        log = paths["raw"].parent / record["log_name"]
        _write(
            log,
            "[==========] 1 test from 1 test suite ran. (1 ms total)\n"
            "[  PASSED  ] 1 test.\n",
        )
        record["log_sha256"] = _sha256(log)
        record["log_bytes"] = log.stat().st_size
        fields = ledger_lines[index].split("\t")
        fields[4] = record["log_sha256"]
        fields[5] = str(record["log_bytes"])
        ledger_lines[index] = "\t".join(fields)
    _write(paths["ledger"], "\n".join(ledger_lines) + "\n")
    raw["ledger"]["sha256"] = _sha256(paths["ledger"])
    raw["ledger"]["bytes"] = paths["ledger"].stat().st_size
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="complete execution-kernel"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_gtest_environment_override(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["environment"]["gtest_overrides_present"] = ["GTEST_FILTER"]
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="GTest environment"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_preload_injection(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["environment"]["ld_preload"] = "/tmp/fake-gtest.so"
    raw["environment"]["segfault_signals"] = "abrt segv"
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="preload environment"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_altered_outer_docker_argv(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["execution"]["outer_command_argv"].insert(4, "--project-name")
    value["execution"]["outer_command_argv"].insert(5, "substituted")
    _json_write(manifest, value)
    with pytest.raises(_VERIFY.ValidationError, match="outer Docker command"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_command_without_fresh_build_guard(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["execution"]["container_command_argv"][-1] = "gate"
    _json_write(manifest, value)
    with pytest.raises(_VERIFY.ValidationError, match="fresh-build gate"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_specs_contract_scope_change(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["identities"]["specs_contract_after"]["excluded_paths"] = [
        "docs/spec/250-contact-policy.md"
    ]
    _json_write(manifest, value)
    with pytest.raises(_VERIFY.ValidationError, match="Specs contract exclusion"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_run_id_not_bound_to_process_uuid(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["run_id"] = (
        "20260720T000000000000000Z-step18-loaded-kernel-"
        "223e4567-e89b-42d3-a456-426614174000"
    )
    _json_write(manifest, value)
    with pytest.raises(_VERIFY.ValidationError, match="exact process instance UUID"):
        _VERIFY.verify(manifest)


@pytest.mark.parametrize("state", ["Z", "T", "t"])
def test_loaded_kernel_verifier_rejects_rehashed_unusable_worker_state(
    tmp_path: Path, state: str,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["iterations"][4]["load_worker_evidence_after"][1]["state"] = state
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="PID/starttime/state/command"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_worker_pid_reuse(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    for phase in ("load_worker_evidence_before", "load_worker_evidence_after"):
        raw["iterations"][8][phase][0]["proc_starttime_ticks"] += 100
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="reused or replaced"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_worker_command_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    raw = json.loads(paths["raw"].read_text(encoding="utf-8"))
    raw["iterations"][2]["load_worker_evidence_before"][2]["command_argv"] = [
        "sleep",
        "infinity",
    ]
    _json_write(paths["raw"], raw)
    _rehash_run_input(manifest, "raw_manifest_sha256", paths["raw"])
    with pytest.raises(_VERIFY.ValidationError, match="PID/starttime/state/command"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_custom_label_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inspect = json.loads(paths["inspect"].read_text(encoding="utf-8"))
    inspect[0]["Config"]["Labels"]["io.manyforge.qualification.run_id"] = "other"
    _json_write(paths["inspect"], inspect)
    _rehash_run_input(manifest, "container_inspect_sha256", paths["inspect"])
    with pytest.raises(_VERIFY.ValidationError, match="qualification label"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_rehashed_image_reference_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inspect = json.loads(paths["inspect"].read_text(encoding="utf-8"))
    inspect[0]["Config"]["Image"] = "other-dev"
    _json_write(paths["inspect"], inspect)
    _rehash_run_input(manifest, "container_inspect_sha256", paths["inspect"])
    with pytest.raises(_VERIFY.ValidationError, match="configured environment"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_nested_workspace_mount(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inspect = json.loads(paths["inspect"].read_text(encoding="utf-8"))
    inspect[0]["Mounts"].append(
        {
            "Type": "bind",
            "Source": str(paths["manyforge"] / "manyforge_core"),
            "Destination": "/workspace/manyforge_core",
            "RW": True,
        }
    )
    _json_write(paths["inspect"], inspect)
    _rehash_run_input(manifest, "container_inspect_sha256", paths["inspect"])
    with pytest.raises(_VERIFY.ValidationError, match="nested mount shadowing"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_mount_rw_substitution(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    inspect = json.loads(paths["inspect"].read_text(encoding="utf-8"))
    inspect[0]["Mounts"][1]["RW"] = False
    _json_write(paths["inspect"], inspect)
    _rehash_run_input(manifest, "container_inspect_sha256", paths["inspect"])
    with pytest.raises(_VERIFY.ValidationError, match="mount inventory/RW"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_rejects_verifier_hash_substitution(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["hashes"]["verifier_script_sha256"] = "c" * 64
    _json_write(manifest, value)
    with pytest.raises(_VERIFY.ValidationError, match="loaded-kernel verifier hash"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_final_rehash_detects_buffered_log_mutation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, _ = _fixture(tmp_path)
    original = _VERIFY._gtest_counts

    def mutate_after_buffered_parse(log: Any, iteration: int) -> tuple[int, int]:
        result = original(log, iteration)
        if iteration == 1:
            log.path.write_bytes(log.data + b"mutated after buffered parse\n")
        return result

    monkeypatch.setattr(_VERIFY, "_gtest_counts", mutate_after_buffered_parse)
    with pytest.raises(_VERIFY.ValidationError, match="changed during verification"):
        _VERIFY.verify(manifest)


def test_loaded_kernel_verifier_cli_rejects_output_outside_ignored_root(
    tmp_path: Path,
) -> None:
    manifest, _ = _fixture(tmp_path)
    output = tmp_path / "visible-summary.json"
    assert (
        _VERIFY.main(["--manifest", str(manifest), "--output", str(output)]) == 2
    )
    assert not output.exists()


def test_loaded_kernel_verifier_rejects_unignored_qualification_output(
    tmp_path: Path,
) -> None:
    _, paths = _fixture(tmp_path)
    _write(paths["manyforge"] / ".gitignore", "build/\n")
    output = paths["manyforge"] / ".manyforge-var/qualification/visible.json"
    with pytest.raises(_VERIFY.ValidationError, match="Git ignore rules"):
        _VERIFY._validate_cli_output_path(output, paths["manyforge"])


def test_loaded_kernel_verifier_cli_writes_inside_ignored_root(
    tmp_path: Path,
) -> None:
    manifest, paths = _fixture(tmp_path)
    output = (
        paths["manyforge"]
        / ".manyforge-var/qualification/loaded-kernel/verification-summary.json"
    )
    assert (
        _VERIFY.main(["--manifest", str(manifest), "--output", str(output)]) == 0
    )
    assert json.loads(output.read_text(encoding="utf-8"))[
        "loaded_kernel_gate_passed"
    ] is True


def test_loaded_kernel_cli_removes_temporary_output_on_postwrite_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path)
    result = _VERIFY.verify(manifest)
    output = (
        paths["manyforge"]
        / ".manyforge-var/qualification/loaded-kernel/publication-drift.json"
    )
    log = paths["raw"].parent / "test_execution_kernel.7.log"
    original_log = log.read_bytes()
    original_recheck = _VERIFY._assert_retained_artifact_manifest_current

    def drift_before_publication(value: Any) -> None:
        log.write_bytes(original_log + b"changed\n")
        original_recheck(value)

    monkeypatch.setattr(_VERIFY, "verify", lambda *args, **kwargs: result)
    monkeypatch.setattr(
        _VERIFY,
        "_assert_retained_artifact_manifest_current",
        drift_before_publication,
    )
    try:
        assert _VERIFY.main(
            ["--manifest", str(manifest), "--output", str(output)]
        ) == 2
        assert not output.exists()
        assert not list(output.parent.glob(f".{output.name}.tmp-*"))
    finally:
        log.write_bytes(original_log)


def test_stability_gate_emits_per_iteration_worker_liveness(
    tmp_path: Path,
) -> None:
    fake_binary = tmp_path / "fake-test-execution-kernel"
    _write(fake_binary, "#!/usr/bin/env bash\necho '[  PASSED  ] fake kernel suite'\n")
    fake_binary.chmod(0o755)
    log_dir = tmp_path / "logs"
    environment = os.environ.copy()
    environment.update(
        {
            "GTEST_FILTER": "DoesNotExist.*",
            "GTEST_TOTAL_SHARDS": "100",
            "GTEST_SHARD_INDEX": "99",
            "MANYFORGE_STABILITY_TEST_BINARY": str(fake_binary),
            "MANYFORGE_CORE_BUILD_DIR": str(tmp_path / "unused-build"),
            "MANYFORGE_STABILITY_ITERATIONS": "2",
            "MANYFORGE_STABILITY_LOAD_WORKERS": "2",
            "MANYFORGE_STABILITY_TIMEOUT_SECONDS": "5",
            "MANYFORGE_STABILITY_LOG_DIR": str(log_dir),
            "MANYFORGE_STABILITY_RUN_ID": "unit-test",
            "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID": (
                "123e4567-e89b-42d3-a456-426614174000"
            ),
        }
    )
    subprocess.run(
        [_REPO_ROOT / "manyforge_core/tools/stability_gate.sh"],
        check=True,
        cwd=_REPO_ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    raw = json.loads(
        (log_dir / "stability-gate-manifest.json").read_text(encoding="utf-8")
    )
    assert raw["passed"] is True
    assert raw["environment"]["gtest_overrides_present"] == []
    assert raw["command_argv"] == [
        str(fake_binary.resolve()),
        "--gtest_filter=*",
        "--gtest_repeat=1",
        "--gtest_brief=1",
    ]
    assert len(raw["iterations"]) == 2
    assert all(
        [worker["ordinal"] for worker in row["load_worker_evidence_before"]]
        == [1, 2]
        and [worker["ordinal"] for worker in row["load_worker_evidence_after"]]
        == [1, 2]
        and all(
            worker["state"] in _VERIFY.ALLOWED_LOAD_WORKER_STATES
            and worker["proc_starttime_ticks"] > 0
            and worker["command_argv"] == _VERIFY.EXPECTED_LOAD_WORKER_COMMAND
            for worker in row["load_worker_evidence_before"]
            + row["load_worker_evidence_after"]
        )
        for row in raw["iterations"]
    )
