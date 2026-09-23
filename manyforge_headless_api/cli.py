"""Thin command-line client and explicit headless API server launcher."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Sequence

import uvicorn

from .app import create_app
from .cclc import MAX_CCLC_REQUEST_BYTES
from .client import HeadlessClientError, HeadlessHttpClient
from .native import (
    DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    HeadlessApiConfig,
    HeadlessApiError,
    strict_json_object,
)


def _print(value: dict[str, Any]) -> None:
    print(
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
    )


def _caller_fields(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--request-owner-epoch", required=True)
    parser.add_argument("--source-label", default="manyforge-headless-cli")
    parser.add_argument("--correlation-id")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-headless")
    subcommands = parser.add_subparsers(dest="command", required=True)

    serve = subcommands.add_parser("serve", help="start the independent H0 API")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=9010)
    serve.add_argument("--projection-root", type=Path, required=True)
    serve.add_argument("--install-root", type=Path, required=True)
    serve.add_argument("--artifact-root", type=Path, required=True)
    serve.add_argument("--node-catalog-path", type=Path, required=True)
    serve.add_argument(
        "--node-environment-manifest-path", type=Path, required=True
    )
    serve.add_argument("--alarm-catalog-path", type=Path, required=True)
    serve.add_argument("--historian-database-path", type=Path, required=True)
    serve.add_argument("--manager-run-root", type=Path, required=True)
    serve.add_argument("--execution-run-root", type=Path, required=True)
    serve.add_argument("--cell-id", required=True)
    serve.add_argument("--resource-id", action="append", default=[])
    serve.add_argument("--owner-id", default="headless-api")
    serve.add_argument("--freshness-ms", type=int, default=2000)
    serve.add_argument(
        "--manager-timeout-s",
        type=float,
        default=DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    )
    serve.add_argument("--cclc-timeout-s", type=float, default=5.0)
    serve.add_argument("--program-runner-url")
    serve.add_argument("--program-runner-timeout-s", type=float, default=30.0)
    serve.add_argument("--activation-preparer", type=Path)
    serve.add_argument("--activation-preparer-capability-root", type=Path)
    serve.add_argument("--activation-preparer-activation-root", type=Path)
    serve.add_argument(
        "--activation-preparer-timeout-s", type=float, default=120.0
    )

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
    acknowledge_all.add_argument("--request-id", required=True)
    acknowledge_all.add_argument("--source-label", default="manyforge-headless-cli")
    acknowledge_all.add_argument("--correlation-id")
    acknowledge_all.add_argument("--reason")
    acknowledge_all.add_argument("--confirmed", action="store_true")

    reconcile = subcommands.add_parser("reconcile-historian")
    reconcile.add_argument("--url", default="http://127.0.0.1:9010")
    reconcile.add_argument("expected_health_generation", type=int)
    _caller_fields(reconcile)

    apply = subcommands.add_parser("apply")
    apply.add_argument("--url", default="http://127.0.0.1:9010")
    apply.add_argument("deployment_artifact_id")
    apply.add_argument("deployment_revision")
    apply.add_argument("expected_graph_revision")
    apply.add_argument("expected_retained_bundle_id")
    # The exact UA2A prepared activation accepted by this Apply. The Manager
    # records these facts and never reopens a mutable Scenario/catalog
    # selection, so the caller must name the input it accepted.
    apply.add_argument("captured_input_digest")
    apply.add_argument("program_binding_id")
    apply.add_argument("prepared_activation_id")
    _caller_fields(apply)

    for name in ("cclc-component", "cclc-route-templates"):
        cclc_component = subcommands.add_parser(name)
        cclc_component.add_argument(
            "--url", default="http://127.0.0.1:9010"
        )
        cclc_component.add_argument("component_id")

    for name in ("cclc-lifecycle-submit", "cclc-route-submit"):
        cclc_submit = subcommands.add_parser(name)
        cclc_submit.add_argument("--url", default="http://127.0.0.1:9010")
        cclc_submit.add_argument("request_file", type=Path)
        cclc_submit.add_argument("--confirmed", action="store_true")

    for name in ("cclc-lifecycle-status", "cclc-route-status"):
        cclc_status = subcommands.add_parser(name)
        cclc_status.add_argument("--url", default="http://127.0.0.1:9010")
        cclc_status.add_argument("transaction_id")
    return parser


def _caller(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "request_id": args.request_id,
        "request_owner_epoch": args.request_owner_epoch,
        "source_label": args.source_label,
        "correlation_id": args.correlation_id,
    }


def _serve(args: argparse.Namespace) -> int:
    config = HeadlessApiConfig(
        projection_root=args.projection_root,
        install_root=args.install_root,
        artifact_root=args.artifact_root,
        node_catalog_path=args.node_catalog_path,
        node_environment_manifest_path=args.node_environment_manifest_path,
        alarm_catalog_path=args.alarm_catalog_path,
        historian_database_path=args.historian_database_path,
        manager_run_root=args.manager_run_root,
        execution_run_root=args.execution_run_root,
        cell_id=args.cell_id,
        resource_ids=tuple(sorted(set(args.resource_id))),
        owner_id=args.owner_id,
        freshness_milliseconds=args.freshness_ms,
        manager_timeout_seconds=args.manager_timeout_s,
        cclc_timeout_seconds=args.cclc_timeout_s,
        program_runner_url=args.program_runner_url,
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
    )
    # Internal request success is not a runtime event. Semantic owner state,
    # warnings, failures, and journals remain observable without a log line
    # for every high-rate observation sample.
    uvicorn.run(
        create_app(config),
        host=args.host,
        port=args.port,
        access_log=False,
        # The Headless real-time channels carry small bounded messages. Avoid
        # per-frame deflate work on the same event loop that admits Stop.
        ws_per_message_deflate=False,
    )
    return 0


def _load_cclc_request(path: Path) -> dict[str, Any]:
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_CCLC_REQUEST_BYTES:
        raise ValueError("CCLC request file is empty or exceeds its byte bound")
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("CCLC request file is not UTF-8") from exc
    value = strict_json_object(text, label="CCLC request file")
    if "confirmed" in value:
        raise ValueError("CCLC request file must omit CLI-owned confirmation")
    return value


def _bind_cclc_owner(
    client: HeadlessHttpClient, request: dict[str, Any]
) -> dict[str, Any]:
    context = request.get("request_context")
    if not isinstance(context, dict):
        raise ValueError("CCLC request file lacks request_context")
    owner = client.cclc_owner_identity()
    return {
        **request,
        "request_context": {
            **context,
            "request_owner_epoch": owner["request_owner_epoch"],
        },
    }


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
                    "request_id": args.request_id,
                    "request_owner_epoch": snapshot["owner_epoch"],
                    "source_label": args.source_label,
                    "correlation_id": args.correlation_id,
                    "members": snapshot["unacknowledged_membership"],
                    "expected_generation": snapshot["generation"],
                    "reason": args.reason,
                    "confirmed": args.confirmed,
                }
            )
        elif args.command == "reconcile-historian":
            value = client.reconcile_historian(
                {
                    **_caller(args),
                    "expected_health_generation": args.expected_health_generation,
                }
            )
        elif args.command == "apply":
            admission = client.authoring_admission()
            preview = client.manager_deployment_candidate_preview(
                {
                    "deployment_artifact_id": args.deployment_artifact_id,
                    "deployment_revision": args.deployment_revision,
                    "expected_current": {
                        "graph_revision": args.expected_graph_revision,
                        "retained_bundle_id": args.expected_retained_bundle_id,
                    },
                    "expected_admission_generation": admission[
                        "admission_generation"
                    ],
                    "request_owner_epoch": args.request_owner_epoch,
                }
            )
            if preview.get("ok") is not True:
                value = preview
                print(json.dumps(value, indent=2, sort_keys=True))
                return 1
            value = client.manager_apply(
                {
                    **_caller(args),
                    "candidate_receipt": preview["candidate_receipt"],
                    "deployment_artifact_id": args.deployment_artifact_id,
                    "deployment_revision": args.deployment_revision,
                    "expected_current": {
                        "graph_revision": args.expected_graph_revision,
                        "retained_bundle_id": args.expected_retained_bundle_id,
                    },
                    "reject_if_affected_busy": True,
                    "prepared_activation": {
                        "captured_input_digest": args.captured_input_digest,
                        "deployment_artifact_id": args.deployment_artifact_id,
                        "deployment_revision": args.deployment_revision,
                        "program_binding_id": args.program_binding_id,
                        "prepared_activation_id": args.prepared_activation_id,
                    },
                    "expected_admission_generation": admission[
                        "admission_generation"
                    ],
                }
            )
        elif args.command == "cclc-component":
            value = client.cclc_component(args.component_id)
        elif args.command == "cclc-route-templates":
            value = client.cclc_route_templates(args.component_id)
        elif args.command == "cclc-lifecycle-submit":
            admission = client.authoring_admission()
            request = _bind_cclc_owner(
                client, _load_cclc_request(args.request_file)
            )
            value = client.submit_component_lifecycle(
                {
                    **request,
                    "confirmed": args.confirmed,
                    "expected_admission_generation": admission[
                        "admission_generation"
                    ],
                }
            )
        elif args.command == "cclc-lifecycle-status":
            value = client.component_lifecycle_status(args.transaction_id)
        elif args.command == "cclc-route-submit":
            request = _bind_cclc_owner(
                client, _load_cclc_request(args.request_file)
            )
            value = client.submit_route_selection(
                {
                    **request,
                    "confirmed": args.confirmed,
                }
            )
        elif args.command == "cclc-route-status":
            value = client.route_selection_status(args.transaction_id)
        else:
            raise AssertionError(f"unsupported command {args.command}")
    _print(value)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        return _serve(args) if args.command == "serve" else _request(args)
    except (HeadlessApiError, HeadlessClientError, ValueError) as exc:
        code = getattr(exc, "code", "headless_cli_invalid")
        print(f"{code}: {exc}", file=sys.stderr)
        return 69 if "unavailable" in code else 65


if __name__ == "__main__":
    raise SystemExit(main())
