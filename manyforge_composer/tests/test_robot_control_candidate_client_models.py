"""Reusable Robot Control values and the typed I7/I8 route gate."""

import pytest
from pydantic import ValidationError

from manyforge_composer.backend import robot_control_candidates
from manyforge_composer.backend.robot_control_candidates import (
    CandidateJointTarget,
    create_robot_control_candidates_router,
)


def test_joint_candidate_target_is_finite_unique_and_length_matched() -> None:
    target = CandidateJointTarget.model_validate(
        {"joint_names": ["joint_1", "joint_2"], "positions_rad": [0.1, -0.2]}
    )
    assert target.positions_rad == [0.1, -0.2]

    with pytest.raises(ValidationError):
        CandidateJointTarget.model_validate(
            {"joint_names": ["joint_1", "joint_1"], "positions_rad": [0.1, 0.2]}
        )
    with pytest.raises(ValidationError):
        CandidateJointTarget.model_validate(
            {"joint_names": ["joint_1"], "positions_rad": [float("nan")]}
        )


def test_joint_authoring_value_rejects_unknown_wire_aliases() -> None:
    with pytest.raises(ValidationError):
        CandidateJointTarget.model_validate(
            {"jointNames": ["joint_1"], "positionsRad": [0.1]}
        )


def test_backend_exports_no_singleton_candidate_request_wire() -> None:
    assert not hasattr(robot_control_candidates, "CandidateCreateRequest")
    assert not hasattr(robot_control_candidates, "CandidateStatus")
    assert not hasattr(robot_control_candidates, "CandidateCartesianTarget")
    assert not hasattr(robot_control_candidates, "CandidateGripperTarget")


def test_candidate_route_is_fenced_before_store_or_state_access() -> None:
    class PoisonState:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"state was accessed: {name}")

    with pytest.raises(
        RuntimeError,
        match="composable_cell_robot_control_client_unavailable",
    ):
        create_robot_control_candidates_router(PoisonState())
