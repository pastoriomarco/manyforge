"""Captured selection semantics; no effect owner or runtime group latch."""

from copy import deepcopy
from pathlib import Path

import pytest

import manyforge_behavior.program as program_module
from manyforge_behavior.canonical_artifact import (
    compute_content_identity,
    load_strict_json_yaml,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    TreeNode,
    apply_tree_param_overrides,
    tree_node_from_dict,
)


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def catalog() -> NodeCatalog:
    return NodeCatalog(ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def identified(document: dict) -> dict:
    result = deepcopy(document)
    result["program_revision"] = compute_content_identity(
        result,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version=result["api_version"],
    )
    return result


def leaf(name: str, groups="inherit") -> dict:
    result = {
        "node_id": "timer",
        "instance_id": name,
        "parameters": {"duration_s": 0.1},
        "children": [],
    }
    if groups != "inherit":
        result["execution_groups"] = groups
    return result


def document(*children: dict, groups="inherit") -> dict:
    result = load_strict_json_yaml(
        ROOT / "examples/pick_and_place_minimal.program.yaml"
    )
    result["api_version"] = "manyforge.program.v3"
    result["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": list(children) or [leaf("wait")],
    }
    if groups != "inherit":
        result["tree"]["execution_groups"] = groups
    names: set[str] = set()
    pending = [result["tree"]]
    while pending:
        node = pending.pop()
        selected = node.get("execution_groups")
        if isinstance(selected, list):
            names.update(name for name in selected if isinstance(name, str))
        pending.extend(node["children"])
    if names:
        result["execution_groups"] = {
            name: {"execution_policy": "independent"} for name in sorted(names)
        }
    return identified(result)


def motion_leaf(name: str, group: str) -> dict:
    result = deepcopy(
        load_strict_json_yaml(
            ROOT / "examples/pick_and_place_minimal.program.yaml"
        )["tree"]["children"][0]
    )
    result["instance_id"] = name
    result["execution_groups"] = [group]
    return result


def test_default_all_and_overlapping_inherited_selection(catalog: NodeCatalog) -> None:
    a = {
        "node_id": "sequence",
        "instance_id": "branch_a",
        "parameters": {},
        "execution_groups": ["robot_a"],
        "children": [leaf("arm_a"), leaf("gripper_a", ["all_grippers", "robot_a"])],
    }
    b = {
        "node_id": "sequence",
        "instance_id": "branch_b",
        "parameters": {},
        "execution_groups": ["robot_b"],
        "children": [leaf("arm_b"), leaf("gripper_b", ["all_grippers", "robot_b"])],
    }
    source = document(a, b)
    program = Program.from_dict(source, catalog)
    assert program.execution_selections == {
        "all": (
            "arm_a",
            "arm_b",
            "branch_a",
            "branch_b",
            "gripper_a",
            "gripper_b",
            "root",
        ),
        "all_grippers": ("gripper_a", "gripper_b"),
        "robot_a": ("arm_a", "branch_a", "gripper_a"),
        "robot_b": ("arm_b", "branch_b", "gripper_b"),
    }
    source["tree"]["children"][0]["execution_groups"] = ["changed_draft"]
    assert "changed_draft" not in program.execution_selections
    with pytest.raises(TypeError):
        program.execution_selections["robot_a"] = ()
    with pytest.raises(KeyError):
        _ = program.execution_selections["unknown"]


def test_all_clears_named_inheritance_without_joining_every_group(
    catalog: NodeCatalog,
) -> None:
    program = Program.from_dict(
        document(leaf("inherited"), leaf("only_all", "all"), groups=["robot_a"]),
        catalog,
    )
    assert program.execution_selections == {
        "all": ("inherited", "only_all", "root"),
        "robot_a": ("inherited", "root"),
    }
    assert Program.from_dict(document(), catalog).execution_selections == {
        "all": ("root", "wait")
    }


def test_explicit_common_start_group_is_one_parallel_motion_set(
    catalog: NodeCatalog,
) -> None:
    source = document()
    source["tree"] = {
        "node_id": "parallel",
        "instance_id": "paired_motion",
        "parameters": {"success_policy": "all"},
        "children": [motion_leaf("arm_a", "cell_pair"), motion_leaf("arm_b", "cell_pair")],
    }
    source["execution_groups"] = {
        "cell_pair": {"execution_policy": "common_start"}
    }
    program = Program.from_dict(identified(source), catalog)

    assert program.execution_group_policies == {"cell_pair": "common_start"}
    assert program.coordinated_execution_selections == {
        "cell_pair": ("arm_a", "arm_b")
    }


@pytest.mark.parametrize("mutation", ["inherited", "not_parallel", "non_motion", "overlap"])
def test_coordinated_groups_reject_ambiguous_or_non_motion_sets(
    catalog: NodeCatalog, mutation: str
) -> None:
    first = motion_leaf("arm_a", "cell_pair")
    second = motion_leaf("arm_b", "cell_pair")
    source = document()
    source["tree"] = {
        "node_id": "parallel",
        "instance_id": "paired_motion",
        "parameters": {"success_policy": "all"},
        "children": [first, second],
    }
    source["execution_groups"] = {
        "cell_pair": {"execution_policy": "common_start"}
    }
    if mutation == "inherited":
        source["tree"]["execution_groups"] = ["cell_pair"]
        first.pop("execution_groups")
        second.pop("execution_groups")
    elif mutation == "not_parallel":
        source["tree"]["node_id"] = "sequence"
        source["tree"]["parameters"] = {}
    elif mutation == "non_motion":
        second.clear()
        second.update(leaf("wait", ["cell_pair"]))
    else:
        first["execution_groups"] = ["cell_pair", "other_pair"]
        second["execution_groups"] = ["cell_pair", "other_pair"]
        source["execution_groups"] = {
            "cell_pair": {"execution_policy": "common_start"},
            "other_pair": {"execution_policy": "common_start"},
        }

    with pytest.raises(ProgramValidationError, match="coordinated|execution_groups"):
        Program.from_dict(identified(source), catalog)


@pytest.mark.parametrize(
    "groups",
    [
        None,
        True,
        "robot_a",
        [],
        ["all"],
        ["inherit"],
        ["a", "a"],
        ["NotPortable"],
        [["nested"]],
        ["a"] * 4097,
    ],
)
def test_invalid_membership_rejects_before_capture(
    catalog: NodeCatalog, groups
) -> None:
    with pytest.raises(ProgramValidationError, match="execution_groups"):
        Program.from_dict(document(leaf("wait"), groups=groups), catalog)


def test_group_set_order_normalizes_before_identity_validation(
    catalog: NodeCatalog,
) -> None:
    canonical = document(leaf("wait"), groups=["a", "z"])
    source = deepcopy(canonical)
    source["tree"]["execution_groups"].reverse()
    assert Program.from_dict(source, catalog).to_dict() == canonical
    explicit_inherit = document()
    explicit_inherit["tree"]["execution_groups"] = "inherit"
    assert Program.from_dict(explicit_inherit, catalog).to_dict() == document()


def test_capture_checks_inheritance_expansion_and_total_names(
    catalog, monkeypatch
) -> None:
    monkeypatch.setattr(program_module, "_MAX_RESOLVED_GROUP_MEMBERSHIPS", 3)
    with pytest.raises(ProgramValidationError, match="memberships exceed"):
        Program.from_dict(
            document(leaf("a"), leaf("b"), groups=["one", "two"]), catalog
        )
    monkeypatch.setattr(program_module, "_MAX_RESOLVED_GROUP_MEMBERSHIPS", 100)
    monkeypatch.setattr(program_module, "_MAX_EXECUTION_GROUPS", 2)
    with pytest.raises(ProgramValidationError, match="groups exceed"):
        Program.from_dict(
            document(leaf("a", ["one"]), leaf("b", ["two"]), groups=["three"]), catalog
        )


def test_tree_edit_and_override_helpers_preserve_detached_membership() -> None:
    names = ["z", "a"]
    tree = TreeNode(
        id="timer", name="wait", params={"duration_s": 0.1}, execution_groups=names
    )
    names.clear()
    assert tree.to_dict()["execution_groups"] == ["a", "z"]
    parsed = tree_node_from_dict(tree.to_dict())
    assert parsed.execution_groups == ("a", "z")
    changed = apply_tree_param_overrides(parsed, {"wait": {"duration_s": 0.2}})
    assert changed.execution_groups == ("a", "z")


def test_target_effect_budget_does_not_reduce_logical_node_capacity(
    catalog, monkeypatch
) -> None:
    monkeypatch.setattr(program_module, "_MAX_RESOLVED_TARGET_EFFECTS", 1)
    assert (
        len(
            Program.from_dict(
                document(leaf("a"), leaf("b")), catalog
            ).execution_selections["all"]
        )
        == 3
    )
    with pytest.raises(ProgramValidationError, match="target effects exceed"):
        Program(ROOT / "examples/pick_and_place_minimal.program.yaml", catalog)


def test_viewer_hidden_execution_groups_name_declared_groups_only(
    catalog: NodeCatalog,
) -> None:
    """Motion panel presentation: a canonical list of declared groups or all."""
    source = document(leaf("arm_a", ["robot_a"]), leaf("arm_b", ["robot_b"]))
    source["viewer"] = {"hidden_execution_groups": ["all", "robot_b"]}
    program = Program.from_dict(identified(source), catalog)
    assert program.to_dict()["viewer"]["hidden_execution_groups"] == ["all", "robot_b"]

    for hidden, message in (
        (["robot_c"], "does not name a declared execution group"),
        (["robot_b", "robot_a"], "canonical-sorted"),
        (["robot_a", "robot_a"], "duplicate-free"),
    ):
        invalid = deepcopy(source)
        invalid["viewer"] = {"hidden_execution_groups": hidden}
        with pytest.raises(ProgramValidationError, match=message):
            Program.from_dict(identified(invalid), catalog)
