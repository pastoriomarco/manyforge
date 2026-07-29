"""Deterministic P4 deployment-compiler prototype.

This module deliberately reuses the current model-authority parser as a
read-only source adapter. It does not emit production artifacts or participate
in Apply/runtime execution. Prefixes, endpoint spellings, and frame names are
opaque prototype data, not a selected ROS naming contract. Physical attachment
transforms, TCPs, and collision semantics remain intentionally outside this
bounded prototype.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Mapping, Sequence
import xml.etree.ElementTree as ET

from manyforge_behavior.robot_model_authority import (
    RobotModelAuthorityError,
    load_robot_model_authority,
    robot_model_instance_view,
)

from .contracts import (
    BindingRevision,
    CapabilityId,
    CatalogRevision,
    CommandTargetId,
    CompiledBindingSnapshot,
    CompiledSymbolMap,
    CompilerRevision,
    ComponentTypeRecord,
    ControllableComponentId,
    ControlBindingId,
    ControlBindingSpec,
    ControlDomainId,
    ControlInterfaceContract,
    ControllerTopologyRecord,
    GraphRevision,
    ModelInstanceId,
    RetainedArtifact,
    RetainedBundleManifest,
    RouteId,
    Sha256Digest,
    SymbolBinding,
)


SCHEMA_ID = "manyforge.experimental.composable_cell.i1a.v0"
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_SYMBOL_KINDS = ("frames", "groups", "joints", "links")


class CompilerPrototypeError(ValueError):
    """Raised when a candidate retained bundle cannot be compiled exactly."""


def _required_name(raw: object, *, label: str) -> str:
    value = str(raw or "").strip()
    if not value:
        raise CompilerPrototypeError(f"{label} is required")
    return value


def _ordered_unique(values: Sequence[str]) -> tuple[str, ...]:
    return tuple(dict.fromkeys(values))


@dataclass(frozen=True, slots=True)
class SrdfGroupDefinition:
    name: str
    joints: tuple[str, ...] = ()
    links: tuple[str, ...] = ()
    chains: tuple[tuple[str, str], ...] = ()
    subgroups: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class ModelSource:
    asset_id: str
    model_name: str
    urdf_path: Path
    srdf_path: Path
    package_paths: tuple[str, ...]

    def __post_init__(self) -> None:
        _required_name(self.asset_id, label="asset_id")
        _required_name(self.model_name, label="model_name")
        if not self.urdf_path.is_file():
            raise CompilerPrototypeError(f"missing URDF: {self.urdf_path}")
        if not self.srdf_path.is_file():
            raise CompilerPrototypeError(f"missing SRDF: {self.srdf_path}")


@dataclass(frozen=True, slots=True)
class ModelInstanceInput:
    component_id: str
    model_instance_id: str
    asset_id: str
    prefix: str
    command_targets: tuple[tuple[str, str], ...]

    def __post_init__(self) -> None:
        for label, value in (
            ("component_id", self.component_id),
            ("model_instance_id", self.model_instance_id),
            ("asset_id", self.asset_id),
            ("prefix", self.prefix),
        ):
            _required_name(value, label=label)
        target_names = [target for target, _group in self.command_targets]
        if len(target_names) != len(set(target_names)):
            raise CompilerPrototypeError("duplicate command target")
        for target_id, group_name in self.command_targets:
            _required_name(target_id, label="command target ID")
            _required_name(group_name, label=f"target {target_id} SRDF group")


@dataclass(frozen=True, slots=True)
class AttachmentInput:
    attachment_id: str
    parent_model_instance_id: str
    child_model_instance_id: str
    parent_frame: str
    child_frame: str

    def __post_init__(self) -> None:
        for label, value in (
            ("attachment_id", self.attachment_id),
            ("parent_model_instance_id", self.parent_model_instance_id),
            ("child_model_instance_id", self.child_model_instance_id),
            ("parent_frame", self.parent_frame),
            ("child_frame", self.child_frame),
        ):
            _required_name(value, label=label)
        if self.parent_model_instance_id == self.child_model_instance_id:
            raise CompilerPrototypeError("attachment cannot self-reference")


@dataclass(frozen=True, slots=True)
class CompiledAttachment:
    attachment_id: str
    parent_model_instance_id: str
    child_model_instance_id: str
    parent_frame: str
    child_frame: str
    generated_parent_frame: str
    generated_child_frame: str

    def __post_init__(self) -> None:
        for label, value in (
            ("attachment_id", self.attachment_id),
            ("parent_model_instance_id", self.parent_model_instance_id),
            ("child_model_instance_id", self.child_model_instance_id),
            ("parent_frame", self.parent_frame),
            ("child_frame", self.child_frame),
            ("generated_parent_frame", self.generated_parent_frame),
            ("generated_child_frame", self.generated_child_frame),
        ):
            _required_name(value, label=label)
        if self.parent_model_instance_id == self.child_model_instance_id:
            raise CompilerPrototypeError("compiled attachment cannot self-reference")


@dataclass(frozen=True, slots=True)
class CompiledModelInstance:
    component_id: str
    model_instance_id: str
    asset_id: str
    asset_fingerprint: str
    instance_fingerprint: str
    prefix: str
    symbols: tuple[tuple[str, tuple[tuple[str, str], ...]], ...]
    targets: tuple[tuple[str, str, tuple[str, ...]], ...]

    def __post_init__(self) -> None:
        for label, value in (
            ("component_id", self.component_id),
            ("model_instance_id", self.model_instance_id),
            ("asset_id", self.asset_id),
            ("prefix", self.prefix),
        ):
            _required_name(value, label=label)
        for label, digest in (
            ("asset_fingerprint", self.asset_fingerprint),
            ("instance_fingerprint", self.instance_fingerprint),
        ):
            if not isinstance(digest, str) or _SHA256_RE.fullmatch(digest) is None:
                raise CompilerPrototypeError(f"{label} must be lowercase SHA-256")
        kinds = tuple(kind for kind, _entries in self.symbols)
        if kinds != _SYMBOL_KINDS:
            raise CompilerPrototypeError(
                f"compiled symbol kinds must be {_SYMBOL_KINDS!r}"
            )
        generated: list[tuple[str, str]] = []
        for kind, entries in self.symbols:
            logical_names = tuple(logical for logical, _runtime in entries)
            runtime_names = tuple(runtime for _logical, runtime in entries)
            if len(logical_names) != len(set(logical_names)):
                raise CompilerPrototypeError(
                    f"duplicate {kind} logical symbol in compiled instance"
                )
            if len(runtime_names) != len(set(runtime_names)):
                raise CompilerPrototypeError(
                    f"duplicate {kind} runtime symbol in compiled instance"
                )
            for logical, runtime in entries:
                _required_name(logical, label=f"{kind} logical symbol")
                _required_name(runtime, label=f"{kind} runtime symbol")
                generated.append((kind, runtime))
        if len(generated) != len(set(generated)):
            raise CompilerPrototypeError("duplicate generated symbol in instance")
        target_ids = tuple(target_id for target_id, _group, _joints in self.targets)
        if len(target_ids) != len(set(target_ids)):
            raise CompilerPrototypeError("duplicate compiled command target")
        runtime_groups = {
            runtime for _logical, runtime in dict(self.symbols)["groups"]
        }
        runtime_joints = {
            runtime for _logical, runtime in dict(self.symbols)["joints"]
        }
        for target_id, generated_group, joints in self.targets:
            _required_name(target_id, label="compiled target ID")
            _required_name(
                generated_group,
                label=f"compiled target {target_id} group",
            )
            if len(joints) != len(set(joints)):
                raise CompilerPrototypeError(
                    f"compiled target {target_id} has duplicate joints"
                )
            if generated_group not in runtime_groups:
                raise CompilerPrototypeError(
                    f"compiled target {target_id} references an unknown group"
                )
            for joint in joints:
                _required_name(joint, label=f"compiled target {target_id} joint")
                if joint not in runtime_joints:
                    raise CompilerPrototypeError(
                        f"compiled target {target_id} references an unknown joint"
                    )

    def symbols_for(self, kind: str) -> Mapping[str, str]:
        return dict(dict(self.symbols)[kind])


@dataclass(frozen=True, slots=True)
class CompiledBundle:
    instances: tuple[CompiledModelInstance, ...]
    attachments: tuple[CompiledAttachment, ...]
    canonical_json: str
    sha256: str

    def __post_init__(self) -> None:
        if _SHA256_RE.fullmatch(self.sha256) is None:
            raise CompilerPrototypeError("bundle sha256 must be lowercase SHA-256")
        expected = hashlib.sha256(self.canonical_json.encode("utf-8")).hexdigest()
        if expected != self.sha256:
            raise CompilerPrototypeError("bundle sha256 does not match content")
        for label, values in (
            (
                "compiled model instance",
                tuple(item.model_instance_id for item in self.instances),
            ),
            (
                "compiled component",
                tuple(item.component_id for item in self.instances),
            ),
            ("compiled prefix", tuple(item.prefix for item in self.instances)),
        ):
            if len(values) != len(set(values)):
                raise CompilerPrototypeError(f"duplicate {label}")
        if tuple(item.model_instance_id for item in self.instances) != tuple(
            sorted(item.model_instance_id for item in self.instances)
        ):
            raise CompilerPrototypeError(
                "compiled model instances must be sorted by identity"
            )
        known_instances = {item.model_instance_id for item in self.instances}
        global_symbols: list[tuple[str, str]] = []
        for instance in self.instances:
            global_symbols.extend(
                (kind, runtime)
                for kind, entries in instance.symbols
                for _logical, runtime in entries
            )
        if len(global_symbols) != len(set(global_symbols)):
            raise CompilerPrototypeError("generated symbol collision in bundle")
        attachment_ids = tuple(item.attachment_id for item in self.attachments)
        if len(attachment_ids) != len(set(attachment_ids)):
            raise CompilerPrototypeError("duplicate compiled attachment")
        if attachment_ids != tuple(sorted(attachment_ids)):
            raise CompilerPrototypeError(
                "compiled attachments must be sorted by identity"
            )
        by_instance = {
            instance.model_instance_id: instance for instance in self.instances
        }
        child_instances: set[str] = set()
        attachment_parents: dict[str, str] = {}
        for attachment in self.attachments:
            if {
                attachment.parent_model_instance_id,
                attachment.child_model_instance_id,
            } - known_instances:
                raise CompilerPrototypeError(
                    "compiled attachment references unknown model instance"
                )
            parent_frames = by_instance[
                attachment.parent_model_instance_id
            ].symbols_for("frames")
            child_frames = by_instance[
                attachment.child_model_instance_id
            ].symbols_for("frames")
            if (
                parent_frames.get(attachment.parent_frame)
                != attachment.generated_parent_frame
                or child_frames.get(attachment.child_frame)
                != attachment.generated_child_frame
            ):
                raise CompilerPrototypeError(
                    "compiled attachment frame mapping is inconsistent"
                )
            if attachment.child_model_instance_id in child_instances:
                raise CompilerPrototypeError(
                    "compiled attachment child has multiple parent edges"
                )
            child_instances.add(attachment.child_model_instance_id)
            attachment_parents[attachment.child_model_instance_id] = (
                attachment.parent_model_instance_id
            )
        for start in known_instances:
            cursor = start
            visited: set[str] = set()
            while cursor in attachment_parents:
                if cursor in visited:
                    raise CompilerPrototypeError("compiled attachment cycle")
                visited.add(cursor)
                cursor = attachment_parents[cursor]
        expected_payload = _canonical_json(
            {
                "$schema": SCHEMA_ID,
                "attachments": [
                    _compiled_attachment_primitive(item)
                    for item in self.attachments
                ],
                "instances": [
                    _compiled_instance_primitive(item)
                    for item in self.instances
                ],
            }
        )
        if self.canonical_json != expected_payload:
            raise CompilerPrototypeError(
                "bundle content does not match parsed semantic records"
            )

    @classmethod
    def from_json(cls, payload: str) -> CompiledBundle:
        try:
            raw = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise CompilerPrototypeError("invalid retained bundle JSON") from exc
        if not isinstance(raw, dict) or raw.get("$schema") != SCHEMA_ID:
            raise CompilerPrototypeError("unexpected retained bundle schema")
        if set(raw) != {"$schema", "attachments", "instances"}:
            raise CompilerPrototypeError("unknown retained bundle fields")
        instances_raw = raw["instances"]
        if not isinstance(instances_raw, list):
            raise CompilerPrototypeError("instances must be a list")
        instances = tuple(
            _compiled_instance_from_primitive(item)
            for item in instances_raw
        )
        attachments_raw = raw["attachments"]
        if not isinstance(attachments_raw, list):
            raise CompilerPrototypeError("attachments must be a list")
        attachments = tuple(
            _compiled_attachment_from_primitive(item)
            for item in attachments_raw
        )
        canonical = _canonical_json(raw)
        if canonical != payload:
            raise CompilerPrototypeError("retained bundle is not canonical JSON")
        return cls(
            instances=instances,
            attachments=attachments,
            canonical_json=canonical,
            sha256=hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
        )


def _canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _canonical_fingerprint(value: object) -> str:
    return hashlib.sha256(_canonical_json(value).encode("utf-8")).hexdigest()


def _retained_asset_fingerprint(
    source: ModelSource,
    *,
    urdf_sha256: str,
    srdf_sha256: str,
) -> str:
    """Fingerprint logical model content without checkout/runtime paths."""

    return _canonical_fingerprint(
        {
            "schema": "manyforge.experimental.composable_cell.asset.i1a.v0",
            "asset_id": source.asset_id,
            "model_name": source.model_name,
            "srdf_sha256": srdf_sha256,
            "urdf_sha256": urdf_sha256,
        }
    )


def _retained_instance_fingerprint(
    instance: ModelInstanceInput,
    *,
    asset_fingerprint: str,
    symbols: tuple[tuple[str, tuple[tuple[str, str], ...]], ...],
    targets: tuple[tuple[str, str, tuple[str, ...]], ...],
) -> str:
    return _canonical_fingerprint(
        {
            "schema": "manyforge.experimental.composable_cell.instance.i1a.v0",
            "asset_fingerprint": asset_fingerprint,
            "model_instance_id": instance.model_instance_id,
            "prefix": instance.prefix,
            "symbols": {
                kind: dict(entries)
                for kind, entries in symbols
            },
            "targets": [
                {
                    "command_target_id": target_id,
                    "expanded_joints": list(joints),
                    "generated_group": generated_group,
                }
                for target_id, generated_group, joints in targets
            ],
        }
    )


def _primitive_string(raw: object, *, label: str) -> str:
    if not isinstance(raw, str):
        raise CompilerPrototypeError(f"{label} must be a string")
    return _required_name(raw, label=label)


def _primitive_string_list(raw: object, *, label: str) -> tuple[str, ...]:
    if not isinstance(raw, list):
        raise CompilerPrototypeError(f"{label} must be a list")
    return tuple(
        _primitive_string(item, label=f"{label} item")
        for item in raw
    )


def _parse_urdf_chain_data(
    urdf_xml: str | None,
) -> tuple[set[str], Mapping[str, tuple[str, str, str]]]:
    if urdf_xml is None:
        return set(), {}
    try:
        root = ET.fromstring(urdf_xml)
    except ET.ParseError as exc:
        raise CompilerPrototypeError("invalid URDF XML") from exc
    links = {
        _required_name(node.attrib.get("name"), label="URDF link name")
        for node in root.findall("./link")
    }
    by_child: dict[str, tuple[str, str, str]] = {}
    for node in root.findall("./joint"):
        name = _required_name(node.attrib.get("name"), label="URDF joint name")
        parent = node.find("parent")
        child = node.find("child")
        if parent is None or child is None:
            raise CompilerPrototypeError(f"joint {name} lacks parent/child")
        parent_name = _required_name(
            parent.attrib.get("link"), label=f"joint {name} parent"
        )
        child_name = _required_name(
            child.attrib.get("link"), label=f"joint {name} child"
        )
        if child_name in by_child:
            raise CompilerPrototypeError(
                f"multiple joints resolve child link {child_name}"
            )
        by_child[child_name] = (
            parent_name,
            name,
            str(node.attrib.get("type", "")).strip(),
        )
    return links, by_child


def _resolve_chain(
    base: str,
    tip: str,
    *,
    links: set[str],
    by_child: Mapping[str, tuple[str, str, str]],
) -> tuple[str, ...]:
    if not by_child:
        return ()
    if base not in links:
        raise CompilerPrototypeError(f"chain references unknown base link {base}")
    if tip not in links:
        raise CompilerPrototypeError(f"chain references unknown tip link {tip}")
    cursor = tip
    reverse_joints: list[str] = []
    visited: set[str] = set()
    while cursor != base:
        if cursor in visited:
            raise CompilerPrototypeError(
                f"kinematic cycle while resolving {base}->{tip}"
            )
        visited.add(cursor)
        edge = by_child.get(cursor)
        if edge is None:
            raise CompilerPrototypeError(f"cannot resolve chain {base}->{tip}")
        parent, joint, joint_type = edge
        if joint_type != "fixed":
            reverse_joints.append(joint)
        cursor = parent
    return tuple(reversed(reverse_joints))


def parse_srdf_groups(
    srdf_xml: str,
    *,
    urdf_xml: str | None = None,
) -> Mapping[str, SrdfGroupDefinition]:
    """Parse top-level SRDF groups, retaining explicit subgroup structure."""

    try:
        root = ET.fromstring(srdf_xml)
    except ET.ParseError as exc:
        raise CompilerPrototypeError("invalid SRDF XML") from exc
    groups: dict[str, SrdfGroupDefinition] = {}
    for node in root.findall("./group"):
        name = _required_name(node.attrib.get("name"), label="SRDF group name")
        if name in groups:
            raise CompilerPrototypeError(f"duplicate SRDF group {name}")
        groups[name] = SrdfGroupDefinition(
            name=name,
            joints=tuple(
                _required_name(child.attrib.get("name"), label=f"group {name} joint")
                for child in node.findall("./joint")
            ),
            links=tuple(
                _required_name(child.attrib.get("name"), label=f"group {name} link")
                for child in node.findall("./link")
            ),
            chains=tuple(
                (
                    _required_name(
                        child.attrib.get("base_link"),
                        label=f"group {name} chain base",
                    ),
                    _required_name(
                        child.attrib.get("tip_link"),
                        label=f"group {name} chain tip",
                    ),
                )
                for child in node.findall("./chain")
            ),
            subgroups=tuple(
                _required_name(
                    child.attrib.get("name"), label=f"group {name} subgroup"
                )
                for child in node.findall("./group")
            ),
        )
    links, by_child = _parse_urdf_chain_data(urdf_xml)
    for group in groups.values():
        for subgroup in group.subgroups:
            if subgroup not in groups:
                raise CompilerPrototypeError(
                    f"group {group.name} references unknown subgroup {subgroup}"
                )
        for base, tip in group.chains:
            _resolve_chain(base, tip, links=links, by_child=by_child)
    return groups


def expand_srdf_group_joints(
    groups: Mapping[str, SrdfGroupDefinition],
    group_name: str,
    *,
    urdf_xml: str | None = None,
) -> tuple[str, ...]:
    """Recursively expand a group and preserve first-seen joint order."""

    links, by_child = _parse_urdf_chain_data(urdf_xml)

    def expand(name: str, stack: tuple[str, ...]) -> tuple[str, ...]:
        group = groups.get(name)
        if group is None:
            raise CompilerPrototypeError(f"unknown SRDF group {name}")
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise CompilerPrototypeError(f"SRDF subgroup cycle: {cycle}")
        values = list(group.joints)
        for base, tip in group.chains:
            values.extend(
                _resolve_chain(base, tip, links=links, by_child=by_child)
            )
        for subgroup in group.subgroups:
            values.extend(expand(subgroup, (*stack, name)))
        return _ordered_unique(values)

    return expand(group_name, ())


def _compiled_instance_primitive(instance: CompiledModelInstance) -> dict[str, Any]:
    return {
        "asset_fingerprint": instance.asset_fingerprint,
        "asset_id": instance.asset_id,
        "component_id": instance.component_id,
        "instance_fingerprint": instance.instance_fingerprint,
        "model_instance_id": instance.model_instance_id,
        "prefix": instance.prefix,
        "symbols": {
            kind: dict(entries)
            for kind, entries in instance.symbols
        },
        "targets": [
            {
                "expanded_joints": list(joints),
                "generated_group": generated_group,
                "target_id": target_id,
            }
            for target_id, generated_group, joints in instance.targets
        ],
    }


def _compiled_attachment_primitive(
    attachment: CompiledAttachment,
) -> dict[str, str]:
    return {
        "attachment_id": attachment.attachment_id,
        "child_frame": attachment.child_frame,
        "child_model_instance_id": attachment.child_model_instance_id,
        "generated_child_frame": attachment.generated_child_frame,
        "generated_parent_frame": attachment.generated_parent_frame,
        "parent_frame": attachment.parent_frame,
        "parent_model_instance_id": attachment.parent_model_instance_id,
    }


def _compiled_attachment_from_primitive(raw: object) -> CompiledAttachment:
    if not isinstance(raw, dict):
        raise CompilerPrototypeError("compiled attachment must be an object")
    expected = {
        "attachment_id",
        "child_frame",
        "child_model_instance_id",
        "generated_child_frame",
        "generated_parent_frame",
        "parent_frame",
        "parent_model_instance_id",
    }
    if set(raw) != expected:
        raise CompilerPrototypeError("unknown compiled attachment fields")
    return CompiledAttachment(
        attachment_id=_primitive_string(
            raw["attachment_id"],
            label="compiled attachment ID",
        ),
        parent_model_instance_id=_primitive_string(
            raw["parent_model_instance_id"],
            label="compiled attachment parent instance",
        ),
        child_model_instance_id=_primitive_string(
            raw["child_model_instance_id"],
            label="compiled attachment child instance",
        ),
        parent_frame=_primitive_string(
            raw["parent_frame"],
            label="compiled attachment parent frame",
        ),
        child_frame=_primitive_string(
            raw["child_frame"],
            label="compiled attachment child frame",
        ),
        generated_parent_frame=_primitive_string(
            raw["generated_parent_frame"],
            label="compiled attachment generated parent frame",
        ),
        generated_child_frame=_primitive_string(
            raw["generated_child_frame"],
            label="compiled attachment generated child frame",
        ),
    )


def _compiled_instance_from_primitive(raw: object) -> CompiledModelInstance:
    if not isinstance(raw, dict):
        raise CompilerPrototypeError("compiled instance must be an object")
    expected = {
        "asset_fingerprint",
        "asset_id",
        "component_id",
        "instance_fingerprint",
        "model_instance_id",
        "prefix",
        "symbols",
        "targets",
    }
    if set(raw) != expected:
        raise CompilerPrototypeError("unknown compiled instance fields")
    symbols_raw = raw["symbols"]
    if not isinstance(symbols_raw, dict):
        raise CompilerPrototypeError("compiled symbols must be an object")
    if tuple(sorted(symbols_raw)) != tuple(sorted(_SYMBOL_KINDS)):
        raise CompilerPrototypeError(
            f"compiled symbol kinds must be {_SYMBOL_KINDS!r}"
        )
    parsed_symbols: list[tuple[str, tuple[tuple[str, str], ...]]] = []
    for kind in _SYMBOL_KINDS:
        mapping = symbols_raw[kind]
        if not isinstance(mapping, dict):
            raise CompilerPrototypeError("compiled symbol map must contain objects")
        entries: list[tuple[str, str]] = []
        for source, generated in sorted(mapping.items()):
            entries.append(
                (
                    _primitive_string(
                        source,
                        label=f"{kind} logical symbol",
                    ),
                    _primitive_string(
                        generated,
                        label=f"{kind} runtime symbol",
                    ),
                )
            )
        parsed_symbols.append((kind, tuple(entries)))
    targets_raw = raw["targets"]
    if not isinstance(targets_raw, list):
        raise CompilerPrototypeError("compiled targets must be a list")
    targets: list[tuple[str, str, tuple[str, ...]]] = []
    for target in targets_raw:
        if not isinstance(target, dict) or set(target) != {
            "expanded_joints",
            "generated_group",
            "target_id",
        }:
            raise CompilerPrototypeError("invalid compiled target")
        targets.append(
            (
                _primitive_string(
                    target["target_id"],
                    label="compiled target ID",
                ),
                _primitive_string(
                    target["generated_group"],
                    label="compiled target group",
                ),
                _primitive_string_list(
                    target["expanded_joints"],
                    label="expanded_joints",
                ),
            )
        )
    return CompiledModelInstance(
        component_id=_primitive_string(raw["component_id"], label="component_id"),
        model_instance_id=_primitive_string(
            raw["model_instance_id"],
            label="model_instance_id",
        ),
        asset_id=_primitive_string(raw["asset_id"], label="asset_id"),
        asset_fingerprint=_primitive_string(
            raw["asset_fingerprint"],
            label="asset_fingerprint",
        ),
        instance_fingerprint=_primitive_string(
            raw["instance_fingerprint"],
            label="instance_fingerprint",
        ),
        prefix=_primitive_string(raw["prefix"], label="prefix"),
        symbols=tuple(parsed_symbols),
        targets=tuple(targets),
    )


def compile_model_cell(
    *,
    sources: Sequence[ModelSource],
    instances: Sequence[ModelInstanceInput],
    attachments: Sequence[AttachmentInput] = (),
) -> CompiledBundle:
    """Compile deterministic symbol/target maps for a candidate cell."""

    source_by_id: dict[str, ModelSource] = {}
    for source in sources:
        if source.asset_id in source_by_id:
            raise CompilerPrototypeError(f"duplicate asset_id {source.asset_id}")
        source_by_id[source.asset_id] = source

    component_ids = [instance.component_id for instance in instances]
    model_instance_ids = [instance.model_instance_id for instance in instances]
    prefixes = [instance.prefix for instance in instances]
    for label, values in (
        ("component_id", component_ids),
        ("model_instance_id", model_instance_ids),
        ("prefix", prefixes),
    ):
        if len(values) != len(set(values)):
            raise CompilerPrototypeError(f"duplicate {label}")

    compiled: list[CompiledModelInstance] = []
    generated_symbols: dict[tuple[str, str], tuple[str, str]] = {}
    for instance in instances:
        source = source_by_id.get(instance.asset_id)
        if source is None:
            raise CompilerPrototypeError(f"unknown asset_id {instance.asset_id}")
        try:
            authority = load_robot_model_authority(
                urdf_path=source.urdf_path,
                srdf_path=source.srdf_path,
                package_paths=source.package_paths,
                model_name=source.model_name,
            )
            view = robot_model_instance_view(
                authority,
                instance_id=instance.model_instance_id,
                prefix=instance.prefix,
                asset_id=instance.asset_id,
            )
        except RobotModelAuthorityError as exc:
            raise CompilerPrototypeError(str(exc)) from exc
        urdf_xml = source.urdf_path.read_text(encoding="utf-8")
        srdf_xml = source.srdf_path.read_text(encoding="utf-8")
        groups = parse_srdf_groups(srdf_xml, urdf_xml=urdf_xml)
        targets: list[tuple[str, str, tuple[str, ...]]] = []
        for target_id, group_name in instance.command_targets:
            if group_name not in groups:
                raise CompilerPrototypeError(
                    f"target {target_id} references unknown SRDF group {group_name}"
                )
            expanded = expand_srdf_group_joints(
                groups,
                group_name,
                urdf_xml=urdf_xml,
            )
            generated_joints = tuple(
                view.joints.generated(joint)
                for joint in expanded
            )
            targets.append(
                (
                    target_id,
                    view.groups.generated(group_name),
                    generated_joints,
                )
            )
        symbols = tuple(
            (
                kind,
                tuple(sorted(mapping.canonical_to_generated.items())),
            )
            for kind, mapping in (
                ("frames", view.frames),
                ("groups", view.groups),
                ("joints", view.joints),
                ("links", view.links),
            )
        )
        for kind, entries in symbols:
            for source_name, generated in entries:
                key = (kind, generated)
                previous = generated_symbols.setdefault(
                    key,
                    (instance.model_instance_id, f"{kind}:{source_name}"),
                )
                if previous != (
                    instance.model_instance_id,
                    f"{kind}:{source_name}",
                ):
                    raise CompilerPrototypeError(
                        f"generated symbol collision: {generated}"
                    )
        asset_fingerprint = _retained_asset_fingerprint(
            source,
            urdf_sha256=view.asset.urdf_sha256,
            srdf_sha256=view.asset.srdf_sha256,
        )
        compiled_targets = tuple(targets)
        compiled.append(
            CompiledModelInstance(
                component_id=instance.component_id,
                model_instance_id=instance.model_instance_id,
                asset_id=instance.asset_id,
                asset_fingerprint=asset_fingerprint,
                instance_fingerprint=_retained_instance_fingerprint(
                    instance,
                    asset_fingerprint=asset_fingerprint,
                    symbols=symbols,
                    targets=compiled_targets,
                ),
                prefix=instance.prefix,
                symbols=symbols,
                targets=compiled_targets,
            )
        )

    compiled_by_id = {item.model_instance_id: item for item in compiled}
    attachment_ids: set[str] = set()
    child_instances: set[str] = set()
    parent_by_child: dict[str, str] = {}
    compiled_attachments: list[CompiledAttachment] = []
    for attachment in attachments:
        if attachment.attachment_id in attachment_ids:
            raise CompilerPrototypeError(
                f"duplicate attachment_id {attachment.attachment_id}"
            )
        attachment_ids.add(attachment.attachment_id)
        parent = compiled_by_id.get(attachment.parent_model_instance_id)
        child = compiled_by_id.get(attachment.child_model_instance_id)
        if parent is None or child is None:
            raise CompilerPrototypeError(
                f"attachment {attachment.attachment_id} references unknown instance"
            )
        if attachment.child_model_instance_id in child_instances:
            raise CompilerPrototypeError(
                f"attachment child {attachment.child_model_instance_id} "
                "has multiple parent edges"
            )
        child_instances.add(attachment.child_model_instance_id)
        parent_by_child[attachment.child_model_instance_id] = (
            attachment.parent_model_instance_id
        )
        try:
            generated_parent = parent.symbols_for("frames")[
                attachment.parent_frame
            ]
        except KeyError as exc:
            raise CompilerPrototypeError(
                f"attachment {attachment.attachment_id} references unknown "
                f"parent frame {attachment.parent_frame}"
            ) from exc
        try:
            generated_child = child.symbols_for("frames")[attachment.child_frame]
        except KeyError as exc:
            raise CompilerPrototypeError(
                f"attachment {attachment.attachment_id} references unknown "
                f"child frame {attachment.child_frame}"
            ) from exc
        compiled_attachments.append(
            CompiledAttachment(
                attachment_id=attachment.attachment_id,
                parent_model_instance_id=attachment.parent_model_instance_id,
                child_model_instance_id=attachment.child_model_instance_id,
                parent_frame=attachment.parent_frame,
                child_frame=attachment.child_frame,
                generated_parent_frame=generated_parent,
                generated_child_frame=generated_child,
            )
        )
    for start in compiled_by_id:
        cursor = start
        visited: set[str] = set()
        while cursor in parent_by_child:
            if cursor in visited:
                raise CompilerPrototypeError("attachment graph contains a cycle")
            visited.add(cursor)
            cursor = parent_by_child[cursor]

    primitive = {
        "$schema": SCHEMA_ID,
        "attachments": [
            _compiled_attachment_primitive(item)
            for item in sorted(
                compiled_attachments,
                key=lambda item: item.attachment_id,
            )
        ],
        "instances": [
            _compiled_instance_primitive(item)
            for item in sorted(compiled, key=lambda item: item.model_instance_id)
        ],
    }
    canonical = _canonical_json(primitive)
    return CompiledBundle(
        instances=tuple(sorted(compiled, key=lambda item: item.model_instance_id)),
        attachments=tuple(
            sorted(
                compiled_attachments,
                key=lambda item: item.attachment_id,
            )
        ),
        canonical_json=canonical,
        sha256=hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
    )


def compile_control_binding(
    *,
    component_type: ComponentTypeRecord,
    interface_contract: ControlInterfaceContract,
    topology: ControllerTopologyRecord,
    controllable_component_id: ControllableComponentId,
    control_domain_id: ControlDomainId,
    control_binding_id: ControlBindingId,
    binding_revision: BindingRevision,
    command_target_id: CommandTargetId,
    capability_id: CapabilityId,
    route_id: RouteId,
    endpoints: tuple[str, ...],
) -> tuple[ControlBindingSpec, CompiledBindingSnapshot]:
    """Select and validate one binding-local capability and controller route.

    The prototype intentionally validates only declared interface names and
    exact resource identities. Mapping vendor/ROS interface spellings and
    controller-specific claim semantics remains a compiler-adapter concern.
    """

    target = next(
        (
            candidate
            for candidate in component_type.command_targets
            if candidate.command_target_id == command_target_id
        ),
        None,
    )
    if target is None:
        raise CompilerPrototypeError(
            f"unknown command target {command_target_id}"
        )
    capability = next(
        (
            candidate
            for candidate in component_type.capabilities
            if candidate.capability_id == capability_id
        ),
        None,
    )
    if capability is None or capability_id not in target.capability_ids:
        raise CompilerPrototypeError(
            f"capability {capability_id} is not available on target "
            f"{command_target_id}"
        )
    route = next(
        (
            candidate
            for candidate in topology.routes
            if candidate.route_id == route_id
        ),
        None,
    )
    if route is None:
        raise CompilerPrototypeError(f"unknown controller route {route_id}")
    if route.capability_id != capability_id:
        raise CompilerPrototypeError(
            f"route {route_id} does not satisfy capability {capability_id}"
        )

    known_resources = set(interface_contract.ordered_resources)
    missing_target_resources = set(target.resource_ids) - known_resources
    if missing_target_resources:
        raise CompilerPrototypeError(
            "command target resources are absent from the interface contract"
        )
    controller_by_id = {
        controller.controller_id: controller
        for controller in topology.controllers
    }
    route_claims = tuple(
        dict.fromkeys(
            resource
            for controller_id in route.controllers
            for resource in controller_by_id[controller_id].claimed_resources
        )
    )
    missing_claims = set(route_claims) - known_resources
    if missing_claims:
        raise CompilerPrototypeError(
            "controller route claims resources absent from the interface contract"
        )
    if set(route_claims) != set(target.resource_ids):
        raise CompilerPrototypeError(
            "controller route claims do not exactly cover target resources"
        )
    interface_target_order = tuple(
        resource
        for resource in interface_contract.ordered_resources
        if resource in set(target.resource_ids)
    )
    if interface_target_order != target.resource_ids:
        raise CompilerPrototypeError(
            "target resource order does not match the interface contract"
        )

    if set(capability.required_interfaces) - set(
        interface_contract.command_interfaces
    ):
        raise CompilerPrototypeError(
            "interface contract command interfaces cannot satisfy capability "
            f"{capability_id}"
        )

    binding = ControlBindingSpec(
        control_binding_id=control_binding_id,
        binding_revision=binding_revision,
        controllable_component_id=controllable_component_id,
        control_domain_id=control_domain_id,
        command_target_id=command_target_id,
        capability_id=capability_id,
        route_id=route_id,
        interface_contract_revision=(
            interface_contract.interface_contract_revision
        ),
        controller_topology_id=topology.controller_topology_id,
        topology_revision=topology.topology_revision,
        resource_ids=target.resource_ids,
        endpoints=endpoints,
    )
    snapshot = CompiledBindingSnapshot(
        control_binding_id=control_binding_id,
        binding_revision=binding_revision,
        controllable_component_id=controllable_component_id,
        control_domain_id=control_domain_id,
        command_target_id=command_target_id,
        capability_id=capability_id,
        route_id=route_id,
        interface_contract_revision=(
            interface_contract.interface_contract_revision
        ),
        controller_topology_id=topology.controller_topology_id,
        topology_revision=topology.topology_revision,
        ordered_resources=target.resource_ids,
        endpoints=endpoints,
    )
    return binding, snapshot


def to_retained_manifest(
    bundle: CompiledBundle,
    *,
    compiler_revision: CompilerRevision,
    catalog_revision: CatalogRevision,
    graph_revision: GraphRevision,
    bindings: tuple[CompiledBindingSnapshot, ...] = (),
) -> RetainedBundleManifest:
    """Project the P4 result into the shared provisional retained contract."""

    symbol_maps = tuple(
        CompiledSymbolMap(
            model_instance_id=ModelInstanceId(instance.model_instance_id),
            symbols=tuple(
                SymbolBinding(
                    logical_name=logical,
                    runtime_name=runtime,
                    kind=kind,
                )
                for kind, entries in instance.symbols
                for logical, runtime in entries
            ),
        )
        for instance in bundle.instances
    )
    return RetainedBundleManifest(
        schema=SCHEMA_ID,
        compiler_revision=compiler_revision,
        catalog_revision=catalog_revision,
        graph_revision=graph_revision,
        artifacts=(
            RetainedArtifact(
                name="compiled-model-bundle",
                kind="compiled_model_bundle",
                relative_path="manifests/compiled-model-bundle.json",
                content_sha256=Sha256Digest(bundle.sha256),
            ),
        ),
        symbol_maps=symbol_maps,
        bindings=tuple(
            sorted(bindings, key=lambda item: item.control_binding_id.value)
        ),
    )
