"""TCP selector lowering against captured Program target bindings."""

import pytest

from manyforge_behavior.production_ports import (
    ProductionPortError,
    _TargetBinding,
    _motion_tcp_payload,
    _tcp_selector_payload,
)
from .v3_fixtures import frame, target


def _targets():
    return {
        ("robot-1", "arm-target"): _TargetBinding("robot-1", "arm-target", {}),
        ("gripper-1", "gripper-target"): _TargetBinding("gripper-1", "gripper-target", {}),
    }


def test_selector_forms_resolve_to_native_wire():
    targets = _targets()
    entry = {"target": target("gripper-1", "gripper-target"), "frame": frame("gripper-1", "tip")}
    selected = {"kind": "entry", **entry}
    assert _tcp_selector_payload(selected, targets=targets) == selected
    assert _tcp_selector_payload({"kind": "current"}, targets=targets, active_tcp=entry) == selected
    default = {"kind": "default_of_target", "target": target("gripper-1", "gripper-target")}
    assert _tcp_selector_payload(default, targets=targets) == default
    assert _motion_tcp_payload({"target": target("robot-1", "arm-target")},
                               targets=targets, active_tcp=entry) == {"tcp_endpoint": selected}
    with pytest.raises(ProductionPortError):
        _tcp_selector_payload({"kind": "default_of_role", "role": "tool"}, targets=targets)


def test_current_without_active_tcp_names_direct_target_key():
    with pytest.raises(ProductionPortError, match=r"active_tcp/robot-1/arm-target"):
        _motion_tcp_payload({"target": target("robot-1", "arm-target")}, targets=_targets())


def test_planning_phase_uses_planning_selector():
    from manyforge_behavior.orchestration_ports import NodeAttemptIdentity
    from manyforge_behavior.production_ports import NativeKernelProgramPort

    default = {"kind": "default_of_target", "target": target("gripper-1", "gripper-target")}
    selector = _tcp_selector_payload(default, targets=_targets())
    identity = NodeAttemptIdentity(
        program_run_id="run", node_instance_id="planner", node_attempt_id="attempt"
    )
    payload = NativeKernelProgramPort._phase_motion_payload(
        identity=identity, parameters={"base_frame": frame("robot-1", "base")},
        tcp_endpoint=selector, phase="approach", motion={"kind": "pose_goal"},
    )
    assert payload["tcp_endpoint"] == default
