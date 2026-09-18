"""The native revision writer must stamp what the strict Program loader reads."""

import os
from pathlib import Path
import subprocess

import pytest
import yaml

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("groups", [None, "inherit", "all", ["z", "a"]])
def test_native_revision_update_roundtrips_through_strict_loader(tmp_path, groups):
    tool = os.environ["MANYFORGE_PROGRAM_REVISION_TOOL"]
    catalog = NodeCatalog(ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    document = load_strict_json_yaml(
        ROOT / "examples/pick_and_place_minimal.program.yaml"
    )
    document["tree"] = {
        "node_id": "timer",
        "instance_id": "wait",
        "parameters": {"duration_s": 0.1},
        "children": [],
    }
    if groups is not None:
        document["tree"]["execution_groups"] = groups
        if isinstance(groups, list):
            document["execution_groups"] = {
                group_id: {"execution_policy": "independent"}
                for group_id in sorted(groups)
            }
    document["program_revision"] = "sha256:" + "0" * 64
    source = tmp_path / "program.yaml"
    source.write_text(yaml.safe_dump(document, sort_keys=False))
    written = subprocess.run(
        [tool, "--update", "--input", str(source)],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    ).stdout.strip()
    program = Program(source, catalog)
    assert program.program_revision == written
    checked = subprocess.run(
        [tool, "--check", "--input", str(source)],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    ).stdout.strip()
    assert checked == written
    assert source.read_text().count("program_revision:") == 1
    assert program.execution_selections["all"] == ("wait",)
    if isinstance(groups, list):
        assert program.to_dict()["tree"]["execution_groups"] == ["a", "z"]
    elif groups in (None, "inherit"):
        assert "execution_groups" not in program.to_dict()["tree"]
