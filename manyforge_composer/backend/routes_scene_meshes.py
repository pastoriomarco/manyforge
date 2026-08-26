"""Bounded scene-mesh client over exact owner or Program-Draft projections.

The last full Composer implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``.  Source blob
``b0a37123e4cb89de32e7cdec055a63b2352bede3`` records limit parsing and
scale validation at lines 63-113, the descriptor and state-bound cache/service
at lines 116-633, content identity at lines 636-659, path-based conversion at
lines 662-739, and state/router integration at lines 742-794. The current
implementation preserves those behaviors through Headless-issued identities;
it does not restore the former Composer scene owner.

The service accepts a mesh URI only when that exact URI and scale are present
either in the fresh logical-scene projection returned by Headless or in the
explicitly identified current Program Draft. These two read scopes use
separate revision-bound URLs and cache records. The service then captures one
contained no-symlink local source snapshot and converts only those immutable
bytes in a bounded subprocess. The worker receives no source path or external
resource resolver.
OBJ materials and GLTF external buffers/images are not loaded; DAE remains
unsupported because its XML/resource closure is not captured here. Worker
address space, output file size, CPU time, and post-start parent wait/teardown
are bounded. The parent accepts only a bounded regular structurally valid GLB
from a private temporary directory.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
import hashlib
import io
import json
import logging
import math
import multiprocessing as mp
import os
from pathlib import Path
import stat as stat_module
import struct
import tempfile
import threading
import time
from typing import Any, Final, Literal, TYPE_CHECKING
from urllib.parse import unquote, urlencode, urlsplit

from fastapi import APIRouter, HTTPException, Path as ApiPath, Query
from fastapi.responses import JSONResponse, Response

from manyforge_headless_api.client import HeadlessClientError

if TYPE_CHECKING:
    from .state import ComposerState


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

_MESH_ASSET_STATUSES: Final = frozenset(
    {
        "ok",
        "pending",
        "missing",
        "unsupported_format",
        "parse_failed",
        "conversion_timeout",
        "too_large",
        "outside_allowed_roots",
    }
)
_SUPPORTED_EXTENSIONS: Final = frozenset(
    {".obj", ".stl", ".ply", ".glb", ".gltf"}
)
_DEFERRED_EXTENSIONS: Final = frozenset({".dae"})
_ASSET_SCHEMA_VERSION: Final = "scene-object-glb-v1"
_CONVERTER_VERSION_TAG: Final = "trimesh-4"

_DEFAULT_MAX_FILE_BYTES: Final = 32 * 1024 * 1024
_HARD_MAX_FILE_BYTES: Final = 64 * 1024 * 1024
_DEFAULT_MAX_OUTPUT_BYTES: Final = 64 * 1024 * 1024
_HARD_MAX_OUTPUT_BYTES: Final = 128 * 1024 * 1024
_DEFAULT_WORKER_MEMORY_BYTES: Final = 1024 * 1024 * 1024
_MIN_WORKER_MEMORY_BYTES: Final = 256 * 1024 * 1024
_HARD_MAX_WORKER_MEMORY_BYTES: Final = 2 * 1024 * 1024 * 1024
_DEFAULT_CONVERSION_TIMEOUT_S: Final = 15.0
_HARD_MAX_CONVERSION_TIMEOUT_S: Final = 60.0
_WORKER_STOP_GRACE_S: Final = 1.0
_WORKER_NATIVE_THREAD_ENV: Final = (
    "BLIS_NUM_THREADS",
    "GOTO_NUM_THREADS",
    "MKL_NUM_THREADS",
    "NUMEXPR_NUM_THREADS",
    "OMP_NUM_THREADS",
    "OMP_THREAD_LIMIT",
    "OPENBLAS_NUM_THREADS",
    "VECLIB_MAXIMUM_THREADS",
)

_MAX_ALLOWED_ROOTS: Final = 64
_MAX_PATH_CHARS: Final = 4096
_MAX_PATH_COMPONENTS: Final = 256
_MAX_URI_CHARS: Final = 4096
_MAX_ERROR_CHARS: Final = 1024
_MAX_ENV_VALUE_CHARS: Final = 64
_MAX_SCALE_COMPONENT: Final = 1_000_000.0
_GLB_HEADER: Final = struct.Struct("<4sII")
_GLB_CHUNK_HEADER: Final = struct.Struct("<I4s")
_GLB_JSON_CHUNK_TYPE: Final = b"JSON"
_GLB_BINARY_CHUNK_TYPE: Final = b"BIN\x00"
_MIN_GLB_BYTES: Final = 24
_MAX_GLB_JSON_BYTES: Final = 4 * 1024 * 1024

_MAX_SERVICE_RECORDS: Final = 64
_MAX_SERVICE_CACHE_BYTES: Final = 256 * 1024 * 1024
_MESH_METADATA_SCHEMA: Final = "manyforge.scene_mesh_asset.v1"

logger = logging.getLogger(__name__)


class SceneMeshInputError(ValueError):
    """Raised when a retained mesh input is malformed or unauthorized."""


class SceneMeshBoundsError(SceneMeshInputError):
    """Raised before retained mesh work can exceed a hard resource bound."""


def _env_float(name: str, default: float, *, hard_max: float) -> float:
    raw = os.environ.get(name)
    if raw is None:
        return default
    if (
        not raw
        or len(raw) > _MAX_ENV_VALUE_CHARS
        or raw != raw.strip()
    ):
        return default
    try:
        value = float(raw)
    except ValueError:
        return default
    if not math.isfinite(value) or value <= 0.0:
        return default
    return min(value, hard_max)


def _env_int(
    name: str,
    default: int,
    *,
    hard_max: int,
    minimum: int = 1,
) -> int:
    raw = os.environ.get(name)
    if (
        raw is None
        or not raw
        or len(raw) > _MAX_ENV_VALUE_CHARS
        or not raw.isascii()
        or not raw.isdigit()
    ):
        return default
    value = int(raw)
    if value < minimum:
        return default
    return min(value, hard_max)


def _require_exact_int(
    value: object,
    *,
    label: str,
    minimum: int,
    maximum: int,
) -> int:
    if type(value) is not int:
        raise SceneMeshInputError(f"{label} must be an integer")
    if value < minimum or value > maximum:
        raise SceneMeshBoundsError(
            f"{label} must be between {minimum} and {maximum}"
        )
    return value


def _require_exact_float(
    value: object,
    *,
    label: str,
    maximum: float,
) -> float:
    if type(value) is not float:
        raise SceneMeshInputError(f"{label} must be a float")
    if not math.isfinite(value) or value <= 0.0 or value > maximum:
        raise SceneMeshBoundsError(
            f"{label} must be finite, positive, and at most {maximum}"
        )
    return value


@dataclass(frozen=True, slots=True)
class MeshConversionLimits:
    """One exact bounded configuration snapshot for a conversion request."""

    max_source_bytes: int = _DEFAULT_MAX_FILE_BYTES
    max_output_bytes: int = _DEFAULT_MAX_OUTPUT_BYTES
    worker_memory_bytes: int = _DEFAULT_WORKER_MEMORY_BYTES
    timeout_s: float = _DEFAULT_CONVERSION_TIMEOUT_S

    def __post_init__(self) -> None:
        _require_exact_int(
            self.max_source_bytes,
            label="max_source_bytes",
            minimum=1,
            maximum=_HARD_MAX_FILE_BYTES,
        )
        _require_exact_int(
            self.max_output_bytes,
            label="max_output_bytes",
            minimum=_MIN_GLB_BYTES,
            maximum=_HARD_MAX_OUTPUT_BYTES,
        )
        _require_exact_int(
            self.worker_memory_bytes,
            label="worker_memory_bytes",
            minimum=_MIN_WORKER_MEMORY_BYTES,
            maximum=_HARD_MAX_WORKER_MEMORY_BYTES,
        )
        _require_exact_float(
            self.timeout_s,
            label="timeout_s",
            maximum=_HARD_MAX_CONVERSION_TIMEOUT_S,
        )

    @classmethod
    def from_environment(cls) -> "MeshConversionLimits":
        return cls(
            max_source_bytes=_env_int(
                "MANYFORGE_MESH_MAX_FILE_BYTES",
                _DEFAULT_MAX_FILE_BYTES,
                hard_max=_HARD_MAX_FILE_BYTES,
            ),
            max_output_bytes=_env_int(
                "MANYFORGE_MESH_MAX_OUTPUT_BYTES",
                _DEFAULT_MAX_OUTPUT_BYTES,
                hard_max=_HARD_MAX_OUTPUT_BYTES,
                minimum=_MIN_GLB_BYTES,
            ),
            worker_memory_bytes=_env_int(
                "MANYFORGE_MESH_WORKER_MEMORY_BYTES",
                _DEFAULT_WORKER_MEMORY_BYTES,
                hard_max=_HARD_MAX_WORKER_MEMORY_BYTES,
                minimum=_MIN_WORKER_MEMORY_BYTES,
            ),
            timeout_s=_env_float(
                "MANYFORGE_MESH_CONVERSION_TIMEOUT_S",
                _DEFAULT_CONVERSION_TIMEOUT_S,
                hard_max=_HARD_MAX_CONVERSION_TIMEOUT_S,
            ),
        )


def _normalize_scale(
    mesh_scale: list[float] | tuple[float, ...] | None,
) -> tuple[float, float, float]:
    if mesh_scale is None:
        return (1.0, 1.0, 1.0)
    if type(mesh_scale) not in (list, tuple) or len(mesh_scale) != 3:
        raise SceneMeshInputError(
            "mesh_scale must be an exact three-item list or tuple"
        )
    normalized: list[float] = []
    for index, value in enumerate(mesh_scale):
        if type(value) not in (int, float):
            raise SceneMeshInputError(
                f"mesh_scale[{index}] must be an integer or float"
            )
        numeric = float(value)
        if (
            not math.isfinite(numeric)
            or numeric <= 0.0
            or numeric > _MAX_SCALE_COMPONENT
        ):
            raise SceneMeshBoundsError(
                f"mesh_scale[{index}] must be finite, positive, and at most "
                f"{_MAX_SCALE_COMPONENT}"
            )
        normalized.append(numeric)
    return (normalized[0], normalized[1], normalized[2])


def _bounded_text(
    value: object,
    *,
    label: str,
    maximum: int,
    allow_empty: bool = False,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise SceneMeshInputError(f"{label} must be {qualifier}")
    if len(value) > maximum:
        raise SceneMeshBoundsError(
            f"{label} exceeds the {maximum}-character bound"
        )
    if "\x00" in value:
        raise SceneMeshInputError(f"{label} must not contain NUL bytes")
    return value


def _validate_lower_hex(value: object, *, label: str, length: int) -> str:
    text = _bounded_text(value, label=label, maximum=length)
    if len(text) != length or any(
        character not in "0123456789abcdef" for character in text
    ):
        raise SceneMeshInputError(
            f"{label} must be exactly {length} lowercase hexadecimal characters"
        )
    return text


def _canonical_absolute_path_text(value: object, *, label: str) -> str:
    text = _bounded_text(value, label=label, maximum=_MAX_PATH_CHARS)
    path = Path(text)
    if not path.is_absolute() or os.path.normpath(text) != text:
        raise SceneMeshInputError(
            f"{label} must be a normalized absolute filesystem path"
        )
    if len(path.parts) > _MAX_PATH_COMPONENTS:
        raise SceneMeshBoundsError(
            f"{label} exceeds the {_MAX_PATH_COMPONENTS}-component bound"
        )
    return text


@dataclass(frozen=True, slots=True)
class MeshAssetDescriptor:
    """Presentation-neutral content identity returned by a future client."""

    asset_hash: str
    source_uri: str
    resolved_realpath: str | None
    file_content_sha256: str | None
    mesh_scale: tuple[float, float, float]
    status: MeshAssetStatus = "pending"
    last_error: str | None = None
    schema_version: str = _ASSET_SCHEMA_VERSION
    converter_version: str = _CONVERTER_VERSION_TAG

    def __post_init__(self) -> None:
        _validate_lower_hex(self.asset_hash, label="asset_hash", length=16)
        _bounded_text(
            self.source_uri,
            label="source_uri",
            maximum=_MAX_URI_CHARS,
            allow_empty=True,
        )
        if self.resolved_realpath is not None:
            _canonical_absolute_path_text(
                self.resolved_realpath,
                label="resolved_realpath",
            )
        if self.file_content_sha256 is not None:
            _validate_lower_hex(
                self.file_content_sha256,
                label="file_content_sha256",
                length=64,
            )
        if type(self.mesh_scale) is not tuple or any(
            type(value) is not float for value in self.mesh_scale
        ):
            raise SceneMeshInputError(
                "mesh_scale DTO must be a normalized tuple of floats"
            )
        _normalize_scale(self.mesh_scale)
        if type(self.status) is not str or self.status not in _MESH_ASSET_STATUSES:
            raise SceneMeshInputError("status is not a supported mesh asset status")
        if self.last_error is not None:
            _bounded_text(
                self.last_error,
                label="last_error",
                maximum=_MAX_ERROR_CHARS,
            )
        if self.schema_version != _ASSET_SCHEMA_VERSION:
            raise SceneMeshInputError("schema_version is not supported")
        if self.converter_version != _CONVERTER_VERSION_TAG:
            raise SceneMeshInputError("converter_version is not supported")

        has_resolved_identity = self.resolved_realpath is not None
        if has_resolved_identity != (self.file_content_sha256 is not None):
            raise SceneMeshInputError(
                "resolved_realpath and file_content_sha256 must be present together"
            )

        identity_statuses = {
            "ok",
            "pending",
            "parse_failed",
            "conversion_timeout",
        }
        if (self.status in identity_statuses) != has_resolved_identity:
            raise SceneMeshInputError(
                "mesh status does not match resolved identity availability"
            )
        if self.status in {"ok", "pending"}:
            if self.last_error is not None:
                raise SceneMeshInputError(
                    "successful or pending mesh status must not carry an error"
                )
        elif self.last_error is None:
            raise SceneMeshInputError("failed mesh status must carry an error")

        if has_resolved_identity:
            if not self.source_uri:
                raise SceneMeshInputError(
                    "resolved mesh identity must carry a source_uri"
                )
            expected_asset_hash = _asset_hash(
                self.resolved_realpath,
                self.file_content_sha256,
                self.mesh_scale,
            )
            if self.asset_hash != expected_asset_hash:
                raise SceneMeshInputError(
                    "asset_hash does not match the resolved mesh identity"
                )
        else:
            # The preceding status/error invariant proves this is a failed
            # descriptor with a concrete error string.
            if self.last_error is None:  # defensive against future reordering
                raise SceneMeshInputError(
                    "failed mesh status must carry an error"
                )
            expected_asset_hash = _hash_error_descriptor(
                f"{self.source_uri}|{self.status}|{self.last_error}",
                self.mesh_scale,
            )
            if self.asset_hash != expected_asset_hash:
                raise SceneMeshInputError(
                    "asset_hash does not match the unresolved mesh identity"
                )


@dataclass(frozen=True, slots=True)
class AuthorizedMeshInput:
    """Immutable bytes captured from one authorized, single-open source."""

    resolved_realpath: str
    source_suffix: str
    content: bytes
    content_sha256: str

    def __post_init__(self) -> None:
        _canonical_absolute_path_text(
            self.resolved_realpath,
            label="resolved_realpath",
        )
        if (
            type(self.source_suffix) is not str
            or self.source_suffix not in _SUPPORTED_EXTENSIONS
        ):
            raise SceneMeshInputError("source_suffix is not supported")
        if Path(self.resolved_realpath).suffix.lower() != self.source_suffix:
            raise SceneMeshInputError(
                "source_suffix does not match resolved_realpath"
            )
        if type(self.content) is not bytes or not self.content:
            raise SceneMeshInputError("content must be non-empty bytes")
        if len(self.content) > _HARD_MAX_FILE_BYTES:
            raise SceneMeshBoundsError(
                f"content exceeds the {_HARD_MAX_FILE_BYTES}-byte hard bound"
            )
        digest = _validate_lower_hex(
            self.content_sha256,
            label="content_sha256",
            length=64,
        )
        if hashlib.sha256(self.content).hexdigest() != digest:
            raise SceneMeshInputError("content_sha256 does not match content")


def _absolute_path(value: str | Path, *, label: str) -> Path:
    if type(value) is str:
        text = value
    elif isinstance(value, Path):
        text = str(value)
    else:
        raise SceneMeshInputError(f"{label} must be a string or Path")
    return Path(_canonical_absolute_path_text(text, label=label))


def _resolved_allowed_roots(
    allowed_roots: list[str | Path] | tuple[str | Path, ...],
) -> tuple[Path, ...]:
    if type(allowed_roots) not in (list, tuple) or not allowed_roots:
        raise SceneMeshInputError("allowed_roots must be a non-empty list or tuple")
    if len(allowed_roots) > _MAX_ALLOWED_ROOTS:
        raise SceneMeshBoundsError(
            f"allowed_roots exceeds the {_MAX_ALLOWED_ROOTS}-root bound"
        )
    result: list[Path] = []
    for index, raw_root in enumerate(allowed_roots):
        root = _absolute_path(raw_root, label=f"allowed_roots[{index}]")
        try:
            metadata = root.lstat()
            resolved = root.resolve(strict=True)
        except (OSError, RuntimeError, ValueError) as exc:
            raise SceneMeshInputError(
                f"allowed_roots[{index}] is not accessible: {exc}"
            ) from exc
        if stat_module.S_ISLNK(metadata.st_mode) or resolved != root:
            raise SceneMeshInputError(
                f"allowed_roots[{index}] must not traverse a symlink"
            )
        if not stat_module.S_ISDIR(metadata.st_mode):
            raise SceneMeshInputError(
                f"allowed_roots[{index}] must be a directory"
            )
        if root not in result:
            result.append(root)
    return tuple(result)


def _relative_to_deepest_root(candidate: Path, roots: tuple[Path, ...]) -> tuple[Path, Path]:
    matches: list[tuple[Path, Path]] = []
    for root in roots:
        try:
            relative = candidate.relative_to(root)
        except ValueError:
            continue
        if relative.parts:
            matches.append((root, relative))
    if not matches:
        raise SceneMeshInputError("mesh file is outside every allowed root")
    return max(matches, key=lambda pair: len(pair[0].parts))


def _require_no_follow_support() -> int:
    no_follow = getattr(os, "O_NOFOLLOW", 0)
    if not no_follow or os.open not in os.supports_dir_fd:
        raise RuntimeError(
            "this platform cannot enforce no-symlink descriptor traversal"
        )
    return no_follow


def _attest_open_descriptor_path(
    descriptor: int,
    *,
    expected_path: Path,
    label: str,
) -> None:
    """Bind captured bytes to the canonical pathname authorized for them."""
    try:
        actual_path = Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
    except (OSError, RuntimeError, ValueError) as exc:
        raise SceneMeshInputError(
            f"{label} changed while it was being authorized"
        ) from exc
    if actual_path != expected_path:
        raise SceneMeshInputError(
            f"{label} changed while it was being authorized"
        )


def _read_stable_descriptor(
    descriptor: int,
    initial: os.stat_result,
    *,
    maximum: int,
    label: str,
    require_single_link: bool = False,
) -> bytes:
    if initial.st_size <= 0:
        raise SceneMeshInputError(f"{label} is empty")
    if initial.st_size > maximum:
        raise SceneMeshBoundsError(
            f"{label} exceeds the {maximum}-byte bound"
        )
    payload = bytearray()
    try:
        while len(payload) <= maximum:
            remaining = maximum + 1 - len(payload)
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                break
            payload.extend(chunk)
        final = os.fstat(descriptor)
    except OSError as exc:
        raise SceneMeshInputError(f"cannot read {label}: {exc}") from exc
    if len(payload) > maximum:
        raise SceneMeshBoundsError(
            f"{label} exceeds the {maximum}-byte bound"
        )
    identity_before = (
        initial.st_dev,
        initial.st_ino,
        initial.st_size,
        initial.st_mtime_ns,
        initial.st_ctime_ns,
    )
    identity_after = (
        final.st_dev,
        final.st_ino,
        final.st_size,
        final.st_mtime_ns,
        final.st_ctime_ns,
    )
    if identity_before != identity_after or len(payload) != final.st_size:
        raise SceneMeshInputError(f"{label} changed while it was being read")
    if require_single_link and (initial.st_nlink != 1 or final.st_nlink != 1):
        raise SceneMeshInputError(f"{label} must have exactly one link")
    return bytes(payload)


def _read_authorized_mesh_input(
    mesh_file: str | Path,
    allowed_roots: list[str | Path] | tuple[str | Path, ...],
    *,
    limits: MeshConversionLimits,
) -> AuthorizedMeshInput:
    """Capture one regular contained source through one no-follow file open."""
    if type(limits) is not MeshConversionLimits:
        raise SceneMeshInputError("limits must be a MeshConversionLimits DTO")
    candidate = _absolute_path(mesh_file, label="mesh_file")
    suffix = candidate.suffix.lower()
    if suffix in _DEFERRED_EXTENSIONS:
        raise SceneMeshInputError(
            "DAE conversion is unsupported because its XML/resource closure "
            "is not part of the retained mesh contract"
        )
    if suffix not in _SUPPORTED_EXTENSIONS:
        raise SceneMeshInputError(f"unsupported mesh format {suffix!r}")
    roots = _resolved_allowed_roots(allowed_roots)
    root, relative = _relative_to_deepest_root(candidate, roots)
    if len(relative.parts) > _MAX_PATH_COMPONENTS:
        raise SceneMeshBoundsError(
            f"mesh_file exceeds the {_MAX_PATH_COMPONENTS}-component bound"
        )

    probe = root
    try:
        for part in relative.parts:
            probe = probe / part
            metadata = probe.lstat()
            if stat_module.S_ISLNK(metadata.st_mode):
                raise SceneMeshInputError(
                    "mesh_file must not traverse a symlink"
                )
    except FileNotFoundError as exc:
        raise SceneMeshInputError("mesh_file does not exist") from exc
    except OSError as exc:
        raise SceneMeshInputError(f"cannot inspect mesh_file: {exc}") from exc

    no_follow = _require_no_follow_support()
    directory_flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_DIRECTORY", 0)
        | no_follow
    )
    file_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | no_follow
    directory_descriptor = -1
    file_descriptor = -1
    try:
        expected_root = root.stat(follow_symlinks=False)
        directory_descriptor = os.open(root, directory_flags)
        opened_root = os.fstat(directory_descriptor)
        if (
            not stat_module.S_ISDIR(opened_root.st_mode)
            or (opened_root.st_dev, opened_root.st_ino)
            != (expected_root.st_dev, expected_root.st_ino)
        ):
            raise SceneMeshInputError("allowed root changed during authorization")
        for part in relative.parts[:-1]:
            next_descriptor = os.open(
                part,
                directory_flags,
                dir_fd=directory_descriptor,
            )
            os.close(directory_descriptor)
            directory_descriptor = next_descriptor
        file_descriptor = os.open(
            relative.parts[-1],
            file_flags,
            dir_fd=directory_descriptor,
        )
        initial = os.fstat(file_descriptor)
        if not stat_module.S_ISREG(initial.st_mode):
            raise SceneMeshInputError("mesh_file must be a regular file")
        content = _read_stable_descriptor(
            file_descriptor,
            initial,
            maximum=limits.max_source_bytes,
            label="mesh_file",
        )
        _attest_open_descriptor_path(
            file_descriptor,
            expected_path=candidate,
            label="mesh_file",
        )
    except SceneMeshInputError:
        raise
    except OSError as exc:
        raise SceneMeshInputError(
            f"mesh_file authorization failed closed: {exc}"
        ) from exc
    finally:
        if file_descriptor >= 0:
            os.close(file_descriptor)
        if directory_descriptor >= 0:
            os.close(directory_descriptor)

    digest = hashlib.sha256(content).hexdigest()
    return AuthorizedMeshInput(
        resolved_realpath=str(root / relative),
        source_suffix=suffix,
        content=content,
        content_sha256=digest,
    )


def _asset_hash(
    resolved_realpath: str,
    file_content_sha256: str,
    mesh_scale: tuple[float, float, float],
) -> str:
    path = _canonical_absolute_path_text(
        resolved_realpath,
        label="resolved_realpath",
    )
    content_digest = _validate_lower_hex(
        file_content_sha256,
        label="file_content_sha256",
        length=64,
    )
    scale = _normalize_scale(mesh_scale)
    digest = hashlib.sha256()
    for value in (
        _ASSET_SCHEMA_VERSION,
        _CONVERTER_VERSION_TAG,
        path,
        content_digest,
        json.dumps(scale, separators=(",", ":")),
    ):
        digest.update(value.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()[:16]


def _hash_error_descriptor(
    seed: str,
    mesh_scale: tuple[float, float, float],
) -> str:
    bounded_seed = _bounded_text(
        seed,
        label="error identity seed",
        maximum=_MAX_URI_CHARS + _MAX_ERROR_CHARS + 64,
        allow_empty=True,
    )
    scale = _normalize_scale(mesh_scale)
    digest = hashlib.sha256()
    digest.update(b"scene-object-error\0")
    digest.update(bounded_seed.encode("utf-8"))
    digest.update(json.dumps(scale, separators=(",", ":")).encode("ascii"))
    return digest.hexdigest()[:16]


def _validate_glb_payload(payload: bytes, *, maximum: int) -> None:
    if type(payload) is not bytes:
        raise SceneMeshInputError("GLB payload must be bytes")
    if type(maximum) is not int or maximum < _MIN_GLB_BYTES:
        raise SceneMeshInputError(
            f"GLB maximum must be an integer of at least {_MIN_GLB_BYTES} bytes"
        )
    if len(payload) < _MIN_GLB_BYTES:
        raise SceneMeshInputError("GLB payload is shorter than a valid GLB")
    if len(payload) > maximum:
        raise SceneMeshBoundsError(
            f"GLB payload exceeds the {maximum}-byte bound"
        )
    magic, version, declared_length = _GLB_HEADER.unpack_from(payload)
    if magic != b"glTF" or version != 2:
        raise SceneMeshInputError("GLB payload has an unsupported header")
    if declared_length != len(payload):
        raise SceneMeshInputError("GLB payload length does not match its header")
    if declared_length % 4:
        raise SceneMeshInputError("GLB payload length is not four-byte aligned")

    chunks: list[tuple[bytes, int, int]] = []
    offset = _GLB_HEADER.size
    while offset < declared_length:
        if declared_length - offset < _GLB_CHUNK_HEADER.size:
            raise SceneMeshInputError("GLB payload has a truncated chunk header")
        chunk_length, chunk_type = _GLB_CHUNK_HEADER.unpack_from(payload, offset)
        if chunk_length % 4:
            raise SceneMeshInputError("GLB chunk length is not four-byte aligned")
        chunk_start = offset + _GLB_CHUNK_HEADER.size
        chunk_end = chunk_start + chunk_length
        if chunk_end > declared_length:
            raise SceneMeshInputError("GLB chunk exceeds the declared payload length")
        if len(chunks) >= 2:
            raise SceneMeshInputError(
                "GLB may contain only one JSON chunk and one optional BIN chunk"
            )
        chunks.append((chunk_type, chunk_start, chunk_end))
        offset = chunk_end

    if not chunks or chunks[0][0] != _GLB_JSON_CHUNK_TYPE:
        raise SceneMeshInputError("GLB first chunk must be JSON")
    if len(chunks) > 2 or (
        len(chunks) == 2 and chunks[1][0] != _GLB_BINARY_CHUNK_TYPE
    ):
        raise SceneMeshInputError(
            "GLB may contain only one JSON chunk and one optional BIN chunk"
        )

    json_start, json_end = chunks[0][1], chunks[0][2]
    if json_end - json_start > _MAX_GLB_JSON_BYTES:
        raise SceneMeshBoundsError(
            f"GLB JSON chunk exceeds the {_MAX_GLB_JSON_BYTES}-byte bound"
        )
    json_chunk = payload[json_start:json_end]
    try:
        document = json.loads(json_chunk.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SceneMeshInputError("GLB JSON chunk is invalid") from exc
    if (
        type(document) is not dict
        or type(document.get("asset")) is not dict
        or document["asset"].get("version") != "2.0"
    ):
        raise SceneMeshInputError("GLB JSON chunk is not a glTF 2.0 document")


def _read_bounded_worker_output(output_path: Path, *, maximum: int) -> bytes:
    no_follow = _require_no_follow_support()
    descriptor = -1
    try:
        descriptor = os.open(
            output_path,
            os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | no_follow,
        )
        initial = os.fstat(descriptor)
        if not stat_module.S_ISREG(initial.st_mode):
            raise SceneMeshInputError("worker output must be a regular file")
        payload = _read_stable_descriptor(
            descriptor,
            initial,
            maximum=maximum,
            label="worker output",
            require_single_link=True,
        )
        _attest_open_descriptor_path(
            descriptor,
            expected_path=output_path,
            label="worker output",
        )
    except SceneMeshInputError:
        raise
    except OSError as exc:
        raise SceneMeshInputError(
            f"cannot open bounded worker output: {exc}"
        ) from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    _validate_glb_payload(payload, maximum=maximum)
    return payload


def _bounded_worker_message(exc: Exception) -> str:
    message = f"{type(exc).__name__}: {exc}".replace("\n", " ")
    if len(message) <= _MAX_ERROR_CHARS:
        return message
    return message[: _MAX_ERROR_CHARS - 3] + "..."


class _NoExternalMeshResolver:
    """Fail closed if a retained parser asks for any non-snapshot asset."""

    @staticmethod
    def _reject() -> bytes:
        raise SceneMeshInputError(
            "external mesh resources are unavailable in snapshot conversion"
        )

    def __getitem__(self, _name: object) -> bytes:
        return self._reject()

    def get(self, _name: object) -> bytes:
        return self._reject()

    def keys(self) -> tuple[object, ...]:
        return ()


def _set_worker_limit(resource_module: Any, kind: int, requested: int) -> None:
    _soft, current_hard = resource_module.getrlimit(kind)
    if current_hard != resource_module.RLIM_INFINITY:
        requested = min(requested, int(current_hard))
    if requested <= 0:
        raise RuntimeError("worker resource limit is unavailable")
    resource_module.setrlimit(kind, (requested, requested))


def _apply_worker_resource_limits(limits: MeshConversionLimits) -> None:
    import resource

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    _set_worker_limit(
        resource,
        resource.RLIMIT_AS,
        limits.worker_memory_bytes,
    )
    _set_worker_limit(
        resource,
        resource.RLIMIT_FSIZE,
        limits.max_output_bytes,
    )
    _set_worker_limit(
        resource,
        resource.RLIMIT_CPU,
        max(1, math.ceil(limits.timeout_s)),
    )


def _configure_worker_native_thread_limits() -> None:
    # The spawned worker imports NumPy/trimesh only after this point.  Force
    # native math libraries to one thread before import so a high host CPU
    # count cannot reserve one stack/address-space arena per ambient thread and
    # exhaust the worker's deliberate RLIMIT_AS ceiling.
    for variable in _WORKER_NATIVE_THREAD_ENV:
        os.environ[variable] = "1"


def _write_worker_output(
    staging_path: str,
    output_path: str,
    payload: bytes,
    *,
    maximum: int,
) -> None:
    _validate_glb_payload(payload, maximum=maximum)
    no_follow = _require_no_follow_support()
    descriptor = os.open(
        staging_path,
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_CLOEXEC", 0)
        | no_follow,
        0o600,
    )
    try:
        offset = 0
        while offset < len(payload):
            written = os.write(descriptor, payload[offset:])
            if written <= 0:
                raise OSError("worker output write made no progress")
            offset += written
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.link(staging_path, output_path, follow_symlinks=False)
    os.unlink(staging_path)


def _mesh_conversion_worker(
    mesh_input: AuthorizedMeshInput,
    mesh_scale: tuple[float, float, float],
    staging_path: str,
    output_path: str,
    limits: MeshConversionLimits,
    status_connection: Any,
) -> None:
    try:
        if type(limits) is not MeshConversionLimits:
            raise SceneMeshInputError("worker limits DTO is invalid")
        if type(mesh_input) is not AuthorizedMeshInput:
            raise SceneMeshInputError("worker mesh input DTO is invalid")
        _configure_worker_native_thread_limits()
        _apply_worker_resource_limits(limits)
        scale = _normalize_scale(mesh_scale)

        import trimesh

        resolver = _NoExternalMeshResolver()
        loaded = trimesh.load(
            file_obj=io.BytesIO(mesh_input.content),
            file_type=mesh_input.source_suffix.lstrip("."),
            force="mesh",
            resolver=resolver,
            skip_materials=True,
        )
        mesh: Any = loaded
        if hasattr(loaded, "dump"):
            try:
                mesh = loaded.dump(concatenate=True)
            except Exception:
                mesh = loaded
        if scale != (1.0, 1.0, 1.0):
            mesh.apply_scale(list(scale))
        data = mesh.export(file_type="glb")
        if type(data) is str:
            result = data.encode("utf-8")
        elif type(data) is bytes:
            result = data
        else:
            raise RuntimeError("trimesh returned unsupported GLB payload type")
        _validate_glb_payload(result, maximum=limits.max_output_bytes)
        trimesh.load(
            file_obj=io.BytesIO(result),
            file_type="glb",
            force="mesh",
            resolver=resolver,
            skip_materials=True,
        )
        _write_worker_output(
            staging_path,
            output_path,
            result,
            maximum=limits.max_output_bytes,
        )
        status_connection.send(("ok", ""))
    except Exception as exc:
        try:
            status_connection.send(("error", _bounded_worker_message(exc)))
        except Exception:
            pass
    finally:
        status_connection.close()


def _stop_worker(process: mp.Process) -> None:
    if not process.is_alive():
        return
    process.terminate()
    process.join(_WORKER_STOP_GRACE_S)
    if process.is_alive():
        process.kill()
        process.join(_WORKER_STOP_GRACE_S)
    if process.is_alive():
        raise RuntimeError(
            "mesh conversion worker remained alive after bounded SIGKILL teardown"
        )


def _validated_worker_status(value: object) -> tuple[str, str]:
    if type(value) is not tuple or len(value) != 2:
        raise RuntimeError("mesh conversion worker returned malformed status")
    status, message = value
    if type(status) is not str or status not in {"ok", "error"}:
        raise RuntimeError("mesh conversion worker returned unknown status")
    if type(message) is not str or len(message) > _MAX_ERROR_CHARS:
        raise RuntimeError("mesh conversion worker returned malformed detail")
    if status == "ok" and message:
        raise RuntimeError("mesh conversion worker success carried error detail")
    if status == "error" and not message:
        raise RuntimeError("mesh conversion worker failure omitted error detail")
    return status, message


def _convert_mesh_asset_subprocess(
    mesh_input: AuthorizedMeshInput,
    mesh_scale: tuple[float, float, float],
    *,
    limits: MeshConversionLimits,
) -> bytes:
    """Convert one immutable authorized snapshot inside a bounded worker."""
    if type(mesh_input) is not AuthorizedMeshInput:
        raise SceneMeshInputError(
            "mesh_input must be an AuthorizedMeshInput DTO"
        )
    if type(limits) is not MeshConversionLimits:
        raise SceneMeshInputError("limits must be a MeshConversionLimits DTO")
    scale = _normalize_scale(mesh_scale)
    context = mp.get_context("spawn")

    with tempfile.TemporaryDirectory(
        prefix="manyforge-scene-mesh-"
    ) as temporary_directory:
        work_directory = Path(temporary_directory)
        metadata = work_directory.stat(follow_symlinks=False)
        if (
            not stat_module.S_ISDIR(metadata.st_mode)
            or metadata.st_mode & 0o077
        ):
            raise RuntimeError("mesh worker directory is not private")
        staging_path = work_directory / "result.part"
        output_path = work_directory / "result.glb"
        receiver, sender = context.Pipe(duplex=False)
        process = context.Process(
            target=_mesh_conversion_worker,
            args=(
                mesh_input,
                scale,
                str(staging_path),
                str(output_path),
                limits,
                sender,
            ),
        )
        started = False
        try:
            deadline = time.monotonic() + limits.timeout_s
            process.start()
            started = True
            sender.close()
            process.join(max(0.0, deadline - time.monotonic()))
            if process.is_alive():
                raise TimeoutError(
                    "mesh conversion exceeded its bounded wall timeout"
                )
            if process.exitcode != 0:
                raise RuntimeError(
                    f"mesh conversion worker exited with {process.exitcode}"
                )
            if not receiver.poll():
                raise RuntimeError("mesh conversion worker returned no status")
            status, message = _validated_worker_status(receiver.recv())
            if status != "ok":
                raise RuntimeError(message)
            return _read_bounded_worker_output(
                output_path,
                maximum=limits.max_output_bytes,
            )
        finally:
            sender.close()
            receiver.close()
            if started:
                _stop_worker(process)
                if not process.is_alive():
                    process.close()


@dataclass(frozen=True, slots=True)
class _OwnerMeshBinding:
    source_owner_epoch: str
    source_generation: int
    cell_id: str
    source_uri: str
    mesh_scale: tuple[float, float, float]


@dataclass(frozen=True, slots=True)
class _DraftMeshBinding:
    draft_id: str
    draft_revision: int
    source_uri: str
    mesh_scale: tuple[float, float, float]


@dataclass(slots=True)
class _MeshRecord:
    scope: Literal["owner", "draft"]
    descriptor: MeshAssetDescriptor
    binding: _OwnerMeshBinding | _DraftMeshBinding
    mesh_input: AuthorizedMeshInput | None
    converted: bytes | None = None
    conversion_error: str | None = None
    conversion_error_kind: Literal["timeout", "bounds", "parse"] | None = None
    lock: threading.Lock = field(default_factory=threading.Lock)


def _scene_mesh_error(
    status_code: int,
    code: str,
    message: str,
) -> HTTPException:
    return HTTPException(
        status_code=status_code,
        detail={"code": code, "message": message},
    )


def _owner_mesh_binding(
    state: "ComposerState",
    source_uri: str,
    mesh_scale: tuple[float, float, float],
) -> _OwnerMeshBinding:
    client = getattr(state, "headless_client", None)
    if client is None or not callable(getattr(client, "scene", None)):
        raise _scene_mesh_error(
            503,
            "scene_mesh_owner_unavailable",
            "the Headless logical-scene owner is unavailable",
        )
    try:
        projection = client.scene()
    except HeadlessClientError as error:
        raise _scene_mesh_error(
            error.status_code or 503,
            error.code,
            str(error),
        ) from error
    except Exception as error:
        raise _scene_mesh_error(
            503,
            "scene_mesh_owner_unavailable",
            "the Headless logical-scene projection could not be read",
        ) from error
    if (
        type(projection) is not dict
        or projection.get("schema_version") != "manyforge.scene_projection.v1"
    ):
        raise _scene_mesh_error(
            503,
            "scene_mesh_owner_projection_invalid",
            "the Headless scene projection has an unsupported contract",
        )
    section = projection.get("logical_scene")
    if (
        type(section) is not dict
        or section.get("available") is not True
        or section.get("freshness") != "fresh"
        or section.get("known_gaps") != []
        or type(section.get("source_owner_epoch")) is not str
        or not section["source_owner_epoch"]
        or type(section.get("source_generation")) is not int
        or isinstance(section.get("source_generation"), bool)
        or section["source_generation"] < 0
    ):
        raise _scene_mesh_error(
            409,
            "scene_mesh_owner_projection_stale",
            "the logical-scene projection is unavailable, stale, or incomplete",
        )
    payload = section.get("payload")
    snapshot = payload.get("snapshot") if type(payload) is dict else None
    if (
        type(payload) is not dict
        or payload.get("api_version") != "manyforge.logical_scene_source.v1"
        or type(snapshot) is not dict
        or type(snapshot.get("cell_id")) is not str
        or not snapshot["cell_id"]
        or type(snapshot.get("objects")) is not list
    ):
        raise _scene_mesh_error(
            503,
            "scene_mesh_owner_projection_invalid",
            "the logical-scene payload is incomplete",
        )
    if len(snapshot["objects"]) > 4_096:
        raise _scene_mesh_error(
            503,
            "scene_mesh_owner_projection_invalid",
            "the logical-scene object bound is exceeded",
        )
    matches = 0
    for scene_object in snapshot["objects"]:
        shapes = (
            scene_object.get("collision_shapes")
            if type(scene_object) is dict
            else None
        )
        if type(shapes) is not list:
            continue
        if len(shapes) > 4_096:
            raise _scene_mesh_error(
                503,
                "scene_mesh_owner_projection_invalid",
                "the logical-scene shape bound is exceeded",
            )
        for shape in shapes:
            if (
                type(shape) is not dict
                or shape.get("type") != "mesh"
                or shape.get("resource_uri") != source_uri
            ):
                continue
            scale = shape.get("scale")
            if type(scale) is not dict or set(scale) != {"x", "y", "z"}:
                continue
            try:
                candidate_scale = _normalize_scale(
                    [scale["x"], scale["y"], scale["z"]]
                )
            except SceneMeshInputError:
                continue
            if candidate_scale == mesh_scale:
                matches += 1
    if matches == 0:
        raise _scene_mesh_error(
            409,
            "scene_mesh_not_owner_authorized",
            "the requested mesh URI and scale are absent from current owner truth",
        )
    return _OwnerMeshBinding(
        source_owner_epoch=section["source_owner_epoch"],
        source_generation=section["source_generation"],
        cell_id=snapshot["cell_id"],
        source_uri=source_uri,
        mesh_scale=mesh_scale,
    )


def _draft_mesh_binding(
    state: "ComposerState",
    source_uri: str,
    mesh_scale: tuple[float, float, float],
    expected_draft_id: str,
    expected_draft_revision: int,
) -> _DraftMeshBinding:
    session = getattr(state, "draft_session", None)
    if session is None or not callable(getattr(session, "snapshot", None)):
        raise _scene_mesh_error(
            503,
            "scene_mesh_draft_unavailable",
            "the Composer Program Draft is unavailable",
        )
    snapshot = session.snapshot()
    if (
        type(snapshot) is not dict
        or type(snapshot.get("draft_id")) is not str
        or type(snapshot.get("draft_revision")) is not int
        or isinstance(snapshot.get("draft_revision"), bool)
    ):
        raise _scene_mesh_error(
            503,
            "scene_mesh_draft_invalid",
            "the Composer Program Draft identity is invalid",
        )
    if (
        snapshot["draft_id"] != expected_draft_id
        or snapshot["draft_revision"] != expected_draft_revision
    ):
        raise _scene_mesh_error(
            409,
            "scene_mesh_draft_revision_changed",
            "the mesh URL belongs to a superseded Program Draft revision",
        )
    authoring = snapshot.get("authoring")
    scene = authoring.get("scene") if type(authoring) is dict else None
    if type(scene) is not list or len(scene) > 4_096:
        raise _scene_mesh_error(
            409,
            "scene_mesh_draft_unavailable",
            "the Program Draft has no bounded initial-scene projection",
        )
    matches = 0
    for resource in scene:
        shape = resource.get("shape") if type(resource) is dict else None
        if (
            type(shape) is not dict
            or shape.get("type") != "mesh"
            or shape.get("mesh_resource_uri") != source_uri
        ):
            continue
        scale = shape.get("mesh_scale", [1.0, 1.0, 1.0])
        try:
            candidate_scale = _normalize_scale(scale)
        except SceneMeshInputError:
            continue
        if candidate_scale == mesh_scale:
            matches += 1
    if matches == 0:
        raise _scene_mesh_error(
            409,
            "scene_mesh_not_draft_authorized",
            "the requested mesh URI and scale are absent from the current Program Draft",
        )
    return _DraftMeshBinding(
        draft_id=expected_draft_id,
        draft_revision=expected_draft_revision,
        source_uri=source_uri,
        mesh_scale=mesh_scale,
    )


def _resolved_mesh_source(
    state: "ComposerState",
    source_uri: str,
) -> tuple[Path, tuple[Path, ...]]:
    uri = _bounded_text(
        source_uri,
        label="mesh_resource_uri",
        maximum=_MAX_URI_CHARS,
    )
    parsed = urlsplit(uri)
    if parsed.scheme not in {"", "file"}:
        raise SceneMeshInputError(
            "scene mesh resources must use a relative path, absolute path, or file URI"
        )
    if parsed.scheme == "file" and parsed.netloc not in {"", "localhost"}:
        raise SceneMeshInputError("scene mesh file URI must be local")
    if parsed.query or parsed.fragment:
        raise SceneMeshInputError("scene mesh resource URI cannot contain query or fragment")
    raw = unquote(parsed.path) if parsed.scheme == "file" else uri
    if "\x00" in raw or not raw:
        raise SceneMeshInputError("mesh_resource_uri is invalid")

    configured = getattr(state, "allowed_fs_roots", None)
    if type(configured) is not list or not configured:
        raise SceneMeshInputError("Composer has no configured mesh resource roots")
    roots = _resolved_allowed_roots(configured)
    path = Path(raw)
    candidates: list[Path] = []
    if path.is_absolute():
        candidates.append(path)
    else:
        program_source = getattr(state, "program_source_path", None)
        if type(program_source) is str and program_source:
            candidates.append(Path(program_source).parent / path)
        candidates.extend(root / path for root in roots)
    for candidate in candidates:
        normalized = Path(os.path.normpath(str(candidate)))
        if not normalized.is_absolute():
            continue
        try:
            _relative_to_deepest_root(normalized, roots)
        except SceneMeshInputError:
            continue
        if normalized.exists():
            return normalized, roots
    raise SceneMeshInputError(
        "mesh_resource_uri does not resolve to an allowed local source"
    )


class SceneMeshAssetService:
    """Read-only, revision-bound mesh conversion and presentation cache."""

    def __init__(self, *, limits: MeshConversionLimits | None = None) -> None:
        self._limits = limits or MeshConversionLimits.from_environment()
        if type(self._limits) is not MeshConversionLimits:
            raise TypeError("limits must be a MeshConversionLimits DTO")
        self._lock = threading.RLock()
        self._records: OrderedDict[str, _MeshRecord] = OrderedDict()
        self._cache_bytes = 0

    def _evict_one_locked(self, *, exclude: set[str] | None = None) -> bool:
        excluded = exclude or set()
        for identity, record in tuple(self._records.items()):
            if identity in excluded or record.lock.locked():
                continue
            self._records.pop(identity, None)
            if record.converted is not None:
                self._cache_bytes -= len(record.converted)
            return True
        return False

    def _record_for_shape(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
        *,
        scope: Literal["owner", "draft"] = "owner",
        draft_id: str | None = None,
        draft_revision: int | None = None,
    ) -> _MeshRecord:
        source_uri = _bounded_text(
            mesh_resource_uri,
            label="mesh_resource_uri",
            maximum=_MAX_URI_CHARS,
        )
        scale = _normalize_scale(mesh_scale)
        if scope == "owner":
            binding: _OwnerMeshBinding | _DraftMeshBinding = _owner_mesh_binding(
                state,
                source_uri,
                scale,
            )
        else:
            if draft_id is None or draft_revision is None:
                raise SceneMeshInputError("Program Draft mesh identity is required")
            binding = _draft_mesh_binding(
                state,
                source_uri,
                scale,
                draft_id,
                draft_revision,
            )
        path, roots = _resolved_mesh_source(state, source_uri)
        mesh_input = _read_authorized_mesh_input(
            path,
            list(roots),
            limits=self._limits,
        )
        asset_hash = _asset_hash(
            mesh_input.resolved_realpath,
            mesh_input.content_sha256,
            scale,
        )
        record_identity = f"{scope}:{asset_hash}"
        with self._lock:
            existing = self._records.get(record_identity)
            if existing is not None:
                existing.binding = binding
                self._records.move_to_end(record_identity)
                return existing
            while len(self._records) >= _MAX_SERVICE_RECORDS:
                if self._evict_one_locked():
                    continue
                raise SceneMeshBoundsError(
                    "all bounded scene mesh cache records are active"
                )
            record = _MeshRecord(
                scope=scope,
                descriptor=MeshAssetDescriptor(
                    asset_hash=asset_hash,
                    source_uri=source_uri,
                    resolved_realpath=mesh_input.resolved_realpath,
                    file_content_sha256=mesh_input.content_sha256,
                    mesh_scale=scale,
                ),
                binding=binding,
                mesh_input=mesh_input,
            )
            self._records[record_identity] = record
            return record

    @staticmethod
    def _record_identity(record: _MeshRecord) -> str:
        return f"{record.scope}:{record.descriptor.asset_hash}"

    def _metadata(self, record: _MeshRecord) -> dict[str, str | int | None]:
        with self._lock:
            binding = record.binding
        if not isinstance(binding, _OwnerMeshBinding):
            raise RuntimeError("owner mesh record has a Draft binding")
        status = (
            "parse_failed"
            if record.conversion_error is not None
            else "ok" if record.converted is not None else "pending"
        )
        query = urlencode(
            {
                "owner_epoch": binding.source_owner_epoch,
                "generation": str(binding.source_generation),
            }
        )
        return {
            "schema_version": _MESH_METADATA_SCHEMA,
            "asset_hash": record.descriptor.asset_hash,
            "mesh_asset_url": (
                f"/api/scene/object-meshes/{record.descriptor.asset_hash}.glb?{query}"
            ),
            "mesh_asset_status": status,
            "source_owner_epoch": binding.source_owner_epoch,
            "source_generation": binding.source_generation,
            "cell_id": binding.cell_id,
            "last_error": record.conversion_error,
        }

    def _draft_metadata(self, record: _MeshRecord) -> dict[str, str | int | None]:
        with self._lock:
            binding = record.binding
        if not isinstance(binding, _DraftMeshBinding):
            raise RuntimeError("Program Draft mesh record has an owner binding")
        status = (
            "parse_failed"
            if record.conversion_error is not None
            else "ok" if record.converted is not None else "pending"
        )
        query = urlencode(
            {
                "draft_id": binding.draft_id,
                "draft_revision": str(binding.draft_revision),
            }
        )
        return {
            "schema_version": "manyforge.program_draft_mesh_asset.v1",
            "asset_hash": record.descriptor.asset_hash,
            "mesh_asset_url": (
                f"/api/program/draft/scene-meshes/{record.descriptor.asset_hash}.glb?{query}"
            ),
            "mesh_asset_status": status,
            "draft_id": binding.draft_id,
            "draft_revision": binding.draft_revision,
            "last_error": record.conversion_error,
        }

    def metadata_for_shape(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
        *,
        schedule_conversion: bool = True,
    ) -> dict[str, str | int | None]:
        del schedule_conversion
        return self._metadata(
            self._record_for_shape(state, mesh_resource_uri, mesh_scale)
        )

    def metadata_for_draft_shape(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
        draft_id: str,
        draft_revision: int,
    ) -> dict[str, str | int | None]:
        return self._draft_metadata(
            self._record_for_shape(
                state,
                mesh_resource_uri,
                mesh_scale,
                scope="draft",
                draft_id=draft_id,
                draft_revision=draft_revision,
            )
        )

    def validate_boundary_or_409(
        self,
        state: "ComposerState",
        mesh_resource_uri: str | None,
        mesh_scale: list[float] | tuple[float, ...] | None,
    ) -> None:
        try:
            self._record_for_shape(state, mesh_resource_uri, mesh_scale)
        except HTTPException:
            raise
        except SceneMeshBoundsError as error:
            raise _scene_mesh_error(
                413,
                "scene_mesh_bounds_exceeded",
                str(error),
            ) from error
        except SceneMeshInputError as error:
            raise _scene_mesh_error(
                409,
                "scene_mesh_input_invalid",
                str(error),
            ) from error

    def prewarm(self, state: "ComposerState") -> None:
        client = getattr(state, "headless_client", None)
        if client is None:
            return
        try:
            projection = client.scene()
            objects = projection["logical_scene"]["payload"]["snapshot"]["objects"]
        except Exception:
            return
        for scene_object in objects[:4_096]:
            if type(scene_object) is not dict:
                continue
            shapes = scene_object.get("collision_shapes")
            if type(shapes) is not list:
                continue
            for shape in shapes[:4_096]:
                if type(shape) is not dict or shape.get("type") != "mesh":
                    continue
                scale = shape.get("scale", {})
                if type(scale) is not dict:
                    continue
                try:
                    self._record_for_shape(
                        state,
                        shape.get("resource_uri"),
                        [scale.get("x"), scale.get("y"), scale.get("z")],
                    )
                except Exception:
                    logger.debug("scene mesh prewarm admission failed", exc_info=True)

    def _validate_binding(
        self,
        state: "ComposerState",
        record: _MeshRecord,
        expected_owner_epoch: str,
        expected_generation: int,
    ) -> _OwnerMeshBinding:
        with self._lock:
            record_identity = self._record_identity(record)
            if self._records.get(record_identity) is not record:
                raise _scene_mesh_error(
                    404,
                    "scene_mesh_asset_unknown",
                    "the scene mesh asset has left the bounded cache",
                )
            binding = record.binding
        if not isinstance(binding, _OwnerMeshBinding):
            raise _scene_mesh_error(
                404,
                "scene_mesh_asset_unknown",
                "the requested asset is not owner-scoped",
            )
        if (
            binding.source_owner_epoch != expected_owner_epoch
            or binding.source_generation != expected_generation
        ):
            raise _scene_mesh_error(
                409,
                "scene_mesh_owner_generation_changed",
                "the mesh URL belongs to a superseded logical-scene generation",
            )
        current = _owner_mesh_binding(
            state,
            binding.source_uri,
            binding.mesh_scale,
        )
        with self._lock:
            if self._records.get(record_identity) is not record:
                raise _scene_mesh_error(
                    404,
                    "scene_mesh_asset_unknown",
                    "the scene mesh asset has left the bounded cache",
                )
            if current != binding or record.binding != binding:
                raise _scene_mesh_error(
                    409,
                    "scene_mesh_owner_generation_changed",
                    "the mesh URL belongs to a superseded logical-scene generation",
                )
            self._records.move_to_end(record_identity)
        return binding

    def _validate_draft_binding(
        self,
        state: "ComposerState",
        record: _MeshRecord,
        expected_draft_id: str,
        expected_draft_revision: int,
    ) -> _DraftMeshBinding:
        record_identity = self._record_identity(record)
        with self._lock:
            if self._records.get(record_identity) is not record:
                raise _scene_mesh_error(
                    404,
                    "scene_mesh_asset_unknown",
                    "the scene mesh asset has left the bounded cache",
                )
            binding = record.binding
        if not isinstance(binding, _DraftMeshBinding):
            raise _scene_mesh_error(
                404,
                "scene_mesh_asset_unknown",
                "the requested asset is not Program-Draft-scoped",
            )
        if (
            binding.draft_id != expected_draft_id
            or binding.draft_revision != expected_draft_revision
        ):
            raise _scene_mesh_error(
                409,
                "scene_mesh_draft_revision_changed",
                "the mesh URL belongs to a superseded Program Draft revision",
            )
        current = _draft_mesh_binding(
            state,
            binding.source_uri,
            binding.mesh_scale,
            expected_draft_id,
            expected_draft_revision,
        )
        with self._lock:
            if (
                self._records.get(record_identity) is not record
                or current != binding
                or record.binding != binding
            ):
                raise _scene_mesh_error(
                    409,
                    "scene_mesh_draft_revision_changed",
                    "the mesh URL belongs to a superseded Program Draft revision",
                )
            self._records.move_to_end(record_identity)
        return binding

    @staticmethod
    def _raise_cached_conversion(record: _MeshRecord) -> None:
        message = record.conversion_error or "scene mesh conversion failed"
        if record.conversion_error_kind == "timeout":
            raise TimeoutError(message)
        if record.conversion_error_kind == "bounds":
            raise SceneMeshBoundsError(message)
        raise SceneMeshInputError(message)

    def _convert(self, record: _MeshRecord) -> bytes:
        with record.lock:
            if record.converted is not None:
                return record.converted
            if record.conversion_error is not None:
                self._raise_cached_conversion(record)
            mesh_input = record.mesh_input
            if mesh_input is None:
                raise SceneMeshInputError("scene mesh source snapshot is unavailable")
            try:
                result = _convert_mesh_asset_subprocess(
                    mesh_input,
                    record.descriptor.mesh_scale,
                    limits=self._limits,
                )
            except Exception as error:
                record.conversion_error = _bounded_worker_message(error)
                record.conversion_error_kind = (
                    "timeout"
                    if isinstance(error, TimeoutError)
                    else "bounds"
                    if isinstance(error, SceneMeshBoundsError)
                    else "parse"
                )
                record.mesh_input = None
                raise
            with self._lock:
                record_identity = self._record_identity(record)
                if self._records.get(record_identity) is not record:
                    return result
                while self._cache_bytes + len(result) > _MAX_SERVICE_CACHE_BYTES:
                    if self._evict_one_locked(
                        exclude={record_identity},
                    ):
                        continue
                    raise SceneMeshBoundsError(
                        "all bounded scene mesh cache bytes are active"
                    )
                record.converted = result
                record.mesh_input = None
                self._cache_bytes += len(result)
                self._records.move_to_end(record_identity)
            return result

    def get_glb_response(
        self,
        state: "ComposerState",
        asset_hash: str,
        expected_owner_epoch: str,
        expected_generation: int,
    ) -> Response:
        try:
            identity = _validate_lower_hex(
                asset_hash,
                label="asset_hash",
                length=16,
            )
        except SceneMeshInputError as error:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": {
                        "code": "scene_mesh_asset_unknown",
                        "message": str(error),
                    }
                },
            )
        with self._lock:
            record = self._records.get(f"owner:{identity}")
        if record is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": {
                        "code": "scene_mesh_asset_unknown",
                        "message": "unknown scene mesh asset",
                    }
                },
            )
        try:
            binding = self._validate_binding(
                state,
                record,
                expected_owner_epoch,
                expected_generation,
            )
            content = self._convert(record)
        except HTTPException as error:
            return JSONResponse(
                status_code=error.status_code,
                content={"detail": error.detail},
            )
        except TimeoutError as error:
            return JSONResponse(
                status_code=504,
                content={
                    "detail": {
                        "code": "scene_mesh_conversion_timeout",
                        "message": str(error),
                    }
                },
            )
        except SceneMeshBoundsError as error:
            return JSONResponse(
                status_code=413,
                content={
                    "detail": {
                        "code": "scene_mesh_bounds_exceeded",
                        "message": str(error),
                    }
                },
            )
        except Exception as error:
            return JSONResponse(
                status_code=422,
                content={
                    "detail": {
                        "code": "scene_mesh_conversion_failed",
                        "message": _bounded_worker_message(error),
                    }
                },
            )
        return Response(
            content=content,
            media_type="model/gltf-binary",
            headers={
                "Cache-Control": "private, no-cache",
                "ETag": f'"{identity}"',
                "X-ManyForge-Scene-Owner-Epoch": binding.source_owner_epoch,
                "X-ManyForge-Scene-Generation": str(binding.source_generation),
            },
        )

    def get_draft_glb_response(
        self,
        state: "ComposerState",
        asset_hash: str,
        expected_draft_id: str,
        expected_draft_revision: int,
    ) -> Response:
        try:
            identity = _validate_lower_hex(
                asset_hash,
                label="asset_hash",
                length=16,
            )
        except SceneMeshInputError as error:
            return JSONResponse(
                status_code=404,
                content={"detail": {"code": "scene_mesh_asset_unknown", "message": str(error)}},
            )
        with self._lock:
            record = self._records.get(f"draft:{identity}")
        if record is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": {
                        "code": "scene_mesh_asset_unknown",
                        "message": "unknown Program Draft scene mesh asset",
                    }
                },
            )
        try:
            binding = self._validate_draft_binding(
                state,
                record,
                expected_draft_id,
                expected_draft_revision,
            )
            content = self._convert(record)
        except HTTPException as error:
            return JSONResponse(status_code=error.status_code, content={"detail": error.detail})
        except TimeoutError as error:
            return JSONResponse(
                status_code=504,
                content={"detail": {"code": "scene_mesh_conversion_timeout", "message": str(error)}},
            )
        except SceneMeshBoundsError as error:
            return JSONResponse(
                status_code=413,
                content={"detail": {"code": "scene_mesh_bounds_exceeded", "message": str(error)}},
            )
        except Exception as error:
            return JSONResponse(
                status_code=422,
                content={
                    "detail": {
                        "code": "scene_mesh_conversion_failed",
                        "message": _bounded_worker_message(error),
                    }
                },
            )
        return Response(
            content=content,
            media_type="model/gltf-binary",
            headers={
                "Cache-Control": "private, no-cache",
                "ETag": f'"{identity}"',
                "X-ManyForge-Draft-Id": binding.draft_id,
                "X-ManyForge-Draft-Revision": str(binding.draft_revision),
            },
        )


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
) -> dict[str, str | int | None]:
    return get_scene_mesh_asset_service(state).metadata_for_shape(
        state,
        mesh_resource_uri,
        mesh_scale,
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
    service = get_scene_mesh_asset_service(state)
    router = APIRouter()

    @router.get("/api/scene/object-meshes/resolve")
    def resolve_scene_object_mesh(
        resource_uri: str = Query(min_length=1, max_length=_MAX_URI_CHARS),
        scale_x: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
        scale_y: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
        scale_z: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
    ) -> dict[str, str | int | None]:
        try:
            return service.metadata_for_shape(
                state,
                resource_uri,
                [scale_x, scale_y, scale_z],
            )
        except HTTPException:
            raise
        except SceneMeshBoundsError as error:
            raise _scene_mesh_error(
                413,
                "scene_mesh_bounds_exceeded",
                str(error),
            ) from error
        except SceneMeshInputError as error:
            raise _scene_mesh_error(
                409,
                "scene_mesh_input_invalid",
                str(error),
            ) from error

    @router.get("/api/scene/object-meshes/{asset_hash}.glb")
    def get_scene_object_mesh(
        asset_hash: str = ApiPath(min_length=16, max_length=16),
        owner_epoch: str = Query(min_length=1, max_length=256),
        generation: int = Query(ge=0),
    ) -> Response:
        return service.get_glb_response(
            state,
            asset_hash,
            owner_epoch,
            generation,
        )

    @router.get("/api/program/draft/scene-meshes/resolve")
    def resolve_program_draft_scene_mesh(
        resource_uri: str = Query(min_length=1, max_length=_MAX_URI_CHARS),
        scale_x: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
        scale_y: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
        scale_z: float = Query(gt=0.0, le=_MAX_SCALE_COMPONENT),
        draft_id: str = Query(min_length=1, max_length=256),
        draft_revision: int = Query(ge=0),
    ) -> dict[str, str | int | None]:
        try:
            return service.metadata_for_draft_shape(
                state,
                resource_uri,
                [scale_x, scale_y, scale_z],
                draft_id,
                draft_revision,
            )
        except HTTPException:
            raise
        except SceneMeshBoundsError as error:
            raise _scene_mesh_error(413, "scene_mesh_bounds_exceeded", str(error)) from error
        except SceneMeshInputError as error:
            raise _scene_mesh_error(409, "scene_mesh_input_invalid", str(error)) from error

    @router.get("/api/program/draft/scene-meshes/{asset_hash}.glb")
    def get_program_draft_scene_mesh(
        asset_hash: str = ApiPath(min_length=16, max_length=16),
        draft_id: str = Query(min_length=1, max_length=256),
        draft_revision: int = Query(ge=0),
    ) -> Response:
        return service.get_draft_glb_response(
            state,
            asset_hash,
            draft_id,
            draft_revision,
        )

    return router


__all__ = [
    "AuthorizedMeshInput",
    "MeshAssetDescriptor",
    "MeshConversionLimits",
    "SceneMeshAssetService",
    "SceneMeshBoundsError",
    "SceneMeshInputError",
    "create_scene_meshes_router",
    "get_scene_mesh_asset_service",
    "scene_mesh_shape_metadata",
    "validate_scene_mesh_resource_or_409",
]
