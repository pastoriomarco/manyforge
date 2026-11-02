from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from manyforge_operator.cell_layout import (
    CELL_LAYOUT_INITIALIZER,
    CELL_LAYOUT_MARKER,
    CELL_LAYOUT_MARKER_MAX_BYTES,
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


def test_clean_bootstrap_creates_missing_private_namespace_chain(
    tmp_path: Path,
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    namespace = tmp_path / "operator-state" / "workbench"
    cell = namespace / "cell.a"

    layout = CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )

    assert layout.cell_root == cell
    for created in (tmp_path / "operator-state", namespace, cell):
        assert created.is_dir()
        assert created.stat().st_mode & 0o777 == 0o700


@pytest.mark.parametrize("unsafe_kind", ["symlink", "file"])
def test_missing_namespace_rejects_unsafe_existing_component_without_mutation(
    tmp_path: Path, unsafe_kind: str
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    namespace = tmp_path / "operator-state"
    namespace.mkdir()
    unsafe = namespace / "workbench"
    outside = tmp_path / "outside"
    outside.mkdir()
    if unsafe_kind == "symlink":
        unsafe.symlink_to(outside, target_is_directory=True)
    else:
        unsafe.write_text("not a directory\n", encoding="utf-8")

    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a",
            cell_root=unsafe / "cell.a",
            install_root=install,
        )

    assert list(outside.iterdir()) == []
    assert not (outside / "cell.a").exists()


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


@pytest.mark.parametrize("cell_id", ["", ".cell", "cell:name", "café", "cell\x00x"])
def test_cell_identity_must_be_one_portable_token(
    tmp_path: Path, cell_id: str
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    with pytest.raises(CellLayoutError, match="path-safe token"):
        CellInstanceLayout.open_or_create(
            cell_id=cell_id, cell_root=cell, install_root=install
        )
    assert not cell.exists()


def test_marked_layout_missing_child_is_rejected_without_repair(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    missing = cell / "run" / "headless"
    missing.rmdir()

    with pytest.raises(CellLayoutError, match="missing roots|capability root"):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=cell, install_root=install
        )
    assert not missing.exists()


@pytest.mark.parametrize(
    "relative_root", ["state/execution-host", "run/execution-host"]
)
def test_state_and_run_capability_replacement_are_both_rejected_without_repair(
    tmp_path: Path, relative_root: str
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    capability = cell / relative_root
    displaced = tmp_path / relative_root.replace("/", "-")
    capability.rename(displaced)
    capability.mkdir()

    with pytest.raises(CellLayoutError, match="capability root was replaced"):
        CellInstanceLayout.open_existing(cell)

    assert capability.is_dir()
    assert list(capability.iterdir()) == []
    assert displaced.is_dir()


def test_marker_symlink_and_oversize_are_rejected_without_following(
    tmp_path: Path,
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    marker = cell / CELL_LAYOUT_MARKER
    elsewhere = tmp_path / "elsewhere.json"
    elsewhere.write_text(marker.read_text(encoding="utf-8"), encoding="utf-8")
    marker.unlink()
    marker.symlink_to(elsewhere)
    with pytest.raises(CellLayoutError):
        CellInstanceLayout.open_existing(cell)
    assert elsewhere.read_text(encoding="utf-8").startswith("{")

    marker.unlink()
    marker.write_bytes(b"x" * (CELL_LAYOUT_MARKER_MAX_BYTES + 1))
    marker.chmod(0o600)
    with pytest.raises(CellLayoutError, match="size|oversized"):
        CellInstanceLayout.open_existing(cell)


def test_hard_linked_marker_is_rejected(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    marker = cell / CELL_LAYOUT_MARKER
    os.link(marker, tmp_path / "marker-alias.json")

    with pytest.raises(CellLayoutError, match="type, owner, mode, or size"):
        CellInstanceLayout.open_existing(cell)


def test_owner_writable_install_is_valid_but_unreadable_install_is_rejected(
    tmp_path: Path,
) -> None:
    install = tmp_path / "install"
    install.mkdir(mode=0o700)
    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )

    install.chmod(0o300)
    try:
        with pytest.raises(CellLayoutError, match="owner-readable"):
            CellInstanceLayout.open_existing(cell)
    finally:
        install.chmod(0o700)


def test_cell_and_install_trees_must_be_disjoint_before_creation(
    tmp_path: Path,
) -> None:
    install = tmp_path / "install"
    install.mkdir()
    nested_cell = install / "cell"

    with pytest.raises(CellLayoutError, match="disjoint"):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=nested_cell, install_root=install
        )

    assert not nested_cell.exists()

    cell = tmp_path / "cell"
    cell.mkdir()
    nested_install = cell / "install"
    nested_install.mkdir()
    with pytest.raises(CellLayoutError, match="disjoint"):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=cell, install_root=nested_install
        )
    assert not (cell / CELL_LAYOUT_MARKER).exists()


def test_initializer_and_unexpected_marked_entries_fail_closed(tmp_path: Path) -> None:
    install = tmp_path / "install"
    install.mkdir()
    partial = tmp_path / "partial"
    partial.mkdir()
    (partial / CELL_LAYOUT_INITIALIZER).write_text(
        "manyforge.cell_instance_layout.v1\n", encoding="ascii"
    )
    with pytest.raises(CellLayoutError, match="no supported layout marker"):
        CellInstanceLayout.open_or_create(
            cell_id="cell.a", cell_root=partial, install_root=install
        )
    assert (partial / CELL_LAYOUT_INITIALIZER).exists()

    cell = tmp_path / "cell"
    CellInstanceLayout.open_or_create(
        cell_id="cell.a", cell_root=cell, install_root=install
    )
    unexpected = cell / "browser-state.json"
    unexpected.write_text("{}\n", encoding="utf-8")
    with pytest.raises(CellLayoutError, match="unexpected or missing roots"):
        CellInstanceLayout.open_existing(cell)
    assert unexpected.exists()
