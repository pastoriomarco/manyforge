"""Bounded mesh primitives and current owner-bound live route.

Historical test blob ``26431589ac4534354e3d33e2120fcaeb6822b112``
at ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains six
cases.  Content identity, bounded conversion, source containment, and file-size
admission remain active below. Owner authorization, generation fencing,
resource containment, bounded conversion/cache, and HTTP delivery are current.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
from typing import Final

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend import routes_scene_meshes as meshes


HISTORICAL_CASE_COUNT: Final = 6
HISTORICAL_SOURCE_BLOB: Final = "b0a37123e4cb89de32e7cdec055a63b2352bede3"
HISTORICAL_TEST_BLOB: Final = "26431589ac4534354e3d33e2120fcaeb6822b112"
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "conversion_and_http_delivery",
        "historical line 53",
        1,
        True,
        (),
        "bounded conversion, owner projection, cache settlement, URL minting, "
        "and HTTP delivery are current",
    ),
    (
        "content_identity_ignores_mtime",
        "historical line 92",
        1,
        True,
        (),
        "content and scale identity remains active without a metadata cache",
    ),
    (
        "workspace_relative_resource_resolution",
        "historical line 116",
        1,
        True,
        (),
        "workspace-relative URI policy is bound to addressed observer inputs",
    ),
    (
        "scene_state_mesh_projection",
        "historical line 151",
        1,
        True,
        (),
        "observer projection, bounded conversion, and URL publication are current",
    ),
    (
        "outside_root_rejection",
        "historical line 188",
        1,
        True,
        (),
        "single-open source authorization and its HTTP 409 mapping are current",
    ),
    (
        "oversize_rejection",
        "historical line 208",
        1,
        True,
        (),
        "hard source/output bounds and route admission are current",
    ),
)


def _limits(**overrides: object) -> meshes.MeshConversionLimits:
    values: dict[str, object] = {
        "max_source_bytes": meshes._DEFAULT_MAX_FILE_BYTES,  # noqa: SLF001
        "max_output_bytes": meshes._DEFAULT_MAX_OUTPUT_BYTES,  # noqa: SLF001
        "worker_memory_bytes": meshes._DEFAULT_WORKER_MEMORY_BYTES,  # noqa: SLF001
        "timeout_s": meshes._DEFAULT_CONVERSION_TIMEOUT_S,  # noqa: SLF001
    }
    values.update(overrides)
    return meshes.MeshConversionLimits(**values)  # type: ignore[arg-type]


def _write_obj(path: Path) -> bytes:
    content = b"\n".join(
        (
            b"v 0 0 0",
            b"v 1 0 0",
            b"v 0 1 0",
            b"v 0 0 1",
            b"f 1 2 3",
            b"f 1 2 4",
        )
    )
    path.write_bytes(content)
    return content


def _minimal_glb(*, bin_payload: bytes | None = None) -> bytes:
    document = b'{"asset":{"version":"2.0"}}'
    document += b" " * (-len(document) % 4)
    chunks = struct.pack("<I4s", len(document), b"JSON") + document
    if bin_payload is not None:
        binary = bin_payload + b"\x00" * (-len(bin_payload) % 4)
        chunks += struct.pack("<I4s", len(binary), b"BIN\x00") + binary
    length = 12 + len(chunks)
    return struct.pack("<4sII", b"glTF", 2, length) + chunks


def test_environment_limits_are_clamped_by_non_configurable_ceilings(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(
        "MANYFORGE_MESH_MAX_FILE_BYTES",
        str(meshes._HARD_MAX_FILE_BYTES * 2),  # noqa: SLF001
    )
    monkeypatch.setenv(
        "MANYFORGE_MESH_MAX_OUTPUT_BYTES",
        str(meshes._HARD_MAX_OUTPUT_BYTES * 2),  # noqa: SLF001
    )
    monkeypatch.setenv(
        "MANYFORGE_MESH_WORKER_MEMORY_BYTES",
        str(meshes._HARD_MAX_WORKER_MEMORY_BYTES * 2),  # noqa: SLF001
    )
    monkeypatch.setenv("MANYFORGE_MESH_CONVERSION_TIMEOUT_S", "600")

    limits = meshes.MeshConversionLimits.from_environment()

    assert limits.max_source_bytes == meshes._HARD_MAX_FILE_BYTES  # noqa: SLF001
    assert limits.max_output_bytes == meshes._HARD_MAX_OUTPUT_BYTES  # noqa: SLF001
    assert (  # noqa: SLF001
        limits.worker_memory_bytes
        == meshes._HARD_MAX_WORKER_MEMORY_BYTES
    )
    assert (  # noqa: SLF001
        limits.timeout_s == meshes._HARD_MAX_CONVERSION_TIMEOUT_S
    )


def test_invalid_environment_limits_fall_back_to_bounded_defaults(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("MANYFORGE_MESH_MAX_FILE_BYTES", "-1")
    monkeypatch.setenv("MANYFORGE_MESH_MAX_OUTPUT_BYTES", "not-an-integer")
    monkeypatch.setenv("MANYFORGE_MESH_WORKER_MEMORY_BYTES", "1")
    monkeypatch.setenv("MANYFORGE_MESH_CONVERSION_TIMEOUT_S", "nan")

    limits = meshes.MeshConversionLimits.from_environment()

    assert limits.max_source_bytes == meshes._DEFAULT_MAX_FILE_BYTES  # noqa: SLF001
    assert limits.max_output_bytes == meshes._DEFAULT_MAX_OUTPUT_BYTES  # noqa: SLF001
    assert (  # noqa: SLF001
        limits.worker_memory_bytes
        == meshes._DEFAULT_WORKER_MEMORY_BYTES
    )
    assert (  # noqa: SLF001
        limits.timeout_s == meshes._DEFAULT_CONVERSION_TIMEOUT_S
    )


def test_limit_and_scale_dtos_reject_bool_coercion_and_nonfinite_values() -> None:
    with pytest.raises(meshes.SceneMeshInputError, match="must be an integer"):
        _limits(max_source_bytes=True)
    with pytest.raises(meshes.SceneMeshInputError, match="must be a float"):
        _limits(timeout_s=1)

    assert meshes._normalize_scale(None) == (1.0, 1.0, 1.0)  # noqa: SLF001
    assert meshes._normalize_scale([1, 2.0, 3]) == (1.0, 2.0, 3.0)  # noqa: SLF001
    for invalid in (
        [True, 1.0, 1.0],
        ["1", 1.0, 1.0],
        [1.0, float("nan"), 1.0],
        [1.0, float("inf"), 1.0],
        [1.0, 0.0, 1.0],
        [1.0, 2.0],
    ):
        with pytest.raises(meshes.SceneMeshInputError):
            meshes._normalize_scale(invalid)  # type: ignore[arg-type]  # noqa: SLF001


def test_descriptor_and_authorized_input_enforce_exact_identity_fields() -> None:
    digest = "a" * 64
    asset_hash = meshes._asset_hash(  # noqa: SLF001
        "/workspace/part.obj",
        digest,
        (1.0, 1.0, 1.0),
    )
    descriptor = meshes.MeshAssetDescriptor(
        asset_hash=asset_hash,
        source_uri="part.obj",
        resolved_realpath="/workspace/part.obj",
        file_content_sha256=digest,
        mesh_scale=(1.0, 1.0, 1.0),
    )

    assert descriptor.asset_hash == asset_hash
    error_message = "mesh file is missing"
    error_hash = meshes._hash_error_descriptor(  # noqa: SLF001
        f"missing.obj|missing|{error_message}",
        (1.0, 1.0, 1.0),
    )
    unresolved = meshes.MeshAssetDescriptor(
        asset_hash=error_hash,
        source_uri="missing.obj",
        resolved_realpath=None,
        file_content_sha256=None,
        mesh_scale=(1.0, 1.0, 1.0),
        status="missing",
        last_error=error_message,
    )
    assert unresolved.asset_hash == error_hash
    with pytest.raises(meshes.SceneMeshInputError, match="asset_hash"):
        meshes.MeshAssetDescriptor(
            asset_hash="not-a-hash",
            source_uri="part.obj",
            resolved_realpath=None,
            file_content_sha256=None,
            mesh_scale=(1.0, 1.0, 1.0),
        )
    with pytest.raises(meshes.SceneMeshInputError, match="present together"):
        meshes.MeshAssetDescriptor(
            asset_hash="a" * 16,
            source_uri="part.obj",
            resolved_realpath="/workspace/part.obj",
            file_content_sha256=None,
            mesh_scale=(1.0, 1.0, 1.0),
        )
    with pytest.raises(meshes.SceneMeshInputError, match="resolved mesh identity"):
        meshes.MeshAssetDescriptor(
            asset_hash="a" * 16,
            source_uri="part.obj",
            resolved_realpath="/workspace/part.obj",
            file_content_sha256=digest,
            mesh_scale=(1.0, 1.0, 1.0),
        )
    with pytest.raises(meshes.SceneMeshInputError, match="status"):
        meshes.MeshAssetDescriptor(
            asset_hash=meshes._hash_error_descriptor(  # noqa: SLF001
                "missing-uri",
                (1.0, 1.0, 1.0),
            ),
            source_uri="",
            resolved_realpath=None,
            file_content_sha256=None,
            mesh_scale=(1.0, 1.0, 1.0),
            status="pending",
        )
    with pytest.raises(meshes.SceneMeshInputError, match="must carry an error"):
        meshes.MeshAssetDescriptor(
            asset_hash=meshes._hash_error_descriptor(  # noqa: SLF001
                "missing-uri",
                (1.0, 1.0, 1.0),
            ),
            source_uri="",
            resolved_realpath=None,
            file_content_sha256=None,
            mesh_scale=(1.0, 1.0, 1.0),
            status="missing",
        )
    with pytest.raises(meshes.SceneMeshInputError, match="unresolved mesh identity"):
        meshes.MeshAssetDescriptor(
            asset_hash="a" * 16,
            source_uri="missing.obj",
            resolved_realpath=None,
            file_content_sha256=None,
            mesh_scale=(1.0, 1.0, 1.0),
            status="missing",
            last_error=error_message,
        )
    with pytest.raises(meshes.SceneMeshInputError, match="does not match"):
        meshes.AuthorizedMeshInput(
            resolved_realpath="/workspace/part.obj",
            source_suffix=".obj",
            content=b"mesh",
            content_sha256="0" * 64,
        )
    with pytest.raises(meshes.SceneMeshInputError, match="source_suffix"):
        meshes.AuthorizedMeshInput(
            resolved_realpath="/workspace/part.obj",
            source_suffix=".stl",
            content=b"mesh",
            content_sha256=hashlib.sha256(b"mesh").hexdigest(),
        )


def test_mesh_asset_identity_is_content_path_and_scale_sensitive() -> None:
    base = meshes._asset_hash(  # noqa: SLF001
        "/workspace/part.obj",
        "a" * 64,
        (1.0, 1.0, 1.0),
    )

    assert base == meshes._asset_hash(  # noqa: SLF001
        "/workspace/part.obj",
        "a" * 64,
        (1.0, 1.0, 1.0),
    )
    assert base != meshes._asset_hash(  # noqa: SLF001
        "/workspace/other.obj",
        "a" * 64,
        (1.0, 1.0, 1.0),
    )
    assert base != meshes._asset_hash(  # noqa: SLF001
        "/workspace/part.obj",
        "b" * 64,
        (1.0, 1.0, 1.0),
    )
    assert base != meshes._asset_hash(  # noqa: SLF001
        "/workspace/part.obj",
        "a" * 64,
        (2.0, 1.0, 1.0),
    )


def test_authorized_reader_captures_one_immutable_regular_file_snapshot(
    tmp_path: Path,
) -> None:
    root = tmp_path / "root"
    root.mkdir()
    mesh_file = root / "part.obj"
    original = _write_obj(mesh_file)

    authorized = meshes._read_authorized_mesh_input(  # noqa: SLF001
        mesh_file,
        [root],
        limits=_limits(),
    )
    mesh_file.write_bytes(b"changed after authorization")

    assert authorized.resolved_realpath == str(mesh_file)
    assert authorized.source_suffix == ".obj"
    assert authorized.content == original
    assert authorized.content_sha256 == hashlib.sha256(original).hexdigest()


def test_authorized_reader_rejects_path_identity_change_after_open(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    root = tmp_path / "root"
    nested = root / "nested"
    moved = root / "moved"
    nested.mkdir(parents=True)
    mesh_file = nested / "part.obj"
    _write_obj(mesh_file)
    original_read = meshes._read_stable_descriptor  # noqa: SLF001

    def read_then_move(*args: object, **kwargs: object) -> bytes:
        payload = original_read(*args, **kwargs)  # type: ignore[arg-type]
        nested.rename(moved)
        return payload

    monkeypatch.setattr(meshes, "_read_stable_descriptor", read_then_move)

    with pytest.raises(meshes.SceneMeshInputError, match="being authorized"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            mesh_file,
            [root],
            limits=_limits(),
        )


def test_authorized_reader_rejects_outside_symlink_directory_and_oversize(
    tmp_path: Path,
) -> None:
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "outside.obj"
    _write_obj(outside)

    with pytest.raises(meshes.SceneMeshInputError, match="outside"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            outside,
            [root],
            limits=_limits(),
        )

    linked = root / "linked.obj"
    linked.symlink_to(outside)
    with pytest.raises(meshes.SceneMeshInputError, match="symlink"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            linked,
            [root],
            limits=_limits(),
        )

    linked_directory = root / "linked-directory"
    linked_directory.symlink_to(tmp_path, target_is_directory=True)
    with pytest.raises(meshes.SceneMeshInputError, match="symlink"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            linked_directory / "outside.obj",
            [root],
            limits=_limits(),
        )

    directory = root / "directory.obj"
    directory.mkdir()
    with pytest.raises(meshes.SceneMeshInputError, match="regular file"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            directory,
            [root],
            limits=_limits(),
        )

    oversized = root / "oversized.obj"
    oversized.write_bytes(b"12345")
    with pytest.raises(meshes.SceneMeshBoundsError, match="4-byte"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            oversized,
            [root],
            limits=_limits(max_source_bytes=4),
        )

    deferred_dae = root / "deferred.dae"
    deferred_dae.write_text("<COLLADA/>", encoding="utf-8")
    with pytest.raises(meshes.SceneMeshInputError, match="unsupported"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            deferred_dae,
            [root],
            limits=_limits(),
        )


def test_allowed_root_itself_must_be_physical_and_non_symlinked(
    tmp_path: Path,
) -> None:
    root = tmp_path / "root"
    root.mkdir()
    mesh_file = root / "part.obj"
    _write_obj(mesh_file)
    linked_root = tmp_path / "linked-root"
    linked_root.symlink_to(root, target_is_directory=True)

    with pytest.raises(meshes.SceneMeshInputError, match="symlink"):
        meshes._read_authorized_mesh_input(  # noqa: SLF001
            linked_root / "part.obj",
            [linked_root],
            limits=_limits(),
        )


def test_parent_output_reader_requires_one_bounded_regular_valid_glb(
    tmp_path: Path,
) -> None:
    output = tmp_path / "result.glb"
    payload = _minimal_glb()
    output.write_bytes(payload)

    assert (  # noqa: SLF001
        meshes._read_bounded_worker_output(output, maximum=len(payload))
        == payload
    )

    larger_payload = _minimal_glb(bin_payload=b"1234")
    output.write_bytes(larger_payload)
    with pytest.raises(
        meshes.SceneMeshBoundsError,
        match=rf"{len(larger_payload) - 1}-byte",
    ):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=len(larger_payload) - 1,
        )

    output.write_bytes(b"not a glb!")
    with pytest.raises(meshes.SceneMeshInputError, match="shorter"):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=64,
        )

    header_only = struct.pack("<4sII", b"glTF", 2, 12)
    output.write_bytes(header_only)
    with pytest.raises(meshes.SceneMeshInputError, match="valid GLB"):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=64,
        )

    invalid_document = b"{}  "
    invalid_glb = (
        struct.pack("<4sII", b"glTF", 2, 24)
        + struct.pack("<I4s", len(invalid_document), b"JSON")
        + invalid_document
    )
    output.write_bytes(invalid_glb)
    with pytest.raises(meshes.SceneMeshInputError, match="glTF 2.0"):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=64,
        )

    target = tmp_path / "target.glb"
    target.write_bytes(payload)
    output.unlink()
    output.symlink_to(target)
    with pytest.raises(meshes.SceneMeshInputError, match="cannot open"):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=64,
        )

    output.unlink()
    output.write_bytes(payload)
    alias = tmp_path / "result-alias.glb"
    alias.hardlink_to(output)
    with pytest.raises(meshes.SceneMeshInputError, match="exactly one link"):
        meshes._read_bounded_worker_output(  # noqa: SLF001
            output,
            maximum=64,
        )


def test_worker_output_publication_never_overwrites_an_existing_name(
    tmp_path: Path,
) -> None:
    staging = tmp_path / "result.part"
    output = tmp_path / "result.glb"
    payload = _minimal_glb()

    meshes._write_worker_output(  # noqa: SLF001
        str(staging),
        str(output),
        payload,
        maximum=len(payload),
    )
    assert not staging.exists()
    assert output.read_bytes() == payload

    output.write_bytes(b"existing")
    with pytest.raises(FileExistsError):
        meshes._write_worker_output(  # noqa: SLF001
            str(staging),
            str(output),
            payload,
            maximum=len(payload),
        )
    assert output.read_bytes() == b"existing"


def test_snapshot_conversion_has_no_external_resource_resolver() -> None:
    resolver = meshes._NoExternalMeshResolver()  # noqa: SLF001

    with pytest.raises(meshes.SceneMeshInputError, match="external mesh resources"):
        resolver["external.bin"]
    with pytest.raises(meshes.SceneMeshInputError, match="external mesh resources"):
        resolver.get("material.mtl")
    assert resolver.keys() == ()


def test_worker_teardown_reports_an_unkillable_process_after_bounded_joins() -> None:
    class UnkillableProcess:
        def __init__(self) -> None:
            self.joins: list[float] = []
            self.terminated = 0
            self.killed = 0

        def is_alive(self) -> bool:
            return True

        def terminate(self) -> None:
            self.terminated += 1

        def kill(self) -> None:
            self.killed += 1

        def join(self, timeout: float) -> None:
            self.joins.append(timeout)

    process = UnkillableProcess()
    with pytest.raises(RuntimeError, match="remained alive"):
        meshes._stop_worker(process)  # type: ignore[arg-type]  # noqa: SLF001

    assert process.terminated == 1
    assert process.killed == 1
    assert process.joins == [
        meshes._WORKER_STOP_GRACE_S,  # noqa: SLF001
        meshes._WORKER_STOP_GRACE_S,  # noqa: SLF001
    ]


def test_worker_converts_only_the_authorized_snapshot(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    for variable in meshes._WORKER_NATIVE_THREAD_ENV:  # noqa: SLF001
        monkeypatch.setenv(variable, "64")
    root = tmp_path / "root"
    root.mkdir()
    mesh_file = root / "part.obj"
    _write_obj(mesh_file)
    limits = _limits(timeout_s=30.0)
    authorized = meshes._read_authorized_mesh_input(  # noqa: SLF001
        mesh_file,
        [root],
        limits=limits,
    )
    mesh_file.unlink()

    result = meshes._convert_mesh_asset_subprocess(  # noqa: SLF001
        authorized,
        (1.0, 1.0, 1.0),
        limits=limits,
    )

    meshes._validate_glb_payload(  # noqa: SLF001
        result,
        maximum=limits.max_output_bytes,
    )


class _SceneClient:
    def __init__(self, source_uri: str) -> None:
        self.source_uri = source_uri
        self.generation = 7

    def scene(self) -> dict[str, object]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "logical_scene": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "source_owner_epoch": "scene.epoch.1",
                "source_generation": self.generation,
                "payload": {
                    "api_version": "manyforge.logical_scene_source.v1",
                    "snapshot": {
                        "cell_id": "cell.1",
                        "objects": [{
                            "collision_shapes": [{
                                "type": "mesh",
                                "resource_uri": self.source_uri,
                                "scale": {"x": 1.0, "y": 2.0, "z": 3.0},
                            }],
                        }],
                    },
                },
            },
        }


class _SceneState:
    def __init__(self, root: Path, source_uri: str) -> None:
        self.allowed_fs_roots = [str(root)]
        self.program_source_path = None
        self.headless_client = _SceneClient(source_uri)


class _DraftSession:
    def __init__(self, source_uri: str) -> None:
        self.source_uri = source_uri
        self.revision = 9

    def snapshot(self) -> dict[str, object]:
        return {
            "draft_id": "draft-1",
            "draft_revision": self.revision,
            "authoring": {
                "scene": [{
                    "scene_resource_id": "part",
                    "shape": {
                        "type": "mesh",
                        "mesh_resource_uri": self.source_uri,
                        "mesh_scale": [1.0, 2.0, 3.0],
                    },
                }],
            },
        }


def test_owner_bound_route_converts_and_fences_superseded_generation(
    tmp_path: Path,
) -> None:
    mesh_file = tmp_path / "part.obj"
    _write_obj(mesh_file)
    state = _SceneState(tmp_path, "part.obj")
    app = FastAPI()
    app.include_router(meshes.create_scene_meshes_router(state))  # type: ignore[arg-type]
    client = TestClient(app)
    params = {
        "resource_uri": "part.obj",
        "scale_x": 1.0,
        "scale_y": 2.0,
        "scale_z": 3.0,
    }

    metadata = client.get("/api/scene/object-meshes/resolve", params=params)
    assert metadata.status_code == 200
    asset = metadata.json()
    assert asset["schema_version"] == "manyforge.scene_mesh_asset.v1"
    assert asset["source_generation"] == 7
    assert "owner_epoch=scene.epoch.1" in asset["mesh_asset_url"]
    assert "generation=7" in asset["mesh_asset_url"]

    converted = client.get(asset["mesh_asset_url"])
    assert converted.status_code == 200
    assert converted.headers["content-type"] == "model/gltf-binary"
    assert converted.content.startswith(b"glTF")
    assert converted.headers["x-manyforge-scene-generation"] == "7"

    state.headless_client.generation = 8
    stale = client.get(asset["mesh_asset_url"])
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "scene_mesh_owner_generation_changed"

    replacement = client.get("/api/scene/object-meshes/resolve", params=params)
    assert replacement.status_code == 200
    assert replacement.json()["asset_hash"] == asset["asset_hash"]
    assert "generation=8" in replacement.json()["mesh_asset_url"]
    assert client.get(replacement.json()["mesh_asset_url"]).status_code == 200


def test_owner_bound_route_rejects_unowned_and_outside_sources(tmp_path: Path) -> None:
    _write_obj(tmp_path / "part.obj")
    outside = tmp_path.parent / "outside.obj"
    _write_obj(outside)
    state = _SceneState(tmp_path, "part.obj")
    app = FastAPI()
    app.include_router(meshes.create_scene_meshes_router(state))  # type: ignore[arg-type]
    client = TestClient(app)

    unowned = client.get(
        "/api/scene/object-meshes/resolve",
        params={
            "resource_uri": str(outside),
            "scale_x": 1.0,
            "scale_y": 2.0,
            "scale_z": 3.0,
        },
    )
    assert unowned.status_code == 409
    assert unowned.json()["detail"]["code"] == "scene_mesh_not_owner_authorized"

    state.headless_client.source_uri = str(outside)
    outside_root = client.get(
        "/api/scene/object-meshes/resolve",
        params={
            "resource_uri": str(outside),
            "scale_x": 1.0,
            "scale_y": 2.0,
            "scale_z": 3.0,
        },
    )
    assert outside_root.status_code == 409
    assert outside_root.json()["detail"]["code"] == "scene_mesh_input_invalid"


def test_program_draft_mesh_route_is_revision_bound_and_cache_is_scope_separated(
    tmp_path: Path,
) -> None:
    mesh_file = tmp_path / "part.obj"
    _write_obj(mesh_file)
    state = _SceneState(tmp_path, "part.obj")
    state.draft_session = _DraftSession("part.obj")
    app = FastAPI()
    app.include_router(meshes.create_scene_meshes_router(state))  # type: ignore[arg-type]
    client = TestClient(app)
    shape = {
        "resource_uri": "part.obj",
        "scale_x": 1.0,
        "scale_y": 2.0,
        "scale_z": 3.0,
    }

    owner = client.get("/api/scene/object-meshes/resolve", params=shape).json()
    draft = client.get(
        "/api/program/draft/scene-meshes/resolve",
        params={**shape, "draft_id": "draft-1", "draft_revision": 9},
    )

    assert draft.status_code == 200
    metadata = draft.json()
    assert metadata["schema_version"] == "manyforge.program_draft_mesh_asset.v1"
    assert metadata["asset_hash"] == owner["asset_hash"]
    assert metadata["draft_revision"] == 9
    converted = client.get(metadata["mesh_asset_url"])
    assert converted.status_code == 200
    assert converted.headers["x-manyforge-draft-revision"] == "9"
    assert client.get(owner["mesh_asset_url"]).status_code == 200

    state.draft_session.revision = 10
    stale = client.get(metadata["mesh_asset_url"])
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "scene_mesh_draft_revision_changed"


def test_historical_scene_mesh_cases_are_split_without_loss() -> None:
    assert HISTORICAL_SOURCE_BLOB == "b0a37123e4cb89de32e7cdec055a63b2352bede3"
    assert HISTORICAL_TEST_BLOB == "26431589ac4534354e3d33e2120fcaeb6822b112"
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == (
        HISTORICAL_CASE_COUNT
    )
    assert all(family[3] for family in HISTORICAL_CASE_FAMILIES)
    assert all(family[4] == () for family in HISTORICAL_CASE_FAMILIES)
