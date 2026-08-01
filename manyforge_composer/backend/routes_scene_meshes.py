"""Bounded mesh transport primitives and the explicit I9 client gate.

The last full Composer implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``.  Source blob
``b0a37123e4cb89de32e7cdec055a63b2352bede3`` records limit parsing and
scale validation at lines 63-113, the descriptor and state-bound cache/service
at lines 116-633, content identity at lines 636-659, path-based conversion at
lines 662-739, and state/router integration at lines 742-794.  Removed scene
lookup, resource resolution, cache, prewarming, and HTTP behavior remain exact
I9 restoration inputs, not dormant fallback code.

This module retains only owner-neutral identity, exact DTO validation, one
contained no-symlink source snapshot, and subprocess-isolated conversion of
that immutable snapshot.  The worker receives no source path or external
resource resolver.  OBJ materials and GLTF external buffers/images are not
loaded; DAE returns at I9 because the pinned image has no pycollada parser and
its XML/resource closure is not captured here.  Worker address space, output
file size, CPU time, and post-start parent wait/teardown are bounded; OS process
creation and a kernel task that remains alive after SIGKILL cannot be made a
hard user-space wall-time guarantee.  The parent accepts only a bounded regular
structurally valid GLB from a private temporary directory.  All state-bound and
route entrypoints fail before reading state.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
import json
import math
import multiprocessing as mp
import os
from pathlib import Path
import stat as stat_module
import struct
import tempfile
import time
from typing import Any, Final, Literal, TYPE_CHECKING

from fastapi import APIRouter

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

SCENE_MESH_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_scene_mesh_client_unavailable"
)
SCENE_MESH_RESTORATION_GATE: Final = "I9"


class SceneMeshInputError(ValueError):
    """Raised when a retained mesh input is malformed or unauthorized."""


class SceneMeshBoundsError(SceneMeshInputError):
    """Raised before retained mesh work can exceed a hard resource bound."""


class SceneMeshClientUnavailableError(RuntimeError):
    """Raised before state lookup, cache creation, or route registration."""


def _raise_scene_mesh_client_unavailable() -> None:
    raise SceneMeshClientUnavailableError(
        f"{SCENE_MESH_CLIENT_UNAVAILABLE_REASON_CODE}: state-bound scene mesh "
        "lookup and routing return at I9 on the new observer owner"
    )


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
            "DAE conversion is unavailable until I9 captures and qualifies its "
            "XML/resource closure"
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


class SceneMeshAssetService:
    """Stable I9 reconnection marker; it owns no state or cache today."""

    def metadata_for_shape(
        self,
        *_args: object,
        **_kwargs: object,
    ) -> dict[str, str | None]:
        _raise_scene_mesh_client_unavailable()

    def validate_boundary_or_409(
        self,
        *_args: object,
        **_kwargs: object,
    ) -> None:
        _raise_scene_mesh_client_unavailable()

    def prewarm(self, *_args: object, **_kwargs: object) -> None:
        _raise_scene_mesh_client_unavailable()

    def get_glb_response(self, *_args: object, **_kwargs: object) -> None:
        _raise_scene_mesh_client_unavailable()


def get_scene_mesh_asset_service(_state: "ComposerState") -> SceneMeshAssetService:
    _raise_scene_mesh_client_unavailable()


def scene_mesh_shape_metadata(
    _state: "ComposerState",
    _mesh_resource_uri: str | None,
    _mesh_scale: list[float] | tuple[float, ...] | None,
) -> dict[str, str | None]:
    _raise_scene_mesh_client_unavailable()


def validate_scene_mesh_resource_or_409(
    _state: "ComposerState",
    _mesh_resource_uri: str | None,
    _mesh_scale: list[float] | tuple[float, ...] | None,
) -> None:
    _raise_scene_mesh_client_unavailable()


def create_scene_meshes_router(_state: "ComposerState") -> APIRouter:
    _raise_scene_mesh_client_unavailable()


__all__ = [
    "AuthorizedMeshInput",
    "MeshAssetDescriptor",
    "MeshConversionLimits",
    "SceneMeshAssetService",
    "SceneMeshBoundsError",
    "SceneMeshClientUnavailableError",
    "SceneMeshInputError",
    "create_scene_meshes_router",
    "get_scene_mesh_asset_service",
    "scene_mesh_shape_metadata",
    "validate_scene_mesh_resource_or_409",
]
