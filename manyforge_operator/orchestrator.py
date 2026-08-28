"""Native O1.3 operator-launch orchestration.

This module is deliberately a mechanical boundary around the already
qualified Manager, Headless API, Program runner, and Composer processes.  It
does not own graph, Apply, execution, scene, alarm, or health truth.  The
native profile is the only live profile in O1.3; Isaac is kept behind the
explicit unavailable gate until its own dependency-ordered slice.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass, field
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import time
from types import SimpleNamespace
from typing import Any, BinaryIO, Iterator, Mapping, Sequence
from urllib.error import URLError
from urllib.request import Request, urlopen

from .contracts import (
    ArtifactKind,
    CONFIGURATION_SCHEMA,
    FailureRecord,
    HostIdentity,
    HostRole,
    LaunchConfiguration,
    LaunchInstanceState,
    LaunchInstanceStatus,
    LaunchProfile,
    ReadinessAuthority,
    ReadinessObservation,
    ReadinessState,
    ResourceKind,
    ResourceObservation,
    ResourcePresence,
    RuntimeArtifactIdentity,
    SERVICE_BIND_HOSTS,
    SERVICE_PORTS,
    ServiceEndpoint,
    ServiceKind,
    SourceIdentityV1,
    StartDisposition,
    can_transition,
    configuration_digest,
    decide_start,
)
from .package_aggregate import (
    PackageAggregateUnavailable,
    package_aggregate_sha256,
)
from .cell_layout import (
    CellInstanceLayout,
    CellLayoutError,
    portable_path_identity,
)


STATUS_ENVELOPE_SCHEMA = "manyforge.native_operator_instance.v1"
STATUS_RECORD_MAX_BYTES = 4 * 1024 * 1024
DEFAULT_STATE_ROOT = Path(".manyforge-var/operator")
DEFAULT_TIMEOUT_SECONDS = 60.0
DEFAULT_POLL_SECONDS = 0.25
_PROCESS_COMPONENTS = ("manager", "headless", "program_runner", "composer")


def _safe_unix_timestamp() -> int:
    """Return a current, JSON-safe epoch tick for the frozen timestamp fields.

    The O1.2 contract retains the ``*_unix_ns`` field names but bounds them
    to JavaScript's safe integer range.  Current epoch nanoseconds exceed that
    bound, so the launcher records epoch microseconds as the finest safe tick.
    """

    return max(1, time.time_ns() // 1_000)


class LauncherError(RuntimeError):
    """Bounded operator error with a stable code and CLI exit status."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        exit_code: int = 70,
        status: LaunchInstanceStatus | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.exit_code = exit_code
        self.status = status


@dataclass(frozen=True, slots=True)
class NativeLauncherConfig:
    """Explicit paths and source-matched inputs for one native launch."""

    source_root: Path
    manager_executable: Path
    install_root: Path
    node_catalog_path: Path
    alarm_catalog_path: Path
    composer_static_dir: Path | None = None
    simulation_control_config: Path | None = None
    canonical_empty_admission_filename: str = "canonical-empty-admission.json"
    cell_id: str = "native"
    state_root: Path = DEFAULT_STATE_ROOT
    advertised_host: str = "localhost"
    instance_id: str = "native"
    package_aggregate_sha256: str = ""
    compiler_build_manifest_sha256: str = ""
    node_environment_sha256: str = ""
    expected_source_fingerprint: str | None = None
    expected_head_commit: str | None = None
    expected_head_tree: str | None = None
    readiness_timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS
    poll_interval_seconds: float = DEFAULT_POLL_SECONDS
    reconciliation_period_ms: int = 500
    manager_timeout_seconds: float = 60.0

    def __post_init__(self) -> None:
        path_fields = (
            "source_root",
            "manager_executable",
            "install_root",
            "node_catalog_path",
            "alarm_catalog_path",
            "state_root",
        )
        for name in path_fields:
            value = Path(getattr(self, name))
            if not value.is_absolute():
                raise ValueError(f"{name} must be an absolute path")
            object.__setattr__(self, name, value)
        if not self.manager_executable.is_file() or not os.access(
            self.manager_executable, os.X_OK
        ):
            raise ValueError("manager_executable must be an executable file")
        for name in (
            "source_root",
            "node_catalog_path",
            "alarm_catalog_path",
        ):
            value = Path(getattr(self, name))
            if not value.exists():
                raise ValueError(f"{name} does not exist: {value}")
        if self.composer_static_dir is not None:
            static_dir = Path(self.composer_static_dir)
            if not static_dir.is_absolute():
                raise ValueError("composer_static_dir must be an absolute path")
            if not static_dir.is_dir() or not (static_dir / "index.html").is_file():
                raise ValueError(
                    "composer_static_dir must be a directory containing index.html"
                )
            object.__setattr__(self, "composer_static_dir", static_dir)
        if self.simulation_control_config is not None:
            simulation_config = Path(self.simulation_control_config)
            if not simulation_config.is_absolute() or not simulation_config.is_file():
                raise ValueError(
                    "simulation_control_config must be an existing absolute file"
                )
            object.__setattr__(self, "simulation_control_config", simulation_config)
        if (
            not self.canonical_empty_admission_filename
            or Path(self.canonical_empty_admission_filename).is_absolute()
            or Path(self.canonical_empty_admission_filename).name
            != self.canonical_empty_admission_filename
            or self.canonical_empty_admission_filename in {".", ".."}
            or "/" in self.canonical_empty_admission_filename
            or "\\" in self.canonical_empty_admission_filename
        ):
            raise ValueError(
                "canonical_empty_admission_filename must be one package-relative filename"
            )
        if not self.advertised_host or any(
            character.isspace() for character in self.advertised_host
        ):
            raise ValueError("advertised_host must be one bounded host reference")
        if not portable_path_identity(self.instance_id):
            raise ValueError("instance_id must be one path-safe non-empty identifier")
        for name, root in (
            ("source_root", str(self.source_root)),
            ("examples_root", str(self.source_root / "examples")),
            ("install_root", str(self.install_root)),
            ("program_workspace", str(self.program_workspace)),
        ):
            if "\x00" in root or "," in root or root != root.strip():
                raise ValueError(
                    f"{name} must be unambiguous in MANYFORGE_FS_ROOTS"
                )
        examples_root = self.source_root / "examples"
        try:
            resolved_examples_root = examples_root.resolve(strict=True)
        except (OSError, RuntimeError) as error:
            raise ValueError(
                "examples_root must resolve to an existing directory"
            ) from error
        if not resolved_examples_root.is_dir():
            raise ValueError("examples_root must resolve to a directory")
        try:
            resolved_package_root = self.install_root.resolve(strict=False)
            resolved_program_workspace = self.program_workspace.resolve(strict=False)
        except (OSError, RuntimeError) as error:
            raise ValueError(
                "install_root and program_workspace must be resolvable"
            ) from error
        if (
            resolved_package_root == resolved_program_workspace
            or resolved_package_root in resolved_program_workspace.parents
            or resolved_program_workspace in resolved_package_root.parents
        ):
            raise ValueError("install_root and program_workspace must be disjoint")
        if not portable_path_identity(self.cell_id):
            raise ValueError("cell_id must be one path-safe non-empty identifier")
        for name in (
            "package_aggregate_sha256",
            "compiler_build_manifest_sha256",
            "node_environment_sha256",
        ):
            value = getattr(self, name)
            if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
                raise ValueError(f"{name} must be one full lowercase SHA-256")
        if self.expected_source_fingerprint is not None and (
            len(self.expected_source_fingerprint) != 64
            or any(c not in "0123456789abcdef" for c in self.expected_source_fingerprint)
        ):
            raise ValueError("expected_source_fingerprint must be a lowercase SHA-256")
        if not 0 < self.readiness_timeout_seconds <= 300:
            raise ValueError("readiness_timeout_seconds must be in (0, 300]")
        if not 0.01 <= self.poll_interval_seconds <= 5:
            raise ValueError("poll_interval_seconds must be in [0.01, 5]")
        if not 10 <= self.reconciliation_period_ms <= 60_000:
            raise ValueError("reconciliation_period_ms must be in [10, 60000]")
        if not 0 < self.manager_timeout_seconds <= 60.0:
            raise ValueError("manager_timeout_seconds must be in (0, 60]")

    @property
    def instance_root(self) -> Path:
        return self.state_root / self.instance_id

    @property
    def program_workspace(self) -> Path:
        return self.authoring_state_root / "program-workspace"

    @property
    def program_cycle_journal(self) -> Path:
        return self.program_runner_state_root / "program-cycle.sqlite3"

    @property
    def artifact_root(self) -> Path:
        return self.instance_root / "artifacts"

    @property
    def projection_root(self) -> Path:
        return self.instance_root / "projections"

    @property
    def manager_state_root(self) -> Path:
        return self.instance_root / "state" / "manager"

    @property
    def manager_run_root(self) -> Path:
        return self.instance_root / "run" / "manager"

    @property
    def execution_state_root(self) -> Path:
        return self.instance_root / "state" / "execution-host"

    @property
    def execution_run_root(self) -> Path:
        return self.instance_root / "run" / "execution-host"

    @property
    def program_runner_state_root(self) -> Path:
        return self.instance_root / "state" / "program-runner"

    @property
    def program_selection_state_root(self) -> Path:
        return self.instance_root / "state" / "program-selection"

    @property
    def logical_scene_database(self) -> Path:
        return self.instance_root / "state" / "logical-scene" / "scene.sqlite3"

    @property
    def historian_database_path(self) -> Path:
        return self.instance_root / "state" / "alarm-history" / "history.sqlite3"

    @property
    def authoring_state_root(self) -> Path:
        return self.instance_root / "state" / "composer-authoring"

    @property
    def authoring_transaction_journal(self) -> Path:
        return self.authoring_state_root / "composer-authoring-transactions.sqlite3"

    @property
    def headless_url(self) -> str:
        return f"http://127.0.0.1:{SERVICE_PORTS['headless']}/"

    @property
    def program_runner_url(self) -> str:
        return f"http://127.0.0.1:{SERVICE_PORTS['program_runner']}/"

    @property
    def composer_url(self) -> str:
        return f"http://{self.advertised_host}:{SERVICE_PORTS['composer']}/"


@dataclass(frozen=True, slots=True)
class ProcessSpec:
    component_id: str
    argv: tuple[str, ...]
    environment: Mapping[str, str]
    cwd: Path


@dataclass(slots=True)
class _ManagedProcess:
    component_id: str
    process: subprocess.Popen[bytes]
    argv: tuple[str, ...]
    log_path: Path
    pid: int
    start_ticks: int
    boot_id: str
    process_group_id: int


def native_commands(config: NativeLauncherConfig) -> tuple[ProcessSpec, ...]:
    """Return the exact four native process specifications, without I/O."""

    package_env = {"MANYFORGE_FS_ROOTS": str(config.install_root)}
    program_runner_liveness_socket = (
        config.manager_run_root / "program-runner-liveness.sock"
    )
    manager = ProcessSpec(
        "manager",
        tuple(
            str(item)
            for item in (
                config.manager_executable,
                "--install-root",
                config.install_root,
                "--canonical-empty-admission-filename",
                config.canonical_empty_admission_filename,
                "--manager-state-root",
                config.manager_state_root,
                "--manager-run-root",
                config.manager_run_root,
                "--execution-state-root",
                config.execution_state_root,
                "--execution-run-root",
                config.execution_run_root,
                "--logical-scene-database",
                config.logical_scene_database,
                "--program-cycle-journal",
                config.program_cycle_journal,
                "--authoring-transaction-journal",
                config.authoring_transaction_journal,
                "--projection-root",
                config.projection_root,
                "--program-runner-liveness-socket",
                program_runner_liveness_socket,
                "--reconciliation-period-ms",
                config.reconciliation_period_ms,
            )
        ),
        package_env,
        config.source_root,
    )
    headless = ProcessSpec(
        "headless",
        (
            sys.executable,
            "-m",
            "manyforge_headless_api",
            "serve",
            "--host",
            SERVICE_BIND_HOSTS["headless"],
            "--port",
            str(SERVICE_PORTS["headless"]),
            "--projection-root",
            str(config.projection_root),
            "--install-root",
            str(config.install_root),
            "--artifact-root",
            str(config.artifact_root),
            "--program-selection-state-root",
            str(config.program_selection_state_root),
            "--node-catalog-path",
            str(config.node_catalog_path),
            "--alarm-catalog-path",
            str(config.alarm_catalog_path),
            "--historian-database-path",
            str(config.historian_database_path),
            "--manager-run-root",
            str(config.manager_run_root),
            "--execution-run-root",
            str(config.execution_run_root),
            "--cell-id",
            config.cell_id,
            "--owner-id",
            f"headless-api.{config.instance_id}",
            "--freshness-ms",
            "2000",
            "--manager-timeout-s",
            str(config.manager_timeout_seconds),
            "--program-runner-url",
            config.program_runner_url,
        ),
        package_env,
        config.source_root,
    )
    program_argv = [
            sys.executable,
            "-m",
            "manyforge_program_runner",
            "--host",
            SERVICE_BIND_HOSTS["program_runner"],
            "--port",
            str(SERVICE_PORTS["program_runner"]),
            "--headless-url",
            config.headless_url,
            "--owner-id",
            f"program-runner.{config.instance_id}",
            "--headless-timeout-s",
            "30",
            "--execution-run-root",
            str(config.execution_run_root),
            "--cycle-journal-path",
            str(config.program_cycle_journal),
            "--manager-liveness-socket",
            str(program_runner_liveness_socket),
            "--cell-id",
            config.cell_id,
    ]
    if config.simulation_control_config is not None:
        program_argv.extend(
            ("--simulation-control-config", str(config.simulation_control_config))
        )
    program_runner = ProcessSpec(
        "program_runner",
        tuple(program_argv),
        package_env,
        config.source_root,
    )
    composer_argv = (
        sys.executable,
        "-m",
        "manyforge_composer",
        "--host",
        SERVICE_BIND_HOSTS["composer"],
        "--port",
        str(SERVICE_PORTS["composer"]),
        "--headless-api-url",
        config.headless_url,
        "--headless-api-timeout-s",
        "30",
        "--authoring-state-root",
        str(config.authoring_state_root),
    )
    if config.composer_static_dir is not None:
        composer_argv += ("--static-dir", str(config.composer_static_dir))
    composer = ProcessSpec(
        "composer",
        composer_argv,
        {
            "MANYFORGE_FS_ROOTS": (
                f"{config.source_root / 'examples'},"
                f"{config.install_root},{config.program_workspace}"
            ),
            "MANYFORGE_PROGRAM_WRITE_ROOTS": str(config.program_workspace),
            "MANYFORGE_DEPLOYMENT_WRITE_ROOTS": str(config.program_workspace),
        },
        config.source_root,
    )
    return (manager, headless, program_runner, composer)


def _boot_id() -> str:
    return Path("/proc/sys/kernel/random/boot_id").read_text(encoding="ascii").strip()


def _proc_observation(pid: int) -> tuple[int, str] | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except (FileNotFoundError, PermissionError, OSError):
        return None
    closing = text.rfind(")")
    if closing < 0:
        return None
    fields = text[closing + 2 :].split()
    if len(fields) < 20:
        return None
    try:
        return int(fields[19]), fields[0]
    except ValueError:
        return None


def _command_line(pid: int) -> tuple[str, ...] | None:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except (FileNotFoundError, PermissionError, OSError):
        return None
    return tuple(item.decode("utf-8", errors="replace") for item in raw.split(b"\0") if item)


def _process_is_authenticated(meta: Mapping[str, Any]) -> bool:
    pid = int(meta["pid"])
    observation = _proc_observation(pid)
    if observation is None or observation[0] != int(meta["start_ticks"]):
        return False
    if observation[1] in {"Z", "X"} or _boot_id() != meta["boot_id"]:
        return False
    try:
        if os.getpgid(pid) != int(meta["process_group_id"]):
            return False
    except (ProcessLookupError, PermissionError, OSError):
        return False
    expected = tuple(str(item) for item in meta["argv"])
    observed = _command_line(pid)
    return observed is not None and observed[: len(expected)] == expected


def _safe_process_is_authenticated(meta: Mapping[str, Any]) -> bool:
    """Treat an incomplete or unreadable mechanical row as unauthenticated.

    The row remains recorded and therefore visible to status; this helper only
    prevents malformed state from turning reconciliation into an unsafe broad
    kill or an unbounded exception.
    """

    try:
        return _process_is_authenticated(meta)
    except (KeyError, TypeError, ValueError, OSError):
        return False


def _sha256_json(value: Mapping[str, Any]) -> str:
    payload = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _launch_input_digest(config: NativeLauncherConfig) -> str:
    return _sha256_json(
        {
            "canonical_empty_admission_filename": config.canonical_empty_admission_filename,
            "cell_id": config.cell_id,
            "manager_timeout_seconds": config.manager_timeout_seconds,
            "simulation_control_config": (
                str(config.simulation_control_config)
                if config.simulation_control_config is not None
                else None
            ),
            "composer_static_dir": (
                str(config.composer_static_dir)
                if config.composer_static_dir is not None
                else None
            ),
        }
    )


def _source_identity(source_root: Path) -> SourceIdentityV1:
    script = source_root / "scripts/qualification/source_identity.py"
    if not script.is_file():
        raise LauncherError("source_identity_unavailable", f"missing source identity collector: {script}", exit_code=71)
    spec = importlib.util.spec_from_file_location("manyforge_operator_source_identity", script)
    if spec is None or spec.loader is None:
        raise LauncherError("source_identity_unavailable", "source identity collector cannot be loaded", exit_code=71)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        value = module.source_identity(source_root)
    except Exception as error:
        raise LauncherError("source_identity_unavailable", str(error), exit_code=71) from error
    try:
        identity = SourceIdentityV1.model_validate(value)
    except Exception as error:
        raise LauncherError("source_identity_invalid", str(error), exit_code=71) from error
    if identity.dirty:
        raise LauncherError("source_dirty", "native launch requires a clean source identity", exit_code=71)
    return identity


def _configuration(config: NativeLauncherConfig) -> LaunchConfiguration:
    source = _source_identity(config.source_root)
    if config.expected_source_fingerprint not in (None, source.fingerprint_sha256):
        raise LauncherError("source_identity_mismatch", "source fingerprint does not match the requested identity", exit_code=71)
    if config.expected_head_commit not in (None, source.head_commit):
        raise LauncherError("source_identity_mismatch", "source HEAD does not match the requested identity", exit_code=71)
    if config.expected_head_tree not in (None, source.head_tree):
        raise LauncherError("source_identity_mismatch", "source tree does not match the requested identity", exit_code=71)
    boot = _boot_id()
    host = HostIdentity(
        host_id="runtime",
        role=HostRole.RUNTIME,
        hostname_or_address=config.advertised_host,
        boot_id=boot,
    )
    artifact = RuntimeArtifactIdentity(
        host_id="runtime",
        kind=ArtifactKind.MOUNTED_INSTALL,
        source_fingerprint=source.fingerprint_sha256,
        package_aggregate_sha256=config.package_aggregate_sha256,
        compiler_build_manifest_sha256=config.compiler_build_manifest_sha256,
        node_environment_sha256=config.node_environment_sha256,
        install_root=str(config.install_root),
    )
    endpoints = [
        ServiceEndpoint(
            service=service,
            bind_host=SERVICE_BIND_HOSTS[service.value],
            port=SERVICE_PORTS[service.value],
            advertised_host=config.advertised_host,
            url=(
                config.composer_url
                if service is ServiceKind.COMPOSER
                else f"http://{config.advertised_host}:{SERVICE_PORTS[service.value]}/"
            ),
        )
        for service in ServiceKind
    ]
    payload = {
        "schema_version": CONFIGURATION_SCHEMA,
        "profile": LaunchProfile.NATIVE.value,
        "source": source.model_dump(mode="json"),
        "hosts": [host.model_dump(mode="json")],
        "artifacts": [artifact.model_dump(mode="json")],
        "endpoints": [endpoint.model_dump(mode="json") for endpoint in endpoints],
        "ros_topology": None,
    }
    return LaunchConfiguration(
        schema_version=CONFIGURATION_SCHEMA,
        profile=LaunchProfile.NATIVE,
        source=source,
        hosts=[host],
        artifacts=[artifact],
        endpoints=endpoints,
        ros_topology=None,
        configuration_digest=configuration_digest(payload),
    )


class _StateStore:
    def __init__(
        self,
        *,
        root: Path,
        lock_root: Path,
    ) -> None:
        self.root = Path(root)
        self.lock_root = Path(lock_root)
        identities: dict[str, tuple[int, int]] = {}
        for label, selected in (
            ("operator state root", self.root),
            ("operator run root", self.lock_root),
        ):
            identities[label] = self._directory_identity(selected, label=label)
        self._root_identity = identities["operator state root"]
        self._lock_root_identity = identities["operator run root"]
        self.record_path = self.root / "launch_instance.json"
        self.lock_path = self.lock_root / "instance.lock"

    @staticmethod
    def _directory_identity(path: Path, *, label: str) -> tuple[int, int]:
        try:
            before = path.lstat()
            canonical = path.resolve(strict=True)
            descriptor = os.open(
                path,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as error:
            raise LauncherError(
                "cell_layout_invalid",
                f"{label} is unavailable: {error}",
                exit_code=71,
            ) from error
        try:
            opened = os.fstat(descriptor)
            after = path.lstat()
            identity = (before.st_dev, before.st_ino)
            required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
            if (
                canonical != path
                or any(
                    not stat.S_ISDIR(metadata.st_mode)
                    or metadata.st_uid != os.geteuid()
                    or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or metadata.st_mode & required != required
                    or (metadata.st_dev, metadata.st_ino) != identity
                    for metadata in (before, opened, after)
                )
            ):
                raise LauncherError(
                    "cell_layout_invalid",
                    f"{label} is not one safe retained layout capability",
                    exit_code=71,
                )
            return identity
        except OSError as error:
            raise LauncherError(
                "cell_layout_invalid",
                f"{label} changed while opening: {error}",
                exit_code=71,
            ) from error
        finally:
            os.close(descriptor)

    @staticmethod
    @contextmanager
    def _open_directory(
        path: Path,
        *,
        expected_identity: tuple[int, int],
        label: str,
    ) -> Iterator[int]:
        observed = _StateStore._directory_identity(path, label=label)
        if observed != expected_identity:
            raise LauncherError(
                "cell_layout_invalid",
                f"{label} was replaced",
                exit_code=71,
            )
        try:
            descriptor = os.open(
                path,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except OSError as error:
            raise LauncherError(
                "cell_layout_invalid",
                f"{label} cannot be opened: {error}",
                exit_code=71,
            ) from error
        try:
            opened = os.fstat(descriptor)
            if (opened.st_dev, opened.st_ino) != expected_identity:
                raise LauncherError(
                    "cell_layout_invalid",
                    f"{label} changed while opening",
                    exit_code=71,
                )
            yield descriptor
            if _StateStore._directory_identity(path, label=label) != expected_identity:
                raise LauncherError(
                    "cell_layout_invalid",
                    f"{label} changed during use",
                    exit_code=71,
                )
        finally:
            os.close(descriptor)

    @staticmethod
    def _safe_file(
        metadata: os.stat_result,
        *,
        maximum_bytes: int | None = None,
    ) -> bool:
        return (
            stat.S_ISREG(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and metadata.st_nlink == 1
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
            == (stat.S_IRUSR | stat.S_IWUSR)
            and (maximum_bytes is None or metadata.st_size <= maximum_bytes)
        )

    @contextmanager
    def lock(self) -> Iterator[None]:
        with self._open_directory(
            self.lock_root,
            expected_identity=self._lock_root_identity,
            label="operator run root",
        ) as root_descriptor:
            try:
                descriptor = os.open(
                    "instance.lock",
                    os.O_RDWR
                    | os.O_CREAT
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    0o600,
                    dir_fd=root_descriptor,
                )
            except OSError as error:
                raise LauncherError(
                    "cell_layout_invalid",
                    f"operator lock cannot be opened safely: {error}",
                    exit_code=71,
            ) from error
            try:
                opened = os.fstat(descriptor)
                if not self._safe_file(opened):
                    raise LauncherError(
                        "cell_layout_invalid",
                        "operator lock is not one safe owner-held file",
                        exit_code=71,
                    )
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                current = os.stat(
                    "instance.lock",
                    dir_fd=root_descriptor,
                    follow_symlinks=False,
                )
                if (current.st_dev, current.st_ino) != (
                    opened.st_dev,
                    opened.st_ino,
                ) or not self._safe_file(current):
                    raise LauncherError(
                        "cell_layout_invalid",
                        "operator lock changed while acquiring it",
                        exit_code=71,
                    )
                try:
                    yield
                finally:
                    current = os.stat(
                        "instance.lock",
                        dir_fd=root_descriptor,
                        follow_symlinks=False,
                    )
                    if (current.st_dev, current.st_ino) != (
                        opened.st_dev,
                        opened.st_ino,
                    ) or not self._safe_file(current):
                        raise LauncherError(
                            "cell_layout_invalid",
                            "operator lock changed while held",
                            exit_code=71,
                        )
                    fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)

    def read(self) -> tuple[LaunchInstanceStatus, dict[str, Any]] | None:
        try:
            with self._open_directory(
                self.root,
                expected_identity=self._root_identity,
                label="operator state root",
            ) as root_descriptor:
                try:
                    before = os.stat(
                        "launch_instance.json",
                        dir_fd=root_descriptor,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    return None
                if not self._safe_file(
                    before,
                    maximum_bytes=STATUS_RECORD_MAX_BYTES,
                ):
                    raise ValueError(
                        "status record type, owner, mode, or size is unsafe"
                    )
                descriptor = os.open(
                    "launch_instance.json",
                    os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=root_descriptor,
                )
                try:
                    opened = os.fstat(descriptor)
                    if (
                        (opened.st_dev, opened.st_ino)
                        != (before.st_dev, before.st_ino)
                        or not self._safe_file(
                            opened,
                            maximum_bytes=STATUS_RECORD_MAX_BYTES,
                        )
                    ):
                        raise ValueError("status record changed while opening")
                    chunks: list[bytes] = []
                    total = 0
                    while True:
                        chunk = os.read(
                            descriptor,
                            min(64 * 1024, STATUS_RECORD_MAX_BYTES + 1 - total),
                        )
                        if not chunk:
                            break
                        chunks.append(chunk)
                        total += len(chunk)
                        if total > STATUS_RECORD_MAX_BYTES:
                            raise ValueError("status record is oversized")
                    after_file = os.fstat(descriptor)
                    after_path = os.stat(
                        "launch_instance.json",
                        dir_fd=root_descriptor,
                        follow_symlinks=False,
                    )
                    identity = (before.st_dev, before.st_ino)
                    if (
                        (after_file.st_dev, after_file.st_ino) != identity
                        or (after_path.st_dev, after_path.st_ino) != identity
                        or after_file.st_size != total
                        or after_path.st_size != total
                        or not self._safe_file(
                            after_path,
                            maximum_bytes=STATUS_RECORD_MAX_BYTES,
                        )
                    ):
                        raise ValueError("status record changed while reading")
                finally:
                    os.close(descriptor)
            envelope = json.loads(b"".join(chunks).decode("utf-8"))
            if envelope.get("schema_version") != STATUS_ENVELOPE_SCHEMA:
                raise ValueError("unexpected status envelope schema")
            status = LaunchInstanceStatus.model_validate_json(
                json.dumps(envelope["status"], ensure_ascii=False)
            )
            mechanical = envelope["mechanical"]
            if not isinstance(mechanical, dict):
                raise ValueError("mechanical status must be an object")
            return status, mechanical
        except LauncherError:
            raise
        except Exception as error:
            raise LauncherError("status_record_invalid", str(error), exit_code=70) from error

    def write(self, status: LaunchInstanceStatus, mechanical: Mapping[str, Any]) -> None:
        envelope = {
            "schema_version": STATUS_ENVELOPE_SCHEMA,
            "status": status.model_dump(mode="json"),
            "mechanical": dict(mechanical),
        }
        encoded = (
            json.dumps(
                envelope,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n"
        ).encode("utf-8")
        if len(encoded) > STATUS_RECORD_MAX_BYTES:
            raise LauncherError(
                "status_record_invalid",
                "status record exceeds its durable bound",
                exit_code=70,
            )
        temporary_name = f".launch_instance.{os.getpid()}.tmp"
        with self._open_directory(
            self.root,
            expected_identity=self._root_identity,
            label="operator state root",
        ) as root_descriptor:
            try:
                existing = os.stat(
                    "launch_instance.json",
                    dir_fd=root_descriptor,
                    follow_symlinks=False,
                )
            except FileNotFoundError:
                existing = None
            if existing is not None and not self._safe_file(
                existing,
                maximum_bytes=STATUS_RECORD_MAX_BYTES,
            ):
                raise LauncherError(
                    "status_record_invalid",
                    "existing status record is unsafe",
                    exit_code=70,
                )
            try:
                descriptor = os.open(
                    temporary_name,
                    os.O_WRONLY
                    | os.O_CREAT
                    | os.O_EXCL
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    0o600,
                    dir_fd=root_descriptor,
                )
            except OSError as error:
                raise LauncherError(
                    "status_record_invalid",
                    f"status record cannot be reserved: {error}",
                    exit_code=70,
                ) from error
            replaced = False
            try:
                os.fchmod(descriptor, 0o600)
                remaining = memoryview(encoded)
                while remaining:
                    written = os.write(descriptor, remaining)
                    if written <= 0:
                        raise LauncherError(
                            "status_record_invalid",
                            "status record write stalled",
                            exit_code=70,
                        )
                    remaining = remaining[written:]
                os.fsync(descriptor)
                written_metadata = os.fstat(descriptor)
                if (
                    not self._safe_file(
                        written_metadata,
                        maximum_bytes=STATUS_RECORD_MAX_BYTES,
                    )
                    or written_metadata.st_size != len(encoded)
                ):
                    raise LauncherError(
                        "status_record_invalid",
                        "new status record is unsafe",
                        exit_code=70,
                    )
                os.replace(
                    temporary_name,
                    "launch_instance.json",
                    src_dir_fd=root_descriptor,
                    dst_dir_fd=root_descriptor,
                )
                replaced = True
                current = os.stat(
                    "launch_instance.json",
                    dir_fd=root_descriptor,
                    follow_symlinks=False,
                )
                if (current.st_dev, current.st_ino) != (
                    written_metadata.st_dev,
                    written_metadata.st_ino,
                ) or not self._safe_file(
                    current,
                    maximum_bytes=STATUS_RECORD_MAX_BYTES,
                ):
                    raise LauncherError(
                        "status_record_invalid",
                        "status record changed during publication",
                        exit_code=70,
                    )
                os.fsync(root_descriptor)
            finally:
                os.close(descriptor)
                if not replaced:
                    try:
                        os.unlink(temporary_name, dir_fd=root_descriptor)
                    except FileNotFoundError:
                        pass

    def open_log(self, filename: str) -> tuple[BinaryIO, Path]:
        if not portable_path_identity(filename):
            raise LauncherError(
                "log_path_invalid",
                "log filename is not one path-safe identity",
                exit_code=70,
            )
        with self._open_directory(
            self.lock_root,
            expected_identity=self._lock_root_identity,
            label="operator run root",
        ) as root_descriptor:
            try:
                os.mkdir("logs", mode=0o700, dir_fd=root_descriptor)
            except FileExistsError:
                pass
            try:
                before = os.stat(
                    "logs",
                    dir_fd=root_descriptor,
                    follow_symlinks=False,
                )
                logs_descriptor = os.open(
                    "logs",
                    os.O_RDONLY
                    | os.O_DIRECTORY
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    dir_fd=root_descriptor,
                )
            except OSError as error:
                raise LauncherError(
                    "log_path_invalid",
                    f"operator log root cannot be opened safely: {error}",
                    exit_code=70,
                ) from error
            try:
                opened = os.fstat(logs_descriptor)
                required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
                if (
                    not stat.S_ISDIR(before.st_mode)
                    or not stat.S_ISDIR(opened.st_mode)
                    or before.st_uid != os.geteuid()
                    or opened.st_uid != os.geteuid()
                    or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or opened.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or before.st_mode & required != required
                    or opened.st_mode & required != required
                    or (before.st_dev, before.st_ino)
                    != (opened.st_dev, opened.st_ino)
                ):
                    raise LauncherError(
                        "log_path_invalid",
                        "operator log root is unsafe",
                        exit_code=70,
                    )
                descriptor = os.open(
                    filename,
                    os.O_WRONLY
                    | os.O_APPEND
                    | os.O_CREAT
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    0o600,
                    dir_fd=logs_descriptor,
                )
                metadata = os.fstat(descriptor)
                current = os.stat(
                    filename,
                    dir_fd=logs_descriptor,
                    follow_symlinks=False,
                )
                if (
                    not self._safe_file(metadata)
                    or not self._safe_file(current)
                    or (metadata.st_dev, metadata.st_ino)
                    != (current.st_dev, current.st_ino)
                ):
                    os.close(descriptor)
                    raise LauncherError(
                        "log_path_invalid",
                        "operator log is not one safe owner-held file",
                        exit_code=70,
                    )
                try:
                    stream = os.fdopen(descriptor, "ab")
                except Exception:
                    os.close(descriptor)
                    raise
                return stream, self.lock_root / "logs" / filename
            finally:
                os.close(logs_descriptor)


def _status(
    configuration: LaunchConfiguration,
    instance_id: str,
    state: LaunchInstanceState,
    resources: Sequence[Mapping[str, Any]],
    readiness: Sequence[ReadinessObservation],
    observations: Sequence[ResourceObservation],
    generation: int,
    failure: FailureRecord | None = None,
) -> LaunchInstanceStatus:
    from .contracts import ProcessResourceIdentity

    identity_fields = {
        "kind",
        "resource_id",
        "component_id",
        "host_id",
        "pid",
        "start_ticks",
        "boot_id",
        "process_group_id",
        "log_reference",
    }
    owned = [
        ProcessResourceIdentity.model_validate_json(
            json.dumps(
                {key: value for key, value in item.items() if key in identity_fields}
            )
        )
        for item in resources
    ]
    owned.sort(key=lambda item: item.resource_id)
    ordered_observations = sorted(observations, key=lambda item: item.resource_id)
    ordered_readiness = sorted(
        readiness, key=lambda item: (item.component_id, item.authority, item.source_schema)
    )
    return LaunchInstanceStatus(
        schema_version="manyforge.launch_instance_status.v1",
        launch_instance_id=instance_id,
        record_generation=generation,
        configuration=configuration,
        configuration_digest=configuration.configuration_digest,
        state=state,
        owned_resources=owned,
        resource_observations=ordered_observations,
        readiness_observations=ordered_readiness,
        last_failure=failure,
        updated_at_unix_ns=_safe_unix_timestamp(),
    )


class NativeLauncher:
    """One short-lived CLI controller for one persistent native stack."""

    def __init__(self, config: NativeLauncherConfig) -> None:
        self.config = config
        try:
            observed_package_aggregate = package_aggregate_sha256(
                config.install_root
            )
        except PackageAggregateUnavailable as error:
            raise LauncherError(
                "package_aggregate_unavailable",
                str(error),
                exit_code=71,
            ) from error
        if observed_package_aggregate != config.package_aggregate_sha256:
            raise LauncherError(
                "package_aggregate_mismatch",
                "observed package aggregate does not match the expected digest",
                exit_code=71,
            )
        try:
            self.layout = CellInstanceLayout.open_or_create(
                cell_id=config.cell_id,
                cell_root=config.instance_root,
                install_root=config.install_root,
            )
            self.layout.validate()
        except CellLayoutError as error:
            raise LauncherError(
                "cell_layout_invalid", str(error), exit_code=71
            ) from error
        self.store = _StateStore(
            root=config.instance_root / "state" / "operator",
            lock_root=config.instance_root / "run" / "operator",
        )
        self._managed: dict[str, _ManagedProcess] = {}
        self.last_start_idempotent = False
        self.last_stop_already_stopped = False

    @classmethod
    def from_state_root(cls, state_root: Path, instance_id: str) -> "NativeLauncher":
        """Open an existing record for status/stop without start inputs."""

        if not portable_path_identity(instance_id):
            raise LauncherError("invalid_instance_id", "instance id is invalid", exit_code=64)
        root = Path(state_root).absolute() / instance_id
        if (
            not root.is_dir()
            or root.is_symlink()
            or not (root / ".manyforge-cell-layout.json").is_file()
        ):
            raise LauncherError(
                "cell_layout_unavailable",
                "status/stop requires one existing launcher-created cell layout",
                exit_code=66,
            )
        launcher = cls.__new__(cls)
        try:
            launcher.layout = CellInstanceLayout.open_existing(root)
        except CellLayoutError as error:
            raise LauncherError(
                "cell_layout_invalid", str(error), exit_code=71
            ) from error
        launcher.config = SimpleNamespace(
            state_root=Path(state_root).absolute(),
            instance_id=instance_id,
            instance_root=root,
            headless_url="http://127.0.0.1:9010/",
            program_runner_url="http://127.0.0.1:9011/",
            composer_url="http://localhost:9000/",
            manager_state_root=root / "state" / "manager",
            manager_run_root=root / "run" / "manager",
            readiness_timeout_seconds=DEFAULT_TIMEOUT_SECONDS,
            poll_interval_seconds=DEFAULT_POLL_SECONDS,
        )
        launcher.store = _StateStore(
            root=root / "state" / "operator",
            lock_root=root / "run" / "operator",
        )
        launcher._managed = {}
        launcher.last_start_idempotent = False
        launcher.last_stop_already_stopped = False
        return launcher

    def _mechanical(self) -> dict[str, Any]:
        return {
            "profile": "native",
            "launch_input_digest": _launch_input_digest(self.config),
            "launch_inputs": {
                "canonical_empty_admission_filename": self.config.canonical_empty_admission_filename,
                "cell_id": self.config.cell_id,
                "manager_timeout_seconds": self.config.manager_timeout_seconds,
                "simulation_control_config": (
                    str(self.config.simulation_control_config)
                    if self.config.simulation_control_config is not None
                    else None
                ),
                "composer_static_dir": (
                    str(self.config.composer_static_dir)
                    if self.config.composer_static_dir is not None
                    else None
                ),
            },
            "paths": {
                "source_root": str(self.config.source_root),
                "install_root": str(self.config.install_root),
                "program_workspace": str(self.config.program_workspace),
                "manager_executable": str(self.config.manager_executable),
                "node_catalog_path": str(self.config.node_catalog_path),
                "alarm_catalog_path": str(self.config.alarm_catalog_path),
                "logical_scene_database": str(self.config.logical_scene_database),
                "historian_database_path": str(self.config.historian_database_path),
                "manager_state_root": str(self.config.manager_state_root),
                "manager_run_root": str(self.config.manager_run_root),
                "execution_state_root": str(self.config.execution_state_root),
                "execution_run_root": str(self.config.execution_run_root),
                "simulation_control_config": (
                    str(self.config.simulation_control_config)
                    if self.config.simulation_control_config is not None
                    else None
                ),
            },
            "processes": {
                component: {
                    "component_id": item.component_id,
                    "resource_id": component,
                    "kind": ResourceKind.PROCESS.value,
                    "host_id": "runtime",
                    "argv": list(item.argv),
                    "pid": item.pid,
                    "start_ticks": item.start_ticks,
                    "boot_id": item.boot_id,
                    "process_group_id": item.process_group_id,
                    "log_reference": str(item.log_path),
                    "log_path": str(item.log_path),
                }
                for component, item in self._managed.items()
            },
        }

    def _prepare_program_workspace(self) -> None:
        """Create or validate the one instance-owned Composer write root."""

        root = self.config.authoring_state_root
        required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        try:
            before_root = root.lstat()
            if root.resolve(strict=True) != root:
                raise OSError("authoring state root is not canonical")
            instance_fd = os.open(
                root,
                os.O_RDONLY
                | os.O_DIRECTORY
                | os.O_CLOEXEC
                | os.O_NOFOLLOW,
            )
        except OSError as error:
            raise LauncherError(
                "program_workspace_invalid",
                f"authoring state root cannot safely contain the Program workspace: {error}",
                exit_code=70,
            ) from error
        try:
            opened_root = os.fstat(instance_fd)
            root_identity = (before_root.st_dev, before_root.st_ino)
            if any(
                not stat.S_ISDIR(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or metadata.st_mode & required != required
                or (metadata.st_dev, metadata.st_ino) != root_identity
                for metadata in (before_root, opened_root)
            ):
                raise LauncherError(
                    "program_workspace_invalid",
                    "authoring state root is not one safe retained capability",
                    exit_code=70,
                )
            try:
                os.mkdir("program-workspace", mode=0o700, dir_fd=instance_fd)
            except FileExistsError:
                pass
            before_workspace = os.stat(
                "program-workspace",
                dir_fd=instance_fd,
                follow_symlinks=False,
            )
            if (
                not stat.S_ISDIR(before_workspace.st_mode)
                or before_workspace.st_uid != os.geteuid()
                or before_workspace.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or before_workspace.st_mode & required != required
            ):
                raise LauncherError(
                    "program_workspace_invalid",
                    "Program workspace is not one safe owner-held directory",
                    exit_code=70,
                )
            workspace_fd = os.open(
                "program-workspace",
                os.O_RDONLY
                | os.O_DIRECTORY
                | os.O_CLOEXEC
                | os.O_NOFOLLOW,
                dir_fd=instance_fd,
            )
            try:
                opened_workspace = os.fstat(workspace_fd)
                after_workspace = os.stat(
                    "program-workspace",
                    dir_fd=instance_fd,
                    follow_symlinks=False,
                )
                workspace_identity = (
                    before_workspace.st_dev,
                    before_workspace.st_ino,
                )
                if any(
                    not stat.S_ISDIR(metadata.st_mode)
                    or metadata.st_uid != os.geteuid()
                    or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or metadata.st_mode & required != required
                    or (metadata.st_dev, metadata.st_ino) != workspace_identity
                    for metadata in (opened_workspace, after_workspace)
                ):
                    raise LauncherError(
                        "program_workspace_invalid",
                        "Program workspace changed while opening",
                        exit_code=70,
                    )
            finally:
                os.close(workspace_fd)
            after_root = root.lstat()
            if (
                (after_root.st_dev, after_root.st_ino) != root_identity
                or root.resolve(strict=True) != root
            ):
                raise LauncherError(
                    "program_workspace_invalid",
                    "authoring state root changed while preparing the workspace",
                    exit_code=70,
                )
        except LauncherError:
            raise
        except OSError as error:
            raise LauncherError(
                "program_workspace_invalid",
                f"Program workspace cannot be safely prepared: {error}",
                exit_code=70,
            ) from error
        finally:
            os.close(instance_fd)

    @staticmethod
    def _resource_records_from_mechanical(
        processes: Mapping[str, Mapping[str, Any]],
    ) -> list[dict[str, Any]]:
        return [
            {
                **dict(item),
                "resource_id": str(item.get("resource_id", component)),
                "kind": ResourceKind.PROCESS.value,
                "component_id": str(item.get("component_id", component)),
                "host_id": str(item.get("host_id", "runtime")),
                "log_reference": str(
                    item.get("log_reference", item.get("log_path", ""))
                )
                or None,
            }
            for component, item in sorted(processes.items())
        ]

    def _port_available(
        self, *, probes: Sequence[tuple[str, int]] | None = None
    ) -> None:
        if probes is None:
            probes = (
                (SERVICE_BIND_HOSTS["composer"], 9000),
                ("127.0.0.1", 9010),
                ("127.0.0.1", 9011),
            )
        listeners: list[socket.socket] = []
        try:
            for host, port in probes:
                listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                listeners.append(listener)
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    listener.bind((host, port))
                    listener.listen()
                except OSError as error:
                    raise LauncherError("port_occupied", f"{host}:{port} is occupied: {error}", exit_code=72) from error
        finally:
            for listener in listeners:
                listener.close()

    def _response(self, url: str) -> dict[str, Any]:
        request = Request(url, method="GET")
        with urlopen(request, timeout=2.0) as response:
            value = json.loads(response.read(4 * 1024 * 1024).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("owner response must be a JSON object")
        return value

    def _readiness(
        self, configuration: LaunchConfiguration | None = None
    ) -> tuple[bool, list[ReadinessObservation], str | None]:
        if configuration is None:
            endpoint_urls = {}
        else:
            # Readiness is queried from the local runtime host.  The
            # advertised URL may be a LAN address, while Headless and the
            # Program runner intentionally bind loopback and Composer binds
            # all interfaces; never use the public address as a local probe.
            endpoint_urls = {
                endpoint.service: (
                    f"http://{('127.0.0.1' if endpoint.bind_host in {'0.0.0.0', '::'} else endpoint.bind_host)}:{endpoint.port}/"
                )
                for endpoint in configuration.endpoints
            }
        endpoints = (
            (
                "headless",
                f"{endpoint_urls.get(ServiceKind.HEADLESS, self.config.headless_url)}api/health",
            ),
            (
                "program_runner",
                f"{endpoint_urls.get(ServiceKind.PROGRAM_RUNNER, self.config.program_runner_url)}api/program/health",
            ),
            (
                "composer",
                f"{endpoint_urls.get(ServiceKind.COMPOSER, self.config.composer_url)}api/health",
            ),
        )
        rows: list[ReadinessObservation] = []
        all_ready = True
        failure: str | None = None
        now = _safe_unix_timestamp()
        for component, url in endpoints:
            try:
                payload = self._response(url)
                if component == "headless":
                    ready = payload.get("status") == "ready" and payload.get("headless_projection_ready") is True
                    schema = "manyforge.headless_health.v1"
                elif component == "program_runner":
                    ready = payload.get("started") is True and payload.get("closed") is False
                    schema = "manyforge.program_runner_health.v1"
                else:
                    ready = payload.get("status") == "ready"
                    schema = "manyforge.composer_health.v1"
                state = ReadinessState.READY if ready else ReadinessState.DEGRADED
                if not ready:
                    all_ready = False
                    failure = f"{component} owner reported not ready"
                rows.append(
                    ReadinessObservation(
                        component_id=component,
                        authority=ReadinessAuthority.OWNER_ISSUED,
                        state=state,
                        source_schema=schema,
                        owner_id=str(payload.get("owner_id", component)),
                        owner_epoch=(str(payload["owner_epoch"]) if payload.get("owner_epoch") is not None else None),
                        payload_sha256=_sha256_json(payload),
                        observed_at_unix_ns=now,
                        reason_code=None if ready else "owner_not_ready",
                    )
                )
            except (OSError, URLError, ValueError, json.JSONDecodeError) as error:
                all_ready = False
                failure = f"{component} readiness unavailable: {error}"
                rows.append(
                    ReadinessObservation(
                        component_id=component,
                        authority=ReadinessAuthority.OWNER_ISSUED,
                        state=ReadinessState.UNAVAILABLE,
                        source_schema=f"manyforge.{component}.health.v1",
                        owner_id=component,
                        payload_sha256=hashlib.sha256(str(error).encode("utf-8")).hexdigest(),
                        observed_at_unix_ns=now,
                        reason_code="owner_unavailable",
                    )
                )
        return all_ready, rows, failure

    def _resource_observations(self, resources: Sequence[Mapping[str, Any]]) -> list[ResourceObservation]:
        now = _safe_unix_timestamp()
        return [
            ResourceObservation(
                resource_id=str(item["resource_id"]),
                presence=(ResourcePresence.PRESENT if _safe_process_is_authenticated(item) else ResourcePresence.ABSENT),
                observed_at_unix_ns=now,
            )
            for item in resources
        ]

    def _spawn(self, spec: ProcessSpec) -> _ManagedProcess:
        log, log_path = self.store.open_log(f"{spec.component_id}.log")
        try:
            environment = os.environ.copy()
            environment.update(spec.environment)
            process = subprocess.Popen(
                list(spec.argv),
                cwd=str(spec.cwd),
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        finally:
            log.close()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            observation = _proc_observation(process.pid)
            if observation is not None:
                break
            time.sleep(0.01)
        observation = _proc_observation(process.pid)
        if observation is None:
            self._terminate_unrecorded_process(process)
            raise LauncherError("process_identity_unavailable", f"cannot observe {spec.component_id} PID {process.pid}", exit_code=70)
        managed = _ManagedProcess(
            component_id=spec.component_id,
            process=process,
            argv=spec.argv,
            log_path=log_path,
            pid=process.pid,
            start_ticks=observation[0],
            boot_id=_boot_id(),
            process_group_id=os.getpgid(process.pid),
        )
        self._managed[spec.component_id] = managed
        return managed

    @staticmethod
    def _terminate_unrecorded_process(process: subprocess.Popen[bytes]) -> None:
        """Stop a just-spawned child whose identity could not be recorded.

        ``start_new_session=True`` makes the Popen PID the exact process-group
        leader.  This fallback therefore remains scoped to the child just
        created by this launcher and never searches for or kills by name/port.
        """

        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                process.terminate()
            except (ProcessLookupError, PermissionError, OSError):
                pass
        try:
            process.wait(timeout=2.0)
            return
        except subprocess.TimeoutExpired:
            pass
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                process.kill()
            except (ProcessLookupError, PermissionError, OSError):
                pass
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            pass

    def _resource_records(self) -> list[dict[str, Any]]:
        return [
            {
                "kind": ResourceKind.PROCESS.value,
                "resource_id": component,
                "component_id": component,
                "host_id": "runtime",
                "pid": item.pid,
                "start_ticks": item.start_ticks,
                "boot_id": item.boot_id,
                "process_group_id": item.process_group_id,
                "log_reference": str(item.log_path),
            }
            for component, item in sorted(self._managed.items())
        ]

    def _write_live(
        self,
        configuration: LaunchConfiguration,
        state: LaunchInstanceState,
        readiness: Sequence[ReadinessObservation] = (),
        failure: FailureRecord | None = None,
        generation: int = 1,
    ) -> LaunchInstanceStatus:
        previous = self.store.read()
        if previous is not None and not can_transition(previous[0].state, state):
            raise LauncherError(
                "invalid_state_transition",
                f"cannot transition {previous[0].state.value} to {state.value}",
                exit_code=70,
                status=previous[0],
            )
        resources = self._resource_records()
        observations = self._resource_observations(self._mechanical()["processes"].values())
        if state is LaunchInstanceState.STOPPED and any(
            observation.presence is not ResourcePresence.ABSENT
            for observation in observations
        ):
            raise LauncherError(
                "cleanup_deadline_exceeded",
                "one or more recorded native processes remain after cleanup",
                exit_code=74,
            )
        status = _status(configuration, self.config.instance_id, state, resources, readiness, observations, generation, failure)
        self.store.write(status, self._mechanical())
        return status

    def _refresh_status(
        self,
        old_status: LaunchInstanceStatus,
        mechanical: Mapping[str, Any],
    ) -> LaunchInstanceStatus:
        """Reconcile recorded processes and owner readiness without restarting."""

        processes = mechanical.get("processes", {})
        resources = self._resource_records_from_mechanical(processes)
        observations = self._resource_observations(resources)
        all_present = bool(resources) and all(
            observation.presence is ResourcePresence.PRESENT
            for observation in observations
        )
        if old_status.state is LaunchInstanceState.STOPPING:
            ready, rows, reason = False, [], "native launch remains stopping"
            state = (
                LaunchInstanceState.DEGRADED
                if any(
                    observation.presence is not ResourcePresence.ABSENT
                    for observation in observations
                )
                else LaunchInstanceState.STOPPED
            )
        else:
            ready, rows, reason = (
                self._readiness(old_status.configuration)
                if all_present
                else (False, [], "one or more owned processes are absent")
            )
            state = (
                LaunchInstanceState.READY
                if all_present and ready
                else LaunchInstanceState.DEGRADED
            )
        failure = None if state is LaunchInstanceState.READY else FailureRecord(
            reason_code="component_unavailable",
            component_id="native",
            detail=(reason or "native launch is degraded")[:4096],
            occurred_at_unix_ns=_safe_unix_timestamp(),
        )
        if not can_transition(old_status.state, state):
            raise LauncherError(
                "invalid_state_transition",
                f"cannot transition {old_status.state.value} to {state.value}",
                exit_code=70,
                status=old_status,
            )
        status = _status(
            old_status.configuration,
            old_status.launch_instance_id,
            state,
            resources,
            rows,
            observations,
            old_status.record_generation + 1,
            failure,
        )
        self.store.write(status, mechanical)
        return status

    def _finish_failed_start(
        self,
        configuration: LaunchConfiguration,
        failure: FailureRecord,
        readiness: Sequence[ReadinessObservation] = (),
    ) -> LaunchInstanceStatus:
        """Boundedly clean a failed start and retain its terminal evidence."""

        current = self.store.read()
        if current is None or current[0].state is not LaunchInstanceState.STOPPING:
            self._write_live(
                configuration,
                LaunchInstanceState.STOPPING,
                readiness,
                failure,
            )
        if self._cleanup_authenticated():
            return self._write_live(
                configuration,
                LaunchInstanceState.STOPPED,
                readiness,
                failure,
            )
        cleanup_failure = FailureRecord(
            reason_code="cleanup_deadline_exceeded",
            component_id="native",
            detail="one or more authenticated native processes remain after bounded cleanup",
            occurred_at_unix_ns=_safe_unix_timestamp(),
        )
        degraded = self._write_live(
            configuration,
            LaunchInstanceState.DEGRADED,
            readiness,
            cleanup_failure,
        )
        raise LauncherError(
            "cleanup_deadline_exceeded",
            str(cleanup_failure.detail),
            exit_code=74,
            status=degraded,
        )

    def start(self) -> LaunchInstanceStatus:
        self.last_start_idempotent = False
        if not self.config.instance_id:
            raise LauncherError("invalid_instance_id", "instance id is empty", exit_code=64)
        try:
            self.layout.validate()
        except CellLayoutError as error:
            raise LauncherError("cell_layout_invalid", str(error), exit_code=71) from error
        with self.store.lock():
            configuration = _configuration(self.config)
            self._prepare_program_workspace()
            existing = self.store.read()
            if existing is not None:
                old_status, mechanical = existing
                mechanical = dict(mechanical)
                paths = mechanical.get("paths")
                if paths is None:
                    paths = {}
                if not isinstance(paths, dict):
                    raise LauncherError(
                        "status_record_invalid",
                        "stored mechanical paths record is not an object",
                        exit_code=70,
                        status=old_status,
                    )
                mechanical["paths"] = {
                    **paths,
                    "program_workspace": str(self.config.program_workspace),
                }
                if old_status.configuration.profile is not LaunchProfile.NATIVE:
                    raise LauncherError(
                        "incompatible_owner",
                        "stored launch has a different native configuration",
                        exit_code=70,
                        status=old_status,
                    )
                if mechanical.get("launch_input_digest") != _launch_input_digest(
                    self.config
                ):
                    raise LauncherError(
                        "incompatible_owner",
                        "stored launch has different cell, admission, or Composer static inputs",
                        exit_code=70,
                        status=old_status,
                    )
                disposition = decide_start(
                    old_status, configuration.configuration_digest
                )
                if disposition is StartDisposition.IDEMPOTENT:
                    self.last_start_idempotent = True
                    return self._refresh_status(old_status, mechanical)
                if disposition is StartDisposition.RECOVER:
                    processes = mechanical.get("processes", {})
                    if not isinstance(processes, dict):
                        raise LauncherError(
                            "status_record_invalid",
                            "stored mechanical process record is not an object",
                            exit_code=70,
                            status=old_status,
                        )
                    if old_status.state is not LaunchInstanceState.UNAVAILABLE:
                        resources = self._resource_records_from_mechanical(processes)
                        previous_observations = self._resource_observations(resources)
                        stopping = _status(
                            old_status.configuration,
                            old_status.launch_instance_id,
                            LaunchInstanceState.STOPPING,
                            resources,
                            (),
                            previous_observations,
                            old_status.record_generation + 1,
                            old_status.last_failure,
                        )
                        self.store.write(stopping, mechanical)
                        paths = mechanical.get("paths")
                        manager_root = (
                            Path(paths["manager_run_root"])
                            if isinstance(paths, dict)
                            and isinstance(paths.get("manager_run_root"), str)
                            else self.config.manager_run_root
                        )
                        clean = self._cleanup_authenticated(
                            processes,
                            manager_run_root=manager_root,
                        )
                        observations = self._resource_observations(resources)
                        if not clean or any(
                            observation.presence is not ResourcePresence.ABSENT
                            for observation in observations
                        ):
                            failure = FailureRecord(
                                reason_code="cleanup_deadline_exceeded",
                                component_id="native",
                                detail=(
                                    "degraded recovery could not prove prior "
                                    "owned resources absent"
                                ),
                                occurred_at_unix_ns=_safe_unix_timestamp(),
                            )
                            degraded = _status(
                                old_status.configuration,
                                old_status.launch_instance_id,
                                LaunchInstanceState.DEGRADED,
                                resources,
                                (),
                                observations,
                                stopping.record_generation + 1,
                                failure,
                            )
                            self.store.write(degraded, mechanical)
                            raise LauncherError(
                                "cleanup_deadline_exceeded",
                                str(failure.detail),
                                exit_code=74,
                                status=degraded,
                            )
                        stopped = _status(
                            old_status.configuration,
                            old_status.launch_instance_id,
                            LaunchInstanceState.STOPPED,
                            resources,
                            (),
                            observations,
                            stopping.record_generation + 1,
                        )
                        self.store.write(stopped, mechanical)
                    self._managed = {}
                if disposition is StartDisposition.CONFLICT:
                    raise LauncherError(
                        "incompatible_owner",
                        "stored launch is stopping or has a different configuration",
                        exit_code=70,
                        status=old_status,
                    )
                processes = mechanical.get("processes", {})
                resources = self._resource_records_from_mechanical(processes)
                if any(
                    observation.presence is ResourcePresence.PRESENT
                    for observation in self._resource_observations(resources)
                ):
                    raise LauncherError(
                        "incompatible_owner",
                        "stored launch still owns an authenticated process",
                        exit_code=70,
                        status=old_status,
                    )
            self._port_available()
            self._write_live(configuration, LaunchInstanceState.STARTING)
            try:
                for spec in native_commands(self.config):
                    self._managed[spec.component_id] = self._spawn(spec)
                    self._write_live(configuration, LaunchInstanceState.STARTING)
                deadline = time.monotonic() + self.config.readiness_timeout_seconds
                last_rows: list[ReadinessObservation] = []
                while time.monotonic() < deadline:
                    ready, last_rows, reason = self._readiness()
                    if ready:
                        return self._write_live(configuration, LaunchInstanceState.READY, last_rows)
                    time.sleep(self.config.poll_interval_seconds)
                failure = FailureRecord(
                    reason_code="readiness_timeout",
                    component_id="native",
                    detail=(reason or "native owner readiness timed out")[:4096],
                    occurred_at_unix_ns=_safe_unix_timestamp(),
                )
                stopped_readiness = [
                    ReadinessObservation(
                        component_id=row.component_id,
                        authority=row.authority,
                        state=ReadinessState.STOPPED,
                        source_schema=row.source_schema,
                        owner_id=row.owner_id,
                        owner_epoch=row.owner_epoch,
                        payload_sha256=row.payload_sha256,
                        observed_at_unix_ns=_safe_unix_timestamp(),
                        reason_code="startup_failed",
                    )
                    for row in last_rows
                ]
                stopped = self._finish_failed_start(
                    configuration,
                    failure,
                    stopped_readiness,
                )
                raise LauncherError(
                    "readiness_timeout",
                    str(failure.detail),
                    exit_code=73,
                    status=stopped,
                )
            except LauncherError as error:
                if error.code == "cleanup_deadline_exceeded" or error.status is not None:
                    raise
                failure = FailureRecord(
                    reason_code=error.code,
                    component_id="native",
                    detail=str(error)[:4096],
                    occurred_at_unix_ns=_safe_unix_timestamp(),
                )
                stopped = self._finish_failed_start(configuration, failure)
                error.status = stopped
                raise
            except Exception as error:
                failure = FailureRecord(
                    reason_code="native_start_failed",
                    component_id="native",
                    detail=str(error)[:4096],
                    occurred_at_unix_ns=_safe_unix_timestamp(),
                )
                stopped = self._finish_failed_start(configuration, failure)
                raise LauncherError(
                    "native_start_failed",
                    str(error),
                    exit_code=70,
                    status=stopped,
                ) from error

    def status(self) -> LaunchInstanceStatus | None:
        try:
            self.layout.validate()
        except CellLayoutError as error:
            raise LauncherError("cell_layout_invalid", str(error), exit_code=71) from error
        with self.store.lock():
            existing = self.store.read()
            if existing is None:
                return None
            old_status, mechanical = existing
            if old_status.state is LaunchInstanceState.STOPPED:
                resources = self._resource_records_from_mechanical(
                    mechanical.get("processes", {})
                )
                observations = self._resource_observations(resources)
                if any(
                    observation.presence is not ResourcePresence.ABSENT
                    for observation in observations
                ):
                    raise LauncherError(
                        "stale_launch_record",
                        "stopped launch has a recorded process that is not proven absent",
                        exit_code=70,
                        status=old_status,
                    )
                return old_status
            self._managed = {}
            return self._refresh_status(old_status, mechanical)

    def _cleanup_authenticated(
        self,
        process_records: Mapping[str, Mapping[str, Any]] | None = None,
        *,
        manager_run_root: Path | None = None,
    ) -> bool:
        processes = (
            dict(process_records)
            if process_records is not None
            else self._mechanical().get("processes", {})
        )
        manager = processes.get("manager")
        if manager is not None and _safe_process_is_authenticated(manager):
            socket_path = (manager_run_root or self.config.manager_run_root) / "manager-command.sock"
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.settimeout(2.0)
                    client.connect(str(socket_path))
                    client.sendall(b'{"api_version":"manyforge.manager_command.v1","command":"shutdown_runtime"}')
                    client.recv(64 * 1024)
            except OSError:
                pass
        deadline = time.monotonic() + min(10.0, self.config.readiness_timeout_seconds)
        for component in ("composer", "program_runner", "headless", "manager"):
            item = processes.get(component)
            if item is None or not _safe_process_is_authenticated(item):
                continue
            pgid = int(item["process_group_id"])
            if pgid > 1 and pgid != os.getpgrp():
                try:
                    os.killpg(pgid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        while time.monotonic() < deadline:
            if not any(_safe_process_is_authenticated(item) for item in processes.values()):
                return True
            time.sleep(0.05)
        for item in processes.values():
            if _safe_process_is_authenticated(item):
                pgid = int(item["process_group_id"])
                if pgid > 1 and pgid != os.getpgrp():
                    try:
                        os.killpg(pgid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        time.sleep(0.05)
        return not any(_safe_process_is_authenticated(item) for item in processes.values())

    def stop(self) -> LaunchInstanceStatus | None:
        self.last_stop_already_stopped = False
        try:
            self.layout.validate()
        except CellLayoutError as error:
            raise LauncherError("cell_layout_invalid", str(error), exit_code=71) from error
        with self.store.lock():
            existing = self.store.read()
            if existing is None:
                return None
            old_status, mechanical = existing
            processes = mechanical.get("processes", {})
            if old_status.state is LaunchInstanceState.STOPPED:
                resources = self._resource_records_from_mechanical(processes)
                if all(
                    observation.presence is ResourcePresence.ABSENT
                    for observation in self._resource_observations(resources)
                ):
                    self.last_stop_already_stopped = True
                    return old_status
                raise LauncherError(
                    "stale_launch_record",
                    "stopped launch still has an authenticated recorded process",
                    exit_code=70,
                    status=old_status,
                )
            if not can_transition(
                old_status.state, LaunchInstanceState.STOPPING
            ):
                raise LauncherError(
                    "invalid_state_transition",
                    f"cannot transition {old_status.state.value} to stopping",
                    exit_code=70,
                    status=old_status,
                )
            stopping = _status(
                old_status.configuration,
                old_status.launch_instance_id,
                LaunchInstanceState.STOPPING,
                self._resource_records_from_mechanical(processes),
                (),
                self._resource_observations(self._resource_records_from_mechanical(processes)),
                old_status.record_generation + 1,
            )
            self.store.write(stopping, mechanical)
            clean = self._cleanup_authenticated(
                processes,
                manager_run_root=Path(
                    mechanical["paths"]["manager_run_root"]
                ),
            )
            observations = [
                ResourceObservation(
                    resource_id=str(item.get("resource_id", component)),
                    presence=ResourcePresence.ABSENT if not _safe_process_is_authenticated(item) else ResourcePresence.UNKNOWN,
                    observed_at_unix_ns=_safe_unix_timestamp(),
                )
                for component, item in processes.items()
            ]
            if not clean:
                failure = FailureRecord(
                    reason_code="cleanup_deadline_exceeded",
                    component_id="native",
                    detail="one or more authenticated native processes remain after bounded cleanup",
                    occurred_at_unix_ns=_safe_unix_timestamp(),
                )
                degraded = _status(old_status.configuration, old_status.launch_instance_id, LaunchInstanceState.DEGRADED, self._resource_records_from_mechanical(processes), (), observations, stopping.record_generation + 1, failure)
                self.store.write(degraded, mechanical)
                raise LauncherError("cleanup_deadline_exceeded", str(failure.detail), exit_code=74, status=degraded)
            stopped = _status(old_status.configuration, old_status.launch_instance_id, LaunchInstanceState.STOPPED, self._resource_records_from_mechanical(processes), (), observations, stopping.record_generation + 1)
            if not can_transition(
                stopping.state, stopped.state
            ) or any(
                observation.presence is not ResourcePresence.ABSENT
                for observation in observations
            ):
                raise LauncherError(
                    "cleanup_deadline_exceeded",
                    "recorded native processes remain after bounded cleanup",
                    exit_code=74,
                )
            self.store.write(stopped, mechanical)
            return stopped


__all__ = [
    "LauncherError",
    "NativeLauncher",
    "NativeLauncherConfig",
    "ProcessSpec",
    "native_commands",
]
