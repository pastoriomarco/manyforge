"""Build-owned node inspection and explicit offline Program migration."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile

import yaml

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_environment import (
    BUILTIN_CATALOG_PATH,
    NodeEnvironmentBuildError,
    build_node_environment,
)
from manyforge_behavior.pick_place_migration import (
    migrate_composite_pick_place_program,
)
from manyforge_behavior.program import (
    migrate_program_execution_groups,
    migrate_program_v1,
)


def _build(_args: argparse.Namespace):
    return build_node_environment()


def _nodes_list(args: argparse.Namespace) -> int:
    environment = _build(args)
    rows = [
        {
            "node_id": entry.id,
            "kind": entry.kind,
            "registered_implementations": dict(entry.runner_implementation_ids),
        }
        for entry in environment.catalog.entries
    ]
    if args.json:
        print(json.dumps(rows, sort_keys=True))
    else:
        for row in rows:
            implementations = ",".join(
                f"{engine}:{implementation}"
                for engine, implementation in sorted(
                    row["registered_implementations"].items()
                )
            )
            print(f"{row['node_id']}\t{row['kind']}\t{implementations}")
    return 0


def _nodes_validate(args: argparse.Namespace) -> int:
    environment = _build(args)
    print(
        json.dumps(
            {
                "status": "accepted",
                "environment_digest": environment.environment_digest,
                "catalog_digest": environment.catalog.catalog_digest,
                "registered_implementation_count": len(environment.registry.rows),
            },
            sort_keys=True,
            indent=2,
        )
    )
    return 0


def _nodes_catalog(args: argparse.Namespace) -> int:
    environment = _build(args)
    if args.json or args.format == "json":
        print(
            json.dumps(
                environment.catalog.raw_data,
                sort_keys=True,
                indent=2,
            )
        )
    else:
        print(
            yaml.safe_dump(
                environment.catalog.raw_data,
                sort_keys=False,
            ),
            end="",
        )
    return 0


def _write_program_no_clobber(program, destination_value: str) -> int:
    """Atomically publish one validated Program without touching an existing file."""

    destination = Path(destination_value)
    # A same-directory temporary plus no-clobber link keeps a partial write
    # invisible. No store, backend, active Draft or existing Binding is touched.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=".program-migration-", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(program.canonical_bytes)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, destination)
        directory = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink()
    print(
        json.dumps(
            {"program_revision": program.program_revision, "output": str(destination)}
        )
    )
    return 0


def _program_migrate(args: argparse.Namespace) -> int:
    """Explicit v1 conversion; runtime loading never calls this path."""

    program = migrate_program_v1(
        load_strict_json_yaml(args.input), NodeCatalog(BUILTIN_CATALOG_PATH)
    )
    return _write_program_no_clobber(program, args.output)


def _program_migrate_pick_place(args: argparse.Namespace) -> int:
    """Replace current composite Pick/Place nodes in one offline conversion."""

    program = migrate_composite_pick_place_program(
        load_strict_json_yaml(args.input), NodeCatalog(BUILTIN_CATALOG_PATH)
    )
    return _write_program_no_clobber(program, args.output)


def _program_migrate_execution_groups(args: argparse.Namespace) -> int:
    program = migrate_program_execution_groups(
        load_strict_json_yaml(args.input), NodeCatalog(BUILTIN_CATALOG_PATH)
    )
    return _write_program_no_clobber(program, args.output)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge")
    commands = parser.add_subparsers(dest="command", required=True)
    nodes = commands.add_parser(
        "nodes",
        help="inspect the build-owned v2 node environment",
    )
    node_commands = nodes.add_subparsers(
        dest="nodes_command",
        required=True,
    )

    list_parser = node_commands.add_parser("list")
    list_parser.add_argument("--json", action="store_true")
    list_parser.set_defaults(handler=_nodes_list)

    validate_parser = node_commands.add_parser("validate")
    validate_parser.set_defaults(handler=_nodes_validate)

    catalog_parser = node_commands.add_parser("catalog")
    catalog_parser.add_argument(
        "--format",
        choices=("yaml", "json"),
        default="yaml",
    )
    catalog_parser.add_argument("--json", action="store_true")
    catalog_parser.set_defaults(handler=_nodes_catalog)

    program = commands.add_parser("program", help="offline Program artifact tools")
    program_commands = program.add_subparsers(dest="program_command", required=True)
    migrate = program_commands.add_parser(
        "migrate-v1",
        help="validate v1 and create a separate v2 artifact; never modifies a Binding",
    )
    migrate.add_argument("--input", required=True)
    migrate.add_argument(
        "--output", required=True, help="new path; existing files are never overwritten"
    )
    migrate.set_defaults(handler=_program_migrate)
    migrate_groups = program_commands.add_parser(
        "migrate-execution-groups",
        help="replace the former v2 synchronization list with policy definitions",
    )
    migrate_groups.add_argument("--input", required=True)
    migrate_groups.add_argument(
        "--output", required=True, help="new path; existing files are never overwritten"
    )
    migrate_groups.set_defaults(handler=_program_migrate_execution_groups)
    migrate_pick_place_help = (
        "replace catalog-2.2 composite Pick/Place nodes with visible ordinary "
        "subtrees; a converted composite whose holder lacks a composed tcp is "
        "refused at binding; open it in Composer and choose the attach frame"
    )
    migrate_pick_place = program_commands.add_parser(
        "migrate-pick-place",
        help=migrate_pick_place_help,
        description=(
            "Replace catalog-2.2 composite Pick/Place nodes with visible ordinary "
            "subtrees. A converted composite whose holder lacks a composed tcp is "
            "refused at binding. Open the migrated Program in Composer and choose "
            "the attach frame. The command never modifies the input or a Binding."
        ),
    )
    migrate_pick_place.add_argument("--input", required=True)
    migrate_pick_place.add_argument(
        "--output", required=True, help="new path; existing files are never overwritten"
    )
    migrate_pick_place.set_defaults(handler=_program_migrate_pick_place)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (NodeEnvironmentBuildError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
