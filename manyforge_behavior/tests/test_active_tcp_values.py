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


_TARGET = {"component_id": "robot", "command_target_id": "arm"}
_KEY = "active_tcp/robot/arm"


def _entry(frame_id: str = "tcp") -> dict:
    return {
        "target": {"component_id": "tool", "command_target_id": "gripper"},
        "frame": {"component_id": "tool", "frame_id": frame_id},
    }


def _store() -> ProgramValueStore:
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    document = {
        "api_version": "manyforge.program.v3",
        "program_id": "active_tcp_values",
        "program_revision": "sha256:" + "0" * 64,  # TCP migration: placeholder digest shape, recomputed below
        "program_profile": "composable-cell-v1",
        "name": "Active TCP values",
        "catalog": {"api_version": catalog.api_version, "content_version": catalog.content_version},
        "tree": {"node_id": "timer", "instance_id": "timer", "parameters": {"duration_s": 1.0}, "children": []},
    }
    document["program_revision"] = compute_content_identity(
        deepcopy(document), own_field="program_revision",
        identity_domain="program-revision", api_version="manyforge.program.v3",
    )
    program = Program.from_dict(document, catalog).with_active_tcp_binding(
        {"target_bindings": [{"target": _TARGET, "robot_tcps": []}]}
    )
    return ProgramValueStore(program)


def test_active_tcp_is_read_only_copied_and_retained_across_run_reset() -> None:
    store = _store()
    entry = _entry()
    assert store.write_active_tcp("robot", "other", entry) is False
    assert store.write_active_tcp("robot", "arm", entry) is True
    entry["frame"]["frame_id"] = "changed"
    actual = store.read(_KEY)
    assert actual["frame"]["frame_id"] == "tcp"
    actual["frame"]["frame_id"] = "changed_again"
    with pytest.raises(ProgramValueError):
        store.write(_KEY, actual)
    store.clear_run_values()
    assert store.read(_KEY)["frame"]["frame_id"] == "tcp"


def test_active_tcp_unavailable_and_invalid_entries() -> None:
    store = _store()
    key = _KEY
    with pytest.raises(KeyError):
        store.read(key)
    with pytest.raises(ProgramValueError):
        store.write_active_tcp("robot", "arm", {"target": _entry()["target"]})
    with pytest.raises(KeyError):
        store.read(key)
    store.write_active_tcp("robot", "arm", _entry())
    assert store.write_active_tcp("robot", "arm", None) is True
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
        resolver.snapshot(**arguments, derived_active_tcp_key=_KEY)
    assert error.value.details["source_reference"] == _KEY
    assert error.value.details["input_problem"] == "missing"
    assert "active_tcp" not in resolver.snapshot(**arguments).blackboard

    first = _entry()
    store.write_active_tcp("robot", "arm", first)
    snapshot = resolver.snapshot(**arguments, derived_active_tcp_key=_KEY)
    store.write_active_tcp("robot", "arm", _entry("tip"))
    assert snapshot.blackboard["active_tcp"] == first


def test_set_tcp_current_captures_the_bound_target_value() -> None:
    store = _store()
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(program=store.program, catalog=catalog, value_store=store)
    parameters = {"target": dict(_TARGET), "tcp": {"kind": "current"}}
    key = _derived_active_tcp_key("motion.set_tcp", parameters)
    arguments = {"node_id": "motion.set_tcp", "node_instance_id": "choose_tcp",
                 "parameters": parameters, "value_sources": {}, "derived_active_tcp_key": key}
    with pytest.raises(NodeInputError) as error:
        resolver.snapshot(**arguments)
    assert error.value.details["source_reference"] == _KEY
    entry = _entry()
    store.write_active_tcp("robot", "arm", entry)
    snapshot = resolver.snapshot(**arguments)
    store.write_active_tcp("robot", "arm", _entry("tip"))
    assert snapshot.blackboard["active_tcp"] == entry


def test_set_tcp_accepts_a_tcp_entry_from_a_blackboard_key() -> None:
    store = _store()
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(program=store.program, catalog=catalog, value_store=store)
    entry = _entry()
    store.write_active_tcp("robot", "arm", entry)
    snapshot = resolver.snapshot(
        node_id="motion.set_tcp", node_instance_id="choose_tcp",
        parameters={"target": dict(_TARGET)},
        value_sources={"tcp": {"kind": "blackboard_key", "reference": _KEY}},
    )
    assert snapshot.parameters["tcp"] == entry
