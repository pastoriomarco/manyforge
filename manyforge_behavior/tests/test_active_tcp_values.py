"""The Program value store's system-owned Active TCP key."""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.executable_node_resolver import _derived_active_tcp_key
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver, NodeInputError, ProgramValueError, ProgramValueStore,
)
from manyforge_behavior.program import Program


def _store() -> ProgramValueStore:
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    document = {
        "api_version": "manyforge.program.v2",
        "program_id": "active_tcp_values",
        "program_revision": "sha256:" + "0" * 64,  # TCP migration: placeholder digest shape, recomputed below
        "program_profile": "composable-cell-v1",
        "name": "Active TCP values",
        "catalog": {"api_version": catalog.api_version, "content_version": catalog.content_version},
        "component_roles": {"robot": {}},
        "target_roles": {"arm": {
            "component_role": "robot",
            "required_capabilities": [{
                "capability_id": "execution.trajectory.execute",
                "parameters": {}, "disposition": "required",
            }],
        }},
        "tree": {"node_id": "timer", "instance_id": "timer", "parameters": {"duration_s": 1.0}, "children": []},
    }
    document["program_revision"] = compute_content_identity(
        deepcopy(document), own_field="program_revision",
        identity_domain="program-revision", api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, catalog).with_active_tcp_binding(
        {"target_role_bindings": [{"target_role_id": "arm", "robot_tcps": []}]}
    )
    return ProgramValueStore(program)


def test_active_tcp_is_read_only_copied_and_retained_across_run_reset() -> None:
    store = _store()
    entry = {"component_id": "tool.1", "target_id": "gripper", "frame_role_id": "tcp"}
    assert store.write_active_tcp("other", entry) is False
    assert store.write_active_tcp("arm", entry) is True
    entry["frame_role_id"] = "changed"
    actual = store.read("active_tcp.arm")
    assert actual["frame_role_id"] == "tcp"
    actual["frame_role_id"] = "changed_again"
    with pytest.raises(ProgramValueError):
        store.write("active_tcp.arm", actual)
    store.clear_run_values()
    assert store.read("active_tcp.arm")["frame_role_id"] == "tcp"


def test_active_tcp_unavailable_and_invalid_entries() -> None:
    store = _store()
    key = "active_tcp.arm"
    with pytest.raises(KeyError):
        store.read(key)
    with pytest.raises(ProgramValueError):
        store.write_active_tcp("arm", {"component_id": "tool", "target_id": "gripper"})
    with pytest.raises(KeyError):
        store.read(key)
    store.write_active_tcp("arm", {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"})
    assert store.write_active_tcp("arm", None) is True
    with pytest.raises(KeyError):
        store.read(key)


def test_derived_current_read_is_in_the_attempt_snapshot_and_names_missing_key() -> None:
    store = _store()
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(program=store.program, catalog=catalog, value_store=store)
    arguments = {
        "node_id": "timer", "node_instance_id": "timer",
        "parameters": {"duration_s": 1.0}, "value_sources": {},
    }
    with pytest.raises(NodeInputError) as error:
        resolver.snapshot(**arguments, derived_active_tcp_key="active_tcp.arm")
    assert error.value.details["source_reference"] == "active_tcp.arm"
    assert error.value.details["input_problem"] == "missing"
    assert "active_tcp" not in resolver.snapshot(**arguments).blackboard

    first = {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}
    store.write_active_tcp("arm", first)
    snapshot = resolver.snapshot(**arguments, derived_active_tcp_key="active_tcp.arm")
    store.write_active_tcp("arm", {**first, "frame_role_id": "tip"})
    assert snapshot.blackboard["active_tcp"] == first


def test_set_tcp_current_captures_the_bound_role_value() -> None:
    store = _store()
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(program=store.program, catalog=catalog, value_store=store)
    parameters = {"target_role": "arm", "tcp": {"kind": "current"}}
    key = _derived_active_tcp_key("motion.set_tcp", parameters)
    arguments = {"node_id": "motion.set_tcp", "node_instance_id": "choose_tcp",
                 "parameters": parameters, "value_sources": {}, "derived_active_tcp_key": key}
    with pytest.raises(NodeInputError) as error:
        resolver.snapshot(**arguments)
    assert error.value.details["source_reference"] == "active_tcp.arm"
    entry = {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}
    store.write_active_tcp("arm", entry)
    snapshot = resolver.snapshot(**arguments)
    store.write_active_tcp("arm", {**entry, "frame_role_id": "tip"})
    assert snapshot.blackboard["active_tcp"] == entry


def test_set_tcp_accepts_a_tcp_entry_from_a_blackboard_key() -> None:
    store = _store()
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(program=store.program, catalog=catalog, value_store=store)
    entry = {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}
    store.write_active_tcp("arm", entry)
    snapshot = resolver.snapshot(
        node_id="motion.set_tcp", node_instance_id="choose_tcp",
        parameters={"target_role": "arm"},
        value_sources={"tcp": {"kind": "blackboard_key", "reference": "active_tcp.arm"}},
    )
    assert snapshot.parameters["tcp"] == entry
