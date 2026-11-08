"""Exact v3 component-reference joins for observed manual targets."""

from copy import deepcopy
from types import SimpleNamespace

import pytest

from manyforge_composer.backend import routes_draft
from manyforge_composer.backend.draft_session import DraftSessionError


def _fixture(monkeypatch, *, kind="tcp_pose", selector=None):
    target = {"component_id": "robot", "command_target_id": "arm"}
    base = {"component_id": "robot", "frame_id": "base"}
    tcp = {"component_id": "tool", "frame_id": "tcp"}
    tool_target = {"component_id": "tool", "command_target_id": "gripper"}
    entry = {"target": tool_target, "frame": tcp}
    base_row = {"reference": base, "compiled_frame_name": "robot.base",
                "model_instance_id": "robot-model", "model_revision": "model-r1"}
    tcp_row = {"reference": tcp, "compiled_frame_name": "tool.tcp",
               "model_instance_id": "tool-model", "model_revision": "model-r1"}
    binding = {
        "graph_revision": "graph-1", "retained_bundle_id": "bundle-1",
        "frame_bindings": [base_row, tcp_row],
        "target_bindings": [
            {"target": target, "frame_references": [base], "robot_tcps": [
                {"entry": entry, "frame": deepcopy(tcp_row)}]},
            {"target": tool_target, "frame_references": [tcp], "default_tcp": entry},
        ],
    }
    monkeypatch.setattr(routes_draft, "_applied_context", lambda _state: (
        SimpleNamespace(binding_document=binding, program_revision=None), None))
    identity = {"owner_epoch": "owner-1", "graph_revision": "graph-1", "retained_bundle_id": "bundle-1"}
    descriptor = {"frames": [deepcopy(base_row)], "tcps": [
        {"entry": entry, "frame": deepcopy(tcp_row), "mounted_here": True}]}
    targets = {"ok": True, "identity": identity, "targets": {"components": [{
        "controllable_component_id": "robot", "targets": [{
            "command_target_id": "arm", "operation_capability_id": "execution.trajectory.execute",
            "presentation": "motion", "command": {"base_frame": base, "target_descriptor": descriptor},
        }]}]}}
    pose = {"position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}}
    observed = {"kind": "joint_positions", "complete": True, "source_epoch": "epoch-1", "sequence": 3,
                "source_joint_names": ["joint1"], "positions_rad": [0.2],
                "tcp_poses": [{"entry": entry, "pose": pose}]}
    control = {"ok": True, "identity": identity, "control_state": {"observed_targets": [{
        "controllable_component_id": "robot", "command_target_id": "arm",
        "operation_capability_id": "execution.trajectory.execute",
        "active_tcp": {"entry": entry}, "observed": observed,
    }]}}
    node = {"instance_id": "move", "node_id": "motion.move_to_pose" if kind == "tcp_pose"
            else "motion.move_to_joint_target", "parameters": {"target": target, "base_frame": base,
            "tcp_endpoint": selector or {"kind": "current"}}}
    state = SimpleNamespace(headless_client=SimpleNamespace(execution_targets=lambda: targets,
                            execution_control_state=lambda: control),
                            draft_session=SimpleNamespace(snapshot=lambda: {"authoring": {"forest": [node]}}))
    return state, binding, targets, control, node, entry


@pytest.mark.parametrize("selector", [None,
    {"kind": "default_of_target", "target": {"component_id": "tool", "command_target_id": "gripper"}},
    {"kind": "entry", "target": {"component_id": "tool", "command_target_id": "gripper"},
     "frame": {"component_id": "tool", "frame_id": "tcp"}}])
def test_observed_pose_uses_exact_target_base_and_mounted_tcp(monkeypatch, selector):
    state, _, _, _, _, _ = _fixture(monkeypatch, selector=selector)
    result, source = routes_draft._observed_move_arguments(state, {"instance_id": "move", "observed_kind": "tcp_pose"})
    assert result["observed"]["frame_ref"] == {"kind": "target_frame", "frame": {"component_id": "robot", "frame_id": "base"}}
    assert result["observed"]["current_tcp_pose"]["position_m"]["x_m"] == 0.1
    assert source == {"source_epoch": "epoch-1", "sequence": 3, "controllable_component_id": "robot", "command_target_id": "arm"}


def test_observed_joint_capture_uses_exact_target(monkeypatch):
    state, _, _, _, _, _ = _fixture(monkeypatch, kind="joint")
    result, _ = routes_draft._observed_move_arguments(state, {"instance_id": "move", "observed_kind": "joint"})
    assert result["observed"] == {"source_joint_names": ["joint1"], "positions_rad": [0.2]}


@pytest.mark.parametrize("change", ["target", "base", "tcp_model", "unmounted", "stale"])
def test_observed_capture_refuses_mismatched_or_stale_identity(monkeypatch, change):
    state, binding, targets, control, node, _ = _fixture(monkeypatch)
    if change == "target":
        node["parameters"]["target"] = {"component_id": "other", "command_target_id": "arm"}
    elif change == "base":
        targets["targets"]["components"][0]["targets"][0]["command"]["base_frame"] = {"component_id": "robot", "frame_id": "other"}
    elif change == "tcp_model":
        binding["frame_bindings"][1]["model_revision"] = "model-r2"
        binding["target_bindings"][0]["robot_tcps"][0]["frame"]["model_revision"] = "model-r2"
    elif change == "unmounted":
        targets["targets"]["components"][0]["targets"][0]["command"]["target_descriptor"]["tcps"][0]["mounted_here"] = False
    else:
        control["identity"] = {**control["identity"], "owner_epoch": "old"}
    with pytest.raises(DraftSessionError):
        routes_draft._observed_move_arguments(state, {"instance_id": "move", "observed_kind": "tcp_pose"})


@pytest.mark.parametrize(("change", "code"), [
    ("missing_target", "observed_move_role_missing"),
    ("missing_base", "observed_move_role_missing"),
    ("malformed_selector", "observed_move_role_missing"),
    ("unbound_target", "observed_move_target_unbound"),
    ("unbound_base", "observed_move_target_unbound"),
    ("duplicate_target", "observed_move_target_unavailable"),
    ("changed_configuration", "observed_move_configuration_changed"),
    ("missing_base_observation", "observed_move_frame_mismatch"),
    ("incomplete_observation", "observed_move_unavailable"),
])
def test_observed_capture_refuses_before_mutation(monkeypatch, change, code):
    state, binding, targets, control, node, _ = _fixture(monkeypatch)
    if change == "missing_target":
        node["parameters"].pop("target")
    elif change == "missing_base":
        node["parameters"].pop("base_frame")
    elif change == "malformed_selector":
        node["parameters"]["tcp_endpoint"] = {"kind": "entry", "target": "arm"}
    elif change == "unbound_target":
        binding["target_bindings"].pop(0)
    elif change == "unbound_base":
        binding["target_bindings"][0]["frame_references"].clear()
    elif change == "duplicate_target":
        rows = targets["targets"]["components"][0]["targets"]
        rows.append(deepcopy(rows[0]))
    elif change == "changed_configuration":
        binding["graph_revision"] = "replaced"
    elif change == "missing_base_observation":
        targets["targets"]["components"][0]["targets"][0]["command"]["target_descriptor"]["frames"].clear()
    else:
        control["control_state"]["observed_targets"][0]["observed"]["complete"] = False
    with pytest.raises(DraftSessionError) as refused:
        routes_draft._observed_move_arguments(state, {"instance_id": "move", "observed_kind": "tcp_pose"})
    assert refused.value.code == code
