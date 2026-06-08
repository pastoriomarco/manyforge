from pathlib import Path

import pytest

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.app import _find_static_dir, create_app
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


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
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())

    with pytest.raises(RuntimeError, match="Explicit frontend static dir"):
        create_app(state, static_dir=str(tmp_path / "missing"))
