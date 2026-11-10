from manyforge_behavior.reason_codes import ReasonCode


def test_shared_reason_codes_retain_machine_values() -> None:
    assert ReasonCode.PREVIEW_FAILED.value == "preview_failed"
    assert (
        ReasonCode.COLLISION_VALIDATION_WORKER_EXCEPTION.value
        == "collision_validation_worker_exception"
    )
    assert ReasonCode.GRIPPER_STATE_UNAVAILABLE.value == "gripper_state_unavailable"
    assert ReasonCode.UNKNOWN_NON_STD_EXCEPTION.value == "unknown_non_std_exception"
    assert ReasonCode.TRACKING_SAMPLE_INVALID.value == "tracking_sample_invalid"
