"""Unit tests for the regen entry point's ordering and pin propagation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import uuid

import pytest


_REGEN = Path(__file__).resolve().parents[1] / "scripts" / "regen.py"
OLD = "a" * 64
NEW = "b" * 64


def _module():
    name = f"regen_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(name, _REGEN)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(module)
    return module


def test_steps_run_in_dependency_order() -> None:
    names = [step.name for step in _module().STEPS]
    assert names.index("catalog-pin") < names.index("node-catalog")
    assert names.index("node-catalog") < names.index("assistant-registry")
    assert names.index("assistant-registry") < names.index("node-environment")
    assert names.index("program-revisions") < names.index("maintained-occurrences")
    assert names.index("ui6i-scenario") < names.index("scenario-pins")
    assert names[-1] == "rest-contracts"


def test_digest_pairs_follow_identity_keys_not_list_positions() -> None:
    regen = _module()
    old = {"rows": [{"schema_id": "x", "rev": f"sha256:{OLD}"},
                    {"schema_id": "y", "rev": f"sha256:{'c' * 64}"}]}
    new = {"rows": [{"schema_id": "y", "rev": f"sha256:{'c' * 64}"},
                    {"schema_id": "x", "rev": f"sha256:{NEW}"}]}
    assert regen.digest_pairs(old, new) == {OLD: NEW}


def test_check_mode_reports_a_moved_pin_as_stale() -> None:
    regen = _module()
    ctx = regen.Context(update=False, deployment_tool=None, program_tool=None)
    with pytest.raises(regen.Stale):
        regen.repin(ctx, OLD, NEW)
    regen.repin(ctx, OLD, OLD)  # an unchanged pin is current


def test_update_replaces_copies_but_not_docs_sealed_or_generated_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    regen = _module()
    files = {
        "pkg/src/pin.cpp": f'"sha256:" "{OLD}"\n',
        "pkg/tests/test_pin.py": f'PIN = "sha256:{OLD}"\n',
        "pkg/resources/generated.json": f'{{"d": "sha256:{OLD}"}}\n',
        "pkg/fixture.deployment.yaml": f"d: sha256:{OLD}\n",
        "docs/history.md": f"{OLD}\n",
        "examples/a.program.yaml": f"r: sha256:{OLD}\n",
    }
    for relative, text in files.items():
        (tmp_path / relative).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / relative).write_text(text, encoding="utf-8")
    monkeypatch.setattr(regen, "ROOT", tmp_path)
    monkeypatch.setattr(
        regen, "STEPS", (regen.Step("gen", lambda ctx: None, ("pkg/resources/*.json",)),)
    )
    ctx = regen.Context(update=True, deployment_tool=None, program_tool=None)

    regen.repin(ctx, OLD, NEW)
    regen.flush_pins(ctx)

    assert sorted(ctx.repinned) == ["pkg/src/pin.cpp", "pkg/tests/test_pin.py"]
    assert ctx.cpp_repinned
    for relative, text in files.items():
        expected = text.replace(OLD, NEW) if relative in ctx.repinned else text
        assert (tmp_path / relative).read_text(encoding="utf-8") == expected
    assert sorted(line.split()[0] for line in regen.leftovers(ctx)) == [
        "examples/a.program.yaml",
        "pkg/fixture.deployment.yaml",
        "pkg/resources/generated.json",
    ]
