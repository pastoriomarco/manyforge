"""ManyForge deployment and generated-node operator tooling."""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys

import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_descriptor import load_node_descriptor
from manyforge_behavior.node_environment import (
    NodeEnvironmentBuildError,
    build_node_environment,
    configured_node_roots,
)
from manyforge_behavior.node_registry import (
    build_node_registry,
    load_external_node_factory,
)


DEFAULT_CATALOG_PATH = (
    Path(__file__).resolve().parents[1] / "resources" / "node_catalog.yaml"
)


def _add_environment_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--catalog", default=DEFAULT_CATALOG_PATH, type=Path)
    parser.add_argument("--root", action="append", type=Path, default=None)
    parser.add_argument("--strict", action="store_true")


def _build(args: argparse.Namespace):
    roots = args.root if args.root is not None else configured_node_roots()
    return build_node_environment(
        args.catalog,
        external_roots=roots,
        strict=args.strict,
    )


def _nodes_list(args: argparse.Namespace) -> int:
    environment = _build(args)
    rows = [
        {
            "node_id": entry.id,
            "kind": entry.kind,
            "source": next(
                (
                    source.kind
                    for source in environment.sources
                    if source.node_id == entry.id
                ),
                "builtin",
            ),
            "executable": environment.registry.contains(entry.id),
        }
        for entry in environment.catalog.entries
    ]
    if args.json:
        print(json.dumps(rows, sort_keys=True))
    else:
        for row in rows:
            print(
                f"{row['node_id']}\t{row['kind']}\t{row['source']}\t"
                f"{'executable' if row['executable'] else 'authoring-only'}"
            )
    return 0


def _nodes_validate(args: argparse.Namespace) -> int:
    if args.folder is not None:
        descriptor = load_node_descriptor(args.folder)
        factory = load_external_node_factory(descriptor)
        builtin = NodeCatalog(args.catalog)
        data = builtin.raw_data
        if any(entry.get("id") == descriptor.node_id for entry in data["entries"]):
            raise ValueError(
                f"external node id '{descriptor.node_id}' duplicates a built-in node"
            )
        data["entries"].append(descriptor.catalog_entry)
        catalog = NodeCatalog.from_data(data)
        build_node_registry(
            catalog,
            {descriptor.node_id: descriptor},
            external_factories={descriptor.node_id: factory},
        )
        print(
            json.dumps(
                {
                    "status": "accepted",
                    "node_id": descriptor.node_id,
                    "version": descriptor.version,
                    "content_hash": descriptor.content_hash,
                    "path": str(descriptor.folder),
                },
                sort_keys=True,
                indent=2,
            )
        )
        return 0
    environment = _build(args)
    payload = {
        "effective_catalog_hash": environment.effective_catalog_hash,
        "generation": environment.generation,
        "report": asdict(environment.report),
        "sources": [asdict(source) for source in environment.sources],
    }
    print(json.dumps(payload, sort_keys=True, indent=2))
    return 0


def _nodes_catalog(args: argparse.Namespace) -> int:
    environment = _build(args)
    if args.json or args.format == "json":
        print(json.dumps(environment.catalog.raw_data, sort_keys=True, indent=2))
    else:
        print(yaml.safe_dump(environment.catalog.raw_data, sort_keys=False), end="")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="manyforge")
    commands = parser.add_subparsers(dest="command", required=True)
    nodes = commands.add_parser("nodes", help="inspect generated node environments")
    node_commands = nodes.add_subparsers(dest="nodes_command", required=True)

    list_parser = node_commands.add_parser("list")
    _add_environment_arguments(list_parser)
    list_parser.add_argument("--json", action="store_true")
    list_parser.set_defaults(handler=_nodes_list)

    validate_parser = node_commands.add_parser("validate")
    _add_environment_arguments(validate_parser)
    validate_parser.add_argument("folder", nargs="?", type=Path)
    validate_parser.set_defaults(handler=_nodes_validate)

    catalog_parser = node_commands.add_parser("catalog")
    _add_environment_arguments(catalog_parser)
    catalog_parser.add_argument("--format", choices=("yaml", "json"), default="yaml")
    catalog_parser.add_argument("--json", action="store_true")
    catalog_parser.set_defaults(handler=_nodes_catalog)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except NodeEnvironmentBuildError as exc:
        print(json.dumps(asdict(exc.report), sort_keys=True, indent=2), file=sys.stderr)
        return 2
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
