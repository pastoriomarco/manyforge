#!/usr/bin/env python3
"""Fail-closed boundary for the unavailable Python program runtime."""

from __future__ import annotations

import argparse
import sys
from typing import Sequence
from typing import NoReturn


UNAVAILABLE_MESSAGE = (
    "composable_cell_program_runtime_unavailable: production Program execution "
    "requires the resource-addressed I6/N binding gate"
)


def create_kernel(_deployment: object) -> NoReturn:
    """Reject every removed production kernel-construction path."""
    raise RuntimeError(
        "composable_cell_kernel_client_binding_unavailable: production Kernel "
        "construction returns through the resource-addressed binding at I6/N"
    )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Launch a ManyForge program")
    parser.add_argument("--program", required=True, help="Path to program YAML")
    parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
    parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
    parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
    parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
