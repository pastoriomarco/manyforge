"""Thin command-line client and explicit headless API server launcher."""

from __future__ import annotations

import argparse
import os
import json
from pathlib import Path
import sys
from typing import Any, Sequence

import uvicorn

from .app import create_app
from . import history_export
from .client import HeadlessClientError, HeadlessHttpClient
from .component_model import COMPONENT_ACTIONS, enable_all_targets, prepare_all_targets
from .identity import parse_application_start_id
from .internal_http import INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS
from .logging_setup import LOG_LEVELS, setup_logging
from .native import (
    DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    HeadlessApiConfig,
    HeadlessApiError,
)


def _print(value: Any) -> None:
    print(
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
    )


def _application_start_id(text: str) -> str:
    try:
        parse_application_start_id(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    return text


def _caller_fields(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--request-key", required=True)
    parser.add_argument(
        "--application-start-id", type=_application_start_id, required=True
    )
    parser.add_argument("--source-label", default="manyforge-headless-cli")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-headless")
    subcommands = parser.add_subparsers(dest="command", required=True)

    serve = subcommands.add_parser("serve", help="start the independent headless API")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=9010)
    serve.add_argument("--install-root", type=Path, required=True)
    serve.add_argument("--artifact-root", type=Path, required=True)
    serve.add_argument("--node-catalog-path", type=Path, required=True)
    serve.add_argument(
        "--node-environment-manifest-path", type=Path, required=True
    )
    serve.add_argument("--alarm-catalog-path", type=Path, required=True)
    serve.add_argument("--historian-directory", type=Path, required=True)
    serve.add_argument("--workbench-previous-session-reports-root", type=Path)
    serve.add_argument("--workbench-previous-session-id")
    serve.add_argument("--manager-run-root", type=Path, required=True)
    serve.add_argument("--execution-run-root", type=Path, required=True)
    # The disk check (OD-215): the free-space floor of the file systems that
    # hold the records (--historian-directory and the parent of
    # --execution-run-root), and the margin above it at which it clears.
    # Both launchers pass them.
    serve.add_argument("--disk-free-floor-bytes", type=int)
    serve.add_argument("--disk-free-margin-bytes", type=int)
    serve.add_argument("--cell-id", required=True)
    # Contract 1.1: the Manager appends it to every child's argv.
    serve.add_argument(
        "--application-start-id", type=_application_start_id, required=True
    )
    # OD-216: how long the Manager's status stream may show no new tick of
    # its main loop before the Manager counts as not answering.
    serve.add_argument("--freshness-ms", type=int, default=2000)
    serve.add_argument(
        "--manager-timeout-s",
        type=float,
        default=DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    )
    serve.add_argument("--program-runner-socket-root", type=Path)
    serve.add_argument("--program-runner-timeout-s", type=float, default=30.0)
    serve.add_argument("--activation-preparer", type=Path)
    serve.add_argument("--activation-preparer-capability-root", type=Path)
    serve.add_argument("--activation-preparer-activation-root", type=Path)
    serve.add_argument("--canonical-empty-admission-filename", type=Path)
    serve.add_argument(
        "--activation-preparer-timeout-s", type=float, default=120.0
    )
    # Structured JSON lines on stderr.
    serve.add_argument("--log-level", default="INFO", choices=LOG_LEVELS)

    for name in (
        "health",
        "runtime",
        "scene",
        "manager-status",
        "manager-graph",
        "manager-diff",
        "node-environment",
        "source-model",
        "alarm-metrics",
        "historian-health",
    ):
        command = subcommands.add_parser(name)
        command.add_argument("--url", default="http://127.0.0.1:9010")

    subcommands.add_parser("assistant-modes").add_argument(
        "--url", default="http://127.0.0.1:9010"
    )
    assistant_mode = subcommands.add_parser("assistant-mode")
    assistant_mode.add_argument("--url", default="http://127.0.0.1:9010")
    assistant_mode.add_argument("mode_id")

    alarms = subcommands.add_parser("alarms")
    alarms.add_argument("--url", default="http://127.0.0.1:9010")
    alarms.add_argument("--limit", type=int)
    alarms.add_argument("--state", action="append", default=[])

    history = subcommands.add_parser("history")
    history.add_argument("--url", default="http://127.0.0.1:9010")
    history.add_argument("--start-ns", type=int)
    history.add_argument("--end-ns", type=int)
    history.add_argument("--component", action="append", default=[])
    history.add_argument("--event-type", action="append", default=[])
    history.add_argument("--before-sequence", type=int)
    history.add_argument("--limit", type=int, default=32)
    history_export.add_parser(history.add_subparsers(dest="history_command"))

    acknowledge = subcommands.add_parser("acknowledge-alarm")
    acknowledge.add_argument("--url", default="http://127.0.0.1:9010")
    acknowledge.add_argument("condition_id")
    acknowledge.add_argument("activation_id")
    acknowledge.add_argument("expected_record_revision", type=int)
    acknowledge.add_argument("--reason")
    acknowledge.add_argument("--confirmed", action="store_true")
    _caller_fields(acknowledge)

    acknowledge_all = subcommands.add_parser("acknowledge-all-alarms")
    acknowledge_all.add_argument("--url", default="http://127.0.0.1:9010")
    acknowledge_all.add_argument("--request-key", required=True)
    acknowledge_all.add_argument("--source-label", default="manyforge-headless-cli")
    acknowledge_all.add_argument("--reason")
    acknowledge_all.add_argument("--confirmed", action="store_true")

    # Component model v2, through the REST endpoints.
    components = subcommands.add_parser("components")
    components.add_argument("--url", default="http://127.0.0.1:9010")
    component_commands = components.add_subparsers(dest="components_command", required=True)
    component_commands.add_parser("list")
    component_action = component_commands.add_parser("action")
    component_action.add_argument("component_id")
    component_action.add_argument("action", choices=sorted(COMPONENT_ACTIONS))
    component_commands.add_parser("enable-all")
    component_commands.add_parser("prepare-all")

    io = subcommands.add_parser("io")
    io.add_argument("--url", default="http://127.0.0.1:9010")
    io_commands = io.add_subparsers(dest="io_command", required=True)
    io_commands.add_parser("list")
    io_set = io_commands.add_parser("set")
    io_set.add_argument("signal_id", help="<component_id>/<signal_name>")
    io_set.add_argument("value", type=_output_value, help="true, false or a number")

    subcommands.add_parser("readiness").add_argument(
        "--url", default="http://127.0.0.1:9010"
    )

    return parser


def _output_value(text: str) -> bool | float:
    if text.lower() in ("true", "false"):
        return text.lower() == "true"
    return float(text)


def _components(client: HeadlessHttpClient, args: argparse.Namespace) -> Any:
    command = args.components_command
    if command == "list":
        return client.components()
    if command == "action":
        return client.component_action(args.component_id, args.action)
    states = client.components()
    action, targets = (
        ("enable", enable_all_targets(states))
        if command == "enable-all"
        else ("prepare", prepare_all_targets(states))
    )
    if not targets:
        return {"action": action, "component_ids": []}
    return {**client.component_actions(action, targets), "component_ids": targets}


def _caller(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "request_key": args.request_key,
        "application_start_id": args.application_start_id,
        "source_label": args.source_label,
    }


def _serve(args: argparse.Namespace) -> int:
    setup_logging(args.log_level, process="headless_api")
    config = HeadlessApiConfig(
        install_root=args.install_root,
        artifact_root=args.artifact_root,
        node_catalog_path=args.node_catalog_path,
        node_environment_manifest_path=args.node_environment_manifest_path,
        alarm_catalog_path=args.alarm_catalog_path,
        historian_directory=args.historian_directory,
        workbench_previous_session_reports_root=args.workbench_previous_session_reports_root,
        workbench_previous_session_id=args.workbench_previous_session_id,
        manager_run_root=args.manager_run_root,
        execution_run_root=args.execution_run_root,
        cell_id=args.cell_id,
        application_start_id=args.application_start_id,
        freshness_milliseconds=args.freshness_ms,
        manager_timeout_seconds=args.manager_timeout_s,
        program_runner_socket_root=args.program_runner_socket_root,
        program_runner_timeout_seconds=args.program_runner_timeout_s,
        activation_preparer_path=args.activation_preparer,
        activation_preparer_capability_root=(
            args.activation_preparer_capability_root
        ),
        activation_preparer_activation_root=(
            args.activation_preparer_activation_root
        ),
        activation_preparer_timeout_seconds=(
            args.activation_preparer_timeout_s
        ),
        canonical_empty_admission_filename=args.canonical_empty_admission_filename,
        disk_free_floor_bytes=args.disk_free_floor_bytes,
        disk_free_margin_bytes=args.disk_free_margin_bytes,
        scene_mesh_allowed_roots=tuple(
            root.strip()
            for root in os.environ.get("MANYFORGE_FS_ROOTS", "").split(",")
            if root.strip()
        ),
    )
    # Internal request success is not a runtime event. Semantic owner state,
    # warnings, failures, and journals remain observable without a log line
    # for every high-rate observation sample.
    uvicorn.run(
        create_app(config),
        host=args.host,
        port=args.port,
        access_log=False,
        timeout_keep_alive=INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS,
        # The Headless real-time channels carry small bounded messages. Avoid
        # per-frame deflate work on the same event loop that admits Stop.
        ws_per_message_deflate=False,
    )
    return 0


def _request(args: argparse.Namespace) -> int:
    with HeadlessHttpClient(args.url) as client:
        if args.command == "health":
            value = client.health()
        elif args.command == "runtime":
            value = client.runtime()
        elif args.command == "scene":
            value = client.scene()
        elif args.command == "manager-status":
            value = client.manager_status()
        elif args.command == "manager-graph":
            value = client.manager_graph()
        elif args.command == "manager-diff":
            value = client.manager_diff()
        elif args.command == "node-environment":
            value = client.node_environment()
        elif args.command == "source-model":
            value = client.source_model()
        elif args.command == "assistant-modes":
            value = client.assistant_modes()
        elif args.command == "assistant-mode":
            value = client.assistant_mode(args.mode_id)
        elif args.command == "alarm-metrics":
            value = client.alarm_metrics()
        elif args.command == "historian-health":
            value = client.historian_health()
        elif args.command == "alarms":
            value = client.alarms(limit=args.limit, states=args.state)
        elif args.command == "history":
            params: list[tuple[str, str | int]] = [("limit", args.limit)]
            for name in ("start_ns", "end_ns", "before_sequence"):
                item = getattr(args, name)
                if item is not None:
                    params.append((name, item))
            params.extend(("components", item) for item in args.component)
            params.extend(("event_types", item) for item in args.event_type)
            value = client.history(params=params)
        elif args.command == "acknowledge-alarm":
            value = client.acknowledge_alarm(
                args.condition_id,
                {
                    **_caller(args),
                    "activation_id": args.activation_id,
                    "expected_record_revision": args.expected_record_revision,
                    "reason": args.reason,
                    "confirmed": args.confirmed,
                },
            )
        elif args.command == "acknowledge-all-alarms":
            snapshot = client.alarms(limit=0)
            value = client.acknowledge_all(
                {
                    "request_key": args.request_key,
                    "source_label": args.source_label,
                    "members": snapshot["unacknowledged_membership"],
                    "expected_generation": snapshot["generation"],
                    "reason": args.reason,
                    "confirmed": args.confirmed,
                }
            )
        elif args.command == "components":
            value = _components(client, args)
        elif args.command == "io":
            value = (
                client.io_signals()
                if args.io_command == "list"
                else client.set_io_output(args.signal_id, args.value)
            )
        elif args.command == "readiness":
            value = client.program_readiness()
        else:
            raise AssertionError(f"unsupported command {args.command}")
    _print(value)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if getattr(args, "history_command", None) == "export":
        return history_export.run(args)
    try:
        return _serve(args) if args.command == "serve" else _request(args)
    except (HeadlessApiError, HeadlessClientError, ValueError) as exc:
        code = getattr(exc, "code", "headless_cli_invalid")
        print(f"{code}: {exc}", file=sys.stderr)
        return 69 if "unavailable" in code else 65


if __name__ == "__main__":
    raise SystemExit(main())
