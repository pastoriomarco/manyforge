"""Pure and unavailable-boundary evidence for simulator control.

Historical live source blob ``769e97215b0df885dd7002ca20ded0e0469b6ef7``
contains the removed client at lines 47-415, one-shot runner/CLI at lines
418-463, and dynamic import hook at lines 519-522.  Historical test blob
``1d9c3d6bb93f7ccc7f377ef7e151e91f338f890f`` contains three live cases at
lines 9-67, 69-112, and 114-218.  Their exact I9/I assignments are recorded
below instead of leaving a runnable pre-I9 ROS fallback.
"""

from __future__ import annotations

import inspect
import math

import pytest

from manyforge_ros import simulation_control


HISTORICAL_LIVE_CASES = (
    (
        "test_simulation_control_resets_state_without_touching_command_gate",
        "lines 9-67",
        "I9/I selected simulation adapter: ResetSimulation success, bounded cleanup, "
        "and proof that the adapter does not publish a second command gate",
    ),
    (
        "test_simulation_control_surfaces_reset_service_failure",
        "lines 69-112",
        "I9/I selected simulation adapter: ResetSimulation result code and message "
        "propagate as a failed typed outcome",
    ),
    (
        "test_simulation_control_state_step_and_entity_services",
        "lines 114-218",
        "I9/I selected simulation adapter: pause, bounded step, and entity-pose "
        "mutation use the retained simulation_interfaces contract",
    ),
)


def test_simulator_result_records_remain_owner_neutral() -> None:
    reset = simulation_control.SimulationResetResult(
        success=False,
        service_name="/reset_simulation",
        scope="state",
        duration_s=0.25,
        result_code=7,
        result_message="rejected",
        error="7: rejected",
    )
    control = simulation_control.SimulationControlResult(
        success=True,
        operation="step",
        service_name="/step_simulation",
        duration_s=0.5,
        steps=3,
    )

    assert reset.to_dict() == {
        "success": False,
        "service_name": "/reset_simulation",
        "scope": "state",
        "duration_s": 0.25,
        "result_code": 7,
        "result_message": "rejected",
        "error": "7: rejected",
    }
    assert control.to_dict()["steps"] == 3
    assert control.to_dict()["operation"] == "step"


def test_service_selection_is_deterministic_and_prefix_bounded() -> None:
    assert simulation_control.select_simulation_service_name(
        ["/vendor/reset", "/reset_simulation"],
        service_type=simulation_control.RESET_SERVICE_TYPE,
    ) == "/reset_simulation"
    assert simulation_control.select_simulation_service_name(
        ["/other/reset", "/isaacsim/reset_simulation"],
        service_type=simulation_control.RESET_SERVICE_TYPE,
        service_prefix="/isaacsim/",
    ) == "/isaacsim/reset_simulation"
    assert (
        simulation_control.select_simulation_service_name(
            ["/other/reset"],
            service_type=simulation_control.RESET_SERVICE_TYPE,
            service_prefix="isaacsim",
        )
        is None
    )


def test_service_selection_rejects_unbounded_or_duplicate_candidates() -> None:
    with pytest.raises(ValueError, match="candidates exceed"):
        simulation_control.select_simulation_service_name(
            [f"/service/{index}" for index in range(4097)],
            service_type=simulation_control.RESET_SERVICE_TYPE,
        )
    with pytest.raises(ValueError, match="candidates must be unique"):
        simulation_control.select_simulation_service_name(
            ["/reset_simulation", " /reset_simulation "],
            service_type=simulation_control.RESET_SERVICE_TYPE,
        )
    with pytest.raises(ValueError, match="prefix exceeds"):
        simulation_control.select_simulation_service_name(
            ["/reset_simulation"],
            service_type=simulation_control.RESET_SERVICE_TYPE,
            service_prefix="p" * 1025,
        )


@pytest.mark.parametrize(
    ("raw", "expected"),
    [(" play ", "playing"), ("PLAYING", "playing"), ("pause", "paused")],
)
def test_simulation_state_normalization(raw: str, expected: str) -> None:
    assert simulation_control.normalize_simulation_state(raw) == expected


def test_reset_scope_and_entity_pose_normalization() -> None:
    assert simulation_control.normalize_reset_scope(" STATE ") == "state"
    frame_id, position, orientation = simulation_control.normalize_entity_pose(
        {
            "frame_id": " world ",
            "position_m": [1, 2.5, 3],
            "orientation_rpy_deg": [0.0, 0.0, 90.0],
        }
    )

    assert frame_id == "world"
    assert position == (1.0, 2.5, 3.0)
    assert orientation == pytest.approx(
        (0.0, 0.0, math.sqrt(0.5), math.sqrt(0.5))
    )

    _, _, direct_orientation = simulation_control.normalize_entity_pose(
        {
            "frame_id": "world",
            "position_m": [0.0, 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 2.0],
        }
    )
    assert direct_orientation == (0.0, 0.0, 0.0, 1.0)


@pytest.mark.parametrize(
    ("pose", "message"),
    [
        ({}, "frame_id is required"),
        ({"frame_id": "world", "position_m": [1, 2]}, "must have length 3"),
        (
            {"frame_id": "world", "position_m": [1, 2, 3]},
            "must include orientation_xyzw or orientation_rpy_deg",
        ),
    ],
)
def test_entity_pose_normalization_rejects_incomplete_shapes(
    pose: dict[str, object],
    message: str,
) -> None:
    with pytest.raises(ValueError, match=message):
        simulation_control.normalize_entity_pose(pose)


@pytest.mark.parametrize(
    ("pose", "message"),
    [
        (
            {
                "frame_id": "f" * 1025,
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "frame_id exceeds",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [True, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "text or boolean",
        ),
        (
            {
                "frame_id": "world",
                "position_m": ["1.0", 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "text or boolean",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [float("inf"), 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "must be finite",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 0.0],
            },
            "finite nonzero norm",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [0.0, 0.0, 0.0],
                "orientation_rpy_deg": [False, 0.0, 0.0],
            },
            "text or boolean",
        ),
    ],
)
def test_entity_pose_normalization_rejects_unbounded_or_non_finite_values(
    pose: dict[str, object],
    message: str,
) -> None:
    with pytest.raises(ValueError, match=message):
        simulation_control.normalize_entity_pose(pose)


def test_live_simulation_control_is_not_a_dormant_callable_body(capsys) -> None:
    assert not hasattr(simulation_control, "SimulationControlClient")
    source = inspect.getsource(simulation_control)
    for forbidden in ("_import_module", "create_client", "call_async", "create_node"):
        assert forbidden not in source

    assert simulation_control.main() == simulation_control.UNAVAILABLE_EXIT_CODE
    assert capsys.readouterr().err.strip() == simulation_control.UNAVAILABLE_MESSAGE
    with pytest.raises(
        simulation_control.SimulationControlUnavailableError,
        match=simulation_control.UNAVAILABLE_REASON_CODE,
    ):
        simulation_control.reset_once(
            scope="ignored-before-validation",
            service_prefix="ignored-before-selection",
            service_timeout_s=-1.0,
        )


def test_all_three_historical_live_cases_remain_assigned_to_i9() -> None:
    assert simulation_control.HISTORICAL_SOURCE_BLOB == (
        "769e97215b0df885dd7002ca20ded0e0469b6ef7"
    )
    assert simulation_control.HISTORICAL_TEST_BLOB == (
        "1d9c3d6bb93f7ccc7f377ef7e151e91f338f890f"
    )
    assert [case[0] for case in HISTORICAL_LIVE_CASES] == [
        "test_simulation_control_resets_state_without_touching_command_gate",
        "test_simulation_control_surfaces_reset_service_failure",
        "test_simulation_control_state_step_and_entity_services",
    ]
    assert [case[1] for case in HISTORICAL_LIVE_CASES] == [
        "lines 9-67",
        "lines 69-112",
        "lines 114-218",
    ]
    assert all(
        "I9/I selected simulation adapter" in case[2]
        for case in HISTORICAL_LIVE_CASES
    )
