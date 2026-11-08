from copy import deepcopy

import pytest

from manyforge_behavior.program_frame_facts import ProgramFrameFacts


IDENTITY = {
    "applied_record_digest": "record",
    "program_binding_id": "binding",
    "program_revision": "revision",
    "retained_bundle_id": "bundle",
    "graph_revision": "graph",
}


def facts() -> dict:
    return {
        **IDENTITY,
        "components": ["arm"],
        "frames": [
            {"reference": {"component_id": "arm", "frame_id": "alias"}, "match_count": 1},
            {"reference": {"component_id": "arm", "frame_id": "ambiguous"}, "match_count": 2},
            {"reference": {"component_id": "arm", "frame_id": "model_base"}, "match_count": 1},
        ],
    }


def test_capture_keeps_unique_ambiguous_and_missing_distinct_and_immutable() -> None:
    wire = facts()
    captured = ProgramFrameFacts.from_capture(wire, IDENTITY)
    wire["frames"][0]["match_count"] = 2
    assert captured.match_count("arm", "alias") == 1
    assert captured.match_count("arm", "model_base") == 1
    assert captured.match_count("arm", "ambiguous") == 2
    assert captured.match_count("arm", "absent") == 0
    assert captured.match_count("derived_companion", "alias") == 0
    with pytest.raises(TypeError):
        captured.frames[("arm", "alias")] = 2


@pytest.mark.parametrize("field", list(IDENTITY))
def test_capture_refuses_every_stale_identity(field: str) -> None:
    stale = deepcopy(facts())
    stale[field] = "stale"
    with pytest.raises(ValueError, match="differs from captured Applied identity"):
        ProgramFrameFacts.from_capture(stale, IDENTITY)


def test_capture_refuses_unsorted_or_nonmember_facts() -> None:
    unsorted = facts()
    unsorted["frames"].reverse()
    with pytest.raises(ValueError, match="sorted and unique"):
        ProgramFrameFacts.from_capture(unsorted, IDENTITY)
    foreign = facts()
    foreign["frames"][0]["reference"]["component_id"] = "derived_companion"
    with pytest.raises(ValueError, match="component"):
        ProgramFrameFacts.from_capture(foreign, IDENTITY)


def test_runtime_program_checks_captured_match_and_allowed_frames() -> None:
    from manyforge_behavior.program import (
        BlackboardKeyDeclaration, Program, ProgramFrameReferenceError,
        ProgramValidationError,
    )

    program = object.__new__(Program)
    program._data = {"api_version": "manyforge.program.v3", "scene": []}
    program.blackboard_keys = {
        "pose": BlackboardKeyDeclaration("pose", "stored_pose", "read_write", "run", {}),
    }
    captured = program.with_program_frame_facts(ProgramFrameFacts.from_capture(facts(), IDENTITY))

    def pose(frame_id: str, component_id: str = "arm") -> dict:
        return {"frame_ref": {"kind": "target_frame", "frame": {
                    "component_id": component_id, "frame_id": frame_id}},
                "position_m": [0.0, 0.0, 0.0],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}

    captured.validate_key_value("pose", pose("alias"), path="pose")
    captured.validate_key_value("pose", pose("model_base"), path="pose")
    for frame, component, code in (("absent", "arm", "frame_missing"),
                                    ("ambiguous", "arm", "reference_ambiguous"),
                                    ("alias", "derived", "frame_missing")):
        with pytest.raises(ProgramFrameReferenceError) as error:
            captured.validate_key_value("pose", pose(frame, component), path="pose")
        assert error.value.code == code
        assert error.value.details["component_id"] == component
        assert error.value.details["reference_id"] == frame
    captured.blackboard_keys = {
        "pose": BlackboardKeyDeclaration(
            "pose", "stored_pose", "read_write", "run", {},
            allowed_frames=({"kind": "target_frame", "frame": {
                "component_id": "arm", "frame_id": "other"}},),
        ),
    }
    with pytest.raises(ProgramValidationError, match="allowed_frames"):
        captured.validate_key_value("pose", pose("alias"), path="pose")


def test_run_and_between_run_values_keep_the_same_typed_frame_evidence() -> None:
    from manyforge_behavior.orchestration_values import ProgramValueError, validate_blackboard_value
    from manyforge_behavior.program import BlackboardKeyDeclaration, Program, ProgramFrameReferenceError

    declaration = BlackboardKeyDeclaration("pose", "stored_pose", "read_write", "persistent", {})
    base = object.__new__(Program)
    base._data = {"api_version": "manyforge.program.v3", "scene": []}
    base.blackboard_keys = {"pose": declaration}
    snapshot = ProgramFrameFacts.from_capture(facts(), IDENTITY)
    for captured in (base.with_program_frame_facts(snapshot), base.with_program_frame_facts(snapshot)):
        bad = {"frame_ref": {"kind": "target_frame", "frame": {
                   "component_id": "arm", "frame_id": "ambiguous"}},
               "position_m": [0.0, 0.0, 0.0],
               "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}
        with pytest.raises(ProgramValueError) as error:
            validate_blackboard_value(captured, declaration, bad, label="pose")
        assert isinstance(error.value.__cause__, ProgramFrameReferenceError)
        assert error.value.__cause__.code == "reference_ambiguous"


def test_reset_keeps_existing_removal_semantics_but_dynamic_write_uses_facts() -> None:
    from manyforge_behavior.orchestration_values import ProgramValueError, ProgramValueStore
    from manyforge_behavior.program import BlackboardKeyDeclaration, Program

    def pose(frame_id: str) -> dict:
        return {"frame_ref": {"kind": "target_frame", "frame": {
                    "component_id": "arm", "frame_id": frame_id}},
                "position_m": [0.0, 0.0, 0.0],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}

    declaration = BlackboardKeyDeclaration(
        "pose", "stored_pose", "read_write", "persistent", {},
        has_initial=True, initial=pose("ambiguous"),
    )
    program = object.__new__(Program)
    program._data = {"api_version": "manyforge.program.v3", "scene": []}
    program.blackboard_keys = {"pose": declaration}
    program.parameters = {}
    captured = program.with_program_frame_facts(ProgramFrameFacts.from_capture(facts(), IDENTITY))
    removed = []
    store = ProgramValueStore(
        captured, restored={"pose": pose("alias")},
        persistent_commit=lambda *_: None,
        persistent_forget=lambda declaration, _metadata: removed.append(declaration.key),
    )
    store.reset_to_initial("pose")
    assert removed == ["pose"]
    assert store.read("pose") == pose("ambiguous")
    with pytest.raises(ProgramValueError) as error:
        store.write("pose", pose("ambiguous"))
    assert error.value.__cause__.code == "reference_ambiguous"
    with pytest.raises(ProgramValueError):
        ProgramValueStore(captured, persistent_commit=lambda *_: None)


def test_node_input_failure_keeps_typed_ambiguous_reference() -> None:
    from pathlib import Path
    import manyforge_behavior.program as program_module
    from manyforge_behavior.catalog import NodeCatalog
    from manyforge_behavior.orchestration_values import (
        NodeAttemptInputResolver, NodeInputError, ProgramValueStore,
    )
    from manyforge_behavior.program import BlackboardKeyDeclaration, Program

    declaration = BlackboardKeyDeclaration("pose", "stored_pose", "read_write", "run", {})
    program = object.__new__(Program)
    program._data = {"api_version": "manyforge.program.v3", "scene": []}
    program.blackboard_keys = {"pose": declaration}
    program.parameters = {}
    from types import SimpleNamespace
    program.expressions = SimpleNamespace(computed_keys={}, computed_key_order=())
    captured = program.with_program_frame_facts(ProgramFrameFacts.from_capture(facts(), IDENTITY))
    catalog = NodeCatalog(Path(program_module.__file__).resolve().parent.parent / "resources/node_catalog.yaml")
    resolver = NodeAttemptInputResolver(
        program=captured, catalog=catalog, value_store=ProgramValueStore(captured),
    )
    value = {"frame_ref": {"kind": "target_frame", "frame": {
                 "component_id": "arm", "frame_id": "ambiguous"}},
             "position_m": [0.0, 0.0, 0.0],
             "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}
    with pytest.raises(NodeInputError) as error:
        resolver.snapshot(node_id="logic.set_value", node_instance_id="set",
                          parameters={"key": "pose", "value": value}, value_sources={})
    assert error.value.details["reason"] == "reference_ambiguous"
    assert error.value.details["component_id"] == "arm"
    assert error.value.details["reference_id"] == "ambiguous"
