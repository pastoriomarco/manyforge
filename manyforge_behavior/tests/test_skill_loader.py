"""Portable skill expansion is effect-free and uses normal Program validation."""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program
from manyforge_behavior.skill_loader import (
    SkillCatalog,
    SkillValidationError,
    default_skills_dir,
    load_skill,
    load_skill_catalog,
)

ROOT = Path(__file__).resolve().parents[2]


def _context(catalog):
    host = Program(
        ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml", catalog
    ).to_dict()
    del host["tree"], host["program_revision"]
    return host


@pytest.fixture
def catalog():
    return NodeCatalog(ROOT / "manyforge_behavior/resources/node_catalog.yaml")


@pytest.fixture
def artifact():
    return load_skill(default_skills_dir() / "move_to_named_config.skill.yaml")


def test_build_compiles_every_maintained_example(catalog):
    skills = load_skill_catalog(default_skills_dir(), catalog=catalog)
    assert {row["skill_id"] for row in skills.artifacts()} == {
        "move_to_named_config",
        "pick_object",
        "place_object",
    }


def test_expansion_preserves_roles_typed_values_and_host(catalog):
    skills = load_skill_catalog(default_skills_dir(), catalog=catalog)
    host = _context(catalog)
    original = deepcopy(host)
    arguments = {
        "configuration_name": "rest",
        "goal": [{"joint_name": "joint1", "position_rad": 0.0}],
        "velocity_scale": 0.2,
        "acceleration_scale": 0.3,
        "max_tcp_linear_speed_mps": 0.1,
    }
    request = dict(
        skill_id="move_to_named_config",
        arguments=arguments,
        component_roles={"robot": "robot"},
        target_roles={"arm": "arm"},
        instance_prefix="selected.rest",
        program_context=host,
    )
    result = skills.expand(**request)
    assert result == skills.expand(**request)
    assert result["tree"]["instance_id"] == "selected.rest.move"
    assert result["tree"]["parameters"]["goal"] == arguments["goal"]
    assert result["trace"][0]["target_roles"] == {"arm": "arm"}
    assert result["tree"]["metadata"]["skill_expansions"] == result["trace"]
    assert host == original
    result["tree"]["parameters"]["goal"][0]["position_rad"] = 2.0
    assert arguments["goal"][0]["position_rad"] == 0.0
    with pytest.raises(SkillValidationError, match="mapped skill role"):
        skills.expand(**{**request, "target_roles": {"arm": "gripper"}})


@pytest.mark.parametrize(
    "mutate,reason",
    [
        (lambda row: row.update(api_version="manyforge.skill.v0"), "unsupported skill"),
        (
            lambda row: row["body"].update(node_id="no.such.node"),
            "unknown skill primitive",
        ),
        (
            lambda row: row["body"]["parameters"].update(goal="${param:missing}"),
            "undeclared",
        ),
        (
            lambda row: row["body"]["parameters"].update(goal="prefix ${param:goal}"),
            "whole value",
        ),
        (
            lambda row: row["body"]["parameters"].update(no_such_parameter=True),
            "unknown primitive",
        ),
        (lambda row: row.update(requires_tools=["unregistered"]), "unknown tool"),
        (lambda row: row.update(python_import="os.system"), "unknown fields"),
        (
            lambda row: row["body"].update(metadata={"skill_expansions": []}),
            "provenance",
        ),
        (
            lambda row: row["target_roles"]["arm"].update(component_role="missing"),
            "declared component",
        ),
        (lambda row: row.update(description="x" * 100_000), "text exceeds"),
    ],
)
def test_invalid_artifacts_fail_at_build(catalog, artifact, mutate, reason):
    mutate(artifact)
    with pytest.raises(SkillValidationError, match=reason):
        SkillCatalog([artifact], catalog=catalog)


def test_nested_skill_depth_and_cycle_are_bounded(catalog, artifact):
    wrapper = deepcopy(artifact)
    wrapper["skill_id"] = "wrapper"
    wrapper["body"] = {
        "skill_id": "move_to_named_config",
        "instance_id": "nested",
        "arguments": {row["id"]: "${param:" + row["id"] + "}" for row in artifact["parameters"]},
        "component_roles": {"robot": "${component:robot}"},
        "target_roles": {"arm": "${target:arm}"},
    }
    skills = SkillCatalog([wrapper, artifact], catalog=catalog)
    host = _context(catalog)
    result = skills.expand(
        skill_id="wrapper",
        arguments={
            **artifact["example"]["arguments"],
            "acceleration_scale": 1.0,
        },
        component_roles={"robot": "robot"},
        target_roles={"arm": "arm"},
        instance_prefix="wrapped",
        program_context=host,
    )
    assert (
        result["trace"][0]["instance_ids"]
        == result["trace"][1]["instance_ids"]
        == ["wrapped.nested.move"]
    )
    outer = deepcopy(wrapper)
    outer["skill_id"] = "outer"
    outer["body"]["skill_id"] = "wrapper"
    with pytest.raises(SkillValidationError, match="too-deep"):
        SkillCatalog([outer, wrapper, artifact], catalog=catalog)
    wrapper["body"]["skill_id"] = "wrapper"
    with pytest.raises(SkillValidationError, match="cyclic"):
        SkillCatalog([wrapper, artifact], catalog=catalog)


def test_duplicate_id_and_unexecutable_example_reject(catalog, artifact):
    with pytest.raises(SkillValidationError, match="duplicate"):
        SkillCatalog([artifact, artifact], catalog=catalog)
    artifact["example"]["arguments"]["goal"] = "not joint values"
    with pytest.raises(SkillValidationError, match="argument goal"):
        SkillCatalog([artifact], catalog=catalog)


def test_repeated_structured_argument_is_bounded_before_output_allocation(catalog, artifact):
    artifact["body"]["metadata"] = {f"copy.{index}": "${param:goal}" for index in range(32)}
    skills = SkillCatalog([artifact], catalog=catalog)
    host = _context(catalog)
    arguments = {
        **artifact["example"]["arguments"],
        "goal": [{"joint_name": f"axis{index}", "position_rad": 0.0} for index in range(200)],
    }
    with pytest.raises(SkillValidationError, match="expanded skill exceeds"):
        skills.expand(
            skill_id=artifact["skill_id"],
            arguments=arguments,
            component_roles={"robot": "robot"},
            target_roles={"arm": "arm"},
            instance_prefix="large",
            program_context=host,
        )


def test_first_subtree_needs_declarations_not_a_dummy_program(catalog, artifact):
    skills = SkillCatalog([artifact], catalog=catalog)
    context = {
        "api_version": "manyforge.program.v2",
        "program_id": "new_program",
        "program_profile": "composable-cell-v1",
        "name": "New Program",
        "catalog": {
            "api_version": catalog.api_version,
            "content_version": catalog.content_version,
        },
        "component_roles": artifact["component_roles"],
        "target_roles": artifact["target_roles"],
    }
    result = skills.expand(
        skill_id=artifact["skill_id"],
        arguments=artifact["example"]["arguments"],
        component_roles={"robot": "robot"},
        target_roles={"arm": "arm"},
        instance_prefix="first",
        program_context=context,
    )
    assert result["tree"]["node_id"] == "motion.move_to_joint_target"
    assert "tree" not in context and "program_revision" not in context
