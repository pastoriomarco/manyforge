"""CLI for the O1.3 native operator boundary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .contracts import LaunchOperation, LaunchProfile, OperatorCommandResult
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
    start.add_argument("--manager-executable", type=Path, required=True)
    start.add_argument("--package-root", type=Path, required=True)
    start.add_argument("--node-catalog-path", type=Path, required=True)
    start.add_argument("--alarm-catalog-path", type=Path, required=True)
    start.add_argument("--publication-root", type=Path, required=True)
    start.add_argument("--manager-state-root", type=Path, required=True)
    start.add_argument("--runtime-state-root", type=Path, required=True)
    start.add_argument("--execution-state-root", type=Path, required=True)
    start.add_argument("--logical-scene-database", type=Path, required=True)
    start.add_argument("--historian-database-path", type=Path, required=True)
    start.add_argument("--package-aggregate-sha256", required=True)
    start.add_argument("--compiler-build-manifest-sha256", required=True)
    start.add_argument("--node-environment-sha256", required=True)
    start.add_argument("--advertised-host", default="localhost")
    start.add_argument("--expected-source-fingerprint")
    start.add_argument("--expected-head-commit")
    start.add_argument("--expected-head-tree")
    start.add_argument("--readiness-timeout-s", type=float, default=60.0)
    start.add_argument("--poll-interval-s", type=float, default=0.25)
    start.add_argument("--reconciliation-period-ms", type=int, default=20)
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
    return NativeLauncherConfig(
        source_root=args.source_root,
        manager_executable=args.manager_executable,
        package_root=args.package_root,
        node_catalog_path=args.node_catalog_path,
        alarm_catalog_path=args.alarm_catalog_path,
        publication_root=args.publication_root,
        manager_state_root=args.manager_state_root,
        runtime_state_root=args.runtime_state_root,
        execution_state_root=args.execution_state_root,
        logical_scene_database=args.logical_scene_database,
        historian_database_path=args.historian_database_path,
        state_root=args.state_root,
        advertised_host=args.advertised_host,
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
    )


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
                value = _result(
                    operation,
                    outcome="failed",
                    code="isaac_profile_unavailable",
                    message="Isaac native split lifecycle is deferred to O1.4",
                )
                _emit(value, json_output=args.json_output)
                return 78
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
        launcher = NativeLauncher.from_state_root(args.state_root, args.instance_id)
        status = launcher.status() if operation is LaunchOperation.STATUS else launcher.stop()
        if status is None:
            value = _result(operation, outcome="failed", code="instance_unavailable", message="no persisted launch instance exists")
            _emit(value, json_output=args.json_output)
            return 0 if operation is LaunchOperation.STOP else 70
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
