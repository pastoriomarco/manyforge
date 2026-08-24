from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from manyforge_operator.cell_layout import (
    CELL_LAYOUT_MARKER,
    SEMANTIC_OWNERS,
    CellInstanceLayout,
    CellLayoutError,
)


def test_clean_bootstrap_reopens_exact_layout(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    first = CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    second = CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    assert second == first
    first.validate()
    assert first.artifact_root.is_dir()
    assert first.projection_root.is_dir()
    assert first.run_root.is_dir()
    assert all(first.owner_state_root(owner).is_dir() for owner in SEMANTIC_OWNERS)


def test_marker_cell_or_install_identity_mismatch_fails(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.b", cell_root=cell, install_root=install
        )
    marker = cell / CELL_LAYOUT_MARKER
    value = json.loads(marker.read_text(encoding="utf-8"))
    value["install_inode"] += 1
    marker.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=cell, install_root=install
        )


def test_nonempty_unmarked_state_tree_is_inert_and_rejected(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    cell.mkdir()
    (cell / "legacy-state.json").write_text("{}\n", encoding="utf-8")

    with pytest.raises(CellLayoutError, match="no supported layout marker"):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=cell, install_root=install
        )

    assert not (cell / CELL_LAYOUT_MARKER).exists()
    assert (cell / "legacy-state.json").read_text(encoding="utf-8") == "{}\n"


def test_symlink_and_inode_replacement_fail_closed(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a",
            cell_root=tmp_path / "cell-link",
            install_root=(tmp_path / "install-link"),
        )
    os.symlink(install, tmp_path / "install-link")
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a",
            cell_root=tmp_path / "cell",
            install_root=tmp_path / "install-link",
        )
    layout = CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=tmp_path / "cell", install_root=install
    )
    artifact = layout.artifact_root
    artifact.rename(tmp_path / "old-artifacts")
    artifact.mkdir()
    with pytest.raises(CellLayoutError):
        layout.validate()


def test_marker_mutation_after_open_fails_validation(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    layout = CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    marker = cell / CELL_LAYOUT_MARKER
    value = json.loads(marker.read_text(encoding="utf-8"))
    value["cell_id"] = "cell.other"
    marker.write_text(json.dumps(value), encoding="utf-8")

    with pytest.raises(CellLayoutError):
        layout.validate()


def test_group_writable_root_is_rejected(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    cell.mkdir(mode=0o770)
    cell.chmod(0o770)
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=cell, install_root=install
        )
