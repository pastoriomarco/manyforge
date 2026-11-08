from manyforge_behavior.orchestration_ports import ProgramRunPortContext
from manyforge_behavior.production_ports import HeadlessSceneProgramPort


def test_scene_binding_keeps_component_observations_and_target_authority_distinct():
    digest = "sha256:" + "0" * 64
    binding = {
        "program_id": "program", "program_revision": digest, "program_binding_id": digest,
        "retained_bundle_id": digest, "graph_revision": digest,
        "component_bindings": [{"component_id": "tool", "model_bindings": [
            {"model_role_id": "tool_model", "model_instance_id": "model", "model_revision": "revision"}]}],
        "frame_bindings": [
            {"reference": {"component_id": "tool", "frame_id": "tcp"}, "model_instance_id": "model", "model_revision": "revision"},
            {"reference": {"component_id": "tool", "frame_id": "other"}, "model_instance_id": "model", "model_revision": "revision"}],
        "collision_body_bindings": [
            {"reference": {"component_id": "tool", "collision_body_id": "jaw"}, "model_instance_id": "model", "model_revision": "revision"},
            {"reference": {"component_id": "tool", "collision_body_id": "cover"}, "model_instance_id": "model", "model_revision": "revision"}],
        "target_bindings": [{"target": {"component_id": "tool", "command_target_id": "grip"},
                             "frame_references": [{"component_id": "tool", "frame_id": "tcp"}],
                             "collision_body_references": [{"component_id": "tool", "collision_body_id": "jaw"}]}],
    }
    capture = ProgramRunPortContext("run", digest, "scenario", digest, "program", digest,
                                    digest, digest, digest, digest, binding)
    context = HeadlessSceneProgramPort._parse_context(capture)
    target = context.targets[("tool", "grip")]
    assert set(target.frames) == {"tcp"}
    assert target.collision_body_roles == frozenset({"jaw"})
    assert ("tool", "other") in context.component_frames


def test_tcp_selector_preserves_independent_target_and_frame_owners():
    from manyforge_behavior.production_ports import _tcp_selector_payload

    entry = {"target": {"component_id": "robot", "command_target_id": "arm"},
             "frame": {"component_id": "tool", "frame_id": "TCP"}}
    expected = {"kind": "entry", **entry}
    assert _tcp_selector_payload({"kind": "current"}, targets={}, active_tcp=entry) == expected
    assert _tcp_selector_payload(expected, targets={}) == expected
    selector = {"kind": "default_of_target", "target": entry["target"]}
    assert _tcp_selector_payload(selector, targets={}) == selector


def test_motion_component_frame_preserves_exact_owner():
    from manyforge_behavior.production_ports import _frame_reference

    frame = {"component_id": "tool", "frame_id": "TCP"}
    assert _frame_reference({"kind": "target_frame", "frame": frame}, field="goal", targets={},
                            binding={"frame_bindings": [{"reference": frame}]}) == {
        "type": "component_frame", "frame": frame}


def test_motion_component_body_and_base_frame_are_direct():
    from manyforge_behavior.production_ports import _is_base_frame, _reference

    base = {"component_id": "robot", "frame_id": "base"}
    body = {"type": "component_body", "body": {"component_id": "tool", "collision_body_id": "Jaw"}}
    assert _reference(body, field="reference", base_frame=base, targets={},
                      binding={"collision_body_bindings": [{"reference": body["body"]}]}) == body
    assert _is_base_frame({"kind": "target_frame", "frame": base}, field="frame", base_frame=base)
    assert not _is_base_frame({"kind": "target_frame", "frame": {**base, "component_id": "other"}}, field="frame", base_frame=base)


def test_mock_simulation_preserves_exact_signal_identity():
    from manyforge_behavior.production_ports import _mock_simulation_row

    row = {"signal": {"component_id": "io", "signal_name": "CI0"}, "start_value": True}
    assert _mock_simulation_row(row) == {**row, "change": None}


def test_motion_uses_reserved_key_for_exact_target():
    from manyforge_behavior.production_ports import _active_tcp_input_key

    assert _active_tcp_input_key({"target": {"component_id": "robot", "command_target_id": "arm.A"}}) == "active_tcp/robot/arm.A"


def test_motion_tcp_payload_accepts_direct_v3_selector():
    from manyforge_behavior.production_ports import _motion_tcp_payload

    target = {"component_id": "robot", "command_target_id": "arm"}
    frame = {"component_id": "tool", "frame_id": "TCP"}
    selector = {"kind": "entry", "target": target, "frame": frame}
    assert _motion_tcp_payload({"target": target, "tcp_endpoint": selector}, targets={}) == {"tcp_endpoint": selector}


def test_runtime_context_uses_exact_direct_target_and_derived_signal_rows():
    from manyforge_behavior.production_ports import NativeKernelProgramPort, _io_signal, IoSignalRefusedError
    import pytest

    digest = "sha256:" + "1" * 64
    target = {"component_id": "io", "command_target_id": "bank"}
    output = {"component_id": "io", "signal_name": "DO0"}
    derived = {"component_id": "io", "signal_name": "DO0.changed_outside"}
    def row(ref, direction):
        return {"reference": ref, "target": target, "runtime_unit_id": "unit", "value_type": "bool",
                "direction": direction, "transition_capability": "sampled_only", "signal_descriptor_digest": digest}
    binding = {"program_id": "program", "program_revision": digest, "program_binding_id": digest,
               "retained_bundle_id": digest, "graph_revision": digest,
               "target_bindings": [{"target": target, "capability_bindings": []}],
               "signal_bindings": [row(output, "output"), row(derived, "input")]}
    capture = ProgramRunPortContext("run", digest, "scenario", digest, "program", digest,
                                    digest, digest, digest, digest, binding)
    context = NativeKernelProgramPort._parse_context(capture, request_owner_epoch="epoch")
    assert context.targets[("io", "bank")].reference() == target
    assert _io_signal(context.signals, output, output=True).signal_name == "DO0"
    assert _io_signal(context.signals, derived).signal_name == "DO0.changed_outside"
    with pytest.raises(IoSignalRefusedError) as error:
        _io_signal(context.signals, {**output, "signal_name": "DO1.changed_outside"})
    assert error.value.details["reference"]["signal_name"] == "DO1"


def test_io_read_sends_signal_pair_and_checks_returned_scope():
    from types import SimpleNamespace
    from manyforge_behavior.orchestration_ports import NodeAttemptIdentity, OwnerNodeInputs
    from manyforge_behavior.production_ports import _NativeTargetQueryPort, _SignalBinding

    reference = {"component_id": "io", "signal_name": "CI0"}
    binding = _SignalBinding("io", "CI0", {"component_id": "io", "command_target_id": "bank"},
                             "bool", "input", "sampled_only", "sha256:" + "0" * 64)
    capture = SimpleNamespace(program_binding_id="binding")
    context = SimpleNamespace(signals={("io", "CI0"): binding}, request_owner_epoch="epoch", capture=capture)
    requests = []
    def query(request):
        requests.append(request)
        return {"target_observation": {**request, "schema_version": "manyforge.target_observation.v2",
                "value": {"value": True, "quality": "fresh"}}}
    owner = SimpleNamespace(_context=lambda run: context, _kernel=SimpleNamespace(query_target=query))
    port = _NativeTargetQueryPort(owner)
    result = port._read_io_input("io.get_input", NodeAttemptIdentity("run", "node", "attempt"),
                                 OwnerNodeInputs({"signal": reference}))
    assert result.succeeded and result.output is True
    assert requests[0]["signal"] == reference
    assert "target_role_id" not in requests[0] and "resource_role_id" not in requests[0]
    from manyforge_behavior.production_ports import ProductionPortError
    import pytest
    owner._kernel.query_target = lambda request: {"target_observation": {
        **request, "schema_version": "manyforge.target_observation.v2",
        "signal": {**reference, "signal_name": "CI1"},
        "value": {"value": True, "quality": "fresh"}}}
    with pytest.raises(ProductionPortError, match="crossed the captured query"):
        port._read_io_input("io.get_input", NodeAttemptIdentity("run", "node", "attempt"),
                            OwnerNodeInputs({"signal": reference}))


def test_motion_participant_uses_direct_target_base_frame_and_tcp():
    from manyforge_behavior.production_ports import _TargetBinding, program_motion_participant

    target = {"component_id": "robot", "command_target_id": "arm"}
    base = {"component_id": "robot", "frame_id": "base"}
    tcp = {"kind": "entry", "target": target, "frame": {"component_id": "tool", "frame_id": "TCP"}}
    bound = _TargetBinding("robot", "arm", {"execution.trajectory.execute": {}})
    params = {"target": target, "base_frame": base, "tcp_endpoint": tcp,
              "goal": [{"joint_name": "axis", "position_rad": 0.0}],
              "joint_position_tol_rad": 0.1, "translation_tol_m": 0.1, "rotation_tol_rad": 0.1,
              "velocity_scale": 0.5, "acceleration_scale": 0.5,
              "planning_timeout_s": 1, "execution_timeout_s": 2, "cache_mode": "off"}
    result = program_motion_participant("motion.move_to_joint_target", params,
        targets={("robot", "arm"): bound}, contact_grasps={}, move_id="move",
        binding={"frame_bindings": [{"reference": base}]})
    assert result.target.reference() == target
    assert result.payload["base_frame"] == base
    assert result.payload["tcp_endpoint"] == tcp


def test_io_watch_registers_exact_signal_reference():
    from threading import RLock
    from types import SimpleNamespace
    from manyforge_behavior.orchestration_ports import NodeAttemptIdentity, OwnerNodeInputs
    from manyforge_behavior.production_ports import _NativeIoWatchTaskPort, _SignalBinding

    reference = {"component_id": "io", "signal_name": "CI0"}
    binding = _SignalBinding("io", "CI0", {"component_id": "io", "command_target_id": "bank"},
                             "bool", "input", "sampled_only", "sha256:" + "0" * 64)
    context = SimpleNamespace(signals={("io", "CI0"): binding})
    requests = []
    def register(request):
        requests.append(request)
        return {"registration": {}, "result": {"state": "released"}}
    owner = SimpleNamespace(_context=lambda run: context, _require_scope_open=lambda run: None,
        _node_attribution=lambda identity: {}, _lock=RLock(), _io_watches={},
        _kernel=SimpleNamespace(register_io_watch=register))
    port = _NativeIoWatchTaskPort(owner)
    port.start("io.wait_for_input", NodeAttemptIdentity("run", "node", "attempt"),
               OwnerNodeInputs({"signal": reference, "expected_value": True}))
    assert requests[0]["signal"] == reference
    assert requests[0]["condition"] == {"kind": "level", "expected_value": True}


def test_pneumatic_plan_preserves_signal_pairs_and_output_order():
    from types import SimpleNamespace
    from manyforge_behavior.orchestration_ports import NodeAttemptIdentity, OwnerNodeInputs
    from manyforge_behavior.production_ports import _NativeKernelOperationTaskPort, _SignalBinding

    def signal(name, direction):
        return _SignalBinding("io", name, {"component_id": "io", "command_target_id": "bank"},
                              "bool", direction, "sampled_only", "sha256:" + "0" * 64)
    a = {"component_id": "io", "signal_name": "A"}
    b = {"component_id": "io", "signal_name": "B"}
    part = {"component_id": "io", "signal_name": "Part"}
    route = {"outputs": [{"signal": a, "open_value": False, "close_value": True},
                         {"signal": b, "open_value": True, "close_value": False}],
             "inputs": [{"input": "workpiece_grasped", "signal": part}]}
    target = SimpleNamespace(pneumatic_io=route)
    context = SimpleNamespace(signals={("io", "A"): signal("A", "output"),
        ("io", "B"): signal("B", "output"), ("io", "Part"): signal("Part", "input")})
    descriptor = {"min_position": 0.0, "max_position": 0.5, "command_duration_s": 1.0}
    def preset(_descriptor, selector):
        return {"kind": "linear_m", "value_m": 0.5 if selector["name"] == "open" else 0.0}
    owner = SimpleNamespace(_context=lambda _: context, _require_scope_open=lambda _: None,
        _target=lambda *_: target, _owner_status=lambda _: "epoch",
        _gripper_descriptor=lambda *_, **__: descriptor, _named_gripper_position=preset)
    plan = _NativeKernelOperationTaskPort(owner).pneumatic_plan(
        NodeAttemptIdentity("run", "node", "attempt"), OwnerNodeInputs({
            "target": {"component_id": "tool", "command_target_id": "gripper"},
            "position": {"kind": "named_preset", "name": "closed"},
            "required_inputs": [{"input": "workpiece_grasped", "value": True}]}))
    assert plan["outputs"] == [{"signal": b, "value": False}, {"signal": a, "value": True}]
    assert plan["inputs"] == [{"signal": part, "expected_value": True}]


def test_signal_refusal_message_includes_exact_component_and_signal():
    from manyforge_behavior.orchestration_ports import describe_owner_failure

    message = describe_owner_failure("unavailable", {"reason": "io_signal_unbound",
        "reference_type": "signal", "reference": {"component_id": "io", "signal_name": "CI0"}})
    assert "io_signal_unbound" in message
    assert "component_id='io'" in message and "signal_name='CI0'" in message
    long_name = "s" * 512
    message = describe_owner_failure("unavailable", {"reference_type": "signal",
        "reference": {"component_id": "io", "signal_name": long_name}})
    assert f"signal_name='{long_name}'" in message
