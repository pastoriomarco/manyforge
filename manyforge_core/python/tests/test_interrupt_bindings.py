import math

import manyforge_core_py as core
import pytest


def test_typed_stop_profile_round_trip_and_validation() -> None:
    profile = core.MotionInterruptStopProfileDTO(
        profile_id="soft.profile.v2",
        stop_mode=core.MotionInterruptStopMode.kSoftOnPath,
        deceleration_time_s=1.25,
        per_joint_max_deceleration={
            "joint_a": core.AngularAccelerationDTO(2.0),
            "slide_z": core.LinearAccelerationDTO(0.4),
        },
        allow_natural_completion_on_short_path=True,
        recoverable_substitution_mode=(
            core.MotionInterruptStopMode.kSpringBackHold
        ),
        require_backend_confirmation=True,
        per_joint_settle_velocity_threshold={
            "joint_a": core.AngularVelocityDTO(0.01),
            "slide_z": core.LinearVelocityDTO(0.002),
        },
        diagnostics={
            "qualified": True,
            "revision": "v2",
            "tolerance": 0.5,
        },
    )

    assert core.motion_interrupt_stop_profile_field_error(profile) == ""
    assert profile.profile_id == "soft.profile.v2"
    assert profile.stop_mode == core.MotionInterruptStopMode.kSoftOnPath
    assert profile.recoverable_substitution_mode == (
        core.MotionInterruptStopMode.kSpringBackHold
    )
    assert profile.per_joint_max_deceleration["joint_a"].value_rad_s2 == 2.0
    assert profile.per_joint_max_deceleration["slide_z"].value_m_s2 == 0.4
    assert profile.per_joint_settle_velocity_threshold is not None
    assert (
        profile.per_joint_settle_velocity_threshold["joint_a"].value_rad_s
        == 0.01
    )
    assert profile.diagnostics == {
        "qualified": True,
        "revision": "v2",
        "tolerance": 0.5,
    }
    assert core.project_interrupt_structured_value_to_text(
        profile.diagnostics
    ) == {
        "qualified": "true",
        "revision": "v2",
        "tolerance": "0.500000",
    }


def test_profile_surface_has_no_retired_fallback_or_timeout_fields() -> None:
    profile = core.MotionInterruptStopProfileDTO(
        profile_id="hard.profile",
        stop_mode=core.MotionInterruptStopMode.kHardDeceleration,
    )

    assert core.motion_interrupt_stop_profile_field_error(profile) == ""
    assert core.motion_interrupt_deceleration_time_or_default(profile) == 1.0
    assert not hasattr(profile, "max_stop_time_s")
    assert not hasattr(profile, "allowed_fallback_modes")
    assert not hasattr(profile, "joint_deceleration_profile_id")
    assert not hasattr(profile, "per_joint_max_deceleration_rad_s2")
    assert not hasattr(profile, "settle_velocity_threshold_rad_s")


def test_profile_and_closed_structured_value_fail_closed() -> None:
    invalid_substitution = core.MotionInterruptStopProfileDTO(
        profile_id="hard.profile",
        stop_mode=core.MotionInterruptStopMode.kHardDeceleration,
        recoverable_substitution_mode=(
            core.MotionInterruptStopMode.kSpringBackHold
        ),
    )
    assert "only soft_on_path" in core.motion_interrupt_stop_profile_field_error(
        invalid_substitution
    )

    empty_settle = core.MotionInterruptStopProfileDTO(
        profile_id="hard.profile",
        stop_mode=core.MotionInterruptStopMode.kHardDeceleration,
        per_joint_settle_velocity_threshold={},
    )
    assert "omitted or non-empty" in (
        core.motion_interrupt_stop_profile_field_error(empty_settle)
    )

    assert core.interrupt_structured_value_error({}) is None
    assert core.interrupt_structured_value_error({}, True) is not None
    assert core.interrupt_structured_value_error({"gain": math.inf}) is not None
    with pytest.raises(TypeError):
        core.interrupt_structured_value_error({"nested": {"value": 1.0}})
