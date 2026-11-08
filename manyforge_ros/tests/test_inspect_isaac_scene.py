"""Cutover evidence for the removed Isaac collider topic-RPC client."""

from __future__ import annotations

from manyforge_ros import inspect_isaac_scene


def test_inspect_isaac_scene_refuses_before_any_effect(capsys) -> None:
    assert inspect_isaac_scene.main(["--include-subtree", "must-not-be-read"]) == 78
    assert capsys.readouterr().err.strip() == inspect_isaac_scene.UNAVAILABLE_MESSAGE
    payload = inspect_isaac_scene.unavailable_payload()

    assert payload["reason_code"] == "composable_cell_runtime_unavailable"
    assert payload["restoration_gate"] == "I9/I"
    assert payload["historical_source_blob"] == (
        "1d074cc77f3a25ccb1af9f0616bdff2bf1298ccc"
    )
    assert payload["historical_test_blob"] == (
        "03e1c87e5a8689fa2fa832b500395b115028bbff"
    )
    assert len(payload["historical_acceptance"]) == 2
    source = inspect_isaac_scene.__file__
    assert source is not None


def test_all_six_historical_scene_inspection_cases_remain_assigned() -> None:
    assignments = inspect_isaac_scene.unavailable_payload()["historical_acceptance"]
    assert assignments == [
        "lines 9-96: correlated read-only ROS topic RPC and bounded cleanup",
        "lines 98-154: five fail-closed response validation cases",
    ]
    assert inspect_isaac_scene.unavailable_payload()["removed_contracts"] == [
        "manyforge.isaac.collider_query.v0.1",
        "manyforge.isaac.collider_report.v0.1",
    ]
