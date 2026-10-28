"""CLI for the source-matched native and split-Isaac operator boundary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .contracts import LaunchOperation, LaunchProfile, OperatorCommandResult
from .isaac import (
    IsaacLauncher,
    IsaacLauncherConfig,
    RemoteNativeConfig,
)
from .orchestrator import (
    DEFAULT_STATE_ROOT,
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-operator")
    parser.add_argument("--state-root", type=Path, default=Path.cwd() / DEFAULT_STATE_ROOT)
    parser.add_argument("--instance-id", default="native")
    parser.add_argument("--json", action="store_true", dest="json_output")
    commands = parser.add_subparsers(dest="operation", required=True)
    start = commands.add_parser("start")
    start.add_argument("--profile", choices=[item.value for item in LaunchProfile], default="native")
    start.add_argument("--source-root", type=Path, required=True)
    start.add_argument("--manager-executable", type=Path)
    start.add_argument("--install-root", type=Path)
    start.add_argument("--node-catalog-path", type=Path)
    start.add_argument("--alarm-catalog-path", type=Path)
    start.add_argument("--composer-static-dir", type=Path)
    start.add_argument("--simulation-control-config", type=Path)
    start.add_argument(
        "--canonical-empty-admission-filename",
        default="canonical-empty-admission.json",
    )
    start.add_argument("--cell-id", default="native")
    start.add_argument("--package-aggregate-sha256")
    start.add_argument("--compiler-build-manifest-sha256")
    start.add_argument("--node-environment-sha256")
    start.add_argument("--advertised-host")
    start.add_argument("--expected-source-fingerprint")
    start.add_argument("--expected-head-commit")
    start.add_argument("--expected-head-tree")
    start.add_argument("--readiness-timeout-s", type=float, default=60.0)
    start.add_argument("--poll-interval-s", type=float, default=0.25)
    start.add_argument("--reconciliation-period-ms", type=int, default=500)
    start.add_argument("--manager-timeout-s", type=float, default=60.0)
    start.add_argument("--joint-commands-topic",
                       help="Arm command topic from the retained host profile; omission keeps the launch default")
    start.add_argument("--simulator-host")
    start.add_argument("--control-host", default="")
    start.add_argument("--remote-target")
    start.add_argument("--remote-source-root")
    start.add_argument("--remote-manager-executable")
    start.add_argument("--remote-install-root")
    start.add_argument("--remote-node-catalog-path")
    start.add_argument("--remote-alarm-catalog-path")
    start.add_argument("--remote-state-root")
    start.add_argument("--remote-composer-static-dir")
    start.add_argument("--remote-container-image-id")
    start.add_argument("--remote-container-name")
    start.add_argument("--remote-cache-root")
    start.add_argument("--remote-runtime-root")
    start.add_argument("--remote-evidence-root")
    start.add_argument("--remote-simulation-control-config")
    start.add_argument("--remote-bridge-state-root")
    start.add_argument("--remote-ros-setup-script")
    start.add_argument("--remote-ros-environment-script")
    start.add_argument("--remote-python-executable", default="python3")
    start.add_argument("--static-peer", action="append", dest="static_peers")
    commands.add_parser("status")
    commands.add_parser("stop")
    return parser


def _result(
    operation: LaunchOperation,
    *,
    outcome: str,
    status: object | None = None,
    code: str | None = None,
    message: str | None = None,
) -> dict:
    value = {
        "schema_version": "manyforge.operator_command_result.v1",
        "operation": operation.value,
        "outcome": outcome,
    }
    if status is not None:
        value["status"] = status.model_dump(mode="json")
    if code is not None:
        value["error"] = {"code": code, "message": message or code}
    OperatorCommandResult.model_validate_json(json.dumps(value, ensure_ascii=False))
    return value


def _config(args: argparse.Namespace) -> NativeLauncherConfig:
    required = (
        "manager_executable",
        "install_root",
        "node_catalog_path",
        "alarm_catalog_path",
        "package_aggregate_sha256",
        "compiler_build_manifest_sha256",
        "node_environment_sha256",
    )
    missing = [name.replace("_", "-") for name in required if getattr(args, name, None) is None]
    if missing:
        raise ValueError(f"native profile requires: {', '.join(missing)}")
    return NativeLauncherConfig(
        source_root=args.source_root,
        manager_executable=args.manager_executable,
        install_root=args.install_root,
        node_catalog_path=args.node_catalog_path,
        alarm_catalog_path=args.alarm_catalog_path,
        composer_static_dir=args.composer_static_dir,
        simulation_control_config=args.simulation_control_config,
        canonical_empty_admission_filename=args.canonical_empty_admission_filename,
        cell_id=args.cell_id,
        state_root=args.state_root,
        advertised_host=args.advertised_host or "localhost",
        instance_id=args.instance_id,
        package_aggregate_sha256=args.package_aggregate_sha256,
        compiler_build_manifest_sha256=args.compiler_build_manifest_sha256,
        node_environment_sha256=args.node_environment_sha256,
        expected_source_fingerprint=args.expected_source_fingerprint,
        expected_head_commit=args.expected_head_commit,
        expected_head_tree=args.expected_head_tree,
        readiness_timeout_seconds=args.readiness_timeout_s,
        poll_interval_seconds=args.poll_interval_s,
        reconciliation_period_ms=args.reconciliation_period_ms,
        manager_timeout_seconds=args.manager_timeout_s,
    )


def _isaac_config(args: argparse.Namespace) -> IsaacLauncherConfig:
    required = (
        "remote_target",
        "advertised_host",
        "simulator_host",
        "static_peers",
        "remote_source_root",
        "remote_container_image_id",
        "remote_container_name",
        "remote_cache_root",
        "remote_runtime_root",
        "remote_evidence_root",
        "remote_manager_executable",
        "remote_install_root",
        "remote_node_catalog_path",
        "remote_alarm_catalog_path",
        "remote_state_root",
        "remote_simulation_control_config",
        "remote_bridge_state_root",
        "remote_ros_setup_script",
        "remote_ros_environment_script",
        "package_aggregate_sha256",
        "compiler_build_manifest_sha256",
        "node_environment_sha256",
    )
    flags = {"static_peers": "static-peer"}
    missing = [
        flags.get(name, name.replace("_", "-"))
        for name in required
        if getattr(args, name, None) is None
    ]
    if missing:
        raise ValueError(f"isaac profile requires: {', '.join(missing)}")
    remote = RemoteNativeConfig(
        target=args.remote_target,
        source_root=args.remote_source_root,
        manager_executable=args.remote_manager_executable,
        install_root=args.remote_install_root,
        node_catalog_path=args.remote_node_catalog_path,
        alarm_catalog_path=args.remote_alarm_catalog_path,
        state_root=args.remote_state_root,
        composer_static_dir=args.remote_composer_static_dir or "",
        container_image_id=args.remote_container_image_id,
        container_name=args.remote_container_name,
        cache_root=args.remote_cache_root,
        runtime_root=args.remote_runtime_root,
        evidence_root=args.remote_evidence_root,
        simulation_control_config=args.remote_simulation_control_config,
        bridge_state_root=args.remote_bridge_state_root,
        ros_setup_script=args.remote_ros_setup_script,
        ros_environment_script=args.remote_ros_environment_script,
        python_executable=args.remote_python_executable,
        advertised_host=args.advertised_host,
        canonical_empty_admission_filename=args.canonical_empty_admission_filename,
        cell_id=args.cell_id,
        package_aggregate_sha256=args.package_aggregate_sha256,
        compiler_build_manifest_sha256=args.compiler_build_manifest_sha256,
        node_environment_sha256=args.node_environment_sha256,
    )
    peers = tuple(sorted(set(args.static_peers)))
    return IsaacLauncherConfig(
        source_root=args.source_root,
        joint_commands_topic=args.joint_commands_topic,
        state_root=args.state_root,
        instance_id=args.instance_id,
        remote=remote,
        simulator_host=args.simulator_host,
        control_host=args.control_host,
        static_peers=peers,
        expected_source_fingerprint=args.expected_source_fingerprint,
        expected_head_commit=args.expected_head_commit,
        expected_head_tree=args.expected_head_tree,
        readiness_timeout_seconds=args.readiness_timeout_s,
        poll_interval_seconds=args.poll_interval_s,
        manager_timeout_seconds=args.manager_timeout_s,
    )


def _persisted_profile(state_root: Path, instance_id: str) -> LaunchProfile | None:
    instance_root = state_root.absolute() / instance_id
    if (instance_root / ".manyforge-cell-layout.json").is_file():
        record = instance_root / "state" / "operator" / "launch_instance.json"
    else:
        record = instance_root / "launch_instance.json"
    if not record.is_file():
        return None
    try:
        envelope = json.loads(record.read_text(encoding="utf-8"))
        profile = envelope["status"]["configuration"]["profile"]
        return LaunchProfile(profile)
    except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise LauncherError("status_record_invalid", str(error), exit_code=70) from error


def _emit(value: dict, *, json_output: bool) -> None:
    if json_output:
        print(json.dumps(value, ensure_ascii=False, sort_keys=True))
    else:
        status = value.get("status") or {}
        if isinstance(status, dict):
            print(f"{value.get('operation')}: {value.get('outcome')} ({status.get('state', 'unavailable')})")
        else:
            print(f"{value.get('operation')}: {value.get('outcome')}")
        if "error" in value:
            print(f"{value['error']['code']}: {value['error']['message']}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    operation = LaunchOperation(args.operation)
    try:
        if operation is LaunchOperation.START:
            if args.profile == LaunchProfile.ISAAC.value:
                launcher = IsaacLauncher(_isaac_config(args))
            else:
                launcher = NativeLauncher(_config(args))
            try:
                status = launcher.start()
                value = _result(
                    operation,
                    outcome="already_running" if launcher.last_start_idempotent else "started",
                    status=status,
                )
                _emit(value, json_output=args.json_output)
                return 0
            except LauncherError as error:
                value = _result(operation, outcome="failed", status=error.status, code=error.code, message=str(error))
                _emit(value, json_output=args.json_output)
                return error.exit_code
        profile = _persisted_profile(args.state_root, args.instance_id)
        launcher = (
            IsaacLauncher.from_state_root(args.state_root, args.instance_id)
            if profile is LaunchProfile.ISAAC
            else NativeLauncher.from_state_root(args.state_root, args.instance_id)
        )
        status = launcher.status() if operation is LaunchOperation.STATUS else launcher.stop()
        if status is None:
            value = _result(operation, outcome="failed", code="instance_unavailable", message="no persisted launch instance exists")
            _emit(value, json_output=args.json_output)
            return 70
        if operation is LaunchOperation.STATUS:
            outcome = "reported"
        else:
            outcome = "already_stopped" if launcher.last_stop_already_stopped else "stopped"
        value = _result(operation, outcome=outcome, status=status)
        _emit(value, json_output=args.json_output)
        return 0
    except LauncherError as error:
        value = _result(operation, outcome="failed", status=error.status, code=error.code, message=str(error))
        _emit(value, json_output=args.json_output)
        return error.exit_code
    except (OSError, ValueError) as error:
        value = _result(operation, outcome="failed", code="operator_configuration_invalid", message=str(error))
        _emit(value, json_output=args.json_output)
        return 64


__all__ = ["main"]
