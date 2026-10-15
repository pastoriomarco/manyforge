"""Strict v1 Program authoring plus retained pure tree-artifact coverage."""

from collections.abc import Iterator, Mapping
from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

import manyforge_behavior.program as program_module
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.canonical_artifact import (
    compute_content_identity,
    load_strict_json_yaml,
)
from manyforge_behavior.program import (
    BlackboardKey,
    Program,
    ProgramValidationError,
    ProgramTreeNode,
    TreeNode,
    apply_tree_param_overrides,
    compute_program_tree_hash,
    find_tree_node_by_name,
    merge_param_patch,
    program_node_policies,
    tree_node_from_dict,
)


_ROOT = Path(__file__).resolve().parents[2]
_MAINTAINED_PROGRAMS = (
    "examples/isaac_lite6_lite_gripper_pick_and_place.program.yaml",
    "examples/isaac_manipulator_scene_light.program.yaml",
    "examples/isaac_manipulator_scene_light_collision.program.yaml",
    "examples/lite6_lite_gripper_pick_and_place.program.yaml",
    "examples/pick_and_place.program.yaml",
    "examples/pick_and_place_minimal.program.yaml",
    "examples/pick_and_place_ur10e_robotiq.program.yaml",
    "examples/semantic_shape_pick.program.yaml",
    "examples/trocar_lite6_foundationpose_pick.program.yaml",
    "examples/vision_guided_bin_pick.program.yaml",
    "examples/vision_guided_pose_pick.program.yaml",
)
_MINIMAL_PROGRAM = "examples/pick_and_place_minimal.program.yaml"
_CONTACT_PROGRAM = "examples/semantic_shape_pick.program.yaml"
_SCENE_PROGRAM = "examples/lite6_lite_gripper_pick_and_place.program.yaml"


def _catalog() -> NodeCatalog:
    # The executable-environment manifest deliberately remains frozen until
    # the complete N6-b source batch; this focused loader test consumes the
    # same exact build-owned catalog directly.
    return NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def _identified(document: dict[str, Any]) -> dict[str, Any]:
    result = deepcopy(document)
    result["program_revision"] = compute_content_identity(
        result,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return result


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


def test_program_construction_reports_missing_source(tmp_path: Path) -> None:
    catalog = _catalog()
    missing = tmp_path / "not-read.program.yaml"

    with pytest.raises(ProgramValidationError, match="cannot read"):
        Program(missing, catalog)


def test_program_mapping_construction_rejects_non_v1_document() -> None:
    catalog = _catalog()

    with pytest.raises(ProgramValidationError, match="missing required fields"):
        Program.from_dict({}, catalog)


@pytest.mark.parametrize("alias", ["open", "closed"])
def test_program_keeps_gripper_alias_symbolic_through_runtime_input_validation(alias) -> None:
    from manyforge_behavior.orchestration_values import NodeAttemptInputResolver, ProgramValueStore

    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    position = {"kind": "named_preset", "name": alias}
    source["tree"] = {
        "node_id": "gripper.command_position", "instance_id": "named-command",
        "parameters": {"target_role": "gripper", "position": position}, "children": [],
    }
    program = Program.from_dict(_identified(source), catalog)
    assert program.to_dict()["tree"]["parameters"]["position"] == position
    resolver = NodeAttemptInputResolver(
        program=program, catalog=catalog, value_store=ProgramValueStore(program),
    )
    snapshot = resolver.snapshot(
        node_id="gripper.command_position", node_instance_id="named-command",
        parameters={"target_role": "gripper", "position": position}, value_sources={},
    )
    assert dict(snapshot.parameters["position"]) == position


def test_program_refuses_nonsemantic_gripper_preset_name_before_runtime() -> None:
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    source["tree"] = {
        "node_id": "gripper.command_position", "instance_id": "named-command",
        "parameters": {
            "target_role": "gripper", "position": {"kind": "named_preset", "name": "released"},
        }, "children": [],
    }
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identified(source), _catalog())


@pytest.mark.parametrize(
    "node_id",
    (
        "controller.select_route",
        "lifecycle.prepare_component",
        "lifecycle.enable_component",
        "lifecycle.disable_component",
        "lifecycle.recover_component",
    ),
)
def test_controlled_component_mutations_are_not_program_nodes(node_id: str) -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    source["tree"]["node_id"] = node_id

    with pytest.raises(ProgramValidationError, match="explicit idle-time"):
        Program.from_dict(_identified(source), catalog)


@pytest.mark.parametrize("relative_path", _MAINTAINED_PROGRAMS)
def test_maintained_v1_programs_validate_against_active_catalog(
    relative_path: str,
) -> None:
    catalog = _catalog()
    program = Program(_ROOT / relative_path, catalog)

    assert program.program_revision.startswith("sha256:")
    assert isinstance(program.tree, ProgramTreeNode)
    assert program.catalog_content_version == catalog.content_version
    assert program.to_dict() == load_strict_json_yaml(_ROOT / relative_path)


def test_program_document_copy_is_detached() -> None:
    catalog = _catalog()
    program = Program(_ROOT / _MINIMAL_PROGRAM, catalog)

    first = program.to_dict()
    first["tree"]["parameters"]["unexpected"] = True

    assert "unexpected" not in program.to_dict()["tree"]["parameters"]


def test_program_scene_support_contacts_are_typed_and_reference_scene() -> None:
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    source["scene"][1]["metadata"] = {
        "display_hint": "retained",
        "support_contacts": [
            {
                "support_object_id": source["scene"][0]["scene_resource_id"],
                "mode": "contact_release",
                "release_evidence_mode": "binary_contact",
                "require_separation_by_end": True,
                "allow_recontact": False,
            }
        ],
    }

    parsed = Program.from_dict(_identified(source), _catalog())

    assert parsed.to_dict()["scene"][1]["metadata"] == source["scene"][1][
        "metadata"
    ]


def test_program_scene_support_contact_accepts_explicit_binary_neutral_fields() -> None:
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    source["scene"][1]["metadata"] = {
        "support_contacts": [
            {
                "support_object_id": source["scene"][0]["scene_resource_id"],
                "mode": "contact_release",
                "release_evidence_mode": "binary_contact",
                "enforce_max_start_penetration": False,
                "max_start_penetration_m": 0.0,
                "release_tolerance_m": 0.0,
                "min_release_progress_m": 0.0,
                "require_strictly_decreasing_penetration": False,
            }
        ]
    }

    identified = _identified(source)
    assert Program.from_dict(identified, _catalog()).to_dict() == identified


@pytest.mark.parametrize("support_object_id", ["missing", "self"])
def test_program_scene_support_contact_rejects_invalid_reference(
    support_object_id: str,
) -> None:
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    if support_object_id == "self":
        support_object_id = source["scene"][0]["scene_resource_id"]
    source["scene"][0]["metadata"] = {
        "support_contacts": [{"support_object_id": support_object_id}]
    }

    with pytest.raises(ProgramValidationError, match="support_object_id"):
        Program.from_dict(_identified(source), _catalog())


def test_program_scene_support_contact_rejects_cycle() -> None:
    source = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    first_id = source["scene"][0]["scene_resource_id"]
    second_id = source["scene"][1]["scene_resource_id"]
    source["scene"][0]["metadata"] = {
        "support_contacts": [{"support_object_id": second_id}]
    }
    source["scene"][1]["metadata"] = {
        "support_contacts": [{"support_object_id": first_id}]
    }

    with pytest.raises(ProgramValidationError, match="cyclic support-contact"):
        Program.from_dict(_identified(source), _catalog())


def test_program_node_policy_defaults_are_omission_compatible() -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    baseline = Program.from_dict(source, catalog)
    explicit = deepcopy(source)
    node = explicit["tree"]["children"][0]
    node["recovery_policy"] = {"allow_intervention": False, "max_retries": 0}
    node["operator_stop_policy"] = "inherit"

    explicit["program_revision"] = baseline.program_revision
    normalized = Program.from_dict(explicit, catalog)

    assert normalized.to_dict() == baseline.to_dict()
    assert normalized.canonical_bytes == baseline.canonical_bytes
    assert normalized.program_revision == baseline.program_revision


@pytest.mark.parametrize(
    "field,value",
    [
        ("recovery_policy", {"allow_intervention": False, "max_retries": 0}),
        ("operator_stop_policy", "inherit"),
    ],
)
def test_program_node_policy_independent_defaults_migrate_to_omission(
    field: str, value: Any
) -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    baseline = Program.from_dict(source, catalog)
    explicit = deepcopy(source)
    explicit["tree"]["children"][0][field] = value
    explicit["program_revision"] = baseline.program_revision

    migrated = Program.from_dict(explicit, catalog)

    assert migrated.to_dict() == baseline.to_dict()
    assert migrated.canonical_bytes == baseline.canonical_bytes
    assert migrated.program_revision == baseline.program_revision


@pytest.mark.parametrize(
    "policy",
    [
        {"recovery_policy": None},
        {"recovery_policy": {}},
        {"recovery_policy": {"allow_intervention": True}},
        {"recovery_policy": {"max_retries": 0}},
        {"operator_stop_policy": None},
    ],
)
def test_program_node_policy_rejects_null_empty_and_partial_objects(
    policy: dict[str, Any],
) -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["children"][0].update(policy)
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize(
    "recovery",
    [
        {"allow_intervention": True, "max_retries": 2},
        # Zero is Unlimited; intervention stays an explicit opt-in.
        {"allow_intervention": True, "max_retries": 0},
        {"allow_intervention": False, "max_retries": 3},
    ],
)
def test_program_node_policy_nondefaults_change_identity_and_round_trip(
    recovery: dict[str, Any],
) -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    node = document["tree"]["children"][0]
    node["recovery_policy"] = recovery
    node["operator_stop_policy"] = "hold"

    identified = _identified(document)
    program = Program.from_dict(identified, _catalog())

    child = program.to_dict()["tree"]["children"][0]
    assert child["recovery_policy"] == recovery
    assert child["operator_stop_policy"] == "hold"
    assert program.program_revision == identified["program_revision"]
    (row,) = program_node_policies(program)
    assert row.to_dict() == {
        "instance_id": node["instance_id"],
        "recovery_policy": recovery,
        "operator_stop_policy": "hold",
    }


@pytest.mark.parametrize(
    "field,value",
    [
        # The superseded five-field form, including an old zero budget with
        # intervention: refused, never reinterpreted as Unlimited.
        ("recovery_policy", {
            "allow_intervention": True,
            "allowed_recovery_actions": ["intervention"],
            "max_retries": 0,
            "allowed_skills": [],
            "persistence_ceiling": "session_only",
        }),
        ("recovery_policy", {
            "allow_intervention": False,
            "max_retries": 0,
            "allowed_recovery_actions": [],
        }),
        ("recovery_policy", {
            "allow_intervention": True,
            "max_retries": 0,
            "persistence_ceiling": "session_only",
        }),
        ("recovery_policy", {
            "allow_intervention": True,
            "max_retries": 0,
            "allowed_skills": [],
        }),
        ("modification_policy", {
            "runtime_override": "forbidden",
            "persistent_edit": "forbidden",
            "intervention_delegate": "forbidden",
        }),
        ("modification_policy", {"intervention_delegate": "llm_allowed"}),
    ],
)
def test_program_node_policy_rejects_removed_policy_forms(
    field: str, value: Any
) -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["children"][0][field] = value
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize(
    "field,value",
    [
        ("recovery_policy", {"allow_intervention": "yes", "max_retries": 0}),
        ("recovery_policy", {"allow_intervention": False, "max_retries": True}),
        ("recovery_policy", {"allow_intervention": True, "max_retries": 256}),
        ("recovery_policy", {"allow_intervention": True, "max_retries": -1}),
        ("recovery_policy", {"allow_intervention": True, "max_retries": 1.5}),
        ("operator_stop_policy", "pause"),
    ],
)
def test_program_node_policy_rejects_closed_values_and_bounds(
    field: str, value: Any
) -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["children"][0][field] = value
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identified(document), _catalog())


def test_program_rejects_stale_revision_before_semantic_use() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["name"] = "changed without updating identity"

    with pytest.raises(ProgramValidationError, match="content identity mismatch"):
        Program.from_dict(document, catalog)


def test_program_stale_revision_wins_over_independently_invalid_semantics() -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["program_revision"] = "sha256:" + "f" * 64
    document["tree"]["children"][0]["node_id"] = "motion.not_in_catalog"

    with pytest.raises(ProgramValidationError, match="content identity mismatch"):
        Program.from_dict(document, _catalog())


def test_program_stale_explicit_defaults_still_migrate_only_after_identity() -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    baseline = Program.from_dict(source, catalog)
    explicit = deepcopy(source)
    explicit["tree"]["children"][0]["operator_stop_policy"] = "inherit"
    explicit["program_revision"] = "sha256:" + "f" * 64

    with pytest.raises(ProgramValidationError, match="content identity mismatch"):
        Program.from_dict(explicit, catalog)

    explicit["program_revision"] = baseline.program_revision
    migrated = Program.from_dict(explicit, catalog)
    assert migrated.program_revision == baseline.program_revision


def test_program_default_policy_identity_migration_obeys_tree_bound(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["operator_stop_policy"] = "inherit"
    monkeypatch.setattr(program_module, "_MAX_TREE_NODES", 1)

    with pytest.raises(ProgramValidationError, match="content identity mismatch"):
        Program.from_dict(document, _catalog())


def test_program_identity_migration_does_not_enter_cyclic_json_model() -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["children"].append(document["tree"])

    with pytest.raises(ProgramValidationError, match="collection nesting exceeds"):
        Program.from_dict(document, _catalog())


def test_program_rejects_removed_singleton_surface() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["robot"] = {"robot_id": "ur5", "group_id": "manipulator"}

    with pytest.raises(ProgramValidationError, match="unknown fields.*robot"):
        Program.from_dict(_identified(document), catalog)


def test_program_rejects_dynamic_target_selection() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["parameters"].append(
        {
            "parameter_id": "selected_target",
            "type": "string",
            "default": "arm",
            "constraints": {},
        }
    )
    move = document["tree"]["children"][0]
    del move["parameters"]["target_role"]
    move["value_sources"]["target_role"] = {
        "kind": "program_parameter",
        "reference": "selected_target",
    }
    move["value_sources"] = dict(sorted(move["value_sources"].items()))

    with pytest.raises(ProgramValidationError, match="dynamically select"):
        Program.from_dict(_identified(document), catalog)


def test_program_rejects_value_source_type_mismatch() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    declaration = document["parameters"][0]
    declaration["type"] = "bool"
    declaration["default"] = True
    declaration["constraints"] = {}

    with pytest.raises(ProgramValidationError, match="incompatible"):
        Program.from_dict(_identified(document), catalog)


def test_program_rejects_duplicate_source_joint_goal() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    goal = document["tree"]["children"][0]["parameters"]["goal"]
    goal.append(deepcopy(goal[0]))

    with pytest.raises(ProgramValidationError, match="duplicate.*joint"):
        Program.from_dict(_identified(document), catalog)


def test_program_accepts_ordered_free_text_parameter_defaults() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["parameters"] = [
        {
            "parameter_id": "labels",
            "type": "string_list",
            "default": ["red workpiece", "red workpiece", "Bin A"],
            "constraints": {"min_length": 1},
        },
        {
            "parameter_id": "operator_note",
            "type": "string",
            "default": "Select the red workpiece from Bin A.",
            "constraints": {"min_length": 1},
        },
        *document["parameters"],
    ]

    program = Program.from_dict(_identified(document), catalog)

    assert program.parameters["operator_note"].default.endswith("Bin A.")
    assert program.parameters["labels"].default[0] == "red workpiece"


@pytest.mark.parametrize(
    ("mutation", "message"),
    (
        ("catalog_version", "exact active v2 catalog"),
        ("duplicate_scene", "unique.*scene_resource_id"),
        ("non_unit_quaternion", "unit quaternion"),
        ("unknown_node", "absent from the active v2 catalog"),
        ("leaf_with_child", "must contain 0-0 children"),
        ("missing_required_parameter", "missing required catalog parameter"),
        ("invalid_parameter_default", "default violates max_inclusive"),
        ("unexpanded_interpolation", "legacy.*interpolation.*value_sources"),
        ("undeclared_value_source", "names no Program parameter"),
        ("empty_blackboard_fields", "must not be empty"),
        ("unknown_viewer_role", "does not name a declared role"),
        ("unknown_viewer_scene_resource", "does not name a declared role"),
        ("unknown_scene_policy", "undeclared collision policies"),
        ("inapplicable_constraint", "numeric constraints are unsupported"),
    ),
)
def test_translated_historical_program_negative_corpus(
    mutation: str,
    message: str,
) -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    move = document["tree"]["children"][0]

    if mutation == "catalog_version":
        document["catalog"]["content_version"] = "99.0.0"
    elif mutation == "duplicate_scene":
        document["scene"].append(deepcopy(document["scene"][0]))
    elif mutation == "non_unit_quaternion":
        document["scene"][0]["pose"]["orientation"]["xyzw"][-1] = 2.0
    elif mutation == "unknown_node":
        move["node_id"] = "motion.missing"
    elif mutation == "leaf_with_child":
        nested = deepcopy(document["tree"]["children"][1])
        nested["instance_id"] = "nested_remove"
        move["children"] = [nested]
    elif mutation == "missing_required_parameter":
        del move["parameters"]["target_role"]
    elif mutation == "invalid_parameter_default":
        document["parameters"][0]["default"] = 2.0
    elif mutation == "unexpanded_interpolation":
        del move["value_sources"]
        move["parameters"]["velocity_scale"] = "${param:pick_velocity_scale}"
    elif mutation == "undeclared_value_source":
        move["value_sources"]["velocity_scale"]["reference"] = "missing"
    elif mutation == "empty_blackboard_fields":
        document["blackboard"] = {
            "namespaces": [],
            "keys": [
                {
                    "key": "result",
                    "type": "model_response",
                    "access": "read_write",
                    "persistence": "run",
                    "fields": [],
                }
            ],
        }
    elif mutation == "unknown_viewer_role":
        document["viewer"] = {"frame_role_visibility": {"unknown": True}}
    elif mutation == "unknown_viewer_scene_resource":
        document["viewer"] = {
            "scene_resource_axes_visibility": {"unknown": True}
        }
    elif mutation == "unknown_scene_policy":
        document["scene"][0]["collision_policy_ids"] = ["missing_policy"]
    elif mutation == "inapplicable_constraint":
        document["parameters"].insert(
            0,
            {
                "parameter_id": "operator_note",
                "type": "string",
                "default": "ready",
                "constraints": {"min_inclusive": 0.0},
            },
        )
    else:  # pragma: no cover - keeps the mutation table closed.
        raise AssertionError(f"unhandled mutation {mutation}")

    with pytest.raises(ProgramValidationError, match=message):
        Program.from_dict(_identified(document), catalog)


def test_contact_selector_must_resolve_to_one_declared_target() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _CONTACT_PROGRAM)
    document["target_roles"]["gripper_secondary"] = deepcopy(
        document["target_roles"]["gripper"]
    )

    with pytest.raises(ProgramValidationError, match="exactly one.*target role"):
        Program.from_dict(_identified(document), catalog)


@pytest.mark.parametrize(
    ("field_name", "invalid_value"),
    (
        ("carrier_frame_role", "base"),
        ("touch_body_roles", ["arm_collision"]),
    ),
)
def test_carried_object_roles_are_scoped_to_selected_target(
    field_name: str,
    invalid_value: object,
) -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(
        _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
    )
    attach = next(
        child
        for child in document["tree"]["children"]
        if child["node_id"] == "scene.attach_carried_object"
    )
    attach["parameters"][field_name] = invalid_value

    with pytest.raises(ProgramValidationError, match="required by target role 'gripper'"):
        Program.from_dict(_identified(document), catalog)


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
