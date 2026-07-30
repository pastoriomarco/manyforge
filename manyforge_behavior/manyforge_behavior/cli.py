"""V2 build-owned node-environment inspection commands."""

from __future__ import annotations

import argparse
import json
import sys

import yaml

from manyforge_behavior.node_environment import (
    NodeEnvironmentBuildError,
    build_node_environment,
)


def _build(_args: argparse.Namespace):
    return build_node_environment()


def _nodes_list(args: argparse.Namespace) -> int:
    environment = _build(args)
    rows = [
        {
            "node_id": entry.id,
            "kind": entry.kind,
            "registered_implementations": dict(
                entry.runner_implementation_ids
            ),
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
                "registered_implementation_count": len(
                    environment.registry.rows
                ),
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
