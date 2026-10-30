"""`manyforge component`: check, create, register and list component folders.

Run through the CLI shim from the repository root, in the workbench container:

    scripts/manyforge component check <folder> [--scenario <scenario folder>]
    scripts/manyforge component new <robot|tool|io_device> <vendor>.<name> [--into <folder>]
    scripts/manyforge component register <folder>
    scripts/manyforge component list [--json]

Problems print as `error: <where>: <what> [<code>]`, one per line, with exit
status 1. Guide: docs/authoring/ADDING_A_COMPONENT.md.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from scripts.components import descriptor


def _workspace_root(args: argparse.Namespace) -> Path:
    return Path(args.workspace_root or os.environ.get("MANYFORGE_ROOT") or ".").resolve()


def _print_report(report: descriptor.Report) -> int:
    for error in report.errors:
        print(error.line())
    if not report.errors:
        for note in report.notes:
            print(note)
    return 1 if report.errors else 0


def _check(args: argparse.Namespace) -> int:
    report, _descriptor = descriptor.check(
        _workspace_root(args), Path(args.folder), args.scenario and Path(args.scenario)
    )
    return _print_report(report)


def _new(args: argparse.Namespace) -> int:
    report, _folder = descriptor.new_component(
        _workspace_root(args), args.kind, args.id, args.into and Path(args.into)
    )
    return _print_report(report)


def _register(args: argparse.Namespace) -> int:
    report, written = descriptor.register(_workspace_root(args), Path(args.folder))
    status = _print_report(report)
    if status == 0:
        for line in written:
            print(f"wrote: {line}")
        if not written:
            print("ok: nothing written; the rows already match")
    return status


def _list(args: argparse.Namespace) -> int:
    listing = descriptor.list_components(_workspace_root(args))
    if args.json:
        print(json.dumps(listing, indent=2))
        return 0
    for entry in listing["components"]:
        if entry["errors"]:
            status = f"error {entry['errors'][0]['code']} (+{len(entry['errors']) - 1} more)"
        else:
            status = "addable" if entry["addable"] else "check only"
        print(f"{entry['kind']}\t{entry['id']}\t{entry['version']}\t{entry['path']}\t{status}")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="manyforge component",
        description="Robot, tool and I/O component folders "
        "(docs/authoring/ADDING_A_COMPONENT.md).",
    )
    parser.add_argument(
        "--workspace-root",
        help="repository root (default: $MANYFORGE_ROOT, else the current directory)",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser(
        "check", help="check one component folder; one line per problem, exit 1 on any"
    )
    check.add_argument("folder")
    check.add_argument(
        "--scenario", help="also check this Scenario's components of the robot or tool"
    )
    check.set_defaults(handler=_check)
    new = commands.add_parser("new", help="create a component folder from the template")
    new.add_argument("kind", choices=descriptor.KINDS)
    new.add_argument("id", help="<vendor>.<name>, for example acme.r6_gripper")
    new.add_argument("--into", help="folder to create (default: the curated root of the kind)")
    new.set_defaults(handler=_new)
    register = commands.add_parser(
        "register",
        help="write a robot's or tool's model family, component profile and Mock "
        "controller rows",
    )
    register.add_argument("folder")
    register.set_defaults(handler=_register)
    listing = commands.add_parser("list", help="list the curated components, each checked")
    listing.add_argument("--json", action="store_true")
    listing.set_defaults(handler=_list)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        return int(args.handler(args))
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
