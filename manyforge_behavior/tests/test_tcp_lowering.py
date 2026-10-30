"""TCP selector lowering against captured Program target bindings."""

import pytest

from manyforge_behavior.production_ports import (
    ProductionPortError,
    _TargetBinding,
    _motion_tcp_payload,
    _tcp_selector_payload,
)


def _targets():
    return {
        "arm": _TargetBinding("arm", "robot", "robot-1", "arm-target", {}),
        "tool": _TargetBinding("tool", "gripper", "gripper-1", "gripper-target", {}),
    }


def test_selector_forms_resolve_to_native_wire():
    targets = _targets()
    entry = {"component_id": "gripper-1", "target_id": "gripper-target", "frame_role_id": "tip"}
    assert _tcp_selector_payload(
        {"kind": "entry", "target_role": "tool", "frame_role": "tip"}, targets=targets
    ) == {"kind": "entry", "entry": entry}
    assert _tcp_selector_payload(entry, targets=targets) == {"kind": "entry", "entry": entry}
    assert _tcp_selector_payload({"kind": "default_of_role", "role": "tool"}, targets=targets) == {
        "kind": "default_of_role", "role": "tool"
    }
    assert _motion_tcp_payload({"target_role": "arm"}, targets=targets, active_tcp=entry) == {
        "tcp_endpoint": {"kind": "entry", "entry": entry}
    }


def test_current_without_active_tcp_names_role_scoped_key():
    with pytest.raises(ProductionPortError, match=r"active_tcp\.arm"):
        _motion_tcp_payload({"target_role": "arm"}, targets=_targets())


def test_planning_phase_uses_planning_selector():
    from manyforge_behavior.orchestration_ports import NodeAttemptIdentity
    from manyforge_behavior.production_ports import NativeKernelProgramPort

    selector = _tcp_selector_payload({"kind": "default_of_role", "role": "tool"}, targets=_targets())
    identity = NodeAttemptIdentity(
        program_run_id="run", node_instance_id="planner", node_attempt_id="attempt"
    )
    payload = NativeKernelProgramPort._phase_motion_payload(
        identity=identity, parameters={"base_frame_role": "base"},
        tcp_endpoint=selector, phase="approach", motion={"kind": "pose_goal"},
    )
    assert payload["tcp_endpoint"] == {"kind": "default_of_role", "role": "tool"}
