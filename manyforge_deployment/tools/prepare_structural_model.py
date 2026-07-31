#!/usr/bin/env python3
"""Prepare one expanded URDF/SRDF pair for the structural compiler boundary.

The pure XML-preparation step is deliberately resolver-free.  It removes only
qualified top-level runtime descriptions and rewrites exact package resource
references into closure-local ``asset://`` names.  The bounded qualification
step then reads only those discovered safe relative paths under one explicit
plain resource root and emits deterministic XML plus an exact preparation
receipt.  Neither step searches ROS package indexes, the host, or the network.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
import xml.etree.ElementTree as ET
import xml.parsers.expat
from pathlib import Path
from typing import Sequence


MAX_XML_ENTRYPOINT_BYTES = 16 * 1024 * 1024
MAX_XML_DEPTH = 128
MAX_XML_ELEMENTS = 250_000
MAX_XML_ATTRIBUTES = 1_000_000
MAX_LOGICAL_URI_BYTES = 2_048
MAX_SAFE_PATH_SEGMENT_BYTES = 128
MAX_SOURCE_ARTIFACTS = 4_096
MAX_OPAQUE_ARTIFACT_BYTES = 256 * 1024 * 1024
MAX_SOURCE_CLOSURE_BYTES = 512 * 1024 * 1024

PREPARATION_API_VERSION = "manyforge.structural_model_preparation.v1"
GENERATOR_ID = "manyforge.structural_model_preparation"
GENERATOR_VERSION = "1.0.0"
CONFIGURATION_IDENTITY_DOMAIN = (
    "structural-model-preparation-receipt"
)
CONTENT_IDENTITY_PREFIX = "manyforge.content-identity.v1"

_PORTABLE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_URI_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*://")
_RUNTIME_ONLY_TOP_LEVEL = frozenset(
    {"gazebo", "ros2_control", "transmission"}
)
_URDF_STRUCTURAL_TOP_LEVEL = frozenset({"joint", "link", "material"})
_SRDF_STRUCTURAL_TOP_LEVEL = frozenset(
    {
        "disable_collisions",
        "end_effector",
        "group",
        "group_state",
        "passive_joint",
    }
)
_RESOURCE_TAGS = frozenset({"mesh", "texture"})
_RESERVED_OUTPUT_PATHS = frozenset(
    {"model.srdf", "model.urdf", "preparation-manifest.json"}
)


class StructuralModelPreparationError(ValueError):
    """The candidate cannot enter the structural-model source boundary."""


@dataclasses.dataclass(frozen=True)
class PreparedResourceReference:
    """One unique resource referenced by the prepared structural URDF."""

    asset_uri: str
    kind: str
    occurrence_count: int
    original_package_uri: str
    relative_path: str


@dataclasses.dataclass(frozen=True)
class PreparedStructuralModel:
    """Deterministic structural XML before any resource lookup."""

    model_asset_id: str
    accepted_package_uri_prefix: str
    urdf_bytes: bytes
    srdf_bytes: bytes | None
    resources: tuple[PreparedResourceReference, ...]
    removed_top_level_elements: tuple[tuple[str, int], ...]


@dataclasses.dataclass(frozen=True)
class PreparedSourceArtifact:
    """One exact artifact in the prepared closure."""

    logical_uri: str
    media_type: str
    byte_size: int
    content_digest: str


@dataclasses.dataclass(frozen=True)
class QualifiedStructuralModelPreparation:
    """Exact deterministic receipt plus the resolved resource bytes.

    This Python value remains build evidence, not provenance authority.  The
    C++ qualifier independently closes the receipt against a
    SourceVerifiedModelAsset before it can mint a GeneratorRecord.
    """

    prepared: PreparedStructuralModel
    manifest: dict[str, object]
    resource_bytes: tuple[tuple[str, bytes], ...]

    def manifest_bytes(self) -> bytes:
        return json.dumps(
            self.manifest,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")


@dataclasses.dataclass
class _XmlBudget:
    elements: int = 0
    attributes: int = 0


@dataclasses.dataclass
class _ParsedXml:
    root: ET.Element
    text_by_element: dict[int, str]


class _StrictXmlBuilder:
    def __init__(self, *, kind: str, budget: _XmlBudget) -> None:
        self._kind = kind
        self._budget = budget
        self._root: ET.Element | None = None
        self._stack: list[ET.Element] = []
        self._text_chunks: dict[int, list[str]] = {}

    def start(self, name: str, raw_attributes: list[str]) -> None:
        if len(self._stack) >= MAX_XML_DEPTH:
            self.fail("exceeds the aggregate XML nesting bound")
        self._budget.elements += 1
        if self._budget.elements > MAX_XML_ELEMENTS:
            self.fail("exceeds the aggregate XML element bound")
        _require_plain_xml_name(name, self._kind)

        if len(raw_attributes) % 2:
            self.fail("reported an invalid attribute vector")
        pairs: list[tuple[str, str]] = []
        seen: set[str] = set()
        for index in range(0, len(raw_attributes), 2):
            attribute_name = raw_attributes[index]
            attribute_value = raw_attributes[index + 1]
            _require_plain_xml_name(attribute_name, self._kind)
            if attribute_name in seen:
                self.fail(
                    f"contains duplicate attribute {attribute_name!r}"
                )
            seen.add(attribute_name)
            self._budget.attributes += 1
            if self._budget.attributes > MAX_XML_ATTRIBUTES:
                self.fail("exceeds the aggregate XML attribute bound")
            pairs.append((attribute_name, attribute_value))

        element = ET.Element(name, dict(pairs))
        self._text_chunks[id(element)] = []
        if self._stack:
            self._stack[-1].append(element)
        elif self._root is None:
            self._root = element
        else:
            self.fail("contains more than one root element")
        self._stack.append(element)

    def end(self, name: str) -> None:
        if not self._stack or self._stack[-1].tag != name:
            self.fail("contains an unbalanced element")
        self._stack.pop()

    def text(self, data: str) -> None:
        if self._stack:
            self._text_chunks[id(self._stack[-1])].append(data)

    def reject_doctype(self, *_args: object) -> None:
        self.fail("contains a forbidden document type")

    def reject_entity(self, *_args: object) -> None:
        self.fail("contains a forbidden entity declaration")

    def reject_external_entity(self, *_args: object) -> int:
        self.fail("contains a forbidden external entity")
        return 0

    def reject_processing_instruction(self, *_args: object) -> None:
        self.fail("contains a forbidden processing instruction")

    def fail(self, reason: str) -> None:
        raise StructuralModelPreparationError(
            f"{self._kind} XML {reason}"
        )

    def finish(self) -> _ParsedXml:
        if self._root is None or self._stack:
            self.fail("does not contain one complete root element")
        return _ParsedXml(
            self._root,
            {
                element_id: "".join(chunks)
                for element_id, chunks in self._text_chunks.items()
            },
        )


def _sha256(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _canonical_json_bytes(value: object) -> bytes:
    """Serialize this receipt's ASCII/I-JSON subset canonically."""

    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _configuration_digest(manifest_without_digest: dict[str, object]) -> str:
    payload = dict(manifest_without_digest)
    payload.pop("configuration_digest", None)
    preimage = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX.encode("ascii"),
            CONFIGURATION_IDENTITY_DOMAIN.encode("ascii"),
            PREPARATION_API_VERSION.encode("ascii"),
            _canonical_json_bytes(payload),
        )
    )
    return _sha256(preimage)


def _tool_build_digest() -> str:
    """Return the exact raw-byte identity independently embedded by CMake."""

    return _sha256(Path(__file__).read_bytes())


def _source_identity(value: bytes) -> dict[str, object]:
    return {
        "byte_size": len(value),
        "content_digest": _sha256(value),
    }


def _resource_media_type(reference: PreparedResourceReference) -> str:
    suffix = Path(reference.relative_path).suffix.casefold()
    if reference.kind == "mesh":
        media_type = {
            ".dae": "model/vnd.collada+xml",
            ".stl": "model/stl",
        }.get(suffix)
    else:
        media_type = {
            ".jpeg": "image/jpeg",
            ".jpg": "image/jpeg",
            ".png": "image/png",
        }.get(suffix)
    if media_type is None:
        raise StructuralModelPreparationError(
            "resource extension has no qualified structural media type: "
            f"{reference.relative_path!r}"
        )
    return media_type


def _read_bounded_plain_resource(
    root: Path,
    relative_path: str,
) -> bytes:
    """Open one closure member without symlink or traversal resolution."""

    _require_safe_relative_path(relative_path, field="resource path")
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    directory_flags = flags | os.O_DIRECTORY
    descriptors: list[int] = []
    try:
        root_descriptor = os.open(root, directory_flags)
        descriptors.append(root_descriptor)
        parent_descriptor = root_descriptor
        segments = relative_path.split("/")
        for segment in segments[:-1]:
            child_descriptor = os.open(
                segment,
                directory_flags,
                dir_fd=parent_descriptor,
            )
            descriptors.append(child_descriptor)
            parent_descriptor = child_descriptor
        descriptor = os.open(
            segments[-1],
            flags,
            dir_fd=parent_descriptor,
        )
        descriptors.append(descriptor)
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise StructuralModelPreparationError(
                f"resource is not a plain file: {relative_path!r}"
            )
        if (
            metadata.st_size <= 0
            or metadata.st_size > MAX_OPAQUE_ARTIFACT_BYTES
        ):
            raise StructuralModelPreparationError(
                "resource size is outside the qualified source bound: "
                f"{relative_path!r}"
            )
        chunks: list[bytes] = []
        remaining = metadata.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise StructuralModelPreparationError(
                    f"resource changed while reading: {relative_path!r}"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise StructuralModelPreparationError(
                f"resource changed while reading: {relative_path!r}"
            )
        post_read = os.fstat(descriptor)
        if (
            post_read.st_dev != metadata.st_dev
            or post_read.st_ino != metadata.st_ino
            or post_read.st_size != metadata.st_size
            or post_read.st_mtime_ns != metadata.st_mtime_ns
        ):
            raise StructuralModelPreparationError(
                f"resource changed while reading: {relative_path!r}"
            )
        return b"".join(chunks)
    except OSError as error:
        raise StructuralModelPreparationError(
            f"could not read plain resource {relative_path!r}: {error}"
        ) from error
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def _require_plain_xml_name(name: str, kind: str) -> None:
    if not name or ":" in name or name == "xmlns" or name.startswith(
        "xmlns:"
    ):
        raise StructuralModelPreparationError(
            f"{kind} XML namespaces are not supported"
        )


def _parse_strict_xml(
    value: bytes, *, kind: str, budget: _XmlBudget
) -> _ParsedXml:
    if not isinstance(value, bytes):
        raise TypeError(f"{kind} input must be bytes")
    if len(value) > MAX_XML_ENTRYPOINT_BYTES:
        raise StructuralModelPreparationError(
            f"{kind} XML exceeds the {MAX_XML_ENTRYPOINT_BYTES}-byte bound"
        )

    builder = _StrictXmlBuilder(kind=kind, budget=budget)
    parser = xml.parsers.expat.ParserCreate()
    parser.ordered_attributes = True
    parser.specified_attributes = True
    parser.StartElementHandler = builder.start
    parser.EndElementHandler = builder.end
    parser.CharacterDataHandler = builder.text
    parser.StartDoctypeDeclHandler = builder.reject_doctype
    parser.EntityDeclHandler = builder.reject_entity
    parser.ExternalEntityRefHandler = builder.reject_external_entity
    parser.ProcessingInstructionHandler = (
        builder.reject_processing_instruction
    )
    parser.CommentHandler = lambda _comment: None
    try:
        parser.Parse(value, True)
    except StructuralModelPreparationError:
        raise
    except xml.parsers.expat.ExpatError as error:
        raise StructuralModelPreparationError(
            f"{kind} XML is malformed at line {error.lineno}, "
            f"column {error.offset}: {error}"
        ) from error
    return builder.finish()


def _require_model_asset_id(value: str) -> None:
    if not _PORTABLE_ID.fullmatch(value):
        raise StructuralModelPreparationError(
            "model_asset_id must use the 1..128-byte portable ID grammar"
        )


def _require_safe_segment(segment: str, *, field: str) -> None:
    encoded = segment.encode("ascii", errors="strict")
    if (
        not encoded
        or len(encoded) > MAX_SAFE_PATH_SEGMENT_BYTES
        or not _PORTABLE_ID.fullmatch(segment)
    ):
        raise StructuralModelPreparationError(
            f"{field} contains unsafe path segment {segment!r}"
        )


def _require_safe_relative_path(value: str, *, field: str) -> None:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise StructuralModelPreparationError(
            f"{field} must be ASCII"
        ) from error
    if (
        not value
        or len(encoded) > MAX_LOGICAL_URI_BYTES
        or value.startswith("/")
        or value.endswith("/")
        or "\\" in value
    ):
        raise StructuralModelPreparationError(
            f"{field} must be one bounded safe relative path"
        )
    for segment in value.split("/"):
        _require_safe_segment(segment, field=field)


def _validate_package_prefix(prefix: str) -> None:
    try:
        encoded = prefix.encode("ascii")
    except UnicodeEncodeError as error:
        raise StructuralModelPreparationError(
            "accepted package URI prefix must be ASCII"
        ) from error
    if (
        len(encoded) > MAX_LOGICAL_URI_BYTES
        or not prefix.startswith("package://")
        or not prefix.endswith("/")
    ):
        raise StructuralModelPreparationError(
            "accepted package URI prefix must be one bounded exact "
            "package://.../ prefix with a trailing slash"
        )
    remainder = prefix.removeprefix("package://")[:-1]
    if not remainder or prefix.count("://") != 1:
        raise StructuralModelPreparationError(
            "accepted package URI prefix is ambiguous"
        )
    _require_safe_relative_path(
        remainder, field="accepted package URI prefix"
    )


def _reject_unresolved_templates(parsed: _ParsedXml, *, kind: str) -> None:
    for element in parsed.root.iter():
        if "xacro" in str(element.tag).casefold():
            raise StructuralModelPreparationError(
                f"{kind} contains an unresolved xacro element"
            )
        for name, value in element.attrib.items():
            if (
                "xacro" in name.casefold()
                or "${" in value
                or "$(" in value
            ):
                raise StructuralModelPreparationError(
                    f"{kind} contains an unresolved template expression"
                )
        text = parsed.text_by_element.get(id(element), "")
        if "${" in text or "$(" in text:
            raise StructuralModelPreparationError(
                f"{kind} contains an unresolved template expression"
            )


def _remove_runtime_only_top_level(
    parsed: _ParsedXml,
) -> tuple[tuple[str, int], ...]:
    root = parsed.root
    counts = {name: 0 for name in sorted(_RUNTIME_ONLY_TOP_LEVEL)}
    removable: list[ET.Element] = []
    for parent in root.iter():
        for child in list(parent):
            tag = str(child.tag)
            if tag not in _RUNTIME_ONLY_TOP_LEVEL:
                continue
            if parent is not root:
                raise StructuralModelPreparationError(
                    f"URDF runtime-only <{tag}> must be a direct <robot> "
                    "child"
                )
            removable.append(child)
            counts[tag] += 1
    for child in removable:
        root.remove(child)
    return tuple(counts.items())


def _require_structural_document(
    parsed: _ParsedXml, *, kind: str
) -> None:
    root = parsed.root
    if root.tag != "robot":
        raise StructuralModelPreparationError(
            f"{kind} root element must be <robot>"
        )
    if not root.attrib.get("name"):
        raise StructuralModelPreparationError(
            f"{kind} <robot> requires a non-empty name"
        )
    allowed = (
        _URDF_STRUCTURAL_TOP_LEVEL
        if kind == "URDF"
        else _SRDF_STRUCTURAL_TOP_LEVEL
    )
    for child in root:
        tag = str(child.tag)
        if tag == "virtual_joint" and kind == "SRDF":
            raise StructuralModelPreparationError(
                "SRDF <virtual_joint> is forbidden because cell placement "
                "owns model roots"
            )
        if tag not in allowed:
            raise StructuralModelPreparationError(
                f"{kind} contains unsupported top-level <{tag}>"
            )
    for element in root.iter():
        if parsed.text_by_element.get(id(element), "").strip():
            raise StructuralModelPreparationError(
                f"{kind} structural element <{element.tag}> contains "
                "non-whitespace character data"
            )


def _looks_like_uri(value: str) -> bool:
    return bool(_URI_SCHEME.match(value))


def _rewrite_resource_uris(
    parsed: _ParsedXml,
    *,
    model_asset_id: str,
    package_prefix: str,
) -> tuple[PreparedResourceReference, ...]:
    occurrences: dict[
        str, tuple[str, str, str, int]
    ] = {}
    casefold_paths = {
        path.casefold(): path for path in _RESERVED_OUTPUT_PATHS
    }

    for element in parsed.root.iter():
        tag = str(element.tag)
        for attribute_name, attribute_value in element.attrib.items():
            is_resource_filename = (
                tag in _RESOURCE_TAGS and attribute_name == "filename"
            )
            if not is_resource_filename and _looks_like_uri(attribute_value):
                raise StructuralModelPreparationError(
                    f"URDF URI {attribute_value!r} appears outside a "
                    "<mesh>/<texture> filename"
                )
        if tag not in _RESOURCE_TAGS:
            continue
        original = element.attrib.get("filename")
        if original is None:
            raise StructuralModelPreparationError(
                f"URDF <{tag}> requires a filename"
            )
        if not original.startswith(package_prefix):
            raise StructuralModelPreparationError(
                f"URDF <{tag}> filename must use exactly the accepted "
                f"package prefix {package_prefix!r}: {original!r}"
            )
        relative_path = original[len(package_prefix) :]
        _require_safe_relative_path(
            relative_path, field=f"URDF <{tag}> resource path"
        )
        asset_uri = f"asset://{model_asset_id}/{relative_path}"
        if len(asset_uri.encode("ascii")) > MAX_LOGICAL_URI_BYTES:
            raise StructuralModelPreparationError(
                "rewritten asset URI exceeds the logical-URI bound"
            )

        folded = relative_path.casefold()
        previous_spelling = casefold_paths.get(folded)
        if previous_spelling in _RESERVED_OUTPUT_PATHS:
            raise StructuralModelPreparationError(
                f"resource path {relative_path!r} collides with a "
                "reserved preparation output"
            )
        if (
            previous_spelling is not None
            and previous_spelling != relative_path
        ):
            raise StructuralModelPreparationError(
                "resource paths collide under portable case folding: "
                f"{previous_spelling!r} and {relative_path!r}"
            )
        casefold_paths[folded] = relative_path

        kind = "mesh" if tag == "mesh" else "texture"
        previous = occurrences.get(asset_uri)
        if previous is None:
            occurrences[asset_uri] = (
                kind,
                original,
                relative_path,
                1,
            )
        else:
            previous_kind, previous_original, previous_path, count = previous
            if (
                previous_kind != kind
                or previous_original != original
                or previous_path != relative_path
            ):
                raise StructuralModelPreparationError(
                    f"ambiguous duplicate resource path {relative_path!r}"
                )
            occurrences[asset_uri] = (
                previous_kind,
                previous_original,
                previous_path,
                count + 1,
            )
        element.set("filename", asset_uri)

    return tuple(
        PreparedResourceReference(
            asset_uri=asset_uri,
            kind=kind,
            occurrence_count=count,
            original_package_uri=original,
            relative_path=relative_path,
        )
        for asset_uri, (kind, original, relative_path, count) in sorted(
            occurrences.items()
        )
    )


def _reject_non_resource_uris(
    parsed: _ParsedXml, *, kind: str
) -> None:
    for element in parsed.root.iter():
        for attribute_name, attribute_value in element.attrib.items():
            if _looks_like_uri(attribute_value):
                raise StructuralModelPreparationError(
                    f"{kind} URI {attribute_value!r} is not a qualified "
                    "structural resource reference"
                )


def _escape_attribute(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace('"', "&quot;")
        .replace("\t", "&#x9;")
        .replace("\n", "&#xA;")
        .replace("\r", "&#xD;")
    )


def _serialize_element(
    element: ET.Element, *, depth: int, output: list[str]
) -> None:
    indent = "  " * depth
    attributes = "".join(
        f' {name}="{_escape_attribute(value)}"'
        for name, value in sorted(element.attrib.items())
    )
    children = list(element)
    if not children:
        output.append(f"{indent}<{element.tag}{attributes}/>\n")
        return
    output.append(f"{indent}<{element.tag}{attributes}>\n")
    for child in children:
        _serialize_element(child, depth=depth + 1, output=output)
    output.append(f"{indent}</{element.tag}>\n")


def _serialize_document(root: ET.Element) -> bytes:
    output = ['<?xml version="1.0" encoding="UTF-8"?>\n']
    _serialize_element(root, depth=0, output=output)
    encoded = "".join(output).encode("utf-8")
    if len(encoded) > MAX_XML_ENTRYPOINT_BYTES:
        raise StructuralModelPreparationError(
            "prepared XML exceeds the compiler-owned byte bound"
        )
    return encoded


def prepare_structural_model(
    urdf_bytes: bytes,
    srdf_bytes: bytes | None,
    *,
    model_asset_id: str,
    accepted_package_uri_prefix: str,
) -> PreparedStructuralModel:
    """Prepare structural XML without resolving any referenced resource."""

    _require_model_asset_id(model_asset_id)
    _validate_package_prefix(accepted_package_uri_prefix)

    budget = _XmlBudget()
    urdf = _parse_strict_xml(urdf_bytes, kind="URDF", budget=budget)
    srdf = (
        None
        if srdf_bytes is None
        else _parse_strict_xml(srdf_bytes, kind="SRDF", budget=budget)
    )
    _reject_unresolved_templates(urdf, kind="URDF")
    if srdf is not None:
        _reject_unresolved_templates(srdf, kind="SRDF")

    removed = _remove_runtime_only_top_level(urdf)
    _require_structural_document(urdf, kind="URDF")
    if srdf is not None:
        _require_structural_document(srdf, kind="SRDF")
        if srdf.root.attrib["name"] != urdf.root.attrib["name"]:
            raise StructuralModelPreparationError(
                "URDF and SRDF robot names must match"
            )

    resources = _rewrite_resource_uris(
        urdf,
        model_asset_id=model_asset_id,
        package_prefix=accepted_package_uri_prefix,
    )
    if srdf is not None:
        _reject_non_resource_uris(srdf, kind="SRDF")
    return PreparedStructuralModel(
        model_asset_id=model_asset_id,
        accepted_package_uri_prefix=accepted_package_uri_prefix,
        urdf_bytes=_serialize_document(urdf.root),
        srdf_bytes=(
            None if srdf is None else _serialize_document(srdf.root)
        ),
        resources=resources,
        removed_top_level_elements=removed,
    )


def qualify_structural_model_preparation(
    prepared: PreparedStructuralModel,
    *,
    original_urdf_bytes: bytes,
    original_srdf_bytes: bytes | None,
    resource_root: Path | None,
) -> QualifiedStructuralModelPreparation:
    """Resolve the exact bounded closure and form its canonical receipt.

    The returned receipt is intentionally not a trusted GeneratorRecord.  It
    is designed for the build-owned C++ qualifier, which independently joins
    every artifact row to a SourceVerifiedModelAsset retrieval plan.
    """

    if not isinstance(original_urdf_bytes, bytes):
        raise TypeError("original URDF input must be bytes")
    if original_srdf_bytes is not None and not isinstance(
        original_srdf_bytes, bytes
    ):
        raise TypeError("original SRDF input must be bytes or None")
    if (prepared.srdf_bytes is None) != (original_srdf_bytes is None):
        raise StructuralModelPreparationError(
            "prepared and original SRDF presence must agree"
        )
    if prepared.resources and resource_root is None:
        raise StructuralModelPreparationError(
            "resource_root is required for a referenced structural closure"
        )
    if resource_root is not None:
        try:
            root_metadata = resource_root.lstat()
        except OSError as error:
            raise StructuralModelPreparationError(
                f"could not inspect resource root: {error}"
            ) from error
        if (
            not stat.S_ISDIR(root_metadata.st_mode)
            or stat.S_ISLNK(root_metadata.st_mode)
        ):
            raise StructuralModelPreparationError(
                "resource_root must be one plain directory"
            )

    artifacts: list[PreparedSourceArtifact] = [
        PreparedSourceArtifact(
            logical_uri=(
                f"asset://{prepared.model_asset_id}/model.urdf"
            ),
            media_type="application/xml",
            byte_size=len(prepared.urdf_bytes),
            content_digest=_sha256(prepared.urdf_bytes),
        )
    ]
    if prepared.srdf_bytes is not None:
        artifacts.append(
            PreparedSourceArtifact(
                logical_uri=(
                    f"asset://{prepared.model_asset_id}/model.srdf"
                ),
                media_type="application/xml",
                byte_size=len(prepared.srdf_bytes),
                content_digest=_sha256(prepared.srdf_bytes),
            )
        )

    resource_bytes: list[tuple[str, bytes]] = []
    resource_rewrites: list[dict[str, object]] = []
    for reference in prepared.resources:
        assert resource_root is not None
        value = _read_bounded_plain_resource(
            resource_root, reference.relative_path
        )
        media_type = _resource_media_type(reference)
        identity = _source_identity(value)
        artifacts.append(
            PreparedSourceArtifact(
                logical_uri=reference.asset_uri,
                media_type=media_type,
                byte_size=int(identity["byte_size"]),
                content_digest=str(identity["content_digest"]),
            )
        )
        resource_rewrites.append(
            {
                "asset_uri": reference.asset_uri,
                "byte_size": identity["byte_size"],
                "content_digest": identity["content_digest"],
                "kind": reference.kind,
                "media_type": media_type,
                "occurrence_count": reference.occurrence_count,
                "original_package_uri": (
                    reference.original_package_uri
                ),
                "relative_path": reference.relative_path,
            }
        )
        resource_bytes.append((reference.asset_uri, value))

    artifacts.sort(key=lambda artifact: artifact.logical_uri)
    resource_bytes.sort(key=lambda item: item[0])
    resource_rewrites.sort(key=lambda row: str(row["asset_uri"]))
    if not 1 <= len(artifacts) <= MAX_SOURCE_ARTIFACTS:
        raise StructuralModelPreparationError(
            "prepared source artifact count exceeds the qualified bound"
        )
    closure_bytes = sum(artifact.byte_size for artifact in artifacts)
    if closure_bytes > MAX_SOURCE_CLOSURE_BYTES:
        raise StructuralModelPreparationError(
            "prepared source closure exceeds the qualified byte bound"
        )

    prepared_source: dict[str, object] = {
        "artifacts": [
            dataclasses.asdict(artifact) for artifact in artifacts
        ],
        "srdf_logical_uri": (
            None
            if prepared.srdf_bytes is None
            else f"asset://{prepared.model_asset_id}/model.srdf"
        ),
        "urdf_logical_uri": (
            f"asset://{prepared.model_asset_id}/model.urdf"
        ),
    }
    manifest: dict[str, object] = {
        "accepted_package_uri_prefix": (
            prepared.accepted_package_uri_prefix
        ),
        "api_version": PREPARATION_API_VERSION,
        "build_digest": _tool_build_digest(),
        "configuration_digest": "sha256:" + "0" * 64,
        "generator_id": GENERATOR_ID,
        "generator_version": GENERATOR_VERSION,
        "model_asset_id": prepared.model_asset_id,
        "original_source": {
            "srdf": (
                None
                if original_srdf_bytes is None
                else _source_identity(original_srdf_bytes)
            ),
            "urdf": _source_identity(original_urdf_bytes),
        },
        "prepared_source": prepared_source,
        "removed_top_level_elements": {
            name: count
            for name, count in prepared.removed_top_level_elements
        },
        "resource_rewrites": resource_rewrites,
    }
    manifest["configuration_digest"] = _configuration_digest(manifest)
    return QualifiedStructuralModelPreparation(
        prepared=prepared,
        manifest=manifest,
        resource_bytes=tuple(resource_bytes),
    )


def _atomic_write(path: Path, value: bytes) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp"
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare expanded URDF/SRDF as a bounded closure-local "
            "structural model"
        )
    )
    parser.add_argument("--urdf", required=True, type=Path)
    parser.add_argument("--srdf", type=Path)
    parser.add_argument("--model-asset-id", required=True)
    parser.add_argument("--accepted-package-uri-prefix", required=True)
    parser.add_argument(
        "--resource-root",
        type=Path,
        help=(
            "plain directory containing every package-relative mesh and "
            "texture referenced by the expanded URDF"
        ),
    )
    parser.add_argument("--output-directory", type=Path)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print the manifest without writing output",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _argument_parser()
    arguments = parser.parse_args(argv)
    if not arguments.dry_run and arguments.output_directory is None:
        parser.error("--output-directory is required without --dry-run")

    try:
        urdf_bytes = arguments.urdf.read_bytes()
        srdf_bytes = (
            None if arguments.srdf is None else arguments.srdf.read_bytes()
        )
        prepared = prepare_structural_model(
            urdf_bytes,
            srdf_bytes,
            model_asset_id=arguments.model_asset_id,
            accepted_package_uri_prefix=(
                arguments.accepted_package_uri_prefix
            ),
        )
        qualified = qualify_structural_model_preparation(
            prepared,
            original_urdf_bytes=urdf_bytes,
            original_srdf_bytes=srdf_bytes,
            resource_root=arguments.resource_root,
        )
        if arguments.dry_run:
            sys.stdout.buffer.write(qualified.manifest_bytes())
            return 0

        output_directory = arguments.output_directory
        output_directory.mkdir(parents=True, exist_ok=True)
        if not output_directory.is_dir() or output_directory.is_symlink():
            raise StructuralModelPreparationError(
                "output directory must be a plain directory"
            )
        outputs = {
            output_directory / "model.urdf": prepared.urdf_bytes,
            output_directory / "preparation-manifest.json": (
                qualified.manifest_bytes()
            ),
        }
        if prepared.srdf_bytes is not None:
            outputs[output_directory / "model.srdf"] = prepared.srdf_bytes

        input_paths = {arguments.urdf.resolve()}
        if arguments.srdf is not None:
            input_paths.add(arguments.srdf.resolve())
        if any(path.resolve() in input_paths for path in outputs):
            raise StructuralModelPreparationError(
                "output path must not overwrite an input document"
            )
        for path, value in sorted(outputs.items()):
            _atomic_write(path, value)
        return 0
    except (OSError, StructuralModelPreparationError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
