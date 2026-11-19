from pathlib import Path

import pytest

from manyforge_composer.backend.app import _find_static_dir, create_app
from manyforge_composer.backend.state import ComposerState


def test_explicit_static_dir_must_exist(tmp_path):
    assert _find_static_dir(str(tmp_path / "missing")) is None


def test_explicit_static_dir_requires_index_html(tmp_path):
    dist = tmp_path / "dist"
    dist.mkdir()

    assert _find_static_dir(str(dist)) is None


def test_explicit_static_dir_resolves_when_built(tmp_path):
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "index.html").write_text("<!doctype html>\n", encoding="utf-8")

    assert _find_static_dir(str(dist)) == Path(dist)


def test_create_app_rejects_invalid_explicit_static_dir(tmp_path):
    state = ComposerState(application_start_id=0x3F9A0000000000A1)

    with pytest.raises(RuntimeError, match="Explicit frontend static dir"):
        create_app(state, static_dir=str(tmp_path / "missing"))


def test_default_static_dir_is_only_the_source_tree_build(tmp_path, monkeypatch):
    from manyforge_composer.backend import app as app_module

    monkeypatch.setattr(app_module, "__file__", str(tmp_path / "backend" / "app.py"))
    assert _find_static_dir(None) is None

    dist = tmp_path / "frontend" / "dist"
    dist.mkdir(parents=True)
    (dist / "index.html").write_text("<!doctype html>\n", encoding="utf-8")
    assert _find_static_dir(None) == dist
