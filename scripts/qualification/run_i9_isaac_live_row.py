#!/usr/bin/env python3
"""Run one bounded source-matched I9 Isaac scenario on the Orin worker.

Isaac Sim is an already-started, separately owned laptop process.  This runner
owns only the scoped Orin-side Manager, Headless, Program, Composer (assistant
row only), ROS control launch, and observation clients.  Robot commands flow
exclusively through the Applied Program and the addressed Kernel owner.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping, Sequence
from contextlib import suppress
from dataclasses import dataclass, field
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from typing import Any, Final
from urllib.request import urlopen

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient

from run_i9_assistant_snapshot_probe import run_probe as run_snapshot_probe
from run_i9_isaac_matrix import (
    ROW_SCHEMA,
    SCENARIO_BY_ID,
    TOPIC_TYPE_BY_NAME,
    ScenarioSpec,
    QualificationError,
    validate_dds_environment,
    validate_row,
)


_MAX_LOG_BYTES: Final = 64 * 1024 * 1024
_MAX_SAMPLE_BYTES: Final = 2 * 1024 * 1024
_MAX_COMMAND_BYTES: Final = 4 * 1024 * 1024
_SHA256_PREFIX: Final = "sha256:"
_TERMINAL_STATES: Final = frozenset({"succeeded", "failed", "stopped"})
_OBJECT_MOTION_RESOURCE: Final = "isaac_cheese_box"
_OBJECT_MOTION_MINIMUM_M: Final = 0.05
_GRIPPER_TOLERANCE_BY_PROFILE: Final = {"lite6": 0.004, "ur10e": 0.04}
_RECOVERY_BOUND_S: Final = 60.0
_SCENE_POSE_QUERY_ATTEMPTS: Final = 16
_CCLC_STARTUP_SCHEMA: Final = "manyforge.i9_cclc_startup.v1"
_ROS2_CONTROL_LIFECYCLE_OBSERVATION_SCHEMA: Final = (
    "manyforge.ros2_control_lifecycle_observation.v1"
)
_ROS2_CONTROL_LIFECYCLE_READY_FACTS: Final = {
    "external.controller_configuration_ready": True,
    "external.fault_present": False,
    "external.transport_ready": True,
}
# The launch barrier owns two fresh, 30-second-bounded service calls to force
# and read back STOPPED even after its startup deadline expires.
_SCENE_BARRIER_CLEANUP_MARGIN_S: Final = 65.0


class LiveRowError(RuntimeError):
    """One live orchestration, product, or observation failure."""


@dataclass(frozen=True, slots=True)
class LiveConfig:
    spec: ScenarioSpec
    source_root: Path
    package_root: Path
    installed_root: Path
    run_root: Path
    node_catalog: Path
    alarm_catalog: Path
    static_dir: Path
    startup_timeout_s: float
    run_timeout_s: float
    composer_port: int | None


@dataclass(slots=True)
class ManagedProcess:
    name: str
    process: subprocess.Popen[bytes]
    log_path: Path
    log_stream: Any

    def require_alive(self) -> None:
        code = self.process.poll()
        if code is not None:
            raise LiveRowError(
                f"{self.name} exited with {code}: {_log_tail(self.log_path)}"
            )

    def stop(self) -> None:
        if self.process.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=8.0)
            except subprocess.TimeoutExpired:
                with suppress(ProcessLookupError):
                    os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=5.0)
        self.log_stream.close()


@dataclass(slots=True)
class TopicSample:
    topic: str
    process: subprocess.Popen[bytes]

    def settle(self, timeout_s: float = 5.0) -> dict[str, Any]:
        try:
            stdout, _stderr = self.process.communicate(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            _signal_process_group(self.process, signal.SIGTERM)
            try:
                stdout, _stderr = self.process.communicate(timeout=2.0)
            except subprocess.TimeoutExpired:
                _signal_process_group(self.process, signal.SIGKILL)
                stdout, _stderr = self.process.communicate(timeout=2.0)
        if len(stdout) > _MAX_SAMPLE_BYTES:
            raise LiveRowError(f"ROS sample exceeded its byte bound: {self.topic}")
        return {
            "bytes": len(stdout),
            "returncode": self.process.returncode,
            "sha256": _digest(stdout),
            "seen": self.process.returncode == 0 and bool(stdout.strip()),
        }

    def stop(self) -> None:
        if self.process.poll() is None:
            _signal_process_group(self.process, signal.SIGTERM)
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                _signal_process_group(self.process, signal.SIGKILL)
                self.process.wait(timeout=2.0)


@dataclass(frozen=True, order=True, slots=True)
class GripperTarget:
    target_role_id: str
    coordinate_kind: str
    value: float

    @property
    def value_field(self) -> str:
        return "value_m" if self.coordinate_kind == "linear_m" else "value_rad"


@dataclass(slots=True)
class RunEvidence:
    gripper_targets: tuple[GripperTarget, ...]
    execution_samples: int = 0
    execution_owner_epochs: set[str] = field(default_factory=set)
    execution_cell_ids: set[str] = field(default_factory=set)
    execution_component_ids: set[str] = field(default_factory=set)
    gripper_hits: set[GripperTarget] = field(default_factory=set)
    object_initial_position: tuple[float, float, float] | None = None
    object_max_displacement_m: float = 0.0
    object_pose_samples: int = 0
    object_carried_observed: bool = False
    pose_query_rejections: int = 0
    foundationpose_source_sequences: set[int] = field(default_factory=set)
    foundationpose_source_identities: set[tuple[str, int]] = field(
        default_factory=set
    )
    foundationpose_provider_sequences: set[int] = field(default_factory=set)
    foundationpose_provider_match: bool = False
    scene_projection_generations: set[int] = field(default_factory=set)

    def summary(self) -> dict[str, Any]:
        source_sequences = sorted(self.foundationpose_source_sequences)
        provider_sequences = sorted(self.foundationpose_provider_sequences)
        return {
            "execution": {
                "cell_ids": sorted(self.execution_cell_ids),
                "component_ids": sorted(self.execution_component_ids),
                "owner_epochs": sorted(self.execution_owner_epochs),
                "samples": self.execution_samples,
            },
            "foundationpose": {
                "provider_match": self.foundationpose_provider_match,
                "provider_sequences": provider_sequences[-8:],
                "source_sequence_max": (
                    source_sequences[-1] if source_sequences else None
                ),
                "source_sequence_min": (
                    source_sequences[0] if source_sequences else None
                ),
                "source_sequence_samples": len(source_sequences),
            },
            "gripper": {
                "hit_count": len(self.gripper_hits),
                "target_count": len(self.gripper_targets),
                "targets_digest": _digest(
                    _json_bytes(
                        [
                            {
                                "coordinate_kind": target.coordinate_kind,
                                "target_role_id": target.target_role_id,
                                "value": target.value,
                            }
                            for target in self.gripper_targets
                        ]
                    )
                ),
            },
            "object_motion": {
                "carried_observed": self.object_carried_observed,
                "max_displacement_m": round(self.object_max_displacement_m, 6),
                "pose_query_rejections": self.pose_query_rejections,
                "pose_samples": self.object_pose_samples,
            },
            "scene_projection_generation_max": max(
                self.scene_projection_generations or {-1}
            ),
            "scene_projection_generation_min": min(
                self.scene_projection_generations or {-1}
            ),
        }


@dataclass(frozen=True, slots=True)
class RecoveryEvidence:
    component_id: str
    provider_epoch_before: str
    provider_sequence_before: int
    provider_epoch_after: str
    provider_sequence_after: int
    dds_loss_elapsed_s: float
    dds_rejoin_elapsed_s: float
    provider_loss_elapsed_s: float
    provider_recovery_elapsed_s: float
    output_sample: Mapping[str, Any]
    exact_world_after_rejoin: bool
    simulation_stopped_during_outage: bool
    simulation_stopped_after_rejoin: bool

    def to_dict(self) -> dict[str, Any]:
        passed = bool(
            self.provider_epoch_before != self.provider_epoch_after
            and self.provider_sequence_before > 0
            and self.provider_sequence_after > 0
            and 0.0 <= self.dds_loss_elapsed_s <= _RECOVERY_BOUND_S
            and 0.0 <= self.dds_rejoin_elapsed_s <= _RECOVERY_BOUND_S
            and 0.0 <= self.provider_loss_elapsed_s <= _RECOVERY_BOUND_S
            and 0.0 <= self.provider_recovery_elapsed_s <= _RECOVERY_BOUND_S
            and self.output_sample.get("seen") is True
            and self.exact_world_after_rejoin
            and self.simulation_stopped_during_outage
            and self.simulation_stopped_after_rejoin
        )
        return {
            "schema_version": "manyforge.i9_isaac_recovery.v1",
            "component_id": self.component_id,
            "dds": {
                "loss_elapsed_s": round(self.dds_loss_elapsed_s, 3),
                "rejoin_elapsed_s": round(self.dds_rejoin_elapsed_s, 3),
            },
            "exact_world_after_rejoin": self.exact_world_after_rejoin,
            "output_sample": dict(self.output_sample),
            "passed": passed,
            "provider": {
                "epoch_after": self.provider_epoch_after,
                "epoch_before": self.provider_epoch_before,
                "loss_elapsed_s": round(self.provider_loss_elapsed_s, 3),
                "recovery_elapsed_s": round(
                    self.provider_recovery_elapsed_s, 3
                ),
                "sequence_after": self.provider_sequence_after,
                "sequence_before": self.provider_sequence_before,
                "source_id": "foundationpose_trocar",
            },
            "simulation_stopped_after_rejoin": (
                self.simulation_stopped_after_rejoin
            ),
            "simulation_stopped_during_outage": (
                self.simulation_stopped_during_outage
            ),
        }


def _signal_process_group(
    process: subprocess.Popen[bytes], signal_number: signal.Signals
) -> None:
    with suppress(ProcessLookupError):
        os.killpg(process.pid, signal_number)


def _bounded_positive(value: float, *, label: str, maximum: float) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        or not 0.1 <= float(value) <= maximum
    ):
        raise LiveRowError(f"{label} must be finite and in [0.1, {maximum}]")
    return float(value)


def _plain_directory(path: Path, *, label: str, create: bool = False) -> Path:
    if create:
        path.mkdir(parents=True, exist_ok=True)
    if not path.is_absolute() or not path.is_dir() or path.is_symlink():
        raise LiveRowError(f"{label} must be one absolute non-symlink directory")
    return path


def _plain_file(path: Path, *, label: str, executable: bool = False) -> Path:
    if not path.is_absolute() or not path.is_file() or path.is_symlink():
        raise LiveRowError(f"{label} must be one absolute regular file")
    if executable and not os.access(path, os.X_OK):
        raise LiveRowError(f"{label} must be executable")
    return path


def validate_config(config: LiveConfig) -> LiveConfig:
    _plain_directory(config.source_root, label="source root")
    _plain_directory(config.package_root, label="package root")
    _plain_directory(config.installed_root, label="installed target root")
    _plain_directory(config.run_root, label="run root", create=True)
    if any(config.run_root.iterdir()):
        raise LiveRowError("run root must be empty for one owner epoch")
    _plain_file(config.node_catalog, label="node catalog")
    _plain_file(config.alarm_catalog, label="alarm catalog")
    _plain_directory(config.static_dir, label="Composer static directory")
    for executable in (
        "manyforge_manager_daemon",
        "manyforge_roboplan_candidate_worker",
        "manyforge_ros2_control_execution_host",
    ):
        _plain_file(
            config.installed_root / executable,
            label=f"installed {executable}",
            executable=True,
        )
    _bounded_positive(
        config.startup_timeout_s, label="startup timeout", maximum=600.0
    )
    _bounded_positive(config.run_timeout_s, label="run timeout", maximum=900.0)
    if config.composer_port is not None and not 1024 <= config.composer_port <= 65535:
        raise LiveRowError("Composer port must be in [1024, 65535]")
    validate_dds_environment(os.environ)
    if shutil.which("ros2") is None:
        raise LiveRowError("ros2 is absent; source the exact Jazzy overlay first")
    return config


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _start_process(
    name: str,
    command: Sequence[str],
    *,
    environment: Mapping[str, str],
    run_root: Path,
) -> ManagedProcess:
    if not command or any(not isinstance(item, str) or not item for item in command):
        raise LiveRowError(f"{name} command is invalid")
    log_path = run_root / f"{name}.log"
    log = log_path.open("xb")
    try:
        process = subprocess.Popen(
            list(command),
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=dict(environment),
            start_new_session=True,
        )
    except Exception:
        log.close()
        raise
    return ManagedProcess(name, process, log_path, log)


def _run_bounded(
    command: Sequence[str],
    *,
    environment: Mapping[str, str],
    timeout_s: float = 10.0,
) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            list(command),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(environment),
            check=False,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as error:
        raise LiveRowError(f"bounded command timed out: {command[0]}") from error
    output = completed.stdout
    if len(output) > _MAX_COMMAND_BYTES:
        raise LiveRowError(f"bounded command output is oversized: {command[0]}")
    return {
        "bytes": len(output),
        "output": output.decode("utf-8", errors="replace"),
        "returncode": completed.returncode,
        "sha256": _digest(output),
    }


def _start_topic_sample(
    topic: str,
    *,
    environment: Mapping[str, str],
) -> TopicSample:
    process = subprocess.Popen(
        [
            "ros2",
            "topic",
            "echo",
            topic,
            TOPIC_TYPE_BY_NAME[topic],
            "--once",
            "--no-daemon",
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=dict(environment),
        start_new_session=True,
    )
    return TopicSample(topic, process)


def _wait_for_api(
    base_url: str,
    scenario_id: str,
    processes: Sequence[ManagedProcess],
    *,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for process in processes:
            process.require_alive()
        try:
            with HeadlessHttpClient(base_url, timeout_seconds=1.0) as client:
                runtime = client.runtime()
                catalog = client.scenarios()
            if runtime.get("complete") is True and scenario_id in {
                row.get("scenario_id")
                for row in catalog.get("scenarios", [])
                if isinstance(row, dict)
            }:
                return runtime
        except Exception as error:
            last_error = error
        time.sleep(0.05)
    raise LiveRowError(f"Headless API did not become complete: {last_error}")


def _wait_for_composer(
    base_url: str,
    process: ManagedProcess,
    *,
    timeout_s: float,
) -> None:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        process.require_alive()
        try:
            with urlopen(  # noqa: S310
                f"{base_url}/api/scenarios", timeout=1.0
            ) as response:
                value = json.loads(response.read())
            if value.get("api_version") == "manyforge.scenario_catalog.v1":
                return
        except Exception as error:
            last_error = error
        time.sleep(0.05)
    raise LiveRowError(f"Composer did not become ready: {last_error}")


def _log_tail(path: Path, maximum: int = 12_000) -> str:
    try:
        size = path.stat().st_size
        if size > _MAX_LOG_BYTES:
            return "<log exceeded its qualification bound>"
        with path.open("rb") as stream:
            stream.seek(max(0, size - maximum))
            return stream.read(maximum).decode("utf-8", errors="replace")
    except OSError as error:
        return f"<unable to read log: {error}>"


def _wait_for_barrier(
    launch: ManagedProcess,
    spec: ScenarioSpec,
    *,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        launch.require_alive()
        text = _log_tail(launch.log_path, maximum=256 * 1024)
        for line in reversed(text.splitlines()):
            start = line.find("{")
            if start < 0 or "manyforge.isaac_scene_load_barrier.v1" not in line:
                continue
            try:
                value = json.loads(line[start:])
            except json.JSONDecodeError:
                continue
            if value.get("success") is False:
                raise LiveRowError(f"Isaac scene barrier failed: {value}")
            if (
                value.get("schema_version")
                == "manyforge.isaac_scene_load_barrier.v1"
                and value.get("world_name") == spec.world_name
                and value.get("world_uri") == spec.world_uri
                and value.get("simulation_state") == "stopped"
            ):
                return value
        time.sleep(0.05)
    raise LiveRowError(
        "Isaac scene barrier did not settle: " + _log_tail(launch.log_path)
    )


def _wait_for_controllers(
    launch: ManagedProcess,
    spec: ScenarioSpec,
    *,
    environment: Mapping[str, str],
    timeout_s: float,
) -> dict[str, Any]:
    controller_manager = f"/manyforge/{spec.component_id}/controller_manager"
    deadline = time.monotonic() + timeout_s
    observed: dict[str, Any] = {}
    while time.monotonic() < deadline:
        launch.require_alive()
        observed = _run_bounded(
            [
                "ros2",
                "control",
                "list_controllers",
                "--controller-manager",
                controller_manager,
            ],
            environment=environment,
            timeout_s=8.0,
        )
        output = observed["output"]
        if (
            observed["returncode"] == 0
            and "isaac_arm_controller" in output
            and "joint_state_broadcaster" in output
            and output.count(" active") >= 2
        ):
            return observed
        time.sleep(0.1)
    raise LiveRowError(
        "Isaac controllers did not become active: "
        + json.dumps(_summary(observed), separators=(",", ":"), sort_keys=True)
    )


def _simulation_snapshot(
    spec: ScenarioSpec,
    *,
    play: bool = False,
    reset: bool = False,
    stop: bool = False,
) -> dict[str, Any]:
    import rclpy
    from rclpy.node import Node
    from simulation_interfaces.srv import GetCurrentWorld

    from manyforge_ros.simulation_control import SimulationControlClient
    from manyforge_ros_bringup.isaac_scene_load_barrier import (
        normalize_file_world_uri,
        select_service_name,
    )

    rclpy.init(args=None)
    node = Node(f"manyforge_i9_world_probe_{os.getpid()}")
    try:
        if play and stop:
            raise LiveRowError("simulation snapshot cannot play and stop together")
        control = SimulationControlClient(node, service_timeout_s=10.0)
        reset_result = control.reset().to_dict() if reset else None
        play_result = (
            control.set_simulation_state(state="playing").to_dict()
            if play
            else None
        )
        stop_result = (
            control.set_simulation_state(state="stopped").to_dict()
            if stop
            else None
        )
        state = control.get_simulation_state().to_dict()
        deadline = time.monotonic() + 10.0
        service_name: str | None = None
        while time.monotonic() < deadline and service_name is None:
            service_name = select_service_name(
                node.get_service_names_and_types(),
                service_type="simulation_interfaces/srv/GetCurrentWorld",
            )
            if service_name is None:
                time.sleep(0.05)
        if service_name is None:
            raise LiveRowError("GetCurrentWorld service is unavailable")
        client = node.create_client(GetCurrentWorld, service_name)
        try:
            if not client.wait_for_service(timeout_sec=10.0):
                raise LiveRowError("GetCurrentWorld service did not become ready")
            future = client.call_async(GetCurrentWorld.Request())
            rclpy.spin_until_future_complete(node, future, timeout_sec=10.0)
            response = future.result() if future.done() else None
        finally:
            node.destroy_client(client)
        if response is None or int(response.result.result) != int(
            response.result.RESULT_OK
        ):
            raise LiveRowError("GetCurrentWorld returned no successful response")
        world = {
            "name": str(response.world.name),
            "uri": normalize_file_world_uri(
                str(response.world.world_resource.uri)
            ),
        }
        return {
            "exact_world": world
            == {"name": spec.world_name, "uri": spec.world_uri},
            "play": play_result,
            "reset": reset_result,
            "state": state,
            "stop": stop_result,
            "world": world,
        }
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


def _apply_program(
    client: HeadlessHttpClient,
    spec: ScenarioSpec,
    program: Mapping[str, Any],
    catalog: Mapping[str, Any],
    *,
    timeout_s: float,
) -> dict[str, Any]:
    body: dict[str, Any] = {
        "expected_applied_record_digest": None,
        "program": program,
        "scenario_catalog_revision": catalog["scenario_catalog_revision"],
        "scenario_id": spec.scenario_id,
    }
    deadline = time.monotonic() + timeout_s
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.program_apply(body)
        if last.get("status") == "applied" and isinstance(last.get("applied"), dict):
            return last["applied"]
        time.sleep(0.05)
    raise LiveRowError(f"Program Apply did not converge: {last}")


def _expected_active_configuration(
    active: Mapping[str, Any] | None,
) -> dict[str, Any]:
    if active is None:
        return {"presence": "absent"}
    identity = active.get("active_control_configuration_id")
    generation = active.get("active_control_configuration_generation")
    if (
        not isinstance(identity, str)
        or not identity
        or type(generation) is not int
        or generation < 1
    ):
        raise LiveRowError("CCLC active configuration identity is incomplete")
    return {
        "presence": "present",
        "active_control_configuration_id": identity,
        "active_control_configuration_generation": generation,
    }


def _binding_rows(
    value: object, *, label: str
) -> tuple[tuple[str, str, str], ...]:
    if not isinstance(value, list) or not value:
        raise LiveRowError(f"{label} has no control bindings")
    rows: list[tuple[str, str, str]] = []
    for item in value:
        if not isinstance(item, dict):
            raise LiveRowError(f"{label} contains a non-object binding")
        row = tuple(
            item.get(key)
            for key in (
                "control_binding_id",
                "control_binding_revision",
                "control_route_id",
            )
        )
        if any(not isinstance(field, str) or not field for field in row):
            raise LiveRowError(f"{label} contains an incomplete binding")
        rows.append(row)  # type: ignore[arg-type]
    if rows != sorted(rows) or len({row[0] for row in rows}) != len(rows):
        raise LiveRowError(f"{label} bindings are not canonical and unique")
    return tuple(rows)


def _lifecycle_expected_domains(
    snapshot: Mapping[str, Any],
) -> tuple[
    list[dict[str, Any]],
    dict[str, tuple[tuple[str, str, str], ...]],
]:
    domains = snapshot.get("domains")
    if not isinstance(domains, list) or not domains:
        raise LiveRowError("CCLC component snapshot has no control domains")
    expected: list[dict[str, Any]] = []
    bindings_by_domain: dict[str, tuple[tuple[str, str, str], ...]] = {}
    for domain in domains:
        if not isinstance(domain, dict):
            raise LiveRowError("CCLC component snapshot has a non-object domain")
        domain_id = domain.get("control_domain_id")
        if not isinstance(domain_id, str) or not domain_id:
            raise LiveRowError("CCLC component snapshot has no domain identity")
        bindings = _binding_rows(
            domain.get("current_control_bindings"),
            label=f"CCLC domain {domain_id}",
        )
        bindings_by_domain[domain_id] = bindings
        row = {
            "control_domain_id": domain_id,
            "control_domain_revision": domain.get("control_domain_revision"),
            "controller_topology_id": domain.get("controller_topology_id"),
            "topology_revision": domain.get("topology_revision"),
            "expected_active_control_configuration": (
                _expected_active_configuration(
                    domain.get("active_control_configuration")
                )
            ),
            "expected_control_bindings": domain.get(
                "current_control_bindings"
            ),
        }
        if any(
            not isinstance(row[key], str) or not row[key]
            for key in (
                "control_domain_revision",
                "controller_topology_id",
                "topology_revision",
            )
        ):
            raise LiveRowError(f"CCLC domain {domain_id} identity is incomplete")
        expected.append(row)
    domain_ids = [row["control_domain_id"] for row in expected]
    if domain_ids != sorted(domain_ids) or len(set(domain_ids)) != len(domain_ids):
        raise LiveRowError("CCLC component domains are not canonical and unique")
    return expected, bindings_by_domain


def _wait_for_cclc_terminal(
    read_status: Callable[[str], dict[str, Any]],
    transaction_id: str,
    *,
    label: str,
    timeout_s: float,
) -> dict[str, Any]:
    if not isinstance(transaction_id, str) or not transaction_id:
        raise LiveRowError(f"CCLC {label} acceptance omitted its transaction ID")
    deadline = time.monotonic() + timeout_s
    last: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        response = read_status(transaction_id)
        status = response.get("status") if isinstance(response, dict) else None
        if status is not None and not isinstance(status, dict):
            raise LiveRowError(f"CCLC {label} status is not an object")
        last = status
        if isinstance(status, dict) and status.get("terminal_result") is not None:
            if status.get("terminal_result") != "succeeded":
                raise LiveRowError(
                    f"CCLC {label} failed: "
                    + json.dumps(
                        _cclc_status_diagnostic(status),
                        separators=(",", ":"),
                        sort_keys=True,
                    )
                )
            return status
        time.sleep(0.05)
    raise LiveRowError(
        f"CCLC {label} did not become terminal: "
        + json.dumps(
            _cclc_status_diagnostic(last),
            separators=(",", ":"),
            sort_keys=True,
        )
    )


def _bounded_error(value: object) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    details = value.get("details")
    reason = details.get("reason") if isinstance(details, dict) else None
    result = {
        "code": value.get("code"),
        "message": (
            value.get("message")[:256]
            if isinstance(value.get("message"), str)
            else None
        ),
        "reason": reason if isinstance(reason, str) else None,
    }
    return {key: item for key, item in result.items() if item is not None}


def _cclc_status_diagnostic(value: object) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    domains = value.get("domains")
    domain_rows = []
    if isinstance(domains, list):
        for domain in domains[:16]:
            if not isinstance(domain, dict):
                continue
            domain_rows.append(
                {
                    "action_state": domain.get("action_state"),
                    "control_domain_id": domain.get("control_domain_id"),
                    "current_state": domain.get("current_state"),
                    "effect": domain.get("effect"),
                    "error": _bounded_error(domain.get("error")),
                }
            )
    return {
        "domains": domain_rows,
        "error": _bounded_error(value.get("error")),
        "phase": value.get("phase"),
        "status_sequence": value.get("status_sequence"),
        "terminal_result": value.get("terminal_result"),
    }


def _validate_ros2_control_lifecycle_observation(
    value: object,
    *,
    expected_domain_ids: Sequence[str],
) -> bool:
    if not isinstance(value, dict):
        raise LiveRowError("execution status omitted ROS lifecycle observation")
    if value.get("schema_version") != (
        _ROS2_CONTROL_LIFECYCLE_OBSERVATION_SCHEMA
    ):
        raise LiveRowError("ROS lifecycle observation schema is not exact")
    domains = value.get("domains")
    if not isinstance(domains, list) or not domains:
        raise LiveRowError("ROS lifecycle observation has no domains")
    observed_domain_ids = [
        row.get("control_domain_id") if isinstance(row, dict) else None
        for row in domains
    ]
    if observed_domain_ids != list(expected_domain_ids):
        raise LiveRowError("ROS lifecycle observation crosses its CCLC domains")

    domain_ready: list[bool] = []
    expected_fact_ids = sorted(_ROS2_CONTROL_LIFECYCLE_READY_FACTS)
    for row in domains:
        if not isinstance(row, dict):
            raise LiveRowError("ROS lifecycle observation domain is not an object")
        generation = row.get("source_generation")
        facts = row.get("facts")
        if type(generation) is not int or generation < 1:
            raise LiveRowError("ROS lifecycle observation generation is invalid")
        if not isinstance(facts, list):
            raise LiveRowError("ROS lifecycle observation facts are absent")
        fact_ids = [
            fact.get("lifecycle_fact_id")
            if isinstance(fact, dict)
            else None
            for fact in facts
        ]
        if fact_ids != expected_fact_ids:
            raise LiveRowError("ROS lifecycle observation facts are not exact")
        fact_values = {
            fact["lifecycle_fact_id"]: fact.get("value")
            for fact in facts
            if isinstance(fact, dict)
        }
        if any(type(item) is not bool for item in fact_values.values()):
            raise LiveRowError("ROS lifecycle observation fact is not boolean")
        ready = bool(
            row.get("component_lifecycle_state") == "active"
            and row.get("exact_controller_configuration_active") is True
            and fact_values == _ROS2_CONTROL_LIFECYCLE_READY_FACTS
        )
        if row.get("ready") is not ready:
            raise LiveRowError("ROS lifecycle observation readiness is incoherent")
        domain_ready.append(ready)
    ready = all(domain_ready)
    if value.get("ready") is not ready:
        raise LiveRowError("aggregate ROS lifecycle readiness is incoherent")
    return ready


def _ros2_control_lifecycle_diagnostic(value: object) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    domains = value.get("domains")
    rows = []
    if isinstance(domains, list):
        for domain in domains[:16]:
            if not isinstance(domain, dict):
                continue
            facts = domain.get("facts")
            rows.append(
                {
                    "component_lifecycle_state": domain.get(
                        "component_lifecycle_state"
                    ),
                    "control_domain_id": domain.get("control_domain_id"),
                    "facts": facts if isinstance(facts, list) else None,
                    "ready": domain.get("ready"),
                    "source_generation": domain.get("source_generation"),
                }
            )
    return {"domains": rows, "ready": value.get("ready")}


def _ros2_control_transport_diagnostic(
    value: object,
) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    reason_codes = value.get("reason_codes")
    return {
        key: value.get(key)
        for key in (
            "command_gate_open",
            "command_path_ready",
            "configured",
            "feedback_messages_received",
            "feedback_path_ready",
            "feedback_publisher_count",
            "feedback_samples_matched",
            "ready",
            "schema_version",
            "state_joint_name",
            "state_topic",
            "transport_kind",
        )
    } | {
        "reason_codes": (
            reason_codes[:16] if isinstance(reason_codes, list) else None
        )
    }


def _wait_for_ros2_control_lifecycle_readiness(
    client: HeadlessHttpClient,
    *,
    expected_owner_epoch: str,
    expected_domain_ids: Sequence[str],
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last: object = None
    last_transport: object = None
    while time.monotonic() < deadline:
        response = client.execution_status()
        status = response.get("status") if isinstance(response, dict) else None
        if (
            not isinstance(response, dict)
            or response.get("ok") is not True
            or response.get("command") != "status"
            or not isinstance(status, dict)
            or status.get("owner_epoch") != expected_owner_epoch
        ):
            raise LiveRowError(
                "execution owner changed while awaiting ROS lifecycle readiness"
            )
        last = status.get("ros2_control_lifecycle_observation")
        last_transport = status.get("ros2_control_transport_diagnostic")
        if _validate_ros2_control_lifecycle_observation(
            last, expected_domain_ids=expected_domain_ids
        ):
            return last
        time.sleep(0.05)
    raise LiveRowError(
        "ROS lifecycle observation did not become ready: "
        + json.dumps(
            {
                "lifecycle": _ros2_control_lifecycle_diagnostic(last),
                "transport": _ros2_control_transport_diagnostic(
                    last_transport
                ),
            },
            separators=(",", ":"),
            sort_keys=True,
        )
    )


def _prepare_and_select_cclc_routes(
    client: HeadlessHttpClient,
    spec: ScenarioSpec,
    *,
    timeout_s: float,
) -> dict[str, Any]:
    """Issue explicit, owner-template-derived CCLC startup requests.

    This is the I9 qualification client's operator-confirmed choice. The
    runtime remains fail-closed and performs no discovery or default route
    selection; every mutable expectation below comes from a fresh CCLC read.
    """

    execution = client.execution_status()
    execution_status = execution.get("status")
    request_owner_epoch = (
        execution_status.get("owner_epoch")
        if isinstance(execution_status, dict)
        else None
    )
    if (
        execution.get("ok") is not True
        or execution.get("command") != "status"
        or not isinstance(request_owner_epoch, str)
        or not request_owner_epoch
    ):
        raise LiveRowError("execution owner epoch is unavailable for CCLC startup")

    initial = client.cclc_component(spec.component_id)
    if (
        initial.get("controllable_component_id") != spec.component_id
        or initial.get("component_lifecycle_state") != "declared"
        or type(initial.get("component_lifecycle_generation")) is not int
        or initial["component_lifecycle_generation"] < 1
        or not isinstance(initial.get("graph_revision"), str)
        or not initial["graph_revision"]
    ):
        raise LiveRowError("fresh CCLC component snapshot is not declared and exact")
    _initial_domains, initial_bindings = _lifecycle_expected_domains(initial)
    domain_ids = sorted(initial_bindings)
    external_readiness = _wait_for_ros2_control_lifecycle_readiness(
        client,
        expected_owner_epoch=request_owner_epoch,
        expected_domain_ids=domain_ids,
        timeout_s=timeout_s,
    )
    lifecycle_rows: list[dict[str, Any]] = []
    correlation_id = f"i9.{spec.scenario_id}.cclc"

    for intent, expected_state in (("prepare", "prepared"), ("enable", "active")):
        snapshot = client.cclc_component(spec.component_id)
        expected_domains, current_bindings = _lifecycle_expected_domains(snapshot)
        if current_bindings != initial_bindings:
            raise LiveRowError("CCLC control bindings changed during lifecycle startup")
        result = client.submit_component_lifecycle(
            {
                "request_context": {
                    "request_id": f"i9.lifecycle.{intent}.{spec.scenario_id}",
                    "request_owner_epoch": request_owner_epoch,
                    "source_label": "i9_isaac_matrix",
                    "correlation_id": correlation_id,
                },
                "controllable_component_id": spec.component_id,
                "intent": intent,
                "expected_graph_revision": snapshot["graph_revision"],
                "expected_component_lifecycle_generation": snapshot[
                    "component_lifecycle_generation"
                ],
                "expected_control_domains": expected_domains,
                "reason": "I9 operator-confirmed simulator startup",
                "confirmed": True,
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise LiveRowError(f"CCLC {intent} was not accepted: {_summary(result)}")
        status = _wait_for_cclc_terminal(
            client.component_lifecycle_status,
            acceptance.get("lifecycle_transaction_id"),
            label=intent,
            timeout_s=timeout_s,
        )
        settled = client.cclc_component(spec.component_id)
        if (
            status.get("controllable_component_id") != spec.component_id
            or status.get("current_state") != expected_state
            or settled.get("component_lifecycle_state") != expected_state
            or settled.get("component_lifecycle_generation")
            != acceptance.get("component_lifecycle_generation")
        ):
            raise LiveRowError(f"CCLC {intent} did not settle to {expected_state}")
        lifecycle_rows.append(
            {
                "component_lifecycle_generation": settled[
                    "component_lifecycle_generation"
                ],
                "intent": intent,
                "state": expected_state,
                "transaction_id": acceptance["lifecycle_transaction_id"],
            }
        )

    templates = client.cclc_route_templates(spec.component_id)
    template_rows = templates.get("templates")
    if not isinstance(template_rows, list):
        raise LiveRowError("CCLC route-template result is incomplete")
    template_domain_ids = [
        row.get("desired_configuration", {}).get("control_domain_id")
        if isinstance(row, dict)
        and isinstance(row.get("desired_configuration"), dict)
        else None
        for row in template_rows
    ]
    if template_domain_ids != domain_ids:
        raise LiveRowError(
            "CCLC route templates do not exactly cover the component domains"
        )

    route_rows: list[dict[str, Any]] = []
    desired_bindings: dict[str, tuple[tuple[str, str, str], ...]] = {}
    for index, domain_id in enumerate(domain_ids, start=1):
        fresh = client.cclc_route_templates(spec.component_id)
        fresh_rows = fresh.get("templates")
        matches = (
            [
                row
                for row in fresh_rows
                if isinstance(row, dict)
                and isinstance(row.get("desired_configuration"), dict)
                and row["desired_configuration"].get("control_domain_id")
                == domain_id
            ]
            if isinstance(fresh_rows, list)
            else []
        )
        if len(matches) != 1:
            raise LiveRowError(f"CCLC route template for {domain_id} is not sole")
        template = matches[0]
        desired = template["desired_configuration"]
        bindings = _binding_rows(
            desired.get("binding_routes"), label=f"CCLC desired route {domain_id}"
        )
        if bindings != initial_bindings[domain_id]:
            raise LiveRowError(
                f"CCLC desired route {domain_id} does not cover every retained binding"
            )
        desired_bindings[domain_id] = bindings
        request_id = "i9.route.%d.%s" % (
            index,
            hashlib.sha256(domain_id.encode()).hexdigest()[:16],
        )
        result = client.submit_route_selection(
            {
                "request_context": {
                    "request_id": request_id,
                    "request_owner_epoch": request_owner_epoch,
                    "source_label": "i9_isaac_matrix",
                    "correlation_id": correlation_id,
                },
                "expected_retained_bundle_id": fresh["retained_bundle_id"],
                "expected_graph_revision": fresh["graph_revision"],
                "desired_configuration": desired,
                "expected_active_control_configuration": template[
                    "expected_active_control_configuration"
                ],
                "component_expectations": template["component_expectations"],
                "confirmed": True,
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise LiveRowError(
                f"CCLC route {domain_id} was not accepted: {_summary(result)}"
            )
        status = _wait_for_cclc_terminal(
            client.route_selection_status,
            acceptance.get("route_selection_transaction_id"),
            label=f"route {domain_id}",
            timeout_s=timeout_s,
        )
        active = status.get("resulting_active_control_configuration")
        if (
            not isinstance(active, dict)
            or active.get("control_domain_id") != domain_id
            or active.get("state") != "reconciled"
            or _binding_rows(
                active.get("binding_routes"),
                label=f"CCLC active route {domain_id}",
            )
            != bindings
        ):
            raise LiveRowError(f"CCLC route {domain_id} did not reconcile exactly")
        route_rows.append(
            {
                "active_control_configuration_generation": active[
                    "active_control_configuration_generation"
                ],
                "active_control_configuration_id": active[
                    "active_control_configuration_id"
                ],
                "binding_count": len(bindings),
                "bindings_digest": _digest(_json_bytes(bindings)),
                "control_domain_id": domain_id,
                "transaction_id": acceptance[
                    "route_selection_transaction_id"
                ],
            }
        )

    final = client.cclc_component(spec.component_id)
    final_expected, final_bindings = _lifecycle_expected_domains(final)
    if (
        final.get("component_lifecycle_state") != "active"
        or sorted(final_bindings) != domain_ids
        or len(final_expected) != len(domain_ids)
    ):
        raise LiveRowError("CCLC final component snapshot is not fully active")
    for domain in final["domains"]:
        domain_id = domain["control_domain_id"]
        active = domain.get("active_control_configuration")
        if (
            not isinstance(active, dict)
            or active.get("state") != "reconciled"
            or _binding_rows(
                active.get("binding_routes"),
                label=f"CCLC final route {domain_id}",
            )
            != desired_bindings[domain_id]
        ):
            raise LiveRowError(f"CCLC final route {domain_id} lost readiness")

    return {
        "schema_version": _CCLC_STARTUP_SCHEMA,
        "component_id": spec.component_id,
        "initial": {
            "generation": initial["component_lifecycle_generation"],
            "state": initial["component_lifecycle_state"],
        },
        "final": {
            "generation": final["component_lifecycle_generation"],
            "state": final["component_lifecycle_state"],
        },
        "external_readiness": {
            "digest": _digest(_json_bytes(external_readiness)),
            "domains": [
                {
                    "control_domain_id": row["control_domain_id"],
                    "source_generation": row["source_generation"],
                }
                for row in external_readiness["domains"]
            ],
            "ready": True,
            "schema_version": external_readiness["schema_version"],
        },
        "lifecycle": lifecycle_rows,
        "routes": route_rows,
        "request_owner_epoch": request_owner_epoch,
        "passed": True,
    }


def _run_program(
    client: HeadlessHttpClient,
    spec: ScenarioSpec,
    catalog: Mapping[str, Any],
    applied: Mapping[str, Any],
    evidence: RunEvidence,
    *,
    timeout_s: float,
) -> dict[str, Any]:
    source = applied.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("record_digest"), str):
        raise LiveRowError("Applied Program omitted its immutable record digest")
    correlation = f"i9.{spec.scenario_id}"
    snapshot = client.program_run_start(
        {
            "correlation_id": correlation,
            "expected_applied_record_digest": source["record_digest"],
            "request_id": f"i9.start.{spec.scenario_id}",
            "request_owner_epoch": "i9-isaac-matrix",
            "scenario_catalog_revision": catalog["scenario_catalog_revision"],
            "scenario_id": spec.scenario_id,
            "source_label": "i9_isaac_matrix",
        }
    )
    capture = snapshot.get("capture")
    if not isinstance(capture, dict) or not isinstance(
        capture.get("program_run_id"), str
    ):
        raise LiveRowError("Program Run start omitted its owner identity")
    run_id = capture["program_run_id"]
    # Program Run start synchronously initializes and publishes the authored
    # logical scene before tree construction.  Object-motion evidence cannot
    # be sampled earlier: Program Apply selects the product but deliberately
    # leaves the logical-scene owner empty.
    _prime_run_evidence(evidence, client, client.scene(), spec)
    deadline = time.monotonic() + timeout_s
    next_observation = 0.0
    while snapshot.get("state") not in _TERMINAL_STATES:
        now = time.monotonic()
        if now >= deadline:
            raise LiveRowError(f"Program Run did not settle: {_summary(snapshot)}")
        if now >= next_observation:
            _observe_run_evidence(evidence, client, spec)
            next_observation = now + 0.1
        time.sleep(0.05)
        snapshot = client.program_run(run_id)
    _observe_run_evidence(evidence, client, spec)
    return snapshot


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode()


def _digest(value: bytes) -> str:
    return _SHA256_PREFIX + hashlib.sha256(value).hexdigest()


def _summary(value: Any) -> dict[str, Any]:
    encoded = _json_bytes(value)
    if len(encoded) > _MAX_COMMAND_BYTES:
        raise LiveRowError("observation exceeds its compact evidence bound")
    result: dict[str, Any] = {
        "bytes": len(encoded),
        "sha256": _digest(encoded),
    }
    if isinstance(value, dict):
        for key in (
            "api_version",
            "complete",
            "generation",
            "message",
            "owner_epoch",
            "program_run_id",
            "projection_generation",
            "projection_owner_epoch",
            "reason_code",
            "run_revision",
            "schema_version",
            "state",
            "tree_status",
        ):
            if key in value and isinstance(value[key], (str, int, bool, type(None))):
                result[key] = value[key]
    return result


def _contains(value: Any, text: str) -> bool:
    return text.lower() in _json_bytes(value).decode("utf-8", errors="replace").lower()


def _scene_fresh(runtime: Mapping[str, Any], scene: Mapping[str, Any]) -> bool:
    sections = runtime.get("sections")
    if not isinstance(sections, dict):
        return False
    for key in ("logical_scene", "scene_runtime"):
        section = sections.get(key)
        if (
            not isinstance(section, dict)
            or section.get("available") is not True
            or section.get("freshness") != "fresh"
        ):
            return False
    return scene.get("schema_version") == "manyforge.scene_projection.v1"


def _generation(value: Mapping[str, Any]) -> int:
    for key in ("projection_generation", "generation", "scene_generation"):
        candidate = value.get(key)
        if type(candidate) is int and candidate >= 0:
            return candidate
    return -1


def _program_run_id(program: Mapping[str, Any]) -> str | None:
    capture = program.get("capture")
    if not isinstance(capture, dict):
        return None
    value = capture.get("program_run_id")
    return value if isinstance(value, str) and value else None


def _extract_gripper_targets(program: Mapping[str, Any]) -> tuple[GripperTarget, ...]:
    targets: set[GripperTarget] = set()

    def add_target(
        *, target_role: object, position: object, label: str
    ) -> None:
        if not isinstance(target_role, str) or not target_role:
            raise LiveRowError(f"{label} has no exact target role")
        if not isinstance(position, dict):
            raise LiveRowError(f"{label} has no typed position")
        kind = position.get("kind")
        if kind not in {"linear_m", "angular_rad"}:
            raise LiveRowError(f"{label} has an unsupported coordinate kind")
        field_name = "value_m" if kind == "linear_m" else "value_rad"
        value = position.get(field_name)
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
        ):
            raise LiveRowError(f"{label} has a non-finite target")
        targets.add(GripperTarget(target_role, kind, float(value)))

    def visit(node: object) -> None:
        if not isinstance(node, dict):
            raise LiveRowError("Program tree contains a non-object node")
        node_id = node.get("node_id")
        parameters = node.get("parameters")
        if not isinstance(parameters, dict):
            raise LiveRowError("Program tree node has no parameter object")
        if node_id == "gripper.command_position":
            add_target(
                target_role=parameters.get("target_role"),
                position=parameters.get("position"),
                label="gripper.command_position",
            )
        elif node_id == "planning.plan_pick_sequence":
            add_target(
                target_role=parameters.get("tool_target_role"),
                position=parameters.get("tool_position"),
                label="planning.plan_pick_sequence tool command",
            )
        children = node.get("children")
        if not isinstance(children, list):
            raise LiveRowError("Program tree node has no children array")
        for child in children:
            visit(child)

    visit(program.get("tree"))
    if not targets:
        raise LiveRowError("I9 Program has no typed gripper command target")
    return tuple(sorted(targets))


def _read_ros_graph(
    spec: ScenarioSpec, *, timeout_s: float = 30.0
) -> tuple[dict[str, Any], dict[str, Any], dict[str, dict[str, Any]]]:
    """Read one converged graph without racing short-lived CLI participants."""
    import rclpy
    from rclpy.node import Node

    def bounded_result(output: str) -> dict[str, Any]:
        encoded = output.encode()
        if len(encoded) > _MAX_COMMAND_BYTES:
            raise LiveRowError("ROS graph observation exceeded its byte bound")
        return {
            "bytes": len(encoded),
            "output": output,
            "returncode": 0,
            "sha256": _digest(encoded),
        }

    expected_namespace = f"/manyforge/{spec.component_id}"
    other_namespaces = {
        f"/manyforge/{candidate.component_id}"
        for candidate in SCENARIO_BY_ID.values()
        if candidate.component_id != spec.component_id
    }
    nodes: list[str] = []
    topic_names_and_types: dict[str, list[str]] = {}
    command_subscriptions = 0
    deadline = time.monotonic() + timeout_s

    rclpy.init(args=None)
    node = Node(f"manyforge_i9_graph_probe_{os.getpid()}")
    try:
        while True:
            remaining_s = deadline - time.monotonic()
            if remaining_s <= 0.0:
                break
            rclpy.spin_once(node, timeout_sec=min(0.1, remaining_s))
            nodes = sorted(
                {
                    (
                        f"/{name.lstrip('/')}"
                        if namespace in ("", "/")
                        else f"/{namespace.strip('/')}/{name.lstrip('/')}"
                    )
                    for name, namespace in node.get_node_names_and_namespaces()
                }
            )
            topic_names_and_types = {
                topic: sorted(set(types))
                for topic, types in node.get_topic_names_and_types()
            }
            command_subscriptions = len(
                node.get_subscriptions_info_by_topic("/isaac_joint_commands")
            )
            topic_types_exact = all(
                topic_names_and_types.get(topic)
                == [TOPIC_TYPE_BY_NAME[topic]]
                for topic in spec.required_topics
            )
            namespace_exact = any(
                candidate == expected_namespace
                or candidate.startswith(expected_namespace + "/")
                for candidate in nodes
            ) and not any(
                candidate == other
                or candidate.startswith(other + "/")
                for candidate in nodes
                for other in other_namespaces
            )
            if topic_types_exact and namespace_exact and command_subscriptions > 0:
                break
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()

    node_output = "".join(f"{name}\n" for name in nodes)
    command_output = f"Subscription count: {command_subscriptions}\n"
    topic_types = {
        topic: {
            "returncode": int(
                topic_names_and_types.get(topic)
                != [TOPIC_TYPE_BY_NAME[topic]]
            ),
            "type": (
                topic_names_and_types[topic][0]
                if len(topic_names_and_types.get(topic, ())) == 1
                else None
            ),
        }
        for topic in spec.required_topics
    }
    return (
        bounded_result(node_output),
        bounded_result(command_output),
        topic_types,
    )


def _section(scene: Mapping[str, Any], name: str) -> Mapping[str, Any] | None:
    value = scene.get(name)
    return value if isinstance(value, dict) else None


def _section_payload(
    scene: Mapping[str, Any], name: str
) -> Mapping[str, Any] | None:
    section = _section(scene, name)
    if section is None:
        return None
    payload = section.get("payload")
    return payload if isinstance(payload, dict) else None


def _logical_snapshot(scene: Mapping[str, Any]) -> Mapping[str, Any] | None:
    payload = _section_payload(scene, "logical_scene")
    if payload is None:
        return None
    snapshot = payload.get("snapshot")
    return snapshot if isinstance(snapshot, dict) else None


def _projection_revision(scene: Mapping[str, Any]) -> Mapping[str, Any] | None:
    payload = _section_payload(scene, "logical_scene")
    if payload is None:
        return None
    access = payload.get("access")
    if not isinstance(access, dict):
        return None
    revision = access.get("active_projection")
    return revision if isinstance(revision, dict) else None


def _resolved_position(
    client: HeadlessHttpClient,
    scene: Mapping[str, Any],
    scene_resource_id: str,
) -> tuple[float, float, float]:
    current_scene = scene
    response: Mapping[str, Any] | None = None
    for attempt in range(_SCENE_POSE_QUERY_ATTEMPTS):
        revision = _projection_revision(current_scene)
        if revision is None:
            raise LiveRowError("logical scene has no active projection revision")
        try:
            response = client.resolve_scene_resources(
                {
                    "api_version": "manyforge.scene_resource_pose_query.v1",
                    "expected_projection_revision": dict(revision),
                    "scene_resource_ids": [scene_resource_id],
                }
            )
            break
        except HeadlessClientError as error:
            if error.code not in {"stale_revision", "scene_projection_changed"}:
                raise
            if attempt + 1 == _SCENE_POSE_QUERY_ATTEMPTS:
                raise LiveRowError(
                    "scene resource pose query could not acquire a stable revision"
                ) from error
            time.sleep(0.01)
            current_scene = client.scene()
    if response is None:
        raise LiveRowError("scene resource pose query produced no response")
    poses = response.get("poses")
    if not isinstance(poses, list) or len(poses) != 1:
        raise LiveRowError("scene resource pose query was incomplete")
    row = poses[0]
    if not isinstance(row, dict) or row.get("scene_resource_id") != scene_resource_id:
        raise LiveRowError("scene resource pose query crossed object identity")
    pose = row.get("pose_in_cell")
    position = pose.get("position_m") if isinstance(pose, dict) else None
    if not isinstance(position, dict):
        raise LiveRowError("scene resource pose query omitted its position")
    coordinates = tuple(position.get(axis) for axis in ("x", "y", "z"))
    if any(
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        for value in coordinates
    ):
        raise LiveRowError("scene resource pose query returned invalid coordinates")
    return tuple(float(value) for value in coordinates)  # type: ignore[arg-type]


def _record_execution_evidence(
    evidence: RunEvidence,
    spec: ScenarioSpec,
    response: Mapping[str, Any],
) -> None:
    status = response.get("status")
    if (
        response.get("ok") is not True
        or response.get("command") != "status"
        or not isinstance(status, dict)
    ):
        raise LiveRowError("execution status is not one successful owner projection")
    owner_epoch = status.get("owner_epoch")
    cell_id = status.get("cell_id")
    observed_targets = status.get("observed_targets")
    if (
        not isinstance(owner_epoch, str)
        or not owner_epoch
        or not isinstance(cell_id, str)
        or not cell_id
        or not isinstance(observed_targets, list)
        or not observed_targets
    ):
        raise LiveRowError("execution status identity or targets are incomplete")
    evidence.execution_samples += 1
    evidence.execution_owner_epochs.add(owner_epoch)
    evidence.execution_cell_ids.add(cell_id)
    for target in observed_targets:
        if not isinstance(target, dict):
            raise LiveRowError("execution observed target is not an object")
        component_id = target.get("controllable_component_id")
        if not isinstance(component_id, str) or not component_id:
            raise LiveRowError("execution observed target omitted its component")
        evidence.execution_component_ids.add(component_id)

    gripper_role = status.get("gripper_target_role_id")
    matching = [
        target
        for target in observed_targets
        if isinstance(target, dict) and target.get("target_role_id") == gripper_role
    ]
    if len(matching) != 1 or not isinstance(gripper_role, str):
        raise LiveRowError("execution status has no sole gripper target")
    gripper = matching[0]
    positions = gripper.get("positions")
    if gripper.get("complete") is not True or not isinstance(positions, list):
        return
    tolerance = _GRIPPER_TOLERANCE_BY_PROFILE[spec.profile]
    for position in positions:
        if not isinstance(position, dict):
            continue
        kind = position.get("kind")
        value_field = "value_m" if kind == "linear_m" else "value_rad"
        value = position.get(value_field)
        if (
            kind not in {"linear_m", "angular_rad"}
            or isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
        ):
            continue
        for expected in evidence.gripper_targets:
            if (
                expected.target_role_id == gripper_role
                and expected.coordinate_kind == kind
                and abs(float(value) - expected.value) <= tolerance
            ):
                evidence.gripper_hits.add(expected)


def _foundationpose_source_rows(
    scene: Mapping[str, Any]
) -> tuple[Mapping[str, Any], ...]:
    rows = _foundationpose_lifetime_rows(scene)
    exact: list[Mapping[str, Any]] = []
    for row in rows:
        observation = row.get("payload")
        pose = observation.get("pose") if isinstance(observation, dict) else None
        source = (
            observation.get("source") if isinstance(observation, dict) else None
        )
        if (
            row.get("quality") == "good"
            and row.get("fresh") is True
            and row.get("eligible") is True
            and isinstance(observation, dict)
            and observation.get("api_version")
            == "manyforge.ros_pose_observation.v1"
            and observation.get("kind") == "detection3d_pose"
            and isinstance(pose, dict)
            and pose.get("frame_kind") == "cell_frame"
            and pose.get("frame_id") == "link_base"
            and isinstance(source, dict)
            and source.get("topic") == "/output"
        ):
            exact.append(row)
    return tuple(exact)


def _foundationpose_lifetime_rows(
    scene: Mapping[str, Any]
) -> tuple[Mapping[str, Any], ...]:
    payload = _section_payload(scene, "scene_runtime")
    state = payload.get("state") if payload is not None else None
    rows = state.get("pose_sources") if isinstance(state, dict) else None
    if not isinstance(rows, list):
        return ()
    selected: list[Mapping[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        if (
            row.get("source_id") == "foundationpose_trocar"
            and row.get("object_id") == "graspable"
            and row.get("policy_id") == "before_planning"
        ):
            selected.append(row)
    return tuple(selected)


def _foundationpose_identity(
    scene: Mapping[str, Any], *, require_fresh: bool
) -> tuple[str, int] | None:
    rows = (
        _foundationpose_source_rows(scene)
        if require_fresh
        else _foundationpose_lifetime_rows(scene)
    )
    identities = {
        (row.get("source_epoch"), row.get("source_sequence"))
        for row in rows
        if isinstance(row.get("source_epoch"), str)
        and type(row.get("source_sequence")) is int
        and row["source_sequence"] > 0
    }
    if len(identities) != 1:
        return None
    epoch, sequence = next(iter(identities))
    return str(epoch), int(sequence)


def _wait_for_foundationpose_identity(
    client: HeadlessHttpClient,
    *,
    timeout_s: float,
    different_from_epoch: str | None = None,
) -> tuple[str, int]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        identity = _foundationpose_identity(client.scene(), require_fresh=True)
        if identity is not None and (
            different_from_epoch is None or identity[0] != different_from_epoch
        ):
            return identity
        time.sleep(0.05)
    raise LiveRowError("FoundationPose source did not publish one fresh identity")


def _record_scene_evidence(
    evidence: RunEvidence,
    scene: Mapping[str, Any],
) -> None:
    generation = _generation(scene)
    if generation >= 0:
        evidence.scene_projection_generations.add(generation)
    source_rows = _foundationpose_source_rows(scene)
    by_identity: dict[tuple[str, int], Mapping[str, Any]] = {}
    for row in source_rows:
        epoch = row.get("source_epoch")
        sequence = row.get("source_sequence")
        if isinstance(epoch, str) and type(sequence) is int and sequence > 0:
            evidence.foundationpose_source_sequences.add(sequence)
            evidence.foundationpose_source_identities.add((epoch, sequence))
            by_identity[(epoch, sequence)] = row
    snapshot = _logical_snapshot(scene)
    if snapshot is None:
        return
    attachments = snapshot.get("carried_object_attachments")
    if isinstance(attachments, list):
        evidence.object_carried_observed = evidence.object_carried_observed or any(
            isinstance(row, dict)
            and row.get("scene_resource_id") == _OBJECT_MOTION_RESOURCE
            for row in attachments
        )
    objects = snapshot.get("objects")
    if not isinstance(objects, list):
        return
    graspable = next(
        (
            row
            for row in objects
            if isinstance(row, dict)
            and row.get("scene_resource_id") == "graspable"
        ),
        None,
    )
    provider = (
        graspable.get("provider_evidence")
        if isinstance(graspable, dict)
        else None
    )
    if not isinstance(provider, dict):
        return
    epoch = provider.get("provider_source_epoch")
    sequence = provider.get("provider_source_sequence")
    if isinstance(epoch, str) and type(sequence) is int and sequence > 0:
        evidence.foundationpose_provider_sequences.add(sequence)
        evidence.foundationpose_provider_match = (
            evidence.foundationpose_provider_match
            or (epoch, sequence) in by_identity
            or (epoch, sequence) in evidence.foundationpose_source_identities
        )


def _prime_run_evidence(
    evidence: RunEvidence,
    client: HeadlessHttpClient,
    scene: Mapping[str, Any],
    spec: ScenarioSpec,
) -> None:
    _record_scene_evidence(evidence, scene)
    if "object_motion_observed" in spec.required_checks:
        evidence.object_initial_position = _resolved_position(
            client, scene, _OBJECT_MOTION_RESOURCE
        )
        evidence.object_pose_samples = 1


def _observe_run_evidence(
    evidence: RunEvidence,
    client: HeadlessHttpClient,
    spec: ScenarioSpec,
) -> None:
    _record_execution_evidence(evidence, spec, client.execution_status())
    scene = client.scene()
    _record_scene_evidence(evidence, scene)
    if "object_motion_observed" not in spec.required_checks:
        return
    try:
        position = _resolved_position(client, scene, _OBJECT_MOTION_RESOURCE)
    except HeadlessClientError:
        evidence.pose_query_rejections += 1
        return
    if evidence.object_initial_position is None:
        raise LiveRowError("object-motion observation has no initial pose")
    evidence.object_pose_samples += 1
    displacement = math.dist(evidence.object_initial_position, position)
    evidence.object_max_displacement_m = max(
        evidence.object_max_displacement_m, displacement
    )


def _history_program_run_ids(history: Mapping[str, Any]) -> set[str]:
    found: set[str] = set()

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            for key, item in value.items():
                if key == "program_run_id" and isinstance(item, str) and item:
                    found.add(item)
                else:
                    visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)

    visit(history.get("events"))
    return found


def _collision_alarm_exact(
    alarms: Mapping[str, Any], program_run_id: str | None
) -> bool:
    rows = alarms.get("alarms")
    if not isinstance(rows, list) or program_run_id is None:
        return False
    for alarm in rows:
        if not isinstance(alarm, dict):
            continue
        subject = alarm.get("subject")
        details = alarm.get("details")
        correlations = alarm.get("correlations")
        if (
            alarm.get("definition_id") == "manyforge.motion.runtime_collision"
            and alarm.get("lifecycle_state") == "active_unacknowledged"
            and alarm.get("priority") == "critical"
            and alarm.get("requires_motion_attention") is True
            and isinstance(subject, dict)
            and subject.get("subject_kind") == "operation"
            and isinstance(subject.get("subject_id"), str)
            and isinstance(subject.get("resource_ids"), list)
            and bool(subject["resource_ids"])
            and isinstance(details, dict)
            and details.get("reason") == "runtime_collision_fault"
            and details.get("source_label") == "kernel.runtime_collision"
            and isinstance(correlations, dict)
            and correlations.get("program_run_id") == program_run_id
            and isinstance(correlations.get("participant_id"), str)
            and isinstance(correlations.get("request_id"), str)
        ):
            return True
    return False


def _collision_stop_history_exact(
    history: Mapping[str, Any], program_run_id: str | None
) -> bool:
    rows = history.get("events")
    if not isinstance(rows, list) or program_run_id is None:
        return False
    for stored in rows:
        event = stored.get("event") if isinstance(stored, dict) else None
        if (
            not isinstance(event, dict)
            or event.get("event_type") != "stop_acceptance_projected"
        ):
            continue
        envelope = event.get("payload")
        journal = (
            envelope.get("journal_payload")
            if isinstance(envelope, dict)
            else None
        )
        intent = journal.get("intent") if isinstance(journal, dict) else None
        value = intent.get("value") if isinstance(intent, dict) else None
        context = value.get("request_context") if isinstance(value, dict) else None
        affected = journal.get("affected_state") if isinstance(journal, dict) else None
        operations = affected.get("operations") if isinstance(affected, dict) else None
        if (
            isinstance(journal, dict)
            and journal.get("api_version")
            == "manyforge.stop_acceptance_projection.v2"
            and isinstance(intent, dict)
            and intent.get("intent_kind") == "ordinary_stop"
            and isinstance(value, dict)
            and value.get("reason") == "runtime_collision_fault"
            and isinstance(context, dict)
            and context.get("source_label") == "kernel.runtime_collision"
            and isinstance(operations, list)
            and len(operations) == 1
            and isinstance(operations[0], dict)
            and operations[0].get("program_run_id") == program_run_id
        ):
            return True
    return False


def _wait_for_trace_evidence(
    client: HeadlessHttpClient,
    spec: ScenarioSpec,
    program_run_id: str,
    *,
    timeout_s: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    deadline = time.monotonic() + min(timeout_s, 15.0)
    history: dict[str, Any] = {}
    alarms: dict[str, Any] = {}
    collision = "collision_stop_terminal" in spec.required_checks
    while time.monotonic() < deadline:
        alarms = client.alarms(limit=128, ordering="attention_newest")
        history = client.history(params=(("limit", 32),))
        if program_run_id in _history_program_run_ids(history) and (
            not collision
            or (
                _collision_alarm_exact(alarms, program_run_id)
                and _collision_stop_history_exact(history, program_run_id)
            )
        ):
            return alarms, history
        time.sleep(0.05)
    return alarms, history


def _logical_viewer_owner_exact(
    scene: Mapping[str, Any], runtime: Mapping[str, Any], spec: ScenarioSpec
) -> bool:
    logical = _section(scene, "logical_scene")
    runtime_section = _section(scene, "scene_runtime")
    snapshot = _logical_snapshot(scene)
    return bool(
        logical is not None
        and runtime_section is not None
        and snapshot is not None
        and logical.get("source_owner_id") == f"logical-scene.{spec.cell_id}"
        and isinstance(logical.get("source_owner_epoch"), str)
        and runtime_section.get("source_owner_id")
        == f"scene-runtime.{spec.cell_id}"
        and isinstance(runtime_section.get("source_owner_epoch"), str)
        and logical.get("projection_generation")
        == runtime.get("projection_generation")
        and runtime_section.get("projection_generation")
        == runtime.get("projection_generation")
        and snapshot.get("cell_id") == spec.cell_id
    )


def _build_checks(
    spec: ScenarioSpec,
    *,
    barrier: Mapping[str, Any],
    before_control: Mapping[str, Any],
    active_control: Mapping[str, Any],
    after_control: Mapping[str, Any],
    controllers: Mapping[str, Any],
    nodes: Mapping[str, Any],
    command_info: Mapping[str, Any],
    cclc_startup: Mapping[str, Any],
    samples: Mapping[str, Mapping[str, Any]],
    topic_types: Mapping[str, Mapping[str, Any]],
    program: Mapping[str, Any],
    run_evidence: RunEvidence,
    runtime: Mapping[str, Any],
    scene_before: Mapping[str, Any],
    scene_after: Mapping[str, Any],
    alarms: Mapping[str, Any],
    history: Mapping[str, Any],
    tf_probe: Mapping[str, Any] | None,
    assistant: Mapping[str, Any] | None,
    viewer_cell_id: str | None,
) -> dict[str, bool]:
    namespace = f"/manyforge/{spec.component_id}"
    expected_terminal = "failed" if "collision" in spec.scenario_id else "succeeded"
    control_before_state = before_control.get("state", {}).get("state")
    control_active_state = active_control.get("state", {}).get("state")
    control_after_state = after_control.get("state", {}).get("state")
    reset_result = after_control.get("reset")
    program_run_id = _program_run_id(program)
    capture = program.get("capture")
    execution_owner_exact = bool(
        run_evidence.execution_samples > 0
        and len(run_evidence.execution_owner_epochs) == 1
        and run_evidence.execution_cell_ids == {spec.cell_id}
        and run_evidence.execution_component_ids == {spec.component_id}
    )
    required_topics_exact = bool(
        set(topic_types) == set(spec.required_topics)
        and all(
            topic_types[topic].get("returncode") == 0
            and topic_types[topic].get("type") == TOPIC_TYPE_BY_NAME[topic]
            for topic in spec.required_topics
        )
    )
    checks: dict[str, bool] = {
        "cclc_route_ready": bool(
            cclc_startup.get("schema_version") == _CCLC_STARTUP_SCHEMA
            and cclc_startup.get("component_id") == spec.component_id
            and cclc_startup.get("passed") is True
        ),
        "controller_ready": (
            controllers.get("returncode") == 0
            and "isaac_arm_controller" in str(controllers.get("output", ""))
            and "joint_state_broadcaster" in str(controllers.get("output", ""))
            and str(controllers.get("output", "")).count(" active") >= 2
        ),
        "cross_host_clock_payload": bool(samples.get("/clock", {}).get("seen")),
        "exact_world_readback": bool(
            barrier.get("world_name") == spec.world_name
            and barrier.get("world_uri") == spec.world_uri
            and before_control.get("exact_world") is True
            and after_control.get("exact_world") is True
        ),
        "execution_owner_exact": execution_owner_exact,
        "joint_command_payload": bool(
            samples.get("/isaac_joint_commands", {}).get("seen")
            and "Subscription count:" in str(command_info.get("output", ""))
            and not re.search(
                r"Subscription count:\s*0(?:\D|$)",
                str(command_info.get("output", "")),
            )
        ),
        "joint_state_payload": bool(
            samples.get("/isaac_joint_states", {}).get("seen")
        ),
        "program_terminal": bool(
            program.get("state") == expected_terminal
            and isinstance(capture, dict)
            and capture.get("scenario_id") == spec.scenario_id
            and program_run_id is not None
        ),
        "reset_observed": bool(
            isinstance(reset_result, dict) and reset_result.get("success") is True
        ),
        "required_topics_exact": required_topics_exact,
        "runtime_namespace_exact": bool(
            nodes.get("returncode") == 0
            and namespace in str(nodes.get("output", ""))
            and all(
                f"/manyforge/{other.component_id}" not in str(nodes.get("output", ""))
                for other in SCENARIO_BY_ID.values()
                if other.component_id != spec.component_id
            )
        ),
        "simulation_playing_during": bool(
            active_control.get("exact_world") is True
            and control_active_state == "playing"
            and isinstance(active_control.get("play"), dict)
            and active_control["play"].get("success") is True
        ),
        "simulation_stopped_after": bool(
            control_after_state == "stopped"
            and isinstance(after_control.get("stop"), dict)
            and after_control["stop"].get("success") is True
        ),
        "simulation_stopped_before": control_before_state == "stopped",
        "status_trace_correlated": bool(
            program_run_id is not None
            and program_run_id in _history_program_run_ids(history)
        ),
    }
    if "scene_observation_fresh" in spec.required_checks:
        checks["scene_observation_fresh"] = _scene_fresh(runtime, scene_after)
    if "gripper_converged" in spec.required_checks:
        gripper_topic = (
            "/isaac_joint_commands_gripper"
            if spec.profile == "lite6"
            else "/isaac_gripper_state"
        )
        checks["gripper_converged"] = bool(
            samples.get(gripper_topic, {}).get("seen")
            and program.get("state") == "succeeded"
            and len(run_evidence.gripper_targets) >= 2
            and run_evidence.gripper_hits == set(run_evidence.gripper_targets)
        )
    if "object_motion_observed" in spec.required_checks:
        checks["object_motion_observed"] = bool(
            program.get("state") == "succeeded"
            and run_evidence.object_carried_observed
            and run_evidence.object_pose_samples >= 2
            and run_evidence.object_max_displacement_m
            > _OBJECT_MOTION_MINIMUM_M
        )
    if "foundationpose_detection_fresh" in spec.required_checks:
        checks["foundationpose_detection_fresh"] = bool(
            samples.get("/output", {}).get("seen")
            and run_evidence.foundationpose_source_identities
        )
        checks["foundationpose_pose_latched"] = (
            run_evidence.foundationpose_provider_match
        )
        checks["tf_chain_exact"] = bool(
            tf_probe is not None
            and tf_probe.get("bytes", 0) > 0
            and _contains(tf_probe, "camera_color_optical_frame")
            and _contains(tf_probe, "link_base")
        )
    if "collision_stop_terminal" in spec.required_checks:
        checks["collision_stop_terminal"] = bool(
            program.get("state") == "failed"
            and _collision_stop_history_exact(history, program_run_id)
        )
        checks["collision_stop_kernel_owned"] = _collision_alarm_exact(
            alarms, program_run_id
        )
    if "assistant_snapshot_owner_exact" in spec.required_checks:
        viewer_generation = (
            assistant.get("viewer_generation") if assistant is not None else None
        )
        projection_generation = runtime.get("projection_generation")
        checks["assistant_snapshot_owner_exact"] = bool(
            assistant is not None
            and assistant.get("passed") is True
            and assistant.get("viewer_owner_epoch")
            == runtime.get("projection_owner_epoch")
            and type(viewer_generation) is int
            and type(projection_generation) is int
            and viewer_generation >= projection_generation
        )
        checks["assistant_snapshot_transmitted"] = bool(
            assistant is not None
            and assistant.get("model_requests") == 1
            and assistant.get("snapshot_completed_delta") == 1
        )
        checks["ghost_and_viewer_owner_exact"] = bool(
            viewer_cell_id == spec.cell_id
            and _logical_viewer_owner_exact(scene_after, runtime, spec)
        )
    if set(checks) != spec.required_checks:
        missing = sorted(spec.required_checks - set(checks))
        extra = sorted(set(checks) - spec.required_checks)
        raise LiveRowError(
            f"derived check set changed: missing={missing}, extra={extra}"
        )
    return checks


def _process_commands(
    config: LiveConfig,
    *,
    api_port: int,
    runner_port: int,
    composer_port: int | None,
    manager_root: Path,
    runtime_root: Path,
    publication_root: Path,
) -> dict[str, list[str]]:
    api_url = f"http://127.0.0.1:{api_port}"
    runner_url = f"http://127.0.0.1:{runner_port}"
    commands = {
        "manager": [
            str(config.installed_root / "manyforge_manager_daemon"),
            "--package-root",
            str(config.package_root),
            "--canonical-empty-admission-filename",
            config.spec.admission_filename,
            "--manager-state-root",
            str(manager_root),
            "--runtime-state-root",
            str(runtime_root),
            "--logical-scene-database",
            str(config.run_root / "logical-scene.sqlite3"),
            "--publication-root",
            str(publication_root),
            "--reconciliation-period-ms",
            "20",
        ],
        "program": [
            sys.executable,
            "-m",
            "manyforge_program_runner",
            "--host",
            "127.0.0.1",
            "--port",
            str(runner_port),
            "--headless-url",
            api_url,
            "--owner-id",
            f"program-runner.{config.spec.scenario_id}",
            "--headless-timeout-s",
            "10",
            "--simulation-control-config",
            str(
                config.source_root
                / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"
            ),
        ],
        "headless": [
            sys.executable,
            "-m",
            "manyforge_headless_api",
            "serve",
            "--host",
            "127.0.0.1",
            "--port",
            str(api_port),
            "--publication-root",
            str(publication_root),
            "--package-root",
            str(config.package_root),
            "--node-catalog-path",
            str(config.node_catalog),
            "--alarm-catalog-path",
            str(config.alarm_catalog),
            "--historian-database-path",
            str(config.run_root / "history.sqlite3"),
            "--manager-state-root",
            str(manager_root),
            "--execution-state-root",
            str(runtime_root),
            "--cell-id",
            config.spec.cell_id,
            "--owner-id",
            f"headless-api.{config.spec.scenario_id}",
            "--freshness-ms",
            "120000",
            "--manager-timeout-s",
            "5",
            "--program-runner-url",
            runner_url,
            "--program-runner-timeout-s",
            "10",
        ],
        "ros": [
            "ros2",
            "launch",
            "manyforge_ros_bringup",
            "isaac_topic_based_control.launch.py",
            f"isaac_profile:={config.spec.profile}",
            f"runtime_namespace:=/manyforge/{config.spec.component_id}",
            f"command_gate_owner_namespace:=/manyforge/{config.spec.component_id}",
            f"world_uri:={config.spec.world_uri}",
            f"expected_world_name:={config.spec.world_name}",
            f"scene_startup_timeout_s:={config.startup_timeout_s}",
            "scene_service_timeout_s:="
            + str(min(config.startup_timeout_s, 120.0)),
            "controller_manager_lock_memory:=false",
            f"manager_state_root:={manager_root}",
            "pose_observer_site_config:="
            + str(
                config.source_root
                / "manyforge_ros_bringup/config"
                / config.spec.pose_observer_filename
            ),
        ],
    }
    if config.spec.scenario_id == "isaac-lite6-trocar-foundationpose":
        commands["ros"].append("additional_required_topics:=/tf")
    if composer_port is not None:
        commands["composer"] = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            "127.0.0.1",
            "--port",
            str(composer_port),
            "--program-path",
            str(config.package_root / config.spec.program_filename),
            "--static-dir",
            str(config.static_dir),
            "--headless-api-url",
            api_url,
            "--headless-api-timeout-s",
            "15",
        ]
    return commands


def _wait_for_runtime_node_presence(
    spec: ScenarioSpec,
    *,
    environment: Mapping[str, str],
    present: bool,
    timeout_s: float,
    started: float,
) -> float:
    expected = f"/manyforge/{spec.component_id}/controller_manager"
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        observed = _run_bounded(
            ["ros2", "node", "list"],
            environment=environment,
            timeout_s=10.0,
        )
        names = {
            line.strip()
            for line in observed["output"].splitlines()
            if line.strip().startswith("/")
        }
        if observed["returncode"] == 0 and ((expected in names) is present):
            return time.monotonic() - started
        time.sleep(0.05)
    state = "join" if present else "leave"
    raise LiveRowError(f"runtime node did not {state} the DDS graph: {expected}")


def _wait_for_foundationpose_loss(
    client: HeadlessHttpClient,
    *,
    source_epoch: str,
    timeout_s: float,
    started: float,
) -> float:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        scene = client.scene()
        if _foundationpose_loss_projected(scene, source_epoch=source_epoch):
            return time.monotonic() - started
        time.sleep(0.05)
    raise LiveRowError("FoundationPose provider loss was not projected")


def _foundationpose_loss_projected(
    scene: Mapping[str, Any], *, source_epoch: str
) -> bool:
    lifetime_rows = _foundationpose_lifetime_rows(scene)
    if len(lifetime_rows) > 1:
        return False
    fresh_rows = _foundationpose_source_rows(scene)
    if fresh_rows:
        return False
    if not lifetime_rows:
        return True
    lifetime = _foundationpose_identity(scene, require_fresh=False)
    return lifetime is not None and lifetime[0] == source_epoch


def _run_foundationpose_recovery(
    config: LiveConfig,
    *,
    api_url: str,
    original_ros: ManagedProcess,
    ros_command: Sequence[str],
    environment: Mapping[str, str],
    processes: list[ManagedProcess],
) -> tuple[ManagedProcess, dict[str, Any], dict[str, Any], RecoveryEvidence]:
    with HeadlessHttpClient(api_url, timeout_seconds=15.0) as client:
        try:
            playing_before = _simulation_snapshot(config.spec, play=True)
            if (
                playing_before.get("exact_world") is not True
                or playing_before.get("state", {}).get("state") != "playing"
                or playing_before.get("play", {}).get("success") is not True
            ):
                raise LiveRowError(
                    "FoundationPose readiness play transition was not exact"
                )
            before_epoch, before_sequence = _wait_for_foundationpose_identity(
                client, timeout_s=config.startup_timeout_s
            )
        finally:
            stopped_before_loss = _simulation_snapshot(config.spec, stop=True)
        if (
            stopped_before_loss.get("exact_world") is not True
            or stopped_before_loss.get("state", {}).get("state") != "stopped"
            or stopped_before_loss.get("stop", {}).get("success") is not True
        ):
            raise LiveRowError(
                "simulation did not stop after provider readiness probe"
            )

        loss_started = time.monotonic()
        original_ros.stop()
        dds_loss_elapsed = _wait_for_runtime_node_presence(
            config.spec,
            environment=environment,
            present=False,
            timeout_s=_RECOVERY_BOUND_S,
            started=loss_started,
        )
        provider_loss_elapsed = _wait_for_foundationpose_loss(
            client,
            source_epoch=before_epoch,
            timeout_s=_RECOVERY_BOUND_S,
            started=loss_started,
        )
        outage = _simulation_snapshot(config.spec, stop=True)

        rejoin_started = time.monotonic()
        replacement = _start_process(
            "ros-rejoin",
            ros_command,
            environment=environment,
            run_root=config.run_root,
        )
        processes.append(replacement)
        barrier = _wait_for_barrier(
            replacement,
            config.spec,
            timeout_s=(
                config.startup_timeout_s + _SCENE_BARRIER_CLEANUP_MARGIN_S
            ),
        )
        controllers = _wait_for_controllers(
            replacement,
            config.spec,
            environment=environment,
            timeout_s=config.startup_timeout_s,
        )
        dds_rejoin_elapsed = _wait_for_runtime_node_presence(
            config.spec,
            environment=environment,
            present=True,
            timeout_s=_RECOVERY_BOUND_S,
            started=rejoin_started,
        )
        output_sample = _start_topic_sample("/output", environment=environment)
        try:
            playing_after = _simulation_snapshot(config.spec, play=True)
            if (
                playing_after.get("exact_world") is not True
                or playing_after.get("state", {}).get("state") != "playing"
                or playing_after.get("play", {}).get("success") is not True
            ):
                raise LiveRowError(
                    "FoundationPose recovery play transition was not exact"
                )
            after_epoch, after_sequence = _wait_for_foundationpose_identity(
                client,
                timeout_s=config.startup_timeout_s,
                different_from_epoch=before_epoch,
            )
            provider_recovery_elapsed = time.monotonic() - rejoin_started
            sampled_output = output_sample.settle(timeout_s=5.0)
        finally:
            output_sample.stop()
            stopped_after = _simulation_snapshot(config.spec, stop=True)

    evidence = RecoveryEvidence(
        component_id=config.spec.component_id,
        provider_epoch_before=before_epoch,
        provider_sequence_before=before_sequence,
        provider_epoch_after=after_epoch,
        provider_sequence_after=after_sequence,
        dds_loss_elapsed_s=dds_loss_elapsed,
        dds_rejoin_elapsed_s=dds_rejoin_elapsed,
        provider_loss_elapsed_s=provider_loss_elapsed,
        provider_recovery_elapsed_s=provider_recovery_elapsed,
        output_sample=sampled_output,
        exact_world_after_rejoin=bool(
            barrier.get("world_name") == config.spec.world_name
            and barrier.get("world_uri") == config.spec.world_uri
            and stopped_after.get("exact_world") is True
        ),
        simulation_stopped_during_outage=bool(
            outage.get("exact_world") is True
            and outage.get("state", {}).get("state") == "stopped"
            and outage.get("stop", {}).get("success") is True
        ),
        simulation_stopped_after_rejoin=bool(
            stopped_after.get("state", {}).get("state") == "stopped"
            and stopped_after.get("stop", {}).get("success") is True
        ),
    )
    if evidence.to_dict()["passed"] is not True:
        raise LiveRowError(
            "FoundationPose recovery did not satisfy its bounded contract: "
            + json.dumps(evidence.to_dict(), separators=(",", ":"), sort_keys=True)
        )
    return replacement, barrier, controllers, evidence


def run_live_row(config: LiveConfig) -> dict[str, Any]:
    validate_config(config)
    started = time.monotonic()
    for directory in ("manager", "runtime", "publications"):
        (config.run_root / directory).mkdir()
    manager_root = config.run_root / "manager"
    runtime_root = config.run_root / "runtime"
    publication_root = config.run_root / "publications"
    api_port = _free_port()
    runner_port = _free_port()
    composer_port = config.composer_port
    commands = _process_commands(
        config,
        api_port=api_port,
        runner_port=runner_port,
        composer_port=composer_port,
        manager_root=manager_root,
        runtime_root=runtime_root,
        publication_root=publication_root,
    )
    environment = dict(os.environ)
    environment["ROS2CLI_NO_DAEMON"] = "1"
    environment["MANYFORGE_API_TOKEN"] = "i9-local-fixture"
    environment["MANYFORGE_FS_ROOTS"] = str(config.package_root)
    processes: list[ManagedProcess] = []
    samples: list[TopicSample] = []
    simulation_services_ready = False
    simulation_stopped = False
    try:
        manager = _start_process(
            "manager",
            commands["manager"],
            environment=environment,
            run_root=config.run_root,
        )
        processes.append(manager)
        program = _start_process(
            "program",
            commands["program"],
            environment=environment,
            run_root=config.run_root,
        )
        processes.append(program)
        headless = _start_process(
            "headless",
            commands["headless"],
            environment=environment,
            run_root=config.run_root,
        )
        processes.append(headless)
        api_url = f"http://127.0.0.1:{api_port}"
        runtime_initial = _wait_for_api(
            api_url,
            config.spec.scenario_id,
            processes,
            timeout_s=config.startup_timeout_s,
        )
        composer: ManagedProcess | None = None
        composer_url: str | None = None
        if "composer" in commands:
            composer = _start_process(
                "composer",
                commands["composer"],
                environment=environment,
                run_root=config.run_root,
            )
            processes.append(composer)
            composer_url = f"http://127.0.0.1:{composer_port}"
            _wait_for_composer(
                composer_url, composer, timeout_s=config.startup_timeout_s
            )
        ros = _start_process(
            "ros", commands["ros"], environment=environment, run_root=config.run_root
        )
        processes.append(ros)
        barrier = _wait_for_barrier(
            ros,
            config.spec,
            timeout_s=(
                config.startup_timeout_s + _SCENE_BARRIER_CLEANUP_MARGIN_S
            ),
        )
        simulation_services_ready = True
        controllers = _wait_for_controllers(
            ros,
            config.spec,
            environment=environment,
            timeout_s=config.startup_timeout_s,
        )
        recovery: RecoveryEvidence | None = None
        if config.spec.scenario_id == "isaac-lite6-trocar-foundationpose":
            ros, barrier, controllers, recovery = _run_foundationpose_recovery(
                config,
                api_url=api_url,
                original_ros=ros,
                ros_command=commands["ros"],
                environment=environment,
                processes=processes,
            )
        before_control = _simulation_snapshot(config.spec)
        program_definition = load_strict_json_yaml(
            config.package_root / config.spec.program_filename
        )
        if not isinstance(program_definition, dict):
            raise LiveRowError("selected Program is not an object")
        run_evidence = RunEvidence(
            gripper_targets=_extract_gripper_targets(program_definition)
        )
        tf_probe: dict[str, Any] | None = None
        with HeadlessHttpClient(api_url, timeout_seconds=15.0) as client:
            catalog = client.scenarios()
            scene_before = client.scene()
            _record_scene_evidence(run_evidence, scene_before)
            # The retained Lite6 gripper publishes feedback on playback ticks.
            # Playing is therefore part of the bounded readiness probe and the
            # immediately following active scenario, never an idle-state
            # workaround. The finally path still owns fail-safe Stop.
            active_control = _simulation_snapshot(config.spec, play=True)
            if (
                active_control.get("exact_world") is not True
                or active_control.get("state", {}).get("state") != "playing"
                or not isinstance(active_control.get("play"), dict)
                or active_control["play"].get("success") is not True
            ):
                raise LiveRowError(
                    "simulation did not enter playing for runtime readiness"
                )
            # Isaac advertises /clock only while playback is active. Read one
            # converged graph before starting echo observers so the command
            # subscription count cannot be satisfied by our own evidence
            # process.
            nodes, command_info, topic_types = _read_ros_graph(config.spec)
            topic_names = {
                "/clock",
                "/isaac_joint_commands",
                "/isaac_joint_states",
                (
                    "/isaac_joint_commands_gripper"
                    if config.spec.profile == "lite6"
                    else "/isaac_gripper_state"
                ),
            }
            if config.spec.scenario_id == "isaac-lite6-trocar-foundationpose":
                topic_names.add("/output")
            samples = [
                _start_topic_sample(topic, environment=environment)
                for topic in sorted(topic_names)
            ]
            applied = _apply_program(
                client,
                config.spec,
                program_definition,
                catalog,
                timeout_s=config.startup_timeout_s,
            )
            cclc_startup = _prepare_and_select_cclc_routes(
                client,
                config.spec,
                timeout_s=config.startup_timeout_s,
            )
            program_snapshot = _run_program(
                client,
                config.spec,
                catalog,
                applied,
                run_evidence,
                timeout_s=config.run_timeout_s,
            )
            program_run_id = _program_run_id(program_snapshot)
            if program_run_id is None:
                raise LiveRowError("terminal Program Run lost its owner identity")
            alarms, history = _wait_for_trace_evidence(
                client,
                config.spec,
                program_run_id,
                timeout_s=config.startup_timeout_s,
            )
            if config.spec.scenario_id == "isaac-lite6-trocar-foundationpose":
                tf_probe = _run_bounded(
                    [
                        "timeout",
                        "5s",
                        "ros2",
                        "run",
                        "tf2_ros",
                        "tf2_echo",
                        "link_base",
                        "camera_color_optical_frame",
                    ],
                    environment=environment,
                    timeout_s=8.0,
                )
            after_control = _simulation_snapshot(
                config.spec, reset=True, stop=True
            )
            simulation_stopped = bool(
                after_control.get("state", {}).get("state") == "stopped"
                and isinstance(after_control.get("stop"), dict)
                and after_control["stop"].get("success") is True
            )
            runtime = client.runtime()
            scene_after = client.scene()
            _record_scene_evidence(run_evidence, scene_after)
            viewer_cell_id: str | None = None
            if composer_url is not None:
                viewer_cell_id = client.viewer_model_urdf().cell_id
                assistant = run_snapshot_probe(composer_url)
            else:
                assistant = None
        sample_results = {sample.topic: sample.settle() for sample in samples}
        samples.clear()
        checks = _build_checks(
            config.spec,
            barrier=barrier,
            before_control=before_control,
            active_control=active_control,
            after_control=after_control,
            controllers=controllers,
            nodes=nodes,
            command_info=command_info,
            cclc_startup=cclc_startup,
            samples=sample_results,
            topic_types=topic_types,
            program=program_snapshot,
            run_evidence=run_evidence,
            runtime=runtime,
            scene_before=scene_before,
            scene_after=scene_after,
            alarms=alarms,
            history=history,
            tf_probe=tf_probe,
            assistant=assistant,
            viewer_cell_id=viewer_cell_id,
        )
        details = {
            "assistant": None if assistant is None else _summary(assistant),
            "barrier": _summary(barrier),
            "cclc_startup": cclc_startup,
            "controllers": {
                key: controllers[key]
                for key in ("bytes", "returncode", "sha256")
            },
            "headless_runtime_initial": _summary(runtime_initial),
            "headless_runtime_terminal": _summary(runtime),
            "history": _summary(history),
            "operator_alarms": _summary(alarms),
            "program": _summary(program_snapshot),
            "recovery": None if recovery is None else recovery.to_dict(),
            "run_evidence": run_evidence.summary(),
            "ros_nodes": {
                key: nodes[key] for key in ("bytes", "returncode", "sha256")
            },
            "ros_samples": sample_results,
            "ros_topic_types": topic_types,
            "scene_after": _summary(scene_after),
            "scene_before": _summary(scene_before),
            "simulation_after": after_control,
            "simulation_active": active_control,
            "simulation_before": before_control,
            "tf_probe": None if tf_probe is None else {
                key: tf_probe[key] for key in ("bytes", "returncode", "sha256")
            },
        }
        row = {
            "schema_version": ROW_SCHEMA,
            "scenario_id": config.spec.scenario_id,
            "component_id": config.spec.component_id,
            "world": {
                "name": config.spec.world_name,
                "sha256": config.spec.world_sha256,
                "uri": config.spec.world_uri,
            },
            "checks": checks,
            "details": details,
            "elapsed_s": round(time.monotonic() - started, 3),
        }
        return validate_row(row)
    finally:
        if simulation_services_ready and not simulation_stopped:
            with suppress(Exception):
                _simulation_snapshot(config.spec, stop=True)
        for sample in reversed(samples):
            with suppress(Exception):
                sample.stop()
        for process in reversed(processes):
            with suppress(Exception):
                process.stop()


def _default_paths(source_root: Path) -> dict[str, Path]:
    return {
        "alarm_catalog": source_root
        / "manyforge_headless/resources/operator-alarm-catalog.v1.yaml",
        "node_catalog": source_root
        / "manyforge_behavior/resources/node_catalog.yaml",
        "static_dir": source_root / "manyforge_composer/frontend/dist",
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=tuple(SCENARIO_BY_ID), required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--installed-root", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--node-catalog", type=Path)
    parser.add_argument("--alarm-catalog", type=Path)
    parser.add_argument("--static-dir", type=Path)
    parser.add_argument("--startup-timeout-s", type=float, default=180.0)
    parser.add_argument("--run-timeout-s", type=float, default=300.0)
    parser.add_argument("--composer-port", type=int, default=9000)
    args = parser.parse_args(argv)
    source_root = args.source_root.resolve()
    defaults = _default_paths(source_root)
    spec = SCENARIO_BY_ID[args.scenario]
    composer_port = (
        args.composer_port
        if args.scenario == "isaac-ur10e-light-scene-assistant"
        else None
    )
    config = LiveConfig(
        spec=spec,
        source_root=source_root,
        package_root=args.package_root.resolve(),
        installed_root=args.installed_root.resolve(),
        run_root=args.run_root.resolve(),
        node_catalog=(args.node_catalog or defaults["node_catalog"]).resolve(),
        alarm_catalog=(args.alarm_catalog or defaults["alarm_catalog"]).resolve(),
        static_dir=(args.static_dir or defaults["static_dir"]).resolve(),
        startup_timeout_s=args.startup_timeout_s,
        run_timeout_s=args.run_timeout_s,
        composer_port=composer_port,
    )
    try:
        row = run_live_row(config)
    except (LiveRowError, QualificationError, HeadlessClientError, OSError) as error:
        print(f"I9 live row failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(row, separators=(",", ":"), sort_keys=True))
    return 0 if all(row["checks"].values()) else 2


if __name__ == "__main__":
    raise SystemExit(main())
