"""Explicit launcher for the isolated ManyForge Program runner."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import sys

import uvicorn

from .app import create_app
from .runtime import ProgramRunnerConfig, ProgramRunnerError, ProgramRuntime


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge-program-runner")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9011)
    parser.add_argument("--headless-url", default="http://127.0.0.1:9010")
    parser.add_argument("--owner-id", default="program-runner")
    parser.add_argument("--headless-timeout-s", type=float, default=30.0)
    parser.add_argument("--tick-interval-ms", type=float, default=10.0)
    parser.add_argument("--tick-workers", type=int, default=4)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        config = ProgramRunnerConfig(
            headless_url=args.headless_url,
            owner_id=args.owner_id,
            headless_timeout_seconds=args.headless_timeout_s,
            tick_interval_seconds=args.tick_interval_ms / 1_000.0,
            tick_workers=args.tick_workers,
        )
        uvicorn.run(
            create_app(ProgramRuntime(config)),
            host=args.host,
            port=args.port,
        )
        return 0
    except (ProgramRunnerError, ValueError) as error:
        code = getattr(error, "code", "program_runner_configuration_invalid")
        print(f"{code}: {error}", file=sys.stderr)
        return 69 if "unavailable" in code else 65


__all__ = ["main"]
