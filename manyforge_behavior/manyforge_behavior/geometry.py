"""Canonical pose value type, conversions, and the single fail-closed parser.

This is the one place orientation/pose representation is reconciled. Scene state
is stored/transported as quaternions (``orientation_xyzw``); humans/agents author
in degrees (``orientation_rpy_deg``) or by copying an existing value. Putting the
canonical type + the conversions + the parser here (a leaf module that imports
nothing from ``nodes``) means:

  * read ≡ write by construction — the serializer emits a ``Pose`` and every
    writer accepts a ``Pose``; they cannot drift;
  * no representation/units math ever lives in an agent or is duplicated across
    node modules (this replaces the two ``_rpy_deg_to_xyzw`` copies and the
    ``_xyzw_to_rpy_deg`` in nodes/*);
  * conversions and composition are deterministic and property-tested.

Convention (fixed, documented, tested): RPY is intrinsic ZYX (roll about X,
pitch about Y, yaw about Z) in degrees; quaternions are ``[x, y, z, w]``.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any

# Rotation-agreement tolerance on the quaternion dot product (spec 420 §4A.2):
# two orientations "agree" iff |dot| >= 1 - QUAT_AGREEMENT_DOT_TOL. Angular
# meaning: |dot| = cos(theta/2), so 1e-3 admits up to theta ≈ 2*acos(1-1e-3)
# ≈ 5.1 degrees of rotation difference. Deliberately forgiving: the check
# exists to catch representation mistakes (component transplants, wrong-axis
# conversions — tens of degrees), while tolerating float/rounding noise in
# hand-copied values. Single named constant; do not redeclare per-module.
QUAT_AGREEMENT_DOT_TOL = 1e-3


class PoseError(ValueError):
    """Raised when a pose input cannot be parsed safely. Fail closed; never guess."""


# ── Conversions (single source of truth) ────────────────────────────────────

def rpy_deg_to_xyzw(rpy_deg: Any) -> list[float]:
    """Roll/pitch/yaw degrees (intrinsic ZYX) → unit quaternion [x, y, z, w]."""
    r, p, y = (math.radians(float(v)) for v in _finite_list(rpy_deg, 3, "orientation_rpy_deg"))
    cr, sr = math.cos(r / 2), math.sin(r / 2)
    cp, sp = math.cos(p / 2), math.sin(p / 2)
    cy, sy = math.cos(y / 2), math.sin(y / 2)
    return normalize_quat([
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    ])


def xyzw_to_rpy_deg(orientation_xyzw: Any) -> list[float]:
    """Unit quaternion [x, y, z, w] → roll/pitch/yaw degrees (intrinsic ZYX)."""
    x, y, z, w = _finite_list(orientation_xyzw, 4, "orientation_xyzw")
    sinr_cosp = 2.0 * (w * x + y * z)
    cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
    roll = math.atan2(sinr_cosp, cosr_cosp)
    sinp = 2.0 * (w * y - z * x)
    pitch = math.copysign(math.pi / 2.0, sinp) if abs(sinp) >= 1.0 else math.asin(sinp)
    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw = math.atan2(siny_cosp, cosy_cosp)
    return [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)]


def normalize_quat(q: Any) -> list[float]:
    vals = _finite_list(q, 4, "orientation_xyzw")
    norm = math.sqrt(sum(c * c for c in vals))
    if norm == 0.0:
        raise PoseError("orientation_xyzw must be a non-zero quaternion")
    return [c / norm for c in vals]


def quats_close(a: Any, b: Any, tol: float = QUAT_AGREEMENT_DOT_TOL) -> bool:
    """Rotation equality: |dot| ≈ 1 (a quaternion and its negation are equal)."""
    qa, qb = normalize_quat(a), normalize_quat(b)
    return abs(sum(x * y for x, y in zip(qa, qb))) >= 1.0 - tol


# ── Composition (one place; explicit basis — never an implicit default) ──────

def quat_mul(a: Any, b: Any) -> list[float]:
    """Hamilton product a⊗b for [x, y, z, w] quaternions (result normalized)."""
    ax, ay, az, aw = _finite_list(a, 4, "quat")
    bx, by, bz, bw = _finite_list(b, 4, "quat")
    return normalize_quat([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ])


def compose_local(current_xyzw: Any, delta_xyzw: Any) -> list[float]:
    """Apply ``delta`` in the body/local frame: q_current ⊗ q_delta."""
    return quat_mul(current_xyzw, delta_xyzw)


def compose_world(current_xyzw: Any, delta_xyzw: Any) -> list[float]:
    """Apply ``delta`` in the world frame: q_delta ⊗ q_current."""
    return quat_mul(delta_xyzw, current_xyzw)


def add_rpy_deg(current_xyzw: Any, delta_rpy_deg: Any) -> list[float]:
    """Component-wise add a degrees delta to the current orientation's RPY.

    This is **field-add semantics**, NOT a rigid-body rotation: it decomposes
    ``current_xyzw`` into intrinsic-ZYX roll/pitch/yaw, adds the per-component
    delta, and recomposes. For a pure-yaw start it coincides with composing the
    rotation, but in general it does not (e.g. from a tilted pose the two
    diverge). The result therefore depends on the canonical RPY decomposition of
    the current orientation. Offered as an explicit, user-selected alternative to
    ``compose_local``/``compose_world`` for authoring in RPY space.
    """
    cur = xyzw_to_rpy_deg(current_xyzw)
    d = _finite_list(delta_rpy_deg, 3, "rpy_delta_deg")
    return rpy_deg_to_xyzw([cur[0] + d[0], cur[1] + d[1], cur[2] + d[2]])


def quat_conjugate(q: Any) -> list[float]:
    """Inverse rotation of a unit quaternion [x, y, z, w]."""
    x, y, z, w = normalize_quat(q)
    return [-x, -y, -z, w]


def quat_rotate_vector(q: Any, v: Any) -> list[float]:
    """Rotate 3-vector ``v`` by unit quaternion ``q`` (q ⊗ v ⊗ q*)."""
    x, y, z, w = normalize_quat(q)
    vx, vy, vz = _finite_list(v, 3, "vector")
    # t = 2 * cross(q_xyz, v); v' = v + w*t + cross(q_xyz, t)
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return [
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    ]


# ── Canonical pose value ─────────────────────────────────────────────────────

@dataclass(frozen=True)
class Pose:
    """The canonical stored/wire pose. Orientation is always a unit quaternion."""

    frame_id: str
    position_m: tuple[float, float, float]
    orientation_xyzw: tuple[float, float, float, float]

    def to_dict(self, *, include_rpy: bool = False) -> dict[str, Any]:
        """Serialize. ``include_rpy`` adds a DERIVED ``orientation_rpy_deg`` for
        assistant/display ergonomics — it is computed from the canonical
        quaternion, not an independent field."""
        out: dict[str, Any] = {
            "frame_id": self.frame_id,
            "position_m": list(self.position_m),
            "orientation_xyzw": list(self.orientation_xyzw),
        }
        if include_rpy:
            out["orientation_rpy_deg"] = xyzw_to_rpy_deg(list(self.orientation_xyzw))
        return out


def parse_pose(raw: Any, *, allow_legacy: bool = False) -> Pose:
    """Validate a pose input → canonical ``Pose``. Fail closed; never guess.

    Accepts orientation as ``orientation_xyzw`` (quaternion) OR
    ``orientation_rpy_deg`` (degrees) OR both **iff they agree** (so a value
    copied verbatim from the canonical+derived read shape is accepted; a
    disagreeing pair is rejected). A missing orientation is an error on the
    write path — ``allow_legacy=True`` (read/legacy-loader path only) tolerates
    it as identity. This is the boundary that the old silent
    ``orientation_xyzw = [0,0,0,1]`` default lived at.
    """
    if not isinstance(raw, dict):
        raise PoseError("pose must be an object")
    frame_id = raw.get("frame_id") or raw.get("frameId")
    if not isinstance(frame_id, str) or not frame_id.strip():
        if not allow_legacy:
            raise PoseError("pose.frame_id is required")
        frame_id = "universe"
    position = raw.get("position_m")
    if position is None and allow_legacy:
        position = [0.0, 0.0, 0.0]
    pos = _finite_list(position, 3, "position_m")

    has_quat = raw.get("orientation_xyzw") is not None
    has_rpy = raw.get("orientation_rpy_deg") is not None
    if has_quat and has_rpy:
        q = normalize_quat(raw["orientation_xyzw"])
        if not quats_close(q, rpy_deg_to_xyzw(raw["orientation_rpy_deg"])):
            raise PoseError(
                "pose orientation_xyzw and orientation_rpy_deg disagree; "
                "supply only one, or matching values"
            )
    elif has_quat:
        q = normalize_quat(raw["orientation_xyzw"])
    elif has_rpy:
        q = rpy_deg_to_xyzw(raw["orientation_rpy_deg"])
    elif allow_legacy:
        q = [0.0, 0.0, 0.0, 1.0]
    else:
        raise PoseError(
            "pose orientation is required: provide orientation_xyzw (quaternion, "
            "e.g. copied from scene state) or orientation_rpy_deg (degrees). "
            "Do not omit it — that silently re-orients the object."
        )
    return Pose(frame_id.strip(), (pos[0], pos[1], pos[2]), (q[0], q[1], q[2], q[3]))


# ── Authoring intent resolution (spec 420 §4A.4) ─────────────────────────────

_ABSOLUTE_INTENT_KEYS = ("orientation_xyzw", "orientation_rpy_deg")
_RELATIVE_INTENT_KEYS = ("rotate_local_rpy_deg", "rotate_world_rpy_deg")
_DELTA_INTENT_KEYS = ("rpy_delta_deg",)
_REFERENCE_INTENT_KEYS = ("orientation_from",)
ORIENTATION_INTENT_KEYS = (
    _ABSOLUTE_INTENT_KEYS + _RELATIVE_INTENT_KEYS + _DELTA_INTENT_KEYS + _REFERENCE_INTENT_KEYS
)


def resolve_orientation(
    intent: Any,
    *,
    current_xyzw: Any = None,
    resolve_reference: Any = None,
) -> list[float]:
    """Resolve one orientation authoring intent → canonical unit quaternion.

    Exactly one intent mode is present (an agreeing absolute xyzw/rpy pair counts
    as one). Absolute modes need nothing else; relative-compose modes
    (``rotate_local_rpy_deg`` / ``rotate_world_rpy_deg``) compose a degrees delta
    against ``current_xyzw`` in an explicit basis; ``rpy_delta_deg`` adds a
    degrees delta component-wise to the current orientation's RPY (field-add, not
    composition — see ``add_rpy_deg``); the reference mode (``orientation_from``)
    adopts orientation from another resource via ``resolve_reference(ref) ->
    xyzw``. All conversion/composition happens here — never in the agent. Fail
    closed: a missing current/source orientation is an error, never an implicit
    identity.
    """
    if not isinstance(intent, dict):
        raise PoseError("orientation intent must be an object")
    present = [k for k in ORIENTATION_INTENT_KEYS if intent.get(k) is not None]
    if set(present) == set(_ABSOLUTE_INTENT_KEYS):
        # both absolute forms supplied — accept iff they agree, treat as one
        q = normalize_quat(intent["orientation_xyzw"])
        if not quats_close(q, rpy_deg_to_xyzw(intent["orientation_rpy_deg"])):
            raise PoseError("orientation_xyzw and orientation_rpy_deg disagree")
        return q
    if len(present) != 1:
        raise PoseError(
            "exactly one orientation intent is required, one of: "
            + ", ".join(ORIENTATION_INTENT_KEYS)
        )
    key = present[0]
    if key == "orientation_xyzw":
        return normalize_quat(intent["orientation_xyzw"])
    if key == "orientation_rpy_deg":
        return rpy_deg_to_xyzw(intent["orientation_rpy_deg"])
    if key in _RELATIVE_INTENT_KEYS:
        if current_xyzw is None:
            raise PoseError(
                f"{key} is relative and needs a current orientation to compose "
                "against; none was resolvable"
            )
        delta = rpy_deg_to_xyzw(intent[key])
        if key == "rotate_local_rpy_deg":
            return compose_local(current_xyzw, delta)
        return compose_world(current_xyzw, delta)
    if key == "rpy_delta_deg":
        if current_xyzw is None:
            raise PoseError(
                "rpy_delta_deg adds to the current orientation's angles and needs "
                "a current orientation; none was resolvable"
            )
        return add_rpy_deg(current_xyzw, intent[key])
    # orientation_from (reference)
    if resolve_reference is None:
        raise PoseError("orientation_from requires a reference resolver")
    ref_xyzw = resolve_reference(intent["orientation_from"])
    if ref_xyzw is None:
        raise PoseError("orientation_from reference could not be resolved")
    return normalize_quat(ref_xyzw)


# ── Typed read-only spatial queries (spec 420 §4A.6) ─────────────────────────

def distance_between(a_position_m: Any, b_position_m: Any) -> float:
    """Euclidean distance (meters) between two positions."""
    a = _finite_list(a_position_m, 3, "position_m")
    b = _finite_list(b_position_m, 3, "position_m")
    return math.sqrt(sum((p - q) ** 2 for p, q in zip(a, b)))


def angle_between(a_xyzw: Any, b_xyzw: Any) -> float:
    """Smallest rotation angle (degrees) between two orientations."""
    qa, qb = normalize_quat(a_xyzw), normalize_quat(b_xyzw)
    dot = min(1.0, abs(sum(p * q for p, q in zip(qa, qb))))
    return math.degrees(2.0 * math.acos(dot))


def pose_relative_to(target: Pose, reference: Pose) -> Pose:
    """Express ``target`` in ``reference``'s local frame: T_ref⁻¹ ∘ T_target."""
    inv = quat_conjugate(reference.orientation_xyzw)
    rel_pos = quat_rotate_vector(
        inv,
        [t - r for t, r in zip(target.position_m, reference.position_m)],
    )
    rel_quat = quat_mul(inv, list(target.orientation_xyzw))
    return Pose(
        reference.frame_id,
        (rel_pos[0], rel_pos[1], rel_pos[2]),
        (rel_quat[0], rel_quat[1], rel_quat[2], rel_quat[3]),
    )


def _finite_list(value: Any, n: int, label: str) -> list[float]:
    if not isinstance(value, (list, tuple)) or len(value) != n:
        raise PoseError(f"{label} must be a list of exactly {n} numbers")
    out: list[float] = []
    for i, v in enumerate(value):
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(float(v)):
            raise PoseError(f"{label}[{i}] must be a finite number")
        out.append(float(v))
    return out
