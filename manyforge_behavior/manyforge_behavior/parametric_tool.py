"""Parametric tools: parameters to a URDF and an SRDF (OD-352, OD-357).

A tool of a generic family is described by its parameters
(``api_version: manyforge.parametric_tool.v1``): inline in the Deployment
row's ``model_source.parameters``, or a parameter file in the project
folder that the row references. Capture (``model_source_capture``) generates
the model from them at Load, and ``manyforge component export`` (or the
Composer's Export) writes the same bytes into a folder.

``kind: tool`` (family ``manyforge.tool``, the one kind) is a Tool: one
component, mounted as a whole. Chains of fixed elements lead from its body to its tool ends (a
jaw pair, a spring slider or a probe tip); each TCP belongs to a tool end or
to the body. Its model uses ``tool_*`` links, and its component semantics (collision bodies, tool ends, TCPs
with their contact bodies, one gripper target per jaw pair, spring-slider
metadata) come with the generated model; Load derives the component type
from them.

A mesh path follows the project path rule (OD-335, OD-364): relative to the
parameter file's folder (inline parameters: the Scenario folder; ``..`` is
allowed) or absolute, inside an allowed folder. The caller resolves and reads
the meshes and tells ``generate`` the file name each mesh path becomes in the
URDF: Load names the captured content (``package://tool/meshes/<digest>``),
Export the file relative to the export folder. Generation is a pure function
of the parameters and those names: the same parameters and the same mesh
content give the same bytes (fixed number format, no times in the output).
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import PurePosixPath
import hashlib
import re
from typing import Any, Final, Mapping, Sequence
from xml.sax.saxutils import quoteattr

from .canonical_artifact import StrictArtifactError, load_strict_json_yaml_bytes


API_VERSION: Final = "manyforge.parametric_tool.v1"
GENERATOR_VERSION: Final = 1
TOOL: Final = "tool"
KINDS: Final = (TOOL,)
TOOL_FAMILY: Final = "tool"  # the generated tools' family, `manyforge.tool`
PACKAGE_NAME: Final = "tool"  # the family's `package://tool/`
ROBOT_NAME: Final = "tool"  # also the exported files' stem
DEFAULT_TCP_ID: Final = "tcp"
MAX_PARAMETER_BYTES: Final = 1024 * 1024
MAX_PARTS: Final = 256
MAX_TCPS: Final = 64
MESH_SUFFIXES: Final = (".stl", ".dae")
EXPORT_FORMATS: Final = ("urdf", "srdf", "usda")
_ID: Final = re.compile(r"[a-z][a-z0-9_]{0,63}")
_GEOMETRY_KINDS: Final = ("box", "cylinder", "sphere", "mesh")


class ParametricToolError(ValueError):
    """A parameter file is refused; ``code`` is its error-catalog code and
    ``field`` the refused field's path in the parameters (for example
    ``chains[0].elements[1].jaw_pair.max_aperture_m``), when one is known."""

    def __init__(self, code: str, message: str, field: str | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.field = field


def _bad_field(where: str, message: str) -> ParametricToolError:
    return ParametricToolError("parametric_tool_invalid", f"{where}: {message}", where)


# --- validation -------------------------------------------------------------


Vector = tuple[float, float, float]
Quaternion = tuple[float, float, float, float]  # x, y, z, w


@dataclass(frozen=True)
class Pose:
    position: Vector = (0.0, 0.0, 0.0)
    rotation: Quaternion = (0.0, 0.0, 0.0, 1.0)


@dataclass(frozen=True)
class Geometry:
    kind: str
    values: tuple[tuple[str, Any], ...]  # sorted (name, value) pairs

    def get(self, name: str) -> Any:
        return dict(self.values)[name]


@dataclass(frozen=True)
class Part:
    name: str
    pose: Pose
    collision: Geometry | None
    collision_pose: Pose
    visual: Geometry | None
    visual_pose: Pose
    mass_kg: float | None
    center_of_mass_m: Vector


def _mapping(value: Any, where: str, required: Sequence[str],
             optional: Sequence[str] = ()) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise _bad_field(where, "must be a mapping")
    missing = [name for name in required if name not in value]
    if missing:
        raise _bad_field(where, f"is missing {', '.join(missing)}")
    unknown = sorted(set(value) - set(required) - set(optional))
    if unknown:
        raise _bad_field(
            where,
            f"has unknown field(s) {', '.join(map(str, unknown))} "
            f"(allowed: {', '.join((*required, *optional))})",
        )
    return value


def _number(value: Any, where: str, *, positive: bool = False,
            non_negative: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _bad_field(where, f"must be a number, got {value!r}")
    result = float(value)
    if not math.isfinite(result):
        raise _bad_field(where, "must be finite")
    if positive and result <= 0.0:
        raise _bad_field(where, f"must be > 0, got {value!r}")
    if non_negative and result < 0.0:
        raise _bad_field(where, f"must be >= 0, got {value!r}")
    return result


def _numbers(value: Any, where: str, count: int, **rules: bool) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != count:
        raise _bad_field(where, f"must be a list of {count} numbers")
    return tuple(_number(item, f"{where}[{index}]", **rules)
                 for index, item in enumerate(value))


def _identifier(value: Any, where: str) -> str:
    if not isinstance(value, str) or not _ID.fullmatch(value):
        raise _bad_field(where, f"must be a lower-case id ([a-z][a-z0-9_]*), got {value!r}")
    return value


def _quaternion_from_rpy(roll: float, pitch: float, yaw: float) -> Quaternion:
    # Fixed-axis X-Y-Z (URDF): R = Rz(yaw) Ry(pitch) Rx(roll).
    cr, sr = math.cos(roll / 2.0), math.sin(roll / 2.0)
    cp, sp = math.cos(pitch / 2.0), math.sin(pitch / 2.0)
    cy, sy = math.cos(yaw / 2.0), math.sin(yaw / 2.0)
    return (
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    )


def _pose(value: Any, where: str) -> Pose:
    if value is None:
        return Pose()
    fields = _mapping(value, where, (), ("position_m", "orientation"))
    position = (
        _numbers(fields["position_m"], f"{where}.position_m", 3)
        if "position_m" in fields else (0.0, 0.0, 0.0)
    )
    rotation: Quaternion = (0.0, 0.0, 0.0, 1.0)
    if "orientation" in fields:
        orientation = fields["orientation"]
        if not isinstance(orientation, dict) or len(orientation) != 1 or not (
            set(orientation) <= {"rpy_deg", "xyzw"}
        ):
            raise _bad_field(f"{where}.orientation", "must be exactly one of rpy_deg or xyzw")
        if "rpy_deg" in orientation:
            roll, pitch, yaw = _numbers(orientation["rpy_deg"],
                                        f"{where}.orientation.rpy_deg", 3)
            rotation = _quaternion_from_rpy(
                math.radians(roll), math.radians(pitch), math.radians(yaw)
            )
        else:
            x, y, z, w = _numbers(orientation["xyzw"], f"{where}.orientation.xyzw", 4)
            norm = math.sqrt(x * x + y * y + z * z + w * w)
            if norm == 0.0:
                raise _bad_field(f"{where}.orientation.xyzw", "must not have norm 0")
            rotation = (x / norm, y / norm, z / norm, w / norm)
    return Pose(position, rotation)  # type: ignore[arg-type]


def _mesh_path(value: Any, where: str) -> str:
    """A mesh path as written; where it resolves is the project path rule's
    (``model_source_capture.resolve_tool_meshes``)."""

    if not isinstance(value, str) or not value.strip():
        raise _bad_field(where, "must be a file path")
    if PurePosixPath(value).suffix.casefold() not in MESH_SUFFIXES:
        raise _bad_field(where, f"mesh {value!r} must be an .stl or .dae file")
    return value


def _geometry(value: Any, where: str) -> Geometry:
    if not isinstance(value, dict) or len(value) != 1 or next(iter(value)) not in _GEOMETRY_KINDS:
        raise _bad_field(where, f"must be exactly one of {', '.join(_GEOMETRY_KINDS)}")
    kind, raw = next(iter(value.items()))
    at = f"{where}.{kind}"
    if kind == "box":
        fields = _mapping(raw, at, ("size_m",))
        values = {"size_m": _numbers(fields["size_m"], f"{at}.size_m", 3, positive=True)}
    elif kind == "cylinder":
        fields = _mapping(raw, at, ("radius_m", "length_m"))
        values = {
            "radius_m": _number(fields["radius_m"], f"{at}.radius_m", positive=True),
            "length_m": _number(fields["length_m"], f"{at}.length_m", positive=True),
        }
    elif kind == "sphere":
        fields = _mapping(raw, at, ("radius_m",))
        values = {"radius_m": _number(fields["radius_m"], f"{at}.radius_m", positive=True)}
    else:
        fields = _mapping(raw, at, ("path",), ("scale",))
        values = {
            "path": _mesh_path(fields["path"], f"{at}.path"),
            "scale": (_numbers(fields["scale"], f"{at}.scale", 3, positive=True)
                      if "scale" in fields else (1.0, 1.0, 1.0)),
        }
    return Geometry(kind, tuple(sorted(values.items())))


def _part(value: Any, where: str, *, collision_required: bool) -> Part:
    fields = _mapping(
        value, where, ("name",),
        ("pose", "collision", "visual", "collision_pose", "visual_pose",
         "mass_kg", "center_of_mass_m"),
    )
    name = _identifier(fields["name"], f"{where}.name")
    collision = (_geometry(fields["collision"], f"{where}.collision")
                 if "collision" in fields else None)
    if "visual" not in fields:
        visual = collision
    elif fields["visual"] == "none":
        visual = None
    else:
        visual = _geometry(fields["visual"], f"{where}.visual")
    if collision is None and visual is None:
        raise _bad_field(where, "needs collision or visual geometry")
    if collision_required and collision is None:
        raise _bad_field(where, "needs collision geometry")
    if "center_of_mass_m" in fields and "mass_kg" not in fields:
        raise _bad_field(f"{where}.center_of_mass_m", "needs the part's mass_kg")
    return Part(
        name=name,
        pose=_pose(fields.get("pose"), f"{where}.pose"),
        collision=collision,
        collision_pose=_pose(fields.get("collision_pose"), f"{where}.collision_pose"),
        visual=visual,
        visual_pose=_pose(fields.get("visual_pose"), f"{where}.visual_pose"),
        mass_kg=(_number(fields["mass_kg"], f"{where}.mass_kg", non_negative=True)
                 if "mass_kg" in fields else None),
        center_of_mass_m=(
            _numbers(fields["center_of_mass_m"], f"{where}.center_of_mass_m", 3)
            if "center_of_mass_m" in fields else (0.0, 0.0, 0.0)
        ),  # type: ignore[arg-type]
    )


def load_parameters(raw: bytes, *, source: str) -> "ToolParameters":
    """Parse and validate one parameter file's bytes."""

    if len(raw) > MAX_PARAMETER_BYTES:
        raise ParametricToolError(
            "parametric_tool_invalid",
            f"{source}: exceeds {MAX_PARAMETER_BYTES} bytes",
        )
    try:
        document = load_strict_json_yaml_bytes(raw, source=source)
    except StrictArtifactError as error:
        raise ParametricToolError("parametric_tool_invalid", str(error)) from error
    return load_parameter_document(document, source=source)


def is_parameter_document(value: Any) -> bool:
    """Whether a Deployment row's ``model_source.parameters`` holds the
    parameters inline (a mapping with ``api_version``), not a reference to a
    parameter file (``project_path`` or ``external_path``)."""

    return isinstance(value, Mapping) and "api_version" in value


def load_parameter_document(document: Any, *, source: str) -> "ToolParameters":
    """Validate parameters given as a mapping: a parsed parameter file, or
    the parameters inline in a Deployment row (``model_source.parameters``)."""

    if not isinstance(document, dict):
        raise ParametricToolError("parametric_tool_invalid", f"{source}: must be a mapping")
    if document.get("api_version") != API_VERSION:
        raise ParametricToolError(
            "parametric_tool_invalid",
            f"{source}: api_version must be {API_VERSION!r}, "
            f"got {document.get('api_version')!r}",
            "api_version",
        )
    if document.get("kind") not in KINDS:
        raise ParametricToolError(
            "parametric_tool_kind_unsupported",
            f"{source}: kind {document.get('kind')!r} is not supported "
            f"(supported: {', '.join(KINDS)})",
            "kind",
        )
    try:
        return _tool(document)
    except ParametricToolError as error:
        raise ParametricToolError(error.code, f"{source}: {error}", error.field) from error


# --- generation -------------------------------------------------------------


def _number_text(value: float) -> str:
    rounded = round(value, 12)
    if rounded == 0.0:
        return "0"
    return format(rounded, ".12g")


def _vector_text(values: Sequence[float]) -> str:
    return " ".join(_number_text(value) for value in values)


def _multiply(left: Quaternion, right: Quaternion) -> Quaternion:
    x1, y1, z1, w1 = left
    x2, y2, z2, w2 = right
    return (
        w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
        w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
        w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
        w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
    )


def _rotate(rotation: Quaternion, vector: Vector) -> Vector:
    x, y, z, w = _multiply(_multiply(rotation, (*vector, 0.0)),
                           (-rotation[0], -rotation[1], -rotation[2], rotation[3]))
    del w
    return (x, y, z)


def _compose(parent: Pose, child: Pose) -> Pose:
    moved = _rotate(parent.rotation, child.position)
    return Pose(
        tuple(a + b for a, b in zip(parent.position, moved)),  # type: ignore[arg-type]
        _multiply(parent.rotation, child.rotation),
    )


def _rpy(rotation: Quaternion) -> Vector:
    x, y, z, w = rotation
    sin_pitch = max(-1.0, min(1.0, 2.0 * (w * y - z * x)))
    return (
        math.atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y)),
        math.asin(sin_pitch),
        math.atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z)),
    )


def _origin(pose: Pose) -> str:
    return (f'<origin xyz="{_vector_text(pose.position)}" '
            f'rpy="{_vector_text(_rpy(pose.rotation))}"/>')


def _geometry_xml(geometry: Geometry, mesh_files: Mapping[str, str]) -> str:
    if geometry.kind == "box":
        return f'<box size="{_vector_text(geometry.get("size_m"))}"/>'
    if geometry.kind == "cylinder":
        return (f'<cylinder radius="{_number_text(geometry.get("radius_m"))}" '
                f'length="{_number_text(geometry.get("length_m"))}"/>')
    if geometry.kind == "sphere":
        return f'<sphere radius="{_number_text(geometry.get("radius_m"))}"/>'
    return (f"<mesh filename={quoteattr(mesh_files[geometry.get('path')])} "
            f'scale="{_vector_text(geometry.get("scale"))}"/>')


def _half_extent(geometry: Geometry) -> Vector:
    """Axis-aligned half extent of a primitive in its own frame (0 for a mesh)."""

    if geometry.kind == "box":
        return tuple(value / 2.0 for value in geometry.get("size_m"))  # type: ignore[return-value]
    if geometry.kind == "cylinder":
        radius = geometry.get("radius_m")
        return (radius, radius, geometry.get("length_m") / 2.0)
    if geometry.kind == "sphere":
        radius = geometry.get("radius_m")
        return (radius, radius, radius)
    return (0.0, 0.0, 0.0)


def _inertial(parts: Sequence[tuple[Part, Pose]], mass: float,
              center: Vector) -> list[str]:
    """One `<inertial>`: mass and centre of mass, with the inertia of a
    solid box spanning the parts' collision primitives (the payload uses the
    mass and centre of mass only)."""

    low = [math.inf] * 3
    high = [-math.inf] * 3
    for part, pose in parts:
        if part.collision is None:
            continue
        frame = _compose(pose, part.collision_pose)
        half = _half_extent(part.collision)
        radius = math.sqrt(sum(value * value for value in half))
        for axis in range(3):
            low[axis] = min(low[axis], frame.position[axis] - radius)
            high[axis] = max(high[axis], frame.position[axis] + radius)
    size = [high[axis] - low[axis] if high[axis] >= low[axis] else 0.0
            for axis in range(3)]
    sx, sy, sz = (max(value, 1e-3) for value in size)
    return [
        "    <inertial>",
        f'      <origin xyz="{_vector_text(center)}" rpy="0 0 0"/>',
        f'      <mass value="{_number_text(mass)}"/>',
        f'      <inertia ixx="{_number_text(mass * (sy * sy + sz * sz) / 12.0)}" '
        'ixy="0" ixz="0" '
        f'iyy="{_number_text(mass * (sx * sx + sz * sz) / 12.0)}" iyz="0" '
        f'izz="{_number_text(mass * (sx * sx + sy * sy) / 12.0)}"/>',
        "    </inertial>",
    ]


def _link(name: str, parts: Sequence[tuple[Part, Pose]],
          inertial: list[str], mesh_files: Mapping[str, str]) -> list[str]:
    lines = [f'  <link name="{name}">', *inertial]
    for part, pose in parts:
        if part.visual is not None:
            lines += [
                "    <visual>",
                f"      {_origin(_compose(pose, part.visual_pose))}",
                f"      <geometry>{_geometry_xml(part.visual, mesh_files)}</geometry>",
                "    </visual>",
            ]
        if part.collision is not None:
            lines += [
                "    <collision>",
                f"      {_origin(_compose(pose, part.collision_pose))}",
                f"      <geometry>{_geometry_xml(part.collision, mesh_files)}</geometry>",
                "    </collision>",
            ]
    return [*lines, "  </link>"]


def _fixed_joint(name: str, parent: str, child: str, pose: Pose) -> list[str]:
    return [
        f'  <joint name="{name}" type="fixed">',
        f'    <parent link="{parent}"/>',
        f'    <child link="{child}"/>',
        f"    {_origin(pose)}",
        "  </joint>",
    ]


def _part_mass(parts: Sequence[tuple[Part, Pose]]) -> tuple[float, Vector]:
    mass = sum(part.mass_kg or 0.0 for part, _pose_ in parts)
    if mass <= 0.0:
        return 0.0, (0.0, 0.0, 0.0)
    moment = [0.0, 0.0, 0.0]
    for part, pose in parts:
        if not part.mass_kg:
            continue
        point = _compose(pose, Pose(part.center_of_mass_m)).position
        for axis in range(3):
            moment[axis] += part.mass_kg * point[axis]
    return mass, tuple(value / mass for value in moment)  # type: ignore[return-value]


@dataclass(frozen=True)
class GeneratedTool:
    urdf: bytes
    srdf: bytes
    mesh_paths: tuple[str, ...]  # as written in the parameters
    robot_name: str = ROBOT_NAME  # also the exported files' stem
    # The generated component's declarations (``component_semantics``).
    component: Mapping[str, Any] | None = None


def _header() -> str:
    return (f"<!-- Generated by ManyForge from {API_VERSION} (generator "
            f"{GENERATOR_VERSION}); edit the parameter file, not this file. -->")


def generate(parameters: "ToolParameters", mesh_files: Mapping[str, str]) -> GeneratedTool:
    """The URDF and SRDF bytes and the component declarations for one
    parameter set; ``mesh_files`` maps each of ``mesh_paths(parameters)`` to
    the file name the URDF names it by."""

    return _generate_tool(parameters, mesh_files)


def captured_mesh_uri(path: str, content: bytes) -> str:
    """The URI Load names one mesh's captured content by: its digest, so the
    same content gives the same model wherever the file lies."""

    suffix = PurePosixPath(path).suffix.casefold()
    return f"package://{PACKAGE_NAME}/meshes/{hashlib.sha256(content).hexdigest()}{suffix}"


# --- the tool: body, chains, tool ends, TCPs --------------------------------
#
# A Tool is one component, mounted as a whole. Chains of fixed elements lead
# from its body to its tool ends: a jaw pair (actuated: a gripper command
# target under its id), a spring slider or a probe tip. Each TCP belongs to
# a tool end or to the body.

TOOL_MOUNT_LINK: Final = "tool_mount"
TOOL_BODY_LINK: Final = "tool_body"
TOOL_TCP_LINK: Final = "tool_tcp"
BODY_ROLE: Final = "body"
# The generated component's frame-only command target: the body, its fixed
# elements and its TCPs. A jaw pair is a gripper target under its own id.
BODY_TARGET: Final = "tool"
MAX_CHAINS: Final = 64
MAX_ELEMENTS: Final = 256
DEFAULT_COMMAND_DURATION_S: Final = 0.8
ELEMENT_KINDS: Final = ("fixed", "jaw_pair", "spring_slider", "tip")
_HALF_TURN_Z: Final = (0.0, 0.0, 1.0, 0.0)  # 180 degrees about z (xyzw)


@dataclass(frozen=True)
class FixedElement:
    element_id: str
    parent_link: str
    pose: Pose  # in the parent link (the chain pose, then the element pose)
    parts: tuple[Part, ...]  # in the element frame


@dataclass(frozen=True)
class TipElement:
    """A probe tip: a tool end whose parts are its TCPs' contact body."""

    element_id: str
    parent_link: str
    pose: Pose
    parts: tuple[Part, ...]


@dataclass(frozen=True)
class SpringSlider:
    element_id: str
    parent_link: str
    pose: Pose
    parts: tuple[Part, ...]
    axis: Vector  # unit, element frame: the rod's line, pointing out of the tool (a push moves the rod against it)
    stroke_m: float
    trip_depth_m: float
    # OD-356: the deepest compression increase between two samples 1 mm of
    # tool travel apart (a contact rule's max_compression_step_m).
    max_compression_step_m: float = 0.002
    # OD-358 `swept_space`: collision-only parts in the element frame that
    # cover every position the parts reach over the stroke; empty without.
    swept: tuple[Part, ...] = ()
    # OD-371: the compression at which a Mock input following the slider
    # releases (0 < release < trip_depth_m); None without.
    release_depth_m: float | None = None


@dataclass(frozen=True)
class JawPair:
    element_id: str
    parent_link: str
    pose: Pose  # the pair frame: the jaws move along its y axis
    positive_jaw: tuple[Part, ...]  # in the +y jaw's frame, at the closed position
    negative_jaw: tuple[Part, ...]  # in the -y jaw's frame (the pair frame turned 180 deg about z)
    positive_pads: tuple[Part, ...]
    negative_pads: tuple[Part, ...]
    min_aperture_m: float
    max_aperture_m: float
    open_aperture_m: float
    closed_aperture_m: float
    command_duration_s: float


Element = FixedElement | TipElement | SpringSlider | JawPair


@dataclass(frozen=True)
class ToolTcp:
    tcp_id: str
    tool_end: str | None  # the tool end it belongs to; None: the body
    parent_link: str
    pose: Pose
    contact_bodies: tuple[str, ...]  # collision body role ids


@dataclass(frozen=True)
class ToolParameters:
    body: tuple[Part, ...]
    elements: tuple[Element, ...]
    tcps: tuple[ToolTcp, ...]
    mass_kg: float | None
    center_of_mass_m: Vector


def _parts(value: Any, where: str, *, collision_required: bool,
           required: bool = True) -> tuple[Part, ...]:
    if not isinstance(value, list) or (required and not value) or len(value) > MAX_PARTS:
        raise _bad_field(where, f"must be a list of {1 if required else 0} to {MAX_PARTS} parts")
    parts = tuple(_part(row, f"{where}[{index}]", collision_required=collision_required)
                  for index, row in enumerate(value))
    if parts and not collision_required and not any(p.collision is not None for p in parts):
        raise _bad_field(where, "needs at least one part with collision geometry")
    return parts


def _turned(parts: Sequence[Part]) -> tuple[Part, ...]:
    """Parts given in the pair frame, placed in the -y jaw's frame."""

    turn = Pose((0.0, 0.0, 0.0), _HALF_TURN_Z)
    return tuple(
        Part(part.name, _compose(turn, part.pose), part.collision, part.collision_pose,
             part.visual, part.visual_pose, part.mass_kg, part.center_of_mass_m)
        for part in parts
    )


def _mirrored(parts: Sequence[Part], suffix: str) -> tuple[Part, ...]:
    """The +y jaw's parts as the -y jaw's: the same poses in the turned frame."""

    return tuple(
        Part(f"{part.name}_{suffix}", part.pose, part.collision, part.collision_pose,
             part.visual, part.visual_pose, part.mass_kg, part.center_of_mass_m)
        for part in parts
    )


def _jaw_pair(fields: Mapping[str, Any], where: str, element_id: str,
              parent: str, pose: Pose) -> JawPair:
    fields = _mapping(
        fields, where, ("id", "positive_jaw", "min_aperture_m", "max_aperture_m"),
        ("pose", "negative_jaw", "pads", "open_aperture_m", "closed_aperture_m",
         "command_duration_s"),
    )
    positive = _parts(fields["positive_jaw"], f"{where}.positive_jaw", collision_required=False)
    negative = (_turned(_parts(fields["negative_jaw"], f"{where}.negative_jaw",
                               collision_required=False))
                if "negative_jaw" in fields else _mirrored(positive, "negative"))
    positive_pads: tuple[Part, ...] = ()
    negative_pads: tuple[Part, ...] = ()
    if "pads" in fields:
        pads = _mapping(fields["pads"], f"{where}.pads", ("positive",), ("negative",))
        positive_pads = _parts(pads["positive"], f"{where}.pads.positive",
                               collision_required=True)
        negative_pads = (_turned(_parts(pads["negative"], f"{where}.pads.negative",
                                        collision_required=True))
                         if "negative" in pads else _mirrored(positive_pads, "negative"))
    low = _number(fields["min_aperture_m"], f"{where}.min_aperture_m", non_negative=True)
    high = _number(fields["max_aperture_m"], f"{where}.max_aperture_m", positive=True)
    if high <= low:
        raise _bad_field(f"{where}.max_aperture_m", "must be > min_aperture_m")
    if positive_pads and "closed_aperture_m" not in fields:
        raise _bad_field(f"{where}.closed_aperture_m", "is required with pads (the assumed "
                         "grasp aperture)")
    opened = (_number(fields["open_aperture_m"], f"{where}.open_aperture_m")
              if "open_aperture_m" in fields else high)
    closed = (_number(fields["closed_aperture_m"], f"{where}.closed_aperture_m")
              if "closed_aperture_m" in fields else low)
    if not low <= closed < opened <= high:
        raise _bad_field(f"{where}.open_aperture_m" if "open_aperture_m" in fields
                         else f"{where}.closed_aperture_m",
                         "needs min_aperture_m <= closed_aperture_m < "
                         "open_aperture_m <= max_aperture_m")
    return JawPair(
        element_id=element_id, parent_link=parent, pose=pose,
        positive_jaw=positive, negative_jaw=negative,
        positive_pads=positive_pads, negative_pads=negative_pads,
        min_aperture_m=low, max_aperture_m=high,
        open_aperture_m=opened, closed_aperture_m=closed,
        command_duration_s=(
            _number(fields["command_duration_s"], f"{where}.command_duration_s", positive=True)
            if "command_duration_s" in fields else DEFAULT_COMMAND_DURATION_S),
    )


def _unit(value: Any, where: str) -> Vector:
    x, y, z = _numbers(value, where, 3)
    norm = math.sqrt(x * x + y * y + z * z)
    if norm == 0.0:
        raise _bad_field(where, "must not be the zero vector")
    return (x / norm, y / norm, z / norm)


def element_link(element_id: str) -> str:
    """The link of a fixed element, a tip, a spring slider or a jaw pair's frame."""

    return f"tool_{element_id}"


def jaw_link(element_id: str, side: str) -> str:
    return f"tool_{element_id}_{side}_jaw"


def pad_link(element_id: str, side: str) -> str:
    return f"tool_{element_id}_{side}_pad"


def finger_body(element_id: str) -> str:
    return f"{element_id}_fingers"


def pads_body(element_id: str) -> str:
    return f"{element_id}_pads"


def touch_body(pair: JawPair) -> str:
    """A jaw pair's touch and grip body: its pads, else its fingers."""

    return pads_body(pair.element_id) if pair.positive_pads else finger_body(pair.element_id)


def swept_link(element_id: str) -> str:
    return f"tool_{element_id}_swept"


def swept_body(element_id: str) -> str:
    """A spring slider's swept-space body (OD-358)."""

    return f"{element_id}_swept"


_AXIS_TOLERANCE: Final = 1e-9


def _conjugate(rotation: Quaternion) -> Quaternion:
    return (-rotation[0], -rotation[1], -rotation[2], rotation[3])


def _rotation_from_z(axis: Vector) -> Quaternion:
    """The shortest rotation taking +z onto the unit vector ``axis``."""

    x, y, z = axis
    if z < -1.0 + _AXIS_TOLERANCE:
        return (1.0, 0.0, 0.0, 0.0)
    w = 1.0 + z
    norm = math.sqrt(w * w + y * y + x * x)
    return (-y / norm, x / norm, 0.0, w / norm)


def _swept_parts(parts: Sequence[Part], axis: Vector, stroke: float,
                 where: str) -> tuple[Part, ...]:
    """Collision-only parts covering every position of ``parts`` pushed back
    by 0 to ``stroke`` against ``axis`` (element frame): a box or cylinder whose
    own axis is the travel axis lengthened by the stroke and moved back by
    half of it; a sphere as a cylinder between its end positions with a
    sphere at each end. Other shapes are refused."""

    swept: list[Part] = []
    shift = tuple(-value * stroke / 2.0 for value in axis)
    for index, part in enumerate(parts):
        if part.collision is None:
            continue
        frame = _compose(part.pose, part.collision_pose)
        geometry = part.collision
        local = _rotate(_conjugate(frame.rotation), axis)
        along = next((i for i in range(3) if abs(abs(local[i]) - 1.0) <= _AXIS_TOLERANCE), None)
        moved = Pose(tuple(a + b for a, b in zip(frame.position, shift)),  # type: ignore[arg-type]
                     frame.rotation)
        name = f"{part.name}_swept"
        if geometry.kind == "box" and along is not None:
            size = list(geometry.get("size_m"))
            size[along] += stroke
            shapes = [(Geometry("box", (("size_m", tuple(size)),)), moved)]
        elif geometry.kind == "cylinder" and along == 2:
            shapes = [(Geometry("cylinder", (
                ("length_m", geometry.get("length_m") + stroke),
                ("radius_m", geometry.get("radius_m")))), moved)]
        elif geometry.kind == "sphere":
            radius = geometry.get("radius_m")
            end = tuple(a + 2.0 * b for a, b in zip(frame.position, shift))
            shapes = [
                (Geometry("cylinder", (("length_m", stroke), ("radius_m", radius))),
                 Pose(moved.position, _rotation_from_z(axis))),
                (geometry, Pose(frame.position)),
                (geometry, Pose(end)),  # type: ignore[arg-type]
            ]
        else:
            raise ParametricToolError(
                "parametric_tool_swept_space_unsupported",
                f"{where}[{index}].collision: a {geometry.kind} "
                + ("" if geometry.kind == "mesh" else "whose own axis is not the slider axis ")
                + "cannot be swept; give the part a box, cylinder (axis along the slider "
                "axis) or sphere collision, or turn swept_space off",
                f"{where}[{index}].collision",
            )
        for number, (shape, pose) in enumerate(shapes):
            swept.append(Part(name if number == 0 else f"{name}_{number}", pose, shape,
                              Pose(), None, Pose(), None, (0.0, 0.0, 0.0)))
    return tuple(swept)


def element_bodies(element: Element) -> tuple[str, ...]:
    """The collision bodies (role ids) of one element."""

    if isinstance(element, JawPair):
        return (finger_body(element.element_id),
                *((pads_body(element.element_id),) if element.positive_pads else ()))
    if isinstance(element, SpringSlider) and element.swept:
        return (element.element_id, swept_body(element.element_id))
    return (element.element_id,)


def _default_contact_bodies(element: Element | None) -> tuple[str, ...]:
    """A TCP's contact bodies when it names none: a jaw pair's touch body, a
    tip's or a spring slider's own body; none on the body."""

    if element is None:
        return ()
    if isinstance(element, JawPair):
        return (touch_body(element),)
    return (element.element_id,)


def _tool(document: Mapping[str, Any]) -> ToolParameters:
    fields = _mapping(
        document, "parameters", ("api_version", "kind", "body", "chains", "tcps"),
        ("mass_kg", "center_of_mass_m"),
    )
    body = _parts(fields["body"], "body", collision_required=False)
    chains = fields["chains"]
    if not isinstance(chains, list) or len(chains) > MAX_CHAINS:
        raise _bad_field("chains", f"must be a list of 0 to {MAX_CHAINS} chains")
    elements: list[Element] = []
    by_id: dict[str, Element] = {}
    for chain_index, chain in enumerate(chains):
        at = f"chains[{chain_index}]"
        chain_fields = _mapping(chain, at, ("elements",), ("attach_to", "jaw", "pose"))
        target = chain_fields.get("attach_to", BODY_ROLE)
        if target == BODY_ROLE:
            parent = TOOL_BODY_LINK
            if "jaw" in chain_fields:
                raise _bad_field(f"{at}.jaw", "applies only to a chain attached to a jaw pair")
        elif isinstance(target, str) and target in by_id:
            attached = by_id[target]
            if "jaw" in chain_fields:
                if not isinstance(attached, JawPair) or chain_fields["jaw"] not in (
                    "positive", "negative"
                ):
                    raise _bad_field(f"{at}.jaw", "must be positive or negative, on a jaw pair")
                parent = jaw_link(target, chain_fields["jaw"])
            else:
                parent = element_link(target)
        else:
            raise _bad_field(f"{at}.attach_to", f"must be 'body' or the id of an element of "
                             f"an earlier chain, got {target!r}")
        chain_pose = _pose(chain_fields.get("pose"), f"{at}.pose")
        rows = chain_fields["elements"]
        if not isinstance(rows, list) or not rows or len(rows) > MAX_ELEMENTS:
            raise _bad_field(f"{at}.elements", f"must be a list of 1 to {MAX_ELEMENTS} elements")
        for index, row in enumerate(rows):
            where = f"{at}.elements[{index}]"
            if not isinstance(row, dict) or len(row) != 1 or next(iter(row)) not in ELEMENT_KINDS:
                raise _bad_field(where, f"must be exactly one of {', '.join(ELEMENT_KINDS)}")
            kind, raw = next(iter(row.items()))
            where = f"{where}.{kind}"
            if not isinstance(raw, dict):
                raise _bad_field(where, "must be a mapping")
            element_id = _identifier(raw.get("id"), f"{where}.id")
            if element_id in (BODY_ROLE, BODY_TARGET) or element_id in by_id:
                raise _bad_field(f"{where}.id", f"duplicate or reserved element id {element_id!r}")
            pose = _compose(chain_pose, _pose(raw.get("pose"), f"{where}.pose"))
            element: Element
            if kind in ("fixed", "tip"):
                element_fields = _mapping(raw, where, ("id", "parts"), ("pose",))
                parts = _parts(element_fields["parts"], f"{where}.parts",
                               collision_required=False)
                element = (FixedElement if kind == "fixed" else TipElement)(
                    element_id, parent, pose, parts)
            elif kind == "spring_slider":
                element_fields = _mapping(raw, where, ("id", "parts", "stroke_m", "trip_depth_m"),
                                          ("pose", "axis", "max_compression_step_m",
                                           "swept_space", "release_depth_m"))
                stroke = _number(element_fields["stroke_m"], f"{where}.stroke_m", positive=True)
                trip = _number(element_fields["trip_depth_m"], f"{where}.trip_depth_m",
                               positive=True)
                if trip > stroke:
                    raise _bad_field(f"{where}.trip_depth_m", "must be <= stroke_m")
                release = None
                if "release_depth_m" in element_fields:
                    release = _number(element_fields["release_depth_m"],
                                      f"{where}.release_depth_m", positive=True)
                    if release >= trip:
                        raise _bad_field(f"{where}.release_depth_m", "must be < trip_depth_m")
                swept_space = element_fields.get("swept_space", False)
                if not isinstance(swept_space, bool):
                    raise _bad_field(f"{where}.swept_space", "must be true or false")
                slider_parts = _parts(element_fields["parts"], f"{where}.parts",
                                      collision_required=False)
                axis = (_unit(element_fields["axis"], f"{where}.axis")
                        if "axis" in element_fields else (0.0, 0.0, 1.0))
                element = SpringSlider(
                    element_id, parent, pose, slider_parts, axis, stroke, trip,
                    _number(element_fields["max_compression_step_m"],
                            f"{where}.max_compression_step_m", positive=True)
                    if "max_compression_step_m" in element_fields else 0.002,
                    _swept_parts(slider_parts, axis, stroke, f"{where}.parts")
                    if swept_space else (),
                    release)
            else:
                element = _jaw_pair(raw, where, element_id, parent, pose)
            by_id[element_id] = element
            elements.append(element)

    roles = {BODY_ROLE}
    for element in elements:
        for name in element_bodies(element):
            if name in roles:
                raise _bad_field("chains", f"collision body id {name!r} is used twice")
            roles.add(name)

    rows = fields["tcps"]
    if not isinstance(rows, list) or not rows or len(rows) > MAX_TCPS:
        raise _bad_field("tcps", f"must be a list of 1 to {MAX_TCPS} TCPs (the first is "
                         "the default)")
    tcps: list[ToolTcp] = []
    for index, row in enumerate(rows):
        where = f"tcps[{index}]"
        tcp_fields = _mapping(row, where, ("id",), ("attach_to", "pose", "contact_bodies"))
        tcp_id = _identifier(tcp_fields["id"], f"{where}.id")
        if index == 0 and tcp_id != DEFAULT_TCP_ID:
            raise _bad_field(f"{where}.id", "the first TCP is the default TCP; its id is 'tcp'")
        if index > 0 and (tcp_id == DEFAULT_TCP_ID or tcp_id.startswith("tool")):
            raise _bad_field(f"{where}.id", f"{tcp_id!r} is reserved for the family's frames")
        if any(tcp.tcp_id == tcp_id for tcp in tcps):
            raise _bad_field(f"{where}.id", f"duplicate TCP id {tcp_id!r}")
        if isinstance(by_id.get(tcp_id), SpringSlider):
            raise _bad_field(f"{where}.id", f"{tcp_id!r} is the frame of a spring slider")
        end_id = tcp_fields.get("attach_to", BODY_ROLE)
        if end_id == BODY_ROLE:
            end, parent = None, TOOL_BODY_LINK
        elif isinstance(end_id, str) and not isinstance(by_id.get(end_id), FixedElement) \
                and end_id in by_id:
            end, parent = by_id[end_id], element_link(end_id)
        else:
            known = sorted(i for i, e in by_id.items() if not isinstance(e, FixedElement))
            raise _bad_field(f"{where}.attach_to", f"must be 'body' or a tool end "
                             f"({', '.join(known) or 'none'}), got {end_id!r}")
        contact = tcp_fields.get("contact_bodies")
        if contact is None:
            bodies = _default_contact_bodies(end)
        else:
            if not isinstance(contact, list) or len(set(map(str, contact))) != len(contact):
                raise _bad_field(f"{where}.contact_bodies", "must be a list of distinct body ids")
            unknown = [name for name in contact if name not in roles]
            if unknown:
                raise _bad_field(f"{where}.contact_bodies",
                                 f"unknown collision body {unknown[0]!r} "
                                 f"(known: {', '.join(sorted(roles))})")
            bodies = tuple(contact)
        tcps.append(ToolTcp(tcp_id, None if end is None else end.element_id, parent,
                            _pose(tcp_fields.get("pose"), f"{where}.pose"), bodies))

    every_part = [*body]
    for element in elements:
        every_part += (
            [*element.positive_jaw, *element.negative_jaw, *element.positive_pads,
             *element.negative_pads] if isinstance(element, JawPair) else list(element.parts))
    names = [part.name for part in every_part]
    if len(set(names)) != len(names):
        raise _bad_field("parts", "part names must be unique in the file (a mirrored jaw's "
                         "parts are named <name>_negative)")
    per_part = any(part.mass_kg is not None for part in every_part)
    mass = None
    if "mass_kg" in fields:
        if per_part:
            raise _bad_field("mass_kg", "give the tool's mass_kg or per-part mass_kg, not both")
        mass = _number(fields["mass_kg"], "mass_kg", non_negative=True)
    elif "center_of_mass_m" in fields:
        raise _bad_field("center_of_mass_m", "needs the tool's mass_kg")
    return ToolParameters(
        body=body, elements=tuple(elements), tcps=tuple(tcps), mass_kg=mass,
        center_of_mass_m=(
            _numbers(fields["center_of_mass_m"], "center_of_mass_m", 3)
            if "center_of_mass_m" in fields else (0.0, 0.0, 0.0)
        ),  # type: ignore[arg-type]
    )


def _tool_link(name: str, parts: Sequence[Part], mesh_files: Mapping[str, str], *,
               tool_mass: float | None = None,
               tool_center: Vector = (0.0, 0.0, 0.0)) -> list[str]:
    placed = [(part, part.pose) for part in parts]
    if tool_mass is not None:
        mass, center = tool_mass, tool_center
    else:
        mass, center = _part_mass(placed)
    return _link(name, placed, _inertial(placed, mass, center) if mass > 0.0 else [],
                 mesh_files)


def _prismatic(name: str, parent: str, child: str, pose: Pose, upper: float,
               mimic: str | None) -> list[str]:
    lines = [
        f'  <joint name="{name}" type="prismatic">',
        f'    <parent link="{parent}"/>',
        f'    <child link="{child}"/>',
        f"    {_origin(pose)}",
        '    <axis xyz="0 1 0"/>',
        f'    <limit effort="10000" lower="0" upper="{_number_text(upper)}" velocity="0.1"/>',
    ]
    if mimic is not None:
        lines.append(f'    <mimic joint="{mimic}" multiplier="1" offset="0"/>')
    return [*lines, "  </joint>"]


def jaw_joint(element_id: str) -> str:
    return f"tool_{element_id}_joint"


def jaw_mimic_joint(element_id: str) -> str:
    return f"tool_{element_id}_mimic"


def tcp_link(index: int, tcp_id: str) -> str:
    return TOOL_TCP_LINK if index == 0 else f"{TOOL_TCP_LINK}_{tcp_id}"


def _travel(pair: JawPair, aperture: float) -> float:
    """Joint travel per jaw for an aperture (0 at min_aperture_m)."""

    return round((aperture - pair.min_aperture_m) / 2.0, 12)


def _tool_end_kind(element: Element) -> str:
    if isinstance(element, JawPair):
        return "jaw_pair"
    return "spring_slider" if isinstance(element, SpringSlider) else "tip"


def component_semantics(parameters: ToolParameters) -> dict[str, Any]:
    """What the generated component declares beside its model: collision
    bodies (role id -> links), its tool ends, its TCPs (each with its tool
    end and contact bodies), one gripper target per jaw pair (an actuated
    tool end: its states, joint, positions, frames and motion time, which
    its Deployment wiring row does not repeat), and each spring slider's
    travel metadata. Load makes each spring slider a frame of the ``tool``
    target under its id, at the slider link (its collision body's link), so
    objects attach to the slider itself (OD-371)."""

    tcp_frames = {tcp.tcp_id: tcp_link(index, tcp.tcp_id)
                  for index, tcp in enumerate(parameters.tcps)}
    bodies: dict[str, list[str]] = {BODY_ROLE: [TOOL_BODY_LINK]}
    ends: list[dict[str, Any]] = []
    targets: list[dict[str, Any]] = []
    sliders: list[dict[str, Any]] = []
    for element in parameters.elements:
        if isinstance(element, FixedElement):
            bodies[element.element_id] = [element_link(element.element_id)]
            continue
        end = element.element_id
        own_tcps = [tcp.tcp_id for tcp in parameters.tcps if tcp.tool_end == end]
        ends.append({
            "id": end,
            "kind": _tool_end_kind(element),
            "actuated": isinstance(element, JawPair),
            "collision_body_role_ids": list(element_bodies(element)),
            "tcp_ids": own_tcps,
        })
        if isinstance(element, JawPair):
            bodies[finger_body(end)] = [jaw_link(end, "positive"), jaw_link(end, "negative")]
            if element.positive_pads:
                bodies[pads_body(end)] = [pad_link(end, "positive"), pad_link(end, "negative")]
            targets.append({
                "command_target_id": end,
                "group": end,
                "joint": jaw_joint(end),
                "mimic_joint": jaw_mimic_joint(end),
                "states": {"open": "open", "closed": "closed"},
                "open_position_m": _travel(element, element.open_aperture_m),
                "closed_position_m": _travel(element, element.closed_aperture_m),
                "attach_frame": element_link(end),
                # The tool end's first TCP, else the tool's default TCP.
                "tcp_frame": tcp_frames[own_tcps[0]] if own_tcps else TOOL_TCP_LINK,
                # The grip body (OD-46): the bodies that may touch what this pair holds.
                "grip_body_role_id": touch_body(element),
                "collision_body_role_ids": list(element_bodies(element)),
                "command_duration_s": element.command_duration_s,
                "actuation": "pneumatic",
            })
        else:
            bodies[end] = [element_link(end)]
            if isinstance(element, SpringSlider) and element.swept:
                bodies[swept_body(end)] = [swept_link(end)]
            if isinstance(element, SpringSlider):
                slider = {
                    "collision_body_role_id": end,
                    "axis": list(element.axis),
                    "stroke_m": element.stroke_m,
                    "trip_depth_m": element.trip_depth_m,
                    "max_compression_step_m": element.max_compression_step_m,
                }
                if element.release_depth_m is not None:
                    slider["release_depth_m"] = element.release_depth_m
                sliders.append(slider)
    return {
        "mount_frame": TOOL_MOUNT_LINK,
        "collision_bodies": bodies,
        "tool_ends": ends,
        "tcps": [
            {"id": tcp.tcp_id, "frame": tcp_frames[tcp.tcp_id],
             "tool_end": tcp.tool_end,
             "contact_body_role_ids": list(tcp.contact_bodies)}
            for tcp in parameters.tcps
        ],
        "gripper_targets": targets,
        "spring_sliders": sliders,
    }


def _all_parts(parameters: ToolParameters) -> list[Part]:
    parts: list[Part] = list(parameters.body)
    for element in parameters.elements:
        parts += ([*element.positive_jaw, *element.negative_jaw, *element.positive_pads,
                   *element.negative_pads] if isinstance(element, JawPair)
                  else list(element.parts))
    return parts


def mesh_paths(parameters: ToolParameters) -> tuple[str, ...]:
    """The mesh paths the parameters name, sorted and unique."""

    return tuple(sorted({
        geometry.get("path")
        for part in _all_parts(parameters)
        for geometry in (part.collision, part.visual)
        if geometry is not None and geometry.kind == "mesh"
    }))


def _generate_tool(parameters: ToolParameters, mesh_files: Mapping[str, str]) -> GeneratedTool:
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        _header(),
        f'<robot name="{ROBOT_NAME}">',
        f'  <link name="{TOOL_MOUNT_LINK}"/>',
        *_fixed_joint(f"{TOOL_BODY_LINK}_fix", TOOL_MOUNT_LINK, TOOL_BODY_LINK, Pose()),
        *_tool_link(TOOL_BODY_LINK, parameters.body, mesh_files, tool_mass=parameters.mass_kg,
                    tool_center=parameters.center_of_mass_m),
    ]
    # (child, parent) for links with geometry, to disable adjacent pairs.
    parent_of: dict[str, str] = {}
    geometric = {TOOL_BODY_LINK}
    srdf_groups: list[str] = []
    swept_links: list[tuple[str, str]] = []  # (swept link, its slider's link)
    for element in parameters.elements:
        link = element_link(element.element_id)
        lines += _fixed_joint(f"{link}_fix", element.parent_link, link, element.pose)
        parent_of[link] = element.parent_link
        if not isinstance(element, JawPair):
            lines += _tool_link(link, element.parts, mesh_files)
            geometric.add(link)
            if isinstance(element, SpringSlider) and element.swept:
                # A fixed body over the whole stroke (OD-358); it does not
                # collide with the slider or with what the slider sits in.
                swept = swept_link(element.element_id)
                lines += [*_fixed_joint(f"{swept}_fix", link, swept, Pose()),
                          *_tool_link(swept, element.swept, mesh_files)]
                parent_of[swept] = link
                geometric.add(swept)
                swept_links.append((swept, link))
            continue
        pair = element.element_id
        lines.append(f'  <link name="{link}"/>')
        upper = (element.max_aperture_m - element.min_aperture_m) / 2.0
        group_links = [link]
        for side, jaw, pads, origin, mimic in (
            ("positive", element.positive_jaw, element.positive_pads, Pose(), None),
            ("negative", element.negative_jaw, element.negative_pads,
             Pose((0.0, 0.0, 0.0), _HALF_TURN_Z), jaw_joint(pair)),
        ):
            name = jaw_joint(pair) if mimic is None else jaw_mimic_joint(pair)
            lines += [*_prismatic(name, link, jaw_link(pair, side), origin, upper, mimic),
                      *_tool_link(jaw_link(pair, side), jaw, mesh_files)]
            parent_of[jaw_link(pair, side)] = link
            geometric.add(jaw_link(pair, side))
            group_links.append(jaw_link(pair, side))
            if pads:
                lines += [*_fixed_joint(f"{pad_link(pair, side)}_fix", jaw_link(pair, side),
                                        pad_link(pair, side), Pose()),
                          *_tool_link(pad_link(pair, side), pads, mesh_files)]
                parent_of[pad_link(pair, side)] = jaw_link(pair, side)
                geometric.add(pad_link(pair, side))
                group_links.append(pad_link(pair, side))
        srdf_groups += [f'  <group name="{pair}">',
                        *(f'    <link name="{name}"/>' for name in group_links),
                        f'    <joint name="{jaw_joint(pair)}"/>',
                        f'    <joint name="{jaw_mimic_joint(pair)}"/>',
                        "  </group>"]
        for state, aperture in (("open", element.open_aperture_m),
                                ("closed", element.closed_aperture_m)):
            value = _number_text(_travel(element, aperture))
            srdf_groups += [f'  <group_state name="{state}" group="{pair}">',
                            f'    <joint name="{jaw_joint(pair)}" value="{value}"/>',
                            f'    <joint name="{jaw_mimic_joint(pair)}" value="{value}"/>',
                            "  </group_state>"]
    for index, tcp in enumerate(parameters.tcps):
        link = tcp_link(index, tcp.tcp_id)
        lines += [*_fixed_joint(f"{link}_fix", tcp.parent_link, link, tcp.pose),
                  f'  <link name="{link}"/>']
    lines.append("</robot>")

    def geometric_parent(link: str) -> str | None:
        parent = parent_of.get(link)
        while parent is not None and parent not in geometric:
            parent = parent_of.get(parent)
        return parent

    srdf = ['<?xml version="1.0" encoding="utf-8"?>', _header(),
            f'<robot name="{ROBOT_NAME}">', *srdf_groups]
    for link in sorted(geometric - {TOOL_BODY_LINK}):
        parent = geometric_parent(link)
        if parent is not None:
            srdf.append(f'  <disable_collisions link1="{parent}" link2="{link}" '
                        'reason="Adjacent"/>')
    for swept, slider in swept_links:
        parent = geometric_parent(slider)
        if parent is not None:
            srdf.append(f'  <disable_collisions link1="{parent}" link2="{swept}" '
                        'reason="Adjacent"/>')
    srdf.append("</robot>")
    return GeneratedTool(
        urdf=("\n".join(lines) + "\n").encode("utf-8"),
        srdf=("\n".join(srdf) + "\n").encode("utf-8"),
        mesh_paths=mesh_paths(parameters),
        robot_name=ROBOT_NAME,
        component=component_semantics(parameters),
    )


def _pose_row(pose: Pose) -> dict[str, list[float]]:
    return {"position_m": [round(v, 12) for v in pose.position],
            "orientation_xyzw": [round(v, 12) for v in pose.rotation]}


def preview(parameters: ToolParameters) -> dict[str, Any]:
    """The tool's shapes and TCP frames in its mount frame, jaws at their
    open position: what the Composer draws while the parameters are edited.
    A part shows its visual geometry, else its collision geometry; a mesh is
    named by its path as written in the parameters."""

    links: dict[str, Pose] = {TOOL_MOUNT_LINK: Pose(), TOOL_BODY_LINK: Pose()}
    shapes: list[dict[str, Any]] = []

    def add(link: str, parts: Sequence[Part]) -> None:
        for part in parts:
            geometry, offset = ((part.visual, part.visual_pose) if part.visual is not None
                                else (part.collision, part.collision_pose))
            if geometry is None:
                continue
            values = {name: list(value) if isinstance(value, tuple) else value
                      for name, value in geometry.values}
            shapes.append({
                "link": link, "part": part.name, "geometry": {geometry.kind: values},
                **_pose_row(_compose(links[link], _compose(part.pose, offset))),
            })

    add(TOOL_BODY_LINK, parameters.body)
    for element in parameters.elements:
        link = element_link(element.element_id)
        links[link] = _compose(links[element.parent_link], element.pose)
        if not isinstance(element, JawPair):
            add(link, element.parts)
            continue
        travel = Pose((0.0, _travel(element, element.open_aperture_m), 0.0))
        for side, jaw, pads, turn in (
            ("positive", element.positive_jaw, element.positive_pads, Pose()),
            ("negative", element.negative_jaw, element.negative_pads,
             Pose((0.0, 0.0, 0.0), _HALF_TURN_Z)),
        ):
            links[jaw_link(element.element_id, side)] = _compose(
                links[link], _compose(turn, travel))
            add(jaw_link(element.element_id, side), jaw)
            if pads:
                links[pad_link(element.element_id, side)] = links[
                    jaw_link(element.element_id, side)]
                add(pad_link(element.element_id, side), pads)
    return {
        "shapes": shapes,
        "tcps": [{"id": tcp.tcp_id, "tool_end": tcp.tool_end,
                  **_pose_row(_compose(links[tcp.parent_link], tcp.pose))}
                 for tcp in parameters.tcps],
    }


def export_files(tool: GeneratedTool, formats: Sequence[str]) -> dict[str, bytes]:
    """The exported files of one generated tool: file name -> bytes."""

    return {f"{tool.robot_name}.{name}": tool.urdf if name == "urdf" else tool.srdf
            for name in require_export_formats(formats)}


def require_export_formats(formats: Sequence[str]) -> tuple[str, ...]:
    """The export formats written now; USDA comes with the Isaac lane."""

    for name in formats:
        if name not in EXPORT_FORMATS:
            raise ParametricToolError(
                "parametric_tool_invalid", f"unknown export format {name!r}"
            )
        if name == "usda":
            raise ParametricToolError(
                "parametric_tool_export_format_unsupported",
                "USDA export is not available yet: it comes with the Isaac "
                "support of parametric tools; export urdf and srdf",
            )
    return tuple(formats)
