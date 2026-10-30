#!/usr/bin/env python3
"""Mock lane controller rows as data (E1, robots as data).

Reads `manyforge_ros_bringup/config/mock_controller_profiles.yaml`, validates
every row, and (with `--output`) writes the C++ row initializers the host's
controller launch artifacts embed at build time. The same row parser backs
`manyforge component check` and `register` (a robot descriptor's
`lanes.mock.controllers`), so the build and the offline check apply one rule
set. The build runs this file as a script.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

DEFAULT_PATH = "manyforge_ros_bringup/config/mock_controller_profiles.yaml"

# Closed list in code: the controller template each name selects
# (manyforge_roboplan/src/controller_launch_artifacts.cpp `ArmTemplate`).
TEMPLATES = {"ufactory": "ArmTemplate::kUFactory", "ur": "ArmTemplate::kUr"}

_REQUIRED = (
    "mock_profile",
    "template",
    "hardware_component_name",
    "hard_stop_completion_timeout_s",
    "gripper_result_timeout_s",
)
# A robot row has an arm controller and its arrival tolerance; a tool row can
# own only a gripper.
_OPTIONAL = (
    "arm_controller",
    "vendor_joint_constraints",
    "gripper_controller",
    "arrival_tolerance_rad",
)
# One bounded ROS name token, as the host requires for every generated name.
_ROS_TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,254}")
_PORTABLE_ID = re.compile(r"[a-z][a-z0-9_]{0,127}")
_CONSTRAINT = re.compile(r"[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?")


class MockControllerProfileError(ValueError):
    """A Mock controller row is invalid; the message names the field."""


@dataclass(frozen=True)
class MockControllerProfile:
    mock_profile: str
    template: str
    hardware_component_name: str
    arm_controller: str | None
    vendor_joint_constraints: tuple[str] | None
    gripper_controller: str | None
    hard_stop_completion_timeout_s: float
    gripper_result_timeout_s: float
    arrival_tolerance_rad: float | None


def _constraint_text(value: Any, where: str) -> str:
    # Copied text, not a computed number: 1.0 stays "1.0" in the template.
    if isinstance(value, bool) or not isinstance(value, (str, int, float)):
        raise MockControllerProfileError(f"{where} must be a number")
    text = value if isinstance(value, str) else repr(value)
    if not _CONSTRAINT.fullmatch(text) or not math.isfinite(float(text)):
        raise MockControllerProfileError(
            f"{where} must be a finite non-negative decimal number, got {text!r}"
        )
    return text


def parse_row(raw: Any, where: str) -> MockControllerProfile:
    """One validated row; raise MockControllerProfileError naming the field."""
    if not isinstance(raw, dict):
        raise MockControllerProfileError(f"{where} must be a mapping")
    keys = [key for key in raw if not isinstance(key, str)]
    if keys:
        raise MockControllerProfileError(f"{where} has non-text field names {keys!r}")
    missing = [key for key in _REQUIRED if key not in raw]
    if missing:
        raise MockControllerProfileError(f"{where} is missing {', '.join(missing)}")
    unknown = sorted(set(raw) - set(_REQUIRED) - set(_OPTIONAL))
    if unknown:
        raise MockControllerProfileError(
            f"{where} has unknown fields {', '.join(unknown)} "
            f"(allowed: {', '.join(_REQUIRED + _OPTIONAL)})"
        )
    profile = raw["mock_profile"]
    if not isinstance(profile, str) or not _PORTABLE_ID.fullmatch(profile):
        raise MockControllerProfileError(
            f"{where}.mock_profile must be a lower-case portable id, got {profile!r}"
        )
    where = f"{where} ({profile})"
    template = raw["template"]
    if not isinstance(template, str) or template not in TEMPLATES:
        raise MockControllerProfileError(
            f"{where}.template must be one of {', '.join(sorted(TEMPLATES))}, "
            f"got {template!r}"
        )
    for key in ("hardware_component_name", "arm_controller", "gripper_controller"):
        if key in raw and (
            not isinstance(raw[key], str) or not _ROS_TOKEN.fullmatch(raw[key])
        ):
            raise MockControllerProfileError(
                f"{where}.{key} must be one ROS name token, got {raw[key]!r}"
            )
    arm = raw.get("arm_controller")
    gripper = raw.get("gripper_controller")
    if arm is None and gripper is None:
        raise MockControllerProfileError(
            f"{where} names no controller: give arm_controller, gripper_controller "
            "or both"
        )
    constraints = raw.get("vendor_joint_constraints")
    if arm is None:
        if constraints is not None:
            raise MockControllerProfileError(
                f"{where}.vendor_joint_constraints must be absent without an "
                "arm_controller"
            )
        vendor = None
    elif template == "ufactory":
        if not isinstance(constraints, dict) or set(constraints) != {"trajectory"}:
            raise MockControllerProfileError(
                f"{where}.vendor_joint_constraints must be "
                "{trajectory: <number>} copied from the vendor controller file "
                "(template ufactory); the goal is arrival_tolerance_rad"
            )
        vendor = (
            _constraint_text(
                constraints["trajectory"], f"{where}.vendor_joint_constraints.trajectory"
            ),
        )
    elif constraints is not None:
        raise MockControllerProfileError(
            f"{where}.vendor_joint_constraints must be absent for template "
            f"{template!r}: its template carries the retained constraints"
        )
    else:
        vendor = None
    timeouts = []
    for key in ("hard_stop_completion_timeout_s", "gripper_result_timeout_s"):
        value = raw[key]
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value <= 0
        ):
            raise MockControllerProfileError(
                f"{where}.{key} must be a positive number of seconds, got {value!r}"
            )
        timeouts.append(float(value))
    arrival = raw.get("arrival_tolerance_rad")
    if arm is None:
        if arrival is not None:
            raise MockControllerProfileError(
                f"{where}.arrival_tolerance_rad must be absent without an "
                "arm_controller"
            )
    elif arrival is None:
        raise MockControllerProfileError(f"{where} is missing arrival_tolerance_rad")
    elif (
        isinstance(arrival, bool)
        or not isinstance(arrival, (int, float))
        or not math.isfinite(arrival)
        or arrival <= 0
    ):
        raise MockControllerProfileError(
            f"{where}.arrival_tolerance_rad must be a positive number of radians, "
            f"got {arrival!r}"
        )
    return MockControllerProfile(
        mock_profile=profile,
        template=template,
        hardware_component_name=raw["hardware_component_name"],
        arm_controller=arm,
        vendor_joint_constraints=vendor,
        gripper_controller=gripper,
        hard_stop_completion_timeout_s=timeouts[0],
        gripper_result_timeout_s=timeouts[1],
        arrival_tolerance_rad=None if arrival is None else float(arrival),
    )


class _UniqueKeyLoader(yaml.SafeLoader):
    """SafeLoader that refuses a repeated mapping key (PyYAML keeps the last)."""

    def construct_mapping(self, node, deep=False):  # type: ignore[override]
        seen: set[object] = set()
        for key_node, _value in node.value:
            key = self.construct_object(key_node, deep=deep)
            if key in seen:
                raise MockControllerProfileError(
                    f"line {key_node.start_mark.line + 1}: duplicate key {key!r}"
                )
            seen.add(key)
        return super().construct_mapping(node, deep=deep)


def load(path: Path) -> list[MockControllerProfile]:
    """Return every row of the file; raise MockControllerProfileError."""
    try:
        document = yaml.load(path.read_text(encoding="utf-8"), Loader=_UniqueKeyLoader)
    except MockControllerProfileError as error:
        raise MockControllerProfileError(f"{path}: {error}") from error
    except yaml.YAMLError as error:
        raise MockControllerProfileError(f"{path} is not valid YAML: {error}") from error
    if not isinstance(document, dict) or set(document) != {"mock_profiles"}:
        raise MockControllerProfileError(f"{path} must contain only `mock_profiles:`")
    raw_rows = document["mock_profiles"]
    if not isinstance(raw_rows, list) or not raw_rows:
        raise MockControllerProfileError(f"{path}: mock_profiles must be a non-empty list")
    rows = [parse_row(raw, f"mock_profiles[{index}]") for index, raw in enumerate(raw_rows)]
    seen: set[str] = set()
    for row in rows:
        if row.mock_profile in seen:
            raise MockControllerProfileError(
                f"{path}: mock_profile {row.mock_profile!r} appears more than once"
            )
        seen.add(row.mock_profile)
    return rows


def _cpp_double(value: float) -> str:
    text = repr(value)
    return text if ("." in text or "e" in text) else text + ".0"


def cpp_rows(rows: list[MockControllerProfile], source: str) -> str:
    """The `ControllerProfileRow` initializers, one per line."""
    lines = [
        f"// Generated at configure time from {source}; do not edit.",
    ]
    for row in rows:
        vendor = (
            "std::nullopt"
            if row.vendor_joint_constraints is None
            else 'VendorJointConstraints{"%s"}' % row.vendor_joint_constraints
        )
        lines.append(
            '{"%s", "%s", %s, "%s", %s, "%s", %s, %s, %s, %s},'
            % (
                row.mock_profile,
                row.hardware_component_name,
                TEMPLATES[row.template],
                row.arm_controller or "",
                vendor,
                row.gripper_controller or "",
                "true" if row.gripper_controller else "false",
                _cpp_double(row.hard_stop_completion_timeout_s),
                _cpp_double(row.gripper_result_timeout_s),
                # A gripper-only row has no arm joint to hold to a goal.
                _cpp_double(row.arrival_tolerance_rad or 0.0),
            )
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path(DEFAULT_PATH))
    parser.add_argument("--output", type=Path, help="write the C++ row initializers")
    parser.add_argument("--source-label", default=DEFAULT_PATH)
    arguments = parser.parse_args(argv)
    try:
        rows = load(arguments.input)
    except (OSError, MockControllerProfileError) as error:
        print(f"mock controller profiles: {error}", file=sys.stderr)
        return 1
    if arguments.output is not None:
        text = cpp_rows(rows, arguments.source_label)
        # Unchanged output keeps its timestamp, so ninja rebuilds nothing.
        if not arguments.output.exists() or arguments.output.read_text() != text:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
