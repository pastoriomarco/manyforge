"""Explicit launcher for the isolated ManyForge Program runner."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import os
from pathlib import Path
import sys

import uvicorn

from manyforge_headless_api.internal_http import (
    INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS,
)
from manyforge_headless_api.program_model_transport import (
    OpenAICompatibleProgramModelTransport,
)

from .app import create_app
from .runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
    parse_application_start_id,
)
from .snapshot_source import ComposerProgramSnapshotSource

# Ends the process without interpreter finalization; tests replace it.
_exit_now = os._exit


def _end_now(status: int) -> None:
    """End at once, as the execution host does with ``_Exit`` (OD-236).

    Used only when the simulation port's rclpy spin thread outlived its stop:
    finalizing the interpreter with that thread, node and context still live
    can crash inside rclpy and hide the real failure.
    """

    sys.stdout.flush()
    sys.stderr.flush()
    _exit_now(status)


def _application_start_id(text: str) -> int:
    try:
        return parse_application_start_id(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-program-runner")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9011)
    parser.add_argument("--headless-url", default="http://127.0.0.1:9010")
    parser.add_argument("--execution-run-root", type=Path, required=True)
    parser.add_argument("--cycle-journal-path", type=Path, required=True)
    parser.add_argument("--manager-liveness-socket", type=Path)
    parser.add_argument("--cell-id")
    parser.add_argument("--persistent-key-register-directory", type=Path)
    # ``A`` from the Manager (16 lowercase hex); required, never minted here.
    parser.add_argument(
        "--application-start-id",
        type=_application_start_id,
        required=True,
    )
    parser.add_argument("--headless-timeout-s", type=float, default=30.0)
    parser.add_argument("--tick-interval-ms", type=float, default=10.0)
    parser.add_argument("--tick-workers", type=int, default=4)
    parser.add_argument("--snapshot-url")
    parser.add_argument("--simulation-control-config")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    runtime: ProgramRuntime | None = None
    model_transport = None
    snapshot_source = None
    simulation_control = None
    # The port's constructor failed and left its spin thread running.
    simulation_ros_left_running = False
    status = 0
    try:
        config = ProgramRunnerConfig(
            headless_url=args.headless_url,
            execution_run_root=args.execution_run_root,
            cycle_journal_path=args.cycle_journal_path,
            application_start_id=args.application_start_id,
            headless_timeout_seconds=args.headless_timeout_s,
            tick_interval_seconds=args.tick_interval_ms / 1_000.0,
            tick_workers=args.tick_workers,
            manager_liveness_socket=args.manager_liveness_socket,
            cell_id=args.cell_id,
            persistent_key_register_directory=args.persistent_key_register_directory,
        )
        # Program model calls go to the active Deployment's AI model component.
        model_transport = OpenAICompatibleProgramModelTransport()
        if args.snapshot_url is not None:
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
                simulation_ros_left_running = (
                    getattr(error, "ros_left_running", False) is True
                )
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
            timeout_keep_alive=INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS,
        )
        return 0
    except (ProgramRunnerError, ValueError) as error:
        code = getattr(error, "code", "program_runner_configuration_invalid")
        print(f"{code}: {error}", file=sys.stderr)
        status = 69 if "unavailable" in code else 65
        return status
    finally:
        try:
            if runtime is None:
                for resource in (snapshot_source, simulation_control, model_transport):
                    close = getattr(resource, "close", None)
                    if callable(close):
                        close()
            elif simulation_control is not None:
                # The simulation port's ROS context and spin thread end with this
                # process even when the runtime's shutdown never reached them (a
                # failed start); after that shutdown closed it, this is a no-op.
                simulation_control.close()
        finally:
            # OD-236: a spin thread that outlived its stop keeps node and
            # context live; end at once with the failure's own status instead
            # of finalizing the interpreter under them.
            if simulation_ros_left_running:
                _end_now(status)
            if simulation_control is not None and simulation_control.ros_left_running:
                print(
                    "program_runner_shutdown_failed: "
                    "simulation rclpy executor did not stop",
                    file=sys.stderr,
                )
                _end_now(65)


__all__ = ["main"]
