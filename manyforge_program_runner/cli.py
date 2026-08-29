"""Explicit launcher for the isolated ManyForge Program runner."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from pathlib import Path
import sys

import uvicorn

from manyforge_headless_api.program_model_transport import (
    OpenAICompatibleProgramModelTransport,
    load_program_model_endpoint_configs,
)

from .app import create_app
from .runtime import ProgramRunnerConfig, ProgramRunnerError, ProgramRuntime
from .snapshot_source import ComposerProgramSnapshotSource


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-program-runner")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9011)
    parser.add_argument("--headless-url", default="http://127.0.0.1:9010")
    parser.add_argument("--execution-run-root", type=Path, required=True)
    parser.add_argument("--cycle-journal-path", type=Path, required=True)
    parser.add_argument("--manager-liveness-socket", type=Path)
    parser.add_argument("--cell-id")
    parser.add_argument("--owner-id", default="program-runner")
    parser.add_argument("--headless-timeout-s", type=float, default=30.0)
    parser.add_argument("--tick-interval-ms", type=float, default=10.0)
    parser.add_argument("--tick-workers", type=int, default=4)
    parser.add_argument("--model-endpoint-config")
    parser.add_argument("--snapshot-url")
    parser.add_argument("--simulation-control-config")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    runtime: ProgramRuntime | None = None
    model_transport = None
    snapshot_source = None
    simulation_control = None
    try:
        config = ProgramRunnerConfig(
            headless_url=args.headless_url,
            execution_run_root=args.execution_run_root,
            cycle_journal_path=args.cycle_journal_path,
            owner_id=args.owner_id,
            headless_timeout_seconds=args.headless_timeout_s,
            tick_interval_seconds=args.tick_interval_ms / 1_000.0,
            tick_workers=args.tick_workers,
            manager_liveness_socket=args.manager_liveness_socket,
            cell_id=args.cell_id,
        )
        model_transport = (
            None
            if args.model_endpoint_config is None
            else OpenAICompatibleProgramModelTransport(
                load_program_model_endpoint_configs(args.model_endpoint_config)
            )
        )
        if args.snapshot_url is not None:
            if model_transport is None:
                raise ValueError("--snapshot-url requires --model-endpoint-config")
            snapshot_source = ComposerProgramSnapshotSource(args.snapshot_url)
        if args.simulation_control_config is not None:
            try:
                from manyforge_ros.program_simulation_port import (
                    create_ros_simulation_program_port,
                )

                simulation_control = create_ros_simulation_program_port(
                    args.simulation_control_config,
                    maximum_retained_tasks=config.maximum_retained_runs,
                )
            except ValueError:
                raise
            except Exception as error:
                raise ProgramRunnerError(
                    "program_runner_simulation_owner_unavailable",
                    str(error) or type(error).__name__,
                ) from error
        runtime = ProgramRuntime(
            config,
            model_transport=model_transport,
            snapshot_source=snapshot_source,
            simulation_control=simulation_control,
        )
        uvicorn.run(
            create_app(runtime),
            host=args.host,
            port=args.port,
            access_log=False,
        )
        return 0
    except (ProgramRunnerError, ValueError) as error:
        code = getattr(error, "code", "program_runner_configuration_invalid")
        print(f"{code}: {error}", file=sys.stderr)
        return 69 if "unavailable" in code else 65
    finally:
        if runtime is None:
            for resource in (snapshot_source, simulation_control, model_transport):
                close = getattr(resource, "close", None)
                if callable(close):
                    close()


__all__ = ["main"]
