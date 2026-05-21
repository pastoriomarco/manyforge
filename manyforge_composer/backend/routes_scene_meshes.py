"""Scene-object mesh asset route for the native Composer viewer.

Scene state carries authored/runtime mesh_resource_uri values. The
browser should not resolve local paths itself, and it should not need to
load OBJ/STL/DAE variants directly. This module resolves allowlisted
mesh resources server-side, converts them to content-addressed GLB
assets in a killable worker process, and exposes operator-visible status
metadata back through /api/scene/state.
"""
from __future__ import annotations

import hashlib
import io
import json
import logging
import multiprocessing as mp
import os
import queue
import tempfile
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal, TYPE_CHECKING

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.model_paths import (
    default_package_paths,
    normalize_package_paths,
    resolve_resource_path,
)

from .security import is_path_within_roots

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)

MeshAssetStatus = Literal[
    "ok",
    "pending",
    "missing",
    "unsupported_format",
    "parse_failed",
    "conversion_timeout",
    "too_large",
    "outside_allowed_roots",
]

_ASSET_SCHEMA_VERSION = "scene-object-glb-v1"
_CONVERTER_VERSION_TAG = "trimesh-4"
_SUPPORTED_EXTENSIONS = {".obj", ".stl", ".ply", ".dae", ".glb", ".gltf"}
_DEFAULT_MAX_FILE_BYTES = 32 * 1024 * 1024
_DEFAULT_MAX_TOTAL_BYTES = 256 * 1024 * 1024
_DEFAULT_MAX_ENTRIES = 1024
_DEFAULT_CONVERSION_TIMEOUT_S = 15.0


def _env_float(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        value = float(raw)
    except ValueError:
        return default
    return value if value > 0.0 else default


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    return value if value > 0 else default


def _conversion_timeout_s() -> float:
    return _env_float("MANYFORGE_MESH_CONVERSION_TIMEOUT_S", _DEFAULT_CONVERSION_TIMEOUT_S)


def _max_file_bytes() -> int:
    return _env_int("MANYFORGE_MESH_MAX_FILE_BYTES", _DEFAULT_MAX_FILE_BYTES)


def _max_total_bytes() -> int:
    return _env_int("MANYFORGE_MESH_CACHE_MAX_BYTES", _DEFAULT_MAX_TOTAL_BYTES)


def _max_entries() -> int:
    return _env_int("MANYFORGE_MESH_CACHE_MAX_ENTRIES", _DEFAULT_MAX_ENTRIES)


def _normalize_scale(mesh_scale: list[float] | tuple[float, ...] | None) -> tuple[float, float, float]:
    if mesh_scale is None:
        return (1.0, 1.0, 1.0)
    if len(mesh_scale) != 3:
        return (1.0, 1.0, 1.0)
    try:
        values = tuple(float(v) for v in mesh_scale)
    except (TypeError, ValueError):
        return (1.0, 1.0, 1.0)
    if any(v <= 0.0 for v in values):
        return (1.0, 1.0, 1.0)
    return values  # type: ignore[return-value]


@dataclass
class MeshAssetDescriptor:
    asset_hash: str
    source_uri: str
    resolved_realpath: str | None
    file_content_sha256: str | None
    mesh_scale: tuple[float, float, float]
    schema_version: str = _ASSET_SCHEMA_VERSION
    converter_version: str = _CONVERTER_VERSION_TAG
    status: MeshAssetStatus = "pending"
    last_error: str | None = None
    updated_ns: int = field(default_factory=time.monotonic_ns)
    lock: threading.Lock = field(default_factory=threading.Lock, repr=False, compare=False)
    in_progress: bool = False

    @property
    def url(self) -> str:
        return f"/api/scene/object-meshes/{self.asset_hash}.glb"


class SceneMeshAssetService:
    """Resolve, convert, cache, and serve scene-object GLB assets."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._descriptors: dict[str, MeshAssetDescriptor] = {}
        self._source_index: dict[tuple[str, tuple[float, float, float]], str] = {}
        self._content_digest_cache: dict[tuple[str, int, int], str] = {}
        self._bytes_cache: OrderedDict[str, bytes] = OrderedDict()
        self._bytes_total = 0

    def metadata_for_shape(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
        *,
        schedule_conversion: bool = True,
    ) -> dict[str, str | None]:
        descriptor = self._descriptor_for_resource(
            state,
            mesh_resource_uri,
            mesh_scale,
            register=True,
        )
        if descriptor.status == "ok":
            return {
                "mesh_asset_url": descriptor.url,
                "mesh_asset_status": descriptor.status,
            }
        if descriptor.status == "pending" and schedule_conversion:
            self._schedule_conversion(descriptor)
        if descriptor.status == "ok":
            return {
                "mesh_asset_url": descriptor.url,
                "mesh_asset_status": descriptor.status,
            }
        return {
            "mesh_asset_url": None,
            "mesh_asset_status": descriptor.status,
        }

    def validate_boundary_or_409(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
    ) -> None:
        descriptor = self._descriptor_for_resource(
            state,
            mesh_resource_uri,
            mesh_scale,
            register=True,
        )
        if descriptor.status in {"pending", "ok", "parse_failed", "conversion_timeout"}:
            return
        raise HTTPException(
            status_code=409,
            detail={
                "error": "mesh_resource_invalid",
                "mesh_asset_status": descriptor.status,
                "mesh_resource_uri": mesh_resource_uri,
                "detail": descriptor.last_error,
            },
        )

    def prewarm(self, state: "ComposerState") -> None:
        if state.kernel is None:
            return
        for scene_object in state.kernel.get_scene_objects():
            shape = getattr(scene_object, "shape", None)
            if shape is None or getattr(shape, "type", None) != "mesh":
                continue
            descriptor = self._descriptor_for_resource(
                state,
                getattr(shape, "mesh_resource_uri", None),
                getattr(shape, "mesh_scale", None),
                register=True,
            )
            if descriptor.status == "pending":
                self._schedule_conversion(descriptor)

    def get_glb_response(self, asset_hash: str) -> Response:
        descriptor = self._descriptors.get(asset_hash)
        if descriptor is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "Unknown scene mesh asset", "asset_hash": asset_hash},
            )
        if descriptor.status not in {"ok", "pending"}:
            status_code = 404 if descriptor.status in {"missing", "outside_allowed_roots"} else 422
            return JSONResponse(
                status_code=status_code,
                content={
                    "detail": descriptor.last_error or descriptor.status,
                    "mesh_asset_status": descriptor.status,
                    "asset_hash": asset_hash,
                },
            )
        try:
            glb_bytes = self._ensure_glb_bytes(descriptor)
        except TimeoutError:
            return JSONResponse(
                status_code=504,
                content={
                    "detail": descriptor.last_error or "Mesh conversion timed out",
                    "mesh_asset_status": descriptor.status,
                    "asset_hash": asset_hash,
                },
            )
        except Exception:
            return JSONResponse(
                status_code=422,
                content={
                    "detail": descriptor.last_error or "Mesh conversion failed",
                    "mesh_asset_status": descriptor.status,
                    "asset_hash": asset_hash,
                },
            )
        return Response(
            content=glb_bytes,
            media_type="model/gltf-binary",
            headers={
                "Cache-Control": "public, max-age=31536000, immutable",
                "ETag": f'"{asset_hash}"',
            },
        )

    def _allowed_roots(self, state: "ComposerState") -> list[str]:
        roots: list[str] = list(getattr(state, "allowed_fs_roots", []) or [])
        for raw in (state.program_source_path, state.deployment_source_path):
            if raw:
                roots.append(str(Path(raw).resolve().parent))
        deployment = state.deployment
        package_paths: list[str] = []
        if deployment is not None and deployment.robot_config is not None:
            base_dir = (
                Path(state.deployment_source_path).resolve().parent
                if state.deployment_source_path
                else Path.cwd()
            )
            extra_package_paths = (
                [] if deployment.robot_config.package_paths else default_package_paths()
            )
            package_paths = normalize_package_paths(
                deployment.robot_config.package_paths,
                base_dir=base_dir,
                extra_package_paths=extra_package_paths,
            )
            roots.extend(package_paths)
        normalized: list[str] = []
        for root in roots:
            try:
                resolved = str(Path(root).expanduser().resolve())
            except Exception:
                continue
            if resolved not in normalized:
                normalized.append(resolved)
        return normalized or [str(Path.cwd().resolve())]

    def _descriptor_for_resource(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
        *,
        register: bool,
    ) -> MeshAssetDescriptor:
        scale = _normalize_scale(mesh_scale)
        source_uri = str(mesh_resource_uri or "").strip()
        if not source_uri:
            return MeshAssetDescriptor(
                asset_hash=_hash_error_descriptor("missing-uri", scale),
                source_uri=source_uri,
                resolved_realpath=None,
                file_content_sha256=None,
                mesh_scale=scale,
                status="missing",
                last_error="mesh_resource_uri is empty",
            )

        try:
            resolved = self._resolve_mesh_resource_path(state, source_uri)
        except FileNotFoundError as exc:
            return self._record_error_descriptor(
                source_uri,
                scale,
                "missing",
                str(exc),
                register=register,
            )
        except Exception as exc:
            return self._record_error_descriptor(
                source_uri,
                scale,
                "missing",
                f"Could not resolve mesh resource: {exc}",
                register=register,
            )

        allowed_roots = self._allowed_roots(state)
        if not is_path_within_roots(resolved, allowed_roots):
            return self._record_error_descriptor(
                source_uri,
                scale,
                "outside_allowed_roots",
                f"Mesh path is outside allowed roots: {resolved}",
                register=register,
            )
        if not resolved.exists():
            return self._record_error_descriptor(
                source_uri,
                scale,
                "missing",
                f"Mesh file not found: {resolved}",
                register=register,
            )
        if not resolved.is_file():
            return self._record_error_descriptor(
                source_uri,
                scale,
                "missing",
                f"Mesh path is not a file: {resolved}",
                register=register,
            )
        extension = resolved.suffix.lower()
        if extension not in _SUPPORTED_EXTENSIONS:
            return self._record_error_descriptor(
                source_uri,
                scale,
                "unsupported_format",
                f"Unsupported mesh format '{extension}'",
                register=register,
            )
        try:
            stat = resolved.stat()
        except OSError as exc:
            return self._record_error_descriptor(
                source_uri,
                scale,
                "missing",
                f"Could not stat mesh file: {exc}",
                register=register,
            )
        max_file_bytes = _max_file_bytes()
        if stat.st_size > max_file_bytes:
            return self._record_error_descriptor(
                source_uri,
                scale,
                "too_large",
                f"Mesh file is {stat.st_size} bytes; cap is {max_file_bytes} bytes",
                register=register,
            )
        digest = self._file_content_digest(resolved, stat)
        asset_hash = _asset_hash(str(resolved), digest, scale)
        with self._lock:
            existing = self._descriptors.get(asset_hash)
            if existing is not None:
                return existing
            descriptor = MeshAssetDescriptor(
                asset_hash=asset_hash,
                source_uri=source_uri,
                resolved_realpath=str(resolved),
                file_content_sha256=digest,
                mesh_scale=scale,
                status="pending",
            )
            if register:
                self._descriptors[asset_hash] = descriptor
                self._source_index[(source_uri, scale)] = asset_hash
            return descriptor

    def _package_paths(self, state: "ComposerState") -> list[str]:
        deployment = state.deployment
        if deployment is None or deployment.robot_config is None:
            return []
        base_dir = (
            Path(state.deployment_source_path).resolve().parent
            if state.deployment_source_path
            else Path.cwd()
        )
        extra_package_paths = (
            [] if deployment.robot_config.package_paths else default_package_paths()
        )
        return normalize_package_paths(
            deployment.robot_config.package_paths,
            base_dir=base_dir,
            extra_package_paths=extra_package_paths,
        )

    def _resolve_mesh_resource_path(self, state: "ComposerState", source_uri: str) -> Path:
        """Resolve a local mesh resource with the same practical semantics as RoboPlan.

        Program-authored mesh paths are local filesystem resources. They may be
        absolute, file://, package://, relative to the loaded program, or relative
        to the Composer workspace root. The final resolved path is still checked
        against allowlisted roots before use.
        """
        base_dir = (
            Path(state.program_source_path).resolve().parent
            if state.program_source_path
            else Path.cwd()
        )
        package_paths = self._package_paths(state)
        if source_uri.startswith("file://") or source_uri.startswith("package://"):
            if source_uri.startswith("file://"):
                resolved_raw = source_uri[len("file://") :]
            else:
                resolved_raw = resolve_resource_path(
                    source_uri,
                    base_dir=base_dir,
                    package_paths=package_paths,
                )
            return Path(resolved_raw).expanduser().resolve()

        raw_path = Path(source_uri).expanduser()
        if raw_path.is_absolute():
            return raw_path.resolve()

        candidates: list[Path] = []

        def add_candidate(path: Path) -> None:
            try:
                resolved = path.resolve()
            except Exception:
                return
            if resolved not in candidates:
                candidates.append(resolved)

        add_candidate(base_dir / raw_path)
        add_candidate(Path.cwd() / raw_path)
        add_candidate(Path("/workspace") / raw_path)
        for root in self._allowed_roots(state):
            add_candidate(Path(root) / raw_path)

        for candidate in candidates:
            if candidate.exists():
                return candidate
        if candidates:
            raise FileNotFoundError(
                "Mesh resource does not exist at any candidate path: "
                + ", ".join(str(candidate) for candidate in candidates)
            )
        raise FileNotFoundError(f"Mesh resource does not exist: {source_uri}")

    def _file_content_digest(self, path: Path, stat: os.stat_result) -> str:
        cache_key = (str(path), int(stat.st_size), int(stat.st_mtime_ns))
        cached = self._content_digest_cache.get(cache_key)
        if cached is not None:
            return cached
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        value = digest.hexdigest()
        self._content_digest_cache[cache_key] = value
        return value

    def _record_error_descriptor(
        self,
        source_uri: str,
        scale: tuple[float, float, float],
        status: MeshAssetStatus,
        message: str,
        *,
        register: bool,
    ) -> MeshAssetDescriptor:
        asset_hash = _hash_error_descriptor(f"{source_uri}|{status}|{message}", scale)
        descriptor = MeshAssetDescriptor(
            asset_hash=asset_hash,
            source_uri=source_uri,
            resolved_realpath=None,
            file_content_sha256=None,
            mesh_scale=scale,
            status=status,
            last_error=message,
        )
        if register:
            with self._lock:
                self._descriptors[asset_hash] = descriptor
                self._source_index[(source_uri, scale)] = asset_hash
        return descriptor

    def _schedule_conversion(self, descriptor: MeshAssetDescriptor) -> None:
        with descriptor.lock:
            if descriptor.status != "pending" or descriptor.in_progress:
                return
            descriptor.in_progress = True
        thread = threading.Thread(
            target=self._background_convert,
            args=(descriptor,),
            name=f"scene-mesh-convert-{descriptor.asset_hash}",
            daemon=True,
        )
        thread.start()

    def _background_convert(self, descriptor: MeshAssetDescriptor) -> None:
        try:
            self._ensure_glb_bytes(descriptor)
        except Exception:
            logger.debug(
                "Scene mesh background conversion failed for %s",
                descriptor.asset_hash,
                exc_info=True,
            )

    def _ensure_glb_bytes(self, descriptor: MeshAssetDescriptor) -> bytes:
        with self._lock:
            cached = self._bytes_cache.get(descriptor.asset_hash)
            if cached is not None:
                self._bytes_cache.move_to_end(descriptor.asset_hash)
                return cached
        with descriptor.lock:
            descriptor.in_progress = True
            try:
                with self._lock:
                    cached = self._bytes_cache.get(descriptor.asset_hash)
                    if cached is not None:
                        self._bytes_cache.move_to_end(descriptor.asset_hash)
                        return cached
                if descriptor.resolved_realpath is None:
                    descriptor.status = "missing"
                    descriptor.last_error = "Mesh asset has no resolved path"
                    raise FileNotFoundError(descriptor.last_error)
                logger.info(
                    "mesh_asset_cache_miss",
                    extra={
                        "event": "mesh_asset_cache_miss",
                        "asset_hash": descriptor.asset_hash,
                        "source_uri": descriptor.source_uri,
                    },
                )
                glb_bytes = _convert_mesh_asset_subprocess(
                    Path(descriptor.resolved_realpath),
                    descriptor.mesh_scale,
                    timeout_s=_conversion_timeout_s(),
                )
                self._record_cache(descriptor.asset_hash, glb_bytes)
                descriptor.status = "ok"
                descriptor.last_error = None
                descriptor.updated_ns = time.monotonic_ns()
                return glb_bytes
            except TimeoutError as exc:
                descriptor.status = "conversion_timeout"
                descriptor.last_error = str(exc)
                descriptor.updated_ns = time.monotonic_ns()
                logger.warning(
                    "mesh_asset_conversion_failed",
                    extra={
                        "event": "mesh_asset_conversion_failed",
                        "asset_hash": descriptor.asset_hash,
                        "source_uri": descriptor.source_uri,
                        "mesh_asset_status": descriptor.status,
                    },
                )
                raise
            except Exception as exc:
                descriptor.status = "parse_failed"
                descriptor.last_error = str(exc)
                descriptor.updated_ns = time.monotonic_ns()
                logger.warning(
                    "mesh_asset_conversion_failed",
                    extra={
                        "event": "mesh_asset_conversion_failed",
                        "asset_hash": descriptor.asset_hash,
                        "source_uri": descriptor.source_uri,
                        "mesh_asset_status": descriptor.status,
                    },
                )
                raise
            finally:
                descriptor.in_progress = False

    def _record_cache(self, asset_hash: str, glb_bytes: bytes) -> None:
        with self._lock:
            prior = self._bytes_cache.pop(asset_hash, None)
            if prior is not None:
                self._bytes_total -= len(prior)
            self._bytes_cache[asset_hash] = glb_bytes
            self._bytes_total += len(glb_bytes)
            self._evict_if_needed()

    def _evict_if_needed(self) -> None:
        max_bytes = _max_total_bytes()
        max_entries = _max_entries()
        while self._bytes_cache and (
            self._bytes_total > max_bytes or len(self._bytes_cache) > max_entries
        ):
            evicted_hash, evicted = self._bytes_cache.popitem(last=False)
            self._bytes_total -= len(evicted)
            logger.info(
                "mesh_asset_cache_evicted",
                extra={
                    "event": "mesh_asset_cache_evicted",
                    "asset_hash": evicted_hash,
                    "evicted_bytes": len(evicted),
                    "remaining_bytes": self._bytes_total,
                },
            )


def _asset_hash(
    resolved_realpath: str,
    file_content_sha256: str,
    mesh_scale: tuple[float, float, float],
) -> str:
    digest = hashlib.sha256()
    digest.update(_ASSET_SCHEMA_VERSION.encode("utf-8"))
    digest.update(b"\0")
    digest.update(_CONVERTER_VERSION_TAG.encode("utf-8"))
    digest.update(b"\0")
    digest.update(resolved_realpath.encode("utf-8", errors="replace"))
    digest.update(b"\0")
    digest.update(file_content_sha256.encode("ascii"))
    digest.update(b"\0")
    digest.update(json.dumps(mesh_scale, separators=(",", ":")).encode("ascii"))
    return digest.hexdigest()[:16]


def _hash_error_descriptor(seed: str, mesh_scale: tuple[float, float, float]) -> str:
    digest = hashlib.sha256()
    digest.update(b"scene-object-error")
    digest.update(seed.encode("utf-8", errors="replace"))
    digest.update(json.dumps(mesh_scale, separators=(",", ":")).encode("ascii"))
    return digest.hexdigest()[:16]


def _convert_mesh_asset_subprocess(
    mesh_file: Path,
    mesh_scale: tuple[float, float, float],
    *,
    timeout_s: float,
) -> bytes:
    with tempfile.NamedTemporaryFile(prefix="manyforge-scene-mesh-", suffix=".glb", delete=False) as tmp:
        output_path = Path(tmp.name)
    ctx = mp.get_context("spawn")
    status_queue: mp.Queue[tuple[str, str]] = ctx.Queue(maxsize=1)
    process = ctx.Process(
        target=_mesh_conversion_worker,
        args=(str(mesh_file), tuple(mesh_scale), str(output_path), status_queue),
    )
    process.start()
    process.join(timeout_s)
    try:
        if process.is_alive():
            process.terminate()
            process.join(1.0)
            if process.is_alive():
                process.kill()
                process.join(1.0)
            raise TimeoutError(f"Mesh conversion timed out after {timeout_s:.1f}s")
        try:
            status, message = status_queue.get_nowait()
        except queue.Empty:
            status, message = (
                "ok" if process.exitcode == 0 and output_path.exists() else "error",
                f"mesh conversion worker exited with {process.exitcode}",
            )
        if status != "ok":
            raise RuntimeError(message)
        glb_bytes = output_path.read_bytes()
        if not glb_bytes:
            raise RuntimeError("mesh conversion produced an empty GLB")
        return glb_bytes
    finally:
        try:
            output_path.unlink(missing_ok=True)
        except Exception:
            pass


def _mesh_conversion_worker(
    mesh_file: str,
    mesh_scale: tuple[float, float, float],
    output_path: str,
    status_queue: "mp.Queue[tuple[str, str]]",
) -> None:
    try:
        import trimesh

        loaded = trimesh.load(mesh_file, force="mesh")
        mesh: Any = loaded
        if hasattr(loaded, "dump"):
            try:
                mesh = loaded.dump(concatenate=True)
            except Exception:
                mesh = loaded
        sx, sy, sz = mesh_scale
        if (abs(sx - 1.0) + abs(sy - 1.0) + abs(sz - 1.0)) > 1e-9:
            mesh.apply_scale([sx, sy, sz])
        data = mesh.export(file_type="glb")
        if isinstance(data, str):
            glb_bytes = data.encode("utf-8")
        elif isinstance(data, bytes):
            glb_bytes = data
        else:
            raise RuntimeError("trimesh returned unsupported GLB payload type")
        trimesh.load(io.BytesIO(glb_bytes), file_type="glb", force="mesh")
        Path(output_path).write_bytes(glb_bytes)
        status_queue.put(("ok", ""))
    except Exception as exc:
        try:
            status_queue.put(("error", str(exc)))
        except Exception:
            pass


def get_scene_mesh_asset_service(state: "ComposerState") -> SceneMeshAssetService:
    service = getattr(state, "_scene_mesh_asset_service", None)
    if isinstance(service, SceneMeshAssetService):
        return service
    service = SceneMeshAssetService()
    setattr(state, "_scene_mesh_asset_service", service)
    return service


def scene_mesh_shape_metadata(
    state: "ComposerState",
    mesh_resource_uri: str | None,
    mesh_scale: list[float] | tuple[float, ...] | None,
) -> dict[str, str | None]:
    return get_scene_mesh_asset_service(state).metadata_for_shape(
        state,
        mesh_resource_uri,
        mesh_scale,
        schedule_conversion=True,
    )


def validate_scene_mesh_resource_or_409(
    state: "ComposerState",
    mesh_resource_uri: str | None,
    mesh_scale: list[float] | tuple[float, ...] | None,
) -> None:
    get_scene_mesh_asset_service(state).validate_boundary_or_409(
        state,
        mesh_resource_uri,
        mesh_scale,
    )


def create_scene_meshes_router(state: "ComposerState") -> APIRouter:
    """Create the scene-object mesh asset router."""
    service = get_scene_mesh_asset_service(state)
    prior_callback = getattr(state, "_on_program_loaded", None)

    def _on_program_loaded_with_mesh_prewarm(st: "ComposerState") -> None:
        if prior_callback is not None:
            prior_callback(st)
        service.prewarm(st)

    setattr(state, "_on_program_loaded", _on_program_loaded_with_mesh_prewarm)

    router = APIRouter()

    @router.get("/{asset_hash}.glb")
    def get_scene_object_mesh(asset_hash: str) -> Response:
        return service.get_glb_response(asset_hash)

    return router
