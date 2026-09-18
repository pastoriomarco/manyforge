#!/usr/bin/env python3
"""D0 development workbench orchestration inside one dev container."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from typing import Any, Callable, Sequence

# `docker exec` does not inherit shell exports made by the container
# entrypoint. Keep this tool and every service it starts self-contained.
_SOURCE_ROOTS = (
    "/workspace",
    "/workspace/manyforge_behavior",
    "/workspace/manyforge_ros",
)
for _source_root in _SOURCE_ROOTS:
    if _source_root not in sys.path:
        sys.path.insert(0, _source_root)
os.environ["PYTHONPATH"] = os.pathsep.join(
    (*_SOURCE_ROOTS, os.environ.get("PYTHONPATH", ""))
).rstrip(os.pathsep)

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


WORKSPACE = Path("/workspace")
CACHE_ROOT = Path("/runner")
BUILD_ROOT = CACHE_ROOT / "build"
STATE_ROOT = Path(os.environ.get("MANYFORGE_DEV_STATE_ROOT", "/tmp/manyforge-state"))
HEADLESS_BUILD = BUILD_ROOT / "headless"
ROBOPLAN_BUILD = BUILD_ROOT / "roboplan"
DEPLOYMENT_AUTHORING_BUILD = BUILD_ROOT / "deployment-authoring"
SCENARIO_ROOT = Path(
    os.environ.get(
        "MANYFORGE_DEV_SCENARIO_ROOT",
        str(CACHE_ROOT / "maintained-scenario-package"),
    )
)
PACKAGE_ROOT = SCENARIO_ROOT / "retained-package"
NATIVE_EXECUTION_HOST = ROBOPLAN_BUILD / "manyforge_native_execution_host"
NATIVE_CANDIDATE_WORKER = ROBOPLAN_BUILD / "manyforge_roboplan_candidate_worker"
DEPLOYMENT_COMPILER = (
    DEPLOYMENT_AUTHORING_BUILD / "manyforge_maintained_deployment_occurrences"
)
MANAGER_BINARY = HEADLESS_BUILD / "manyforge_manager_daemon"
NODE_CATALOG = WORKSPACE / "manyforge_behavior/resources/node_catalog.yaml"
NODE_ENVIRONMENT_MANIFEST = (
    WORKSPACE / "manyforge_behavior/resources/node_environment_manifest.yaml"
)
ALARM_CATALOG = (
    WORKSPACE / "manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
)
FRONTEND_ROOT = WORKSPACE / "manyforge_composer/frontend"


class WorkbenchError(RuntimeError):
    """One precise orchestration failure."""


def _json(value: Any) -> str:
    return json.dumps(value, allow_nan=False, indent=2, sort_keys=True)


def _run(command: Sequence[str], *, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise WorkbenchError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}"
        )


def _qualified_root() -> Path:
    return Path(os.environ.get("MANYFORGE_DEV_QUALIFIED_ROOT", "/manyforge-qualified"))


def _import_package() -> None:
    """Import immutable Scenario content without caching backend executables."""

    if PACKAGE_ROOT.is_dir():
        return
    qualified = _qualified_root()
    source_package = qualified / "maintained-package"
    if not source_package.is_dir():
        raise WorkbenchError(
            "the maintained package is unavailable; set "
            "MANYFORGE_DEV_QUALIFIED_ROOT to an existing qualified package root"
        )
    if SCENARIO_ROOT.exists():
        raise WorkbenchError(
            "maintained package cache is incomplete; inspect or remove "
            f"{SCENARIO_ROOT}"
        )
    temporary = CACHE_ROOT / f"maintained-scenario.import-{os.getpid()}"
    temporary.mkdir(parents=True)
    try:
        shutil.copytree(source_package, temporary / "retained-package")
        temporary.replace(SCENARIO_ROOT)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _build_jobs() -> str:
    jobs = os.environ.get("MANYFORGE_DEV_BUILD_JOBS", "").strip()
    if not jobs:
        # The native closure has several memory-heavy C++ translation units.
        # Eight keeps ordinary development hosts responsive without serializing
        # the build; callers may explicitly tune the shared limit.
        jobs = str(min(8, max(1, os.cpu_count() or 1)))
    if not jobs.isdecimal() or int(jobs) < 1:
        raise WorkbenchError("MANYFORGE_DEV_BUILD_JOBS must be a positive integer")
    return jobs


def _ensure_current_backend() -> Path:
    """Build only the stale current Manager/Headless production closure."""

    cache = HEADLESS_BUILD / "CMakeCache.txt"
    if not cache.is_file():
        completed = subprocess.run(
            (
                sys.executable,
                "-c",
                "import pybind11; print(pybind11.get_cmake_dir())",
            ),
            check=False,
            capture_output=True,
            text=True,
        )
        pybind11_dir = completed.stdout.strip()
        if completed.returncode != 0 or not pybind11_dir:
            raise WorkbenchError(
                "cannot resolve the development pybind11 CMake package"
            )
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_headless"),
                "-B",
                str(HEADLESS_BUILD),
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "-DBUILD_TESTING=OFF",
                "-DMANYFORGE_HEADLESS_BUILD_PYTHON=ON",
                f"-Dpybind11_DIR={pybind11_dir}",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(HEADLESS_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_manager_daemon",
            "manyforge_headless_py",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(MANAGER_BINARY, "current Manager", executable=True)
    extensions = sorted(HEADLESS_BUILD.glob("manyforge_headless_py*.so"))
    if len(extensions) != 1:
        raise WorkbenchError("current Headless Python extension is absent or ambiguous")
    _regular_file(extensions[0], "current Headless Python extension")
    return extensions[0]


def _ensure_current_native_backend() -> tuple[Path, Path]:
    """Incrementally build the current workspace native execution owners."""

    if not (ROBOPLAN_BUILD / "CMakeCache.txt").is_file():
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_roboplan"),
                "-B",
                str(ROBOPLAN_BUILD),
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "-DBUILD_TESTING=OFF",
                "-DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=OFF",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(ROBOPLAN_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_native_execution_host",
            "manyforge_roboplan_candidate_worker",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(
        NATIVE_EXECUTION_HOST, "current native execution host", executable=True
    )
    _regular_file(NATIVE_CANDIDATE_WORKER, "current candidate worker", executable=True)
    return NATIVE_EXECUTION_HOST, NATIVE_CANDIDATE_WORKER


def _ensure_deployment_compiler() -> tuple[Path, tuple[tuple[str, Path], ...]]:
    """Build the maintained compiler and its exact normalized model inputs."""

    if not (DEPLOYMENT_AUTHORING_BUILD / "CMakeCache.txt").is_file():
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_deployment"),
                "-B",
                str(DEPLOYMENT_AUTHORING_BUILD),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=ON",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(DEPLOYMENT_AUTHORING_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_maintained_deployment_occurrences",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(DEPLOYMENT_COMPILER, "Deployment compiler", executable=True)
    prepared = tuple(
        (
            family_id,
            DEPLOYMENT_AUTHORING_BUILD / "generated" / directory,
        )
        for family_id, directory in (
            ("lite6_lite_gripper", "lite6_lite_gripper_structural"),
            ("uf850_xarm_gripper", "uf850_xarm_gripper_structural"),
            ("ur10e_robotiq_2f_140", "ur10e_robotiq_2f_140_structural"),
            ("ur5_roboplan", "ur5_roboplan_structural"),
            ("ur5e_robotiq_2f_85", "ur5e_robotiq_2f_85_structural"),
        )
    )
    for family_id, root in prepared:
        if not root.is_dir() or root.is_symlink():
            raise WorkbenchError(
                f"prepared Deployment model is unavailable for {family_id}: {root}"
            )
    return DEPLOYMENT_COMPILER, prepared


def _regular_file(path: Path, label: str, *, executable: bool = False) -> None:
    if path.is_symlink() or not path.is_file():
        raise WorkbenchError(f"{label} is not a regular file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise WorkbenchError(f"{label} is not executable: {path}")


def _remove_stale_socket(path: Path, label: str) -> None:
    """Remove one named ephemeral socket without touching persistent state."""

    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(mode):
        raise WorkbenchError(f"{label} is not a socket: {path}")
    path.unlink()


def _require_current_workspace_deployment(
    scenario: dict[str, Any],
    deployment_path: Path | None = None,
) -> Path:
    """Reject an imported package that predates its current source contract."""

    deployment_path = deployment_path or (
        WORKSPACE / "examples" / f"{scenario['deployment_artifact_id']}.deployment.yaml"
    )
    _regular_file(deployment_path, "matching workspace Deployment")
    try:
        deployment = json.loads(deployment_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise WorkbenchError(
            "matching workspace Deployment is invalid: " f"{deployment_path}: {error}"
        ) from error
    if (
        deployment.get("deployment_artifact_id") != scenario["deployment_artifact_id"]
        or deployment.get("deployment_revision") != scenario["deployment_revision"]
    ):
        raise WorkbenchError(
            "imported maintained Scenario package is stale relative to its "
            "workspace Deployment; refresh the qualified package cache"
        )
    return deployment_path


def _require_current_workspace_node_environment(
    binding: dict[str, Any],
) -> Path:
    """Reject a package compiled against stale executable node source."""

    _regular_file(NODE_ENVIRONMENT_MANIFEST, "workspace node environment")
    try:
        manifest = json.loads(NODE_ENVIRONMENT_MANIFEST.read_text(encoding="utf-8"))
        catalog = manifest["catalog"]
        expected = {
            "catalog_digest": catalog["catalog_digest"],
            "catalog_id": catalog["catalog_id"],
            "content_version": catalog["content_version"],
            "environment_digest": manifest["environment_digest"],
        }
    except (KeyError, OSError, UnicodeError, TypeError, json.JSONDecodeError) as error:
        raise WorkbenchError(
            "workspace node environment manifest is invalid: "
            f"{NODE_ENVIRONMENT_MANIFEST}: {error}"
        ) from error
    if binding.get("node_environment") != expected:
        raise WorkbenchError(
            "imported maintained Scenario package is stale relative to its "
            "workspace node environment; refresh the qualified package cache"
        )
    return NODE_ENVIRONMENT_MANIFEST


def _compiler_build_profile(bundle: dict[str, Any]) -> str:
    digest = bundle["provenance"]["compiler_build_digest"]
    if (
        not isinstance(digest, str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None
    ):
        raise ValueError("Scenario compiler build digest is invalid")
    path = PACKAGE_ROOT / "build-manifests/compiler_build" / f"{digest[7:]}.json"
    _regular_file(path, "Scenario compiler build manifest")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("compiler_build_digest") != digest:
        raise ValueError(
            "Scenario compiler manifest does not match its exact reference"
        )
    profile = manifest["build_profile"]
    if not isinstance(profile, str):
        raise ValueError("compiler build profile must be text")
    return profile


def _load_scenario(scenario_id: str) -> dict[str, Any]:
    catalog_path = PACKAGE_ROOT / "maintained.scenario-catalog.yaml"
    _regular_file(catalog_path, "maintained Scenario catalog")
    try:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        rows = [
            row
            for row in catalog["scenarios"]
            if row["scenario_id"] == scenario_id
            or scenario_id in row.get("aliases", [])
        ]
        if len(rows) != 1:
            raise ValueError("Scenario is absent or ambiguous")
        row = rows[0]
        if row["deployment_compatibility"]["lane"] != "manyforge-only":
            raise ValueError("Scenario is not a native ManyForge lane")
        binding_path = (
            PACKAGE_ROOT
            / "program-bindings/sha256"
            / f"{row['program_binding_id'].removeprefix('sha256:')}.json"
        )
        bundle_path = (
            PACKAGE_ROOT
            / "retained-bundles/sha256"
            / f"{row['retained_bundle_id'].removeprefix('sha256:')}.json"
        )
        _regular_file(binding_path, "Scenario Program Binding")
        _regular_file(bundle_path, "Scenario retained bundle")
        binding = json.loads(binding_path.read_text(encoding="utf-8"))
        bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
        build_profile = _compiler_build_profile(bundle)
        program_path = (
            PACKAGE_ROOT
            / "programs/sha256"
            / f"{binding['program_revision'].removeprefix('sha256:')}.json"
        )
        _regular_file(program_path, "Scenario Program")
        program = json.loads(program_path.read_text(encoding="utf-8"))
        graph = bundle["cell_graph"]
        runtime_units = graph["runtime_units"]
        controllable_components = graph["controllable_components"]
    except (KeyError, OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise WorkbenchError(
            f"maintained Scenario {scenario_id!r} is invalid: {error}"
        ) from error
    if (
        binding.get("program_binding_id") != row["program_binding_id"]
        or binding.get("retained_bundle_id") != row["retained_bundle_id"]
        or binding.get("graph_revision") != graph.get("graph_revision")
        or bundle.get("deployment_artifact_id") != row["deployment_artifact_id"]
        or bundle.get("deployment_revision") != row["deployment_revision"]
        or program.get("program_id") != binding.get("program_id")
        or program.get("program_revision") != binding.get("program_revision")
        or not isinstance(runtime_units, list)
        or len(runtime_units) != 1
        or runtime_units[0].get("configuration", {}).get("mode") != "simulated"
        or not isinstance(controllable_components, list)
        or not controllable_components
    ):
        raise WorkbenchError(
            "maintained Scenario artifacts do not form one exact simulated tuple"
        )
    admission_filename = f"canonical-empty-admission.{graph['cell_id']}.json"
    _regular_file(PACKAGE_ROOT / admission_filename, "canonical-empty admission")
    workspace_program = WORKSPACE / "examples" / f"{program['program_id']}.program.yaml"
    _regular_file(workspace_program, "matching workspace Program")
    _require_current_workspace_deployment(row)
    _require_current_workspace_node_environment(binding)
    # Composer parses this artifact and the backend Load Scenario command
    # compares its semantic Program revision with the retained selection.  Do
    # not add a stricter byte-format identity in the process launcher.
    component_ids = [
        (
            component.get("controllable_component_id")
            if isinstance(component, dict)
            else None
        )
        for component in controllable_components
    ]
    if (
        any(not isinstance(item, str) or not item for item in component_ids)
        or component_ids != sorted(component_ids)
        or len(set(component_ids)) != len(component_ids)
    ):
        raise WorkbenchError(
            "maintained Scenario controllable components are not canonical"
        )
    return {
        "admission_filename": admission_filename,
        "build_profile": build_profile,
        "cell_id": graph["cell_id"],
        "component_ids": component_ids,
        "deployment_artifact_id": row["deployment_artifact_id"],
        "deployment_revision": row["deployment_revision"],
        "graph_revision": graph["graph_revision"],
        "program_binding_id": row["program_binding_id"],
        "program_id": program["program_id"],
        "program_path": str(workspace_program),
        "program_revision": program["program_revision"],
        "retained_bundle_id": row["retained_bundle_id"],
        "scenario_catalog_revision": catalog["scenario_catalog_revision"],
        "scenario_id": row["scenario_id"],
    }


def _ensure_frontend_dependencies() -> None:
    lock_path = FRONTEND_ROOT / "package-lock.json"
    marker = FRONTEND_ROOT / "node_modules/.manyforge-lock.sha256"
    digest = hashlib.sha256(lock_path.read_bytes()).hexdigest()
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == digest:
        return
    _run(("npm", "ci"), cwd=FRONTEND_ROOT)
    marker.write_text(digest + "\n", encoding="utf-8")


def _port_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listener.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def _wait_until(
    description: str,
    predicate: Callable[[], Any],
    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
    *,
    timeout_seconds: float = 20.0,
) -> Any:
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for name, process in processes:
            if process.poll() is not None:
                raise WorkbenchError(
                    f"{name} exited with status {process.returncode} while waiting "
                    f"for {description}"
                )
        try:
            result = predicate()
            if result:
                return result
        except Exception as error:
            last_error = error
        time.sleep(0.05)
    suffix = f": {last_error}" if last_error is not None else ""
    raise WorkbenchError(f"timed out waiting for {description}{suffix}")


def _manager_projection_ready(status: dict[str, Any], diff: dict[str, Any]) -> bool:
    """Accept only the existing fully converged Manager baseline."""

    source = status.get("source")
    payload = status.get("status")
    runtime = payload.get("runtime") if isinstance(payload, dict) else None
    diff_source = diff.get("source")
    return bool(
        isinstance(source, dict)
        and source.get("available") is True
        and source.get("freshness") == "fresh"
        and source.get("known_gaps") == []
        and isinstance(diff_source, dict)
        and diff_source.get("available") is True
        and diff_source.get("freshness") == "fresh"
        and diff_source.get("known_gaps") == []
        and isinstance(payload, dict)
        and payload.get("manager_ready") is True
        and isinstance(runtime, dict)
        and runtime.get("converged") is True
        and diff.get("converged") is True
    )


def _active_configuration_matches(active: object, desired: object) -> bool:
    """Compare only the retained route fields; owner facts remain projections."""

    if not isinstance(active, dict) or not isinstance(desired, dict):
        return False
    return active.get("state") == "reconciled" and all(
        active.get(field) == desired.get(field)
        for field in (
            "control_domain_id",
            "control_domain_revision",
            "controller_topology_id",
            "topology_revision",
            "activation_set_id",
            "binding_routes",
        )
    )


def _expected_active_configuration(active: object) -> dict[str, Any]:
    if active is None:
        return {"presence": "absent"}
    if not isinstance(active, dict):
        raise WorkbenchError("CCLC active route is not an object")
    identity = active.get("active_control_configuration_id")
    generation = active.get("active_control_configuration_generation")
    if not isinstance(identity, str) or not identity or type(generation) is not int:
        raise WorkbenchError("CCLC active route identity is incomplete")
    return {
        "presence": "present",
        "active_control_configuration_id": identity,
        "active_control_configuration_generation": generation,
    }


def _lifecycle_expected_domains(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
    domains = snapshot.get("domains")
    if not isinstance(domains, list) or not domains:
        raise WorkbenchError("CCLC component has no control domains")
    expected: list[dict[str, Any]] = []
    for domain in domains:
        if not isinstance(domain, dict):
            raise WorkbenchError("CCLC component contains an invalid control domain")
        row = {
            "control_domain_id": domain.get("control_domain_id"),
            "control_domain_revision": domain.get("control_domain_revision"),
            "controller_topology_id": domain.get("controller_topology_id"),
            "topology_revision": domain.get("topology_revision"),
            "expected_active_control_configuration": (
                _expected_active_configuration(
                    domain.get("active_control_configuration")
                )
            ),
            "expected_control_bindings": domain.get("current_control_bindings"),
        }
        if any(
            not isinstance(row[field], str) or not row[field]
            for field in (
                "control_domain_id",
                "control_domain_revision",
                "controller_topology_id",
                "topology_revision",
            )
        ) or not isinstance(row["expected_control_bindings"], list):
            raise WorkbenchError("CCLC control-domain identity is incomplete")
        expected.append(row)
    domain_ids = [row["control_domain_id"] for row in expected]
    if domain_ids != sorted(domain_ids) or len(set(domain_ids)) != len(domain_ids):
        raise WorkbenchError("CCLC control domains are not canonical")
    return expected


def _request_id(*parts: object) -> str:
    digest = hashlib.sha256(
        json.dumps(parts, separators=(",", ":"), ensure_ascii=True).encode()
    ).hexdigest()[:32]
    return f"development-scenario-start.{digest}"


def _wait_for_cclc_terminal(
    read_status: Callable[[str], dict[str, Any]],
    transaction_id: object,
    *,
    label: str,
    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
) -> dict[str, Any]:
    if not isinstance(transaction_id, str) or not transaction_id:
        raise WorkbenchError(f"CCLC {label} omitted its transaction identity")
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        for name, process in processes:
            if process.poll() is not None:
                raise WorkbenchError(
                    f"{name} exited with status {process.returncode} while "
                    f"waiting for CCLC {label}"
                )
        response = read_status(transaction_id)
        status = response.get("status")
        if status is None:
            time.sleep(0.05)
            continue
        if not isinstance(status, dict):
            raise WorkbenchError(f"CCLC {label} status is invalid")
        result = status.get("terminal_result")
        if result is None:
            time.sleep(0.05)
            continue
        if result != "succeeded":
            raise WorkbenchError(f"CCLC {label} failed: {result}")
        return status
    raise WorkbenchError(f"timed out waiting for CCLC {label}")


def _converge_component(
    client: HeadlessHttpClient,
    component_id: str,
    *,
    scenario_id: str,
    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
) -> None:
    """Bring one simulated Scenario component to its retained active route."""

    snapshot = _wait_until(
        f"CCLC component {component_id}",
        lambda: client.cclc_component(component_id),
        processes,
        timeout_seconds=20.0,
    )
    owner = client.cclc_owner_identity().get("request_owner_epoch")
    if not isinstance(owner, str) or not owner:
        raise WorkbenchError("CCLC owner identity is unavailable")
    for intent, expected_state in (("prepare", "prepared"), ("enable", "active")):
        state = snapshot.get("component_lifecycle_state")
        if state == "active" or state == expected_state:
            continue
        if state != ("declared" if intent == "prepare" else "prepared"):
            raise WorkbenchError(
                f"component {component_id} cannot start from lifecycle state {state!r}"
            )
        generation = snapshot.get("component_lifecycle_generation")
        graph_revision = snapshot.get("graph_revision")
        if type(generation) is not int or not isinstance(graph_revision, str):
            raise WorkbenchError("CCLC component lifecycle fence is incomplete")
        admission = client.authoring_admission().get("admission_generation")
        if type(admission) is not int:
            raise WorkbenchError("authoring admission generation is unavailable")
        request_id = _request_id(
            scenario_id, component_id, intent, graph_revision, generation
        )
        result = client.submit_component_lifecycle(
            {
                "request_context": {
                    "request_id": request_id,
                    "request_owner_epoch": owner,
                    "source_label": "development_scenario_start",
                    "correlation_id": scenario_id,
                },
                "controllable_component_id": component_id,
                "intent": intent,
                "expected_graph_revision": graph_revision,
                "expected_component_lifecycle_generation": generation,
                "expected_control_domains": _lifecycle_expected_domains(snapshot),
                "reason": "start selected simulated development Scenario",
                "confirmed": True,
                "expected_admission_generation": admission,
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise WorkbenchError(f"CCLC {intent} was not accepted")
        _wait_for_cclc_terminal(
            client.component_lifecycle_status,
            acceptance.get("lifecycle_transaction_id"),
            label=f"{component_id} {intent}",
            processes=processes,
        )
        snapshot = client.cclc_component(component_id)
        if snapshot.get("component_lifecycle_state") != expected_state:
            raise WorkbenchError(f"CCLC {component_id} did not become {expected_state}")

    if snapshot.get("component_lifecycle_state") != "active":
        raise WorkbenchError(f"CCLC component {component_id} is not active")
    templates = client.cclc_route_templates(component_id)
    rows = templates.get("templates")
    if not isinstance(rows, list) or not rows:
        raise WorkbenchError(f"CCLC component {component_id} has no route templates")
    rows = sorted(
        rows,
        key=lambda row: (
            row.get("desired_configuration", {}).get("control_domain_id", "")
            if isinstance(row, dict)
            else ""
        ),
    )
    snapshot_domains = {
        domain.get("control_domain_id"): domain
        for domain in snapshot.get("domains", [])
        if isinstance(domain, dict)
    }
    desired_rows = [
        (
            row,
            row.get("desired_configuration") if isinstance(row, dict) else None,
        )
        for row in rows
    ]
    desired_domain_ids = [
        desired.get("control_domain_id") if isinstance(desired, dict) else None
        for _row, desired in desired_rows
    ]
    if desired_domain_ids != sorted(snapshot_domains):
        raise WorkbenchError(
            f"CCLC routes do not exactly cover component {component_id}"
        )
    for row, desired in desired_rows:
        if not isinstance(row, dict) or not isinstance(desired, dict):
            raise WorkbenchError(f"CCLC route for {component_id} is invalid")
        domain_id = desired["control_domain_id"]
        if _active_configuration_matches(
            snapshot_domains[domain_id].get("active_control_configuration"),
            desired,
        ):
            continue
        admission = client.authoring_admission().get("admission_generation")
        if type(admission) is not int:
            raise WorkbenchError("authoring admission generation is unavailable")
        request_id = _request_id(
            scenario_id,
            component_id,
            domain_id,
            templates.get("retained_bundle_id"),
            templates.get("graph_revision"),
            desired,
            row.get("expected_active_control_configuration"),
            row.get("component_expectations"),
        )
        result = client.submit_route_selection(
            {
                "request_context": {
                    "request_id": request_id,
                    "request_owner_epoch": owner,
                    "source_label": "development_scenario_start",
                    "correlation_id": scenario_id,
                },
                "expected_admission_generation": admission,
                "expected_retained_bundle_id": templates.get("retained_bundle_id"),
                "expected_graph_revision": templates.get("graph_revision"),
                "desired_configuration": desired,
                "expected_active_control_configuration": row.get(
                    "expected_active_control_configuration"
                ),
                "component_expectations": row.get("component_expectations"),
                "confirmed": True,
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise WorkbenchError(f"CCLC route {domain_id} was not accepted")
        _wait_for_cclc_terminal(
            client.route_selection_status,
            acceptance.get("route_selection_transaction_id"),
            label=f"route {domain_id}",
            processes=processes,
        )
        snapshot = client.cclc_component(component_id)
        snapshot_domains = {
            domain.get("control_domain_id"): domain
            for domain in snapshot.get("domains", [])
            if isinstance(domain, dict)
        }
        if not _active_configuration_matches(
            snapshot_domains.get(domain_id, {}).get("active_control_configuration"),
            desired,
        ):
            raise WorkbenchError(f"CCLC route {domain_id} did not reconcile")


def _converge_control_management(
    client: HeadlessHttpClient,
    component_ids: Sequence[str],
    *,
    scenario_id: str,
    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
) -> str:
    """Converge only products that explicitly use component lifecycle control."""

    def current_mode() -> str | None:
        response = client.execution_identity()
        identity = response.get("identity")
        if not isinstance(identity, dict):
            raise WorkbenchError("execution identity is unavailable")
        mode = identity.get("control_management_mode")
        if mode == "pending":
            return None
        if mode not in {"direct", "component_lifecycle"}:
            raise WorkbenchError("execution control-management mode is invalid")
        return mode

    mode = _wait_until(
        "execution control management",
        current_mode,
        processes,
        timeout_seconds=30.0,
    )
    if mode == "direct":
        return mode
    for component_id in component_ids:
        _converge_component(
            client,
            component_id,
            scenario_id=scenario_id,
            processes=processes,
        )
    return mode


def query_status(*, scenario_id: str | None, composer_port: int) -> dict[str, Any]:
    """Read Composer's owner-derived current Scenario; retain no launcher copy."""

    with HeadlessHttpClient(
        f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0
    ) as client:
        result = client.request("GET", "/api/scenarios/current")
    current = result.get("current_scenario")
    if (
        scenario_id is not None
        and isinstance(current, dict)
        and current.get("scenario_id") != scenario_id
    ):
        result = {
            **result,
            "ready": False,
            "current_scenario": None,
            "unavailable_reasons": [
                *result.get("unavailable_reasons", []),
                "requested_scenario_mismatch",
            ],
        }
    return {
        **result,
        "classification": {
            "kind": "development_workbench",
            "package_kind": "maintained_qualified",
            "physical_execution_authorized": False,
        },
    }


def _startup_cycle_state(client: HeadlessHttpClient) -> str:
    """Accept only contained startup states; terminal state remains resettable."""

    cycle = client.program_cycle_status()
    state = cycle.get("state")
    if state not in {"idle_after_reset", "terminal_awaiting_reset"}:
        raise WorkbenchError(f"Program cycle is not contained at startup: {state!r}")
    return state


class Workbench:
    def __init__(
        self,
        *,
        scenario_id: str,
        headless_port: int,
        runner_port: int,
        composer_port: int,
        frontend_port: int,
        artifact_root: Path | None = None,
    ) -> None:
        self.scenario_id = scenario_id
        self.headless_port = headless_port
        self.runner_port = runner_port
        self.composer_port = composer_port
        self.frontend_port = frontend_port
        self.artifact_root = artifact_root or WORKSPACE / "examples"
        self.metadata: dict[str, Any] = {}
        self.processes: list[tuple[str, subprocess.Popen[bytes]]] = []
        self.stop_requested = False

    def _spawn(
        self,
        name: str,
        command: Sequence[str],
        *,
        cwd: Path | None = None,
    ) -> None:
        print(f"starting {name}: {' '.join(command)}", flush=True)
        process = subprocess.Popen(command, cwd=cwd, env=os.environ.copy())
        self.processes.append((name, process))

    def _paths(self) -> dict[str, Path]:
        paths = {
            "manager_state": STATE_ROOT / "manager",
            "manager_run": STATE_ROOT / "manager-run",
            "execution_state": STATE_ROOT / "execution-state",
            "execution_run": STATE_ROOT / "execution-run",
            "publications": STATE_ROOT / "publications",
            "artifacts": STATE_ROOT / "artifacts",
            "program_selection": STATE_ROOT / "program-selection-state",
            "composer_authoring": STATE_ROOT / "composer-authoring",
        }
        for path in paths.values():
            path.mkdir(parents=True, exist_ok=True)
        return paths

    def start(self) -> None:
        for port in (
            self.headless_port,
            self.runner_port,
            self.composer_port,
            self.frontend_port,
        ):
            if not _port_available(port):
                raise WorkbenchError(
                    f"required development port {port} is already in use"
                )
        _import_package()
        self.metadata = _load_scenario(self.scenario_id)
        current_headless_extension = _ensure_current_backend()
        current_native_targets = _ensure_current_native_backend()
        deployment_compiler, prepared_models = _ensure_deployment_compiler()
        _regular_file(
            MANAGER_BINARY,
            "current Manager executable",
            executable=True,
        )
        _regular_file(
            current_headless_extension,
            "current Headless Python extension",
        )
        os.environ["PYTHONPATH"] = (
            f"{HEADLESS_BUILD}:{os.environ.get('PYTHONPATH', '')}"
        )
        _ensure_frontend_dependencies()
        paths = self._paths()
        installed = STATE_ROOT / "installed-targets"
        installed.mkdir(parents=True, exist_ok=True)
        manager = installed / "manyforge_manager_daemon"
        shutil.copy2(MANAGER_BINARY, manager)
        _regular_file(manager, "installed Manager", executable=True)
        for source in current_native_targets:
            target = installed / source.name
            shutil.copy2(source, target)
            _regular_file(target, f"installed {source.name}", executable=True)
        manager_socket = paths["manager_run"] / "manager-command.sock"
        runner_socket = paths["manager_run"] / "program-runner-liveness.sock"
        _remove_stale_socket(manager_socket, "Manager command socket")
        _remove_stale_socket(runner_socket, "Program runner liveness socket")
        authoring_journal = (
            paths["composer_authoring"] / "composer-authoring-transactions.sqlite3"
        )
        # Bring up the two reconciliation services before Manager.  A new
        # Manager must be able to reconcile the retained Program Runner owner
        # on its first heartbeat; starting Manager first creates a needless
        # dependency cycle in which its native child can expire while the
        # Runner is still waiting for Headless cell-admission recovery.
        self._spawn(
            "Program runner",
            (
                sys.executable,
                "-m",
                "manyforge_program_runner",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.runner_port),
                "--headless-url",
                f"http://127.0.0.1:{self.headless_port}",
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cycle-journal-path",
                str(STATE_ROOT / "program-cycle.sqlite3"),
                "--manager-liveness-socket",
                str(runner_socket),
                "--cell-id",
                self.metadata["cell_id"],
                "--owner-id",
                "program-runner.development-workbench",
            ),
        )
        self._spawn(
            "Headless API",
            (
                sys.executable,
                "-m",
                "manyforge_headless_api",
                "serve",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.headless_port),
                "--projection-root",
                str(paths["publications"]),
                "--install-root",
                str(PACKAGE_ROOT),
                "--artifact-root",
                str(paths["artifacts"]),
                "--program-selection-state-root",
                str(paths["program_selection"]),
                "--node-catalog-path",
                str(NODE_CATALOG),
                "--node-environment-manifest-path",
                str(NODE_ENVIRONMENT_MANIFEST),
                "--alarm-catalog-path",
                str(ALARM_CATALOG),
                "--historian-database-path",
                str(STATE_ROOT / "history.sqlite3"),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cell-id",
                self.metadata["cell_id"],
                "--owner-id",
                "headless-api.development-workbench",
                "--freshness-ms",
                "2000",
                "--manager-timeout-s",
                "15",
                "--program-runner-url",
                f"http://127.0.0.1:{self.runner_port}",
                "--deployment-compiler",
                str(deployment_compiler),
                "--deployment-compiler-workspace-root",
                str(WORKSPACE),
                *(
                    argument
                    for family_id, prepared_root in prepared_models
                    for argument in (
                        "--deployment-compiler-prepared",
                        f"{family_id}={prepared_root}",
                    )
                ),
            ),
        )
        base_url = f"http://127.0.0.1:{self.headless_port}"

        def reconciliation_services_ready() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.runner_port}", timeout_seconds=0.5
            ) as client:
                runner = client.request("GET", "/api/program/health")
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                api = client.runtime()
            return bool(runner.get("started") is True and api.get("schema_version"))

        _wait_until(
            "Program Runner and Headless reconciliation services",
            reconciliation_services_ready,
            self.processes,
        )
        self._spawn(
            "Manager",
            (
                str(manager),
                "--install-root",
                str(PACKAGE_ROOT),
                "--candidate-package-root",
                str(paths["artifacts"]),
                "--manager-state-root",
                str(paths["manager_state"]),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-state-root",
                str(paths["execution_state"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--logical-scene-database",
                str(STATE_ROOT / "logical-scene.sqlite3"),
                "--program-cycle-journal",
                str(STATE_ROOT / "program-cycle.sqlite3"),
                "--authoring-transaction-journal",
                str(authoring_journal),
                "--projection-root",
                str(paths["publications"]),
                "--canonical-empty-admission-filename",
                self.metadata["admission_filename"],
                "--program-runner-liveness-socket",
                str(runner_socket),
            ),
        )
        _wait_until(
            "Manager command socket",
            manager_socket.is_socket,
            self.processes,
        )

        def manager_ready() -> dict[str, Any] | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                status = client.manager_status()
                diff = client.manager_diff()
                if _manager_projection_ready(status, diff):
                    return status
            return None

        _wait_until("fresh converged Manager baseline", manager_ready, self.processes)

        def node_environment_ready() -> dict[str, Any] | None:
            # Manager convergence and Program Runner availability are
            # published independently. Composer startup consumes their exact
            # joined environment, so wait for that existing Headless contract
            # instead of relying on process timing or retrying a failed load.
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                environment = client.node_environment()
                return environment if environment.get("schema_version") else None

        _wait_until(
            "active Program node environment",
            node_environment_ready,
            self.processes,
        )

        def contained_startup_cycle() -> str | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                try:
                    return _startup_cycle_state(client)
                except WorkbenchError:
                    # Manager rebuilds the exact retained native product on its
                    # existing asynchronous builder lane.  A replacement Runner
                    # stays fenced until that product can certify containment;
                    # treat the intervening active state as startup progress,
                    # never as permission to continue the old run.
                    return None

        startup_cycle_state = _wait_until(
            "contained Program-cycle startup reconciliation",
            contained_startup_cycle,
            self.processes,
        )

        def authoring_ready() -> dict[str, Any] | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                admission = client.authoring_admission()
                return admission if admission.get("authoring_allowed") is True else None

        composer_environment = os.environ.copy()
        composer_environment["MANYFORGE_CORS_ORIGINS"] = (
            f"http://localhost:{self.frontend_port},"
            f"http://127.0.0.1:{self.frontend_port}"
        )
        composer_environment["MANYFORGE_FS_ROOTS"] = str(self.artifact_root)
        composer_environment["MANYFORGE_PROGRAM_WRITE_ROOTS"] = str(self.artifact_root)
        composer_environment["MANYFORGE_DEPLOYMENT_WRITE_ROOTS"] = str(
            self.artifact_root
        )
        print("starting Composer backend", flush=True)
        # Composer owns recovery of its durable authoring transactions. Start
        # it once the current Manager/Kernel baseline is reachable, even when
        # an interrupted prior transaction still holds authoring admission;
        # waiting for admission first would prevent the sole recovery owner
        # from ever running.
        composer_command = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.composer_port),
            "--program-path",
            self.metadata["program_path"],
            "--headless-api-url",
            base_url,
            "--headless-api-timeout-s",
            "20",
            "--authoring-state-root",
            str(paths["composer_authoring"]),
        ]
        if startup_cycle_state == "idle_after_reset":
            composer_command.extend(("--startup-scenario", self.scenario_id))
        composer = subprocess.Popen(
            composer_command,
            env=composer_environment,
        )
        self.processes.append(("Composer backend", composer))

        def composer_ready() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.composer_port}", timeout_seconds=0.5
            ) as client:
                return bool(client.health())

        _wait_until("Composer backend", composer_ready, self.processes)

        def exact_status() -> dict[str, Any] | None:
            result = query_status(
                scenario_id=self.metadata["scenario_id"],
                composer_port=self.composer_port,
            )
            return result if result["ready"] else None

        _wait_until("exact current Scenario tuple", exact_status, self.processes)
        if startup_cycle_state == "idle_after_reset":
            with HeadlessHttpClient(base_url, timeout_seconds=1.0) as client:
                _converge_control_management(
                    client,
                    self.metadata["component_ids"],
                    scenario_id=self.metadata["scenario_id"],
                    processes=self.processes,
                )
            _wait_until("idle authoring admission", authoring_ready, self.processes)
        vite_environment = os.environ.copy()
        vite_environment["MANYFORGE_COMPOSER_PROXY_TARGET"] = (
            f"http://127.0.0.1:{self.composer_port}"
        )
        print("starting Vite HMR", flush=True)
        vite = subprocess.Popen(
            (
                "node",
                str(FRONTEND_ROOT / "node_modules/vite/bin/vite.js"),
                "--host",
                "0.0.0.0",
                "--port",
                str(self.frontend_port),
            ),
            cwd=FRONTEND_ROOT,
            env=vite_environment,
        )
        self.processes.append(("Vite HMR", vite))

        def frontend_ready() -> bool:
            with socket.create_connection(
                ("127.0.0.1", self.frontend_port), timeout=0.5
            ):
                return True

        _wait_until("Vite HMR", frontend_ready, self.processes)
        print(
            _json(
                {
                    "ready": True,
                    "url": f"http://localhost:{self.frontend_port}",
                    "scenario_id": self.scenario_id,
                    "classification": (
                        "development workbench; maintained qualified package; "
                        "simulated execution only"
                    ),
                }
            ),
            flush=True,
        )

    def _shutdown_manager(self) -> None:
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is None or manager.poll() is not None:
            return
        request = (
            b'{"api_version":"manyforge.manager_command.v1",'
            b'"command":"shutdown_runtime"}'
        )
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1.0)
                client.connect(str(STATE_ROOT / "manager-run/manager-command.sock"))
                client.sendall(request)
                response = client.recv(16 * 1024)
                if b'"ok":true' not in response:
                    raise WorkbenchError("Manager rejected bounded shutdown")
            manager.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired, WorkbenchError):
            pass

    def _stop_manager(self) -> None:
        """Stop Manager before waiting for the Runner's short lease."""

        self._shutdown_manager()
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is not None:
            # The bounded command is preferred, but a failed command must not
            # leave Manager renewing the lease until the container disappears.
            self._terminate(manager)

    def _wait_for_program_runner_manager_expiry(self) -> None:
        """Let the runner durably observe Manager loss before it exits."""

        runner = next(
            (process for name, process in self.processes if name == "Program runner"),
            None,
        )
        if runner is None or runner.poll() is not None:
            return

        def manager_expired() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.runner_port}", timeout_seconds=0.2
            ) as client:
                status = client.request("GET", "/api/program/liveness/status")
                cycle = client.program_cycle_status()
            return bool(
                status.get("state") == "expired"
                and status.get("command_fenced") is True
                and cycle.get("state")
                in {"idle_after_reset", "terminal_awaiting_reset"}
            )

        try:
            _wait_until(
                "Program Runner Manager-loss retention",
                manager_expired,
                (("Program runner", runner),),
                timeout_seconds=2.0,
            )
        except WorkbenchError:
            # Cleanup remains bounded even if the runner is already unhealthy.
            # Its next start will retain/fence the predecessor through the same
            # watchdog before accepting a replacement Manager.
            pass

    @staticmethod
    def _terminate(process: subprocess.Popen[bytes]) -> None:
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    def close(self) -> None:
        for name in ("Vite HMR", "Composer backend"):
            for process_name, process in reversed(self.processes):
                if process_name == name:
                    self._terminate(process)
        self._stop_manager()
        self._wait_for_program_runner_manager_expiry()
        for process_name, process in reversed(self.processes):
            if process_name == "Program runner":
                self._terminate(process)
        for process_name, process in reversed(self.processes):
            if process_name == "Headless API":
                self._terminate(process)
        for name, process in reversed(self.processes):
            if name not in {
                "Vite HMR",
                "Composer backend",
                "Program runner",
                "Manager",
                "Headless API",
            }:
                self._terminate(process)

    def serve(self) -> None:
        def request_stop(_signum: int, _frame: Any) -> None:
            self.stop_requested = True

        signal.signal(signal.SIGTERM, request_stop)
        signal.signal(signal.SIGINT, request_stop)
        try:
            self.start()
            while not self.stop_requested:
                for name, process in self.processes:
                    if process.poll() is not None:
                        raise WorkbenchError(
                            f"{name} exited unexpectedly with status "
                            f"{process.returncode}"
                        )
                time.sleep(0.2)
        finally:
            self.close()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="dev-workbench.py")
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name in ("serve", "status"):
        command = subcommands.add_parser(name)
        command.add_argument("--scenario", required=name == "serve")
        command.add_argument("--headless-port", type=int, default=9010)
        command.add_argument("--runner-port", type=int, default=9011)
        command.add_argument("--composer-port", type=int, default=9020)
        command.add_argument("--frontend-port", type=int, default=3000)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "status":
            result = query_status(
                scenario_id=args.scenario,
                composer_port=args.composer_port,
            )
            print(_json(result))
            return 0 if result["ready"] else 69
        Workbench(
            scenario_id=args.scenario,
            headless_port=args.headless_port,
            runner_port=args.runner_port,
            composer_port=args.composer_port,
            frontend_port=args.frontend_port,
        ).serve()
        return 0
    except (HeadlessClientError, OSError, ValueError, WorkbenchError) as error:
        print(f"development_workbench_unavailable: {error}", file=sys.stderr)
        return 69


if __name__ == "__main__":
    raise SystemExit(main())
