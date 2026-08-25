"""Supported O1 split Isaac operator profile.

The profile is an outer lifecycle owner only. It starts Isaac Sim on the
laptop, starts the source-matched ManyForge/ROS composition on the configured
runtime host through an explicit SSH command, and records exact process
identities from both hosts. Manager, Headless, Program runner, Composer, and
the Isaac scene barrier retain semantic ownership of their respective state.

The remote helpers are deliberately small and identity-scoped. They never
search by process name, kill by port, or adopt an unrecorded process.
"""

from __future__ import annotations

import base64
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import time
from types import SimpleNamespace
from typing import Any, Mapping, Sequence

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
    RosTopology,
    RuntimeArtifactIdentity,
    SERVICE_BIND_HOSTS,
    SERVICE_PORTS,
    ServiceEndpoint,
    ServiceKind,
    can_transition,
    configuration_digest,
)
from .orchestrator import (
    DEFAULT_POLL_SECONDS,
    DEFAULT_STATE_ROOT,
    DEFAULT_TIMEOUT_SECONDS,
    LauncherError,
    _StateStore,
    _boot_id,
    _command_line,
    _proc_observation,
    _safe_unix_timestamp,
    _status,
    _source_identity,
)


DEFAULT_WORLD_NAME = "isaac_manipulator_scene_light"
DEFAULT_RUNTIME_NAMESPACE = "/manyforge/isaac_ur10e_light_robot"
DEFAULT_SIMULATOR_HOST = "192.168.1.132"
DEFAULT_RUNTIME_HOST = "tndlux@192.168.1.68"
DEFAULT_STATIC_PEERS = ("192.168.1.132", "192.168.1.68")
DEFAULT_ROS_DOMAIN = 0
DEFAULT_RMW = "rmw_fastrtps_cpp"
DEFAULT_DISCOVERY_RANGE = "LOCALHOST"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _sha256_json(value: Mapping[str, Any]) -> str:
    return hashlib.sha256(
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    ).hexdigest()


def _last_json_object(text: str) -> dict[str, Any]:
    for line in reversed(text.splitlines()):
        candidate = line.strip()
        if not candidate:
            continue
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    raise ValueError("remote operator returned no JSON object")


def _world_uri(path: Path) -> str:
    return "file:" + path.absolute().as_posix()


def _as_namespace(value: Any) -> Any:
    if isinstance(value, dict):
        return SimpleNamespace(**{key: _as_namespace(item) for key, item in value.items()})
    if isinstance(value, list):
        return [_as_namespace(item) for item in value]
    return value


def _namespace_to_dict(value: Any) -> Any:
    """Convert a persisted namespace back to JSON-ready values."""

    if isinstance(value, SimpleNamespace):
        return {key: _namespace_to_dict(item) for key, item in vars(value).items()}
    if isinstance(value, Mapping):
        return {str(key): _namespace_to_dict(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_namespace_to_dict(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class RemoteNativeConfig:
    """All remote paths and identities required for one Orin launch."""

    target: str = DEFAULT_RUNTIME_HOST
    source_root: str = "/mnt/nova_ssd/codex-manyforge/runners/manyforge"
    manager_executable: str = ""
    install_root: str = ""
    node_catalog_path: str = ""
    alarm_catalog_path: str = ""
    state_root: str = ""
    composer_static_dir: str = ""
    canonical_empty_admission_filename: str = "canonical-empty-admission.json"
    cell_id: str = "ur10e_isaac_cell"
    advertised_host: str = "192.168.1.68"
    package_aggregate_sha256: str = ""
    compiler_build_manifest_sha256: str = ""
    node_environment_sha256: str = ""
    simulation_control_config: str = ""
    bridge_state_root: str = "/mnt/nova_ssd/codex-manyforge/runners/isaac-bridge"
    ros_setup_script: str = "/opt/ros/jazzy/setup.bash"
    ros_environment_script: str = ""
    python_executable: str = "python3"

    def __post_init__(self) -> None:
        if not self.target or any(character.isspace() for character in self.target):
            raise ValueError("remote target must be one bounded SSH target")
        required_paths = (
            "source_root",
            "manager_executable",
            "install_root",
            "node_catalog_path",
            "alarm_catalog_path",
            "state_root",
            "simulation_control_config",
            "bridge_state_root",
            "ros_setup_script",
            "ros_environment_script",
        )
        for name in required_paths:
            value = getattr(self, name)
            if not value or not value.startswith("/"):
                raise ValueError(f"{name} must be one absolute remote path")
        if self.composer_static_dir and not self.composer_static_dir.startswith("/"):
            raise ValueError("composer_static_dir must be an absolute remote path")
        if not self.advertised_host or any(
            character.isspace() for character in self.advertised_host
        ):
            raise ValueError("remote advertised_host is invalid")
        for name in (
            "package_aggregate_sha256",
            "compiler_build_manifest_sha256",
            "node_environment_sha256",
        ):
            value = getattr(self, name)
            if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
                raise ValueError(f"{name} must be one full lowercase SHA-256")
        if not self.canonical_empty_admission_filename or "/" in self.canonical_empty_admission_filename:
            raise ValueError("canonical_empty_admission_filename must be package-relative")


@dataclass(frozen=True, slots=True)
class IsaacLauncherConfig:
    """Laptop outer-lifecycle configuration for the split Isaac profile."""

    source_root: Path
    isaac_executable: Path
    world_path: Path
    remote: RemoteNativeConfig
    state_root: Path = DEFAULT_STATE_ROOT
    instance_id: str = "isaac"
    simulator_host: str = DEFAULT_SIMULATOR_HOST
    control_host: str = ""
    static_peers: tuple[str, ...] = DEFAULT_STATIC_PEERS
    ros_domain: int = DEFAULT_ROS_DOMAIN
    rmw: str = DEFAULT_RMW
    discovery_range: str = DEFAULT_DISCOVERY_RANGE
    local_ros_setup_script: Path = Path("/opt/ros/jazzy/setup.bash")
    local_ros_environment_script: Path | None = None
    expected_world_name: str = DEFAULT_WORLD_NAME
    runtime_namespace: str = DEFAULT_RUNTIME_NAMESPACE
    expected_source_fingerprint: str | None = None
    expected_head_commit: str | None = None
    expected_head_tree: str | None = None
    readiness_timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS
    poll_interval_seconds: float = DEFAULT_POLL_SECONDS
    manager_timeout_seconds: float = 60.0
    ssh_executable: Path = Path("/usr/bin/ssh")
    ssh_timeout_seconds: float = 15.0

    def __post_init__(self) -> None:
        for name in (
            "source_root",
            "isaac_executable",
            "world_path",
            "state_root",
            "local_ros_setup_script",
            "ssh_executable",
        ):
            value = Path(getattr(self, name))
            if not value.is_absolute():
                raise ValueError(f"{name} must be an absolute path")
            object.__setattr__(self, name, value)
        if not self.source_root.is_dir():
            raise ValueError(f"source_root does not exist: {self.source_root}")
        if not self.isaac_executable.is_file() or not os.access(self.isaac_executable, os.X_OK):
            raise ValueError("isaac_executable must be an executable file")
        isaac_ros_setup = self.isaac_executable.parent / "setup_ros_env.sh"
        if not isaac_ros_setup.is_file():
            raise ValueError("Isaac install must provide setup_ros_env.sh")
        if not self.world_path.is_file():
            raise ValueError("world_path must be an existing USD file")
        if not self.local_ros_setup_script.is_file():
            raise ValueError("local_ros_setup_script must be an existing file")
        if not self.ssh_executable.is_file() or not os.access(self.ssh_executable, os.X_OK):
            raise ValueError("ssh_executable must be executable")
        environment_script = self.local_ros_environment_script
        if environment_script is None:
            environment_script = self.source_root / "config/ros/isaac_i9.env.sh"
        environment_script = Path(environment_script)
        if not environment_script.is_absolute() or not environment_script.is_file():
            raise ValueError("local_ros_environment_script must be an existing absolute file")
        object.__setattr__(self, "local_ros_environment_script", environment_script)
        if not self.instance_id or "/" in self.instance_id or "\\" in self.instance_id:
            raise ValueError("instance_id must be one path-safe identifier")
        if not self.expected_world_name or "/" in self.expected_world_name:
            raise ValueError("expected_world_name must be one bounded world name")
        if not self.runtime_namespace.startswith("/") or " " in self.runtime_namespace:
            raise ValueError("runtime_namespace must be one absolute ROS namespace")
        if type(self.ros_domain) is not int or not 0 <= self.ros_domain <= 232:
            raise ValueError("ros_domain must be in [0, 232]")
        if self.rmw != "rmw_fastrtps_cpp" or self.discovery_range != "LOCALHOST":
            raise ValueError("O1 Isaac requires the recorded Fast DDS LOCALHOST topology")
        peers = tuple(sorted(set(self.static_peers)))
        if peers != self.static_peers or not peers:
            raise ValueError("static_peers must be sorted and unique")
        if not 0 < self.readiness_timeout_seconds <= 600:
            raise ValueError("readiness_timeout_seconds must be in (0, 600]")
        if not 0.01 <= self.poll_interval_seconds <= 5:
            raise ValueError("poll_interval_seconds must be in [0.01, 5]")
        if not 0 < self.manager_timeout_seconds <= 60:
            raise ValueError("manager_timeout_seconds must be in (0, 60]")
        if not 0 < self.ssh_timeout_seconds <= 120:
            raise ValueError("ssh_timeout_seconds must be in (0, 120]")
        if self.expected_source_fingerprint is not None and (
            len(self.expected_source_fingerprint) != 64
            or any(
                character not in "0123456789abcdef"
                for character in self.expected_source_fingerprint
            )
        ):
            raise ValueError("expected_source_fingerprint must be a lowercase SHA-256")
        if self.expected_head_commit is not None and (
            len(self.expected_head_commit) != 40
            or any(
                character not in "0123456789abcdef"
                for character in self.expected_head_commit
            )
        ):
            raise ValueError("expected_head_commit must be a lowercase Git SHA")
        if self.expected_head_tree is not None and (
            len(self.expected_head_tree) != 40
            or any(
                character not in "0123456789abcdef"
                for character in self.expected_head_tree
            )
        ):
            raise ValueError("expected_head_tree must be a lowercase Git tree SHA")

    @property
    def world_uri(self) -> str:
        return _world_uri(self.world_path)

    @property
    def instance_root(self) -> Path:
        return self.state_root / self.instance_id

    def input_digest(self) -> str:
        return _sha256_json(
            {
                "instance_id": self.instance_id,
                "world_uri": self.world_uri,
                "world_name": self.expected_world_name,
                "runtime_namespace": self.runtime_namespace,
                "simulator_host": self.simulator_host,
                "remote_target": self.remote.target,
                "remote_source_root": self.remote.source_root,
                "remote_state_root": self.remote.state_root,
                "remote_cell_id": self.remote.cell_id,
                "remote_advertised_host": self.remote.advertised_host,
                "static_peers": self.static_peers,
            }
        )

    def record(self) -> dict[str, Any]:
        value = asdict(self)
        value["source_root"] = str(self.source_root)
        value["isaac_executable"] = str(self.isaac_executable)
        value["world_path"] = str(self.world_path)
        value["state_root"] = str(self.state_root)
        value["local_ros_setup_script"] = str(self.local_ros_setup_script)
        value["local_ros_environment_script"] = str(self.local_ros_environment_script)
        value["ssh_executable"] = str(self.ssh_executable)
        value["remote"] = asdict(self.remote)
        value["static_peers"] = list(self.static_peers)
        return value


_REMOTE_SPAWN = r'''
import base64,json,os,pathlib,signal,subprocess,sys,time
payload=json.loads(base64.b64decode(sys.argv[1]).decode())
def obs(pid):
    try: text=pathlib.Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError,PermissionError,OSError): return None
    close=text.rfind(")")
    if close < 0: return None
    fields=text[close+2:].split()
    if len(fields) < 20: return None
    try: return int(fields[19]),fields[0]
    except ValueError: return None
signal.signal(signal.SIGHUP,signal.SIG_IGN)
root=pathlib.Path(payload["state_root"]); root.mkdir(parents=True,exist_ok=True)
log_path=pathlib.Path(payload["log_path"]); log_path.parent.mkdir(parents=True,exist_ok=True)

def cmdline(pid):
    try: return [x.decode(errors="replace") for x in pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if x]
    except (FileNotFoundError,PermissionError,OSError): return None

log=log_path.open("ab")
env=os.environ.copy(); env.update(payload.get("environment",{}))
p=subprocess.Popen(payload["argv"],cwd=payload.get("cwd") or None,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
deadline=time.monotonic()+5.0
while time.monotonic() < deadline and obs(p.pid) is None: time.sleep(.01)
o=obs(p.pid)
actual_argv=cmdline(p.pid)
if o is None or actual_argv is None: raise SystemExit("remote process identity unavailable")
meta={"resource_id":payload["resource_id"],"component_id":payload["resource_id"],"kind":"process","host_id":"runtime","pid":p.pid,"start_ticks":o[0],"boot_id":pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip(),"process_group_id":os.getpgid(p.pid),"argv":actual_argv,"log_reference":str(log_path),"log_path":str(log_path),"meta_path":str(payload["meta_path"])}
pathlib.Path(payload["meta_path"]).write_text(json.dumps(meta,sort_keys=True,separators=(",",":"))+"\n")
print(json.dumps(meta,sort_keys=True,separators=(",",":")),flush=True)
os._exit(0)
'''

_REMOTE_PROBE = r'''
import base64,json,os,pathlib,sys
meta=json.loads(base64.b64decode(sys.argv[1]).decode())
def obs(pid):
    try: text=pathlib.Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError,PermissionError,OSError): return None
    close=text.rfind(")")
    if close < 0: return None
    fields=text[close+2:].split()
    if len(fields) < 20: return None
    try: return int(fields[19]),fields[0]
    except ValueError: return None
pid=int(meta["pid"]); current=obs(pid); present=False
if current is not None and current[0] == int(meta["start_ticks"]) and current[1] not in {"Z","X"}:
    try: present=pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip() == meta["boot_id"] and os.getpgid(pid) == int(meta["process_group_id"])
    except (FileNotFoundError,PermissionError,OSError): present=False
    if present:
        try:
            cmd=tuple(x.decode(errors="replace") for x in pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if x)
            present=cmd[:len(meta["argv"])] == tuple(meta["argv"])
        except (FileNotFoundError,PermissionError,OSError): present=False
print(json.dumps({"present":present,"pid":pid,"resource_id":meta.get("resource_id")},sort_keys=True,separators=(",",":")))
'''

_REMOTE_STOP = r'''
import base64,json,os,pathlib,signal,sys,time
meta=json.loads(base64.b64decode(sys.argv[1]).decode()); pid=int(meta["pid"])
def obs(pid):
    try: text=pathlib.Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError,PermissionError,OSError): return None
    close=text.rfind(")")
    if close < 0: return None
    fields=text[close+2:].split()
    if len(fields) < 20: return None
    try: return int(fields[19]),fields[0]
    except ValueError: return None
def auth():
    current=obs(pid)
    if current is None or current[0] != int(meta["start_ticks"]) or current[1] in {"Z","X"}: return False
    try:
        if pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip() != meta["boot_id"] or os.getpgid(pid) != int(meta["process_group_id"]): return False
        cmd=tuple(x.decode(errors="replace") for x in pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if x)
        return cmd[:len(meta["argv"])] == tuple(meta["argv"])
    except (FileNotFoundError,PermissionError,OSError): return False
if auth():
    try: os.killpg(int(meta["process_group_id"]),signal.SIGTERM)
    except (ProcessLookupError,PermissionError,OSError): pass
deadline=time.monotonic()+8.0
while time.monotonic() < deadline and auth(): time.sleep(.05)
if auth():
    try: os.killpg(int(meta["process_group_id"]),signal.SIGKILL)
    except (ProcessLookupError,PermissionError,OSError): pass
time.sleep(.05)
print(json.dumps({"clean":not auth(),"present":auth()},sort_keys=True,separators=(",",":")))
'''

_REMOTE_BARRIER_READ = r'''
import base64,json,pathlib,sys
path=pathlib.Path(json.loads(base64.b64decode(sys.argv[1]).decode()))
try: text=path.read_text(errors="replace")[-1048576:]
except (FileNotFoundError,PermissionError,OSError): print("{}"); raise SystemExit(0)
for line in reversed(text.splitlines()):
    start=line.find("{")
    if start < 0: continue
    try: value=json.loads(line[start:])
    except json.JSONDecodeError: continue
    if isinstance(value,dict) and value.get("schema_version")=="manyforge.isaac_scene_load_barrier.v1":
        print(json.dumps(value,sort_keys=True,separators=(",",":"))); raise SystemExit(0)
print("{}")
'''

_REMOTE_META_READ = r'''
import base64,json,pathlib,sys
path=pathlib.Path(json.loads(base64.b64decode(sys.argv[1]).decode()))
try: value=json.loads(path.read_text())
except (FileNotFoundError,PermissionError,OSError,ValueError): print("{}"); raise SystemExit(0)
if isinstance(value,dict): print(json.dumps(value,sort_keys=True,separators=(",",":")))
else: print("{}")
'''

_REMOTE_META_STOP = r'''
import base64,json,os,pathlib,signal,sys,time
path=pathlib.Path(json.loads(base64.b64decode(sys.argv[1]).decode()))
try: meta=json.loads(path.read_text())
except (FileNotFoundError,PermissionError,OSError,ValueError):
    print(json.dumps({"clean":False,"present":False,"reason":"meta_unavailable"},sort_keys=True,separators=(",",":"))); raise SystemExit(0)
pid=int(meta["pid"])
def obs(pid):
    try: text=pathlib.Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError,PermissionError,OSError): return None
    close=text.rfind(")")
    if close < 0: return None
    fields=text[close+2:].split()
    if len(fields) < 20: return None
    try: return int(fields[19]),fields[0]
    except ValueError: return None
def auth():
    current=obs(pid)
    if current is None or current[0] != int(meta["start_ticks"]) or current[1] in {"Z","X"}: return False
    try:
        if pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip() != meta["boot_id"] or os.getpgid(pid) != int(meta["process_group_id"]): return False
        cmd=tuple(x.decode(errors="replace") for x in pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if x)
        return cmd[:len(meta["argv"])] == tuple(meta["argv"])
    except (FileNotFoundError,PermissionError,OSError): return False
if auth():
    try: os.killpg(int(meta["process_group_id"]),signal.SIGTERM)
    except (ProcessLookupError,PermissionError,OSError): pass
deadline=time.monotonic()+8.0
while time.monotonic() < deadline and auth(): time.sleep(.05)
if auth():
    try: os.killpg(int(meta["process_group_id"]),signal.SIGKILL)
    except (ProcessLookupError,PermissionError,OSError): pass
time.sleep(.05)
print(json.dumps({"clean":not auth(),"present":auth()},sort_keys=True,separators=(",",":")))
'''

_REMOTE_SIM_STOP = r'''
import json
import rclpy
from manyforge_ros.simulation_control import SimulationControlClient
rclpy.init(args=None)
node=rclpy.create_node("manyforge_operator_isaac_stop")
try:
    result=SimulationControlClient(node,service_timeout_s=5.0).set_simulation_state(state="stopped")
    print(json.dumps(result.to_dict(),sort_keys=True,separators=(",",":")))
finally:
    node.destroy_node()
    if rclpy.ok(): rclpy.shutdown()
'''


class IsaacLauncher:
    """Persistent split-profile controller with bounded remote operations."""

    def __init__(self, config: IsaacLauncherConfig) -> None:
        self.config = config
        self.store = self._make_store(config)
        self.last_start_idempotent = False
        self.last_stop_already_stopped = False
        self._remote_status: dict[str, Any] | None = None
        self._processes: dict[str, dict[str, Any]] = {}
        self._persisted_launch_input_digest: str | None = None
        self._unrecorded_bridge_meta_path: str | None = None
        self._sim_stop_unverified = False

    @staticmethod
    def _make_store(config: IsaacLauncherConfig) -> _StateStore:
        config.instance_root.mkdir(parents=True, exist_ok=True)
        return _StateStore(
            root=config.instance_root,
            lock_root=config.instance_root,
        )

    @classmethod
    def from_state_root(cls, state_root: Path, instance_id: str) -> "IsaacLauncher":
        if not instance_id or "/" in instance_id or "\\" in instance_id:
            raise LauncherError("invalid_instance_id", "instance id is invalid", exit_code=64)
        root = Path(state_root).absolute() / instance_id
        root.mkdir(parents=True, exist_ok=True)
        store = _StateStore(root=root, lock_root=root)
        launcher = cls.__new__(cls)
        launcher.store = store
        launcher.config = None
        launcher.last_start_idempotent = False
        launcher.last_stop_already_stopped = False
        launcher._remote_status = None
        launcher._processes = {}
        launcher._persisted_launch_input_digest = None
        launcher._unrecorded_bridge_meta_path = None
        launcher._sim_stop_unverified = False
        if store.record_path.exists():
            try:
                envelope = json.loads(store.record_path.read_text(encoding="utf-8"))
                mechanical = envelope.get("mechanical", {})
                launcher.config = _as_namespace(mechanical.get("config", {}))
                launcher._remote_status = mechanical.get("native_status")
                launcher._processes = dict(mechanical.get("processes", {}))
                digest = mechanical.get("launch_input_digest")
                launcher._persisted_launch_input_digest = digest if isinstance(digest, str) else None
                orphan_meta_path = mechanical.get("unrecorded_bridge_meta_path")
                launcher._unrecorded_bridge_meta_path = orphan_meta_path if isinstance(orphan_meta_path, str) else None
            except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
                raise LauncherError("status_record_invalid", str(error), exit_code=70) from error
        return launcher

    def _require_config(self) -> Any:
        if self.config is None:
            raise LauncherError("instance_unavailable", "no persisted Isaac launch configuration exists", exit_code=70)
        return self.config

    def _ssh(self, remote_command: str, *, timeout: float | None = None) -> str:
        config = self._require_config()
        executable = Path(getattr(config, "ssh_executable", "/usr/bin/ssh"))
        target = getattr(getattr(config, "remote", None), "target", DEFAULT_RUNTIME_HOST)
        command = [
            str(executable),
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5",
            target,
            remote_command,
        ]
        try:
            completed = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=timeout or float(getattr(config, "ssh_timeout_seconds", 15.0)),
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise LauncherError("runtime_host_unavailable", str(error), exit_code=75) from error
        stdout = completed.stdout.decode("utf-8", errors="replace")
        if completed.returncode != 0:
            detail = completed.stderr.decode("utf-8", errors="replace")[-4096:].strip()
            try:
                _last_json_object(stdout)
            except ValueError:
                pass
            else:
                return stdout
            raise LauncherError(
                "runtime_host_unavailable",
                detail or f"SSH exited with status {completed.returncode}",
                exit_code=75,
            )
        return stdout

    def _ssh_json(self, command: str, *, timeout: float | None = None) -> dict[str, Any]:
        try:
            return _last_json_object(self._ssh(command, timeout=timeout))
        except (UnicodeDecodeError, ValueError) as error:
            raise LauncherError("remote_protocol_invalid", str(error), exit_code=75) from error

    def _remote_python_json(self, source: str, argument: Any, *, timeout: float | None = None) -> dict[str, Any]:
        encoded_source = base64.b64encode(source.encode("utf-8")).decode("ascii")
        encoded_argument = base64.b64encode(
            json.dumps(argument, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        ).decode("ascii")
        python = getattr(getattr(self._require_config(), "remote", None), "python_executable", "python3")
        code = f"import base64;exec(base64.b64decode({encoded_source!r}))"
        command = f"{shlex.quote(python)} -c {shlex.quote(code)} {shlex.quote(encoded_argument)}"
        return self._ssh_json(command, timeout=timeout)

    def _remote_shell(self, body: str, *, timeout: float | None = None) -> str:
        return self._ssh(f"bash -lc {shlex.quote(body)}", timeout=timeout)

    def _remote_runtime_env(self) -> dict[str, str]:
        config = self._require_config()
        return {
            "ROS_DOMAIN_ID": str(getattr(config, "ros_domain", DEFAULT_ROS_DOMAIN)),
            "RMW_IMPLEMENTATION": str(getattr(config, "rmw", DEFAULT_RMW)),
            "ROS_AUTOMATIC_DISCOVERY_RANGE": str(getattr(config, "discovery_range", DEFAULT_DISCOVERY_RANGE)),
            "ROS_STATIC_PEERS": ";".join(getattr(config, "static_peers", DEFAULT_STATIC_PEERS)),
        }

    def _remote_source_prefix(self) -> str:
        config = self._require_config()
        remote = config.remote
        return (
            f"source {shlex.quote(remote.ros_setup_script)} && "
            f"source {shlex.quote(remote.ros_environment_script)} && "
            f"export PYTHONPATH={shlex.quote(remote.source_root)}:$PYTHONPATH && "
        )

    def _remote_native_shell(self, operation: str) -> str:
        config = self._require_config()
        remote = config.remote
        global_args = [
            remote.python_executable, "-m", "manyforge_operator",
            "--state-root", remote.state_root,
            "--instance-id", config.instance_id,
            "--json",
        ]
        if operation == "start":
            args = [
                *global_args, "start", "--profile", "native",
                "--source-root", remote.source_root,
                "--manager-executable", remote.manager_executable,
                "--install-root", remote.install_root,
                "--node-catalog-path", remote.node_catalog_path,
                "--alarm-catalog-path", remote.alarm_catalog_path,
                "--canonical-empty-admission-filename", remote.canonical_empty_admission_filename,
                "--cell-id", remote.cell_id,
                "--advertised-host", remote.advertised_host,
                "--package-aggregate-sha256", remote.package_aggregate_sha256,
                "--compiler-build-manifest-sha256", remote.compiler_build_manifest_sha256,
                "--node-environment-sha256", remote.node_environment_sha256,
                "--readiness-timeout-s", str(config.readiness_timeout_seconds),
                "--poll-interval-s", str(config.poll_interval_seconds),
                "--manager-timeout-s", str(config.manager_timeout_seconds),
                "--simulation-control-config", remote.simulation_control_config,
            ]
            for option, value in (
                ("--expected-source-fingerprint", getattr(config, "expected_source_fingerprint", None)),
                ("--expected-head-commit", getattr(config, "expected_head_commit", None)),
                ("--expected-head-tree", getattr(config, "expected_head_tree", None)),
            ):
                if value:
                    args.extend((option, value))
            if remote.composer_static_dir:
                args.extend(("--composer-static-dir", remote.composer_static_dir))
        else:
            args = [*global_args, operation]
        return self._remote_source_prefix() + shlex.join(args)

    def _remote_boot_id(self) -> str:
        value = self._remote_shell("cat /proc/sys/kernel/random/boot_id", timeout=10).strip()
        if len(value) != 36:
            raise LauncherError("runtime_identity_unavailable", "remote boot identity is invalid", exit_code=75)
        return value

    def _configuration(self, runtime_boot_id: str) -> LaunchConfiguration:
        config = self._require_config()
        source = _source_identity(Path(config.source_root))
        if config.expected_source_fingerprint not in (None, source.fingerprint_sha256):
            raise LauncherError("source_identity_mismatch", "local source fingerprint does not match requested identity", exit_code=71)
        if config.expected_head_commit not in (None, source.head_commit):
            raise LauncherError("source_identity_mismatch", "local source HEAD does not match requested identity", exit_code=71)
        if config.expected_head_tree not in (None, source.head_tree):
            raise LauncherError("source_identity_mismatch", "local source tree does not match requested identity", exit_code=71)
        remote = config.remote
        hosts = [
            HostIdentity("control", HostRole.CONTROL, config.control_host or socket.gethostname(), _boot_id()),
            HostIdentity("runtime", HostRole.RUNTIME, remote.advertised_host, runtime_boot_id),
            HostIdentity("simulator", HostRole.SIMULATOR, config.simulator_host, _boot_id()),
        ]
        simulator_script = _sha256_file(Path(config.isaac_executable))
        world_digest = _sha256_file(Path(config.world_path))
        env_digest = _sha256_file(Path(config.local_ros_environment_script))
        artifacts = [
            RuntimeArtifactIdentity(
                host_id="runtime", kind=ArtifactKind.MOUNTED_INSTALL,
                source_fingerprint=source.fingerprint_sha256,
                package_aggregate_sha256=remote.package_aggregate_sha256,
                compiler_build_manifest_sha256=remote.compiler_build_manifest_sha256,
                node_environment_sha256=remote.node_environment_sha256,
                install_root=remote.install_root,
            ),
            RuntimeArtifactIdentity(
                host_id="simulator", kind=ArtifactKind.MOUNTED_INSTALL,
                source_fingerprint=source.fingerprint_sha256,
                package_aggregate_sha256=world_digest,
                compiler_build_manifest_sha256=simulator_script,
                node_environment_sha256=env_digest,
                install_root=str(Path(config.isaac_executable).parent),
            ),
        ]
        endpoints = [
            ServiceEndpoint(
                service=service, bind_host=SERVICE_BIND_HOSTS[service.value],
                port=SERVICE_PORTS[service.value], advertised_host=remote.advertised_host,
                url=f"http://{remote.advertised_host}:{SERVICE_PORTS[service.value]}/",
            )
            for service in ServiceKind
        ]
        topology = RosTopology(
            domain=config.ros_domain, rmw=config.rmw,
            automatic_discovery_range=config.discovery_range,
            static_peers=list(config.static_peers),
        )
        payload = {
            "schema_version": CONFIGURATION_SCHEMA,
            "profile": LaunchProfile.ISAAC.value,
            "source": source.model_dump(mode="json"),
            "hosts": [host.model_dump(mode="json") for host in hosts],
            "artifacts": [artifact.model_dump(mode="json") for artifact in artifacts],
            "endpoints": [endpoint.model_dump(mode="json") for endpoint in endpoints],
            "ros_topology": topology.model_dump(mode="json"),
        }
        return LaunchConfiguration(
            schema_version=CONFIGURATION_SCHEMA, profile=LaunchProfile.ISAAC,
            source=source, hosts=hosts, artifacts=artifacts, endpoints=endpoints,
            ros_topology=topology, configuration_digest=configuration_digest(payload),
        )

    @staticmethod
    def _local_process_authenticated(meta: Mapping[str, Any]) -> bool:
        try:
            pid = int(meta["pid"])
            observation = _proc_observation(pid)
            if observation is None or observation[0] != int(meta["start_ticks"]):
                return False
            if observation[1] in {"Z", "X"} or _boot_id() != meta["boot_id"]:
                return False
            if os.getpgid(pid) != int(meta["process_group_id"]):
                return False
            observed = _command_line(pid)
            expected = tuple(str(item) for item in meta["argv"])
            return observed is not None and observed[: len(expected)] == expected
        except (KeyError, TypeError, ValueError, ProcessLookupError, PermissionError, OSError):
            return False

    def _spawn_local_simulator(self) -> dict[str, Any]:
        config = self._require_config()
        log_path = self.store.root / "logs" / "isaac_simulator.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        command = (
            f"source {shlex.quote(str(config.isaac_executable.parent / 'setup_ros_env.sh'))} && "
            f"source {shlex.quote(str(config.local_ros_setup_script))} && "
            f"source {shlex.quote(str(config.local_ros_environment_script))} && "
            f"{shlex.quote(str(config.isaac_executable))} --no-ros-env --no-window "
            "--enable_extension isaacsim.ros2.bridge "
            "--enable_extension isaacsim.ros2.sim_control"
        )
        argv = ["bash", "-lc", command]
        log = log_path.open("ab")
        try:
            process = subprocess.Popen(
                argv, cwd=str(Path(config.isaac_executable).parent),
                env=os.environ.copy(), stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            )
        finally:
            log.close()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and _proc_observation(process.pid) is None:
            time.sleep(0.01)
        observation = _proc_observation(process.pid)
        if observation is None:
            self._terminate_local({"pid": process.pid, "process_group_id": process.pid, "argv": argv})
            raise LauncherError("simulator_identity_unavailable", "Isaac PID identity could not be recorded", exit_code=73)
        actual_argv = _command_line(process.pid)
        if actual_argv is None:
            self._terminate_local(
                {
                    "pid": process.pid,
                    "process_group_id": process.pid,
                    "argv": argv,
                    "start_ticks": observation[0],
                    "boot_id": _boot_id(),
                }
            )
            raise LauncherError(
                "simulator_identity_unavailable",
                "Isaac command line could not be recorded",
                exit_code=73,
            )
        return {
            "resource_id": "isaac_simulator", "component_id": "isaac_simulator",
            "kind": ResourceKind.PROCESS.value, "host_id": "simulator",
            "pid": process.pid, "start_ticks": observation[0], "boot_id": _boot_id(),
            "process_group_id": os.getpgid(process.pid), "argv": actual_argv,
            "log_reference": str(log_path), "log_path": str(log_path),
        }

    def _bridge_argv(self) -> list[str]:
        config = self._require_config()
        remote = config.remote
        launch = [
            "ros2", "launch", "manyforge_ros_bringup", "isaac_topic_based_control.launch.py",
            "isaac_profile:=ur10e", f"runtime_namespace:={config.runtime_namespace}",
            f"command_gate_owner_namespace:={config.runtime_namespace}",
            f"world_uri:={config.world_uri}", f"expected_world_name:={config.expected_world_name}",
            f"scene_startup_timeout_s:={min(config.readiness_timeout_seconds, 600.0)}",
            f"scene_service_timeout_s:={min(config.readiness_timeout_seconds, 120.0)}",
        ]
        body = f"source {shlex.quote(remote.ros_setup_script)} && source {shlex.quote(remote.ros_environment_script)} && {shlex.join(launch)}"
        return ["bash", "-lc", body]

    def _spawn_remote_bridge(self) -> dict[str, Any]:
        config = self._require_config()
        remote = config.remote
        state_root = f"{remote.bridge_state_root.rstrip('/')}/{config.instance_id}"
        payload = {
            "resource_id": "isaac_bridge", "state_root": state_root,
            "meta_path": f"{state_root}/bridge.json", "log_path": f"{state_root}/bridge.log",
            "argv": self._bridge_argv(), "environment": self._remote_runtime_env(),
            "cwd": remote.source_root,
        }
        try:
            result = self._remote_python_json(_REMOTE_SPAWN, payload, timeout=20)
        except LauncherError as error:
            self._unrecorded_bridge_meta_path = payload["meta_path"]
            recovered: dict[str, Any] | None = None
            try:
                recovered = self._remote_python_json(
                    _REMOTE_META_READ,
                    payload["meta_path"],
                    timeout=10,
                )
            except LauncherError:
                recovered = None
            if recovered is not None and recovered.get("resource_id") == "isaac_bridge":
                self._unrecorded_bridge_meta_path = None
                result = recovered
            else:
                try:
                    if self._remote_stop_process_by_meta_path(payload["meta_path"]) is True:
                        self._unrecorded_bridge_meta_path = None
                except LauncherError:
                    pass
                raise error
        if result.get("resource_id") != "isaac_bridge" or result.get("pid", 0) <= 0:
            try:
                if self._remote_stop_process_by_meta_path(payload["meta_path"]) is True:
                    self._unrecorded_bridge_meta_path = None
            except LauncherError:
                pass
            raise LauncherError("isaac_bridge_start_failed", "remote bridge did not return an authenticated identity", exit_code=73)
        return result

    def _remote_probe(self, meta: Mapping[str, Any]) -> bool | None:
        try:
            result = self._remote_python_json(_REMOTE_PROBE, dict(meta), timeout=10)
        except LauncherError as error:
            if error.code == "runtime_host_unavailable":
                return None
            raise
        value = result.get("present")
        return value if type(value) is bool else None

    def _remote_stop_process(self, meta: Mapping[str, Any]) -> bool | None:
        try:
            result = self._remote_python_json(_REMOTE_STOP, dict(meta), timeout=20)
        except LauncherError as error:
            if error.code == "runtime_host_unavailable":
                return None
            raise
        return result.get("clean") is True and result.get("present") is False

    def _remote_stop_process_by_meta_path(self, meta_path: str) -> bool | None:
        try:
            result = self._remote_python_json(_REMOTE_META_STOP, meta_path, timeout=20)
        except LauncherError as error:
            if error.code == "runtime_host_unavailable":
                return None
            raise
        return result.get("clean") is True and result.get("present") is False

    def _read_remote_barrier(self, meta: Mapping[str, Any]) -> dict[str, Any] | None:
        path = meta.get("log_path") or meta.get("log_reference")
        if not isinstance(path, str):
            return None
        try:
            result = self._remote_python_json(_REMOTE_BARRIER_READ, path, timeout=10)
        except LauncherError as error:
            if error.code == "runtime_host_unavailable":
                return None
            raise
        return result or None

    def _wait_for_barrier(self, bridge: Mapping[str, Any]) -> dict[str, Any]:
        config = self._require_config()
        deadline = time.monotonic() + config.readiness_timeout_seconds
        while time.monotonic() < deadline:
            present = self._remote_probe(bridge)
            if present is False:
                raise LauncherError("isaac_bridge_exited", "Isaac ROS bridge exited before scene readiness", exit_code=73)
            barrier = self._read_remote_barrier(bridge)
            if barrier is not None and barrier.get("schema_version") == "manyforge.isaac_scene_load_barrier.v1":
                if barrier.get("success") is False:
                    detail = barrier.get("error")
                    raise LauncherError(
                        "isaac_scene_barrier_failed",
                        str(detail) if isinstance(detail, str) and detail else "Isaac scene barrier reported failure",
                        exit_code=73,
                    )
                if barrier.get("world_uri") == config.world_uri and barrier.get("world_name") == config.expected_world_name and barrier.get("simulation_state") == "stopped" and isinstance(barrier.get("required_topics"), list):
                    return barrier
                raise LauncherError("isaac_world_identity_mismatch", "Isaac scene barrier returned a mismatched world or state", exit_code=73)
            time.sleep(config.poll_interval_seconds)
        raise LauncherError("isaac_readiness_timeout", "Isaac scene/bridge readiness timed out", exit_code=73)

    def _remote_native_start(self) -> dict[str, Any]:
        try:
            result = self._ssh_json(f"bash -lc {shlex.quote(self._remote_native_shell('start'))}", timeout=self.config.readiness_timeout_seconds + 30)
        except LauncherError as error:
            try:
                recovered = self._remote_native_status()
            except LauncherError:
                raise error
            if recovered is not None:
                self._remote_status = recovered
            raise error
        status = result.get("status")
        if isinstance(status, dict):
            self._remote_status = status
        if result.get("outcome") not in {"started", "already_running"}:
            error = result.get("error") if isinstance(result.get("error"), dict) else {}
            raise LauncherError(str(error.get("code", "runtime_native_start_failed")), str(error.get("message", "remote native operator start failed")), exit_code=73)
        if not isinstance(status, dict):
            raise LauncherError("remote_protocol_invalid", "remote native start omitted status", exit_code=75)
        return status

    def _remote_native_status(self) -> dict[str, Any] | None:
        result = self._ssh_json(f"bash -lc {shlex.quote(self._remote_native_shell('status'))}", timeout=20)
        if result.get("outcome") == "failed":
            error = result.get("error") if isinstance(result.get("error"), dict) else {}
            if error.get("code") == "instance_unavailable":
                return None
            raise LauncherError(str(error.get("code", "runtime_status_failed")), str(error.get("message", "remote native status failed")), exit_code=75)
        status = result.get("status")
        return status if isinstance(status, dict) else None

    def _remote_native_stop(self) -> dict[str, Any] | None:
        result = self._ssh_json(f"bash -lc {shlex.quote(self._remote_native_shell('stop'))}", timeout=self.config.readiness_timeout_seconds + 30)
        if result.get("outcome") == "failed":
            error = result.get("error") if isinstance(result.get("error"), dict) else {}
            if error.get("code") == "instance_unavailable":
                return None
            raise LauncherError(str(error.get("code", "runtime_stop_failed")), str(error.get("message", "remote native operator stop failed")), exit_code=74)
        status = result.get("status")
        return status if isinstance(status, dict) else None

    def _remote_stop_simulation(self) -> bool | None:
        config = self._require_config()
        remote = config.remote
        source = self._remote_source_prefix() + shlex.join([remote.python_executable, "-c", _REMOTE_SIM_STOP])
        try:
            result = self._ssh_json(f"bash -lc {shlex.quote(source)}", timeout=20)
        except LauncherError as error:
            if error.code == "runtime_host_unavailable":
                return None
            raise
        return result.get("success") is True

    def _resource_observations(self, resources: Sequence[Mapping[str, Any]], remote_status: Mapping[str, Any] | None, bridge: Mapping[str, Any] | None, simulator: Mapping[str, Any] | None) -> list[ResourceObservation]:
        remote_observations = {
            str(row["resource_id"]): ResourceObservation.model_validate(row)
            for row in (remote_status or {}).get("resource_observations", [])
            if isinstance(row, dict) and "resource_id" in row
        }
        values: list[ResourceObservation] = []
        for resource in resources:
            resource_id = str(resource["resource_id"])
            if resource_id in remote_observations:
                values.append(remote_observations[resource_id])
                continue
            if resource_id == "isaac_simulator" and simulator is not None:
                presence = ResourcePresence.PRESENT if self._local_process_authenticated(simulator) else ResourcePresence.ABSENT
            elif resource_id == "isaac_bridge" and bridge is not None:
                probed = self._remote_probe(bridge)
                presence = ResourcePresence.PRESENT if probed is True else ResourcePresence.ABSENT if probed is False else ResourcePresence.UNKNOWN
            else:
                presence = ResourcePresence.UNKNOWN
            values.append(ResourceObservation(resource_id=resource_id, presence=presence, observed_at_unix_ns=_safe_unix_timestamp()))
        return sorted(values, key=lambda row: row.resource_id)

    def _readiness_rows(self, remote_status: Mapping[str, Any] | None, bridge: Mapping[str, Any] | None, simulator: Mapping[str, Any] | None, barrier: Mapping[str, Any] | None) -> list[ReadinessObservation]:
        rows = [ReadinessObservation.model_validate(row) for row in (remote_status or {}).get("readiness_observations", []) if isinstance(row, dict)]
        now = _safe_unix_timestamp()
        sim_present = simulator is not None and self._local_process_authenticated(simulator)
        rows.append(ReadinessObservation(
            component_id="isaac_simulator", authority=ReadinessAuthority.EXTERNAL_OBSERVATION,
            state=ReadinessState.READY if sim_present else ReadinessState.UNAVAILABLE,
            source_schema="manyforge.isaac_simulator_process.v1", owner_id="isaac-sim",
            payload_sha256=_sha256_json({"present": sim_present}), observed_at_unix_ns=now,
            reason_code=None if sim_present else "sim_stop_unverified" if self._sim_stop_unverified else "simulator_unavailable",
        ))
        barrier_ok = barrier is not None and barrier.get("world_uri") == self._require_config().world_uri and barrier.get("simulation_state") == "stopped"
        bridge_present = bridge is not None and self._remote_probe(bridge) is True
        rows.append(ReadinessObservation(
            component_id="isaac_scene_barrier", authority=ReadinessAuthority.OWNER_ISSUED,
            state=ReadinessState.READY if bridge_present and barrier_ok else ReadinessState.DEGRADED if bridge_present else ReadinessState.UNAVAILABLE,
            source_schema="manyforge.isaac_scene_load_barrier.v1", owner_id="manyforge_isaac_scene_load_barrier",
            payload_sha256=_sha256_json(barrier or {"present": bridge_present}), observed_at_unix_ns=now,
            reason_code=None if bridge_present and barrier_ok else "isaac_scene_unready",
        ))
        return sorted(rows, key=lambda row: (row.component_id, row.authority, row.source_schema))

    def _snapshot(self, configuration: LaunchConfiguration, old_status: LaunchInstanceStatus | None, mechanical: Mapping[str, Any], *, remote_status: Mapping[str, Any] | None, barrier: Mapping[str, Any] | None, state_override: LaunchInstanceState | None = None, failure: FailureRecord | None = None) -> LaunchInstanceStatus:
        old_resources = [resource.model_dump(mode="json") for resource in old_status.owned_resources] if old_status else []
        native_resources = [dict(resource) for resource in (remote_status or {}).get("owned_resources", []) if isinstance(resource, dict)]
        if remote_status is None:
            native_resources = [row for row in old_resources if row.get("resource_id") not in {"isaac_simulator", "isaac_bridge"}]
        local_resources = [dict(item) for item in self._processes.values()]
        resources_by_id = {str(item["resource_id"]): item for item in native_resources + local_resources}
        resources = [resources_by_id[key] for key in sorted(resources_by_id)]
        bridge = resources_by_id.get("isaac_bridge")
        simulator = resources_by_id.get("isaac_simulator")
        observations = self._resource_observations(resources, remote_status, bridge, simulator)
        readiness = self._readiness_rows(remote_status, bridge, simulator, barrier)
        native_ready = remote_status is not None and remote_status.get("state") == LaunchInstanceState.READY.value
        all_present = bool(resources) and all(row.presence is ResourcePresence.PRESENT for row in observations)
        all_ready = bool(readiness) and all(row.state is ReadinessState.READY for row in readiness)
        if state_override is not None:
            state = state_override
        elif all_present and native_ready and all_ready:
            state = LaunchInstanceState.READY
        elif not resources and remote_status is not None and remote_status.get("state") == LaunchInstanceState.STOPPED.value:
            state = LaunchInstanceState.STOPPED
        else:
            state = LaunchInstanceState.DEGRADED
        if state is LaunchInstanceState.READY:
            failure = None
        elif failure is None:
            reason = "isaac_profile_degraded"
            if remote_status is None:
                reason = "runtime_host_unavailable"
            elif not native_ready:
                reason = "native_runtime_unready"
            elif not all_ready:
                reason = "isaac_readiness_unavailable"
            failure = FailureRecord(reason_code=reason, component_id="isaac", detail="split Isaac launch did not prove all owned resources and owner readiness ready", occurred_at_unix_ns=_safe_unix_timestamp())
        generation = old_status.record_generation + 1 if old_status else 1
        launch_id = old_status.launch_instance_id if old_status else getattr(self._require_config(), "instance_id", "isaac")
        return _status(configuration, launch_id, state, resources, readiness, observations, generation, failure)

    def _mechanical(self, barrier: Mapping[str, Any] | None = None) -> dict[str, Any]:
        config = self._require_config()
        launch_input_digest = (
            config.input_digest()
            if hasattr(config, "input_digest")
            else self._persisted_launch_input_digest or ""
        )
        return {
            "profile": "isaac",
            "launch_input_digest": launch_input_digest,
            "config": config.record() if hasattr(config, "record") else _namespace_to_dict(config),
            "processes": dict(self._processes),
            "native_status": self._remote_status,
            "barrier": barrier,
            "unrecorded_bridge_meta_path": self._unrecorded_bridge_meta_path,
        }

    def _write(self, status: LaunchInstanceStatus, mechanical: Mapping[str, Any]) -> None:
        self.store.write(status, mechanical)

    def _cleanup(self, mechanical: Mapping[str, Any]) -> bool:
        processes = mechanical.get("processes", {})
        bridge = processes.get("isaac_bridge") if isinstance(processes, dict) else None
        simulator = processes.get("isaac_simulator") if isinstance(processes, dict) else None
        orphan_bridge_meta_path = mechanical.get("unrecorded_bridge_meta_path")
        if not isinstance(orphan_bridge_meta_path, str):
            orphan_bridge_meta_path = self._unrecorded_bridge_meta_path
        native_status = mechanical.get("native_status")
        if self._remote_status is None and isinstance(native_status, dict):
            self._remote_status = dict(native_status)
        native_started = isinstance(self._remote_status, dict) and (
            bool(self._remote_status.get("owned_resources"))
            or self._remote_status.get("state")
            in {
                LaunchInstanceState.STARTING.value,
                LaunchInstanceState.READY.value,
                LaunchInstanceState.DEGRADED.value,
                LaunchInstanceState.STOPPING.value,
            }
        )
        self._sim_stop_unverified = False
        clean = True
        simulator_present = simulator is not None and self._local_process_authenticated(simulator)
        if bridge is not None or orphan_bridge_meta_path is not None:
            if simulator_present:
                simulation_stopped = self._remote_stop_simulation()
            else:
                self._sim_stop_unverified = True
                simulation_stopped = True
            if simulation_stopped is not True:
                clean = False
        if bridge is not None:
            remote_clean = self._remote_stop_process(bridge)
            if remote_clean is not True:
                clean = False
        if orphan_bridge_meta_path is not None:
            orphan_clean = self._remote_stop_process_by_meta_path(orphan_bridge_meta_path)
            if orphan_clean is not True:
                clean = False
            else:
                self._unrecorded_bridge_meta_path = None
        if native_started:
            remote_native = self._remote_native_stop()
            if remote_native is None:
                clean = False
            else:
                self._remote_status = remote_native
        self._terminate_local(simulator)
        if simulator is not None and self._local_process_authenticated(simulator):
            clean = False
        return clean

    def _assert_stopped_record_absent(
        self,
        old_status: LaunchInstanceStatus,
        mechanical: Mapping[str, Any],
    ) -> None:
        """Re-probe every recorded local and remote resource before idempotence."""

        processes = mechanical.get("processes", {})
        if not isinstance(processes, dict):
            raise LauncherError(
                "status_record_invalid",
                "stored Isaac mechanical process record is not an object",
                exit_code=70,
                status=old_status,
            )
        self._processes = dict(processes)
        for resource_id in ("isaac_simulator",):
            process = self._processes.get(resource_id)
            if process is not None and self._local_process_authenticated(process):
                raise LauncherError(
                    "stale_launch_record",
                    f"stopped Isaac launch still owns {resource_id}",
                    exit_code=70,
                    status=old_status,
                )
        bridge = self._processes.get("isaac_bridge")
        if bridge is not None:
            present = self._remote_probe(bridge)
            if present is None:
                raise LauncherError(
                    "runtime_host_unavailable",
                    "cannot prove the stopped Isaac bridge is absent",
                    exit_code=75,
                    status=old_status,
                )
            if present:
                raise LauncherError(
                    "stale_launch_record",
                    "stopped Isaac launch still owns isaac_bridge",
                    exit_code=70,
                    status=old_status,
                )
        remote_status = self._remote_native_status()
        if remote_status is None:
            return
        remote_observations = {
            str(row.get("resource_id")): row
            for row in remote_status.get("resource_observations", [])
            if isinstance(row, dict) and row.get("resource_id") is not None
        }
        remote_resource_ids = {
            str(row.get("resource_id"))
            for row in remote_status.get("owned_resources", [])
            if isinstance(row, dict) and row.get("resource_id") is not None
        }
        if (
            remote_status.get("state") != LaunchInstanceState.STOPPED.value
            or any(
                resource_id not in remote_observations
                or remote_observations[resource_id].get("presence")
                != ResourcePresence.ABSENT.value
                for resource_id in remote_resource_ids
            )
            or any(
                row.get("presence") != ResourcePresence.ABSENT.value
                for row in remote_observations.values()
            )
        ):
            raise LauncherError(
                "stale_launch_record",
                "stopped Isaac launch has a remote native resource that is not proven absent",
                exit_code=70,
                status=old_status,
            )

    def start(self) -> LaunchInstanceStatus:
        if not isinstance(self.config, IsaacLauncherConfig):
            raise LauncherError("operator_configuration_invalid", "Isaac start requires complete profile inputs", exit_code=64)
        self.last_start_idempotent = False
        with self.store.lock():
            existing = self.store.read()
            if existing is not None:
                old_status, mechanical = existing
                if old_status.configuration.profile is not LaunchProfile.ISAAC:
                    raise LauncherError("incompatible_owner", "stored launch belongs to another profile", exit_code=70, status=old_status)
                if mechanical.get("launch_input_digest") != self.config.input_digest():
                    raise LauncherError("incompatible_owner", "stored Isaac launch has different split configuration", exit_code=70, status=old_status)
                if old_status.state in {LaunchInstanceState.STARTING, LaunchInstanceState.READY}:
                    self.last_start_idempotent = True
                    self._processes = dict(mechanical.get("processes", {}))
                    self._remote_status = self._remote_native_status()
                    barrier = self._read_remote_barrier(self._processes.get("isaac_bridge", {})) if self._processes.get("isaac_bridge") else mechanical.get("barrier")
                    observed = self._snapshot(old_status.configuration, old_status, mechanical, remote_status=self._remote_status, barrier=barrier)
                    self._write(observed, self._mechanical(barrier))
                    return observed
                if old_status.state is LaunchInstanceState.DEGRADED:
                    self._processes = dict(mechanical.get("processes", {}))
                    self._remote_status = (
                        dict(mechanical["native_status"])
                        if isinstance(mechanical.get("native_status"), dict)
                        else None
                    )
                    stopping = self._snapshot(
                        old_status.configuration,
                        old_status,
                        mechanical,
                        remote_status=self._remote_status,
                        barrier=mechanical.get("barrier"),
                        state_override=LaunchInstanceState.STOPPING,
                    )
                    self._write(stopping, self._mechanical())
                    try:
                        clean = self._cleanup(mechanical)
                    except LauncherError:
                        clean = False
                    if not clean:
                        failure = FailureRecord(
                            reason_code="cleanup_deadline_exceeded",
                            component_id="isaac",
                            detail="degraded Isaac recovery could not prove prior owned resources absent",
                            occurred_at_unix_ns=_safe_unix_timestamp(),
                        )
                        degraded = self._snapshot(
                            old_status.configuration,
                            stopping,
                            mechanical,
                            remote_status=self._remote_status,
                            barrier=None,
                            state_override=LaunchInstanceState.DEGRADED,
                            failure=failure,
                        )
                        self._write(degraded, self._mechanical())
                        raise LauncherError(
                            "cleanup_deadline_exceeded",
                            str(failure.detail),
                            exit_code=74,
                            status=degraded,
                        )
                    stopped = self._snapshot(
                        old_status.configuration,
                        stopping,
                        mechanical,
                        remote_status=self._remote_status,
                        barrier=None,
                        state_override=LaunchInstanceState.STOPPED,
                    )
                    self._write(stopped, self._mechanical())
                    self._processes = {}
                    self._remote_status = None
                if old_status.state is LaunchInstanceState.STOPPING:
                    raise LauncherError("incompatible_owner", "stored Isaac launch is stopping", exit_code=70, status=old_status)
                if any(row.presence is not ResourcePresence.ABSENT for row in old_status.resource_observations):
                    raise LauncherError("stale_launch_record", "stopped Isaac record is not proven absent", exit_code=70, status=old_status)
            runtime_boot_id = self._remote_boot_id()
            configuration = self._configuration(runtime_boot_id)
            self._processes = {}
            starting = self._snapshot(configuration, None, self._mechanical(), remote_status=None, barrier=None, state_override=LaunchInstanceState.STARTING)
            self._write(starting, self._mechanical())
            try:
                simulator = self._spawn_local_simulator()
                self._processes["isaac_simulator"] = simulator
                self._write(self._snapshot(configuration, starting, self._mechanical(), remote_status=None, barrier=None, state_override=LaunchInstanceState.STARTING), self._mechanical())
                bridge = self._spawn_remote_bridge()
                self._processes["isaac_bridge"] = bridge
                self._write(self._snapshot(configuration, starting, self._mechanical(), remote_status=None, barrier=None, state_override=LaunchInstanceState.STARTING), self._mechanical())
                barrier = self._wait_for_barrier(bridge)
                self._remote_status = self._remote_native_start()
                remote_config = LaunchConfiguration.model_validate(self._remote_status["configuration"])
                if remote_config.source.fingerprint_sha256 != configuration.source.fingerprint_sha256:
                    raise LauncherError("source_identity_mismatch", "remote native source differs from local Isaac source", exit_code=71)
                final = self._snapshot(configuration, starting, self._mechanical(barrier), remote_status=self._remote_status, barrier=barrier)
                if final.state is not LaunchInstanceState.READY:
                    raise LauncherError("isaac_readiness_timeout", "split Isaac profile did not reach ready", exit_code=73, status=final)
                self._write(final, self._mechanical(barrier))
                return final
            except LauncherError as error:
                try:
                    clean = self._cleanup(self._mechanical())
                except LauncherError:
                    clean = False
                if clean:
                    stopped = self._snapshot(configuration, starting, self._mechanical(), remote_status=None, barrier=None, state_override=LaunchInstanceState.STOPPED, failure=FailureRecord(reason_code=error.code, component_id="isaac", detail=str(error)[:4096], occurred_at_unix_ns=_safe_unix_timestamp()))
                    self._write(stopped, self._mechanical())
                    error.status = stopped
                else:
                    degraded = self._snapshot(configuration, starting, self._mechanical(), remote_status=self._remote_status, barrier=None, failure=FailureRecord(reason_code="cleanup_deadline_exceeded", component_id="isaac", detail="Isaac startup failed and exact cleanup was not proven", occurred_at_unix_ns=_safe_unix_timestamp()))
                    self._write(degraded, self._mechanical())
                    error.status = degraded
                raise
            except Exception as error:
                failure = LauncherError("isaac_start_failed", str(error), exit_code=70)
                try:
                    clean = self._cleanup(self._mechanical())
                except LauncherError:
                    clean = False
                final = self._snapshot(configuration, starting, self._mechanical(), remote_status=self._remote_status, barrier=None, state_override=LaunchInstanceState.STOPPED if clean else LaunchInstanceState.DEGRADED, failure=FailureRecord(reason_code="isaac_start_failed" if clean else "cleanup_deadline_exceeded", component_id="isaac", detail=str(error)[:4096], occurred_at_unix_ns=_safe_unix_timestamp()))
                self._write(final, self._mechanical())
                failure.status = final
                raise failure from error

    def status(self) -> LaunchInstanceStatus | None:
        with self.store.lock():
            existing = self.store.read()
            if existing is None:
                return None
            old_status, mechanical = existing
            if old_status.state is LaunchInstanceState.STOPPED:
                self._assert_stopped_record_absent(old_status, mechanical)
                return old_status
            self._processes = dict(mechanical.get("processes", {}))
            try:
                self._remote_status = self._remote_native_status()
            except LauncherError as error:
                if error.code != "runtime_host_unavailable":
                    raise
                self._remote_status = None
            bridge = self._processes.get("isaac_bridge")
            barrier = self._read_remote_barrier(bridge) if bridge else mechanical.get("barrier")
            observed = self._snapshot(old_status.configuration, old_status, mechanical, remote_status=self._remote_status, barrier=barrier)
            if not can_transition(old_status.state, observed.state) and old_status.state != observed.state:
                raise LauncherError("invalid_state_transition", f"cannot transition {old_status.state.value} to {observed.state.value}", exit_code=70, status=old_status)
            self._write(observed, self._mechanical(barrier))
            return observed

    def stop(self) -> LaunchInstanceStatus | None:
        self.last_stop_already_stopped = False
        with self.store.lock():
            existing = self.store.read()
            if existing is None:
                return None
            old_status, mechanical = existing
            if old_status.state is LaunchInstanceState.STOPPED:
                self._assert_stopped_record_absent(old_status, mechanical)
                self.last_stop_already_stopped = True
                return old_status
            if not can_transition(old_status.state, LaunchInstanceState.STOPPING):
                raise LauncherError("invalid_state_transition", f"cannot transition {old_status.state.value} to stopping", exit_code=70, status=old_status)
            self._processes = dict(mechanical.get("processes", {}))
            self._remote_status = mechanical.get("native_status")
            stopping = self._snapshot(old_status.configuration, old_status, mechanical, remote_status=self._remote_status, barrier=mechanical.get("barrier"), state_override=LaunchInstanceState.STOPPING)
            self._write(stopping, self._mechanical())
            try:
                clean = self._cleanup(mechanical)
            except LauncherError:
                clean = False
            remote_status = None
            try:
                remote_status = self._remote_native_status()
            except LauncherError:
                remote_status = None
            final = self._snapshot(old_status.configuration, stopping, mechanical, remote_status=remote_status, barrier=None, state_override=LaunchInstanceState.STOPPED if clean else LaunchInstanceState.DEGRADED, failure=None if clean else FailureRecord(reason_code="cleanup_deadline_exceeded", component_id="isaac", detail="Isaac stop did not prove every owned resource absent", occurred_at_unix_ns=_safe_unix_timestamp()))
            self._write(final, self._mechanical())
            if final.state is LaunchInstanceState.STOPPED:
                return final
            raise LauncherError("cleanup_deadline_exceeded", "Isaac stop did not prove every owned resource absent", exit_code=74, status=final)

    def _terminate_local(self, meta: Mapping[str, Any] | None) -> None:
        if not meta:
            return
        try:
            if not self._local_process_authenticated(meta):
                return
            pgid = int(meta["process_group_id"])
            if pgid <= 1 or pgid == os.getpgrp():
                return
            os.killpg(pgid, signal.SIGINT)
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline and self._local_process_authenticated(meta):
                time.sleep(0.05)
            if self._local_process_authenticated(meta):
                os.killpg(pgid, signal.SIGTERM)
                time.sleep(0.2)
            if self._local_process_authenticated(meta):
                os.killpg(pgid, signal.SIGKILL)
        except (KeyError, TypeError, ValueError, ProcessLookupError, PermissionError, OSError):
            return


__all__ = ["DEFAULT_WORLD_NAME", "IsaacLauncher", "IsaacLauncherConfig", "RemoteNativeConfig"]
