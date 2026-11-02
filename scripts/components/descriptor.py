"""Component descriptors: one `component.yaml` per component folder.

A descriptor (`api_version: manyforge.component_package.v1`) describes one
robot, tool, I/O device, machine or sensor as data. A machine (`kind: machine`) is an
owner with member slots `{role, model}` and lane rows naming their adapter
(`lanes.real: {adapter}`; plan, "Machines: owners and members"); a machine's own I/O bank is an `io_device` in model form (signals
once plus `lanes.real {adapter, bank}`); a sensor (`kind: sensor`, e.g. a
point-cloud source) names its sensor type and its one adapter row
(`adapter_row: {adapter}`); it has no lane. Its field names are the planned component
package manifest's, so packages can adopt the file unchanged. Versions are
`MAJOR.MINOR.PATCH`; nothing here pins a digest.

What reads a descriptor today:

- `manyforge component check <folder>`: the descriptor against its own URDF
  and SRDF and, for a robot or a tool, against the rows Apply reads (the
  model family row and component profile rows of the maintained description,
  the generated catalog files) and its lane rows (`lanes.<lane>`, CONTRACTS
  §12) against their files. With `--scenario`, a
  Scenario's components of that robot or tool; a tool's must be mounted on a
  free robot `flange`.
- `manyforge component register <folder>`: writes those rows from a robot or
  tool descriptor; a machine, a model-form I/O device or a sensor is listed
  in the description's `component_models`.
- `manyforge component new <kind> <id>`: a folder from the templates.
- `manyforge component list` / `list_components()`: the curated components,
  each checked; read-only. `runtime.modelFamilyId` joins a robot or tool to
  the rows Composer's component catalog offers.

Apply still reads the rows, never the descriptor. Every problem is one
`ComponentError` with a typed code, printed as one line.
"""

from __future__ import annotations

import json
import math
import os
import re
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from pathlib import PurePosixPath
from typing import Any

import yaml


API_VERSION = "manyforge.component_package.v1"
LIST_SCHEMA = "manyforge.component_list.v1"
DESCRIPTOR = "component.yaml"
KINDS = ("robot", "tool", "io_device", "simulator", "machine", "sensor")
# Curated roots. A robot or tool sits beside its model files, under the
# `package://` root the runtime resolves; I/O devices sit in components/<kind>/.
ROBOT_ROOT = "manyforge_example_models/models"
COMPONENT_ROOT = "components"
TEMPLATES = "components/templates"
GUIDE = "docs/authoring/ADDING_A_COMPONENT.md"
DESCRIPTION = (
    "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
)
COMPONENT_TYPES = "manyforge_deployment/resources/maintained/catalog/component_type"
MODEL_ASSETS = "manyforge_deployment/resources/maintained/model_assets"
# The profile row an I/O device component of a Deployment selects.
IO_DEVICE_PROFILE = "io_device_v0"

_COMMON_REQUIRED = ("api_version", "kind", "id", "version", "title", "license")
_COMMON_OPTIONAL = ("provenance",)
_KIND_FIELDS = {
    "robot": (
        ("model", "roles", "tcps", "limits", "lanes"),
        ("defaults",),
    ),
    "tool": (("model", "mount", "tcps"), ("roles", "gripper", "lanes")),
    # Provider form (a separate device, each signal names its provider) or
    # model form (a machine's own I/O bank: `lanes.real {adapter, bank}`).
    "io_device": (("signals",), ("lanes",)),
    # A machine (owner) with its member slots and its own lane rows.
    "machine": (("default_owner_id", "members", "lanes"), ()),
    # A simulator (CONTRACTS §14) is a component with its lane rows only.
    "simulator": (("lanes",), ()),
    # A sensor: its sensor type and its one adapter row (it has no lane).
    "sensor": (("sensor", "adapter_row"), ()),
}
# The sensor types a `kind: sensor` model may name.
SENSOR_TYPES = ("pointcloud",)
REQUIRED_FRAME_ROLES = ("base", "flange")
GRIPPER_ACTUATIONS = ("pneumatic", "position_commanded")
INSTANCE_CARDINALITIES = ("multiple_per_cell", "one_per_cell")
# Closed lists in code: the I/O provider profiles the runtime schema accepts
# (manyforge.value_schema.runtime.io_provider_execution.v1).
IO_PROVIDER_PROFILES = {
    "input": (
        "geometric_contact",
        "mock",
        "ros2_control_gpio",
        "ros_bool",
    ),
    "output": ("mock_output", "ros2_control_gpio", "ros_set_bool"),
}
# A model-form I/O bank's real row: its vendor family and which bank it is.
IO_BANKS = ("controller", "tool")
# The description's list of model-free component models the compiler reads
# without a URDF: machines, model-form I/O devices and sensors.
COMPONENT_MODELS = "component_models"
_INPUT_ONLY_FIELDS = ("freshness", "transition_capability", "update_mode")
# The lanes a model may declare (`lanes.<lane>`, a Deployment component's
# `lane`). Each row names its adapter, which reads its own keys (CONTRACTS §12).
LANES = ("mock", "isaac", "real")
_LANE_ROW_REQUIRED = ("adapter", "controller")
# The Mock lane adapter (manyforge_ros2_control_execution/lane/): its row
# names the model's own controllers file.
MOCK_ADAPTER = "mock"
# Family row `source` fields a descriptor decides; the others (historical
# paths, projection kinds) are kept as they are on an existing row.
_DERIVED_SOURCE = ("accepted_package_uri_prefix", "resource_root", "srdf_path", "urdf_path")
_ID = re.compile(r"[a-z][a-z0-9_]{0,63}\.[a-z][a-z0-9_]{0,127}")
_PORTABLE_ID = re.compile(r"[a-z][a-z0-9_]{0,127}")
_VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


@dataclass(frozen=True)
class ComponentError:
    """One problem: a typed code, where it is, and what is wrong."""

    code: str
    where: str
    message: str

    def line(self) -> str:
        return f"error: {self.where}: {self.message} [{self.code}]"

    def as_dict(self) -> dict[str, Any]:
        return {"code": self.code, "message": self.message, "details": {"where": self.where}}


@dataclass
class Report:
    errors: list[ComponentError] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def error(self, code: str, where: str, message: str) -> None:
        self.errors.append(ComponentError(code, where, message))

    def note(self, message: str) -> None:
        self.notes.append(f"ok: {message}")


@dataclass(frozen=True)
class Descriptor:
    folder: Path  # resolved
    label: str  # how messages name the file
    data: dict[str, Any]
    text: str

    @property
    def id(self) -> str:
        return self.data["id"]

    @property
    def kind(self) -> str:
        return self.data["kind"]

    @property
    def name(self) -> str:
        """The id after the vendor: a robot's or tool's model family and Mock profile id."""
        return self.data["id"].split(".", 1)[1]


@dataclass
class Model:
    links: set[str]
    joints: dict[str, str]  # name -> type
    mimic_joints: set[str]
    mesh_uris: list[str]
    groups: set[str]
    group_states: dict[str, set[str]]  # group -> state names


def _display(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def _one_line(error: Exception) -> str:
    return " ".join(str(error).split())


class _UniqueKeyLoader(yaml.SafeLoader):
    """SafeLoader that refuses a repeated mapping key (PyYAML keeps the last)."""

    def construct_mapping(self, node, deep=False):  # type: ignore[override]
        seen: set[object] = set()
        for key_node, _value in node.value:
            key = self.construct_object(key_node, deep=deep)
            if key in seen:
                raise yaml.constructor.ConstructorError(
                    None, None, f"duplicate key {key!r}", key_node.start_mark
                )
            seen.add(key)
        return super().construct_mapping(node, deep=deep)


# --- descriptor shape -----------------------------------------------------


class _Fields:
    """Validation context: every message names the descriptor and the field."""

    def __init__(self, report: Report, label: str) -> None:
        self.report = report
        self.label = label

    def fail(self, path: str, message: str, code: str = "component_manifest_invalid") -> None:
        self.report.error(code, f"{self.label}: {path}" if path else self.label, message)

    def mapping(
        self,
        value: Any,
        path: str,
        required: tuple[str, ...] = (),
        optional: tuple[str, ...] = (),
        *,
        open_keys: bool = False,
    ) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            self.fail(path, f"must be a mapping, got {value!r}")
            return None
        keys = [key for key in value if not isinstance(key, str)]
        if keys:
            self.fail(path, f"has non-text field names {keys!r}")
            return None
        missing = [key for key in required if key not in value]
        if missing:
            self.fail(path, f"is missing {', '.join(missing)}")
        if not open_keys:
            unknown = sorted(set(value) - set(required) - set(optional))
            if unknown:
                self.fail(
                    path,
                    f"has unknown fields {', '.join(unknown)} "
                    f"(allowed: {', '.join((*required, *optional))})",
                )
        return value

    def text(self, value: Any, path: str) -> bool:
        if isinstance(value, str) and value.strip():
            return True
        self.fail(path, f"must be non-empty text, got {value!r}")
        return False

    def portable(self, value: Any, path: str) -> bool:
        if isinstance(value, str) and _PORTABLE_ID.fullmatch(value):
            return True
        self.fail(path, f"must be a lower-case portable id ([a-z][a-z0-9_]*), got {value!r}")
        return False

    def number(self, value: Any, path: str) -> bool:
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value <= 0
        ):
            self.fail(path, f"must be a positive number, got {value!r}")
            return False
        return True

    def names(self, value: Any, path: str) -> bool:
        if (
            isinstance(value, list)
            and value
            and all(isinstance(item, str) and item for item in value)
        ):
            return True
        self.fail(path, f"must be a non-empty list of names, got {value!r}")
        return False

    def file(self, value: Any, path: str) -> None:
        if not self.text(value, path):
            return
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            self.fail(path, f"must be a file inside the component folder, got {value!r}")


def _validate_roles(f: _Fields, value: Any, robot: bool) -> dict[str, Any]:
    if robot:
        required = ("root_frame", "frames", "groups", "joints")
        optional: tuple[str, ...] = ("collision_bodies", "instance_cardinality")
    else:
        required, optional = (), ("frames", "groups", "joints", "collision_bodies")
    roles = f.mapping(value, "roles", required, optional)
    if roles is None:
        return {}
    if robot and "root_frame" in roles:
        f.text(roles["root_frame"], "roles.root_frame")
    found: dict[str, dict[str, Any]] = {}
    for key, is_list in (
        ("frames", False),
        ("groups", False),
        ("joints", True),
        ("collision_bodies", True),
    ):
        if key not in roles:
            found[key] = {}
            continue
        rows = f.mapping(roles[key], f"roles.{key}", open_keys=True) or {}
        for role, item in rows.items():
            f.portable(role, f"roles.{key}")
            (f.names if is_list else f.text)(item, f"roles.{key}.{role}")
        found[key] = rows
    frames, groups, joints = found["frames"], found["groups"], found["joints"]
    if robot:
        for role in REQUIRED_FRAME_ROLES:
            if role not in frames:
                f.fail("roles.frames", f"has no {role!r} role")
        if "root" in frames and frames["root"] != roles.get("root_frame"):
            f.fail("roles.frames.root", "must equal roles.root_frame when present")
        if "arm" not in groups:
            f.fail("roles.groups", "has no 'arm' role (the SRDF group of the arm joints)")
        if "arm_joints" not in joints:
            f.fail("roles.joints", "has no 'arm_joints' role (the arm joints, base to tip)")
        if "fingers" in joints or "gripper" in groups:
            f.fail("roles", "a robot descriptor is arm-only; describe its gripper as a tool")
        cardinality = roles.get("instance_cardinality", "multiple_per_cell")
        if cardinality not in INSTANCE_CARDINALITIES:
            f.fail(
                "roles.instance_cardinality",
                f"must be one of {', '.join(INSTANCE_CARDINALITIES)}, got {cardinality!r}",
            )
    fingers = joints.get("fingers")
    if fingers is not None:
        if isinstance(fingers, list) and len(fingers) != 1:
            f.fail("roles.joints.fingers", "must name the single commanded gripper joint")
        if "gripper" not in groups:
            f.fail("roles.groups", "a 'fingers' joint role needs a 'gripper' group role")
    return roles


def _frame_roles(data: dict[str, Any]) -> dict[str, Any]:
    """The frame roles a descriptor names besides its TCPs: `roles.frames`, a
    tool's `mount`, a robot's `root` (when no role is on its root link)."""
    roles = data.get("roles") if isinstance(data.get("roles"), dict) else {}
    frames = dict(roles["frames"]) if isinstance(roles.get("frames"), dict) else {}
    if data.get("kind") == "tool":
        mount = data.get("mount")
        if isinstance(mount, dict) and "frame" in mount:
            frames["mount"] = mount["frame"]
    elif ("root_frame" in roles and "root" not in frames
          and roles["root_frame"] not in frames.values()):
        frames["root"] = roles["root_frame"]
    return frames


def _validate_tcps(f: _Fields, value: Any, frames: dict[str, Any]) -> None:
    if not isinstance(value, list) or not value:
        f.fail("tcps", "must be a non-empty list of {id, frame}; the first is the default TCP")
        return
    seen: set[str] = set()
    for index, row in enumerate(value):
        tcp = f.mapping(row, f"tcps[{index}]", ("id", "frame"))
        if tcp is None:
            continue
        if "id" in tcp and f.portable(tcp["id"], f"tcps[{index}].id"):
            if tcp["id"] in seen:
                f.fail(f"tcps[{index}].id", f"{tcp['id']!r} appears more than once")
            seen.add(tcp["id"])
            # A TCP may share a frame role's name only on the same link.
            if tcp["id"] in frames and tcp.get("frame") != frames[tcp["id"]]:
                f.fail(f"tcps[{index}].id",
                       f"{tcp['id']!r} is already the frame role on link "
                       f"{frames[tcp['id']]!r}, but this TCP is on link "
                       f"{tcp.get('frame')!r}; rename the TCP or use the same link")
        if "frame" in tcp:
            f.text(tcp["frame"], f"tcps[{index}].frame")


def _validate_model(f: _Fields, value: Any, robot: bool) -> None:
    # A tool's model family row reads its SRDF too (groups, states, collisions).
    required = ("urdf", "srdf", "package_name") if robot else ("urdf", "srdf")
    optional = () if robot else ("package_name",)
    model = f.mapping(value, "model", required, optional)
    if model is None:
        return
    for key in ("urdf", "srdf"):
        if key in model:
            f.file(model[key], f"model.{key}")
    if "package_name" in model:
        f.text(model["package_name"], "model.package_name")


def _validate_gripper(f: _Fields, value: Any) -> None:
    gripper = f.mapping(
        value,
        "gripper",
        ("actuation", "states", "attach_frame", "touch_links"),
        ("command_duration_s",),
    )
    if gripper is None:
        return
    if "command_duration_s" in gripper:
        f.number(gripper["command_duration_s"], "gripper.command_duration_s")
    elif gripper.get("actuation") == "pneumatic":
        f.fail("gripper", "a pneumatic gripper needs command_duration_s (the seconds "
               "the valve takes to open or close the fingers)")
    if "actuation" in gripper and gripper["actuation"] not in GRIPPER_ACTUATIONS:
        f.fail(
            "gripper.actuation",
            f"must be one of {', '.join(GRIPPER_ACTUATIONS)}, got {gripper['actuation']!r}",
        )
    if "states" in gripper:
        states = f.mapping(gripper["states"], "gripper.states", ("open", "closed"))
        for key in ("open", "closed"):
            if states is not None and key in states:
                f.text(states[key], f"gripper.states.{key}")
    if "attach_frame" in gripper:
        f.text(gripper["attach_frame"], "gripper.attach_frame")
    if "touch_links" in gripper:
        f.names(gripper["touch_links"], "gripper.touch_links")


def _validate_lanes(f: _Fields, data: dict[str, Any], robot: bool,
                    simulator: bool = False) -> None:
    """`lanes.<lane>` rows (CONTRACTS §12): every row names its adapter and,
    but a simulator's, its controller (the arm's, or a tool's gripper
    controller); a robot's row its `arrival_tolerance_rad`, a tool's none. The
    other keys are the adapter's."""
    lanes = f.mapping(data["lanes"], "lanes", (), LANES) if "lanes" in data else None
    required = ("adapter",) if simulator else _LANE_ROW_REQUIRED
    for lane, row in sorted((lanes or {}).items()):
        path = f"lanes.{lane}"
        if f.mapping(row, path, required, open_keys=True) is None:
            continue
        for key in required:
            if key in row:
                f.portable(row[key], f"{path}.{key}")
        if robot and "arrival_tolerance_rad" not in row:
            f.fail(path, "a robot's row needs arrival_tolerance_rad (every arm joint's "
                   "JTC goal)", "component_lane_invalid")
        if not robot and not simulator and "arrival_tolerance_rad" in row:
            f.fail(f"{path}.arrival_tolerance_rad", "a tool's row has none; the robot's "
                   "row moves the arm", "component_lane_invalid")
        for key in ("arrival_tolerance_rad", "goal_time_s"):
            if key in row:
                f.number(row[key], f"{path}.{key}")
        if "controllers_file" in row:
            f.file(row["controllers_file"], f"{path}.controllers_file")
        if "vendor" in row:
            f.portable(row["vendor"], f"{path}.vendor")
        if row.get("adapter") == MOCK_ADAPTER and "controllers_file" not in row \
                and "vendor" not in row:
            f.fail(path, "a Mock row needs a vendor or a controllers_file (the Mock lane "
                   "has no controllers of its own)", "component_lane_invalid")


def _validate_simulator(f: _Fields, data: dict[str, Any]) -> None:
    _validate_lanes(f, data, robot=False, simulator=True)


def _validate_sensor(f: _Fields, data: dict[str, Any]) -> None:
    """A sensor type and one adapter row naming its adapter, the same on every
    lane (its other keys are the adapter's)."""
    if "sensor" in data and data["sensor"] not in SENSOR_TYPES:
        f.fail("sensor", f"must be one of {', '.join(SENSOR_TYPES)}, got {data['sensor']!r}")
    if "adapter_row" in data:
        row = f.mapping(data["adapter_row"], "adapter_row", ("adapter",), open_keys=True)
        if row is not None and "adapter" in row:
            f.portable(row["adapter"], "adapter_row.adapter")


def _validate_machine(f: _Fields, data: dict[str, Any]) -> None:
    """Member slots `{role, model}` and lane rows naming their adapter; no
    row on a lane: the owner gets the host's passive adapter. Every member may
    be deactivated on every lane (AUD-5b revised)."""
    _validate_machine_members(f, data)
    if "lanes" in data:
        lanes = f.mapping(data["lanes"], "lanes", (), LANES)
        for lane, row in sorted((lanes or {}).items()):
            path = f"lanes.{lane}"
            if f.mapping(row, path, ("adapter",), open_keys=True) is None:
                continue
            if "adapter" in row:
                f.portable(row["adapter"], f"{path}.adapter")


def _validate_machine_members(f: _Fields, data: dict[str, Any]) -> None:
    """The member slots `{role, model}`, each role once."""
    roles: set[str] = set()
    # The owner's component id when a machine is added (members:
    # `<owner_id>_<role>`); editable in the Deployment.
    if "default_owner_id" in data:
        f.portable(data["default_owner_id"], "default_owner_id")
    if "members" not in data:
        return
    members = data["members"]
    if not isinstance(members, list) or not members:
        f.fail("members", "must be a non-empty list of member slots")
        return
    for index, value in enumerate(members):
        path = f"members[{index}]"
        member = f.mapping(value, path, ("role", "model"))
        if member is None:
            continue
        role = member.get("role")
        if "role" in member and f.portable(role, f"{path}.role"):
            if role in roles:
                f.fail(f"{path}.role", f"{role!r} appears more than once")
            roles.add(role)
        if "model" in member and (
            not isinstance(member["model"], str) or not _ID.fullmatch(member["model"])
        ):
            f.fail(f"{path}.model", f"must be a <vendor>.<name> model id, got {member['model']!r}")


def _validate_robot(f: _Fields, data: dict[str, Any]) -> None:
    # A missing required field is already reported once by `validate`.
    if "model" in data:
        _validate_model(f, data["model"], robot=True)
    roles = _validate_roles(f, data["roles"], robot=True) if "roles" in data else {}
    if "tcps" in data:
        _validate_tcps(f, data["tcps"], _frame_roles(data))
    limit = None
    limits = (
        f.mapping(data["limits"], "limits", ("max_tcp_linear_speed_mps",))
        if "limits" in data
        else None
    )
    if limits is not None and "max_tcp_linear_speed_mps" in limits:
        if f.number(limits["max_tcp_linear_speed_mps"], "limits.max_tcp_linear_speed_mps"):
            limit = limits["max_tcp_linear_speed_mps"]
    joints = roles.get("joints") if isinstance(roles.get("joints"), dict) else {}
    _validate_lanes(f, data, robot=True)
    if "defaults" in data:
        defaults = f.mapping(
            data["defaults"],
            "defaults",
            (),
            ("initial_joint_positions_rad", "maximum_tcp_linear_speed_mps"),
        )
        if defaults is None:
            return
        initial = defaults.get("initial_joint_positions_rad")
        arm = joints.get("arm_joints")
        if initial is not None:
            if not isinstance(initial, list) or not all(
                not isinstance(value, bool)
                and isinstance(value, (int, float))
                and math.isfinite(value)
                for value in initial
            ):
                f.fail("defaults.initial_joint_positions_rad", "must be a list of numbers")
            elif isinstance(arm, list) and len(initial) != len(arm):
                f.fail(
                    "defaults.initial_joint_positions_rad",
                    f"has {len(initial)} values for {len(arm)} arm joints {arm}",
                    "component_joint_count_mismatch",
                )
        speed = defaults.get("maximum_tcp_linear_speed_mps")
        if speed is not None and f.number(speed, "defaults.maximum_tcp_linear_speed_mps"):
            if limit is not None and speed > limit:
                f.fail(
                    "defaults.maximum_tcp_linear_speed_mps",
                    f"{speed} exceeds limits.max_tcp_linear_speed_mps {limit}",
                    "component_limit_exceeded",
                )


def _validate_tool(f: _Fields, data: dict[str, Any]) -> None:
    if "model" in data:
        _validate_model(f, data["model"], robot=False)
    roles = _validate_roles(f, data["roles"], robot=False) if "roles" in data else {}
    joints = roles.get("joints") if isinstance(roles.get("joints"), dict) else {}
    _validate_lanes(f, data, robot=False)
    mount = f.mapping(data["mount"], "mount", ("frame",)) if "mount" in data else None
    if mount is not None and "frame" in mount:
        f.text(mount["frame"], "mount.frame")
    if "tcps" in data:
        _validate_tcps(f, data["tcps"], _frame_roles(data))
    if "gripper" in data:
        _validate_gripper(f, data["gripper"])


def _signal_name(f: _Fields, value: Any, path: str) -> bool:
    """A device's own signal name (e.g. CI0, TO1): letters, digits and `_`."""
    if isinstance(value, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,127}", value):
        return True
    f.fail(path, f"must be a signal name ([A-Za-z][A-Za-z0-9_]*), got {value!r}")
    return False


def is_io_model_form(data: dict[str, Any]) -> bool:
    """An `io_device` descriptor in model form (a machine's own I/O bank)."""
    return data.get("kind") == "io_device" and "lanes" in data


def _validate_io(f: _Fields, data: dict[str, Any]) -> None:
    # Model form: a machine's own I/O bank, its provider per lane (real: the
    # vendor family and bank; mock and isaac: the host's Mock I/O).
    model_form = "lanes" in data
    if model_form:
        lanes = f.mapping(data["lanes"], "lanes", ("real",))
        # `input_maximum_age_ms`: the inputs' freshness budget (sample age).
        row = f.mapping(lanes["real"], "lanes.real",
                        ("adapter", "bank", "input_maximum_age_ms")) if (
            lanes is not None and "real" in lanes) else None
        if row is not None:
            if "adapter" in row:
                f.portable(row["adapter"], "lanes.real.adapter")
            if "input_maximum_age_ms" in row:
                f.number(row["input_maximum_age_ms"], "lanes.real.input_maximum_age_ms")
            if "bank" in row and row["bank"] not in IO_BANKS:
                f.fail("lanes.real.bank", f"must be one of {', '.join(IO_BANKS)}, "
                       f"got {row['bank']!r}")
    if "signals" not in data:
        return
    signals = data["signals"]
    if not isinstance(signals, list) or not signals:
        f.fail("signals", "must be a non-empty list of signals")
        return
    seen: set[str] = set()
    for index, value in enumerate(signals):
        path = f"signals[{index}]"
        direction = value.get("direction") if isinstance(value, dict) else None
        if direction not in ("input", "output"):
            f.fail(f"{path}.direction", f"must be input or output, got {direction!r}")
            continue
        extra = _INPUT_ONLY_FIELDS if direction == "input" and not model_form else ()
        provider = () if model_form else ("provider",)
        signal = f.mapping(
            value, path, ("signal_name", "direction", "value_type", *provider, *extra)
        )
        if signal is None:
            continue
        name = signal.get("signal_name")
        if "signal_name" in signal and (
            f.portable(name, f"{path}.signal_name") if not model_form
            else _signal_name(f, name, f"{path}.signal_name")
        ):
            if name in seen:
                f.fail(f"{path}.signal_name", f"{name!r} appears more than once")
            seen.add(name)
        if "value_type" in signal and signal["value_type"] != "bool":
            f.fail(f"{path}.value_type", f"must be bool, got {signal['value_type']!r}")
        if "provider" in signal and not model_form:
            provider = f.mapping(
                signal["provider"], f"{path}.provider", ("profile",), open_keys=True
            )
            profiles = IO_PROVIDER_PROFILES[direction]
            if provider and "profile" in provider and provider["profile"] not in profiles:
                f.fail(
                    f"{path}.provider.profile",
                    f"must be one of {', '.join(profiles)} for an {direction}, "
                    f"got {provider['profile']!r}",
                )


def validate(data: Any, report: Report, label: str) -> bool:
    """The descriptor's shape; True when the model and row checks may run."""
    f = _Fields(report, label)
    before = len(report.errors)
    if not isinstance(data, dict):
        f.fail("", "must be a mapping")
        return False
    kind = data.get("kind")
    if kind not in KINDS:
        f.fail("kind", f"must be one of {', '.join(KINDS)}, got {kind!r}")
        return False
    required, optional = _KIND_FIELDS[kind]
    f.mapping(data, "", (*_COMMON_REQUIRED, *required), (*_COMMON_OPTIONAL, *optional))
    if data.get("api_version") != API_VERSION:
        f.fail("api_version", f"must be {API_VERSION}, got {data.get('api_version')!r}")
    if not isinstance(data.get("id"), str) or not _ID.fullmatch(data["id"]):
        f.fail(
            "id",
            "must be <vendor>.<name>, each a lower-case portable id "
            f"([a-z][a-z0-9_]*), got {data.get('id')!r}",
        )
    if not isinstance(data.get("version"), str) or not _VERSION.fullmatch(data["version"]):
        f.fail("version", f"must be MAJOR.MINOR.PATCH text, got {data.get('version')!r}")
    for key in ("title", "license"):
        if key in data:
            f.text(data[key], key)
    if "provenance" in data:
        provenance = f.mapping(data["provenance"], "provenance", open_keys=True) or {}
        for key, value in provenance.items():
            f.text(value, f"provenance.{key}")
    {"robot": _validate_robot, "tool": _validate_tool, "io_device": _validate_io,
     "simulator": _validate_simulator, "machine": _validate_machine,
     "sensor": _validate_sensor}[kind](
        f, data
    )
    return len(report.errors) == before


def load_descriptor(root: Path, folder: Path, report: Report) -> Descriptor | None:
    """Read and validate `<folder>/component.yaml`; None when it is no mapping."""
    folder = folder.resolve()
    path = folder / DESCRIPTOR
    label = _display(root, path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        report.error("component_manifest_invalid", label, f"cannot read: {_one_line(error)}")
        return None
    try:
        data = yaml.load(text, Loader=_UniqueKeyLoader)
    except yaml.YAMLError as error:
        report.error("component_manifest_invalid", label, f"is not valid YAML: {_one_line(error)}")
        return None
    validate(data, report, label)
    if not isinstance(data, dict):
        return None
    return Descriptor(folder=folder, label=label, data=data, text=text)


# --- model files ----------------------------------------------------------


def _xml(path: Path, report: Report, where: str) -> ElementTree.Element | None:
    try:
        return ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError) as error:
        report.error("component_model_unreadable", where, f"cannot read {path}: {_one_line(error)}")
        return None


def _load_model(urdf: Path, srdf: Path | None, report: Report, label: str) -> Model | None:
    urdf_root = _xml(urdf, report, f"{label}: model.urdf")
    srdf_root = _xml(srdf, report, f"{label}: model.srdf") if srdf is not None else None
    if urdf_root is None or (srdf is not None and srdf_root is None):
        return None
    group_states: dict[str, set[str]] = {}
    groups: set[str] = set()
    if srdf_root is not None:
        for state in srdf_root.iter("group_state"):
            group_states.setdefault(state.get("group", ""), set()).add(state.get("name", ""))
        groups = {group.get("name", "") for group in srdf_root.iter("group")}
    return Model(
        links={link.get("name", "") for link in urdf_root.iter("link")},
        joints={joint.get("name", ""): joint.get("type", "") for joint in urdf_root.iter("joint")},
        mimic_joints={
            joint.get("name", "")
            for joint in urdf_root.iter("joint")
            if joint.find("mimic") is not None
        },
        mesh_uris=[mesh.get("filename", "") for mesh in urdf_root.iter("mesh")],
        groups=groups,
        group_states=group_states,
    )


def package_prefix(folder: Path, package_name: str) -> str | None:
    """`package://<package>/<path>/` for a folder inside a folder named <package>."""
    folder = folder.resolve()
    for parent in folder.parents:
        if parent.name == package_name:
            return f"package://{package_name}/{folder.relative_to(parent).as_posix()}/"
    return None


def _mesh_uris(descriptor: Descriptor) -> list[str] | None:
    """The URDF's mesh URIs; None when it cannot be read (check_model reports it)."""
    try:
        urdf_root = ElementTree.parse(
            descriptor.folder / descriptor.data["model"]["urdf"]
        ).getroot()
    except (OSError, ElementTree.ParseError):
        return None
    return [mesh.get("filename", "") for mesh in urdf_root.iter("mesh")]


def _model_package(root: Path, descriptor: Descriptor, uris: list[str]) -> str | None:
    """`model.package_name`; for a tool without it, the one package of its mesh
    URIs, or with no mesh the workspace folder it sits in."""
    package = descriptor.data.get("model", {}).get("package_name")
    if package is not None or descriptor.kind != "tool":
        return package
    names = {uri[len("package://"):].split("/", 1)[0]
             for uri in uris if uri.startswith("package://")}
    if len(names) == 1:
        return next(iter(names))
    relative = _relative(root, descriptor.folder)
    return relative.split("/", 1)[0] if relative and not uris else None


def _model_mesh_source(root: Path, descriptor: Descriptor) -> tuple[str | None, str | None]:
    """Derive the maintained mesh root from the package URIs in this model."""
    uris = _mesh_uris(descriptor)
    package = _model_package(root, descriptor, uris or [])
    if package is None:
        return None, None
    if not uris:
        return package_prefix(descriptor.folder, package), _relative(root, descriptor.folder)
    uri_prefix = f"package://{package}/"
    roots: set[tuple[str, ...]] = set()
    for uri in uris:
        if not uri.startswith(uri_prefix):
            return None, None
        parts = Path(uri[len(uri_prefix):]).parts
        if "meshes" not in parts:
            return None, None
        roots.add(parts[:parts.index("meshes")])
    if len(roots) != 1:
        return None, None
    parts = next(iter(roots))
    relative = Path(package, *parts).as_posix()
    suffix = f"{'/'.join(parts)}/" if parts else ""
    return f"{uri_prefix}{suffix}", relative


def check_model(root: Path, descriptor: Descriptor, report: Report) -> Model | None:
    """A robot or tool descriptor against its own URDF/SRDF and meshes."""
    data = descriptor.data
    robot = descriptor.kind == "robot"
    files = data["model"]
    urdf = descriptor.folder / files["urdf"]
    srdf = descriptor.folder / files["srdf"] if "srdf" in files else None
    model = _load_model(urdf, srdf, report, descriptor.label)
    if model is None:
        return None
    before = len(report.errors)

    def fail(code: str, path: str, message: str) -> None:
        report.error(code, f"{descriptor.label}: {path}", message)

    def link(value: str, path: str) -> None:
        if value not in model.links:
            fail("component_role_unknown_link", path, f"link {value!r} is not in {urdf.name}")

    roles = data.get("roles", {})
    frames = roles.get("frames", {})
    groups = roles.get("groups", {})
    joints = roles.get("joints", {})
    if robot:
        link(roles["root_frame"], "roles.root_frame")
    for role, name in frames.items():
        link(name, f"roles.frames.{role}")
    for role, names in roles.get("collision_bodies", {}).items():
        for name in names:
            link(name, f"roles.collision_bodies.{role}")
    for role, group in groups.items():
        if srdf is None:
            fail("component_group_unknown", f"roles.groups.{role}", "group roles need model.srdf")
        elif group not in model.groups:
            fail("component_group_unknown", f"roles.groups.{role}",
                 f"group {group!r} is not in {srdf.name}")
    for role, names in joints.items():
        for joint in names:
            path = f"roles.joints.{role}"
            if joint not in model.joints:
                fail("component_joint_invalid", path, f"joint {joint!r} is not in {urdf.name}")
            elif model.joints[joint] in ("fixed", "floating", "planar"):
                fail("component_joint_invalid", path,
                     f"joint {joint!r} is {model.joints[joint]}, not a commanded joint")
            elif joint in model.mimic_joints:
                fail("component_joint_invalid", path,
                     f"joint {joint!r} is a mimic follower; name the joint it follows")
    gripper_group = groups.get("gripper")
    states = model.group_states.get(gripper_group, set())
    if "fingers" in joints and gripper_group is not None:
        for state in ("open", "closed"):
            if state not in states:
                fail("component_state_unknown", "roles.groups.gripper",
                     f"SRDF group {gripper_group!r} has no group_state {state!r}")
    gripper = data.get("gripper")
    if gripper is not None:
        if gripper_group is None:
            fail("component_group_unknown", "gripper", "needs a 'gripper' role in roles.groups")
        else:
            for key in ("open", "closed"):
                if gripper["states"][key] not in states:
                    fail("component_state_unknown", f"gripper.states.{key}",
                         f"{gripper['states'][key]!r} is not a group_state of SRDF group "
                         f"{gripper_group!r}")
        link(gripper["attach_frame"], "gripper.attach_frame")
        for name in gripper.get("touch_links", []):
            link(name, "gripper.touch_links")
    for index, tcp in enumerate(data["tcps"]):
        link(tcp["frame"], f"tcps[{index}].frame")
    if descriptor.kind == "tool":
        link(data["mount"]["frame"], "mount.frame")
    package = files.get("package_name")
    package_names = {
        uri[len("package://"):].split("/", 1)[0]
        for uri in model.mesh_uris if uri.startswith("package://")
    }
    if package is None and len(package_names) == 1:
        package = next(iter(package_names))
    if package is not None:
        uri_prefix = f"package://{package}/"
        for uri in model.mesh_uris:
            if not uri.startswith(uri_prefix):
                fail("component_mesh_unresolved", "model.urdf",
                     f"mesh {uri!r} is not under {uri_prefix}")
            elif not (root / package / uri[len(uri_prefix):]).is_file():
                fail("component_mesh_unresolved", "model.urdf",
                     f"mesh {uri!r} has no file under package {package!r}")
    elif model.mesh_uris:
        fail("component_mesh_unresolved", "model.urdf",
             "mesh URIs do not identify one package under the workspace")
    if model.mesh_uris:
        # The model family row names one mesh root (`register`).
        mesh_prefix, resource_root = _model_mesh_source(root, descriptor)
        if mesh_prefix is None or resource_root is None:
            fail("component_mesh_unresolved", "model.urdf",
                 f"{descriptor.kind} mesh URIs must share one package source root "
                 "before `meshes/`")
    if len(report.errors) == before:
        files_read = urdf.name + (f" and {srdf.name}" if srdf is not None else "")
        report.note(f"{descriptor.id}: roles resolve in {files_read}, "
                    f"{len(model.mesh_uris)} mesh URIs resolve")
    return model


# --- the rows Apply reads (robots) ----------------------------------------


def _relative(root: Path, folder: Path) -> str | None:
    try:
        return folder.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def family_row(root: Path, descriptor: Descriptor) -> dict[str, Any] | None:
    """The model family row a robot or tool descriptor stands for; None outside the workspace."""
    relative = _relative(root, descriptor.folder)
    if relative is None:
        return None
    data = descriptor.data
    name = descriptor.name
    roles = data.get("roles", {})
    frames = dict(roles.get("frames", {}))
    if descriptor.kind == "tool":
        # A tool's root is its mount.
        frames["mount"] = data["mount"]["frame"]
        root_role = "mount"
    else:
        root_link = roles["root_frame"]
        # The root frame role is an existing role on that link, else a `root` role.
        if frames.get("root") == root_link:
            root_role = "root"
        else:
            root_role = next(
                (role for role in sorted(frames) if frames[role] == root_link), None
            )
            if root_role is None:
                frames["root"] = root_link
                root_role = "root"
    # Each TCP is a frame role under its own id; one that shares a frame
    # role's name is on that role's link (`_validate_tcps`).
    tcps = [tcp["id"] for tcp in data["tcps"]]
    for tcp in data["tcps"]:
        frames.setdefault(tcp["id"], tcp["frame"])
    urdf = f"{relative}/{data['model']['urdf']}"
    srdf = f"{relative}/{data['model']['srdf']}"
    mesh_prefix, resource_root = _model_mesh_source(root, descriptor)
    return {
        "catalog_source": f"{MODEL_ASSETS}/{name}.model_asset.json",
        "model_family_id": name,
        "model_asset_id": name,
        "semantics": {
            "collision_body_role_bindings": [
                {"collision_body_role_id": role, "source_link_names": sorted(links)}
                for role, links in sorted(roles.get("collision_bodies", {}).items())
            ],
            "frame_role_bindings": [
                {"frame_role_id": role, "source_link_name": link}
                for role, link in sorted(frames.items())
            ],
            "instance_cardinality": roles.get("instance_cardinality", "multiple_per_cell"),
            "model_group_role_bindings": [
                {"model_group_role_id": role, "source_group_name": group}
                for role, group in sorted(roles.get("groups", {}).items())
            ],
            "resource_role_bindings": [
                {"resource_role_id": role, "source_joint_names": list(joints)}
                for role, joints in sorted(roles.get("joints", {}).items())
            ],
            "root_frame_role_id": root_role,
        },
        "source": {
            "accepted_package_uri_prefix": mesh_prefix,
            "historical_srdf_path": srdf,
            "historical_urdf_path": urdf,
            "resource_root": resource_root,
            "srdf_path": srdf,
            "srdf_projection_kind": "identity",
            "urdf_path": urdf,
            "urdf_projection_kind": "identity",
        },
        # The TCPs, the first the default. Left out when it is the one the
        # generator assumes: a tool's `tcp`; a robot's `tcp`, else `flange`.
        **({} if tcps == [_assumed_default_tcp(descriptor.kind, frames)]
           else {"tcp_frame_role_ids": tcps}),
        # The model the family's components select by its `id` (lanes and all).
        "component_descriptor": f"{relative}/{DESCRIPTOR}",
    }


def _assumed_default_tcp(kind: str, frames: dict[str, str]) -> str:
    """The default TCP the generator assumes for a family row without `tcp_frame_role_ids`."""
    return "tcp" if kind == "tool" or "tcp" in frames else "flange"


def _semantic_roles(semantics: Any) -> tuple[Any, ...]:
    """Family semantics without binding or collision-link order."""
    if not isinstance(semantics, dict):
        return ()

    def pairs(key: str, role: str, value: str) -> dict[Any, Any]:
        return {
            row.get(role): row.get(value)
            for row in semantics.get(key, [])
            if isinstance(row, dict)
        }

    collisions = pairs("collision_body_role_bindings", "collision_body_role_id",
                       "source_link_names")
    return (
        pairs("frame_role_bindings", "frame_role_id", "source_link_name"),
        pairs("model_group_role_bindings", "model_group_role_id", "source_group_name"),
        pairs("resource_role_bindings", "resource_role_id", "source_joint_names"),
        {role: sorted(links) for role, links in collisions.items() if isinstance(links, list)},
        semantics.get("root_frame_role_id"),
        semantics.get("instance_cardinality"),
    )


def _family_differences(existing: dict[str, Any], expected: dict[str, Any]) -> list[str]:
    differing = [
        key for key in ("catalog_source", "model_asset_id") if existing.get(key) != expected[key]
    ]
    if _semantic_roles(existing.get("semantics")) != _semantic_roles(expected["semantics"]):
        differing.append("semantics")
    if existing.get("tcp_frame_role_ids") != expected.get("tcp_frame_role_ids"):
        differing.append("tcp_frame_role_ids")
    if existing.get("component_descriptor") != expected["component_descriptor"]:
        differing.append("component_descriptor")
    source = existing.get("source", {})
    differing += [
        f"source.{key}" for key in _DERIVED_SOURCE if source.get(key) != expected["source"][key]
    ]
    return differing


def _family_mesh_source_errors(root: Path, descriptor: Descriptor,
                               source: dict[str, Any]) -> list[str]:
    """Ensure a maintained family still resolves the meshes named by its URDF."""
    uris = _mesh_uris(descriptor)
    package = _model_package(root, descriptor, uris or [])
    prefix = source.get("accepted_package_uri_prefix")
    resource_root = source.get("resource_root")
    if not isinstance(package, str) or not isinstance(prefix, str) or not isinstance(
        resource_root, str
    ):
        return ["source does not name a package mesh root"]
    package_root = f"{package}/"
    if not resource_root.startswith(package_root):
        return [f"source.resource_root {resource_root!r} is outside package {package!r}"]
    expected_prefix = (
        f"package://{package}/{resource_root[len(package_root):].rstrip('/')}/"
    )
    if prefix != expected_prefix:
        return ["source.accepted_package_uri_prefix does not match source.resource_root"]
    if uris is None:
        return []  # check_model reports the parse/read error with its source location.
    errors = []
    for uri in uris:
        if not uri.startswith(prefix):
            errors.append(f"mesh {uri!r} is outside {prefix}")
            continue
        relative = Path(uri[len(prefix):])
        if relative.is_absolute() or ".." in relative.parts:
            errors.append(f"mesh {uri!r} escapes source.resource_root")
        elif not (root / resource_root / relative).is_file():
            errors.append(f"mesh {uri!r} has no file under {resource_root}")
    return errors


def _read_description(root: Path, report: Report) -> dict[str, Any] | None:
    try:
        return json.loads((root / DESCRIPTION).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        report.error("component_not_registered", DESCRIPTION, f"cannot read: {_one_line(error)}")
        return None


def check_registration(
    root: Path, descriptor: Descriptor, report: Report
) -> set[str]:
    """The family row Apply reads agrees with a robot or tool descriptor.

    Returns the model ids a Deployment component selects (`component_profile_id`):
    the descriptor's `id`; none when the row is missing or stale.
    """
    name = descriptor.name
    expected = family_row(root, descriptor)
    if expected is None:
        report.error("component_not_registered", descriptor.label,
                     f"the folder is outside the workspace {root}; a {descriptor.kind} runs "
                     f"from {ROBOT_ROOT}/<name>/ today")
        return set()
    description = _read_description(root, report)
    if description is None:
        return set()
    hint = f"run `manyforge component register {_display(root, descriptor.folder)}`"
    before = len(report.errors)
    family = next(
        (row for row in description.get("model_families", [])
         if row.get("model_family_id") == name),
        None,
    )
    if family is None:
        report.error("component_not_registered", DESCRIPTION,
                     f"has no model family row {name!r}; {hint}")
    else:
        family_source = family.get("source", {})
        if isinstance(family_source, dict):
            for message in _family_mesh_source_errors(root, descriptor, family_source):
                report.error("component_mesh_unresolved", DESCRIPTION, message)
        differing = _family_differences(family, expected)
        if differing:
            report.error("component_registration_stale", DESCRIPTION,
                         f"model family row {name!r} differs from {DESCRIPTOR} in "
                         f"{', '.join(differing)}; {hint}")
    agreed = ["model family row"]
    lanes = descriptor.data.get("lanes", {})
    for lane, row in sorted(lanes.items()):
        controllers = row.get("controllers_file")
        if controllers is not None and not (descriptor.folder / controllers).is_file():
            report.error("component_lane_invalid", f"{descriptor.label}: lanes.{lane}",
                         f"controllers_file {controllers!r} is not a file in the folder")
    if lanes:
        agreed.append(f"lane rows {sorted(lanes)}")
    for generated in (
        expected["catalog_source"],
        f"{COMPONENT_TYPES}/{name}.component.json",
        f"{COMPONENT_TYPES}/{name}.fixed_scale.component.json",
    ):
        if not (root / generated).is_file():
            report.error("component_generated_missing", generated,
                         f"is missing; generate it ({GUIDE}, step 6)")
    if len(report.errors) != before:
        return set()
    report.note(f"{descriptor.id}: {', '.join(agreed)} agree with the descriptor")
    return {descriptor.id}


def _descriptor_path(root: Path, descriptor: Descriptor) -> str | None:
    """The descriptor's workspace-relative path, as `component_models` lists it."""
    folder = _relative(root, descriptor.folder)
    return None if folder is None else f"{folder}/{DESCRIPTOR}"


def check_component_model_registration(
    root: Path, descriptor: Descriptor, report: Report
) -> None:
    """A machine, a model-form I/O device or a sensor is listed in the
    description's `component_models`, which the compiler reads without a URDF."""
    path = _descriptor_path(root, descriptor)
    if path is None:
        report.error("component_not_registered", descriptor.label,
                     f"the folder is outside the workspace {root}")
        return
    description = _read_description(root, report)
    if description is None:
        return
    if path not in description.get(COMPONENT_MODELS, []):
        report.error("component_not_registered", DESCRIPTION,
                     f"{COMPONENT_MODELS} does not list {path}; run "
                     f"`manyforge component register {_display(root, descriptor.folder)}`")
        return
    report.note(f"{descriptor.id}: listed in {COMPONENT_MODELS}")


def _curated_models(root: Path) -> dict[str, dict[str, Any]]:
    """Every curated descriptor by id (shape only, no checks)."""
    result: dict[str, dict[str, Any]] = {}
    for folder in component_folders(root):
        try:
            data = yaml.safe_load((folder / DESCRIPTOR).read_text(encoding="utf-8"))
        except (OSError, yaml.YAMLError):
            continue
        if isinstance(data, dict) and isinstance(data.get("id"), str):
            result.setdefault(data["id"], data)
    return result


def check_machine_members(root: Path, descriptor: Descriptor, report: Report) -> None:
    """Every member slot names a curated model a member can be: a robot or a
    model-form I/O device."""
    models = _curated_models(root)
    before = len(report.errors)
    for index, member in enumerate(descriptor.data["members"]):
        where = f"{descriptor.label}: members[{index}].model"
        model = models.get(member["model"])
        if model is None:
            report.error("component_manifest_invalid", where,
                         f"{member['model']!r} is not a curated component")
        elif model.get("kind") != "robot" and not is_io_model_form(model):
            report.error("component_manifest_invalid", where,
                         f"{member['model']!r} is a {model.get('kind')}; a member is a "
                         "robot or a model-form io_device")
    if len(report.errors) == before:
        report.note(f"{descriptor.id}: {len(descriptor.data['members'])} member slot(s) "
                    "name curated models")


def _scenario_file(scenario: Path, name: str, report: Report) -> dict[str, Any] | None:
    try:
        document = yaml.safe_load((scenario / name).read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        report.error("component_scenario_mismatch", str(scenario),
                     f"cannot read {name}: {_one_line(error)}")
        return None
    return document


def _check_gripper_rows(rows: list[Any], model: Model, gripper_group: str | None,
                        fail: Callable[[str], None]) -> None:
    """Gripper rows name links and SRDF states of the model they move."""
    for row in rows:
        for key in ("tcp_frame", "attach_frame", "mount_frame"):
            if row.get(key) not in model.links:
                fail(f"gripper {key} {row.get(key)!r} is not a URDF link")
        for name in row.get("touch_links", []):
            if name not in model.links:
                fail(f"gripper touch link {name!r} is not a URDF link")
        for key in ("open_state_ref", "closed_state_ref"):
            if row.get(key) not in model.group_states.get(gripper_group, set()):
                fail(f"gripper {key} {row.get(key)!r} is not a group_state of SRDF "
                     f"group {gripper_group!r}")


def check_scenario(
    scenario: Path,
    descriptor: Descriptor,
    model: Model,
    models: set[str],
    report: Report,
) -> None:
    """This robot's components in one Scenario's deployment.yaml."""
    deployment = _scenario_file(scenario, "deployment.yaml", report)
    if deployment is None:
        return
    components = [
        component
        for component in deployment.get("components", [])
        if component.get("component_profile_id") in models
    ]
    if not components:
        report.error("component_scenario_mismatch", str(scenario),
                     f"no component selects the model {descriptor.id}")
        return
    lanes = descriptor.data.get("lanes", {})
    roles = descriptor.data["roles"]
    arm = list(roles["joints"]["arm_joints"])
    fingers = roles["joints"].get("fingers")
    gripper_group = roles["groups"].get("gripper")
    limit = descriptor.data["limits"]["max_tcp_linear_speed_mps"]
    before = len(report.errors)
    for component in components:
        where = f"{scenario.name} component {component.get('component_id', '?')}"

        def fail(message: str, code: str = "component_scenario_mismatch") -> None:
            report.error(code, where, message)

        configuration = component.get("configuration", {})
        runtime = configuration.get("runtime_configuration", {})
        # A member runs on its owner's lane (CONTRACTS §16).
        owner = next((row for row in deployment.get("components", [])
                      if "owner" in component
                      and row.get("component_id") == component["owner"]), component)
        if owner.get("lane") not in lanes:
            fail(f"lane {owner.get('lane')!r} is not a lane of {descriptor.id} "
                 f"({sorted(lanes)})", "component_lane_invalid")
        initial = runtime.get("initial_joint_positions_rad", [])
        if len(initial) != len(arm):
            fail(f"initial_joint_positions_rad has {len(initial)} values for {len(arm)} "
                 f"arm joints {arm}", "component_joint_count_mismatch")
        arm_settings = component.get("target_settings", {}).get("arm", {})
        stop_joints = [
            row.get("source_joint_name")
            for row in arm_settings.get("controlled_stop", {}).get("per_joint_limits", [])
        ]
        if stop_joints != arm:
            fail("target_settings.arm.controlled_stop.per_joint_limits must name the arm "
                 f"joints in order {arm}, got {stop_joints}")
        fault = set(
            configuration.get("default_fault_stop_profile", {})
            .get("per_joint_max_deceleration_rad_s2", {})
        )
        if not set(arm) <= fault:
            fail("default_fault_stop_profile.per_joint_max_deceleration_rad_s2 must name "
                 f"every arm joint {arm}")
        grippers = runtime.get("gripper_target_configurations", [])
        streams = sorted(row.get("stream_id") for row in runtime.get("component_streams", []))
        expected_streams = sorted(["measured_joints"] + (["gripper_feedback"] if grippers else []))
        if streams != expected_streams:
            fail(f"component_streams must declare exactly {expected_streams}, got {streams}")
        if fingers and not grippers:
            fail("the robot has a gripper ('fingers' role) but "
                 "gripper_target_configurations is empty")
        _check_gripper_rows(grippers, model, gripper_group, fail)
        speed = arm_settings.get("maximum_tcp_linear_speed_mps")
        if isinstance(speed, bool) or not isinstance(speed, (int, float)) or speed <= 0:
            fail("target_settings.arm.maximum_tcp_linear_speed_mps must be a positive number")
        elif speed > limit:
            fail(f"target_settings.arm.maximum_tcp_linear_speed_mps {speed} exceeds "
                 f"limits.max_tcp_linear_speed_mps {limit} of {descriptor.id}",
                 "component_limit_exceeded")
    if len(report.errors) == before:
        report.note(f"{scenario.name}: {len(components)} component(s) of {descriptor.id} agree "
                    "with the descriptor")


def _model_families(root: Path, description: dict[str, Any]) -> dict[str, str]:
    """Model id (a descriptor's `id`) -> its model family id."""
    families: dict[str, str] = {}
    for row in description.get("model_families", []):
        path = row.get("component_descriptor") if isinstance(row, dict) else None
        if not isinstance(path, str):
            continue
        try:
            data = yaml.safe_load((root / path).read_text(encoding="utf-8"))
        except (OSError, yaml.YAMLError):
            continue
        if isinstance(data, dict) and isinstance(data.get("id"), str):
            families[data["id"]] = row["model_family_id"]
    return families


def _attachment_interfaces(root: Path, family: str | None) -> set[str]:
    """The attachment interfaces of a model family's component type; none for a
    component without a family or whose type is not generated yet."""
    path = root / COMPONENT_TYPES / f"{family}.fixed_scale.component.json"
    if family is None or not path.is_file():
        return set()
    document = json.loads(path.read_text(encoding="utf-8"))
    return {
        row.get("attachment_interface_id")
        for row in document.get("attachment_interfaces", [])
    }


def _scenario_source_path(reference: Any, project_root: Path) -> Path | None:
    """Resolve one Deployment source reference, or return None for an invalid shape."""
    if type(reference) is not dict:
        return None
    if set(reference) == {"project_path"}:
        value = reference["project_path"]
        if (type(value) is not str or not value or len(value.encode("utf-8")) > 4096
                or value.startswith("/")
                or "\\" in value):
            return None
        path = PurePosixPath(value)
        if str(path) != value or value in {".", ".."} \
                or any(part in {"", "."} for part in path.parts):
            return None
        return Path(os.path.abspath(project_root / value))
    if set(reference) == {"external_path"}:
        value = reference["external_path"]
        if (type(value) is not str or not value or len(value.encode("utf-8")) > 4096
                or not Path(value).is_absolute()
                or os.path.normpath(value) != value):
            return None
        return Path(value)
    return None


def check_tool_scenario(
    root: Path,
    scenario: Path,
    descriptor: Descriptor,
    model: Model,
    models: set[str],
    report: Report,
) -> None:
    """This tool's components in one Scenario: each mounted on a free robot `flange`.

    A mounted tool has one `attachments` row
    `{parent: {component_id: <robot>, interface: flange}, child: {component_id:
    <tool>, interface: mount}}`, no `root_pose_in_cell`, and the assignment's
    `tool` role, when present, selects a mounted tool.
    """
    deployment = _scenario_file(scenario, "deployment.yaml", report)
    if deployment is None:
        return
    all_components = deployment.get("components", [])
    components = [
        component for component in all_components
        if component.get("component_profile_id") in models
    ]
    if not components:
        report.error("component_scenario_mismatch", str(scenario),
                     f"no component selects the model {descriptor.id}")
        return
    description = _read_description(root, report) or {}
    family_of = _model_families(root, description)
    model_families = {
        row.get("model_family_id"): row
        for row in description.get("model_families", [])
        if isinstance(row, dict) and isinstance(row.get("model_family_id"), str)
    }
    by_id = {component.get("component_id"): component for component in all_components}
    attachments = deployment.get("attachments", [])
    children = {row.get("child", {}).get("component_id") for row in attachments}
    gripper = descriptor.data.get("gripper")
    gripper_group = descriptor.data.get("roles", {}).get("groups", {}).get("gripper")
    mount_frame = descriptor.data["mount"]["frame"]
    before = len(report.errors)
    for component in components:
        component_id = component.get("component_id", "?")
        where = f"{scenario.name} component {component_id}"

        def fail(message: str, code: str = "component_scenario_mismatch") -> None:
            report.error(code, where, message)

        if "root_pose_in_cell" in component:
            fail("a mounted tool omits root_pose_in_cell; its attachments row places it",
                 "component_attachment_invalid")
        mounts = [row for row in attachments
                  if row.get("child", {}).get("component_id") == component_id]
        if len(mounts) != 1:
            fail(f"needs exactly one attachments row {{parent: {{component_id: <robot>, "
                 f"interface: flange}}, child: {{component_id: {component_id}, interface: "
                 f"mount}}}}, got {len(mounts)}", "component_attachment_invalid")
        for row in mounts[:1]:
            parent = row.get("parent", {})
            parent_id = parent.get("component_id")
            if row.get("child", {}).get("interface") != "mount":
                fail("attachments child.interface must be 'mount' (the tool's root), got "
                     f"{row.get('child', {}).get('interface')!r}",
                     "component_attachment_invalid")
            if parent.get("interface") != "flange":
                fail(f"attachments parent.interface must be 'flange', got "
                     f"{parent.get('interface')!r}", "component_attachment_invalid")
            elif parent_id not in by_id:
                fail(f"attachments parent {parent_id!r} is not a component of the Scenario",
                     "component_attachment_invalid")
            elif "flange" not in _attachment_interfaces(
                root, family_of.get(by_id[parent_id].get("component_profile_id"))
            ):
                fail(f"attachments parent {parent_id!r} has no 'flange' interface (its "
                     "component type offers none)", "component_attachment_invalid")
            else:
                on_flange = [
                    other.get("child", {}).get("component_id") for other in attachments
                    if other.get("parent", {}).get("component_id") == parent_id
                    and other.get("parent", {}).get("interface") == "flange"
                ]
                if len(on_flange) != 1:
                    fail(f"the flange of {parent_id!r} is not free: it carries "
                         f"{sorted(map(str, on_flange))}", "component_attachment_invalid")
        runtime = component.get("configuration", {}).get("runtime_configuration", {})
        family = family_of.get(component["component_profile_id"])
        family_row = model_families.get(family)
        registered_source = family_row.get("source") if isinstance(family_row, dict) else None
        model_source = component.get("model_source")
        if type(model_source) is not dict:
            fail("model_source is required and must match the registered model source")
        else:
            if not {"urdf", "package_roots"} <= set(model_source) or set(model_source) - {
                "urdf", "srdf", "package_roots"
            }:
                fail("model_source must contain urdf and package_roots, with only optional srdf")
            roots = model_source.get("package_roots")
            if type(roots) is not dict:
                fail("model_source.package_roots must be a mapping")
                roots = {}
            elif len(roots) > 128:
                fail("model_source.package_roots exceeds the bound")
            for package_name, reference in roots.items():
                if (not isinstance(package_name, str)
                        or re.fullmatch(r"[a-z][a-z0-9_-]{0,127}", package_name) is None):
                    fail("model_source.package_roots has an invalid package name")
                if _scenario_source_path(reference, scenario) is None:
                    fail(f"model_source.package_roots.{package_name} is not a Deployment source reference")

            if not isinstance(registered_source, dict):
                fail(f"profile {component.get('component_profile_id')!r} has no registered model source")
            else:
                for field, registered_field in (("urdf", "urdf_path"), ("srdf", "srdf_path")):
                    registered_path = registered_source.get(registered_field)
                    reference = model_source.get(field)
                    if field == "srdf" and field not in model_source:
                        continue
                    if not isinstance(registered_path, str):
                        if reference is not None:
                            fail(f"model_source.{field} has no registered {registered_field}")
                        continue
                    resolved = _scenario_source_path(reference, scenario)
                    expected = Path(os.path.abspath(root / registered_path))
                    if resolved != expected:
                        fail(f"model_source.{field} must resolve to the registered source path {registered_path!r}")
                prefix = registered_source.get("accepted_package_uri_prefix")
                package = (prefix[len("package://"):].split("/", 1)[0]
                           if isinstance(prefix, str) and prefix.startswith("package://")
                           else None)
                if package is not None:
                    package_path = _scenario_source_path(roots.get(package), scenario)
                    if package_path != Path(os.path.abspath(root / package)):
                        fail(f"model_source.package_roots.{package} must resolve to its registered package path")
        # A pneumatic tool may name no lane: it runs no controller (D4).
        lane_free = "lane" not in component and (gripper or {}).get("actuation") == "pneumatic"
        if not lane_free and component.get("lane") not in descriptor.data.get("lanes", {}):
            fail(f"lane {component.get('lane')!r} is not a lane of {descriptor.id}",
                 "component_lane_invalid")
        streams = sorted(row.get("stream_id") for row in runtime.get("component_streams", []))
        # A lane-free (pneumatic) tool runs no controller, so it has no streams.
        expected_streams = [] if lane_free else ["gripper_feedback", "measured_joints"]
        if streams != expected_streams:
            fail(f"component_streams must declare exactly {expected_streams}, got {streams}")
        grippers = runtime.get("gripper_target_configurations", [])
        if gripper is not None and not grippers:
            fail("the tool has a gripper but gripper_target_configurations is empty")
        for row in grippers:
            if row.get("controllable_component_id") != component_id:
                fail(f"gripper row names {row.get('controllable_component_id')!r}; a tool's "
                     f"gripper row names the tool {component_id!r}")
            if row.get("mount_frame") != mount_frame:
                fail(f"gripper mount_frame {row.get('mount_frame')!r} must be mount.frame "
                     f"{mount_frame!r}")
        _check_gripper_rows(grippers, model, gripper_group, fail)
    assignment = (
        _scenario_file(scenario, "assignment.yaml", report)
        if (scenario / "assignment.yaml").is_file()
        else None
    ) or {}
    for role in assignment.get("component_roles", []):
        selected = role.get("controllable_component_id")
        if role.get("component_role_id") == "tool" and selected not in children:
            report.error("component_attachment_invalid", f"{scenario.name} assignment.yaml",
                         f"role 'tool' selects {selected!r}, which is not a mounted tool; "
                         "select the tool component")
    if len(report.errors) == before:
        report.note(f"{scenario.name}: {len(components)} component(s) of {descriptor.id} are "
                    "mounted and agree with the descriptor")


def check(
    root: Path, folder: Path, scenario: Path | None = None
) -> tuple[Report, Descriptor | None]:
    """Everything `manyforge component check` reports for one folder."""
    report = Report()
    descriptor = load_descriptor(root, folder, report)
    if descriptor is None or report.errors:
        return report, descriptor
    model = None
    if descriptor.kind in ("robot", "tool"):
        model = check_model(root, descriptor, report)
    if descriptor.kind == "robot":
        report.note(f"{descriptor.id}: limits.max_tcp_linear_speed_mps "
                    f"{descriptor.data['limits']['max_tcp_linear_speed_mps']} m/s must come "
                    "from the robot's datasheet; checking it is your responsibility, and "
                    "ManyForge enforces whatever value is entered")
    if model is not None:
        models = check_registration(root, descriptor, report)
        if descriptor.kind == "robot":
            if scenario is not None and models:
                check_scenario(scenario, descriptor, model, models, report)
        elif scenario is not None and models:
            check_tool_scenario(root, scenario, descriptor, model, models, report)
    elif descriptor.kind in ("io_device", "machine", "sensor") and scenario is not None:
        report.error("component_manifest_invalid", str(scenario),
                     "--scenario checks a robot's or a tool's components only")
    if descriptor.kind == "machine":
        check_machine_members(root, descriptor, report)
    if descriptor.kind in ("machine", "sensor") or is_io_model_form(descriptor.data):
        check_component_model_registration(root, descriptor, report)
    if descriptor.kind == "io_device" and not report.errors:
        report.note(f"{descriptor.id}: {len(descriptor.data['signals'])} signal(s)")
    return report, descriptor


# --- authoring ------------------------------------------------------------


def new_component(
    root: Path, kind: str, component_id: str, into: Path | None = None
) -> tuple[Report, Path | None]:
    """A component folder from `components/templates/<kind>/`."""
    report = Report()
    if kind not in KINDS or kind in ("simulator", "sensor"):
        report.error("component_manifest_invalid", "kind",
                     f"must be one of robot, tool, io_device, machine, got {kind!r}")
    if not _ID.fullmatch(component_id):
        report.error("component_manifest_invalid", "id",
                     "must be <vendor>.<name>, each a lower-case portable id "
                     f"([a-z][a-z0-9_]*), got {component_id!r}")
    if report.errors:
        return report, None
    name = component_id.split(".", 1)[1]
    if into is not None:
        folder = into
    elif kind in ("robot", "tool"):
        folder = root / ROBOT_ROOT / name
    else:
        folder = root / COMPONENT_ROOT / kind / component_id
    if (folder / DESCRIPTOR).exists():
        report.error("component_folder_exists", _display(root, folder / DESCRIPTOR),
                     "already exists; edit it instead")
        return report, None
    folder.mkdir(parents=True, exist_ok=True)
    for source in sorted((root / TEMPLATES / kind).iterdir()):
        target = folder / source.name
        # Model files or a README already in the folder are the author's.
        if source.is_file() and not target.exists():
            target.write_text(
                source.read_text(encoding="utf-8")
                .replace("__ID__", component_id)
                .replace("__NAME__", name),
                encoding="utf-8",
            )
    shown = _display(root, folder)
    report.note(f"created {shown}/{DESCRIPTOR}; fill it in ({GUIDE}), then run "
                f"`manyforge component check {shown}`")
    return report, folder


def _json_value_text(value: Any, indent: int, element: bool = False) -> str:
    """The maintained description's layout: scalar lists, and scalar objects
    inside a list, on one line; any other container one entry per line."""
    if not isinstance(value, (dict, list)) or not value:
        return json.dumps(value)
    if isinstance(value, list) and not any(isinstance(item, (dict, list)) for item in value):
        return json.dumps(value)
    if element and isinstance(value, dict) and not any(
        isinstance(item, (dict, list)) for item in value.values()
    ):
        return json.dumps(value)
    pad = " " * (indent + 2)
    if isinstance(value, list):
        items = [pad + _json_value_text(item, indent + 2, element=True) for item in value]
        return "[\n" + ",\n".join(items) + "\n" + " " * indent + "]"
    items = []
    for key, item in value.items():
        if key == "ros2_control_runtime" and isinstance(item, dict) and item:
            # A profile row's runtime fields each keep one line.
            fields = [f"{pad}  {json.dumps(name)}: {json.dumps(field_value)}"
                      for name, field_value in item.items()]
            items.append(f"{pad}{json.dumps(key)}: {{\n" + ",\n".join(fields) + f"\n{pad}}}")
        else:
            items.append(f"{pad}{json.dumps(key)}: {_json_value_text(item, indent + 2)}")
    return "{\n" + ",\n".join(items) + "\n" + " " * indent + "}"


def _with_json_row(text: str, key: str, id_key: str, row: dict[str, Any]) -> str:
    """The description text with one row of the top-level list `key` replaced in
    place, or inserted in id order; every other byte kept."""
    decoder = json.JSONDecoder()
    index = text.index(f'"{key}": [') + len(f'"{key}": [')
    spans: list[tuple[int, int, str]] = []
    while True:
        while text[index] in " \n,":
            index += 1
        if text[index] == "]":
            break
        found, end = decoder.raw_decode(text, index)
        spans.append((index, end, found[id_key]))
        index = end
    row_text = _json_value_text(row, 4)
    for start, end, row_id in spans:
        if row_id == row[id_key]:
            return text[:start] + row_text + text[end:]
        if row_id > row[id_key]:
            return text[:start] + row_text + ",\n    " + text[start:]
    end = spans[-1][1]
    return text[:end] + ",\n    " + row_text + text[end:]


def _register_component_model(
    root: Path, descriptor: Descriptor, report: Report
) -> tuple[Report, list[str]]:
    """List a machine's, a model-form I/O device's or a sensor's descriptor in
    the description's sorted `component_models` (after `api_version`)."""
    path = _descriptor_path(root, descriptor)
    if path is None:
        report.error("component_not_registered", descriptor.label,
                     f"the folder is outside the workspace {root}")
        return report, []
    target = root / DESCRIPTION
    text = target.read_text(encoding="utf-8")
    listed = json.loads(text).get(COMPONENT_MODELS)
    if listed is not None and path in listed:
        return report, []
    paths = sorted([*(listed or []), path])
    row_text = "[\n" + ",\n".join(f"    {json.dumps(item)}" for item in paths) + "\n  ]"
    if listed is None:
        anchor = '"model_families": ['
        index = text.index(anchor)
        text = text[:index] + f'"{COMPONENT_MODELS}": {row_text},\n  ' + text[index:]
    else:
        start = text.index(f'"{COMPONENT_MODELS}": [')
        end = text.index("]", start) + 1
        text = text[:start] + f'"{COMPONENT_MODELS}": {row_text}' + text[end:]
    target.write_text(text, encoding="utf-8")
    return report, [f"{DESCRIPTION}: {COMPONENT_MODELS} entry {path}"]


def register(root: Path, folder: Path) -> tuple[Report, list[str]]:
    """Write a robot's or tool's model family row, naming its descriptor
    (`component_descriptor`); list a machine, a model-form I/O device or a
    sensor in `component_models`. Lane rows stay in the descriptor, which the runtime
    reads. Writes nothing when the row already matches."""
    report = Report()
    descriptor = load_descriptor(root, folder, report)
    if descriptor is None or report.errors:
        return report, []
    if descriptor.kind in ("machine", "sensor") or is_io_model_form(descriptor.data):
        return _register_component_model(root, descriptor, report)
    if descriptor.kind not in ("robot", "tool"):
        report.note(f"{descriptor.id}: a {descriptor.kind} descriptor has no rows to register")
        return report, []
    check_model(root, descriptor, report)
    expected = family_row(root, descriptor)
    if expected is None:
        report.error("component_not_registered", descriptor.label,
                     f"the folder is outside the workspace {root}; a {descriptor.kind} runs "
                     f"from {ROBOT_ROOT}/<name>/ today")
    if report.errors:
        return report, []
    name = descriptor.name
    written: list[str] = []
    path = root / DESCRIPTION
    text = path.read_text(encoding="utf-8")
    # Only the rows that change are written; every other row keeps its bytes.
    updated = text
    description = json.loads(text)
    families = description["model_families"]
    existing = next((row for row in families if row["model_family_id"] == name), None)
    if existing is not None:
        # A curated row may read a projected model (historical paths and
        # projection kinds); the descriptor and its URI roots decide the source.
        source = dict(existing["source"])
        source.update({key: expected["source"][key] for key in _DERIVED_SOURCE})
        expected["source"] = dict(sorted(source.items()))
    # The same match as `check`: binding and collision-link order are not a change.
    if existing is None or _family_differences(existing, expected):
        updated = _with_json_row(updated, "model_families", "model_family_id", expected)
        written.append(f"{DESCRIPTION}: model family row {name}")
    if updated != text:
        path.write_text(updated, encoding="utf-8")
    return report, written


# --- the curated list -----------------------------------------------------


def _runtime(data: dict[str, Any]) -> dict[str, Any]:
    """What a Deployment component of this descriptor selects: its model id
    (`component_profile_id`) and one of its lanes; a pneumatic tool may name
    none (`lane_free`, D4)."""
    component_id = data.get("id")
    if is_io_model_form(data):
        # Only a machine member: its model is its profile.
        return {"componentProfileId": component_id}
    if data.get("kind") == "io_device":
        return {"componentProfileId": IO_DEVICE_PROFILE}
    if data.get("kind") == "machine":
        return {"componentProfileId": component_id}
    if data.get("kind") == "sensor":
        # One adapter on every lane.
        return {"componentProfileId": component_id, "lanes": sorted(LANES)}
    if (
        data.get("kind") not in ("robot", "tool")
        or not isinstance(component_id, str)
        or not _ID.fullmatch(component_id)
    ):
        return {}
    lanes = sorted(data.get("lanes") or {})
    if data.get("kind") == "tool" and (data.get("gripper") or {}).get("actuation") == "pneumatic":
        lanes = sorted([*lanes, "lane_free"])
    return {
        "modelFamilyId": component_id.split(".", 1)[1],
        "componentProfileId": component_id,
        "lanes": lanes,
    }


def component_folders(root: Path) -> list[Path]:
    """Every curated component folder: robots first, then components/<kind>/."""
    folders = sorted(path.parent for path in (root / ROBOT_ROOT).glob(f"*/{DESCRIPTOR}"))
    for kind in KINDS:
        folders += sorted(
            path.parent for path in (root / COMPONENT_ROOT / kind).glob(f"*/{DESCRIPTOR}")
        )
    return folders


def _machine_runtime(data: dict[str, Any], models: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """A machine runs on its arm member's lanes; its members."""
    members = [m for m in data.get("members") or [] if isinstance(m, dict)]
    arm = next((m for m in members
                if (models.get(m.get("model")) or {}).get("kind") == "robot"), None)
    lanes = sorted((models.get(arm["model"]) or {}).get("lanes") or {}) if arm else []
    return {
        "componentProfileId": data.get("id"),
        "lanes": lanes,
        "members": [{"role": m.get("role"), "modelId": m.get("model")} for m in members],
    }


def list_components(root: Path) -> dict[str, Any]:
    """The read-only list of curated components, each checked as by `check`.

    A component with errors is listed with them and is not addable. Two
    folders with the same id and version but different bytes are both in
    error; with the same bytes the second is left out. A robot is addable
    with a Mock lane, a tool with a Mock lane or lane-free, a machine when its
    arm has a Mock lane, a sensor always (it has no lane); a
    machine's member model (its arm robot, a
    model-form I/O device) only as a machine member (AUD-5).
    """
    entries: list[dict[str, Any]] = []
    seen: dict[tuple[str, str], tuple[str, dict[str, Any], str]] = {}
    models = _curated_models(root)
    member_models = {member.get("model") for model in models.values()
                     if model.get("kind") == "machine"
                     for member in model.get("members") or [] if isinstance(member, dict)}
    for folder in component_folders(root):
        report, descriptor = check(root, folder)
        data = descriptor.data if descriptor is not None else {}
        path = _display(root, folder)
        key = (str(data.get("id")), str(data.get("version")))
        if descriptor is not None and key in seen:
            text, other, other_path = seen[key]
            if text == descriptor.text:
                continue
            message = f"{key[0]} {key[1]} is also in {{}} with different content"
            other["errors"].append(ComponentError(
                "component_package_conflict", other_path, message.format(path)).as_dict())
            other["addable"] = False
            report.error("component_package_conflict", path, message.format(other_path))
        entry = {
            "id": data.get("id"),
            "kind": data.get("kind"),
            "version": data.get("version"),
            "title": data.get("title"),
            "path": path,
            "descriptor": data or None,
            "runtime": (_machine_runtime(data, models) if data.get("kind") == "machine"
                        else _runtime(data)),
            "addable": False,
            "errors": [error.as_dict() for error in report.errors],
        }
        if descriptor is not None and not report.errors:
            lanes = entry["runtime"].get("lanes", [])
            entry["addable"] = (
                (descriptor.kind == "io_device" and not is_io_model_form(data))
                or descriptor.kind in ("simulator", "sensor")
                or (descriptor.kind == "machine" and "mock" in lanes)
                or (descriptor.kind == "robot" and "mock" in lanes
                    and descriptor.id not in member_models)
                or (descriptor.kind == "tool" and ("mock" in lanes or "lane_free" in lanes))
            )
        if descriptor is not None:
            seen.setdefault(key, (descriptor.text, entry, path))
        entries.append(entry)
    entries.sort(key=lambda entry: (str(entry["kind"]), str(entry["id"]), str(entry["version"])))
    return {"schemaVersion": LIST_SCHEMA, "components": entries}
