"""Canonical robot-model authority metadata.

This module is intentionally ROS-free and Pinocchio-free. It parses the
deployment URDF/SRDF as source data and produces immutable metadata that later
adapter-specific layers can use to build their own consumer views.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence


class RobotModelAuthorityError(ValueError):
    """Raised when a robot model cannot be trusted as canonical input."""


@dataclass(frozen=True)
class JointLimit:
    lower: float | None = None
    upper: float | None = None
    effort: float | None = None
    velocity: float | None = None


@dataclass(frozen=True)
class MimicLaw:
    master: str
    multiplier: float = 1.0
    offset: float = 0.0


@dataclass(frozen=True)
class RobotJoint:
    name: str
    joint_type: str
    parent_link: str
    child_link: str
    axis_xyz: tuple[float, float, float]
    limit: JointLimit | None = None
    mimic: MimicLaw | None = None
    is_mimic_master: bool = False

    @property
    def is_fixed(self) -> bool:
        return self.joint_type == "fixed"

    @property
    def is_derived(self) -> bool:
        return self.mimic is not None

    @property
    def is_commandable(self) -> bool:
        return not self.is_fixed and not self.is_derived


@dataclass(frozen=True)
class RobotGroup:
    name: str
    joints: tuple[str, ...] = ()
    links: tuple[str, ...] = ()
    chains: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class RobotGroupState:
    name: str
    group: str
    joint_positions: Mapping[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class RobotEndEffector:
    name: str
    group: str
    parent_link: str
    parent_group: str = ""


@dataclass(frozen=True)
class DisabledCollisionPair:
    link1: str
    link2: str
    reason: str = ""


@dataclass(frozen=True)
class RobotModelGripperConfig:
    group_id: str
    joint_name: str
    touch_links: tuple[str, ...] = ()
    mount_frame: str = ""
    attach_frame: str = ""
    tcp_frame: str = ""
    auxiliary_frames: tuple[str, ...] = ()


@dataclass(frozen=True)
class RobotModelGripperProfile:
    gripper_id: str
    group_id: str
    command_joint: str
    derived_joints: tuple[str, ...]
    touch_links: tuple[str, ...]
    mount_frame: str = ""
    attach_frame: str = ""
    tcp_frame: str = ""
    auxiliary_frames: tuple[str, ...] = ()
    presets: Mapping[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class RobotModelAssetDescriptor:
    """Stable source-asset descriptor consumed by composition layers."""

    asset_id: str
    model_name: str
    canonical_fingerprint: str
    urdf_sha256: str
    srdf_sha256: str
    urdf_path: str
    srdf_path: str
    package_paths: tuple[str, ...]
    links: tuple[str, ...]
    joints: tuple[str, ...]
    frames: tuple[str, ...]
    groups: tuple[str, ...]
    group_states: tuple[tuple[str, str], ...]
    end_effectors: tuple[str, ...]
    disabled_collision_pairs: tuple[tuple[str, str], ...]
    grippers: tuple[str, ...]
    commandable_joints: tuple[str, ...]
    derived_joints: tuple[str, ...]


@dataclass(frozen=True)
class RobotModelInstanceSpec:
    """Per-instance composition input for a validated robot asset."""

    instance_id: str
    prefix: str
    mount_parent_frame: str = ""
    mount_xyz: tuple[float, float, float] = (0.0, 0.0, 0.0)
    mount_xyzw: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    controller_bindings: Mapping[str, str] = field(default_factory=dict)
    state_sources: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class RobotModelNameMap:
    canonical_to_generated: Mapping[str, str]
    generated_to_canonical: Mapping[str, str]

    def generated(self, canonical_name: str) -> str:
        return self.canonical_to_generated[canonical_name]

    def canonical(self, generated_name: str) -> str:
        return self.generated_to_canonical[generated_name]


@dataclass(frozen=True)
class GeneratedMimicLaw:
    source_joint: str
    generated_joint: str
    source_master: str
    generated_master: str
    multiplier: float
    offset: float


@dataclass(frozen=True)
class GeneratedDisabledCollisionPair:
    source_link1: str
    source_link2: str
    generated_link1: str
    generated_link2: str
    reason: str = ""


@dataclass(frozen=True)
class GeneratedGripperProfile:
    source_gripper_id: str
    generated_gripper_id: str
    source_group_id: str
    generated_group_id: str
    source_command_joint: str
    generated_command_joint: str
    source_derived_joints: tuple[str, ...]
    generated_derived_joints: tuple[str, ...]
    source_touch_links: tuple[str, ...]
    generated_touch_links: tuple[str, ...]
    source_mount_frame: str = ""
    generated_mount_frame: str = ""
    source_attach_frame: str = ""
    generated_attach_frame: str = ""
    source_tcp_frame: str = ""
    generated_tcp_frame: str = ""
    source_auxiliary_frames: tuple[str, ...] = ()
    generated_auxiliary_frames: tuple[str, ...] = ()
    presets: Mapping[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class RobotModelTraceabilityRecord:
    kind: str
    source_asset_id: str
    source_fingerprint: str
    instance_id: str
    source_name: str
    generated_name: str
    mount_parent_frame: str = ""
    mount_xyz: tuple[float, float, float] = (0.0, 0.0, 0.0)
    mount_xyzw: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)


@dataclass(frozen=True)
class RobotModelInstanceView:
    """Generated-name view for one robot asset instance.

    This is not a composite URDF/SRDF yet. It is the deterministic authority map
    that later compilers must use when producing composite files or runtime
    diagnostics.
    """

    asset: RobotModelAssetDescriptor
    spec: RobotModelInstanceSpec
    instance_fingerprint: str
    links: RobotModelNameMap
    joints: RobotModelNameMap
    frames: RobotModelNameMap
    groups: RobotModelNameMap
    group_states: RobotModelNameMap
    end_effectors: RobotModelNameMap
    generated_mimic_laws: tuple[GeneratedMimicLaw, ...]
    disabled_collision_pairs: tuple[GeneratedDisabledCollisionPair, ...]
    grippers: Mapping[str, GeneratedGripperProfile]
    traceability: tuple[RobotModelTraceabilityRecord, ...]


@dataclass(frozen=True)
class RobotModelAuthority:
    model_name: str
    urdf_path: str
    srdf_path: str
    package_paths: tuple[str, ...]
    urdf_sha256: str
    srdf_sha256: str
    canonical_fingerprint: str
    links: tuple[str, ...]
    joints: Mapping[str, RobotJoint]
    groups: Mapping[str, RobotGroup]
    group_states: tuple[RobotGroupState, ...]
    end_effectors: tuple[RobotEndEffector, ...]
    disabled_collisions: tuple[DisabledCollisionPair, ...]
    grippers: Mapping[str, RobotModelGripperProfile]

    @property
    def commandable_joint_names(self) -> tuple[str, ...]:
        return tuple(
            name for name, joint in self.joints.items() if joint.is_commandable
        )

    @property
    def derived_joint_names(self) -> tuple[str, ...]:
        return tuple(name for name, joint in self.joints.items() if joint.is_derived)

    def group_commandable_joints(self, group_id: str) -> tuple[str, ...]:
        group = self.groups.get(group_id)
        if group is None:
            raise RobotModelAuthorityError(f"unknown SRDF group: {group_id}")
        return tuple(
            joint_name
            for joint_name in group.joints
            if self.joints.get(joint_name) is not None
            and self.joints[joint_name].is_commandable
        )


@dataclass(frozen=True)
class RobotModelImportView:
    """Private consumer view derived from a canonical robot model.

    The view is intentionally not a product model. It exists for consumers with
    parser limitations, while user-facing names remain canonical through the
    mapping fields.
    """

    consumer: str
    canonical_fingerprint: str
    view_fingerprint: str
    urdf_xml: str
    srdf_xml: str
    canonical_to_view_joint_names: Mapping[str, str]
    view_to_canonical_joint_names: Mapping[str, str]

    @property
    def changed(self) -> bool:
        return any(
            canonical != view
            for canonical, view in self.canonical_to_view_joint_names.items()
        )

    def view_joint_name(self, canonical_joint_name: str) -> str:
        return self.canonical_to_view_joint_names.get(
            canonical_joint_name,
            canonical_joint_name,
        )

    def canonical_joint_name(self, view_joint_name: str) -> str:
        return self.view_to_canonical_joint_names.get(
            view_joint_name,
            view_joint_name,
        )

    def write_to_directory(
        self,
        directory: str | Path,
        *,
        stem: str,
    ) -> tuple[Path, Path]:
        target = Path(directory)
        target.mkdir(parents=True, exist_ok=True)
        urdf_path = target / f"{stem}.urdf"
        srdf_path = target / f"{stem}.srdf"
        urdf_path.write_text(self.urdf_xml, encoding="utf-8")
        srdf_path.write_text(self.srdf_xml, encoding="utf-8")
        return urdf_path, srdf_path


def _read_bytes(path: Path, *, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise RobotModelAuthorityError(f"failed to read {label}: {path}") from exc


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _parse_xml(data: bytes, *, label: str) -> ET.Element:
    try:
        return ET.fromstring(data)
    except ET.ParseError as exc:
        raise RobotModelAuthorityError(f"invalid {label} XML: {exc}") from exc


def _float_attr(
    element: ET.Element,
    name: str,
    *,
    default: float | None = None,
    context: str,
) -> float | None:
    raw = element.attrib.get(name)
    if raw is None:
        return default
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise RobotModelAuthorityError(f"{context}.{name} must be numeric") from exc
    if not math.isfinite(value):
        raise RobotModelAuthorityError(f"{context}.{name} must be finite")
    return value


def _required_attr(element: ET.Element, name: str, *, context: str) -> str:
    value = str(element.attrib.get(name, "")).strip()
    if not value:
        raise RobotModelAuthorityError(f"{context}.{name} is required")
    return value


def _parse_axis(joint_element: ET.Element) -> tuple[float, float, float]:
    axis = joint_element.find("axis")
    if axis is None:
        return (1.0, 0.0, 0.0)
    raw = axis.attrib.get("xyz", "1 0 0").split()
    if len(raw) != 3:
        raise RobotModelAuthorityError(
            "joint "
            f"{joint_element.attrib.get('name', '<unnamed>')} "
            "axis must have 3 values"
        )
    try:
        values = (float(raw[0]), float(raw[1]), float(raw[2]))
    except ValueError as exc:
        raise RobotModelAuthorityError(
            "joint "
            f"{joint_element.attrib.get('name', '<unnamed>')} "
            "axis must be numeric"
        ) from exc
    if not all(math.isfinite(value) for value in values):
        raise RobotModelAuthorityError(
            f"joint {joint_element.attrib.get('name', '<unnamed>')} axis must be finite"
        )
    return values


def _parse_limit(joint_element: ET.Element) -> JointLimit | None:
    limit = joint_element.find("limit")
    if limit is None:
        return None
    joint_name = joint_element.attrib.get("name", "<unnamed>")
    return JointLimit(
        lower=_float_attr(limit, "lower", context=f"joint {joint_name} limit"),
        upper=_float_attr(limit, "upper", context=f"joint {joint_name} limit"),
        effort=_float_attr(limit, "effort", context=f"joint {joint_name} limit"),
        velocity=_float_attr(limit, "velocity", context=f"joint {joint_name} limit"),
    )


def _parse_mimic(joint_element: ET.Element) -> MimicLaw | None:
    mimic = joint_element.find("mimic")
    if mimic is None:
        return None
    joint_name = joint_element.attrib.get("name", "<unnamed>")
    multiplier = _float_attr(
        mimic,
        "multiplier",
        default=1.0,
        context=f"joint {joint_name} mimic",
    )
    offset = _float_attr(
        mimic,
        "offset",
        default=0.0,
        context=f"joint {joint_name} mimic",
    )
    return MimicLaw(
        master=_required_attr(mimic, "joint", context=f"joint {joint_name} mimic"),
        multiplier=1.0 if multiplier is None else multiplier,
        offset=0.0 if offset is None else offset,
    )


def _parse_urdf(root: ET.Element) -> tuple[tuple[str, ...], dict[str, RobotJoint]]:
    if root.tag != "robot":
        raise RobotModelAuthorityError("URDF root must be <robot>")
    link_names: list[str] = []
    seen_links: set[str] = set()
    for link in root.findall("link"):
        name = _required_attr(link, "name", context="link")
        if name in seen_links:
            raise RobotModelAuthorityError(f"duplicate URDF link: {name}")
        seen_links.add(name)
        link_names.append(name)

    joints: dict[str, RobotJoint] = {}
    for joint in root.findall("joint"):
        name = _required_attr(joint, "name", context="joint")
        if name.startswith("__rma_mimic_"):
            raise RobotModelAuthorityError(
                "joint names starting with __rma_mimic_ are reserved for "
                "private Robot Model Authority import views"
            )
        if name in joints:
            raise RobotModelAuthorityError(f"duplicate URDF joint: {name}")
        parent = joint.find("parent")
        child = joint.find("child")
        if parent is None or child is None:
            raise RobotModelAuthorityError(
                f"joint {name} must declare parent and child"
            )
        joints[name] = RobotJoint(
            name=name,
            joint_type=str(joint.attrib.get("type", "fixed") or "fixed").strip(),
            parent_link=_required_attr(parent, "link", context=f"joint {name} parent"),
            child_link=_required_attr(child, "link", context=f"joint {name} child"),
            axis_xyz=_parse_axis(joint),
            limit=_parse_limit(joint),
            mimic=_parse_mimic(joint),
        )

    for joint in joints.values():
        if joint.parent_link not in seen_links:
            raise RobotModelAuthorityError(
                f"joint {joint.name} references unknown parent link {joint.parent_link}"
            )
        if joint.child_link not in seen_links:
            raise RobotModelAuthorityError(
                f"joint {joint.name} references unknown child link {joint.child_link}"
            )

    _validate_mimic_graph(joints)
    masters = {joint.mimic.master for joint in joints.values() if joint.mimic}
    joints = {
        name: RobotJoint(
            name=joint.name,
            joint_type=joint.joint_type,
            parent_link=joint.parent_link,
            child_link=joint.child_link,
            axis_xyz=joint.axis_xyz,
            limit=joint.limit,
            mimic=joint.mimic,
            is_mimic_master=name in masters,
        )
        for name, joint in joints.items()
    }
    return tuple(link_names), joints


def _validate_mimic_graph(joints: Mapping[str, RobotJoint]) -> None:
    graph = {
        name: joint.mimic.master
        for name, joint in joints.items()
        if joint.mimic is not None
    }
    for child, master in graph.items():
        if master not in joints:
            raise RobotModelAuthorityError(
                f"mimic joint {child} references unknown master joint {master}"
            )
        if master == child:
            raise RobotModelAuthorityError(f"mimic joint {child} references itself")

    for start in graph:
        visiting: list[str] = []
        current = start
        while current in graph:
            if current in visiting:
                cycle = " -> ".join([*visiting, current])
                raise RobotModelAuthorityError(f"cyclic mimic graph: {cycle}")
            visiting.append(current)
            current = graph[current]


def _parse_srdf(
    root: ET.Element | None,
) -> tuple[
    dict[str, RobotGroup],
    tuple[RobotGroupState, ...],
    tuple[RobotEndEffector, ...],
    tuple[DisabledCollisionPair, ...],
]:
    if root is None:
        return {}, (), (), ()
    if root.tag != "robot":
        raise RobotModelAuthorityError("SRDF root must be <robot>")

    groups: dict[str, RobotGroup] = {}
    for group in root.findall("group"):
        name = _required_attr(group, "name", context="group")
        if name in groups:
            raise RobotModelAuthorityError(f"duplicate SRDF group: {name}")
        groups[name] = RobotGroup(
            name=name,
            joints=tuple(
                _required_attr(joint, "name", context=f"group {name} joint")
                for joint in group.findall("joint")
            ),
            links=tuple(
                _required_attr(link, "name", context=f"group {name} link")
                for link in group.findall("link")
            ),
            chains=tuple(
                (
                    _required_attr(chain, "base_link", context=f"group {name} chain"),
                    _required_attr(chain, "tip_link", context=f"group {name} chain"),
                )
                for chain in group.findall("chain")
            ),
        )

    states: list[RobotGroupState] = []
    for state in root.findall("group_state"):
        state_name = _required_attr(state, "name", context="group_state")
        group_name = _required_attr(state, "group", context=f"group_state {state_name}")
        positions: dict[str, float] = {}
        for joint in state.findall("joint"):
            joint_name = _required_attr(
                joint,
                "name",
                context=f"group_state {state_name} joint",
            )
            if joint_name in positions:
                raise RobotModelAuthorityError(
                    f"duplicate joint {joint_name} in group_state {state_name}"
                )
            value = _float_attr(
                joint,
                "value",
                context=f"group_state {state_name} joint {joint_name}",
            )
            if value is None:
                raise RobotModelAuthorityError(
                    f"group_state {state_name} joint {joint_name}.value is required"
                )
            positions[joint_name] = value
        states.append(
            RobotGroupState(
                name=state_name,
                group=group_name,
                joint_positions=positions,
            )
        )

    end_effectors = tuple(
        RobotEndEffector(
            name=_required_attr(end_effector, "name", context="end_effector"),
            group=_required_attr(end_effector, "group", context="end_effector"),
            parent_link=_required_attr(
                end_effector,
                "parent_link",
                context="end_effector",
            ),
            parent_group=str(end_effector.attrib.get("parent_group", "")).strip(),
        )
        for end_effector in root.findall("end_effector")
    )

    disabled = tuple(
        DisabledCollisionPair(
            link1=_required_attr(pair, "link1", context="disable_collisions"),
            link2=_required_attr(pair, "link2", context="disable_collisions"),
            reason=str(pair.attrib.get("reason", "")).strip(),
        )
        for pair in root.findall("disable_collisions")
    )
    return groups, tuple(states), end_effectors, disabled


def _validate_srdf_references(
    *,
    joints: Mapping[str, RobotJoint],
    links: Sequence[str],
    groups: Mapping[str, RobotGroup],
    group_states: Sequence[RobotGroupState],
    end_effectors: Sequence[RobotEndEffector],
    disabled_collisions: Sequence[DisabledCollisionPair],
) -> None:
    link_set = set(links)
    for group in groups.values():
        for joint_name in group.joints:
            if joint_name not in joints:
                raise RobotModelAuthorityError(
                    f"SRDF group {group.name} references unknown joint {joint_name}"
                )
        for link_name in group.links:
            if link_name not in link_set:
                raise RobotModelAuthorityError(
                    f"SRDF group {group.name} references unknown link {link_name}"
                )
        for base_link, tip_link in group.chains:
            if base_link not in link_set:
                raise RobotModelAuthorityError(
                    "SRDF group "
                    f"{group.name} chain references unknown base link {base_link}"
                )
            if tip_link not in link_set:
                raise RobotModelAuthorityError(
                    "SRDF group "
                    f"{group.name} chain references unknown tip link {tip_link}"
                )

    for state in group_states:
        if state.group not in groups:
            raise RobotModelAuthorityError(
                f"SRDF group_state {state.name} references unknown group {state.group}"
            )
        for joint_name in state.joint_positions:
            if joint_name not in joints:
                raise RobotModelAuthorityError(
                    "SRDF group_state "
                    f"{state.name} references unknown joint {joint_name}"
                )

    for end_effector in end_effectors:
        if end_effector.group not in groups:
            raise RobotModelAuthorityError(
                f"SRDF end_effector {end_effector.name} references unknown group "
                f"{end_effector.group}"
            )
        if end_effector.parent_link not in link_set:
            raise RobotModelAuthorityError(
                f"SRDF end_effector {end_effector.name} references unknown "
                f"parent_link {end_effector.parent_link}"
            )
        if end_effector.parent_group and end_effector.parent_group not in groups:
            raise RobotModelAuthorityError(
                f"SRDF end_effector {end_effector.name} references unknown "
                f"parent_group {end_effector.parent_group}"
            )

    for pair in disabled_collisions:
        if pair.link1 not in link_set:
            raise RobotModelAuthorityError(
                f"SRDF disable_collisions references unknown link {pair.link1}"
            )
        if pair.link2 not in link_set:
            raise RobotModelAuthorityError(
                f"SRDF disable_collisions references unknown link {pair.link2}"
            )


def _coerce_gripper_config(raw: Any) -> RobotModelGripperConfig:
    if isinstance(raw, RobotModelGripperConfig):
        return raw
    if isinstance(raw, Mapping):
        group_id = str(raw.get("group_id", "") or "").strip()
        joint_name = str(raw.get("joint_name", "") or "").strip()
        touch_links = tuple(str(link) for link in raw.get("touch_links", ()) or ())
        return RobotModelGripperConfig(
            group_id=group_id,
            joint_name=joint_name,
            touch_links=touch_links,
            mount_frame=str(raw.get("mount_frame", "") or ""),
            attach_frame=str(raw.get("attach_frame", "") or ""),
            tcp_frame=str(raw.get("tcp_frame", "") or ""),
            auxiliary_frames=tuple(
                str(frame) for frame in raw.get("auxiliary_frames", ()) or ()
            ),
        )
    group_id = str(getattr(raw, "group_id", "") or "").strip()
    joint_name = str(getattr(raw, "joint_name", "") or "").strip()
    touch_links = tuple(str(link) for link in getattr(raw, "touch_links", ()) or ())
    return RobotModelGripperConfig(
        group_id=group_id,
        joint_name=joint_name,
        touch_links=touch_links,
        mount_frame=str(getattr(raw, "mount_frame", "") or ""),
        attach_frame=str(getattr(raw, "attach_frame", "") or ""),
        tcp_frame=str(getattr(raw, "tcp_frame", "") or ""),
        auxiliary_frames=tuple(
            str(frame) for frame in getattr(raw, "auxiliary_frames", ()) or ()
        ),
    )


def _build_gripper_profiles(
    *,
    grippers: Mapping[str, Any] | None,
    joints: Mapping[str, RobotJoint],
    links: Sequence[str],
    groups: Mapping[str, RobotGroup],
    group_states: Sequence[RobotGroupState],
) -> dict[str, RobotModelGripperProfile]:
    profiles: dict[str, RobotModelGripperProfile] = {}
    link_set = set(links)
    for gripper_id, raw_config in (grippers or {}).items():
        config = _coerce_gripper_config(raw_config)
        if config.group_id not in groups:
            raise RobotModelAuthorityError(
                f"gripper {gripper_id} references unknown group {config.group_id}"
            )
        if config.joint_name not in joints:
            raise RobotModelAuthorityError(
                "gripper "
                f"{gripper_id} references unknown command joint {config.joint_name}"
            )
        command_joint = joints[config.joint_name]
        if not command_joint.is_commandable:
            raise RobotModelAuthorityError(
                "gripper "
                f"{gripper_id} command joint {config.joint_name} is not commandable"
            )
        for touch_link in config.touch_links:
            if touch_link not in link_set:
                raise RobotModelAuthorityError(
                    f"gripper {gripper_id} references unknown touch link "
                    f"{touch_link}"
                )
        for label, frame in (
            ("mount_frame", config.mount_frame),
            ("attach_frame", config.attach_frame),
            ("tcp_frame", config.tcp_frame),
            *(
                (f"auxiliary_frames[{index}]", frame)
                for index, frame in enumerate(config.auxiliary_frames)
            ),
        ):
            if frame and frame not in link_set:
                raise RobotModelAuthorityError(
                    f"gripper {gripper_id} {label} references unknown frame {frame}"
                )
        group = groups[config.group_id]
        derived = tuple(
            joint_name
            for joint_name in group.joints
            if joints.get(joint_name) is not None
            and joints[joint_name].mimic is not None
            and joints[joint_name].mimic.master == config.joint_name
        )
        presets: dict[str, float] = {}
        for state in group_states:
            if state.group != config.group_id:
                continue
            if config.joint_name in state.joint_positions:
                presets[state.name] = state.joint_positions[config.joint_name]
        profiles[str(gripper_id)] = RobotModelGripperProfile(
            gripper_id=str(gripper_id),
            group_id=config.group_id,
            command_joint=config.joint_name,
            derived_joints=derived,
            touch_links=config.touch_links,
            mount_frame=config.mount_frame,
            attach_frame=config.attach_frame,
            tcp_frame=config.tcp_frame,
            auxiliary_frames=config.auxiliary_frames,
            presets=presets,
        )
    return profiles


def _fingerprint(
    *,
    urdf_sha256: str,
    srdf_sha256: str,
    package_paths: Sequence[str],
    joints: Mapping[str, RobotJoint],
    groups: Mapping[str, RobotGroup],
    end_effectors: Sequence[RobotEndEffector] = (),
) -> str:
    payload = {
        "schema": "manyforge.robot_model_authority.v1",
        "urdf_sha256": urdf_sha256,
        "srdf_sha256": srdf_sha256,
        "package_paths": list(package_paths),
        "joints": [
            {
                "name": joint.name,
                "type": joint.joint_type,
                "mimic": (
                    {
                        "master": joint.mimic.master,
                        "multiplier": joint.mimic.multiplier,
                        "offset": joint.mimic.offset,
                    }
                    if joint.mimic
                    else None
                ),
            }
            for joint in joints.values()
        ],
        "groups": {
            name: {
                "joints": list(group.joints),
                "links": list(group.links),
                "chains": [list(chain) for chain in group.chains],
            }
            for name, group in groups.items()
        },
        "end_effectors": [
            {
                "name": end_effector.name,
                "group": end_effector.group,
                "parent_link": end_effector.parent_link,
                "parent_group": end_effector.parent_group,
            }
            for end_effector in end_effectors
        ],
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def _descriptor_asset_id(authority: RobotModelAuthority, asset_id: str | None) -> str:
    value = str(asset_id or authority.model_name or "").strip()
    if not value:
        raise RobotModelAuthorityError("robot model asset_id is required")
    return value


def _frame_names(authority: RobotModelAuthority) -> tuple[str, ...]:
    frames: set[str] = set(authority.links)
    for group in authority.groups.values():
        for base_link, tip_link in group.chains:
            frames.add(base_link)
            frames.add(tip_link)
        frames.update(group.links)
    for end_effector in authority.end_effectors:
        frames.add(end_effector.parent_link)
    for gripper in authority.grippers.values():
        frames.update(
            frame
            for frame in (
                gripper.mount_frame,
                gripper.attach_frame,
                gripper.tcp_frame,
                *gripper.auxiliary_frames,
            )
            if frame
        )
    return tuple(sorted(frames))


def robot_model_asset_descriptor(
    authority: RobotModelAuthority,
    *,
    asset_id: str | None = None,
) -> RobotModelAssetDescriptor:
    """Build a stable source-asset descriptor from canonical authority data."""

    resolved_asset_id = _descriptor_asset_id(authority, asset_id)
    return RobotModelAssetDescriptor(
        asset_id=resolved_asset_id,
        model_name=authority.model_name,
        canonical_fingerprint=authority.canonical_fingerprint,
        urdf_sha256=authority.urdf_sha256,
        srdf_sha256=authority.srdf_sha256,
        urdf_path=authority.urdf_path,
        srdf_path=authority.srdf_path,
        package_paths=authority.package_paths,
        links=authority.links,
        joints=tuple(authority.joints.keys()),
        frames=_frame_names(authority),
        groups=tuple(authority.groups.keys()),
        group_states=tuple(
            (state.group, state.name) for state in authority.group_states
        ),
        end_effectors=tuple(
            end_effector.name for end_effector in authority.end_effectors
        ),
        disabled_collision_pairs=tuple(
            (pair.link1, pair.link2) for pair in authority.disabled_collisions
        ),
        grippers=tuple(authority.grippers.keys()),
        commandable_joints=authority.commandable_joint_names,
        derived_joints=authority.derived_joint_names,
    )


def _validate_generated_prefix(prefix: str) -> str:
    value = str(prefix or "").strip()
    if not value:
        raise RobotModelAuthorityError("robot model instance prefix is required")
    if value.startswith("__rma_mimic_"):
        raise RobotModelAuthorityError(
            "robot model instance prefixes starting with __rma_mimic_ are reserved"
        )
    return value


def _validate_mount_tuple(
    values: Sequence[float],
    *,
    expected: int,
    label: str,
) -> tuple[float, ...]:
    if len(values) != expected:
        raise RobotModelAuthorityError(f"{label} must have {expected} values")
    normalized = tuple(float(value) for value in values)
    if not all(math.isfinite(value) for value in normalized):
        raise RobotModelAuthorityError(f"{label} values must be finite")
    return normalized


def _name_map(names: Sequence[str], prefix: str) -> RobotModelNameMap:
    canonical_to_generated = {name: f"{prefix}{name}" for name in names}
    generated_to_canonical: dict[str, str] = {}
    for canonical, generated in canonical_to_generated.items():
        if generated in generated_to_canonical:
            raise RobotModelAuthorityError(
                f"generated name collision for {generated}"
            )
        generated_to_canonical[generated] = canonical
    return RobotModelNameMap(
        canonical_to_generated=canonical_to_generated,
        generated_to_canonical=generated_to_canonical,
    )


def _group_state_key(group: str, name: str) -> str:
    return f"{group}/{name}"


def _map_optional_frame(frames: RobotModelNameMap, frame: str) -> str:
    if not frame:
        return ""
    return frames.canonical_to_generated.get(frame, frame)


def _instance_fingerprint(
    *,
    asset: RobotModelAssetDescriptor,
    spec: RobotModelInstanceSpec,
    link_map: RobotModelNameMap,
    joint_map: RobotModelNameMap,
    group_map: RobotModelNameMap,
    group_state_map: RobotModelNameMap,
) -> str:
    payload = {
        "schema": "manyforge.robot_model_instance_view.v1",
        "asset_id": asset.asset_id,
        "asset_fingerprint": asset.canonical_fingerprint,
        "instance_id": spec.instance_id,
        "prefix": spec.prefix,
        "mount_parent_frame": spec.mount_parent_frame,
        "mount_xyz": list(spec.mount_xyz),
        "mount_xyzw": list(spec.mount_xyzw),
        "controller_bindings": dict(sorted(spec.controller_bindings.items())),
        "state_sources": dict(sorted(spec.state_sources.items())),
        "links": dict(sorted(link_map.canonical_to_generated.items())),
        "joints": dict(sorted(joint_map.canonical_to_generated.items())),
        "groups": dict(sorted(group_map.canonical_to_generated.items())),
        "group_states": dict(sorted(group_state_map.canonical_to_generated.items())),
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def robot_model_instance_view(
    authority: RobotModelAuthority,
    *,
    instance_id: str,
    prefix: str,
    asset_id: str | None = None,
    mount_parent_frame: str = "",
    mount_xyz: Sequence[float] = (0.0, 0.0, 0.0),
    mount_xyzw: Sequence[float] = (0.0, 0.0, 0.0, 1.0),
    controller_bindings: Mapping[str, str] | None = None,
    state_sources: Mapping[str, str] | None = None,
) -> RobotModelInstanceView:
    """Build the deterministic generated-name view for one robot instance."""

    clean_instance_id = str(instance_id or "").strip()
    if not clean_instance_id:
        raise RobotModelAuthorityError("robot model instance_id is required")
    clean_prefix = _validate_generated_prefix(prefix)
    clean_mount_xyz = _validate_mount_tuple(
        mount_xyz,
        expected=3,
        label="mount_xyz",
    )
    clean_mount_xyzw = _validate_mount_tuple(
        mount_xyzw,
        expected=4,
        label="mount_xyzw",
    )
    spec = RobotModelInstanceSpec(
        instance_id=clean_instance_id,
        prefix=clean_prefix,
        mount_parent_frame=str(mount_parent_frame or ""),
        mount_xyz=(
            clean_mount_xyz[0],
            clean_mount_xyz[1],
            clean_mount_xyz[2],
        ),
        mount_xyzw=(
            clean_mount_xyzw[0],
            clean_mount_xyzw[1],
            clean_mount_xyzw[2],
            clean_mount_xyzw[3],
        ),
        controller_bindings=dict(controller_bindings or {}),
        state_sources=dict(state_sources or {}),
    )
    asset = robot_model_asset_descriptor(authority, asset_id=asset_id)
    link_map = _name_map(asset.links, clean_prefix)
    joint_map = _name_map(asset.joints, clean_prefix)
    frame_map = _name_map(asset.frames, clean_prefix)
    group_map = _name_map(asset.groups, clean_prefix)
    group_state_names = tuple(
        _group_state_key(group, name) for group, name in asset.group_states
    )
    group_state_generated = {
        _group_state_key(group, name): _group_state_key(
            group_map.generated(group),
            f"{clean_prefix}{name}",
        )
        for group, name in asset.group_states
    }
    group_state_map = RobotModelNameMap(
        canonical_to_generated=group_state_generated,
        generated_to_canonical={
            generated: canonical
            for canonical, generated in group_state_generated.items()
        },
    )
    if set(group_state_map.canonical_to_generated) != set(group_state_names):
        raise RobotModelAuthorityError("group_state map construction failed")
    end_effector_map = _name_map(asset.end_effectors, clean_prefix)

    generated_mimics = tuple(
        GeneratedMimicLaw(
            source_joint=joint.name,
            generated_joint=joint_map.generated(joint.name),
            source_master=joint.mimic.master,
            generated_master=joint_map.generated(joint.mimic.master),
            multiplier=joint.mimic.multiplier,
            offset=joint.mimic.offset,
        )
        for joint in authority.joints.values()
        if joint.mimic is not None
    )
    disabled_pairs = tuple(
        GeneratedDisabledCollisionPair(
            source_link1=pair.link1,
            source_link2=pair.link2,
            generated_link1=link_map.generated(pair.link1),
            generated_link2=link_map.generated(pair.link2),
            reason=pair.reason,
        )
        for pair in authority.disabled_collisions
    )
    generated_grippers = {
        gripper_id: GeneratedGripperProfile(
            source_gripper_id=gripper_id,
            generated_gripper_id=f"{clean_prefix}{gripper_id}",
            source_group_id=profile.group_id,
            generated_group_id=group_map.generated(profile.group_id),
            source_command_joint=profile.command_joint,
            generated_command_joint=joint_map.generated(profile.command_joint),
            source_derived_joints=profile.derived_joints,
            generated_derived_joints=tuple(
                joint_map.generated(joint) for joint in profile.derived_joints
            ),
            source_touch_links=profile.touch_links,
            generated_touch_links=tuple(
                link_map.generated(link) for link in profile.touch_links
            ),
            source_mount_frame=profile.mount_frame,
            generated_mount_frame=_map_optional_frame(frame_map, profile.mount_frame),
            source_attach_frame=profile.attach_frame,
            generated_attach_frame=_map_optional_frame(frame_map, profile.attach_frame),
            source_tcp_frame=profile.tcp_frame,
            generated_tcp_frame=_map_optional_frame(frame_map, profile.tcp_frame),
            source_auxiliary_frames=profile.auxiliary_frames,
            generated_auxiliary_frames=tuple(
                _map_optional_frame(frame_map, frame)
                for frame in profile.auxiliary_frames
            ),
            presets=dict(profile.presets),
        )
        for gripper_id, profile in authority.grippers.items()
    }
    traceability = tuple(
        RobotModelTraceabilityRecord(
            kind=kind,
            source_asset_id=asset.asset_id,
            source_fingerprint=asset.canonical_fingerprint,
            instance_id=clean_instance_id,
            source_name=source,
            generated_name=generated,
            mount_parent_frame=spec.mount_parent_frame,
            mount_xyz=spec.mount_xyz,
            mount_xyzw=spec.mount_xyzw,
        )
        for kind, mapping in (
            ("link", link_map.canonical_to_generated),
            ("joint", joint_map.canonical_to_generated),
            ("frame", frame_map.canonical_to_generated),
            ("group", group_map.canonical_to_generated),
            ("group_state", group_state_map.canonical_to_generated),
            ("end_effector", end_effector_map.canonical_to_generated),
        )
        for source, generated in sorted(mapping.items())
    )
    instance_fingerprint = _instance_fingerprint(
        asset=asset,
        spec=spec,
        link_map=link_map,
        joint_map=joint_map,
        group_map=group_map,
        group_state_map=group_state_map,
    )
    return RobotModelInstanceView(
        asset=asset,
        spec=spec,
        instance_fingerprint=instance_fingerprint,
        links=link_map,
        joints=joint_map,
        frames=frame_map,
        groups=group_map,
        group_states=group_state_map,
        end_effectors=end_effector_map,
        generated_mimic_laws=generated_mimics,
        disabled_collision_pairs=disabled_pairs,
        grippers=generated_grippers,
        traceability=traceability,
    )


def _safe_joint_suffix(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_")
    return cleaned or "joint"


def _mimic_components_requiring_import_rename(
    joints: Mapping[str, RobotJoint],
) -> list[list[str]]:
    graph = {
        name: joint.mimic.master
        for name, joint in joints.items()
        if joint.mimic is not None
    }
    undirected: dict[str, set[str]] = {}
    for child, master in graph.items():
        undirected.setdefault(child, set()).add(master)
        undirected.setdefault(master, set()).add(child)

    components: list[list[str]] = []
    visited: set[str] = set()
    for start in sorted(undirected):
        if start in visited:
            continue
        stack = [start]
        component: set[str] = set()
        while stack:
            current = stack.pop()
            if current in component:
                continue
            component.add(current)
            stack.extend(sorted(undirected.get(current, ()), reverse=True))
        visited.update(component)
        requires_rename = any(
            child in component and master in component and child < master
            for child, master in graph.items()
        )
        if requires_rename and any(name in component for name in graph):
            components.append(_topological_mimic_component_order(component, graph))
    return components


def _topological_mimic_component_order(
    component: set[str],
    graph: Mapping[str, str],
) -> list[str]:
    children_by_master: dict[str, list[str]] = {}
    for child, master in graph.items():
        if child in component and master in component:
            children_by_master.setdefault(master, []).append(child)
    for children in children_by_master.values():
        children.sort()
    roots = sorted(name for name in component if name not in graph)
    ordered: list[str] = []

    def visit(name: str) -> None:
        if name in ordered:
            return
        ordered.append(name)
        for child in children_by_master.get(name, ()):
            visit(child)

    for root in roots:
        visit(root)
    for name in sorted(component):
        visit(name)
    return ordered


def _build_import_joint_name_map(
    joints: Mapping[str, RobotJoint],
) -> dict[str, str]:
    mapping = {name: name for name in joints}
    counter = 0
    for component in _mimic_components_requiring_import_rename(joints):
        width = max(4, len(str(counter + len(component))))
        for joint_name in component:
            mapping[joint_name] = (
                f"__rma_mimic_{counter:0{width}d}_{_safe_joint_suffix(joint_name)}"
            )
            counter += 1
    return mapping


def _rewrite_joint_names_for_import_view(
    root: ET.Element,
    mapping: Mapping[str, str],
) -> None:
    for element in root.iter():
        if element.tag in {"joint", "passive_joint"}:
            name = element.attrib.get("name")
            if name in mapping:
                element.set("name", mapping[name])
        for attr_name in ("joint",):
            value = element.attrib.get(attr_name)
            if value in mapping:
                element.set(attr_name, mapping[value])


def _xml_to_string(root: ET.Element) -> str:
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode") + "\n"


def _import_view_fingerprint(
    *,
    consumer: str,
    canonical_fingerprint: str,
    urdf_xml: str,
    srdf_xml: str,
    mapping: Mapping[str, str],
) -> str:
    payload = {
        "schema": "manyforge.robot_model_import_view.v1",
        "consumer": consumer,
        "canonical_fingerprint": canonical_fingerprint,
        "urdf_sha256": _sha256(urdf_xml.encode("utf-8")),
        "srdf_sha256": _sha256(srdf_xml.encode("utf-8")),
        "joint_name_map": dict(sorted(mapping.items())),
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def build_roboplan_import_view(authority: RobotModelAuthority) -> RobotModelImportView:
    """Build a private RoboPlan/Pinocchio-safe model view.

    The generated view keeps canonical source files untouched. It renames only
    mimic components whose canonical joint names can make name-ordered URDF
    importers visit a dependent mimic before its master.
    """

    urdf_data = _read_bytes(Path(authority.urdf_path), label="URDF")
    srdf_data = (
        _read_bytes(Path(authority.srdf_path), label="SRDF")
        if authority.srdf_path
        else b"<robot name=\"empty\" />"
    )
    urdf_root = _parse_xml(urdf_data, label="URDF")
    srdf_root = _parse_xml(srdf_data, label="SRDF")
    mapping = _build_import_joint_name_map(authority.joints)
    if any(canonical != view for canonical, view in mapping.items()):
        _rewrite_joint_names_for_import_view(urdf_root, mapping)
        _rewrite_joint_names_for_import_view(srdf_root, mapping)
    urdf_xml = _xml_to_string(urdf_root)
    srdf_xml = _xml_to_string(srdf_root)
    view_fingerprint = _import_view_fingerprint(
        consumer="roboplan_pinocchio",
        canonical_fingerprint=authority.canonical_fingerprint,
        urdf_xml=urdf_xml,
        srdf_xml=srdf_xml,
        mapping=mapping,
    )
    return RobotModelImportView(
        consumer="roboplan_pinocchio",
        canonical_fingerprint=authority.canonical_fingerprint,
        view_fingerprint=view_fingerprint,
        urdf_xml=urdf_xml,
        srdf_xml=srdf_xml,
        canonical_to_view_joint_names=mapping,
        view_to_canonical_joint_names={
            view: canonical for canonical, view in mapping.items()
        },
    )


def load_robot_model_authority(
    *,
    urdf_path: str | Path,
    srdf_path: str | Path | None = None,
    package_paths: Sequence[str] | None = None,
    grippers: Mapping[str, Any] | None = None,
    model_name: str | None = None,
) -> RobotModelAuthority:
    """Load and validate canonical robot-model metadata.

    Paths should already be resolved to filesystem paths by the deployment path
    resolver. Package paths are carried into the fingerprint and later consumer
    views, but this parser does not resolve mesh URIs.
    """

    urdf = Path(urdf_path).resolve()
    srdf = Path(srdf_path).resolve() if srdf_path else None
    normalized_package_paths = tuple(
        str(Path(path).resolve()) for path in package_paths or ()
    )

    urdf_data = _read_bytes(urdf, label="URDF")
    srdf_data = b""
    if srdf is not None:
        srdf_data = _read_bytes(srdf, label="SRDF")

    urdf_root = _parse_xml(urdf_data, label="URDF")
    srdf_root = _parse_xml(srdf_data, label="SRDF") if srdf_data else None
    links, joints = _parse_urdf(urdf_root)
    groups, group_states, end_effectors, disabled_collisions = _parse_srdf(srdf_root)
    _validate_srdf_references(
        joints=joints,
        links=links,
        groups=groups,
        group_states=group_states,
        end_effectors=end_effectors,
        disabled_collisions=disabled_collisions,
    )
    gripper_profiles = _build_gripper_profiles(
        grippers=grippers,
        joints=joints,
        links=links,
        groups=groups,
        group_states=group_states,
    )
    urdf_digest = _sha256(urdf_data)
    srdf_digest = _sha256(srdf_data)
    return RobotModelAuthority(
        model_name=model_name or urdf.stem or "robot_model",
        urdf_path=str(urdf),
        srdf_path=str(srdf) if srdf is not None else "",
        package_paths=normalized_package_paths,
        urdf_sha256=urdf_digest,
        srdf_sha256=srdf_digest,
        canonical_fingerprint=_fingerprint(
            urdf_sha256=urdf_digest,
            srdf_sha256=srdf_digest,
            package_paths=normalized_package_paths,
            joints=joints,
            groups=groups,
            end_effectors=end_effectors,
        ),
        links=links,
        joints=joints,
        groups=groups,
        group_states=group_states,
        end_effectors=end_effectors,
        disabled_collisions=disabled_collisions,
        grippers=gripper_profiles,
    )
