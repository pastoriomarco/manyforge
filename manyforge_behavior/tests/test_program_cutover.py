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
    ProgramNodeValidationError,
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
    "recovery,plain",
    [
        (
            {"allow_intervention": False, "max_retries": 0, "intervention_stop_scope": "all"},
            None,
        ),
        (
            {"allow_intervention": True, "max_retries": 2, "intervention_stop_scope": "all"},
            {"allow_intervention": True, "max_retries": 2},
        ),
    ],
)
def test_program_intervention_stop_scope_default_migrates_to_omission(
    recovery: dict[str, Any],
    plain: dict[str, Any] | None,
) -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    if plain is None:
        baseline = Program.from_dict(source, catalog)
    else:
        plain_document = deepcopy(source)
        plain_document["tree"]["children"][0]["recovery_policy"] = plain
        baseline = Program.from_dict(_identified(plain_document), catalog)
    explicit = deepcopy(source)
    explicit["tree"]["children"][0]["recovery_policy"] = recovery
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
        {"recovery_policy": {"intervention_stop_scope": "own_groups"}},
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
        {
            "allow_intervention": True,
            "max_retries": 2,
            "intervention_stop_scope": "own_groups",
        },
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
        ("recovery_policy", {
            "allow_intervention": True,
            "max_retries": 0,
            "intervention_stop_scope": "groups",
        }),
        ("recovery_policy", {
            "allow_intervention": True,
            "max_retries": 0,
            "intervention_stop_scope": None,
        }),
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


def test_program_hand_edit_loads_with_edited_outside_diagnostic() -> None:
    catalog = _catalog()
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    assert Program.from_dict(document, catalog).load_diagnostics == ()
    document["name"] = "changed without updating identity"

    program = Program.from_dict(document, catalog, authoring_load=True)

    assert [row["code"] for row in program.load_diagnostics] == [
        "edited_outside_manyforge"
    ]
    assert program.load_diagnostics[0]["path"].endswith(".program_revision")
    # The computed revision is the one recorded, and it reloads cleanly.
    assert program.program_revision == _identified(document)["program_revision"]
    assert program.to_dict()["program_revision"] == program.program_revision
    assert Program.from_dict(program.to_dict(), catalog).load_diagnostics == ()


def test_program_stale_revision_does_not_hide_semantic_errors() -> None:
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["program_revision"] = "sha256:" + "f" * 64
    document["tree"]["children"][0]["node_id"] = "motion.not_in_catalog"

    with pytest.raises(ProgramNodeValidationError, match="absent from the active v2 catalog"):
        Program.from_dict(document, _catalog(), authoring_load=True)


def test_program_explicit_defaults_migrate_and_stale_identity_is_diagnosed() -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    baseline = Program.from_dict(source, catalog)
    explicit = deepcopy(source)
    explicit["tree"]["children"][0]["operator_stop_policy"] = "inherit"
    explicit["program_revision"] = "sha256:" + "f" * 64

    stale = Program.from_dict(explicit, catalog, authoring_load=True)
    assert [row["code"] for row in stale.load_diagnostics] == ["edited_outside_manyforge"]
    assert stale.program_revision == baseline.program_revision

    explicit["program_revision"] = baseline.program_revision
    migrated = Program.from_dict(explicit, catalog)
    assert migrated.program_revision == baseline.program_revision
    assert migrated.load_diagnostics == ()


def _with_catalog_version(document: dict[str, Any], version: str) -> dict[str, Any]:
    changed = deepcopy(document)
    changed["catalog"]["content_version"] = version
    return _identified(changed)


def test_program_saved_under_older_catalog_minor_loads_as_active() -> None:
    catalog = _catalog()
    major, minor, _patch = catalog.content_version.split(".")
    assert int(minor) > 0
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    active = Program.from_dict(source, catalog)
    older = _with_catalog_version(source, f"{major}.{int(minor) - 1}.0")

    # Strict (Applied/runtime) reads keep M3: an older selector is refused.
    with pytest.raises(ProgramValidationError, match="exact active v2 catalog"):
        Program.from_dict(older, catalog)
    program = Program.from_dict(older, catalog, authoring_load=True)

    # Not an edit outside ManyForge: its own identity matches.
    assert program.load_diagnostics == ()
    assert program.saved_catalog_content_version == f"{major}.{int(minor) - 1}.0"
    # Save writes the active version; the recorded revision is the active one.
    assert program.to_dict()["catalog"]["content_version"] == catalog.content_version
    assert program.program_revision == active.program_revision
    assert program.to_dict() == active.to_dict()


@pytest.mark.parametrize("offset", ("next_major", "previous_major", "next_minor"))
def test_program_incompatible_catalog_version_is_refused(offset: str) -> None:
    catalog = _catalog()
    major, minor, _patch = (int(part) for part in catalog.content_version.split("."))
    version = {
        "next_major": f"{major + 1}.0.0",
        "previous_major": f"{major - 1}.{minor}.0",
        "next_minor": f"{major}.{minor + 1}.0",
    }[offset]
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)

    with pytest.raises(ProgramValidationError, match="compatible active v2 catalog"):
        Program.from_dict(
            _with_catalog_version(source, version), catalog, authoring_load=True
        )


def test_program_node_errors_name_the_node_and_field() -> None:
    catalog = _catalog()
    source = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    node = source["tree"]["children"][0]

    unknown = deepcopy(source)
    unknown["tree"]["children"][0]["parameters"]["removed_parameter"] = 1
    with pytest.raises(ProgramNodeValidationError) as raised:
        Program.from_dict(_identified(unknown), catalog)
    assert raised.value.code == "program_node_invalid"
    assert (raised.value.instance_id, raised.value.node_id, raised.value.field) == (
        node["instance_id"],
        node["node_id"],
        "parameters.removed_parameter",
    )

    removed = deepcopy(source)
    removed["tree"]["children"][0]["node_id"] = "motion.removed"
    with pytest.raises(ProgramNodeValidationError) as raised:
        Program.from_dict(_identified(removed), catalog)
    assert (raised.value.instance_id, raised.value.node_id, raised.value.field) == (
        node["instance_id"],
        "motion.removed",
        "node_id",
    )


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
        ("removed_scene_policy_ids", r"contains unknown fields \['collision_policy_ids'\]"),
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
    elif mutation == "removed_scene_policy_ids":
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


def _logic_only_document() -> dict[str, Any]:
    """A Program with no component or target role and only a timer."""

    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["component_roles"] = {}
    document["target_roles"] = {}
    document.pop("scene", None)
    document.pop("parameters", None)
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "main_sequence",
        "parameters": {},
        "children": [
            {
                "node_id": "timer",
                "instance_id": "wait",
                "parameters": {"duration_s": 0.1},
                "children": [],
            }
        ],
    }
    return document


def test_logic_only_program_parses_with_an_empty_component_role_map() -> None:
    program = Program.from_dict(_identified(_logic_only_document()), _catalog())

    assert program.to_dict()["component_roles"] == {}
    assert program.to_dict()["target_roles"] == {}


def test_logic_only_declarations_accept_an_empty_component_role_map() -> None:
    parameters, keys = program_module.program_value_declarations(
        _identified(_logic_only_document()), catalog=_catalog()
    )

    assert parameters == {}
    assert keys == {}


def test_program_refuses_an_absent_component_role_map() -> None:
    document = _logic_only_document()
    del document["component_roles"]

    with pytest.raises(ProgramValidationError, match="component_roles"):
        Program.from_dict(_identified(document), _catalog())
    with pytest.raises(ProgramValidationError, match="component_roles"):
        program_module.program_value_declarations(
            _identified(document), catalog=_catalog()
        )


def test_program_still_refuses_a_target_without_a_declared_component_role() -> None:
    document = _logic_only_document()
    document["target_roles"] = {
        "arm": {
            "component_role": "robot",
            "required_capabilities": [],
        }
    }

    with pytest.raises(ProgramValidationError, match="declared component role"):
        Program.from_dict(_identified(document), _catalog())


def test_component_role_map_keeps_its_fixed_upper_bound() -> None:
    too_many = {
        f"role_{index}": {}
        for index in range(program_module._MAX_PROGRAM_ROLES + 1)
    }

    with pytest.raises(ProgramValidationError, match="bounded mapping"):
        program_module._validate_component_roles(too_many, path="Program.component_roles")


@pytest.mark.parametrize("literal", [2**53, -(2**53), 2**63])
def test_integer_literals_outside_the_exact_range_are_refused_at_parse(literal: int) -> None:
    safe = 2**53 - 1

    def tree(value: int) -> dict[str, Any]:
        return {
            "id": "sequence",
            "name": "root",
            "children": [
                {"id": "flow.wait", "name": "pause", "params": {"count": value}}
            ],
        }

    assert tree_node_from_dict(tree(safe)).children[0].params["count"] == safe
    assert tree_node_from_dict(tree(-safe)).children[0].params["count"] == -safe
    with pytest.raises(ProgramValidationError, match=r"outside the exact range ±\(2\^53−1\)"):
        tree_node_from_dict(tree(literal))
    with pytest.raises(ProgramValidationError, match=r"±\(2\^53−1\)"):
        merge_param_patch({}, {"nested": {"count": literal}})
    document = load_strict_json_yaml(_ROOT / _MINIMAL_PROGRAM)
    document["tree"]["children"][0].setdefault("parameters", {})["count"] = literal
    with pytest.raises(ProgramValidationError, match="safe range"):
        Program.from_dict(document, _catalog())


def _endpoint_motion_document():
    document = load_strict_json_yaml(_ROOT / _SCENE_PROGRAM)
    pending = [document['tree']]
    while pending:
        node = pending.pop()
        if node['node_id'] == 'motion.move_to_pose':
            document['tree'] = deepcopy(node)
            break
        pending.extend(node.get('children', ()))
    else:
        raise AssertionError('fixture has no pose motion')
    parameters = document['tree']['parameters']
    parameters.pop('tcp_frame_role', None)  # Build only the new fixture form.
    parameters['tcp_endpoint'] = {'kind': 'entry', 'target_role': 'gripper', 'frame_role': 'tcp'}  # TCP migration: selector now needs kind (entry)
    return document


def test_motion_endpoint_uses_qualified_declared_target_frame() -> None:
    document = _endpoint_motion_document()
    program = Program.from_dict(_identified(document), _catalog())
    assert program.to_dict()['tree']['parameters']['tcp_endpoint'] == {  # TCP migration: selector needs kind (entry)
        'kind': 'entry', 'target_role': 'gripper', 'frame_role': 'tcp',
    }
    assert 'tcp_frame_role' not in program.to_dict()['tree']['parameters']


@pytest.mark.parametrize('mutation', ['both', 'legacy_only', 'unknown_target', 'wrong_frame', 'extra', 'null', 'dynamic'])  # TCP migration: missing endpoint = current, not refused
def test_motion_endpoint_rejects_ambiguous_or_uncaptured_selection(mutation) -> None:
    document = _endpoint_motion_document()
    parameters = document['tree']['parameters']
    if mutation == 'both':
        parameters['tcp_frame_role'] = ''
    elif mutation == 'legacy_only':
        parameters.pop('tcp_endpoint')
        parameters['tcp_frame_role'] = 'tcp'
    elif mutation == 'unknown_target':
        parameters['tcp_endpoint']['target_role'] = 'missing'
    elif mutation == 'wrong_frame':
        parameters['tcp_endpoint']['frame_role'] = 'base'
    elif mutation == 'extra':
        parameters['tcp_endpoint']['collision_body_role'] = 'gripper_collision'
    elif mutation == 'null':
        parameters['tcp_endpoint'] = None
    else:
        document['tree']['value_sources'] = {'tcp_endpoint.frame_role': {'kind': 'parameter', 'parameter_id': 'endpoint'}}
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize('reverse', [False, True])
def test_contact_policy_motion_endpoint_is_one_exact_pair(reverse) -> None:
    document = load_strict_json_yaml(_ROOT / _CONTACT_PROGRAM)
    # Every user of endpoint-derived contact bodies must select its TCP.
    for node in document['tree']['children']:
        parameters = node.get('parameters', {})
        if 'contact_policy_id' in parameters:
            parameters['tcp_endpoint'] = {'kind': 'entry', 'target_role': 'gripper', 'frame_role': 'tcp'}  # TCP migration: selector now needs kind (entry)
    policy = document['contact_policies'][0]
    old_pair = policy['zones'][0]['body_pairs'][0]
    runtime = next(value for value in old_pair.values() if value['kind'] == 'runtime_object')
    pair = {'body_a': {'kind': 'motion_endpoint'}, 'body_b': runtime}
    if reverse:
        pair = {'body_a': runtime, 'body_b': {'kind': 'motion_endpoint'}}
    policy['zones'][0]['body_pairs'] = [pair]
    policy['version'] = compute_content_identity(policy, own_field='version', identity_domain='catalog-record-revision/contact_policy', api_version='manyforge.contact_policy.v1')
    assert Program.from_dict(_identified(document), _catalog()).to_dict()['contact_policies'][0]['zones'][0]['body_pairs'] == [pair]
    for invalid_pairs in ([pair, old_pair], [{'body_a': {'kind': 'motion_endpoint', 'target_role': 'gripper'}, 'body_b': runtime}]):
        policy['zones'][0]['body_pairs'] = invalid_pairs
        policy['version'] = compute_content_identity(policy, own_field='version', identity_domain='catalog-record-revision/contact_policy', api_version='manyforge.contact_policy.v1')
        with pytest.raises(ProgramValidationError):
            Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize("node_kind", ["probe", "pick"])
@pytest.mark.parametrize("selection", ["qualified_explicit", "qualified_endpoint"])  # TCP migration: missing endpoint = current, not refused
def test_endpoint_contact_policy_requires_qualified_tcp_at_authoring(node_kind, selection) -> None:
    document = load_strict_json_yaml(_ROOT / _CONTACT_PROGRAM)
    if node_kind == "probe":
        document["tree"] = {
            "node_id": "motion.probe_search", "instance_id": "probe", "children": [],
            "parameters": {
                "target_role": "arm", "base_frame_role": "base",
                "tcp_endpoint": {"kind": "current"},  # TCP migration: the arm declares no tcp frame
                "goal": {
                    "frame_ref": {"kind": "target_frame", "target_role": "arm", "frame_role": "base"},
                    "position_m": [0.4, 0.0, 0.2], "orientation": {"rpy_deg": [180.0, 0.0, 0.0]},
                },
                "contact_policy_id": "selected_object_grasp",
                "contact_scene_resource_id": "cube_part",
                "stop_mode": "soft_on_path",
                "interrupt_condition": {"compare": {
                    "op": "==", "operand": {"key": "gripper_closed"}, "value": True,
                }},
            },
        }
    pending = [document["tree"]]
    users = []
    while pending:
        node = pending.pop()
        if "contact_policy_id" in node.get("parameters", {}):
            users.append(node)
        pending.extend(node.get("children", ()))
    assert len(users) == 1
    parameters = users[0]["parameters"]
    assert users[0]["node_id"] == (
        "motion.probe_search" if node_kind == "probe" else "planning.plan_pick_sequence"
    )
    parameters["tcp_endpoint"] = {"kind": "current"}  # TCP migration: contact follows the Active TCP
    policy = next(
        row for row in document["contact_policies"]
        if row["policy_id"] == parameters["contact_policy_id"]
    )
    if selection != "qualified_explicit":
        old_pair = policy["zones"][0]["body_pairs"][0]
        runtime = next(value for value in old_pair.values() if value["kind"] == "runtime_object")
        policy["zones"][0]["body_pairs"] = [
            {"body_a": {"kind": "motion_endpoint"}, "body_b": runtime}
        ]
        policy["version"] = compute_content_identity(
            policy, own_field="version", identity_domain="catalog-record-revision/contact_policy",
            api_version="manyforge.contact_policy.v1",
        )
    parsed = Program.from_dict(_identified(document), _catalog())
    assert parsed.to_dict()["contact_policies"] == document["contact_policies"]


def test_joint_goal_can_author_an_explicit_endpoint_withdrawal_policy() -> None:
    authoring = load_strict_json_yaml(_ROOT / 'examples/r5_trial_e_signal.scenario/program.yaml')
    assert len(authoring['forest']) == 1
    document = {
        key: deepcopy(value) for key, value in authoring.items()
        if key not in {'schema_version', 'forest', 'positions'}
    }
    document['tree'] = deepcopy(authoring['forest'][0])
    document['program_revision'] = 'sha256:' + '0' * 64
    pending = [document['tree']]
    while pending:
        node = pending.pop()
        if node['instance_id'] == 'end_rest':
            assert node['node_id'] == 'motion.move_to_joint_target'
            node['parameters']['contact_policy_id'] = 'withdrawal_contact_policy'
            node['parameters']['contact_scene_resource_id'] = 'probe_block'
            break
        pending.extend(node.get('children', ()))
    else:
        raise AssertionError('Trial E has no withdrawal move')
    program = Program.from_dict(_identified(document), _catalog())
    pending = [program.to_dict()['tree']]
    while pending:
        node = pending.pop()
        if node['instance_id'] == 'end_rest':
            assert node['parameters']['contact_policy_id'] == 'withdrawal_contact_policy'
            assert node['parameters']['contact_scene_resource_id'] == 'probe_block'
            assert node['parameters']['tcp_endpoint'] == {'kind': 'default_of_role', 'role': 'gripper'}  # TCP migration: withdrawal follows gripper default
            break
        pending.extend(node.get('children', ()))


def _contact_release_document(*, evidence="signed_penetration", pick=False):
    document = load_strict_json_yaml(_ROOT / _CONTACT_PROGRAM)
    if not pick:
        pending = [document["tree"]]
        while pending:
            node = pending.pop()
            if node["node_id"] == "motion.move_to_pose":
                document["tree"] = deepcopy(node)
                break
            pending.extend(node.get("children", ()))
        else:
            raise AssertionError("contact fixture has no pose move")
        document["tree"]["parameters"].update(
            tcp_endpoint={"kind": "entry", "target_role": "gripper", "frame_role": "tcp"},  # TCP migration: selector now needs kind (entry)
            contact_policy_id=document["contact_policies"][0]["policy_id"],
            contact_scene_resource_id="cube_part",
        )
    policy = document["contact_policies"][0]
    policy["mode"] = "contact_release"
    zone = policy["zones"][0]
    zone.pop("max_penetration_m")
    zone.pop("terminal_window_start_fraction")
    zone["release_evidence_mode"] = evidence
    old_pair = zone["body_pairs"][0]
    runtime = next(side for side in old_pair.values() if side["kind"] == "runtime_object")
    zone["body_pairs"] = [{"body_a": {"kind": "motion_endpoint"}, "body_b": runtime}]
    return document


def _identify_contact_release(document):
    policy = document["contact_policies"][0]
    policy["version"] = compute_content_identity(
        policy, own_field="version", identity_domain="catalog-record-revision/contact_policy",
        api_version="manyforge.contact_policy.v1",
    )
    return _identified(document)


@pytest.mark.parametrize("evidence", ["signed_penetration", "binary_contact"])
@pytest.mark.parametrize("explicit", [False, True])
def test_contact_release_keeps_exact_endpoint_policy_and_authored_envelope(evidence, explicit):
    document = _contact_release_document(evidence=evidence)
    if explicit:
        document["contact_policies"][0]["zones"][0].update(
            enforce_max_start_penetration=evidence == "signed_penetration",
            max_start_penetration_m=0.002 if evidence == "signed_penetration" else 0.0,
            min_release_progress_m=0.0,
            release_tolerance_m=1e-5 if evidence == "signed_penetration" else 0.0,
            require_strictly_decreasing_penetration=evidence == "signed_penetration",
            require_separation_by_end=True,
            allow_recontact=False,
        )
    identified = _identify_contact_release(document)
    parsed = Program.from_dict(identified, _catalog())
    assert parsed.to_dict()["contact_policies"] == identified["contact_policies"]
    assert parsed.to_dict()["tree"]["parameters"]["tcp_endpoint"] == {  # TCP migration: selector needs kind (entry)
        "kind": "entry", "target_role": "gripper", "frame_role": "tcp",
    }


@pytest.mark.parametrize("field,value", [
    ("release_evidence_mode", None), ("release_evidence_mode", "distance"),
    ("max_start_penetration_m", -0.001), ("min_release_progress_m", 1000.001),
    ("release_tolerance_m", True), ("allow_recontact", 1),
    ("max_penetration_m", 0.002), ("terminal_window_start_fraction", 0.9),
    ("unknown_release_field", False),
])
def test_contact_release_rejects_crossed_or_malformed_envelope(field, value):
    document = _contact_release_document()
    zone = document["contact_policies"][0]["zones"][0]
    if value is None:
        zone.pop(field)
    else:
        zone[field] = value
    with pytest.raises(ProgramValidationError):
        Program.from_dict(_identify_contact_release(document), _catalog())


@pytest.mark.parametrize("field,value", [
    ("enforce_max_start_penetration", True), ("max_start_penetration_m", 0.002),
    ("min_release_progress_m", 0.001), ("release_tolerance_m", 1e-5),
    ("require_strictly_decreasing_penetration", True),
])
def test_binary_contact_release_rejects_non_neutral_metric_constraints(field, value):
    document = _contact_release_document(evidence="binary_contact")
    document["contact_policies"][0]["zones"][0][field] = value
    with pytest.raises(ProgramValidationError, match="binary_contact forbids"):
        Program.from_dict(_identify_contact_release(document), _catalog())


def test_pick_rejects_a_contact_release_policy_before_runtime():
    document = _contact_release_document(pick=True)
    with pytest.raises(ProgramValidationError, match="pick requires a contact_grasp policy"):
        Program.from_dict(_identify_contact_release(document), _catalog())


@pytest.mark.parametrize(
    "selector",
    [
        {"kind": "current"},
        {"kind": "default_of_role", "role": "tool"},
        {"kind": "entry", "target_role": "tool", "frame_role": "tip"},
    ],
)
def test_portable_tcp_selector_forms(selector) -> None:
    assert program_module.motion_tcp_selection({"tcp_endpoint": selector}) == {
        "tcp_endpoint": selector
    }
    assert program_module.motion_tcp_selection({}) == {
        "tcp_endpoint": {"kind": "current"}
    }


@pytest.mark.parametrize(
    "selector",
    [
        {"target_role": "tool", "frame_role": "tip"},
        {"kind": "current", "role": "tool"},
        {"kind": "default_of_role"},
        {"kind": "entry", "target_role": "tool"},
        {"kind": []},
    ],
)
def test_portable_tcp_selector_rejects_malformed_forms(selector) -> None:
    with pytest.raises(ProgramValidationError):
        program_module.motion_tcp_selection({"tcp_endpoint": selector})


def test_tcp_entry_value_is_structured_and_preserves_dotted_ids() -> None:
    entry = {"component_id": "tool.one", "target_id": "gripper.one", "frame_role_id": "tip.one"}
    program_module._validate_declared_type_value("tcp_entry", entry, path="value")
    for invalid in ("tool.one.gripper.one.tip.one", {**entry, "extra": "x"}, {"component_id": "tool"}):
        with pytest.raises(ProgramValidationError):
            program_module._validate_declared_type_value("tcp_entry", invalid, path="value")


def test_active_tcp_namespace_is_reserved_for_system_keys() -> None:
    assert program_module.active_tcp_key("arm.left") == "active_tcp.arm.left"
    assert program_module.is_active_tcp_key("active_tcp.arm.left")
    with pytest.raises(ProgramValidationError, match="reserved"):
        program_module._validate_blackboard(
            {"namespaces": [{"namespace_id": "active_tcp", "owner": "program"}], "keys": []},
            path="blackboard", scene_resource_ids=set(), target_roles={},
        )


def test_motion_role_declares_read_only_active_tcp_key() -> None:
    document = _logic_only_document()
    document["component_roles"] = {"robot": {}}
    document["target_roles"] = {
        "arm": {
            "component_role": "robot",
            "required_capabilities": [{
                "capability_id": "execution.trajectory.execute",
                "parameters": {},
                "disposition": "required",
            }],
        }
    }
    document["catalog"]["content_version"] = _catalog().content_version
    program = Program.from_dict(_identified(document), _catalog())
    key = program.blackboard_keys["active_tcp.arm"]
    assert (key.type, key.access, key.persistence, key.has_initial) == (
        "tcp_entry", "read", "run", False
    )
    program.validate_key_value(
        key.key,
        {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tip"},
        path="value",
    )


def test_set_tcp_accepts_whole_tcp_entry_source() -> None:
    document = _logic_only_document()
    document["component_roles"] = {"robot": {}}
    document["target_roles"] = {
        "arm": {
            "component_role": "robot",
            "required_capabilities": [{
                "capability_id": "execution.trajectory.execute",
                "parameters": {},
                "disposition": "required",
            }],
        }
    }
    document["catalog"]["content_version"] = _catalog().content_version
    document["tree"] = {
        "node_id": "motion.set_tcp", "instance_id": "choose_tcp",
        "parameters": {"target_role": "arm"},
        "value_sources": {"tcp": {"kind": "blackboard_key", "reference": "active_tcp.arm"}},
        "children": [],
    }
    program = Program.from_dict(_identified(document), _catalog())
    assert program.to_dict()["tree"]["value_sources"]["tcp"]["reference"] == "active_tcp.arm"


def test_active_tcp_declarations_follow_bound_target_presence() -> None:
    document = _logic_only_document()
    document["component_roles"] = {"robot": {}}
    document["target_roles"] = {
        "arm": {
            "component_role": "robot",
            "required_capabilities": [{
                "capability_id": "execution.trajectory.execute",
                "parameters": {}, "disposition": "required",
            }],
        },
        "planner": {
            "component_role": "robot",
            "required_capabilities": [{
                "capability_id": "planning.pick_sequence",
                "parameters": {}, "disposition": "required",
            }],
        },
    }
    document["catalog"]["content_version"] = _catalog().content_version
    program = Program.from_dict(_identified(document), _catalog())
    bound = program.with_active_tcp_binding({"target_role_bindings": [
        {"target_role_id": "arm"},
        {"target_role_id": "planner", "robot_tcps": []},
    ]})
    assert "active_tcp.arm" in program.blackboard_keys
    assert "active_tcp.arm" not in bound.blackboard_keys
    key = bound.blackboard_keys["active_tcp.planner"]
    assert (key.type, key.access, key.persistence) == ("tcp_entry", "read", "run")
    assert "active_tcp.planner" not in program.blackboard_keys


def test_portable_active_tcp_key_is_declared_for_planning_role():
    keys = program_module._declare_active_tcp_selector_keys(
        {}, [{
            "node_id": "planning.plan_pick_sequence",
            "parameters": {"target_role": "planner"},
            "children": [],
        }], {"planner": {}},
    )
    assert keys["active_tcp.planner"].type == "tcp_entry"
    assert keys["active_tcp.planner"].access == "read"
    assert program_module._declare_active_tcp_selector_keys(
        {}, [{"node_id": "planning.plan_place_sequence",
              "parameters": {"target_role": "missing"}, "children": []}],
        {"planner": {}},
    ) == {}
