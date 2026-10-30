#!/usr/bin/env python3
"""Cut a combined arm+tool URDF/SRDF at its mount joint (merge 6, slice A).

    python3 scripts/models/cut_combined_model.py \
        --urdf <combined>.urdf --srdf <combined>.srdf --mount-joint gripper_fix \
        --arm-name lite6 --arm-out manyforge_example_models/models/lite6/lite6 \
        --tool-name lite_gripper \
        --tool-out manyforge_example_models/models/lite_gripper/lite_gripper

The tool is every link below the mount joint. The arm bundle keeps every other
top-level element; the tool bundle gets a new root link `<tool>_mount` (the
robot flange plane) and the mount joint itself, re-parented onto that link with
its original origin. A robot `flange` attachment with an identity pose therefore
puts every tool link exactly where the combined model had it.

Elements are copied as their original text (indentation and all), so geometry,
origins and the vendor's formatting stay byte-identical; only the `<robot name>`
and the mount joint's `<parent link>` change. Comments between top-level
elements are not copied. SRDF rules:
- a group goes to the arm when it has an arm member, otherwise to the tool; its
  members from the other half are dropped;
- group states follow their group; passive joints, `<gripper>` (HPP) and
  `disable_default_collisions` follow their joint or link;
- `disable_collisions` and `end_effector` are kept only when both ends are in
  one half. Pairs across the mount are left to Apply, which disables collisions
  between the two rigid groups that meet there.

`--check` writes nothing and exits 1 when an output differs.
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


class CutError(ValueError):
    """The combined model cannot be cut as asked."""


@dataclass(frozen=True)
class _Element:
    indent: str
    text: str
    node: ET.Element


@dataclass(frozen=True)
class _Document:
    prolog: str
    robot_open: str
    elements: tuple[_Element, ...]
    indent: str


@dataclass(frozen=True)
class CutModel:
    arm_urdf: str
    arm_srdf: str
    tool_urdf: str
    tool_srdf: str


_ROBOT_OPEN = re.compile(r"<robot\b[^>]*>")
_ROBOT_NAME = re.compile(r'(<robot\b[^>]*?\bname=")[^"]*(")')


def _element_end(text: str, start: int) -> int:
    """Return the offset just past the element that starts at `start`."""

    depth = 0
    position = start
    while True:
        if text.startswith("<!--", position):
            close = text.find("-->", position)
            if close < 0:
                raise CutError("unterminated comment")
            position = close + 3
            continue
        tag_start = text.find("<", position)
        if tag_start < 0:
            raise CutError("unterminated element")
        if text.startswith("<!--", tag_start):
            position = tag_start
            continue
        tag_end = text.find(">", tag_start)
        if tag_end < 0:
            raise CutError("unterminated tag")
        tag = text[tag_start : tag_end + 1]
        if tag.startswith("</"):
            depth -= 1
        elif not tag.endswith("/>"):
            depth += 1
        position = tag_end + 1
        if depth == 0:
            return position


def _parse(text: str, kind: str) -> _Document:
    match = _ROBOT_OPEN.search(text)
    if match is None:
        raise CutError(f"{kind} has no <robot> element")
    close = text.rfind("</robot>")
    if close < match.end():
        raise CutError(f"{kind} <robot> is not closed")
    body = text[match.end() : close]
    elements: list[_Element] = []
    indents: set[str] = set()
    position = 0
    while position < len(body):
        start = body.find("<", position)
        if start < 0:
            if body[position:].strip():
                raise CutError(f"{kind} has character data outside elements")
            break
        if body[position:start].strip():
            raise CutError(f"{kind} has character data outside elements")
        if body.startswith("<!--", start):
            end = body.find("-->", start)
            if end < 0:
                raise CutError(f"{kind} has an unterminated comment")
            position = end + 3
            continue
        end = _element_end(body, start)
        line_start = body.rfind("\n", 0, start) + 1
        indent = body[line_start:start]
        if indent.strip():
            indent = "  "
        indents.add(indent)
        element_text = body[start:end]
        elements.append(_Element(indent, element_text, ET.fromstring(element_text)))
        position = end
    return _Document(
        prolog=text[: match.start()],
        robot_open=match.group(0),
        elements=tuple(elements),
        indent=min(indents, key=len) if indents else "  ",
    )


def _render(document: _Document, name: str, elements: list[tuple[str, str]]) -> str:
    robot_open, count = _ROBOT_NAME.subn(rf"\g<1>{name}\g<2>", document.robot_open)
    if count != 1:
        raise CutError("<robot> needs exactly one name attribute")
    lines = [document.prolog + robot_open]
    lines.extend(indent + text for indent, text in elements)
    lines.append("</robot>")
    return "\n".join(lines) + "\n"


def _names(node: ET.Element) -> set[str]:
    """Every attribute value and text token in a subtree."""

    found: set[str] = set()
    for element in node.iter():
        found.update(element.attrib.values())
        if element.text and element.text.strip():
            found.add(element.text.strip())
    return found


def _material_names(text: str) -> set[str]:
    return set(re.findall(r'<material\s+name="([^"]+)"', text))


def _cut_urdf(
    text: str, *, mount_joint: str, arm_name: str, tool_name: str
) -> tuple[str, str, set[str], set[str]]:
    document = _parse(text, "URDF")
    joints = {
        element.node.get("name"): element.node
        for element in document.elements
        if element.node.tag == "joint"
    }
    links = {
        element.node.get("name")
        for element in document.elements
        if element.node.tag == "link"
    }
    if mount_joint not in joints:
        raise CutError(f"URDF has no joint {mount_joint!r}")
    mount = joints[mount_joint]
    if mount.get("type") != "fixed":
        raise CutError(f"mount joint {mount_joint!r} must be fixed")
    children: dict[str, list[str]] = {}
    for joint in joints.values():
        children.setdefault(joint.find("parent").get("link"), []).append(
            joint.find("child").get("link")
        )
    tool_links: set[str] = set()
    pending = [mount.find("child").get("link")]
    while pending:
        link = pending.pop()
        if link in tool_links:
            continue
        tool_links.add(link)
        pending.extend(children.get(link, []))
    arm_links = links - tool_links
    tool_joints = {
        name for name, joint in joints.items()
        if joint.find("child").get("link") in tool_links
    }
    arm_joints = set(joints) - tool_joints
    mount_link = f"{tool_name}_mount"
    if mount_link in links:
        raise CutError(f"URDF already has a link {mount_link!r}")
    arm_names = arm_links | arm_joints
    tool_names = tool_links | tool_joints

    arm: list[tuple[str, str]] = []
    tool: list[tuple[str, str]] = []
    materials: list[_Element] = []
    for element in document.elements:
        node = element.node
        if node.tag == "material":
            materials.append(element)
            continue
        if node.tag == "link":
            side = tool if node.get("name") in tool_links else arm
        elif node.tag == "joint":
            side = tool if node.get("name") in tool_joints else arm
        else:
            mentioned = _names(node)
            in_arm, in_tool = bool(mentioned & arm_names), bool(mentioned & tool_names)
            if in_arm and in_tool:
                raise CutError(
                    f"URDF <{node.tag} {node.attrib}> names links or joints on "
                    "both sides of the mount"
                )
            side = tool if in_tool else arm
        element_text = element.text
        if node.tag == "joint" and node.get("name") == mount_joint:
            parent = mount.find("parent").get("link")
            element_text, count = re.subn(
                rf'(<parent\s+link=")({re.escape(parent)})(")',
                rf"\g<1>{mount_link}\g<3>",
                element_text,
            )
            if count != 1:
                raise CutError("mount joint needs exactly one <parent link>")
            tool.append(
                (
                    element.indent,
                    f"<!-- The robot flange plane: cut at {mount_joint} from "
                    f"the combined model; the robot's `flange` attachment puts "
                    f"this link on {parent}. -->",
                )
            )
            tool.append((element.indent, f'<link name="{mount_link}"/>'))
        side.append((element.indent, element_text))

    def with_materials(kept: list[tuple[str, str]]) -> list[tuple[str, str]]:
        used = set().union(*(_material_names(text) for _, text in kept)) if kept else set()
        return [
            (element.indent, element.text)
            for element in materials
            if element.node.get("name") in used
        ] + kept

    return (
        _render(document, arm_name, with_materials(arm)),
        _render(document, tool_name, with_materials(tool)),
        arm_names,
        tool_names | {mount_link},
    )


def _cut_srdf(
    text: str,
    *,
    arm_name: str,
    tool_name: str,
    arm_names: set[str],
    tool_names: set[str],
) -> tuple[str, str]:
    document = _parse(text, "SRDF")

    def half(name: str | None) -> str:
        if name in tool_names:
            return "tool"
        if name in arm_names:
            return "arm"
        raise CutError(f"SRDF names {name!r}, which is not in the URDF")

    groups: dict[str, str] = {}
    kept: dict[str, list[tuple[str, str]]] = {"arm": [], "tool": []}
    for element in document.elements:
        node = element.node
        element_text = element.text
        if node.tag == "group":
            members: list[tuple[ET.Element, str]] = []
            for member in node:
                if member.tag == "chain":
                    base, tip = half(member.get("base_link")), half(member.get("tip_link"))
                    if base != tip:
                        raise CutError(f"SRDF chain of group {node.get('name')} crosses the mount")
                    members.append((member, base))
                elif member.tag == "group":
                    members.append((member, groups[member.get("name")]))
                else:
                    members.append((member, half(member.get("name"))))
            side = "arm" if any(side == "arm" for _, side in members) else "tool"
            for member, member_side in members:
                if member_side == side:
                    continue
                line = re.compile(
                    rf'\n[ \t]*<{member.tag}\s+name="{re.escape(member.get("name"))}"\s*/>'
                )
                element_text, count = line.subn("", element_text)
                if count != 1:
                    raise CutError(
                        f"SRDF group {node.get('name')} member {member.get('name')} "
                        "is not on its own line"
                    )
            groups[node.get("name")] = side
        elif node.tag == "group_state":
            side = groups[node.get("group")]
        elif node.tag in ("passive_joint", "disable_default_collisions"):
            side = half(node.get("name") or node.get("link"))
        elif node.tag == "gripper":
            sides = {half(link.get("name")) for link in node.iter("link")}
            if len(sides) != 1:
                raise CutError("SRDF <gripper> must name links of one half")
            side = sides.pop()
        elif node.tag == "disable_collisions":
            sides = {half(node.get("link1")), half(node.get("link2"))}
            if len(sides) != 1:
                continue
            side = sides.pop()
        elif node.tag == "end_effector":
            sides = {half(node.get("parent_link")), groups[node.get("group")]}
            if len(sides) != 1:
                continue
            side = sides.pop()
        else:
            raise CutError(f"SRDF top-level <{node.tag}> has no cut rule")
        kept[side].append((element.indent, element_text))
    return (
        _render(document, arm_name, kept["arm"]),
        _render(document, tool_name, kept["tool"]),
    )


def cut_combined_model(
    urdf: str, srdf: str, *, mount_joint: str, arm_name: str, tool_name: str
) -> CutModel:
    """Return the arm and tool URDF/SRDF texts of one combined model."""

    arm_urdf, tool_urdf, arm_names, tool_names = _cut_urdf(
        urdf, mount_joint=mount_joint, arm_name=arm_name, tool_name=tool_name
    )
    arm_srdf, tool_srdf = _cut_srdf(
        srdf,
        arm_name=arm_name,
        tool_name=tool_name,
        arm_names=arm_names,
        tool_names=tool_names,
    )
    return CutModel(arm_urdf, arm_srdf, tool_urdf, tool_srdf)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--urdf", type=Path, required=True)
    parser.add_argument("--srdf", type=Path, required=True)
    parser.add_argument("--mount-joint", required=True)
    parser.add_argument("--arm-name", required=True)
    parser.add_argument("--tool-name", required=True)
    parser.add_argument("--arm-out", type=Path, required=True,
                        help="output path without suffix; .urdf and .srdf are added")
    parser.add_argument("--tool-out", type=Path, required=True,
                        help="output path without suffix; .urdf and .srdf are added")
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        model = cut_combined_model(
            arguments.urdf.read_text(encoding="utf-8"),
            arguments.srdf.read_text(encoding="utf-8"),
            mount_joint=arguments.mount_joint,
            arm_name=arguments.arm_name,
            tool_name=arguments.tool_name,
        )
    except (OSError, CutError, ET.ParseError) as error:
        print(f"cut_combined_model: {error}", file=sys.stderr)
        return 2
    outputs = {
        arguments.arm_out.with_name(arguments.arm_out.name + ".urdf"): model.arm_urdf,
        arguments.arm_out.with_name(arguments.arm_out.name + ".srdf"): model.arm_srdf,
        arguments.tool_out.with_name(arguments.tool_out.name + ".urdf"): model.tool_urdf,
        arguments.tool_out.with_name(arguments.tool_out.name + ".srdf"): model.tool_srdf,
    }
    stale = [
        path for path, text in outputs.items()
        if not path.is_file() or path.read_text(encoding="utf-8") != text
    ]
    if arguments.check:
        for path in stale:
            print(f"cut_combined_model: stale {path}", file=sys.stderr)
        return 1 if stale else 0
    for path in stale:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(outputs[path], encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
