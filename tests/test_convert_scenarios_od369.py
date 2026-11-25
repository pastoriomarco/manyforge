"""Tests for scripts/convert_scenarios_od369.py (retired Program profile)."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import uuid

from manyforge_behavior.authoring_forest import empty_authoring
from manyforge_behavior.canonical_artifact import (
    compute_content_identity,
    load_strict_json_yaml_bytes,
    verify_content_identity,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.scenario_artifacts import (
    build_scenario_document,
    capture_scenario_artifact,
    save_scenario_artifact,
)


_ROOT = Path(__file__).resolve().parents[1]
_SCRIPT = _ROOT / "scripts" / "convert_scenarios_od369.py"
_OLD = "composable-cell-v1"
_NEW = "manyforge-cell-v1"


def _module():
    name = f"convert_scenarios_od369_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(name, _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _revision(document: dict) -> str:
    return compute_content_identity(document, own_field="program_revision",
                                    identity_domain="program-revision",
                                    api_version=document["api_version"])


def _load(text: str) -> dict:
    return load_strict_json_yaml_bytes(text.encode("utf-8"), source="test")


def _snapshot(path: Path) -> dict[str, bytes]:
    return {item.name: item.read_bytes() for item in sorted(path.iterdir())}


def test_scenario_with_the_retired_profile_converts_once(tmp_path: Path, capsys) -> None:
    catalog = build_node_environment().catalog
    document = build_scenario_document(
        scenario_id="old_cell", name="old_cell", description="",
        program_draft=empty_authoring(catalog), deployment_draft=None,
        manyforge_version="0.4.0", last_known_status="ready")
    project = Path(save_scenario_artifact(document, str(tmp_path / "old_cell.scenario"),
                                          allowed_roots=[str(tmp_path)], overwrite=False))
    member = project / "program.yaml"
    value = json.loads(member.read_text(encoding="utf-8"))
    value["program_profile"] = _OLD
    member.write_text(json.dumps(value) + "\n", encoding="utf-8")

    module = _module()
    assert module.main([str(project)]) == 0
    assert f"program_profile {_OLD} -> {_NEW}" in capsys.readouterr().out
    manifest = json.loads((project / "scenario.yaml").read_text(encoding="utf-8"))
    assert manifest["members"]["program.yaml"] == (
        "sha256:" + hashlib.sha256(member.read_bytes()).hexdigest())
    loaded, _, _ = capture_scenario_artifact(str(project), allowed_roots=[str(tmp_path)])
    assert loaded["program_draft"]["program_profile"] == _NEW

    snapshot = _snapshot(project)
    assert module.main([str(project)]) == 0
    assert "already current" in capsys.readouterr().out
    assert _snapshot(project) == snapshot


def test_program_file_with_the_retired_profile_gets_a_current_revision(
        tmp_path: Path, capsys) -> None:
    source = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"
    lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    lines = [f"program_profile: {_OLD}\n" if line.startswith("program_profile:") else line
             for line in lines]
    # A consistent old file: its revision matches its own (old) content.
    old = _load("".join(lines))
    lines = [f"program_revision: {_revision(old)}\n"
             if line.startswith("program_revision:") else line for line in lines]
    path = tmp_path / "old.program.yaml"
    path.write_text("".join(lines), encoding="utf-8")

    module = _module()
    assert module.main([str(path)]) == 0
    assert f"program_profile {_OLD} -> {_NEW}" in capsys.readouterr().out
    converted = _load(path.read_text(encoding="utf-8"))
    assert converted == {**old, "program_profile": _NEW,
                         "program_revision": converted["program_revision"]}
    verify_content_identity(converted, own_field="program_revision",
                            identity_domain="program-revision",
                            api_version=converted["api_version"], label=str(path))
    Program(path, build_node_environment().catalog)

    before = path.read_bytes()
    assert module.main([str(path)]) == 0
    assert "already current" in capsys.readouterr().out
    assert path.read_bytes() == before


def test_program_file_lane_labels_are_removed(tmp_path: Path, capsys) -> None:
    source = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"
    lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    at = lines.index("compatibility:\n") + 1
    lines[at:at] = ["  lanes:\n", "  - manyforge-only\n"]
    old = _load("".join(lines))
    lines = [f"program_revision: {_revision(old)}\n"
             if line.startswith("program_revision:") else line for line in lines]
    path = tmp_path / "old.program.yaml"
    path.write_text("".join(lines), encoding="utf-8")

    module = _module()
    assert module.main([str(path)]) == 0
    assert "compatibility.lanes removed" in capsys.readouterr().out
    converted = _load(path.read_text(encoding="utf-8"))
    expected = {**old, "compatibility": {key: value for key, value in old["compatibility"].items()
                                          if key != "lanes"}}
    assert converted == {**expected, "program_revision": converted["program_revision"]}
    Program(path, build_node_environment().catalog)
