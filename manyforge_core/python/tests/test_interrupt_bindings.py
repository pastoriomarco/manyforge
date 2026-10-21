import math

import manyforge_core_py as core
import pytest


def test_explicit_jerk_settings_resolve_without_changing_the_authored_profile():
    profile = core.MotionInterruptStopProfileDTO(
        profile_id="jerk.profile",
        stop_mode=core.MotionInterruptStopMode.kSoftOnPath,
        deceleration_time_s=1.0,
        per_joint_max_deceleration={
            "joint": core.AngularAccelerationDTO(4.0),
            "slide": core.LinearAccelerationDTO(0.5),
        },
        jerk_limits_enabled=True,
        per_joint_max_jerk={"joint": core.AngularJerkDTO(20.0)},
    )
    frozen = core.resolve_motion_interrupt_stop_jerk_limits(profile)
    assert len(profile.per_joint_max_jerk) == 1
    assert frozen.per_joint_max_jerk["joint"].value_rad_s3 == 20.0
    # Q36: missing jerk is derived per Stop phase at Stop time, not frozen.
    assert "slide" not in frozen.per_joint_max_jerk
    profile.jerk_limits_enabled = False
    assert (
        len(core.resolve_motion_interrupt_stop_jerk_limits(profile).per_joint_max_jerk)
        == 1
    )
    profile.jerk_limits_enabled = None
    with pytest.raises(ValueError, match="explicit"):
        core.resolve_motion_interrupt_stop_jerk_limits(profile)


def test_omitted_jerk_choice_cannot_silently_waive_a_limit() -> None:
    profile = core.MotionInterruptStopProfileDTO(
        profile_id="unreviewed.profile",
        stop_mode=core.MotionInterruptStopMode.kSoftOnPath,
        deceleration_time_s=1.0,
        per_joint_max_deceleration={"joint": core.AngularAccelerationDTO(4.0)},
    )
    assert profile.jerk_limits_enabled is None
    with pytest.raises(ValueError, match="explicit"):
        core.resolve_motion_interrupt_stop_jerk_limits(profile)
    profile.jerk_limits_enabled = False
    assert not core.resolve_motion_interrupt_stop_jerk_limits(
        profile
    ).per_joint_max_jerk


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
        fallback_stop_modes=[core.MotionInterruptStopMode.kSpringBackHold],
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
    assert list(profile.fallback_stop_modes) == [core.MotionInterruptStopMode.kSpringBackHold]
    assert profile.per_joint_max_deceleration["joint_a"].value_rad_s2 == 2.0
    assert profile.per_joint_max_deceleration["slide_z"].value_m_s2 == 0.4
    assert profile.per_joint_settle_velocity_threshold is not None
    assert profile.per_joint_settle_velocity_threshold["joint_a"].value_rad_s == 0.01
    assert profile.diagnostics == {
        "qualified": True,
        "revision": "v2",
        "tolerance": 0.5,
    }
    assert core.project_interrupt_structured_value_to_text(profile.diagnostics) == {
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
    # Q51 (as `main`): the per-profile controlled Stop deadline is restored.
    assert profile.max_stop_time_s is None
    profile.max_stop_time_s = 3.0
    assert core.motion_interrupt_stop_profile_field_error(profile) == ""
    profile.max_stop_time_s = 0.0
    assert core.motion_interrupt_stop_profile_field_error(profile) != ""
    assert not hasattr(profile, "allowed_fallback_modes")
    assert not hasattr(profile, "joint_deceleration_profile_id")
    assert not hasattr(profile, "per_joint_max_deceleration_rad_s2")
    assert not hasattr(profile, "settle_velocity_threshold_rad_s")


def test_profile_and_closed_structured_value_fail_closed() -> None:
    invalid_substitution = core.MotionInterruptStopProfileDTO(
        profile_id="hard.profile",
        stop_mode=core.MotionInterruptStopMode.kHardDeceleration,
        fallback_stop_modes=[core.MotionInterruptStopMode.kSpringBackHold],
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
