from manyforge_behavior.pick_stage_causes import PickStageFailureCause
from manyforge_behavior.reason_codes import ReasonCode


def test_pick_stage_causes_are_backed_by_shared_reason_codes() -> None:
    assert PickStageFailureCause is ReasonCode
    assert ReasonCode.RAW_IK_FAILURE.value == "raw_ik_failure"
    assert (
        ReasonCode.COLLISION_VALIDATION_WORKER_EXCEPTION.value
        == "collision_validation_worker_exception"
    )
    assert ReasonCode.GRIPPER_STATE_UNAVAILABLE.value == "gripper_state_unavailable"
    assert ReasonCode.UNKNOWN_NON_STD_EXCEPTION.value == "unknown_non_std_exception"
    assert ReasonCode.TRACKING_SAMPLE_INVALID.value == "tracking_sample_invalid"
