"""Reproduces the Isaac-mode Save finding without Isaac, SSH or Docker.

In operator mode the Composer's only write root is the runtime host
checkout's ``examples/`` (``scripts/dev-workbench.py`` passes
``/workspace/examples`` as ``MANYFORGE_PROGRAM_WRITE_ROOTS`` and
``scripts/compose-dev.sh`` mounts the checkout read-write at
``/workspace``). The remote source check runs the real collector, which counts
tracked edits and untracked files under ``examples/``, so any Save there makes
the next ``start`` refuse with ``source_dirty``.
"""

from __future__ import annotations

from dataclasses import replace
import importlib.util
import shutil
import subprocess
from pathlib import Path

import pytest

from manyforge_operator.contracts import SourceIdentityV1
from manyforge_operator.isaac import IsaacLauncher
from manyforge_operator.orchestrator import LauncherError

from .test_isaac import _container_config

REPO_ROOT = Path(__file__).resolve().parents[2]


def _collector():
    path = REPO_ROOT / "scripts/qualification/source_identity.py"
    spec = importlib.util.spec_from_file_location("isaac_save_source_identity", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _git(root: Path, *arguments: str) -> None:
    subprocess.run(
        ["git", "-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
         *arguments],
        cwd=root, check=True, capture_output=True,
    )


@pytest.mark.skipif(shutil.which("git") is None, reason="git is required")
@pytest.mark.parametrize("save", ["save_in_place", "save_as_new_scenario"])
def test_composer_save_into_remote_examples_makes_next_isaac_start_source_dirty(
    tmp_path, monkeypatch, save
):
    checkout = tmp_path / "runtime-checkout"
    scenario = checkout / "examples/fixture_cell"
    scenario.mkdir(parents=True)
    (scenario / "program.json").write_text('{"revision": 1}\n', encoding="utf-8")
    _git(checkout, "init", "-q")
    _git(checkout, "add", "-A")
    _git(checkout, "commit", "-q", "-m", "fixture")

    collector = _collector()
    clean = SourceIdentityV1.model_validate(collector.source_identity(checkout))
    assert clean.dirty is False

    config = _container_config(tmp_path)
    config = replace(config, remote=replace(config.remote, source_root=str(checkout)))
    launcher = IsaacLauncher(config)
    # Stand-in for the SSH hop: run the same collector on the "remote" checkout.
    monkeypatch.setattr(
        launcher, "_ssh_json",
        lambda command, **kwargs: collector.source_identity(checkout),
    )
    launcher._verify_remote_source(clean)

    if save == "save_in_place":
        (scenario / "program.json").write_text('{"revision": 2}\n', encoding="utf-8")
    else:
        saved_as = checkout / "examples/fixture_cell_copy"
        saved_as.mkdir()
        (saved_as / "program.json").write_text('{"revision": 1}\n', encoding="utf-8")

    with pytest.raises(LauncherError) as refused:
        launcher._verify_remote_source(clean)
    assert refused.value.code == "source_dirty"
