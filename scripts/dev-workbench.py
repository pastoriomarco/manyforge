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
from typing import Any, Sequence

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
from manyforge_operator.application_processes import (
    MANAGER_STOPPED,
    START_AFTER_BASELINE,
    ApplicationProcess,
    parent_death_preexec,
    request_manager_shutdown,
    write_document as write_application_processes,
)
from scripts.lib.export_retention import _LEGACY_SESSION_ENTRIES, create_host_ros_log_session, run_session
from scripts.lib.workbench_session import Session


WORKSPACE = Path("/workspace")
CACHE_ROOT = Path("/runner")
BUILD_ROOT = CACHE_ROOT / "build"
STATE_ROOT = Path(os.environ.get("MANYFORGE_DEV_STATE_ROOT", "/tmp/manyforge-state"))
# Removed disk-limit variables and the site settings member that replaced each.
REMOVED_DISK_VARIABLES = {
    "MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT": "disk_max_percent",
    "MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES": "disk_always_free_bytes",
}
HEADLESS_BUILD = BUILD_ROOT / "headless"
DEPLOYMENT_AUTHORING_BUILD = BUILD_ROOT / "deployment-authoring"
# The Core Python module (manyforge_core_py): Headless reference validation
# (the GUI's and the assistant's) prepares candidates with it.
CORE_BINDINGS_CORE_BUILD = BUILD_ROOT / "core-bindings-core"
CORE_PYTHON_BUILD = BUILD_ROOT / "core-python"
# Python packages the workbench services import from the source tree.
SERVED_PYTHON_PACKAGES = (
    "manyforge_behavior/manyforge_behavior",
    "manyforge_headless_api",
    "manyforge_program_runner",
    "manyforge_composer/backend",
    "manyforge_assistant_bridge",
    "manyforge_ros",
)
BOOTSTRAP_ROOT = CACHE_ROOT / "repository-bootstrap-package"
# Installation configuration, independent of the optional startup project.
# Preserve the existing workbench identity; never mint one per Load or restart.
WORKBENCH_CELL_ID = "c1_build_authority_cell"
BOOTSTRAP_ADMISSION = f"canonical-empty-admission.{WORKBENCH_CELL_ID}.json"
ACTIVATION_PREPARER = (
    DEPLOYMENT_AUTHORING_BUILD / "manyforge_prepare_activation"
)
CANONICAL_EMPTY_GENERATOR = (
    DEPLOYMENT_AUTHORING_BUILD / "manyforge_generate_canonical_empty_package"
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
# Written by the native build step; serve reports it beside its readiness time.
NATIVE_BUILD_TIMING = BUILD_ROOT / "native-build-timing.json"
# How often the wait for the Manager's exit passes further stop requests on.
MANAGER_STOP_POLL_SECONDS = 0.25


class WorkbenchError(RuntimeError):
    """One precise orchestration failure."""


class NativeBuildMissingError(WorkbenchError):
    """serve found a native artifact the separate build step must produce."""


def _project_scenario_id(raw_path: str, *, allowed_root: Path) -> str:
    """Read only the bounded project identity needed by launcher status."""

    from manyforge_behavior.canonical_artifact import (
        StrictArtifactError,
        load_strict_json_yaml,
    )

    root = allowed_root.resolve(strict=True)
    candidate = Path(raw_path).expanduser()
    project = candidate.resolve(strict=True)
    if (
        candidate.is_symlink()
        or not project.is_dir()
        or not project.is_relative_to(root)
    ):
        raise WorkbenchError("Scenario project is outside the workbench project root")
    manifest = project / "scenario.yaml"
    if (
        manifest.is_symlink()
        or not manifest.is_file()
        or manifest.stat().st_size > 64 * 1024
    ):
        raise WorkbenchError("Scenario project manifest is unavailable or oversized")
    try:
        value = load_strict_json_yaml(manifest)
    except StrictArtifactError as error:
        raise WorkbenchError(str(error)) from error
    if value.get("api_version") != "manyforge.scenario.project.v3":
        raise WorkbenchError(
            "Unsupported Scenario project version; re-author it with direct references"
        )
    scenario_id = value.get("scenario_id")
    if (
        not isinstance(scenario_id, str)
        # One name: folder `<name>.scenario`, scenario_id == name == <name>.
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", scenario_id) is None
        or value.get("name") != scenario_id
        or project.name != f"{scenario_id}.scenario"
    ):
        raise WorkbenchError(
            "Scenario project identity is invalid: the folder must be "
            "'<name>.scenario' and its scenario_id and name must both be <name>"
        )
    return scenario_id


def _json(value: Any) -> str:
    return json.dumps(value, allow_nan=False, indent=2, sort_keys=True)


def _composer_filesystem_roots(artifact_root: Path) -> str:
    """Keep examples available alongside folders mounted by the launcher."""

    extra_roots = os.environ.get("MANYFORGE_DEV_PROJECT_ROOTS", "")
    return f"{artifact_root},{extra_roots}" if extra_roots else str(artifact_root)


def _run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, env=env, check=False)
    if completed.returncode != 0:
        raise WorkbenchError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}"
        )


def _persistent_key_register_directory() -> Path:
    """Installation state beside the Composer settings, owned by the runner,
    which names its register file in here by format version."""

    state_dir = os.environ.get("MANYFORGE_STATE_DIR", "").strip()
    if not state_dir or not Path(state_dir).is_absolute():
        raise WorkbenchError(
            "MANYFORGE_STATE_DIR must name the persistent installation state "
            "directory (scripts/compose-dev.sh sets it)"
        )
    return Path(state_dir) / "program-runner"


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
    return _current_backend()


def _current_backend() -> Path:
    _built_file(MANAGER_BINARY, "current Manager", executable=True)
    return _built_extension(HEADLESS_BUILD, "manyforge_headless_py", "current Headless Python extension")


def _ros_setup_environment(
    setups: Sequence[Path], environment: dict[str, str]
) -> dict[str, str]:
    """Import explicitly selected ROS setups in order, using literal paths."""

    completed = subprocess.run(
        ("bash", "-c", 'for setup in "$@"; do source "$setup" >/dev/null || exit; done; env -0',
         "bash", *(str(setup) for setup in setups)),
        check=False,
        capture_output=True,
        env=environment,
    )
    if completed.returncode != 0:
        raise WorkbenchError(
            "cannot source ROS setup scripts: " + ", ".join(str(setup) for setup in setups)
        )
    imported = {}
    for entry in completed.stdout.split(b"\0"):
        if entry:
            key, value = os.fsdecode(entry).split("=", 1)
            imported[key] = value
    return imported


def _ros_backend_environment() -> dict[str, str]:
    """The ROS build and runtime environment, before the application overlay."""

    build_root = BUILD_ROOT / "isaac"
    environment = os.environ.copy()
    configured_setups = environment.get("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", "")
    if configured_setups:
        setups = []
        for configured in configured_setups.split(os.pathsep):
            setup = Path(configured)
            if not configured or not setup.is_absolute() or not setup.is_file():
                raise WorkbenchError(
                    f"configured ROS dependency setup is unavailable: {configured!r}"
                )
            setups.append(setup)
        environment = _ros_setup_environment(setups, environment)
    environment.update(
        WORKSPACE=str(WORKSPACE),
        MANYFORGE_ROS2_CONTROL_BUILD_ROOT=str(build_root),
        MANYFORGE_ROBOPLAN_BUILD_DIR=str(BUILD_ROOT / "ros-integration"),
        MANYFORGE_ISAAC_ROS_OVERLAY=str(build_root / "ros-overlay"),
        CMAKE_BUILD_PARALLEL_LEVEL=_build_jobs(),
        CMAKE_GENERATOR="Ninja",
        MANYFORGE_ROS2_CONTROL_BUILD_TESTING=os.environ.get(
            "MANYFORGE_ROS2_CONTROL_BUILD_TESTING", "OFF"
        ),
    )
    return environment


def _ensure_current_ros_backend() -> tuple[Path, Path]:
    """Build the separately installed ROS product; never launch a provider."""

    environment = _ros_backend_environment()
    _run(
        (
            str(WORKSPACE / "scripts/ros/build-ros2-control-runtime.sh"),
            "--source-dir",
            str(WORKSPACE / "scripts/ros/ros-integration"),
        ),
        env=environment,
    )
    return _current_ros_backend(environment)


def _current_ros_backend(environment: dict[str, str]) -> tuple[Path, Path]:
    """Check the built ROS product and import its overlay into this process."""

    ros_build = BUILD_ROOT / "ros-integration"
    overlay = BUILD_ROOT / "isaac/ros-overlay/install/setup.bash"
    targets = (
        ros_build / "manyforge_roboplan_candidate_worker",
        ros_build / "manyforge_ros2_control_execution_host",
    )
    for target in targets:
        _built_file(target, f"current {target.name}", executable=True)
    _built_file(overlay, "current ROS overlay setup")
    # The build helper's exports belong to its subprocess. Import the generated
    # overlay before spawning the Runner or Manager; Manager's installed-target
    # children inherit this same environment through their existing protocol.
    os.environ.update(_ros_setup_environment((overlay,), environment))
    # The execution host loads its adapter families from the build tree's
    # ament prefix.
    os.environ["AMENT_PREFIX_PATH"] = os.pathsep.join(
        path for path in (str(ros_build / "ament_cmake_index"),
                          os.environ.get("AMENT_PREFIX_PATH", "")) if path
    )
    return targets


def _stage_runtime_targets(sources: Sequence[Path], installed: Path) -> None:
    for source in sources:
        target = installed / source.name
        shutil.copy2(source, target)
        _regular_file(target, f"installed {source.name}", executable=True)


def _ensure_core_bindings() -> Path:
    """Build the Core Python module against the authoring Deployment build.

    Same inputs as scripts/ci/run-core-bindings.sh: the Core and Deployment
    static archives, then the module itself.
    """

    if not (CORE_BINDINGS_CORE_BUILD / "CMakeCache.txt").is_file():
        _run((
            "cmake", "-G", "Ninja", "-S", str(WORKSPACE / "manyforge_core"),
            "-B", str(CORE_BINDINGS_CORE_BUILD),
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        ))
    _run((
        "cmake", "--build", str(CORE_BINDINGS_CORE_BUILD), "--parallel", _build_jobs(),
        "--target", "manyforge_content_identity", "manyforge_core",
    ))
    _run((
        "cmake", "--build", str(DEPLOYMENT_AUTHORING_BUILD), "--parallel", _build_jobs(),
        "--target", "manyforge_content_identity", "manyforge_deployment",
        "manyforge_deployment_admission", "manyforge_deployment_canonical_package_generator",
        "manyforge_deployment_verification_core", "manyforge_expression",
        "manyforge_preparation_support",
    ))
    if not (CORE_PYTHON_BUILD / "CMakeCache.txt").is_file():
        completed = subprocess.run(
            (sys.executable, "-c", "import pybind11; print(pybind11.get_cmake_dir())"),
            check=False, capture_output=True, text=True,
        )
        pybind11_dir = completed.stdout.strip()
        if completed.returncode != 0 or not pybind11_dir:
            raise WorkbenchError("cannot resolve the development pybind11 CMake package")
        _run((
            "cmake", "-G", "Ninja", "-S", str(WORKSPACE / "manyforge_core/python"),
            "-B", str(CORE_PYTHON_BUILD), "-DCMAKE_BUILD_TYPE=Release",
            f"-DPython3_EXECUTABLE={sys.executable}", f"-DPython_EXECUTABLE={sys.executable}",
            f"-Dpybind11_DIR={pybind11_dir}",
            f"-DMANYFORGE_CORE_DIR={CORE_BINDINGS_CORE_BUILD}",
            f"-DMANYFORGE_CORE_INCLUDE_DIR={WORKSPACE / 'manyforge_core/include'}",
            f"-DMANYFORGE_DEPLOYMENT_DIR={DEPLOYMENT_AUTHORING_BUILD}",
            f"-DMANYFORGE_DEPLOYMENT_INCLUDE_DIR={WORKSPACE / 'manyforge_deployment/include'}",
            f"-DMANYFORGE_CONTENT_IDENTITY_SOURCE_DIR={WORKSPACE / 'manyforge_content_identity'}",
        ))
    _run((
        "cmake", "--build", str(CORE_PYTHON_BUILD), "--parallel", _build_jobs(),
        "--target", "manyforge_core_py",
    ))
    return _current_core_bindings()


def _current_core_bindings() -> Path:
    return _built_extension(CORE_PYTHON_BUILD, "manyforge_core_py", "current Core Python extension")


def _ensure_activation_preparer() -> Path:
    """Build repository-owned activation and empty-bootstrap tools."""

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
                "-DBUILD_TESTING=OFF",
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
            "manyforge_prepare_activation",
            "manyforge_generate_canonical_empty_package",
            "--",
            "-k",
            "0",
        )
    )
    return _current_activation_preparer()


def _current_activation_preparer() -> Path:
    _built_file(ACTIVATION_PREPARER, "activation preparer", executable=True)
    _built_file(CANONICAL_EMPTY_GENERATOR, "canonical-empty generator", executable=True)
    return ACTIVATION_PREPARER


def build_native() -> None:
    """The native build step; serve only checks its artifacts."""

    started = time.monotonic()
    _ensure_current_backend()
    _ensure_current_ros_backend()
    _ensure_activation_preparer()
    _ensure_core_bindings()
    # Bytecode for the served Python packages, compiled by the same
    # interpreter the services use: a cold first start then reaches
    # readiness within its bounded reconciliation wait.
    _run((sys.executable, "-m", "compileall", "-q", "-j", "0",
          *(str(WORKSPACE / package) for package in SERVED_PYTHON_PACKAGES)))
    NATIVE_BUILD_TIMING.write_text(
        _json({"build_elapsed_seconds": time.monotonic() - started}) + "\n", encoding="utf-8"
    )


def _ensure_repository_bootstrap_package() -> Path:
    """Materialize selected infrastructure without selecting any Scenario."""

    runtime = "ros2_control"
    profile = (
        WORKSPACE / "manyforge_deployment/resources/maintained/catalog/"
        f"runtime_profile/manyforge.{runtime}_execution_runtime.json"
    )
    # Cache placement only, not a semantic artifact identity. Never overwrite
    # an immutable admission created by an older compiler/profile.
    digest = hashlib.sha256(f"{runtime}-workbench:{WORKBENCH_CELL_ID}\0".encode())
    for source in (CANONICAL_EMPTY_GENERATOR, profile):
        with source.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        digest.update(b"\0")
    package_root = BOOTSTRAP_ROOT / digest.hexdigest()
    package_root.mkdir(parents=True, exist_ok=True)
    _run(
        (
            str(CANONICAL_EMPTY_GENERATOR),
            "--output-root",
            str(package_root),
            "--cell-id",
            WORKBENCH_CELL_ID,
            "--ros-runtime-profile",
            str(profile),
        )
    )
    _regular_file(
        package_root / BOOTSTRAP_ADMISSION,
        "repository canonical-empty admission",
    )
    return package_root


def _capture_ros_bootstrap_project(
    project_path: str, *, allowed_root: Path
) -> tuple[dict[str, Any], str]:
    """Capture the intended first Load through the ordinary authoring owners."""

    from manyforge_behavior.model_source_capture import (
        ModelSourceCaptureError,
        capture_deployment_model_sources,
    )
    from manyforge_behavior.node_environment import build_node_environment
    from manyforge_composer.backend.authoring_forest import (
        AuthoringForestError,
        derive_candidate,
        validate_invariants,
    )
    from manyforge_composer.backend.deployment_intent import MODEL_FREE_RUNTIME_KINDS
    from manyforge_composer.backend.scenario_artifacts import (
        ScenarioArtifactError,
        capture_scenario_artifact,
    )
    from manyforge_composer.backend.state import canonical_project_path

    try:
        scenario, source_path, source_digest = capture_scenario_artifact(
            project_path, allowed_roots=[str(allowed_root)]
        )
        deployment = scenario["deployment_draft"]
        runtime_kinds = {
            component.get("configuration", {}).get("runtime_kind")
            for component in (deployment or {}).get("components", [])
            if "runtime_kind" in component.get("configuration", {})
        }
        # Model-free components (I/O providers, pose sources, AI models) run
        # inside the same ROS execution host. The activation preparer remains
        # the real gate; this only refuses what ROS startup cannot run.
        if "ros2_control" not in runtime_kinds or not runtime_kinds <= (
            {"ros2_control"} | MODEL_FREE_RUNTIME_KINDS
        ):
            raise WorkbenchError(
                "ROS startup requires an intended first Scenario with a "
                "ros2_control execution component and otherwise only I/O "
                "provider, pose source or AI model components"
            )
        catalog = build_node_environment().catalog
        authoring = scenario["program_draft"]
        if authoring is None:
            raise WorkbenchError("intended first ROS Scenario is incomplete")
        validate_invariants(authoring, catalog)
        program, diagnostics = derive_candidate(authoring, catalog)
        no_program = authoring.get("forest") == [] and not authoring.get("scene")
        if diagnostics or (program is None and not no_program):
            raise WorkbenchError(
                "intended first ROS Scenario has an incomplete authored Program"
            )
        captured = {
            "scenario_id": scenario["scenario_id"],
            "project_path": canonical_project_path(source_path),
            "source_document_digest": source_digest,
            "deployment": deployment,
            "model_source_closure": capture_deployment_model_sources(
                deployment, project_root=Path(source_path)
            ),
            "program": program.to_dict() if program is not None else None,
        }
    except (ScenarioArtifactError, AuthoringForestError, ModelSourceCaptureError) as error:
        raise WorkbenchError(str(error)) from error
    return captured, source_path


def _ensure_ros_bootstrap_package(
    captured: dict[str, Any], *, activation_preparer: Path
) -> Path:
    """Publish the exact first Deployment's empty predecessor before Manager."""

    from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
    from manyforge_headless_api.activation_preparer import ActivationPreparer
    from manyforge_headless_api.native import HeadlessApiError

    captured_digest = raw_sha256_digest(canonical_json_bytes(captured))
    digest = hashlib.sha256(
        f"ros-workbench:{WORKBENCH_CELL_ID}\0{captured_digest}\0".encode()
    )
    digest.update(activation_preparer.read_bytes())
    package_root = BOOTSTRAP_ROOT / digest.hexdigest()
    package_root.mkdir(parents=True, exist_ok=True)
    try:
        preparer = ActivationPreparer(
            executable=activation_preparer,
            capability_root=WORKSPACE,
            activation_root=package_root,
            runtime_cell_id=WORKBENCH_CELL_ID,
            timeout_seconds=180,
            publish_canonical_empty=True,
        )
        preparer.prepare(captured_input_digest=captured_digest, **captured)
    except HeadlessApiError as error:
        raise WorkbenchError(str(error)) from error
    _regular_file(package_root / BOOTSTRAP_ADMISSION, "ROS canonical-empty admission")
    return package_root


def _regular_file(path: Path, label: str, *, executable: bool = False) -> None:
    if path.is_symlink() or not path.is_file():
        raise WorkbenchError(f"{label} is not a regular file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise WorkbenchError(f"{label} is not executable: {path}")


_NATIVE_BUILD_HINT = (
    "run the native build step first: python3 scripts/dev-workbench.py build "
    "(scripts/compose-dev.sh start runs it before serve)"
)


def _built_file(path: Path, label: str, *, executable: bool = False) -> None:
    try:
        _regular_file(path, label, executable=executable)
    except WorkbenchError as error:
        raise NativeBuildMissingError(f"{error}; {_NATIVE_BUILD_HINT}") from error


def _built_extension(build: Path, module: str, label: str) -> Path:
    extensions = sorted(build.glob(f"{module}*.so"))
    if len(extensions) != 1:
        raise NativeBuildMissingError(f"{label} is absent or ambiguous in {build}; {_NATIVE_BUILD_HINT}")
    _built_file(extensions[0], label)
    return extensions[0]


def _native_build_elapsed() -> float | None:
    try:
        return json.loads(NATIVE_BUILD_TIMING.read_text(encoding="utf-8"))["build_elapsed_seconds"]
    except FileNotFoundError:
        return None


def _remove_stale_socket(path: Path, label: str) -> None:
    """Remove one named ephemeral socket without touching persistent state."""

    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(mode):
        raise WorkbenchError(f"{label} is not a socket: {path}")
    path.unlink()


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


def query_status(*, scenario_id: str | None, composer_port: int) -> dict[str, Any]:
    """Read Composer readiness and, when selected, the exact active project."""

    with HeadlessHttpClient(
        f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0
    ) as client:
        health = client.health()
        result = client.request("GET", "/api/scenarios/current")
    if scenario_id is None:
        result = {
            **result,
            "ready": bool(health),
            "unavailable_reasons": (
                [] if health else ["composer_unavailable"]
            ),
        }
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
            "package_kind": "repository_source",
            "physical_execution_authorized": False,
        },
    }


def _operator_manager_ready(manager: dict[str, Any]) -> bool:
    source = manager.get("source")
    payload = manager.get("status")
    return (isinstance(source, dict) and isinstance(payload, dict)
            and source.get("available") is True and source.get("freshness") == "fresh"
            and source.get("known_gaps") == [] and payload.get("manager_ready") is True)


def query_operator_status(*, headless_port: int, runner_port: int,
                          composer_port: int) -> dict[str, Any]:
    """Read real owner payloads; timing is never a readiness predicate."""
    with HeadlessHttpClient(f"http://127.0.0.1:{headless_port}", timeout_seconds=1.0) as client:
        headless = client.health()
        manager = client.manager_status()
    with HeadlessHttpClient(f"http://127.0.0.1:{runner_port}", timeout_seconds=1.0) as client:
        runner = client.request("GET", "/api/program/health")
    with HeadlessHttpClient(f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0) as client:
        composer = client.health()
    if not all(isinstance(row, dict) for row in (manager, headless, runner, composer)):
        raise WorkbenchError("owner status must be an object")
    ready = (_operator_manager_ready(manager)
             and headless.get("status") == "ready"
             and headless.get("headless_projection_ready") is True
             and runner.get("started") is True and runner.get("closed") is False
             and composer.get("status") == "ready")
    try:
        timing = json.loads((STATE_ROOT / "current/workbench-startup-timing.json").read_text())
    except (OSError, ValueError):
        timing = None
    return {"ready": ready, "manager": manager, "headless": headless,
            "program_runner": runner, "composer": composer, "timing": timing}


class Workbench:
    def __init__(
        self,
        *,
        project_path: str | None,
        headless_port: int,
        runner_port: int,
        composer_port: int,
        frontend_port: int,
        artifact_root: Path | None = None,
        ros2_control: bool = True,
        bootstrap_project_path: str | None = None,
        operator_install_root: Path | None = None,
        operator_cell_id: str | None = None,
        canonical_empty_admission_filename: str | None = None,
        composer_static_dir: Path | None = None,
        composer_bind_host: str = "127.0.0.1",
        simulation_control_config: Path | None = None,
        site_settings: Path | None = None,
        package_aggregate_digest: str | None = None,
        operator_instance_id: str | None = None,
        node_catalog_path: Path | None = None,
        alarm_catalog_path: Path | None = None,
    ) -> None:
        self.project_path = project_path
        self.headless_port = headless_port
        self.runner_port = runner_port
        self.composer_port = composer_port
        self.frontend_port = frontend_port
        self.artifact_root = artifact_root or WORKSPACE / "examples"
        self.ros2_control = ros2_control
        self.bootstrap_project_path = bootstrap_project_path
        self.operator_install_root = operator_install_root
        self.operator_cell_id = operator_cell_id
        self.operator_admission = canonical_empty_admission_filename
        self.composer_static_dir = composer_static_dir
        self.composer_bind_host = composer_bind_host
        self.simulation_control_config = simulation_control_config
        self.site_settings = site_settings
        self.package_aggregate_digest = package_aggregate_digest
        self.operator_instance_id = operator_instance_id
        self.node_catalog_path = node_catalog_path or NODE_CATALOG
        self.alarm_catalog_path = alarm_catalog_path or ALARM_CATALOG
        self.scenario_id: str | None = None
        self.manager: subprocess.Popen[bytes] | None = None
        # Dev mode only: Vite HMR, this launcher's own child (not listed).
        self._vite_command: tuple[str, ...] | None = None
        self.vite: subprocess.Popen[bytes] | None = None
        self._ready_reported = False
        self.stop_requested = False
        # SIGTERM/SIGINT received; each one after the first is passed on to
        # the Manager, where a second stop request forces its stop (OD-187).
        self.stop_requests = 0
        self.session: Session | None = None
        for variable, member in REMOVED_DISK_VARIABLES.items():
            if variable in os.environ:
                raise WorkbenchError(
                    f"{variable} is no longer read: set {member} in the site settings file "
                    "(MANYFORGE_DEV_SITE_SETTINGS) and unset the variable")
        # The disk limits: the site settings file's, or the defaults.
        self.disk_limits = run_session.read_site_disk_limits(site_settings)
        self.runtime_root = STATE_ROOT / "current"

    def _paths(self) -> dict[str, Path]:
        paths = {
            "manager_state": self.runtime_root / "manager",
            "manager_run": self.runtime_root / "manager-run",
            "execution_state": self.runtime_root / "execution-state",
            "execution_run": self.runtime_root / "execution-run",
            "artifacts": self.runtime_root / "artifacts",
            "composer_authoring": self.runtime_root / "composer-authoring",
        }
        for path in paths.values():
            path.mkdir(parents=True, exist_ok=True)
        if self.ros2_control:
            # One runtime-wide ROS home preserves controller-manager spawner
            # lock coordination across controllers, replacements and epochs.
            # Controller child logs still use their existing artifact root.
            ros_home = paths["execution_run"] / "ros-home"
            ros_home.mkdir(parents=True, exist_ok=True)
            os.environ["ROS_HOME"] = str(ros_home)
        return paths

    def _composer_command(self, base_url: str) -> list[str]:
        command = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            self.composer_bind_host,
            "--port",
            str(self.composer_port),
            "--headless-api-url",
            base_url,
            "--headless-api-timeout-s",
            "20",
        ]
        if self.composer_static_dir is not None:
            command.extend(("--static-dir", str(self.composer_static_dir)))
        if self.project_path is not None:
            command.extend(("--startup-scenario-document", self.project_path))
        return command

    def _runtime_targets(self) -> tuple[Path, ...]:
        return _current_ros_backend(_ros_backend_environment())

    def _simulation_control_arguments(self) -> tuple[str, ...]:
        if not self.ros2_control:
            return ()
        config = self.simulation_control_config or WORKSPACE / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"
        _regular_file(config, "Isaac Program simulation site configuration")
        return ("--simulation-control-config", str(config))

    def _site_manager_arguments(self) -> tuple[str, ...]:
        # Machine facts only (manyforge.site.v2); the Manager validates the file.
        if self.site_settings is None:
            return ()
        _regular_file(self.site_settings, "site settings")
        return ("--site-settings", str(self.site_settings))
    def start(self) -> None:
        if not self.ros2_control:
            raise WorkbenchError("workbench requires ROS control; native execution is unavailable")
        if self.bootstrap_project_path is not None and (
            not self.ros2_control or self.project_path is not None
        ):
            raise WorkbenchError("--bootstrap-project requires ROS control and excludes --project")
        operator_mode = self.operator_install_root is not None
        if operator_mode:
            if not self.ros2_control or self.project_path or self.bootstrap_project_path:
                raise WorkbenchError("operator install requires ROS and excludes project auto-Load")
            if not self.operator_cell_id or not self.operator_admission:
                raise WorkbenchError("operator install requires its original cell/admission identity")
            if Path(self.operator_admission).name != self.operator_admission:
                raise WorkbenchError("operator admission must be package-relative")
            from manyforge_operator.package_aggregate import package_aggregate_sha256
            if package_aggregate_sha256(self.operator_install_root) != self.package_aggregate_digest:
                raise WorkbenchError("qualified operator package aggregate mismatch")
            _regular_file(self.operator_install_root / self.operator_admission, "qualified operator admission")
            if self.composer_static_dir is not None:
                _regular_file(self.composer_static_dir / "index.html", "built Composer frontend")
        ros_capture: dict[str, Any] | None = None
        if self.ros2_control and not operator_mode:
            intended_project = self.bootstrap_project_path or self.project_path
            if intended_project is not None:
                ros_capture, source_path = _capture_ros_bootstrap_project(
                    intended_project, allowed_root=self.artifact_root
                )
                if self.project_path is not None:
                    self.project_path = source_path
                    self.scenario_id = ros_capture["scenario_id"]
        for port in (
            self.headless_port,
            self.runner_port,
            self.composer_port,
            *((self.frontend_port,) if not operator_mode else ()),
        ):
            if not _port_available(port):
                raise WorkbenchError(
                    f"required development port {port} is already in use"
                )
        self.runtime_root.mkdir(parents=True, exist_ok=True)
        # Timing is diagnostic only; status always reads the actual owners.
        # The separate build step records the build time; serve never builds.
        timing = {"build_elapsed_seconds": _native_build_elapsed(),
                  "readiness_elapsed_seconds": None}
        timing_path = self.runtime_root / "workbench-startup-timing.json"
        timing_path.write_text(_json(timing) + "\n", encoding="utf-8")
        current_headless_extension = _current_backend()
        current_runtime_targets = self._runtime_targets()
        activation_preparer = _current_activation_preparer()
        _current_core_bindings()
        bootstrap_root: Path
        bootstrap_admission = self.operator_admission or BOOTSTRAP_ADMISSION
        cell_id = self.operator_cell_id or WORKBENCH_CELL_ID
        bootstrap_root = self.operator_install_root or (
            _ensure_ros_bootstrap_package(ros_capture, activation_preparer=activation_preparer)
            if ros_capture is not None else _ensure_repository_bootstrap_package()
        )
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
            f"{HEADLESS_BUILD}:{CORE_PYTHON_BUILD}:{os.environ.get('PYTHONPATH', '')}"
        )
        if not operator_mode:
            _ensure_frontend_dependencies()
        paths = self._paths()
        readiness_started = time.monotonic()
        installed = self.runtime_root / "installed-targets"
        installed.mkdir(parents=True, exist_ok=True)
        manager = installed / "manyforge_manager_daemon"
        shutil.copy2(MANAGER_BINARY, manager)
        _regular_file(manager, "installed Manager", executable=True)
        _stage_runtime_targets(current_runtime_targets, installed)
        manager_socket = paths["manager_run"] / "manager-command.sock"
        runner_socket = paths["manager_run"] / "program-runner-liveness.sock"
        _remove_stale_socket(manager_socket, "Manager command socket")
        _remove_stale_socket(runner_socket, "Program runner liveness socket")
        if ros_capture is not None:
            current_capture, _ = _capture_ros_bootstrap_project(
                source_path, allowed_root=self.artifact_root
            )
            if current_capture != ros_capture:
                raise WorkbenchError(
                    "intended first ROS Scenario changed during startup preparation; "
                    "no runtime was started"
                )
        if self.ros2_control:
            # One actual workbench session owns host logs. Controller children
            # retain their per-artifact override; ROS_HOME remains runtime-wide.
            # Session-local lock, distinct from the installation root flock
            # held by this launcher for its entire owner lifetime.
            ros_logs = create_host_ros_log_session(self.runtime_root)
            os.environ["ROS_LOG_DIR"] = str(ros_logs)
        base_url = f"http://127.0.0.1:{self.headless_port}"
        history_directory = STATE_ROOT / "history"
        history_directory.mkdir(parents=True, exist_ok=True)
        composer_environment = {
            "MANYFORGE_CORS_ORIGINS": (
                f"http://localhost:{self.frontend_port},"
                f"http://127.0.0.1:{self.frontend_port}"
            ),
        }
        composer_roots = _composer_filesystem_roots(self.artifact_root)
        composer_environment["MANYFORGE_FS_ROOTS"] = composer_roots
        composer_environment["MANYFORGE_PROGRAM_WRITE_ROOTS"] = composer_roots
        composer_environment["MANYFORGE_DEPLOYMENT_WRITE_ROOTS"] = composer_roots
        # The Manager starts, watches and stops every application process
        # (spec 3.3, S3); none depends on another being up first. Composer
        # starts after the Manager's first converged baseline, so a --project
        # Load meets a ready Manager (4.7). Composer owns only process-lifetime
        # Draft state; its installation settings persist under the inherited
        # MANYFORGE_STATE_DIR (composer/).
        processes = [
            ApplicationProcess("headless", (
                sys.executable,
                "-m",
                "manyforge_headless_api",
                "serve",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.headless_port),
                "--install-root",
                str(bootstrap_root),
                "--canonical-empty-admission-filename",
                str(bootstrap_admission),
                "--artifact-root",
                str(paths["artifacts"]),
                "--node-catalog-path",
                str(self.node_catalog_path),
                "--node-environment-manifest-path",
                str(NODE_ENVIRONMENT_MANIFEST),
                "--alarm-catalog-path",
                str(self.alarm_catalog_path),
                "--historian-directory",
                str(history_directory),
                *self._previous_session_arguments(),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--disk-free-floor-bytes",
                str(self.disk_limits.floor_bytes),
                "--disk-free-margin-bytes",
                str(run_session.DISK_FLOOR_CLEAR_MARGIN_BYTES),
                "--cell-id",
                cell_id,
                "--freshness-ms",
                "2000",
                "--manager-timeout-s",
                "15",
                "--program-runner-url",
                f"http://127.0.0.1:{self.runner_port}",
                "--activation-preparer",
                str(activation_preparer),
                "--activation-preparer-capability-root",
                str(WORKSPACE),
                "--activation-preparer-activation-root",
                str(paths["artifacts"]),
            )),
            ApplicationProcess("program_runner", (
                sys.executable,
                "-m",
                "manyforge_program_runner",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.runner_port),
                "--headless-url",
                base_url,
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cycle-journal-path",
                str(self.runtime_root / "program-cycle.sqlite3"),
                "--manager-liveness-socket",
                str(runner_socket),
                "--cell-id",
                cell_id,
                "--persistent-key-register-directory",
                str(_persistent_key_register_directory()),
                "--snapshot-url",
                f"http://127.0.0.1:{self.composer_port}/api/scene/snapshot/render",
                *self._simulation_control_arguments(),
            )),
            ApplicationProcess(
                "composer",
                tuple(self._composer_command(base_url)),
                composer_environment,
                START_AFTER_BASELINE,
            ),
        ]
        if not operator_mode:
            # Vite is development tooling, not an application process: the
            # list holds only the three roles of contract 1.1. This launcher
            # starts it once Composer is ready and keeps it as its own child.
            node = shutil.which("node")
            if node is None:
                raise WorkbenchError("Vite HMR requires node on PATH")
            self._vite_command = (
                node,
                str(FRONTEND_ROOT / "node_modules/vite/bin/vite.js"),
                "--host",
                "0.0.0.0",
                "--port",
                str(self.frontend_port),
            )
        process_list = write_application_processes(
            self.runtime_root / "application-processes.json", processes
        )
        self._spawn_manager(
            (
                str(manager),
                "--install-root",
                str(bootstrap_root),
                "--activation-root",
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
                str(self.runtime_root / "logical-scene.sqlite3"),
                "--canonical-empty-admission-filename",
                bootstrap_admission,
                "--program-runner-liveness-socket",
                str(runner_socket),
                "--application-processes",
                str(process_list),
                # Development value only: relative Program scene meshes such
                # as examples/<name>/meshes/*.obj resolve below the checkout.
                "--scene-asset-root",
                str(WORKSPACE),
            ) + self._site_manager_arguments(),
            # The execution host and its component processes inherit them: a
            # point-cloud source sends its clouds to this Composer; the
            # assistant's bridge reads Headless and calls back into Composer.
            environment={
                "MANYFORGE_COMPOSER_URL": f"http://127.0.0.1:{self.composer_port}",
                "MANYFORGE_HEADLESS_API_URL": base_url,
            },
        )
        self._operator_mode = operator_mode
        self._timing = timing
        self._timing_path = timing_path
        self._readiness_started = readiness_started

    def _start_vite_when_composer_ready(self) -> None:
        """Start Vite HMR once, after Composer answers (dev mode only)."""

        if self._vite_command is None or self.vite is not None:
            return
        try:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.composer_port}", timeout_seconds=0.5
            ) as client:
                if not client.health():
                    return
        except (HeadlessClientError, OSError, ValueError):
            return
        print("starting Vite HMR", flush=True)
        self.vite = subprocess.Popen(
            self._vite_command,
            cwd=FRONTEND_ROOT,
            env={**os.environ,
                 "MANYFORGE_COMPOSER_PROXY_TARGET": f"http://127.0.0.1:{self.composer_port}"},
            preexec_fn=parent_death_preexec(),
        )

    def _report_ready(self) -> None:
        """Print the ready line once the owners report ready.

        Reporting only: it never decides anything about the application and
        never stops a process; the Manager supervises them (spec 3.3).
        """

        if self._ready_reported:
            return
        try:
            if self._operator_mode:
                if not query_operator_status(headless_port=self.headless_port,
                                             runner_port=self.runner_port,
                                             composer_port=self.composer_port)["ready"]:
                    return
                self._timing["readiness_elapsed_seconds"] = time.monotonic() - self._readiness_started
                self._timing_path.write_text(_json(self._timing) + "\n", encoding="utf-8")
                print(_json({"ready": True, "url": f"http://localhost:{self.composer_port}",
                             "timing": self._timing}), flush=True)
            else:
                if self.vite is None or not query_status(
                        scenario_id=self.scenario_id, composer_port=self.composer_port)["ready"]:
                    return
                with socket.create_connection(("127.0.0.1", self.frontend_port), timeout=0.5):
                    pass
                print(
                    _json(
                        {
                            "ready": True,
                            "url": f"http://localhost:{self.frontend_port}",
                            "scenario_id": self.scenario_id,
                            "project_path": self.project_path,
                            "classification": (
                                "development workbench; repository source; "
                                "simulated execution only"
                            ),
                        }
                    ),
                    flush=True,
                )
        except (HeadlessClientError, OSError, ValueError, WorkbenchError):
            return
        self._ready_reported = True

    def _previous_session_arguments(self) -> tuple[str, ...]:
        if self.session is None:
            return ()
        arguments = ("--workbench-previous-session-reports-root", str(STATE_ROOT / "session-reports"))
        previous = self.session.marker.get("previous_session")
        return arguments + (("--workbench-previous-session-id", previous["session_id"]) if previous else ())

    def _spawn_manager(self, command: Sequence[str], *, environment: dict[str, str]) -> None:
        """Start the Manager, the one application supervisor; it dies with us."""

        print(f"starting Manager: {' '.join(command)}", flush=True)
        self.manager = subprocess.Popen(
            command,
            env={**os.environ, **environment},
            preexec_fn=parent_death_preexec(),
        )

    def _stop_manager(self) -> bool:
        """Ask the Manager to end the application; True when it stopped cleanly.

        The Manager halts the robots, stops Composer, the Program runner and
        Headless in reverse list order, then the execution host, then exits
        (spec 3.3 point 5). The wait for its exit has no time limit (OD-187):
        the request stands for the first stop request this workbench got;
        each later SIGTERM or SIGINT is passed on as one SIGTERM, and a second
        stop request forces the Manager's stop. The container's stop limit
        stays the outer end. A status other than "stopped" (a forced or
        incomplete stop) is not a clean stop.
        """

        manager = self.manager
        if manager is None:
            return False
        if manager.poll() is None:
            passed_on = 1 if self.stop_requests else 0
            request_manager_shutdown(
                self.runtime_root / "manager-run/manager-command.sock", timeout_seconds=1.0
            )
            while True:
                while passed_on < self.stop_requests:
                    manager.send_signal(signal.SIGTERM)
                    passed_on += 1
                try:
                    manager.wait(timeout=MANAGER_STOP_POLL_SECONDS)
                    break
                except subprocess.TimeoutExpired:
                    continue
        return manager.returncode == MANAGER_STOPPED

    def _stop_vite(self) -> bool:
        vite = self.vite
        if vite is None or vite.poll() is not None:
            return vite is None
        vite.terminate()
        try:
            vite.wait(timeout=5)
            return True
        except subprocess.TimeoutExpired:
            vite.kill()
            vite.wait(timeout=5)
            return False

    def close(self) -> None:
        verified = (self.stop_requested and self.manager is not None and self.manager.poll() is None
                    and (self.vite is None or self.vite.poll() is None))
        # The Manager first: its stop begins with the robots' Halt, which
        # waits for nothing else (OD-187); Vite serves only the frontend.
        verified = self._stop_manager() and verified
        verified = self._stop_vite() and verified
        if self.session is not None:
            try:
                self.session.stopped(verified=verified)
            finally:
                self.session.close()
                self.session = None

    def serve(self) -> None:
        def request_stop(_signum: int, _frame: Any) -> None:
            self.stop_requested = True
            self.stop_requests += 1

        signal.signal(signal.SIGTERM, request_stop)
        signal.signal(signal.SIGINT, request_stop)
        try:
            STATE_ROOT.mkdir(parents=True, exist_ok=True)
            selected = os.environ.get("MANYFORGE_DEV_COLD_SESSION_ID")
            self.session = Session(STATE_ROOT, selected) if selected else Session.direct(STATE_ROOT, self.disk_limits)
            self.start()
            next_prune = time.monotonic() + run_session.DISK_PRUNE_PERIOD_SECONDS
            # Wait on the Manager for its whole life. It ends the application
            # when any application process exits and has printed which one.
            while not self.stop_requested:
                try:
                    status = self.manager.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    # A Vite exit ends the session as before: close() stops
                    # the Manager, which ends the application.
                    if self.vite is not None and self.vite.poll() is not None:
                        raise WorkbenchError(f"Vite HMR exited with status {self.vite.returncode}")
                    self._start_vite_when_composer_ready()
                    self._report_ready()
                    # ManyForge's own obsolete files, only above their limits.
                    if time.monotonic() >= next_prune:
                        next_prune = time.monotonic() + run_session.DISK_PRUNE_PERIOD_SECONDS
                        run_session.prune_while_running(self.session.root, self.session.fd, self.disk_limits,
                                                        legacy_entries=_LEGACY_SESSION_ENTRIES)
                    continue
                if status != MANAGER_STOPPED:
                    raise WorkbenchError(f"Manager ended the application with status {status}")
                return
        finally:
            self.close()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="dev-workbench.py")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("build", help="build the native artifacts serve requires")
    for name in ("serve", "status"):
        command = subcommands.add_parser(name)
        projects = command.add_mutually_exclusive_group()
        projects.add_argument("--project")
        command.add_argument("--headless-port", type=int, default=9010)
        command.add_argument("--runner-port", type=int, default=9011)
        command.add_argument("--composer-port", type=int, default=9020)
        command.add_argument("--frontend-port", type=int, default=3000)
        command.add_argument("--operator-runtime", action="store_true")
        if name == "serve":
            projects.add_argument(
                "--bootstrap-project",
                help="select the first ROS Scenario for empty startup, without loading it",
            )
            command.add_argument("--operator-install-root", type=Path)
            command.add_argument("--operator-cell-id")
            command.add_argument("--operator-instance-id")
            command.add_argument("--node-catalog-path", type=Path)
            command.add_argument("--alarm-catalog-path", type=Path)
            command.add_argument("--canonical-empty-admission-filename")
            command.add_argument("--composer-static-dir", type=Path)
            command.add_argument("--composer-bind-host", default="127.0.0.1")
            command.add_argument("--simulation-control-config", type=Path)
            command.add_argument("--site-settings", type=Path)
            command.add_argument("--package-aggregate-sha256")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "build":
            build_native()
            return 0
        if args.command == "status" and args.operator_runtime:
            result = query_operator_status(headless_port=args.headless_port,
                                           runner_port=args.runner_port,
                                           composer_port=args.composer_port)
            print(_json(result))
            return 0 if result["ready"] else 69
        if args.command == "status":
            scenario_id = None
            if args.project is not None:
                try:
                    scenario_id = _project_scenario_id(
                        args.project,
                        allowed_root=WORKSPACE / "examples",
                    )
                except (OSError, ValueError) as error:
                    raise WorkbenchError(str(error)) from error
            result = query_status(
                scenario_id=scenario_id,
                composer_port=args.composer_port,
            )
            print(_json(result))
            return 0 if result["ready"] else 69
        if args.operator_runtime != (args.operator_install_root is not None):
            raise WorkbenchError("operator runtime requires its qualified install root and explicit startup mode")
        Workbench(
            project_path=args.project,
            headless_port=args.headless_port,
            runner_port=args.runner_port,
            composer_port=args.composer_port,
            frontend_port=args.frontend_port,
            bootstrap_project_path=args.bootstrap_project,
            operator_install_root=args.operator_install_root,
            operator_cell_id=args.operator_cell_id,
            canonical_empty_admission_filename=args.canonical_empty_admission_filename,
            composer_static_dir=args.composer_static_dir,
            composer_bind_host=args.composer_bind_host,
            simulation_control_config=args.simulation_control_config,
            site_settings=args.site_settings,
            package_aggregate_digest=args.package_aggregate_sha256,
            operator_instance_id=args.operator_instance_id,
            node_catalog_path=args.node_catalog_path,
            alarm_catalog_path=args.alarm_catalog_path,
        ).serve()
        return 0
    except run_session.DiskBelowFloor as error:
        print(f"{error.code}: {error}", file=sys.stderr)
        return 69
    except (HeadlessClientError, OSError, ValueError, WorkbenchError) as error:
        print(f"development_workbench_unavailable: {error}", file=sys.stderr)
        return 69


if __name__ == "__main__":
    raise SystemExit(main())
