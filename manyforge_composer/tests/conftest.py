"""Test-owned persistent roots for every Headless-backed Composer fixture."""

from __future__ import annotations

import pytest


@pytest.fixture(autouse=True)
def _composer_authoring_state_root(tmp_path, monkeypatch) -> None:
    root = tmp_path / "composer-authoring-state"
    root.mkdir()
    monkeypatch.setenv(
        "MANYFORGE_COMPOSER_AUTHORING_STATE_ROOT", str(root)
    )
