from __future__ import annotations

import base64
import gzip
import os
from pathlib import Path

import pytest

from manyforge_behavior import model_source_capture
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


@pytest.mark.parametrize("runtime_kind", ["io_provider", "pose_source", "ai_model"])
def test_capture_skips_model_free_components(
    tmp_path: Path, runtime_kind: str
) -> None:
    deployment = {
        "components": [
            {
                "component_id": "free",
                "configuration": {"runtime_kind": runtime_kind},
            }
        ]
    }

    captured = capture_deployment_model_sources(deployment, project_root=tmp_path)

    assert captured["components"] == []


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


def _counted_memo(monkeypatch: pytest.MonkeyPatch, **bounds: int) -> list[bytes]:
    """A fresh memo whose compressions are recorded."""

    encoded: list[bytes] = []
    original = model_source_capture.encode_model_source_artifact

    def counted(value: bytes, *, media_type: str) -> dict[str, object]:
        encoded.append(value)
        return original(value, media_type=media_type)

    monkeypatch.setattr(model_source_capture, "encode_model_source_artifact", counted)
    monkeypatch.setattr(
        model_source_capture,
        "_ENCODED_SOURCES",
        model_source_capture._EncodedSourceMemo(
            entries=bounds.get("entries", 16),
            transferred_bytes=bounds.get("transferred_bytes", 1 << 20),
        ),
    )
    return encoded


def _mesh_model(tmp_path: Path, mesh: bytes) -> Path:
    (tmp_path / "package" / "meshes").mkdir(parents=True, exist_ok=True)
    (tmp_path / "model.urdf").write_bytes(
        b'<robot name="r"><link name="a"><visual><geometry>'
        b'<mesh filename="package://example/meshes/a.stl"/>'
        b"</geometry></visual></link></robot>"
    )
    (tmp_path / "model.srdf").write_bytes(b'<robot name="r"/>')
    path = tmp_path / "package" / "meshes" / "a.stl"
    path.write_bytes(mesh)
    return path


def _mesh_bytes(captured: dict[str, object]) -> bytes:
    resource = captured["components"][0]["resources"][0]  # type: ignore[index]
    return gzip.decompress(
        b"".join(base64.b64decode(chunk) for chunk in resource["chunks"])
    )


def test_capture_compresses_each_unchanged_source_once(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    encoded = _counted_memo(monkeypatch)
    _mesh_model(tmp_path, b"solid a\nendsolid a\n")

    first = capture_deployment_model_sources(_deployment(), project_root=tmp_path)
    assert len(encoded) == 3  # URDF, SRDF and the mesh
    second = capture_deployment_model_sources(_deployment(), project_root=tmp_path)

    assert len(encoded) == 3
    assert second == first
    # A reused value is the caller's own: editing it cannot reach the memo.
    second["components"][0]["resources"][0]["chunks"].clear()
    third = capture_deployment_model_sources(_deployment(), project_root=tmp_path)
    assert third == first and len(encoded) == 3


def test_capture_never_reuses_a_changed_or_missing_source(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    encoded = _counted_memo(monkeypatch)
    mesh = _mesh_model(tmp_path, b"solid a\nendsolid a\n")
    before = mesh.stat()
    capture_deployment_model_sources(_deployment(), project_root=tmp_path)

    # Same size, timestamps restored: only the content identity differs.
    mesh.write_bytes(b"solid b\nendsolid b\n")
    os.utime(mesh, ns=(before.st_atime_ns, before.st_mtime_ns))
    changed = capture_deployment_model_sources(_deployment(), project_root=tmp_path)
    assert _mesh_bytes(changed) == b"solid b\nendsolid b\n"
    assert encoded[-1] == b"solid b\nendsolid b\n" and len(encoded) == 4

    # A replaced file with the original bytes is a new file identity.
    replacement = mesh.with_name("a.stl.new")
    replacement.write_bytes(b"solid a\nendsolid a\n")
    os.replace(replacement, mesh)
    replaced = capture_deployment_model_sources(_deployment(), project_root=tmp_path)
    assert _mesh_bytes(replaced) == b"solid a\nendsolid a\n"
    assert len(encoded) == 5

    mesh.unlink()
    with pytest.raises(ModelSourceCaptureError, match="could not read package resource"):
        capture_deployment_model_sources(_deployment(), project_root=tmp_path)


def test_capture_memo_stays_within_its_bounds(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    encoded = _counted_memo(monkeypatch, entries=2)
    _mesh_model(tmp_path, b"solid a\nendsolid a\n")

    capture_deployment_model_sources(_deployment(), project_root=tmp_path)
    capture_deployment_model_sources(_deployment(), project_root=tmp_path)

    # Three sources never fit two entries: the oldest is compressed again.
    assert len(encoded) > 3
    assert len(model_source_capture._ENCODED_SOURCES._values) == 2
