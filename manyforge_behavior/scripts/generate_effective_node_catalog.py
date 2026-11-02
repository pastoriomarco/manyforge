#!/usr/bin/env python3
"""Compose the statically selected extension into the qualified catalog.

An intentional content successor also updates the pinned descriptor identity
in catalog.py. This command never changes that acceptance pin on its own.
"""
import argparse

import yaml

from manyforge_behavior.canonical_artifact import (
    compute_omitted_field_digest,
    load_strict_json_yaml,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH
from manyforge_behavior.qualified_node_extensions import NODE_ID, descriptor


def candidate():
    value = load_strict_json_yaml(BUILTIN_CATALOG_PATH)
    extension = descriptor()
    identities = [row["id"] for row in value["entries"]]
    if len(identities) != len(set(identities)):
        raise ValueError("effective catalog contains duplicate node identities")
    value["entries"] = [row for row in value["entries"] if row["id"] != NODE_ID]
    value["entries"].extend(extension["entries"])
    value["content_version"] = extension["catalog_content_version"]
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
        "# Generated extension composition; edit extension entries in their package descriptor.\n"
        + yaml.safe_dump(value, sort_keys=False, allow_unicode=True)
    )
    if args.check:
        if BUILTIN_CATALOG_PATH.read_text() != rendered:
            parser.error("effective catalog is stale")
    else:
        BUILTIN_CATALOG_PATH.write_text(rendered)


if __name__ == "__main__":
    main()
