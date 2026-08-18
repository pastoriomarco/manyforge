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
import subprocess
import sys
import time
from types import SimpleNamespace
from typing import Any, Iterator, Mapping, Sequence
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


STATUS_ENVELOPE_SCHEMA = "manyforge.native_operator_instance.v1"
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
    package_root: Path
    node_catalog_path: Path
    alarm_catalog_path: Path
    publication_root: Path
    manager_state_root: Path
    runtime_state_root: Path
    execution_state_root: Path
    logical_scene_database: Path
    historian_database_path: Path
    composer_static_dir: Path | None = None
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
    reconciliation_period_ms: int = 20

    def __post_init__(self) -> None:
        path_fields = (
            "source_root",
            "manager_executable",
            "package_root",
            "node_catalog_path",
            "alarm_catalog_path",
            "publication_root",
            "manager_state_root",
            "runtime_state_root",
            "execution_state_root",
            "logical_scene_database",
            "historian_database_path",
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
            "package_root",
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
        if not self.instance_id or "/" in self.instance_id or "\\" in self.instance_id:
            raise ValueError("instance_id must be one path-safe non-empty identifier")
        if (
            not self.cell_id
            or "/" in self.cell_id
            or "\\" in self.cell_id
            or any(character.isspace() for character in self.cell_id)
        ):
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
        if not 1 <= self.reconciliation_period_ms <= 60_000:
            raise ValueError("reconciliation_period_ms must be in [1, 60000]")

    @property
    def instance_root(self) -> Path:
        return self.state_root / self.instance_id

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

    base_env = {"MANYFORGE_FS_ROOTS": str(config.package_root)}
    manager = ProcessSpec(
        "manager",
        tuple(
            str(item)
            for item in (
                config.manager_executable,
                "--package-root",
                config.package_root,
                "--canonical-empty-admission-filename",
                config.canonical_empty_admission_filename,
                "--manager-state-root",
                config.manager_state_root,
                "--runtime-state-root",
                config.runtime_state_root,
                "--logical-scene-database",
                config.logical_scene_database,
                "--publication-root",
                config.publication_root,
                "--reconciliation-period-ms",
                config.reconciliation_period_ms,
            )
        ),
        base_env,
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
            "--publication-root",
            str(config.publication_root),
            "--package-root",
            str(config.package_root),
            "--node-catalog-path",
            str(config.node_catalog_path),
            "--alarm-catalog-path",
            str(config.alarm_catalog_path),
            "--historian-database-path",
            str(config.historian_database_path),
            "--manager-state-root",
            str(config.manager_state_root),
            "--execution-state-root",
            # The native profile uses Manager's managed execution host.  Its
            # command and CCLC surfaces are intentionally published under the
            # Manager-provided staging/runtime root, not the standalone-host
            # execution root.
            str(config.runtime_state_root),
            "--cell-id",
            config.cell_id,
            "--owner-id",
            f"headless-api.{config.instance_id}",
            "--freshness-ms",
            "2000",
            "--program-runner-url",
            config.program_runner_url,
        ),
        base_env,
        config.source_root,
    )
    program_runner = ProcessSpec(
        "program_runner",
        (
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
        ),
        base_env,
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
    )
    if config.composer_static_dir is not None:
        composer_argv += ("--static-dir", str(config.composer_static_dir))
    composer = ProcessSpec(
        "composer",
        composer_argv,
        base_env,
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
        install_root=str(config.package_root),
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
    def __init__(self, config: NativeLauncherConfig) -> None:
        self.root = config.instance_root
        self.root.mkdir(parents=True, exist_ok=True)
        self.record_path = self.root / "launch_instance.json"
        self.lock_path = self.root / "instance.lock"

    @contextmanager
    def lock(self) -> Iterator[None]:
        with self.lock_path.open("a+b") as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)

    def read(self) -> tuple[LaunchInstanceStatus, dict[str, Any]] | None:
        if not self.record_path.exists():
            return None
        try:
            envelope = json.loads(self.record_path.read_text(encoding="utf-8"))
            if envelope.get("schema_version") != STATUS_ENVELOPE_SCHEMA:
                raise ValueError("unexpected status envelope schema")
            status = LaunchInstanceStatus.model_validate_json(
                json.dumps(envelope["status"], ensure_ascii=False)
            )
            mechanical = envelope["mechanical"]
            if not isinstance(mechanical, dict):
                raise ValueError("mechanical status must be an object")
            return status, mechanical
        except Exception as error:
            raise LauncherError("status_record_invalid", str(error), exit_code=70) from error

    def write(self, status: LaunchInstanceStatus, mechanical: Mapping[str, Any]) -> None:
        envelope = {
            "schema_version": STATUS_ENVELOPE_SCHEMA,
            "status": status.model_dump(mode="json"),
            "mechanical": dict(mechanical),
        }
        temporary = self.root / f".launch_instance.{os.getpid()}.tmp"
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(envelope, stream, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.record_path)
        directory_fd = os.open(self.root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)


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
        self.store = _StateStore(config)
        self._managed: dict[str, _ManagedProcess] = {}
        self.last_start_idempotent = False
        self.last_stop_already_stopped = False

    @classmethod
    def from_state_root(cls, state_root: Path, instance_id: str) -> "NativeLauncher":
        """Open an existing record for status/stop without start inputs."""

        root = Path(state_root).absolute() / instance_id
        if not instance_id or "/" in instance_id or "\\" in instance_id:
            raise LauncherError("invalid_instance_id", "instance id is invalid", exit_code=64)
        root.mkdir(parents=True, exist_ok=True)
        launcher = cls.__new__(cls)
        launcher.config = SimpleNamespace(
            state_root=Path(state_root).absolute(),
            instance_id=instance_id,
            instance_root=root,
            headless_url="http://127.0.0.1:9010/",
            program_runner_url="http://127.0.0.1:9011/",
            composer_url="http://localhost:9000/",
            manager_state_root=root / "manager",
            readiness_timeout_seconds=DEFAULT_TIMEOUT_SECONDS,
            poll_interval_seconds=DEFAULT_POLL_SECONDS,
        )
        launcher.store = object.__new__(_StateStore)
        launcher.store.root = root
        launcher.store.record_path = root / "launch_instance.json"
        launcher.store.lock_path = root / "instance.lock"
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
                "composer_static_dir": (
                    str(self.config.composer_static_dir)
                    if self.config.composer_static_dir is not None
                    else None
                ),
            },
            "paths": {
                "source_root": str(self.config.source_root),
                "package_root": str(self.config.package_root),
                "manager_executable": str(self.config.manager_executable),
                "node_catalog_path": str(self.config.node_catalog_path),
                "alarm_catalog_path": str(self.config.alarm_catalog_path),
                "logical_scene_database": str(self.config.logical_scene_database),
                "historian_database_path": str(self.config.historian_database_path),
                "manager_state_root": str(self.config.manager_state_root),
                "runtime_state_root": str(self.config.runtime_state_root),
                "execution_state_root": str(self.config.execution_state_root),
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

    def _port_available(self) -> None:
        probes = ((SERVICE_BIND_HOSTS["composer"], 9000), ("127.0.0.1", 9010), ("127.0.0.1", 9011))
        listeners: list[socket.socket] = []
        try:
            for host, port in probes:
                listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
                try:
                    listener.bind((host, port))
                except OSError as error:
                    listener.close()
                    raise LauncherError("port_occupied", f"{host}:{port} is occupied: {error}", exit_code=72) from error
                listeners.append(listener)
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
                presence=(ResourcePresence.PRESENT if _process_is_authenticated(item) else ResourcePresence.ABSENT),
                observed_at_unix_ns=now,
            )
            for item in resources
        ]

    def _spawn(self, spec: ProcessSpec) -> _ManagedProcess:
        self.store.root.joinpath("logs").mkdir(parents=True, exist_ok=True)
        log_path = self.store.root / "logs" / f"{spec.component_id}.log"
        log = log_path.open("ab")
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
        with self.store.lock():
            configuration = _configuration(self.config)
            existing = self.store.read()
            if existing is not None:
                old_status, mechanical = existing
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
                if disposition in (
                    StartDisposition.IDEMPOTENT,
                    StartDisposition.RECOVER,
                ):
                    self.last_start_idempotent = True
                    return self._refresh_status(old_status, mechanical)
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
            self.config.instance_root.mkdir(parents=True, exist_ok=True)
            for path in (
                self.config.publication_root,
                self.config.manager_state_root,
                self.config.runtime_state_root,
                self.config.execution_state_root,
            ):
                path.mkdir(parents=True, exist_ok=True)
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
        manager_state_root: Path | None = None,
    ) -> bool:
        processes = (
            dict(process_records)
            if process_records is not None
            else self._mechanical().get("processes", {})
        )
        manager = processes.get("manager")
        if manager is not None and _process_is_authenticated(manager):
            socket_path = (manager_state_root or self.config.manager_state_root) / "manager-command.sock"
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
            if item is None or not _process_is_authenticated(item):
                continue
            pgid = int(item["process_group_id"])
            if pgid > 1 and pgid != os.getpgrp():
                try:
                    os.killpg(pgid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        while time.monotonic() < deadline:
            if not any(_process_is_authenticated(item) for item in processes.values()):
                return True
            time.sleep(0.05)
        for item in processes.values():
            if _process_is_authenticated(item):
                pgid = int(item["process_group_id"])
                if pgid > 1 and pgid != os.getpgrp():
                    try:
                        os.killpg(pgid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        time.sleep(0.05)
        return not any(_process_is_authenticated(item) for item in processes.values())

    def stop(self) -> LaunchInstanceStatus | None:
        self.last_stop_already_stopped = False
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
                manager_state_root=Path(
                    mechanical["paths"]["manager_state_root"]
                ),
            )
            observations = [
                ResourceObservation(
                    resource_id=str(item.get("resource_id", component)),
                    presence=ResourcePresence.ABSENT if not _process_is_authenticated(item) else ResourcePresence.UNKNOWN,
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
