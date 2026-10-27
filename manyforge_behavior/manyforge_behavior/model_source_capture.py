"""Capture exact Deployment model sources for preparation and export.

The authored Deployment retains user-managed paths.  This module resolves those
paths once at the authoring boundary and returns a JSON-safe immutable closure;
downstream preparation never reopens the mutable source paths.
"""

from __future__ import annotations

import base64
from collections import OrderedDict
import gzip
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import stat
import threading
from typing import Any, Final, Mapping
import xml.parsers.expat


SOURCE_CLOSURE_SCHEMA: Final = "manyforge.activation_model_source_closure.v1"
MAX_XML_BYTES: Final = 16 * 1024 * 1024
MAX_ARTIFACT_BYTES: Final = 256 * 1024 * 1024
MAX_CLOSURE_BYTES: Final = 512 * 1024 * 1024
MAX_RESOURCES: Final = 4_096
MAX_XML_DEPTH: Final = 128
MAX_XML_ELEMENTS: Final = 250_000
MAX_XML_ATTRIBUTES: Final = 1_000_000
_TRANSFER_CHUNK_BYTES: Final = 512 * 1024
_ENCODED_MEMO_ENTRIES: Final = 1_024
_ENCODED_MEMO_BYTES: Final = 256 * 1024 * 1024


class ModelSourceCaptureError(ValueError):
    """The authored source set cannot be captured as one bounded closure."""


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def encode_model_source_artifact(
    value: bytes, *, media_type: str
) -> dict[str, Any]:
    """Encode one already bounded artifact for the native operation wire."""

    if not isinstance(value, bytes) or not value:
        raise ModelSourceCaptureError("model source artifact must be non-empty bytes")
    if not isinstance(media_type, str) or not media_type:
        raise ModelSourceCaptureError("model source media type must be non-empty text")
    transferred = gzip.compress(value, compresslevel=6, mtime=0)
    return {
        "byte_size": len(value),
        "chunks": [
            base64.b64encode(
                transferred[offset : offset + _TRANSFER_CHUNK_BYTES]
            ).decode("ascii")
            for offset in range(0, len(transferred), _TRANSFER_CHUNK_BYTES)
        ],
        "content_digest": _digest(value),
        "encoding": "gzip+base64",
        "media_type": media_type,
        "transferred_byte_size": len(transferred),
    }


_FileIdentity = tuple[int, int, int, int, int]


def _file_identity(status: os.stat_result) -> _FileIdentity:
    return (
        status.st_dev,
        status.st_ino,
        status.st_size,
        status.st_mtime_ns,
        status.st_ctime_ns,
    )


class _EncodedSourceMemo:
    """Bounded reuse of one source file's wire encoding.

    Every capture still reads and hashes the file exactly as before, so a
    missing, replaced or rewritten file is detected at the same point and with
    the same error. Only the compression is reused, and only for the same
    path, file identity (device, inode, size, mtime, ctime), content digest
    and media type: the encoding is a pure function of the bytes just read,
    so a reused value can never be stale. Least recently used entries are
    evicted beyond the entry and transferred-byte bounds.
    """

    def __init__(self, *, entries: int, transferred_bytes: int) -> None:
        self._entries = entries
        self._transferred_bytes = transferred_bytes
        self._lock = threading.Lock()
        self._values: OrderedDict[tuple[Any, ...], dict[str, Any]] = OrderedDict()
        self._size = 0

    def encode(
        self,
        value: bytes,
        *,
        path: str,
        identity: _FileIdentity,
        media_type: str,
    ) -> dict[str, Any]:
        key = (path, identity, _digest(value), media_type)
        with self._lock:
            known = self._values.get(key)
            if known is not None:
                self._values.move_to_end(key)
                return {**known, "chunks": list(known["chunks"])}
        encoded = encode_model_source_artifact(value, media_type=media_type)
        size = encoded["transferred_byte_size"]
        if size <= self._transferred_bytes:
            with self._lock:
                previous = self._values.pop(key, None)
                if previous is not None:
                    self._size -= previous["transferred_byte_size"]
                self._values[key] = {**encoded, "chunks": tuple(encoded["chunks"])}
                self._size += size
                while (
                    len(self._values) > self._entries
                    or self._size > self._transferred_bytes
                ):
                    _key, evicted = self._values.popitem(last=False)
                    self._size -= evicted["transferred_byte_size"]
        return encoded


_ENCODED_SOURCES = _EncodedSourceMemo(
    entries=_ENCODED_MEMO_ENTRIES, transferred_bytes=_ENCODED_MEMO_BYTES
)


def decode_model_source_artifact(
    artifact: Mapping[str, Any], *, maximum_bytes: int, label: str
) -> bytes:
    """Decode and independently verify one bounded operation-wire artifact."""

    expected = {
        "byte_size",
        "chunks",
        "content_digest",
        "encoding",
        "media_type",
        "transferred_byte_size",
    }
    if set(artifact) != expected:
        raise ModelSourceCaptureError(f"{label} has an invalid transfer shape")
    byte_size = artifact.get("byte_size")
    transferred_size = artifact.get("transferred_byte_size")
    chunks = artifact.get("chunks")
    digest = artifact.get("content_digest")
    if (
        type(byte_size) is not int
        or not 0 < byte_size <= maximum_bytes
        or type(transferred_size) is not int
        or not 0 < transferred_size <= maximum_bytes
        or artifact.get("encoding") != "gzip+base64"
        or not isinstance(artifact.get("media_type"), str)
        or not artifact["media_type"]
        or not isinstance(digest, str)
        or not digest.startswith("sha256:")
        or len(digest) != 71
        or not isinstance(chunks, list)
        or not chunks
    ):
        raise ModelSourceCaptureError(f"{label} has invalid transfer metadata")
    encoded_chunk_bound = ((_TRANSFER_CHUNK_BYTES + 2) // 3) * 4
    if len(chunks) > (transferred_size + _TRANSFER_CHUNK_BYTES - 1) // _TRANSFER_CHUNK_BYTES:
        raise ModelSourceCaptureError(f"{label} has too many transfer chunks")
    transferred_parts: list[bytes] = []
    for chunk in chunks:
        if (
            not isinstance(chunk, str)
            or not chunk
            or len(chunk) > encoded_chunk_bound
        ):
            raise ModelSourceCaptureError(f"{label} has an invalid transfer chunk")
        try:
            transferred_parts.append(base64.b64decode(chunk, validate=True))
        except (ValueError, base64.binascii.Error) as error:
            raise ModelSourceCaptureError(
                f"{label} has invalid base64 transfer data"
            ) from error
    transferred = b"".join(transferred_parts)
    if len(transferred) != transferred_size:
        raise ModelSourceCaptureError(f"{label} transferred size does not match")
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(transferred), mode="rb") as stream:
            value = stream.read(maximum_bytes + 1)
            if stream.read(1):
                raise ModelSourceCaptureError(f"{label} exceeds its decoded bound")
    except (EOFError, OSError) as error:
        raise ModelSourceCaptureError(f"{label} has invalid gzip data") from error
    if len(value) != byte_size or _digest(value) != digest:
        raise ModelSourceCaptureError(f"{label} identity does not match its bytes")
    return value


def _read_plain_file(
    path: Path, *, maximum: int, label: str
) -> tuple[bytes, _FileIdentity]:
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    directory_flags = flags | os.O_DIRECTORY
    descriptors: list[int] = []
    try:
        absolute = Path(os.path.abspath(path))
        if not absolute.is_absolute() or len(absolute.parts) < 2:
            raise ModelSourceCaptureError(f"{label} has an invalid absolute path")
        parent = os.open("/", directory_flags)
        descriptors.append(parent)
        for part in absolute.parts[1:-1]:
            child = os.open(part, directory_flags, dir_fd=parent)
            descriptors.append(child)
            parent = child
        descriptor = os.open(absolute.parts[-1], flags, dir_fd=parent)
        descriptors.append(descriptor)
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ModelSourceCaptureError(
                f"{label} is not one non-empty bounded plain file"
            )
        chunks: list[bytes] = []
        remaining = before.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise ModelSourceCaptureError(f"{label} changed during capture")
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ModelSourceCaptureError(f"{label} changed during capture")
        after = os.fstat(descriptor)
        if (
            after.st_dev != before.st_dev
            or after.st_ino != before.st_ino
            or after.st_size != before.st_size
            or after.st_mtime_ns != before.st_mtime_ns
        ):
            raise ModelSourceCaptureError(f"{label} changed during capture")
        return b"".join(chunks), _file_identity(after)
    except OSError as error:
        raise ModelSourceCaptureError(f"could not read {label}: {error}") from error
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def _reference_path(reference: Mapping[str, Any], *, project_root: Path) -> Path:
    if set(reference) == {"project_path"} and isinstance(
        reference.get("project_path"), str
    ):
        return Path(os.path.abspath(project_root / reference["project_path"]))
    if set(reference) == {"external_path"} and isinstance(
        reference.get("external_path"), str
    ):
        return Path(reference["external_path"])
    raise ModelSourceCaptureError("model source reference has an invalid shape")


def _safe_resource_path(value: str) -> tuple[str, ...]:
    path = PurePosixPath(value)
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or path.is_absolute()
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ModelSourceCaptureError(
            f"package resource has an unsafe relative path: {value!r}"
        )
    return path.parts


def _read_package_resource(
    root: Path, relative: str
) -> tuple[bytes, _FileIdentity]:
    parts = _safe_resource_path(relative)
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    directory_flags = flags | os.O_DIRECTORY
    descriptors: list[int] = []
    try:
        parent = os.open(root, directory_flags)
        descriptors.append(parent)
        for part in parts[:-1]:
            child = os.open(part, directory_flags, dir_fd=parent)
            descriptors.append(child)
            parent = child
        descriptor = os.open(parts[-1], flags, dir_fd=parent)
        descriptors.append(descriptor)
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or not 0 < before.st_size <= MAX_ARTIFACT_BYTES
        ):
            raise ModelSourceCaptureError(
                f"package resource is not one bounded plain file: {relative!r}"
            )
        chunks: list[bytes] = []
        remaining = before.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise ModelSourceCaptureError(
                    f"package resource changed during capture: {relative!r}"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ModelSourceCaptureError(
                f"package resource changed during capture: {relative!r}"
            )
        after = os.fstat(descriptor)
        if (
            after.st_dev != before.st_dev
            or after.st_ino != before.st_ino
            or after.st_size != before.st_size
            or after.st_mtime_ns != before.st_mtime_ns
        ):
            raise ModelSourceCaptureError(
                f"package resource changed during capture: {relative!r}"
            )
        return b"".join(chunks), _file_identity(after)
    except OSError as error:
        raise ModelSourceCaptureError(
            f"could not read package resource {relative!r}: {error}"
        ) from error
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


class _ResourceScanner:
    def __init__(self) -> None:
        self.depth = 0
        self.elements = 0
        self.attributes = 0
        self.resources: set[str] = set()
        self.parser: Any = None

    def fail(self, message: str) -> None:
        raise ModelSourceCaptureError(message)

    def start(self, name: str, attributes: list[str]) -> None:
        if self.depth >= MAX_XML_DEPTH:
            self.fail("URDF exceeds the XML nesting bound")
        self.depth += 1
        self.elements += 1
        self.attributes += len(attributes) // 2
        if self.elements > MAX_XML_ELEMENTS or self.attributes > MAX_XML_ATTRIBUTES:
            self.fail("URDF exceeds the XML shape bound")
        if ":" in name or len(attributes) % 2:
            self.fail("URDF uses an unsupported XML shape")
        values: dict[str, str] = {}
        for index in range(0, len(attributes), 2):
            key = attributes[index]
            if ":" in key or key in values:
                self.fail("URDF uses namespaces or duplicate attributes")
            values[key] = attributes[index + 1]
        if name in {"mesh", "texture"}:
            filename = values.get("filename")
            if filename is None:
                self.fail(f"URDF <{name}> has no filename")
            if not filename.startswith("package://"):
                self.fail(
                    f"URDF <{name}> filename is not a package URI: {filename!r}"
                )
            self.resources.add(filename)

    def end(self, _name: str) -> None:
        self.depth -= 1

    def forbidden(self, *_args: Any) -> None:
        self.fail("URDF contains a forbidden XML construct")


def _resource_uris(urdf: bytes) -> tuple[str, ...]:
    scanner = _ResourceScanner()
    parser = xml.parsers.expat.ParserCreate()
    scanner.parser = parser
    parser.ordered_attributes = True
    parser.specified_attributes = True
    parser.StartElementHandler = scanner.start
    parser.EndElementHandler = scanner.end
    parser.StartDoctypeDeclHandler = scanner.forbidden
    parser.EntityDeclHandler = scanner.forbidden
    parser.ExternalEntityRefHandler = lambda *_args: scanner.forbidden()
    parser.ProcessingInstructionHandler = scanner.forbidden
    try:
        parser.Parse(urdf, True)
    except ModelSourceCaptureError:
        raise
    except xml.parsers.expat.ExpatError as error:
        raise ModelSourceCaptureError(f"URDF is malformed: {error}") from error
    return tuple(sorted(scanner.resources))


def _resource_media_type(uri: str) -> str:
    suffix = PurePosixPath(uri).suffix.casefold()
    result = {
        ".dae": "model/vnd.collada+xml",
        ".stl": "model/stl",
        ".jpeg": "image/jpeg",
        ".jpg": "image/jpeg",
        ".png": "image/png",
    }.get(suffix)
    if result is None:
        raise ModelSourceCaptureError(
            f"package resource has an unsupported extension: {uri!r}"
        )
    return result


def capture_deployment_model_sources(
    deployment: Mapping[str, Any], *, project_root: Path
) -> dict[str, Any]:
    """Capture each component's exact model-source closure once."""

    components = deployment.get("components")
    if not isinstance(components, list):
        raise ModelSourceCaptureError("Deployment model components must be a list")
    captured: list[dict[str, Any]] = []
    total_bytes = 0
    total_resources = 0
    seen_components: set[str] = set()
    for raw_component in components:
        if not isinstance(raw_component, Mapping):
            raise ModelSourceCaptureError("Deployment component is not an object")
        component_id = raw_component.get("component_id")
        source = raw_component.get("model_source")
        configuration = raw_component.get("configuration")
        if (
            source is None
            and "model_source" not in raw_component
            and isinstance(configuration, Mapping)
            and configuration.get("runtime_kind") in ("io_provider", "pose_source", "ai_model")
        ):
            # A model-free I/O device, pose source or AI model has no model source closure.
            if (
                not isinstance(component_id, str)
                or not component_id
                or component_id in seen_components
            ):
                raise ModelSourceCaptureError("Deployment component source is invalid")
            seen_components.add(component_id)
            continue
        if (
            not isinstance(component_id, str)
            or not component_id
            or component_id in seen_components
            or not isinstance(source, Mapping)
        ):
            raise ModelSourceCaptureError("Deployment component source is invalid")
        seen_components.add(component_id)
        urdf_path = _reference_path(source.get("urdf", {}), project_root=project_root)
        urdf, urdf_identity = _read_plain_file(
            urdf_path, maximum=MAX_XML_BYTES, label=f"{component_id} URDF"
        )
        srdf_reference = source.get("srdf")
        srdf_path = (
            None
            if srdf_reference is None
            else _reference_path(srdf_reference, project_root=project_root)
        )
        srdf, srdf_identity = (
            (None, None)
            if srdf_path is None
            else _read_plain_file(
                srdf_path, maximum=MAX_XML_BYTES, label=f"{component_id} SRDF"
            )
        )
        package_roots = source.get("package_roots")
        if not isinstance(package_roots, Mapping):
            raise ModelSourceCaptureError(
                f"{component_id} package_roots is not an object"
            )
        resolved_roots = {
            name: _reference_path(reference, project_root=project_root)
            for name, reference in package_roots.items()
            if isinstance(name, str) and isinstance(reference, Mapping)
        }
        if len(resolved_roots) != len(package_roots):
            raise ModelSourceCaptureError(
                f"{component_id} package_roots has an invalid row"
            )
        resources: list[dict[str, Any]] = []
        for uri in _resource_uris(urdf):
            remainder = uri.removeprefix("package://")
            package_name, separator, relative = remainder.partition("/")
            root = resolved_roots.get(package_name)
            if not separator or root is None:
                raise ModelSourceCaptureError(
                    f"{component_id} has no root for {uri!r}"
                )
            value, identity = _read_package_resource(root, relative)
            total_bytes += len(value)
            total_resources += 1
            if total_resources > MAX_RESOURCES or total_bytes > MAX_CLOSURE_BYTES:
                raise ModelSourceCaptureError("model source closure exceeds its bound")
            resources.append(
                {
                    "logical_uri": uri,
                    **_ENCODED_SOURCES.encode(
                        value,
                        path=os.path.join(os.path.abspath(root), *_safe_resource_path(relative)),
                        identity=identity,
                        media_type=_resource_media_type(uri),
                    ),
                }
            )
        total_bytes += len(urdf) + (0 if srdf is None else len(srdf))
        if total_bytes > MAX_CLOSURE_BYTES:
            raise ModelSourceCaptureError("model source closure exceeds its bound")
        entrypoints: dict[str, Any] = {
            "urdf": _ENCODED_SOURCES.encode(
                urdf,
                path=os.path.abspath(urdf_path),
                identity=urdf_identity,
                media_type="application/xml",
            )
        }
        if srdf is not None:
            entrypoints["srdf"] = _ENCODED_SOURCES.encode(
                srdf,
                path=os.path.abspath(srdf_path),
                identity=srdf_identity,
                media_type="application/xml",
            )
        captured.append(
            {
                "component_id": component_id,
                "entrypoints": entrypoints,
                "resources": resources,
            }
        )
    captured.sort(key=lambda row: row["component_id"])
    return {
        "schema_version": SOURCE_CLOSURE_SCHEMA,
        "components": captured,
    }
