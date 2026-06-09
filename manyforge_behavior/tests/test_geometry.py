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
    compose_local,
    compose_world,
    parse_pose,
    quats_close,
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
