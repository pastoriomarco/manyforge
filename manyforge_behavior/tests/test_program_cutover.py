"""Active Program cutover fence and pure tree-artifact coverage."""

from collections.abc import Iterator, Mapping
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import (
    BlackboardKey,
    Program,
    ProgramArtifactUnavailableError,
    ProgramValidationError,
    TreeNode,
    apply_tree_param_overrides,
    compute_program_tree_hash,
    find_tree_node_by_name,
    merge_param_patch,
    tree_node_from_dict,
)


class _PoisonMapping(Mapping[str, Any]):
    """A Mapping whose protocol must never be entered by the bounded API."""

    def __getitem__(self, key: str) -> Any:
        raise AssertionError(f"unsafe mapping lookup: {key}")

    def __iter__(self) -> Iterator[str]:
        raise AssertionError("unsafe mapping iteration")

    def __len__(self) -> int:
        raise AssertionError("unsafe mapping length")


class _PoisonKey:
    def __hash__(self) -> int:
        return 7

    def __str__(self) -> str:
        raise AssertionError("unsafe field-name stringification")


def test_program_construction_is_fenced_before_artifact_io(tmp_path: Path) -> None:
    environment = build_node_environment()
    missing = tmp_path / "not-read.program.yaml"

    with pytest.raises(
        ProgramArtifactUnavailableError,
        match="composable_cell_program_artifact_unavailable",
    ):
        Program(missing, environment.catalog)


def test_program_mapping_construction_is_fenced() -> None:
    environment = build_node_environment()

    with pytest.raises(
        ProgramArtifactUnavailableError,
        match="composable_cell_program_artifact_unavailable",
    ):
        Program.from_dict({}, environment.catalog)


def test_pure_tree_helpers_remain_available_during_the_port() -> None:
    tree = tree_node_from_dict(
        {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "motion.move_to_joint_target",
                    "name": "move_home",
                    "params": {"target": [0.0, 0.0]},
                }
            ],
        }
    )
    original_hash = compute_program_tree_hash(tree)

    updated = apply_tree_param_overrides(
        tree,
        {"move_home": {"target": [0.5, -0.5]}},
    )

    assert isinstance(updated, TreeNode)
    assert updated.children[0].params["target"] == [0.5, -0.5]
    assert tree.children[0].params["target"] == [0.0, 0.0]
    assert find_tree_node_by_name(updated, "move_home") is updated.children[0]
    assert compute_program_tree_hash(updated) != original_hash


def test_public_value_construction_is_detached_and_exact_json_only() -> None:
    source = {"target": [0.0, 1.0], "tuple_alias": (True, None)}
    tree = TreeNode(id="leaf", name="move", params=source)

    source["target"][0] = 99.0
    assert tree.params == {
        "target": [0.0, 1.0],
        "tuple_alias": [True, None],
    }
    serialized = tree.to_dict()
    assert serialized["params"] == tree.params
    serialized["params"]["target"][0] = -1.0
    assert tree.params["target"] == [0.0, 1.0]

    with pytest.raises(ProgramValidationError, match="built-in mapping"):
        TreeNode(
            id="leaf",
            name="move",
            params=_PoisonMapping(),  # type: ignore[arg-type]
        )
    with pytest.raises(ProgramValidationError, match="built-in mapping"):
        tree_node_from_dict(_PoisonMapping())
    with pytest.raises(ProgramValidationError, match="built-in mapping"):
        merge_param_patch({}, _PoisonMapping())
    with pytest.raises(ProgramValidationError, match="built-in mapping"):
        apply_tree_param_overrides(tree, _PoisonMapping())


def test_mapping_keys_are_rejected_without_stringification() -> None:
    document: dict[Any, Any] = {"id": "root", _PoisonKey(): None}

    with pytest.raises(ProgramValidationError, match="non-string field name"):
        tree_node_from_dict(document)  # type: ignore[arg-type]

    with pytest.raises(ProgramValidationError, match="non-string mapping key"):
        TreeNode(
            id="leaf",
            name="leaf",
            params={_PoisonKey(): "value"},  # type: ignore[dict-item]
        )

    tree = TreeNode(id="root", name="root")
    with pytest.raises(ProgramValidationError, match="non-empty string"):
        apply_tree_param_overrides(
            tree,
            {_PoisonKey(): {}},  # type: ignore[dict-item]
        )


def test_public_tree_traversals_reject_mutated_poison_mapping() -> None:
    tree = TreeNode(id="root", name="root")
    tree.params = _PoisonMapping()  # type: ignore[assignment]

    traversals = (
        tree.to_dict,
        lambda: compute_program_tree_hash(tree),
        lambda: find_tree_node_by_name(tree, "root"),
        lambda: apply_tree_param_overrides(tree, {}),
    )
    for traversal in traversals:
        with pytest.raises(ProgramValidationError, match="built-in mapping"):
            traversal()


def test_tree_and_parameter_cycles_fail_closed_on_every_traversal() -> None:
    cyclic_params: dict[str, Any] = {}
    cyclic_params["self"] = cyclic_params
    with pytest.raises(ProgramValidationError, match="container cycle"):
        TreeNode(id="leaf", name="leaf", params=cyclic_params)
    with pytest.raises(ProgramValidationError, match="container cycle"):
        merge_param_patch({}, cyclic_params)

    cyclic_document: dict[str, Any] = {"id": "root"}
    cyclic_document["children"] = [cyclic_document]
    with pytest.raises(ProgramValidationError, match="tree cycle"):
        tree_node_from_dict(cyclic_document)

    tree = TreeNode(id="root", name="root")
    tree.children.append(tree)
    traversals = (
        tree.to_dict,
        lambda: compute_program_tree_hash(tree),
        lambda: find_tree_node_by_name(tree, "root"),
        lambda: apply_tree_param_overrides(tree, {}),
    )
    for traversal in traversals:
        with pytest.raises(ProgramValidationError, match="tree cycle"):
            traversal()


@pytest.mark.parametrize("unsafe", [float("nan"), float("inf"), 1 << 257])
def test_tree_values_reject_nonfinite_or_oversized_numbers(unsafe: object) -> None:
    with pytest.raises(ProgramValidationError):
        TreeNode(id="leaf", name="leaf", params={"unsafe": unsafe})


def test_tree_values_and_hash_revalidate_post_construction_mutation() -> None:
    nested: dict[str, Any] = {}
    cursor = nested
    for _ in range(34):
        child: dict[str, Any] = {}
        cursor["next"] = child
        cursor = child
    with pytest.raises(ProgramValidationError, match="value depth"):
        TreeNode(id="leaf", name="leaf", params=nested)

    tree = TreeNode(id="leaf", name="leaf", params={"value": 1})
    tree.params["value"] = float("-inf")
    with pytest.raises(ProgramValidationError, match="finite"):
        tree.to_dict()
    with pytest.raises(ProgramValidationError, match="finite"):
        compute_program_tree_hash(tree)


def test_patch_merge_is_detached_and_preserves_discriminator_replacement() -> None:
    base = {"nested": {"keep": [1]}, "typed": {"type": "old", "value": 1}}
    patch = {"nested": {"add": 2}, "typed": {"type": "new", "value": 3}}

    merged = merge_param_patch(base, patch)
    base["nested"]["keep"].append(99)
    patch["typed"]["value"] = 4

    assert merged == {
        "nested": {"keep": [1], "add": 2},
        "typed": {"type": "new", "value": 3},
    }


def test_blackboard_key_construction_is_bounded() -> None:
    assert BlackboardKey(id="key-id", type="string", key="name").key == "name"
    with pytest.raises(ProgramValidationError, match="text bound"):
        BlackboardKey(id="x" * 257, type="string", key="name")
