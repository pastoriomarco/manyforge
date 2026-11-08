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
        "frames": [{"reference": {"component_id": "arm", "frame_id": "ambiguous"},
                    "match_count": 2}],
    }

def test_persistent_write_preserves_the_runtime_frame_reason() -> None:
    from manyforge_behavior.program import BlackboardKeyDeclaration, Program
    from manyforge_program_runner.runtime import ProgramRunnerError, _validate_persistent_value

    declaration = BlackboardKeyDeclaration("pose", "stored_pose", "read_write", "persistent", {})
    program = object.__new__(Program)
    program._data = {"api_version": "manyforge.program.v3", "scene": []}
    program.blackboard_keys = {"pose": declaration}
    program.parameters = {}
    captured = program.with_program_frame_facts(ProgramFrameFacts.from_capture(deepcopy(facts()), IDENTITY))
    value = {"frame_ref": {"kind": "target_frame", "frame": {
                 "component_id": "arm", "frame_id": "ambiguous"}},
             "position_m": [0.0, 0.0, 0.0],
             "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}
    with pytest.raises(ProgramRunnerError) as error:
        _validate_persistent_value(captured, declaration, value)
    assert error.value.code == "persistent_key_value_invalid"
    assert error.value.details == {"reason": "reference_ambiguous", "reference_type": "component_frame",
                                   "reference_id": "ambiguous", "component_id": "arm"}

