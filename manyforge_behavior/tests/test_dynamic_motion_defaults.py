"""Dynamic motion-default values (decision Q1.2-b, 2026-11-10).

A velocity or acceleration scale may come from any typed value source. The
Program check accepts it; when the move resolves its inputs, a value out of
range or above the Program parameter of the same name refuses the move before
any motion, with a typed reason. It is never clamped.
"""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import (
    MOTION_VALUE_ABOVE_PROGRAM_LIMIT,
    NODE_INPUT_UNAVAILABLE,
    MotionLimitInputError,
    NodeAttemptInputResolver,
    NodeInputError,
    ProgramValueStore,
    input_failure_reason,
)
from manyforge_behavior.program import Program, ProgramValidationError

_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM = _ROOT / "examples" / "lite6_lite_gripper_pick_and_place.program.yaml"
_CATALOG = Path(__file__).resolve().parents[1] / "resources/node_catalog.yaml"
_MOVE = "move_to_pick_approach"


def _program(source: dict, *, literal: float | None = None) -> Program:
    catalog = NodeCatalog(_CATALOG)
    document = Program(_PROGRAM, catalog).to_dict()
    document["blackboard"] = {
        "namespaces": [{"namespace_id": "cell", "owner": "program"}],
        "keys": [{
            "key": "cell.speed", "type": "float64", "access": "read_write", "persistence": "run",
        }],
    }
    move = next(node for node in document["tree"]["children"] if node["instance_id"] == _MOVE)
    move["value_sources"]["velocity_scale"] = deepcopy(source)
    if literal is not None:
        move["value_sources"].pop("velocity_scale")
        move["parameters"]["velocity_scale"] = literal
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return Program.from_dict(document, catalog)


def _snapshot(program: Program, speed: float):
    store = ProgramValueStore(program)
    store.write("cell.speed", speed)
    resolver = NodeAttemptInputResolver(
        program=program, catalog=NodeCatalog(_CATALOG), value_store=store,
    )
    move = next(node for node in program.to_dict()["tree"]["children"]
                if node["instance_id"] == _MOVE)
    return resolver.snapshot(
        node_id=move["node_id"],
        node_instance_id=_MOVE,
        parameters=move["parameters"],
        value_sources=move["value_sources"],
    )


_KEY_SOURCE = {"kind": "blackboard_key", "reference": "cell.speed"}


def test_a_dynamic_speed_source_passes_the_program_check() -> None:
    _program(_KEY_SOURCE)
    with pytest.raises(ProgramValidationError, match="must not exceed Program velocity_scale"):
        _program(_KEY_SOURCE, literal=0.9)


def test_a_valid_dynamic_speed_resolves_for_the_move() -> None:
    snapshot = _snapshot(_program(_KEY_SOURCE), 0.25)
    assert snapshot.parameters["velocity_scale"] == 0.25


def test_a_dynamic_speed_above_the_program_ceiling_refuses_the_move() -> None:
    with pytest.raises(MotionLimitInputError) as refused:
        _snapshot(_program(_KEY_SOURCE), 0.9)
    assert input_failure_reason(refused.value) == MOTION_VALUE_ABOVE_PROGRAM_LIMIT
    assert refused.value.details["path"] == "velocity_scale"
    assert refused.value.details["source_kind"] == "blackboard_key"
    assert refused.value.details["source_reference"] == "cell.speed"
    assert "exceeds Program velocity_scale=0.4" in str(refused.value)


@pytest.mark.parametrize("speed", [0.0, -0.1, 1.5])
def test_a_dynamic_speed_out_of_range_refuses_the_move(speed: float) -> None:
    with pytest.raises(NodeInputError) as refused:
        _snapshot(_program(_KEY_SOURCE), speed)
    assert not isinstance(refused.value, MotionLimitInputError)
    assert input_failure_reason(refused.value) == NODE_INPUT_UNAVAILABLE
    assert refused.value.details["input_problem"] == "invalid"
    assert refused.value.details["path"] == "velocity_scale"
