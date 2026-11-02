"""Cutover evidence for the removed Isaac mesh-transfer topic RPC."""

from __future__ import annotations

from manyforge_ros import isaac_mesh_transfer


def test_isaac_mesh_transfer_is_a_pre_effect_i9_boundary(capsys) -> None:
    assert isaac_mesh_transfer.main(["--output-path", "must-not-be-written"]) == 78
    assert capsys.readouterr().err.strip() == isaac_mesh_transfer.UNAVAILABLE_MESSAGE
    payload = isaac_mesh_transfer.unavailable_payload()

    assert payload["reason_code"] == "composable_cell_runtime_unavailable"
    assert payload["restoration_gate"] == "I9/I"
    assert payload["historical_source_blob"] == (
        "ed0b7fa46046f9e257baae9f6f07c97049b11718"
    )
    assert payload["historical_test_blob"] == (
        "3653dea92b75948e9bd8a47d1b9ce6af4c3bcc66"
    )


def test_both_historical_mesh_transfer_cases_remain_assigned() -> None:
    payload = isaac_mesh_transfer.unavailable_payload()
    assert payload["historical_acceptance"] == [
        "lines 11-37: packet framing, length, and sha256 integrity",
        "lines 40-156: correlated two-chunk ROS transfer and bounded cleanup",
    ]
    assert len(payload["removed_contracts"]) == 3
