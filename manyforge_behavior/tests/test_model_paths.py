"""Tests for robot model path resolution helpers."""

from __future__ import annotations

from pathlib import Path

from manyforge_behavior.model_paths import resolve_package_uri, resolve_resource_path


def test_resolve_package_uri_rejects_root_escape(tmp_path):
    package_root = tmp_path / "pkgroot"
    package_root.mkdir()
    secret = tmp_path / "secret.txt"
    secret.write_text("secret", encoding="utf-8")

    resolved = resolve_package_uri(
        "package://../secret.txt",
        [str(package_root)],
    )

    assert resolved is None


def test_resolve_resource_path_rejects_package_root_escape(tmp_path):
    package_root = tmp_path / "pkgroot"
    package_root.mkdir()
    secret = tmp_path / "secret.txt"
    secret.write_text("secret", encoding="utf-8")

    try:
        resolve_resource_path(
            "package://../secret.txt",
            base_dir=tmp_path,
            package_paths=[str(package_root)],
        )
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("package root escape should not resolve")


def test_resolve_resource_path_allows_in_root_resource(tmp_path):
    package_root = tmp_path / "pkgroot"
    asset_dir = package_root / "meshes"
    asset_dir.mkdir(parents=True)
    mesh = asset_dir / "tool.stl"
    mesh.write_text("mesh", encoding="utf-8")

    resolved = resolve_resource_path(
        "package://meshes/tool.stl",
        base_dir=tmp_path,
        package_paths=[str(package_root)],
    )

    assert resolved == str(mesh.resolve())
