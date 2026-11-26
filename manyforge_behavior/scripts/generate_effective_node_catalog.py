#!/usr/bin/env python3
"""Recompute the catalog_digest of the built-in node catalog.

An intentional content change also updates the pinned digest in catalog.py.
This command never changes that acceptance pin on its own.
"""
import argparse

import yaml

from manyforge_behavior.canonical_artifact import (
    compute_omitted_field_digest,
    load_strict_json_yaml,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH


def candidate():
    value = load_strict_json_yaml(BUILTIN_CATALOG_PATH)
    identities = [row["id"] for row in value["entries"]]
    if len(identities) != len(set(identities)):
        raise ValueError("effective catalog contains duplicate node identities")
    value["catalog_digest"] = compute_omitted_field_digest(value, own_field="catalog_digest")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--candidate-digest", action="store_true")
    args = parser.parse_args()
    value = candidate()
    if args.candidate_digest:
        print(value["catalog_digest"])
        return
    NodeCatalog.from_data(value)
    rendered = (
        "# Built-in node catalog. Edit the entries here, then run "
        "manyforge_behavior/scripts/generate_effective_node_catalog.py to refresh catalog_digest.\n"
        + yaml.safe_dump(value, sort_keys=False, allow_unicode=True)
    )
    if args.check:
        if BUILTIN_CATALOG_PATH.read_text() != rendered:
            parser.error("effective catalog is stale")
    else:
        BUILTIN_CATALOG_PATH.write_text(rendered)


if __name__ == "__main__":
    main()
