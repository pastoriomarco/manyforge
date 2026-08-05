"""Focused tests for the retained owner-neutral geometry algorithms.

Historical source ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``
is blob ``f37241575f262f4e73ed086be4bffa40b8aa48ed`` (29 definitions,
definition digest ``1cb1bfda55185be5d04ae2a1f0af423a327248f2b81e33c5b204202ece9ce137``).

The eight old ``Pose``/``parse_pose``/serializer wire cases at lines 68-121
(digest ``ed73ee537d705664264580c03c0b553694b42fac3d7fe098d652ee913ff6ed03``)
are current H0/I5 logical-scene-owner and future thin-client acceptance input. The
eight dict-key ``resolve_orientation`` cases at lines 131-179 and 233 (digest
``c38ec56cf62f8133269d5bb7fef87d0302dd4608577c91ad9d90646cdd6000ca``)
are current H0/I5 scene-reference-owner and future N/I6 addressed
authoring-client acceptance input. Neither historical wire returns as a
compatibility parser. Their
underlying conversion/composition calculations remain exercised below.
"""

import inspect
import math

import pytest

from manyforge_behavior.geometry import (
    QUAT_AGREEMENT_DOT_TOL,
    GeometryError,
    add_rpy_deg,
    angle_between,
    compose_local,
    compose_world,
    distance_between,
    normalize_quat,
    quat_conjugate,
    quat_rotate_vector,
    quats_close,
    relative_pose_components,
    rpy_deg_to_xyzw,
    xyzw_to_rpy_deg,
)


_RPY_SAMPLES = [
    [0, 0, 0],
    [0, 0, -45],
    [180, 0, 0],
    [0, 0, 90],
    [0, 0, -90],
    [15, 0, 0],
    [0, 30, 0],
    [10, 20, 30],
    [-25, 5, -60],
    [0, 0, 179],
]


@pytest.mark.parametrize("rpy", _RPY_SAMPLES)
def test_rpy_quat_roundtrip(rpy):
    quaternion = rpy_deg_to_xyzw(rpy)
    assert quats_close(rpy_deg_to_xyzw(xyzw_to_rpy_deg(quaternion)), quaternion)


def test_graspable_minus45_about_z():
    quaternion = [0.0, 0.0, -0.3826834323650898, 0.9238795325112867]
    assert xyzw_to_rpy_deg(quaternion) == pytest.approx(
        [0.0, 0.0, -45.0],
        abs=1e-6,
    )
    assert quats_close(rpy_deg_to_xyzw([0, 0, -45]), quaternion)


@pytest.mark.parametrize("rpy", _RPY_SAMPLES)
def test_compose_local_then_inverse_is_identity(rpy):
    current = rpy_deg_to_xyzw([10, -20, 35])
    delta = rpy_deg_to_xyzw(rpy)
    assert quats_close(
        compose_local(compose_local(current, delta), quat_conjugate(delta)),
        current,
    )


def test_compose_local_vs_world_differ_for_noncommuting_axes():
    current = rpy_deg_to_xyzw([0, 0, 90])
    z_delta = rpy_deg_to_xyzw([0, 0, 15])
    assert quats_close(
        compose_local(current, z_delta),
        rpy_deg_to_xyzw([0, 0, 105]),
    )
    assert quats_close(
        compose_world(current, z_delta),
        rpy_deg_to_xyzw([0, 0, 105]),
    )

    x_delta = rpy_deg_to_xyzw([15, 0, 0])
    assert not quats_close(
        compose_local(current, x_delta),
        compose_world(current, x_delta),
    )


def test_normalization_and_vector_rotation_are_explicit_and_finite():
    assert normalize_quat([0, 0, 0, 2]) == [0.0, 0.0, 0.0, 1.0]
    huge = normalize_quat([1e308, 1e308, 1e308, 1e308])
    assert all(math.isfinite(component) for component in huge)
    assert math.hypot(*huge) == pytest.approx(1.0)
    quarter_turn = rpy_deg_to_xyzw([0, 0, 90])
    assert quat_rotate_vector(quarter_turn, [1, 0, 0]) == pytest.approx(
        [0, 1, 0],
        abs=1e-9,
    )


@pytest.mark.parametrize(
    ("function", "value"),
    [
        (rpy_deg_to_xyzw, [0, 0]),
        (rpy_deg_to_xyzw, [0, math.nan, 0]),
        (normalize_quat, [0, 0, 0, 0]),
        (normalize_quat, [0, False, 0, 1]),
        (quat_rotate_vector, [1, 2]),
    ],
)
def test_algorithms_reject_invalid_or_nonfinite_components(function, value):
    with pytest.raises(GeometryError):
        if function is quat_rotate_vector:
            function([0, 0, 0, 1], value)
        else:
            function(value)


def test_distance_and_angle_queries():
    assert distance_between([0, 0, 0], [3, 4, 0]) == pytest.approx(5.0)
    identity = rpy_deg_to_xyzw([0, 0, 0])
    quarter_turn = rpy_deg_to_xyzw([0, 0, 90])
    assert angle_between(identity, quarter_turn) == pytest.approx(90.0, abs=1e-6)
    assert angle_between(identity, identity) == pytest.approx(0.0, abs=1e-6)


def test_relative_pose_components_self_is_identity():
    position = [0.3, -0.3, 0.15]
    orientation = rpy_deg_to_xyzw([0, 0, -45])
    relative_position, relative_orientation = relative_pose_components(
        target_position_m=position,
        target_orientation_xyzw=orientation,
        reference_position_m=position,
        reference_orientation_xyzw=orientation,
    )
    assert relative_position == pytest.approx([0, 0, 0], abs=1e-9)
    assert quats_close(relative_orientation, [0, 0, 0, 1])


def test_relative_pose_components_recovers_rotated_offset():
    orientation = rpy_deg_to_xyzw([0, 0, 90])
    relative_position, relative_orientation = relative_pose_components(
        target_position_m=[1, 1, 0],
        target_orientation_xyzw=orientation,
        reference_position_m=[1, 0, 0],
        reference_orientation_xyzw=orientation,
    )
    assert relative_position == pytest.approx([1, 0, 0], abs=1e-9)
    assert quats_close(relative_orientation, [0, 0, 0, 1])


def test_add_rpy_deg_is_field_add():
    start = rpy_deg_to_xyzw([0, 11, -40])
    result = add_rpy_deg(start, [0, 73, 91])
    assert xyzw_to_rpy_deg(result) == pytest.approx([0, 84, 51], abs=1e-6)


def test_add_rpy_deg_diverges_from_composition_when_tilted():
    start = rpy_deg_to_xyzw([0, 11, -40])
    field_add = add_rpy_deg(start, [0, 73, 91])
    composed = compose_local(
        compose_local(start, rpy_deg_to_xyzw([0, 73, 0])),
        rpy_deg_to_xyzw([0, 0, 91]),
    )
    assert not quats_close(field_add, composed)
    assert angle_between(field_add, composed) > 90.0


def test_add_rpy_deg_matches_composition_from_pure_yaw():
    start = rpy_deg_to_xyzw([0, 0, -45])
    field_add = add_rpy_deg(start, [0, 11, 5])
    composed = compose_local(
        compose_local(start, rpy_deg_to_xyzw([0, 0, 5])),
        rpy_deg_to_xyzw([0, 11, 0]),
    )
    assert quats_close(field_add, composed)


def test_quats_close_near_boundary_and_accepts_sign_flip():
    identity = rpy_deg_to_xyzw([0, 0, 0])
    assert quats_close(identity, rpy_deg_to_xyzw([0, 0, 4]))
    assert not quats_close(identity, rpy_deg_to_xyzw([0, 0, 6]))
    assert quats_close(identity, [-component for component in identity])


@pytest.mark.parametrize("tolerance", [False, 0, 1, math.nan, "0.1"])
def test_quats_close_rejects_invalid_tolerance(tolerance) -> None:
    with pytest.raises(GeometryError, match="tol must be a finite number"):
        quats_close([0, 0, 0, 1], [0, 0, 0, 1], tolerance)


def test_distance_rejects_unrepresentable_finite_difference() -> None:
    with pytest.raises(GeometryError, match="finite numeric range"):
        distance_between([1e308, 0, 0], [-1e308, 0, 0])


def test_quat_agreement_constant_is_wired():
    signature = inspect.signature(quats_close)
    assert signature.parameters["tol"].default == QUAT_AGREEMENT_DOT_TOL == 1e-3
