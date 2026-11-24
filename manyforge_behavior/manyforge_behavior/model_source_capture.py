"""Capture exact Deployment model sources and Program scene meshes.

The authored Deployment and Program retain user-managed paths. This module
resolves those paths once at the authoring boundary, under the one project
path rule (``project_paths``), and returns JSON-safe immutable closures;
downstream preparation never reopens the mutable source paths.
"""

from __future__ import annotations

import base64
from collections import OrderedDict
from copy import deepcopy
import gzip
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import re
import stat
import threading
from typing import Any, Final, Iterable, Iterator, Literal, Mapping
import xml.parsers.expat

from . import parametric_tool
from .project_paths import ProjectPathError, ProjectPathRule


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
    """The authored source set cannot be captured as one bounded closure.

    ``code`` is an error-catalog code when the refusal has its own (a
    parameter file's ``parametric_tool_*``); otherwise ``None`` and the
    caller reports its boundary's code.
    """

    def __init__(self, message: str, *, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


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


def _reference_path(
    reference: Mapping[str, Any],
    *,
    project_root: Path,
    paths: ProjectPathRule,
    label: str,
    kind: Literal["file", "directory"] = "file",
) -> Path:
    """One model-source reference under the project path rule: a
    ``project_path`` relative to the Scenario project folder or an absolute
    ``external_path``, inside an allowed folder."""

    if set(reference) == {"project_path"} and isinstance(
        reference.get("project_path"), str
    ):
        text = reference["project_path"]
        if os.path.isabs(text):
            raise ModelSourceCaptureError(
                f"{label} project_path {text!r} must be relative"
            )
    elif set(reference) == {"external_path"} and isinstance(
        reference.get("external_path"), str
    ):
        text = reference["external_path"]
        if not os.path.isabs(text):
            raise ModelSourceCaptureError(
                f"{label} external_path {text!r} must be absolute"
            )
    else:
        raise ModelSourceCaptureError("model source reference has an invalid shape")
    try:
        return paths.resolve(
            text, project_folder=project_root, label=label, kind=kind
        )
    except ProjectPathError as error:
        raise ModelSourceCaptureError(str(error)) from error


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


def _authored_model_source(
    component_id: str,
    source: Mapping[str, Any],
    *,
    project_root: Path,
    paths: ProjectPathRule,
) -> tuple[bytes, bytes | None, dict[str, Any], dict[str, Path]]:
    """A project URDF, its optional SRDF and package roots, read once."""

    urdf_path = _reference_path(
        source.get("urdf", {}), project_root=project_root, paths=paths,
        label=f"{component_id} URDF",
    )
    urdf, urdf_identity = _read_plain_file(
        urdf_path, maximum=MAX_XML_BYTES, label=f"{component_id} URDF"
    )
    srdf_reference = source.get("srdf")
    srdf_path = (
        None
        if srdf_reference is None
        else _reference_path(
            srdf_reference, project_root=project_root, paths=paths,
            label=f"{component_id} SRDF",
        )
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
        name: _reference_path(
            reference, project_root=project_root, paths=paths,
            label=f"{component_id} package root {name!r}", kind="directory",
        )
        for name, reference in package_roots.items()
        if isinstance(name, str) and isinstance(reference, Mapping)
    }
    if len(resolved_roots) != len(package_roots):
        raise ModelSourceCaptureError(
            f"{component_id} package_roots has an invalid row"
        )
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
    return urdf, srdf, entrypoints, resolved_roots


def load_tool_parameters(
    component_id: str,
    parameters: Any,
    *,
    project_root: Path,
    paths: ProjectPathRule,
) -> tuple[parametric_tool.ToolParameters, Path]:
    """A row's ``model_source.parameters`` validated, and the folder its mesh
    paths are relative to: inline parameters (the project folder) or a
    parameter file read once (its folder). Raises ``ParametricToolError``
    for the parameters and ``ModelSourceCaptureError`` for the file."""

    if parametric_tool.is_parameter_document(parameters):
        return parametric_tool.load_parameter_document(
            parameters, source=f"{component_id} model_source.parameters"
        ), project_root
    label = f"{component_id} parameter file"
    if not isinstance(parameters, Mapping):
        raise ModelSourceCaptureError("model source reference has an invalid shape")
    path = _reference_path(parameters, project_root=project_root, paths=paths, label=label)
    raw, _identity = _read_plain_file(
        path, maximum=parametric_tool.MAX_PARAMETER_BYTES, label=label
    )
    return parametric_tool.load_parameters(raw, source=f"{label} {path}"), path.parent


def resolve_tool_meshes(
    component_id: str,
    parameters: parametric_tool.ToolParameters,
    *,
    mesh_folder: Path,
    paths: ProjectPathRule,
) -> dict[str, Path]:
    """Each mesh path of a tool's parameters resolved under the project path
    rule (OD-364): relative to ``mesh_folder`` (``..`` allowed) or absolute,
    inside an allowed folder. A refusal is ``parametric_tool_mesh_unavailable``
    and names the allowed folders."""

    resolved: dict[str, Path] = {}
    for relative in parametric_tool.mesh_paths(parameters):
        try:
            resolved[relative] = paths.resolve(
                relative, project_folder=mesh_folder, label=f"{component_id} tool mesh"
            )
        except ProjectPathError as error:
            raise ModelSourceCaptureError(
                str(error), code="parametric_tool_mesh_unavailable"
            ) from error
    return resolved


def read_tool_mesh(
    component_id: str, path: Path, *, paths: ProjectPathRule
) -> tuple[bytes, _FileIdentity]:
    """One resolved tool mesh read once (bounded, no symlinks)."""

    try:
        return _read_plain_file(
            path, maximum=MAX_ARTIFACT_BYTES, label=f"{component_id} tool mesh {path}"
        )
    except ModelSourceCaptureError as error:
        raise ModelSourceCaptureError(
            f"{error} (allowed folders: {paths.allowed_text()})",
            code="parametric_tool_mesh_unavailable",
        ) from error


_CapturedFile = tuple[str, Path, bytes, _FileIdentity]  # (logical URI, path, bytes, identity)


def _generated_model_source(
    component_id: str,
    source: Mapping[str, Any],
    *,
    project_root: Path,
    paths: ProjectPathRule,
) -> tuple[bytes, bytes, dict[str, Any], list[_CapturedFile], Mapping[str, Any] | None]:
    """A tool's parameters (OD-352): inline in the row, or a parameter file
    read once; the model generated from them. Its meshes stay where they are
    and are read once; the model names each by its content
    (``parametric_tool.captured_mesh_uri``, OD-364). The last value is what
    the tool declares beside its model (its collision bodies, tool ends,
    TCPs, gripper targets and spring sliders)."""

    if set(source) != {"parameters"} or not isinstance(source["parameters"], Mapping):
        raise ModelSourceCaptureError(
            f"{component_id} model_source.parameters excludes urdf, srdf and "
            "package_roots",
            code="parametric_tool_invalid",
        )
    try:
        parameters, mesh_folder = load_tool_parameters(
            component_id, source["parameters"], project_root=project_root, paths=paths
        )
    except parametric_tool.ParametricToolError as error:
        raise ModelSourceCaptureError(str(error), code=error.code) from error
    mesh_files: dict[str, str] = {}
    captured: dict[str, _CapturedFile] = {}
    total = 0
    for relative, path in resolve_tool_meshes(
        component_id, parameters, mesh_folder=mesh_folder, paths=paths
    ).items():
        value, identity = read_tool_mesh(component_id, path, paths=paths)
        total += len(value)
        if total > MAX_CLOSURE_BYTES:
            raise ModelSourceCaptureError("model source closure exceeds its bound")
        uri = parametric_tool.captured_mesh_uri(relative, value)
        mesh_files[relative] = uri
        captured.setdefault(uri, (uri, path, value, identity))
    tool = parametric_tool.generate(parameters, mesh_files)
    entrypoints = {
        "urdf": encode_model_source_artifact(tool.urdf, media_type="application/xml"),
        "srdf": encode_model_source_artifact(tool.srdf, media_type="application/xml"),
    }
    return (tool.urdf, tool.srdf, entrypoints, [captured[uri] for uri in sorted(captured)],
            tool.component)


def _package_file(
    component_id: str, uri: str, roots: Mapping[str, Path]
) -> _CapturedFile:
    """One ``package://`` resource of an authored URDF, read from its root."""

    package_name, separator, relative = uri.removeprefix("package://").partition("/")
    root = roots.get(package_name)
    if not separator or root is None:
        raise ModelSourceCaptureError(f"{component_id} has no root for {uri!r}")
    value, identity = _read_package_resource(root, relative)
    return (uri, Path(os.path.join(os.path.abspath(root), *_safe_resource_path(relative))),
            value, identity)


def capture_deployment_model_sources(
    deployment: Mapping[str, Any], *, project_root: Path, paths: ProjectPathRule
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
        model_free = isinstance(configuration, Mapping) and configuration.get(
            "runtime_kind"
        ) in ("io_provider", "pose_source", "ai_model", "assistant")
        # A machine's owner (no runtime kind) or member without a model source.
        machine_row = "owner" in raw_component or (
            isinstance(configuration, Mapping) and "runtime_kind" not in configuration
        )
        if source is None and "model_source" not in raw_component and (
            model_free or machine_row
        ):
            # A model-free I/O device, pose source, AI model or assistant, a
            # machine's owner or its I/O member has no model source closure.
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
        files: Iterable[_CapturedFile]  # read one by one, inside the bounds
        if "parameters" in source:
            # OD-352: a parameter file in place of urdf/srdf/package_roots;
            # the model is generated here, so preparation sees an ordinary
            # URDF/SRDF bundle.
            (urdf, srdf, entrypoints, files,
             generated_component) = _generated_model_source(
                component_id, source, project_root=project_root, paths=paths
            )
        else:
            generated_component = None
            urdf, srdf, entrypoints, resolved_roots = _authored_model_source(
                component_id, source, project_root=project_root, paths=paths
            )
            files = (
                _package_file(component_id, uri, resolved_roots)
                for uri in _resource_uris(urdf)
            )
        resources: list[dict[str, Any]] = []
        for uri, path, value, identity in files:
            total_bytes += len(value)
            total_resources += 1
            if total_resources > MAX_RESOURCES or total_bytes > MAX_CLOSURE_BYTES:
                raise ModelSourceCaptureError("model source closure exceeds its bound")
            resources.append(
                {
                    "logical_uri": uri,
                    **_ENCODED_SOURCES.encode(
                        value,
                        path=str(path),
                        identity=identity,
                        media_type=_resource_media_type(uri),
                    ),
                }
            )
        total_bytes += len(urdf) + (0 if srdf is None else len(srdf))
        if total_bytes > MAX_CLOSURE_BYTES:
            raise ModelSourceCaptureError("model source closure exceeds its bound")
        captured.append(
            {
                "component_id": component_id,
                "entrypoints": entrypoints,
                "resources": resources,
                # A Tool from its parameters: what its component declares;
                # activation derives the component type from it.
                **(
                    {}
                    if generated_component is None
                    else {"generated_component": generated_component}
                ),
            }
        )
    captured.sort(key=lambda row: row["component_id"])
    return {
        "schema_version": SOURCE_CLOSURE_SCHEMA,
        "components": captured,
    }


SCENE_MESH_CLOSURE_SCHEMA: Final = "manyforge.activation_scene_mesh_closure.v1"
_URI_SCHEME_TEXT: Final = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*:")
SCENE_MESH_MEDIA_TYPE: Final = "application/octet-stream"
# collision kMaximumSceneMeshBytes
MAX_SCENE_MESH_BYTES: Final = MAX_ARTIFACT_BYTES
MAX_SCENE_MESHES: Final = MAX_RESOURCES


class SceneMeshPathError(ValueError):
    """A scene mesh cannot be resolved or read under the project path rule.

    ``code`` is ``scene_mesh_unavailable`` or ``scene_mesh_bounds_exceeded``.
    """

    def __init__(self, code: str, message: str, *, uri: str | None) -> None:
        super().__init__(message)
        self.code = code
        self.uri = uri


def resolve_scene_mesh_path(
    uri: str,
    *,
    project_folder: str | os.PathLike[str] | None,
    paths: ProjectPathRule,
) -> Path:
    """Resolve one Program mesh URI to its normalized absolute file path."""

    try:
        return paths.resolve(uri, project_folder=project_folder, label="scene mesh")
    except ProjectPathError as error:
        raise SceneMeshPathError(
            "scene_mesh_unavailable", str(error),
            uri=uri if isinstance(uri, str) else None,
        ) from error


def _read_scene_mesh(
    uri: str,
    *,
    project_folder: str | os.PathLike[str] | None,
    paths: ProjectPathRule,
) -> tuple[Path, bytes, _FileIdentity]:
    path = resolve_scene_mesh_path(uri, project_folder=project_folder, paths=paths)
    try:
        size = os.lstat(path).st_size
    except OSError:
        size = 0
    if size > MAX_SCENE_MESH_BYTES:
        raise SceneMeshPathError(
            "scene_mesh_bounds_exceeded",
            f"scene mesh {uri!r} ({path}) exceeds the {MAX_SCENE_MESH_BYTES}-byte bound",
            uri=uri,
        )
    try:
        value, identity = _read_plain_file(
            path, maximum=MAX_SCENE_MESH_BYTES, label=f"scene mesh {uri!r} ({path})"
        )
    except ModelSourceCaptureError as error:
        raise SceneMeshPathError(
            "scene_mesh_unavailable",
            f"{error} (allowed folders: {paths.allowed_text()})",
            uri=uri,
        ) from error
    return path, value, identity


def read_scene_mesh(
    uri: str,
    *,
    project_folder: str | os.PathLike[str] | None,
    paths: ProjectPathRule,
) -> tuple[Path, bytes]:
    """Resolve one mesh URI and read its bytes once (bounded, no symlinks)."""

    path, value, _identity = _read_scene_mesh(
        uri, project_folder=project_folder, paths=paths
    )
    return path, value


def _mesh_shapes(value: Any) -> Iterator[dict[str, Any]]:
    if type(value) is dict:
        if value.get("type") == "mesh" and isinstance(value.get("mesh_resource_uri"), str):
            yield value
        for child in value.values():
            yield from _mesh_shapes(child)
    elif type(value) is list:
        for child in value:
            yield from _mesh_shapes(child)


def program_scene_mesh_shapes(program: Any) -> Iterator[dict[str, Any]]:
    """Every ``{type: mesh, mesh_resource_uri}`` object anywhere in a Program.

    This covers the initial ``scene`` and ``scene.add_object`` /
    ``scene.upsert_object`` shape parameters alike.
    """

    yield from _mesh_shapes(program)


def program_scene_mesh_uris(program: Any) -> list[str]:
    """The Program's mesh URIs, sorted and unique (none for no Program)."""

    if program is None:
        return []
    return sorted({shape["mesh_resource_uri"] for shape in program_scene_mesh_shapes(program)})


def rebase_program_scene_mesh_paths(
    program: Mapping[str, Any],
    *,
    source_project_folder: str | os.PathLike[str],
    destination_project_folder: str | os.PathLike[str],
) -> dict[str, Any]:
    """The Program (or its authoring forest) with every relative scene mesh
    path rewritten so it names the same file from another project folder, as
    Save as rebases model sources. Absolute paths stay; a path with a URI
    scheme is left for the path rule to refuse."""

    result = deepcopy(dict(program))
    source = os.fspath(source_project_folder)
    destination = os.fspath(destination_project_folder)
    for shape in program_scene_mesh_shapes(result):
        uri = shape["mesh_resource_uri"]
        if not uri or os.path.isabs(uri) or _URI_SCHEME_TEXT.match(uri):
            continue
        target = os.path.normpath(os.path.join(source, uri))
        shape["mesh_resource_uri"] = PurePosixPath(
            os.path.relpath(target, destination)
        ).as_posix()
    return result


def capture_program_scene_meshes(
    program: Mapping[str, Any] | None,
    *,
    project_folder: str | os.PathLike[str] | None,
    paths: ProjectPathRule,
) -> dict[str, Any]:
    """Capture every Program scene mesh once (activation scene-mesh closure v1)."""

    uris = program_scene_mesh_uris(program)
    if len(uris) > MAX_SCENE_MESHES:
        raise SceneMeshPathError(
            "scene_mesh_bounds_exceeded",
            f"the Program names {len(uris)} scene meshes; at most "
            f"{MAX_SCENE_MESHES} are captured",
            uri=None,
        )
    rows: list[dict[str, Any]] = []
    total = 0
    for uri in uris:
        path, value, identity = _read_scene_mesh(
            uri, project_folder=project_folder, paths=paths
        )
        total += len(value)
        if total > MAX_CLOSURE_BYTES:
            raise SceneMeshPathError(
                "scene_mesh_bounds_exceeded",
                f"the Program's scene meshes exceed the {MAX_CLOSURE_BYTES}-byte bound",
                uri=uri,
            )
        rows.append({
            "resource_uri": uri,
            **_ENCODED_SOURCES.encode(
                value, path=str(path), identity=identity,
                media_type=SCENE_MESH_MEDIA_TYPE,
            ),
        })
    return {"schema_version": SCENE_MESH_CLOSURE_SCHEMA, "meshes": rows}
