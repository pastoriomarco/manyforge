"""The runtime retains strict direct TCP references in frozen owner inputs."""

import pytest

from manyforge_behavior.orchestration_ports import OwnerNodeInputs
from manyforge_behavior.production_ports import ProductionPortError, _motion_tcp_payload
from .v3_fixtures import frame, target


@pytest.mark.parametrize("selector", [
    {"kind": "entry", "target": target("robot.1", "arm.command"), "frame": frame("tool.1", "tcp")},
    {"kind": "default_of_target", "target": target("tool.1", "gripper.command")},
])
def test_frozen_direct_tcp_references_reach_native_selection(selector):
    inputs = OwnerNodeInputs({"target": target("robot.1", "arm.command"), "tcp_endpoint": selector})
    assert _motion_tcp_payload(inputs.parameters, targets={}) == {"tcp_endpoint": selector}


@pytest.mark.parametrize("selector", [
    {"kind": "entry", "target": target("robot.1", "arm.command"), "frame": {"component_id": "tool.1"}},
    {"kind": "entry", "target": {**target("robot.1", "arm.command"), "extra": "bad"}, "frame": frame("tool.1", "tcp")},
    {"kind": "default_of_target", "target": {"component_id": "tool.1"}},
])
def test_frozen_malformed_direct_tcp_references_are_rejected(selector):
    inputs = OwnerNodeInputs({"target": target("robot.1", "arm.command"), "tcp_endpoint": selector})
    with pytest.raises(ProductionPortError):
        _motion_tcp_payload(inputs.parameters, targets={})


def test_unbound_or_ambiguous_captured_references_are_refused():
    from manyforge_behavior.production_ports import _require_captured_reference

    identity = frame("tool.1", "tcp")
    with pytest.raises(ProductionPortError, match="absent from the Program Binding"):
        _require_captured_reference({"frame_bindings": []}, "frame_bindings", identity, field="frame")
    with pytest.raises(ProductionPortError, match="ambiguous in the Program Binding"):
        _require_captured_reference({"frame_bindings": [{"reference": identity}] * 2},
                                    "frame_bindings", identity, field="frame")
    _require_captured_reference({"frame_bindings": [{"reference": identity}]},
                                "frame_bindings", identity, field="frame")
