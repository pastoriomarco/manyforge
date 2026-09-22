from __future__ import annotations

import base64
import gzip
import os
from pathlib import Path

import pytest

from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    capture_deployment_model_sources,
)


def _deployment() -> dict[str, object]:
    return {
        "components": [
            {
                "component_id": "robot",
                "model_source": {
                    "urdf": {"project_path": "model.urdf"},
                    "srdf": {"project_path": "model.srdf"},
                    "package_roots": {
                        "example": {"project_path": "package"},
                    },
                },
            }
        ]
    }


def test_capture_owns_exact_entrypoints_and_resource_bytes(tmp_path: Path) -> None:
    (tmp_path / "package" / "meshes").mkdir(parents=True)
    urdf = (
        b'<robot name="r"><link name="a"><visual><geometry>'
        b'<mesh filename="package://example/meshes/a.stl"/>'
        b"</geometry></visual></link></robot>"
    )
    srdf = b'<robot name="r"><group name="arm"><link name="a"/></group></robot>'
    mesh = b"solid exact\nendsolid exact\n"
    (tmp_path / "model.urdf").write_bytes(urdf)
    (tmp_path / "model.srdf").write_bytes(srdf)
    (tmp_path / "package" / "meshes" / "a.stl").write_bytes(mesh)

    captured = capture_deployment_model_sources(
        _deployment(), project_root=tmp_path
    )

    component = captured["components"][0]
    assert gzip.decompress(
        b"".join(
            base64.b64decode(chunk)
            for chunk in component["entrypoints"]["urdf"]["chunks"]
        )
    ) == urdf
    assert gzip.decompress(
        b"".join(
            base64.b64decode(chunk)
            for chunk in component["entrypoints"]["srdf"]["chunks"]
        )
    ) == srdf
    assert component["resources"][0]["logical_uri"] == (
        "package://example/meshes/a.stl"
    )
    assert gzip.decompress(
        b"".join(
            base64.b64decode(chunk)
            for chunk in component["resources"][0]["chunks"]
        )
    ) == mesh


def test_capture_rejects_unregistered_package_resource(tmp_path: Path) -> None:
    (tmp_path / "model.urdf").write_bytes(
        b'<robot name="r"><link name="a"><visual><geometry>'
        b'<mesh filename="package://foreign/a.stl"/>'
        b"</geometry></visual></link></robot>"
    )
    (tmp_path / "model.srdf").write_bytes(b'<robot name="r"/>')
    (tmp_path / "package").mkdir()

    with pytest.raises(ModelSourceCaptureError, match="has no root"):
        capture_deployment_model_sources(_deployment(), project_root=tmp_path)


def test_capture_rejects_package_path_traversal(tmp_path: Path) -> None:
    (tmp_path / "model.urdf").write_bytes(
        b'<robot name="r"><link name="a"><visual><geometry>'
        b'<mesh filename="package://example/../outside.stl"/>'
        b"</geometry></visual></link></robot>"
    )
    (tmp_path / "model.srdf").write_bytes(b'<robot name="r"/>')
    (tmp_path / "package").mkdir()

    with pytest.raises(ModelSourceCaptureError, match="unsafe relative path"):
        capture_deployment_model_sources(_deployment(), project_root=tmp_path)


def test_capture_rejects_symlinked_entrypoint_parent(tmp_path: Path) -> None:
    actual = tmp_path / "actual"
    actual.mkdir()
    (actual / "model.urdf").write_bytes(b'<robot name="r"><link name="a"/></robot>')
    (tmp_path / "model.srdf").write_bytes(b'<robot name="r"/>')
    (tmp_path / "package").mkdir()
    (tmp_path / "alias").symlink_to(actual, target_is_directory=True)
    deployment = _deployment()
    deployment["components"][0]["model_source"]["urdf"] = {
        "project_path": "alias/model.urdf"
    }

    with pytest.raises(ModelSourceCaptureError, match="could not read robot URDF"):
        capture_deployment_model_sources(deployment, project_root=tmp_path)


def test_capture_rejects_a_source_changed_during_its_bounded_read(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    urdf_path = tmp_path / "model.urdf"
    urdf_path.write_bytes(b'<robot name="r"/>')
    (tmp_path / "model.srdf").write_bytes(b'<robot name="r"/>')
    (tmp_path / "package").mkdir()
    original_read = os.read
    changed = False

    def read_then_change(descriptor: int, count: int) -> bytes:
        nonlocal changed
        result = original_read(descriptor, count)
        if result and not changed:
            changed = True
            before = urdf_path.stat()
            urdf_path.write_bytes(b'<robot name="s"/>')
            os.utime(
                urdf_path,
                ns=(before.st_atime_ns, before.st_mtime_ns + 1_000_000),
            )
        return result

    monkeypatch.setattr(os, "read", read_then_change)
    with pytest.raises(ModelSourceCaptureError, match="changed during capture"):
        capture_deployment_model_sources(_deployment(), project_root=tmp_path)
