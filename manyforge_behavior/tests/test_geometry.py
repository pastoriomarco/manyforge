"""Property + example tests for the canonical pose geometry boundary.

These guard the whole orientation-representation bug class: conversions
round-trip, composition is exact and basis-correct, and parse_pose fails closed
instead of silently re-orienting (the original bug).
"""
import math

import pytest

from manyforge_behavior.geometry import (
    Pose,
    PoseError,
    angle_between,
    compose_local,
    compose_world,
    distance_between,
    parse_pose,
    pose_relative_to,
    quats_close,
    resolve_orientation,
    rpy_deg_to_xyzw,
    xyzw_to_rpy_deg,
)

# A spread of orientations incl. the graspable's -45° about Z and gripper-down.
_RPY_SAMPLES = [
    [0, 0, 0], [0, 0, -45], [180, 0, 0], [0, 0, 90], [0, 0, -90],
    [15, 0, 0], [0, 30, 0], [10, 20, 30], [-25, 5, -60], [0, 0, 179],
]


@pytest.mark.parametrize("rpy", _RPY_SAMPLES)
def test_rpy_quat_roundtrip(rpy):
    # rpy -> quat -> rpy returns the SAME rotation (compare as quaternions, not
    # raw triples, since Euler is non-unique).
    q = rpy_deg_to_xyzw(rpy)
    assert quats_close(rpy_deg_to_xyzw(xyzw_to_rpy_deg(q)), q)


def test_graspable_minus45_about_z():
    # The exact scene value the live bug got wrong.
    q = [0.0, 0.0, -0.3826834323650898, 0.9238795325112867]
    assert xyzw_to_rpy_deg(q) == pytest.approx([0.0, 0.0, -45.0], abs=1e-6)
    assert quats_close(rpy_deg_to_xyzw([0, 0, -45]), q)


@pytest.mark.parametrize("rpy", _RPY_SAMPLES)
def test_compose_local_then_inverse_is_identity(rpy):
    cur = rpy_deg_to_xyzw([10, -20, 35])
    delta = rpy_deg_to_xyzw(rpy)
    inv = [-delta[0], -delta[1], -delta[2], delta[3]]  # quaternion conjugate
    assert quats_close(compose_local(compose_local(cur, delta), inv), cur)


def test_compose_local_vs_world_differ_and_are_correct():
    cur = rpy_deg_to_xyzw([0, 0, 90])        # yaw 90
    dz15 = rpy_deg_to_xyzw([0, 0, 15])       # +15 about Z
    # About Z both bases coincide (commute) → +105 either way.
    assert quats_close(compose_local(cur, dz15), rpy_deg_to_xyzw([0, 0, 105]))
    assert quats_close(compose_world(cur, dz15), rpy_deg_to_xyzw([0, 0, 105]))
    # About a different axis the basis matters → local != world.
    dx15 = rpy_deg_to_xyzw([15, 0, 0])
    assert not quats_close(compose_local(cur, dx15), compose_world(cur, dx15))


def test_parse_pose_absolute_degrees():
    p = parse_pose({"frame_id": "universe", "position_m": [0.3, -0.3, 0.15],
                    "orientation_rpy_deg": [0, 0, -45]})
    assert isinstance(p, Pose)
    assert quats_close(list(p.orientation_xyzw), rpy_deg_to_xyzw([0, 0, -45]))


def test_parse_pose_absolute_quaternion():
    q = [0.0, 0.0, -0.3826834323650898, 0.9238795325112867]
    p = parse_pose({"frame_id": "universe", "position_m": [0.3, -0.3, 0.15],
                    "orientation_xyzw": q})
    assert quats_close(list(p.orientation_xyzw), q)


def test_parse_pose_both_present_and_agreeing_is_accepted():
    # The read shape surfaces canonical quat + derived rpy; copying both back
    # must be accepted because they agree.
    q = rpy_deg_to_xyzw([0, 0, -45])
    p = parse_pose({"frame_id": "universe", "position_m": [0, 0, 0],
                    "orientation_xyzw": q, "orientation_rpy_deg": [0, 0, -45]})
    assert quats_close(list(p.orientation_xyzw), q)


def test_parse_pose_both_present_and_disagreeing_fails_closed():
    with pytest.raises(PoseError):
        parse_pose({"frame_id": "universe", "position_m": [0, 0, 0],
                    "orientation_xyzw": rpy_deg_to_xyzw([0, 0, -45]),
                    "orientation_rpy_deg": [0, 0, 90]})


def test_parse_pose_missing_orientation_fails_closed():
    # The original bug: a missing orientation must NOT default to identity.
    with pytest.raises(PoseError):
        parse_pose({"frame_id": "universe", "position_m": [0.3, -0.3, 0.15]})


def test_parse_pose_legacy_tolerates_missing_orientation():
    p = parse_pose({"position_m": [0.3, -0.3, 0.15]}, allow_legacy=True)
    assert tuple(p.orientation_xyzw) == (0.0, 0.0, 0.0, 1.0)
    assert p.frame_id == "universe"


def test_parse_pose_rejects_bad_inputs():
    for bad in (
        {"frame_id": "universe", "position_m": [0, 0]},          # short position
        {"frame_id": "universe", "position_m": [0, 0, "x"]},     # non-numeric
        {"position_m": [0, 0, 0], "orientation_rpy_deg": [0, 0, 0]},  # no frame
        {"frame_id": "u", "position_m": [0, 0, 0], "orientation_xyzw": [0, 0, 0, 0]},  # zero quat
    ):
        with pytest.raises(PoseError):
            parse_pose(bad)


def test_to_dict_includes_derived_rpy_only_on_request():
    p = parse_pose({"frame_id": "universe", "position_m": [0, 0, 0],
                    "orientation_rpy_deg": [0, 0, -45]})
    assert "orientation_rpy_deg" not in p.to_dict()
    d = p.to_dict(include_rpy=True)
    assert d["orientation_rpy_deg"] == pytest.approx([0, 0, -45], abs=1e-6)


# ── Phase 2: authoring intent resolution (§4A.4) ─────────────────────────────

def test_resolve_orientation_absolute_modes():
    assert quats_close(resolve_orientation({"orientation_rpy_deg": [0, 0, -45]}),
                       rpy_deg_to_xyzw([0, 0, -45]))
    q = rpy_deg_to_xyzw([0, 0, -45])
    assert quats_close(resolve_orientation({"orientation_xyzw": q}), q)


def test_resolve_orientation_absolute_pair_agree_and_disagree():
    q = rpy_deg_to_xyzw([0, 0, -45])
    assert quats_close(
        resolve_orientation({"orientation_xyzw": q, "orientation_rpy_deg": [0, 0, -45]}), q)
    with pytest.raises(PoseError):
        resolve_orientation({"orientation_xyzw": q, "orientation_rpy_deg": [0, 0, 0]})


def test_resolve_orientation_relative_local_then_undo_is_current():
    current = rpy_deg_to_xyzw([0, 0, -45])
    rotated = resolve_orientation({"rotate_local_rpy_deg": [0, 0, 90]}, current_xyzw=current)
    back = resolve_orientation({"rotate_local_rpy_deg": [0, 0, -90]}, current_xyzw=rotated)
    assert quats_close(back, current)


def test_resolve_orientation_local_vs_world_differ_for_noncommuting():
    current = rpy_deg_to_xyzw([0, 90, 0])
    delta = {"rotate_local_rpy_deg": [90, 0, 0]}
    world = {"rotate_world_rpy_deg": [90, 0, 0]}
    local_q = resolve_orientation(delta, current_xyzw=current)
    world_q = resolve_orientation(world, current_xyzw=current)
    assert not quats_close(local_q, world_q)


def test_resolve_orientation_relative_without_current_fails_closed():
    with pytest.raises(PoseError):
        resolve_orientation({"rotate_local_rpy_deg": [15, 0, 0]})


def test_resolve_orientation_reference_mode():
    target = rpy_deg_to_xyzw([0, 0, 90])
    got = resolve_orientation(
        {"orientation_from": {"type": "object", "id": "fixture"}},
        resolve_reference=lambda ref: target,
    )
    assert quats_close(got, target)
    # unresolvable reference fails closed
    with pytest.raises(PoseError):
        resolve_orientation({"orientation_from": {"id": "x"}}, resolve_reference=lambda r: None)


def test_resolve_orientation_requires_exactly_one_mode():
    with pytest.raises(PoseError):
        resolve_orientation({})
    with pytest.raises(PoseError):
        resolve_orientation({"rotate_local_rpy_deg": [1, 0, 0], "orientation_rpy_deg": [0, 0, 0]})


# ── Phase 2: typed read-only queries (§4A.6) ─────────────────────────────────

def test_distance_between():
    assert distance_between([0, 0, 0], [3, 4, 0]) == pytest.approx(5.0)


def test_angle_between():
    a = rpy_deg_to_xyzw([0, 0, 0])
    b = rpy_deg_to_xyzw([0, 0, 90])
    assert angle_between(a, b) == pytest.approx(90.0, abs=1e-6)
    assert angle_between(a, a) == pytest.approx(0.0, abs=1e-6)


def test_pose_relative_to_self_is_identity():
    p = parse_pose({"frame_id": "universe", "position_m": [0.3, -0.3, 0.15],
                    "orientation_rpy_deg": [0, 0, -45]})
    rel = pose_relative_to(p, p)
    assert rel.position_m == pytest.approx((0, 0, 0), abs=1e-9)
    assert quats_close(rel.orientation_xyzw, [0, 0, 0, 1])


def test_pose_relative_to_recovers_offset():
    ref = parse_pose({"frame_id": "universe", "position_m": [1, 0, 0],
                      "orientation_rpy_deg": [0, 0, 90]})
    tgt = parse_pose({"frame_id": "universe", "position_m": [1, 1, 0],
                      "orientation_rpy_deg": [0, 0, 90]})
    rel = pose_relative_to(tgt, ref)
    # tgt is +1 in world Y; ref is yawed +90°, so in ref's frame that is +X.
    assert rel.position_m == pytest.approx((1, 0, 0), abs=1e-9)
    assert quats_close(rel.orientation_xyzw, [0, 0, 0, 1])
