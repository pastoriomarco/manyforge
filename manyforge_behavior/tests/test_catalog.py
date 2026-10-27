"""Contract tests for the sole v2 node-catalog runtime input."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

import pytest
import rfc8785

import manyforge_behavior.catalog as catalog_module
from manyforge_behavior.catalog import (
    EXPECTED_API_VERSION,
    EXPECTED_CONTENT_VERSION,
    CatalogValidationError,
    NodeCatalog,
    profile_satisfied,
)
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.node_registry import build_node_registry


CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
EXPECTED_CATALOG_DIGEST = "sha256:29eaa1447b105a20b4582b508938ac0ba97d77b9fcdc26cd97467b42b628dc93"


def _catalog_data() -> dict[str, object]:
    return NodeCatalog(CATALOG_PATH).raw_data


def _write_recomputed(tmp_path: Path, mutation) -> Path:
    data = copy.deepcopy(_catalog_data())
    mutation(data)
    payload = dict(data)
    payload.pop("catalog_digest", None)
    data["catalog_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(payload)).hexdigest()
    path = tmp_path / "catalog.yaml"
    path.write_text(
        json.dumps(data, ensure_ascii=False, allow_nan=False),
        encoding="utf-8",
    )
    return path


def test_shipped_catalog_is_exact_frozen_v2_packet() -> None:
    catalog = NodeCatalog(CATALOG_PATH)

    assert catalog.api_version == EXPECTED_API_VERSION
    assert catalog.content_version == EXPECTED_CONTENT_VERSION
    assert catalog.catalog_id == "manyforge_builtin"
    assert catalog.catalog_digest == EXPECTED_CATALOG_DIGEST
    assert len(catalog.entries) == 49
    assert (
        len(
            {
                (entry.id, engine_id, implementation_id)
                for entry in catalog.entries
                for engine_id, implementation_id in entry.runner_implementation_ids.items()
            }
        )
        == 49
    )


@pytest.mark.parametrize("node_id,parameter_id", [
    ("gripper.command_position", "position"),
    ("planning.plan_pick_sequence", "tool_position"),
    ("planning.plan_place_sequence", "tool_position"),
])
@pytest.mark.parametrize("value,valid", [
    ({"kind": "named_preset", "name": "open"}, True),
    ({"kind": "named_preset", "name": "closed"}, True),
    ({"kind": "angular_rad", "value_rad": 0.2}, True),
    ({"kind": "linear_m", "value_m": 0.008}, True),
    ({"kind": "named_preset"}, False),
    ({"kind": "named_preset", "name": "released"}, False),
    ({"kind": "named_preset", "name": "open", "value_m": 0.0}, False),
    ({"kind": "linear_m", "value_m": 0.0, "name": "open"}, False),
    ({"kind": "angular_rad", "value_rad": 0.1, "value_m": 0.0}, False),
])
def test_gripper_position_schema_keeps_numeric_and_strict_semantic_aliases(
    node_id, parameter_id, value, valid,
) -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    parameter = next(row for row in catalog.get(node_id).parameters if row.id == parameter_id)
    issues = validate_value(parameter.schema, value, registry=catalog.schema_registry)
    assert (not issues) is valid


def test_gripper_program_dwell_policy_is_optional_pause_default() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    assert catalog.content_version == "2.7.0"
    gripper = catalog.get("gripper.command_position")
    policy = next(parameter for parameter in gripper.parameters if parameter.id == "dwell_while_held")
    assert policy.required is False
    assert policy.has_default is True
    assert policy.default == "pause"
    assert catalog.schema_registry.resolve(policy.schema).enum_values == ("count", "pause")
    duration = next(parameter for parameter in gripper.parameters if parameter.id == "command_duration_s")
    assert duration.required is False
    assert duration.has_default is False
    assert not validate_value(duration.schema, 0.0, registry=catalog.schema_registry)


@pytest.mark.parametrize("value,valid", [
    ("count", True), ("pause", True), ("continue", False), ("", False),
    (None, False), (False, False), (0, False), (1.0, False),
])
def test_gripper_dwell_policy_uses_the_closed_enum(value, valid) -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    policy = next(parameter for parameter in catalog.get("gripper.command_position").parameters
                  if parameter.id == "dwell_while_held")
    assert (not validate_value(policy.schema, value, registry=catalog.schema_registry)) is valid


@pytest.mark.parametrize("node_id", [
    "motion.move_to_joint_target",
    "motion.move_to_pose",
    "motion.follow_cartesian_path",
])
def test_motion_automatic_retry_parameters_are_optional_with_bounded_defaults(node_id) -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    parameters = {parameter.id: parameter for parameter in catalog.get(node_id).parameters}
    retries = parameters["automatic_retries"]
    delay = parameters["automatic_retry_delay_s"]
    assert (retries.required, retries.has_default, retries.default) == (False, True, 3)
    assert (delay.required, delay.has_default, delay.default) == (False, True, 1.0)
    for value, valid in ((0, True), (10, True), (11, False), (-1, False)):
        assert (not validate_value(retries.schema, value, registry=catalog.schema_registry)) is valid
    for value, valid in ((0.0, True), (60.0, True), (60.5, False), (-0.1, False)):
        assert (not validate_value(delay.schema, value, registry=catalog.schema_registry)) is valid


@pytest.mark.parametrize("node_id", [
    "motion.move_to_joint_target",
    "motion.move_to_pose",
    "motion.follow_cartesian_path",
])
def test_motion_preemption_parameters_are_optional_with_closed_values(node_id) -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    parameters = {parameter.id: parameter for parameter in catalog.get(node_id).parameters}
    priority = parameters["preemption_priority"]
    result = parameters["preempted_result"]
    # The outcome lives in the system-owned node_outcome.<instance_id> key.
    assert "outcome_key" not in parameters
    assert (priority.required, priority.has_default, priority.default) == (False, True, 0)
    assert (result.required, result.has_default, result.default) == (False, True, "failure")
    for value, valid in ((0, True), (100, True), (101, False), (-1, False)):
        assert (not validate_value(priority.schema, value, registry=catalog.schema_registry)) is valid
    for value, valid in (("success", True), ("failure", True), ("aborted", False)):
        assert (not validate_value(result.schema, value, registry=catalog.schema_registry)) is valid


def test_recomputed_previous_catalog_version_is_not_an_alias(tmp_path: Path) -> None:
    path = _write_recomputed(tmp_path, lambda data: data.__setitem__("content_version", "2.6.0"))
    with pytest.raises(CatalogValidationError, match="content_version must be '2.7.0'"):
        NodeCatalog(path)


def test_catalog_exposes_structured_v2_semantics() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    move = catalog.get("motion.move_to_pose")

    assert move.kind == "leaf"
    assert move.category == "motion"
    assert move.min_program_profile == "composable-cell-v1"
    assert move.execution.owner_api == "kernel.operation"
    assert move.runner_implementation_ids == {"py_trees": "motion_move_to_pose_v1"}
    assert {parameter.id for parameter in move.parameters} == {
        "acceleration_scale",
        "automatic_retries",
        "automatic_retry_delay_s",
        "base_frame_role",
        "cache_mode",
        "contact_scene_resource_id",
        "contact_policy_id",
        "execution_timeout_s",
        "goal",
        "joint_position_tol_rad",
        "max_tcp_linear_speed_mps",
        "offset_reference",
        "per_joint_dynamics_limits",
        "planning_profile",
        "planning_timeout_s",
        "preempted_result",
        "preemption_priority",
        "rotation_tol_rad",
        "target_reference",
        "target_mode",
        "target_role",
        "tcp_endpoint",
        "translation_tol_m",
        "velocity_scale",
    }
    parameters = {parameter.id: parameter for parameter in move.parameters}
    for parameter_id, default in {
        "automatic_retries": 3,
        "automatic_retry_delay_s": 1.0,
        "preemption_priority": 0,
        "preempted_result": "failure",
    }.items():
        assert parameters[parameter_id].required is False
        assert parameters[parameter_id].has_default is True
        assert parameters[parameter_id].default == default
    assert "outcome_key" not in parameters
    assert {
        (requirement.capability_id, requirement.scope) for requirement in move.required_capabilities
    } >= {
        ("planning.motion.pose_goal", "target_binding"),
        ("execution.trajectory.execute", "target_binding"),
    }
    assert {
        requirement.capability_id
        for requirement in move.required_capabilities
        if requirement.effectful
    } == {"execution.trajectory.execute"}


def test_pose_source_observation_is_target_scoped() -> None:
    assert catalog_module.CAPABILITY_SCOPES["scene.pose.observe"] == "target_binding"


def test_effectful_requirements_are_explicit_target_commands() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    expected = {
        "execution.reset",
        "execution.runtime_scale.operation_scoped",
        "execution.stop.soft",
        "execution.trajectory.execute",
        "io.gpio.set_output",
        "tool.gripper.command",
    }
    declared = {
        requirement.capability_id
        for entry in catalog.entries
        for requirement in entry.required_capabilities
        if requirement.effectful
    }
    assert declared == expected
    assert all(
        requirement.scope == "target_binding"
        for entry in catalog.entries
        for requirement in entry.required_capabilities
        if requirement.effectful
    )


def test_profile_comparison_has_no_legacy_profile_family() -> None:
    assert profile_satisfied("composable-cell-v1", "composable-cell-v2")
    assert not profile_satisfied("composable-cell-v2", "composable-cell-v1")
    assert not profile_satisfied("v0.1", "v0.2")


def test_catalog_schema_retains_semantic_role_and_validates_defaults() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    move = catalog.get("motion.move_to_pose")
    joint_target = catalog.schema_registry.types["joint_target"]
    target_role = next(parameter for parameter in move.parameters if parameter.id == "target_role")
    planning_profile = next(
        parameter for parameter in move.parameters if parameter.id == "planning_profile"
    )
    joint_name = catalog.schema_registry.resolve(joint_target.properties["joint_name"])

    assert catalog.schema_registry.resolve(target_role.schema).semantic_type == "target_role"
    assert joint_target.required == ("joint_name", "position_rad")
    assert joint_name.semantic_type == "source_joint"
    assert planning_profile.has_default is True
    assert planning_profile.default == "balanced"
    assert (
        validate_value(
            planning_profile.schema,
            "balanced",
            registry=catalog.schema_registry,
        )
        == []
    )

    dynamics = catalog.schema_registry.types["per_joint_dynamics_limits"]
    assert (
        validate_value(
            dynamics,
            {"shoulder_pan_joint": {"max_velocity_rad_s": 1.0}},
            registry=catalog.schema_registry,
        )
        == []
    )
    assert validate_value(
        dynamics,
        {"shoulder_pan_joint": {"max_velocity_rad_s": 0.0}},
        registry=catalog.schema_registry,
    )


def test_reference_frames_name_any_declared_robot_while_effect_frames_stay_scoped() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    registry = catalog.schema_registry
    reference = registry.resolve(registry.types["motion_reference"])
    assert reference.properties["type"].enum_values[-1] == "cell_frame"
    target = registry.resolve(reference.properties["target_role"])
    assert target.semantic_type == "target_role"
    assert dict(target.allowed_when) == {"type": "frame_role"}
    # The unqualified form stays the moving robot's own frame role.
    frame = registry.resolve(reference.properties["frame_role_id"])
    assert frame.semantic_scope_parameter == "target_role"
    # Pick and approach offsets accept every motion-reference form, including
    # the cell frame and another robot's frame: one shared schema.
    assert "pick_offset_reference" not in registry.types
    synthesis = registry.resolve(registry.types["pick_synthesis"])
    for offset in ("pick_offset_reference", "approach_offset_reference"):
        assert synthesis.properties[offset].ref == "#/schema_types/motion_reference"
    move = {parameter.id: parameter for parameter in catalog.get("motion.move_to_pose").parameters}
    assert registry.resolve(move["base_frame_role"].schema).semantic_scope_parameter == "target_role"
    assert move["tcp_endpoint"].required and not move["tcp_endpoint"].has_default
    endpoint = registry.resolve(move["tcp_endpoint"].schema)
    assert endpoint.required == ("target_role", "frame_role")
    assert registry.resolve(endpoint.properties["target_role"]).semantic_type == "target_role"
    assert registry.resolve(endpoint.properties["frame_role"]).semantic_type == "frame_role"
    # Geometry and Scene nodes choose any declared frame.
    offset = {parameter.id: parameter for parameter in catalog.get("geometry.offset_pose").parameters}
    assert offset["offset_frame"].required is False and not offset["offset_frame"].has_default
    assert offset["offset_frame"].schema.ref == "#/schema_types/pose_frame_ref"
    object_pose = {
        parameter.id: parameter for parameter in catalog.get("scene.get_object_pose").parameters
    }
    assert object_pose["frame"].schema.ref == "#/schema_types/pose_frame_ref"
    for node_id, phrase in (
        ("geometry.offset_pose", "in the pose's own frame"),
        ("geometry.check_pose_bounds", "first expressed in frame_ref"),
        ("geometry.check_pose_distance", "Pose B is first expressed in pose A's frame"),
        ("scene.get_object_pose", "the cell frame when absent"),
    ):
        assert phrase in catalog.get(node_id).description


def test_catalog_projects_declared_semantic_scope_for_fields_and_array_items() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    attach = catalog.get("scene.attach_carried_object")
    detach = catalog.get("scene.detach_carried_object")
    parameters = {parameter.id: parameter for parameter in attach.parameters}

    # ``carried`` is the internal owner/API placement kind.  The operator
    # vocabulary remains the historical Attached/Detached workflow.
    assert attach.name == "Attach object"
    assert detach.name == "Detach object"
    assert "Attached" in detach.description
    assert "Detached" in detach.description

    carrier_schema = catalog.schema_registry.to_json_schema(parameters["carrier_frame_role"].schema)
    touch_schema = catalog.schema_registry.to_json_schema(parameters["touch_body_roles"].schema)

    assert carrier_schema["x-manyforge-semantic-scope-parameter"] == "target_role"
    assert touch_schema["items"]["x-manyforge-semantic-scope-parameter"] == ("target_role")


def test_attach_node_grasp_lost_bound_parameters_have_defaults() -> None:
    attach = NodeCatalog(CATALOG_PATH).get("scene.attach_carried_object")
    parameters = {parameter.id: parameter for parameter in attach.parameters}
    for parameter_id, default in (
        ("grasp_lost_translation_m", 0.03),
        ("grasp_lost_rotation_rad", 0.26),
    ):
        parameter = parameters[parameter_id]
        assert parameter.required is False
        assert parameter.has_default is True
        assert parameter.default == default


def test_recomputed_catalog_rejects_unknown_semantic_scope_parameter(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        attach = next(
            entry for entry in data["entries"] if entry["id"] == "scene.attach_carried_object"
        )
        carrier = next(
            parameter
            for parameter in attach["parameters"]
            if parameter["id"] == "carrier_frame_role"
        )
        carrier["schema"]["semantic_scope_parameter"] = "missing_target"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="semantic scope references unknown"):
        NodeCatalog(path)



def test_key_typed_value_parameters_declare_their_key_and_closed_types() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    registry = catalog.schema_registry
    setter = {parameter.id: parameter for parameter in catalog.get("logic.set_value").parameters}
    checker = {parameter.id: parameter for parameter in catalog.get("logic.check_value").parameters}

    assert setter["value"].schema.value_type_from_key == "key"
    assert setter["value"].schema.value_types == (
        "bool",
        "float64",
        "int64",
        "stored_pose",
        "string",
    )
    assert checker["value"].schema.value_types == ("bool", "float64", "int64", "string")
    projected = registry.to_json_schema(setter["value"].schema)
    assert projected["x-manyforge-value-type-from-key"] == "key"
    assert [branch.get("type") for branch in projected["anyOf"]] == [
        "boolean",
        "number",
        "integer",
        "object",
        "string",
    ]
    # Context-free validation is the closed union; the Program narrows it.
    pose = {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [0.0, 0.0, 0.0],
        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }
    for value in (True, 3, 2.5, "text", pose):
        assert validate_value(setter["value"].schema, value, registry=registry) == []
    for value in ({"x": 1.0}, [1, 2], None, float("inf")):
        assert validate_value(setter["value"].schema, value, registry=registry)
    assert validate_value(checker["value"].schema, pose, registry=registry)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (
            lambda value, entry: value["schema"].update(value_types=["bool", "int64"]),
            "value_types must equal the blackboard binding types of 'key'",
        ),
        (
            lambda value, entry: value["schema"].update(value_type_from_key="missing"),
            "references unknown parameter 'missing'",
        ),
        (
            lambda value, entry: value.update(default=True),
            "typed by a key and cannot declare a default",
        ),
        (
            lambda value, entry: value["schema"].update(value_types=["stored_pose", "bool"]),
            "value_types must be sorted",
        ),
        (
            lambda value, entry: value["schema"].update(value_types=["model_response"]),
            "value_types must be a non-empty subset",
        ),
        (
            lambda value, entry: value.update(
                schema={"type": "bool", "value_type_from_key": "key"}
            ),
            "require type=key_value",
        ),
        (
            lambda value, entry: value.update(
                schema={
                    "type": "object",
                    "properties": {"nested": dict(value["schema"])},
                }
            ),
            "nests a key-typed value",
        ),
        (
            lambda value, entry: value["schema"].pop("value_type_from_key"),
            "key_value parameter needs value_type_from_key",
        ),
    ],
)
def test_recomputed_catalog_rejects_inconsistent_key_typed_declarations(
    tmp_path: Path, mutation, message
) -> None:
    def mutate(data: dict[str, object]) -> None:
        entry = next(entry for entry in data["entries"] if entry["id"] == "logic.set_value")
        value = next(parameter for parameter in entry["parameters"] if parameter["id"] == "value")
        mutation(value, entry)

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match=message):
        NodeCatalog(path)

def test_boolean_logic_is_one_condition_check_over_typed_values() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    ids = {entry.id for entry in catalog.entries}

    assert not ids & {"logic.check_bool", "logic.set_bool", "logic.wait_for_bool"}
    condition = catalog.get("logic.check_condition")
    assert (condition.name, condition.category) == ("Check condition", "logic")
    assert [(parameter.id, parameter.required) for parameter in condition.parameters] == [
        ("condition", True)
    ]
    assert condition.parameters[0].schema.schema_type == "bool"
    assert condition.execution.owner_api == "orchestration.blackboard"
    assert condition.runner_implementation_ids == {"py_trees": "logic_check_condition_v1"}


def test_check_input_reads_one_input_like_get_input() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    check = catalog.get("io.check_input")
    get = catalog.get("io.get_input")

    assert (check.name, check.category) == ("Check input", "io")
    assert [parameter.id for parameter in check.parameters] == [
        "target_role",
        "resource_role",
        "expected_value",
        "mock_simulation",
    ]
    assert check.parameters[2].schema.schema_type == "bool"
    assert check.required_capabilities == get.required_capabilities
    assert check.execution.owner_api == get.execution.owner_api
    assert not check.blackboard_writes


def test_query_model_template_variables_are_a_map_of_typed_values() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    registry = catalog.schema_registry
    parameter = next(
        parameter
        for parameter in catalog.get("assistant.query_model").parameters
        if parameter.id == "template_variables"
    )
    schema = registry.resolve(parameter.schema)

    assert {"value_source", "template_variable_binding"}.isdisjoint(registry.types)
    assert parameter.default == {}
    assert schema.schema_type == "object" and not schema.properties
    entry = schema.additional_property_schema
    assert entry.schema_type == "key_value" and entry.value_type_from_key is None
    assert entry.value_types == ("bool", "float64", "int64", "string", "string_list")
    for value in ({}, {"text": "a", "ids": ["a", "b"], "count": 2, "flag": True}):
        assert validate_value(parameter.schema, value, registry=registry) == []
    for value in ([], {"pose": {"x": 1.0}}, {"ids": ["a", 1]}):
        assert validate_value(parameter.schema, value, registry=registry)


def test_catalog_preserves_assistant_and_planning_only_pick_place_contracts() -> None:
    catalog = NodeCatalog(CATALOG_PATH)

    query = catalog.get("assistant.query_model")
    assert {
        "model_endpoint_alias",
        "prompt_template_id",
        "template_variables",
        "scene_attachment",
        "output_key",
        "timeout_s",
        "min_confidence",
    } <= {parameter.id for parameter in query.parameters}

    for node_id, owner_api in (
        ("planning.plan_pick_sequence", "planning.pick_sequence"),
        ("planning.plan_place_sequence", "planning.place_sequence"),
    ):
        preview = catalog.get(node_id)
        assert preview.execution.owner_api == owner_api
        assert preview.blackboard_writes[0].value_type == "pick_planning_result"
        selected_object = next(
            parameter
            for parameter in preview.parameters
            if parameter.id == "selected_object_id"
        )
        assert (
            catalog.schema_registry.resolve(selected_object.schema).semantic_type
            == "scene_resource"
        )

    with pytest.raises(KeyError):
        catalog.get("motion.pick_object")
    with pytest.raises(KeyError):
        catalog.get("motion.place_object")

    with pytest.raises(KeyError):
        catalog.get("motion.invalidate_cached_plans")


@pytest.mark.parametrize(
    "api_version",
    ["manyforge.node_catalog.v0", "manyforge.node_catalog.v1"],
)
def test_old_catalog_apis_are_not_runtime_inputs(
    tmp_path: Path,
    api_version: str,
) -> None:
    path = _write_recomputed(
        tmp_path,
        lambda data: data.__setitem__("api_version", api_version),
    )

    with pytest.raises(
        CatalogValidationError,
        match="api_version must be 'manyforge.node_catalog.v2'",
    ):
        NodeCatalog(path)


def test_catalog_digest_is_verified_over_rfc8785_content(tmp_path: Path) -> None:
    data = _catalog_data()
    data["entries"][0]["name"] = "Changed without digest"
    path = tmp_path / "catalog.yaml"
    path.write_text(json.dumps(data), encoding="utf-8")

    with pytest.raises(CatalogValidationError, match="digest mismatch"):
        NodeCatalog(path)


def test_recomputed_packet_change_cannot_reuse_frozen_content_version(
    tmp_path: Path,
) -> None:
    path = _write_recomputed(
        tmp_path,
        lambda data: data["entries"][0].__setitem__(
            "name",
            "Semantically valid but not the frozen packet",
        ),
    )

    with pytest.raises(
        CatalogValidationError,
        match="does not match the frozen built-in",
    ):
        NodeCatalog(path)


def test_strict_loader_rejects_duplicate_keys(tmp_path: Path) -> None:
    path = tmp_path / "duplicate.yaml"
    path.write_text(
        "api_version: manyforge.node_catalog.v2\n" "api_version: manyforge.node_catalog.v2\n",
        encoding="utf-8",
    )

    with pytest.raises(CatalogValidationError, match="duplicate mapping key"):
        NodeCatalog(path)


def test_strict_loader_rejects_excessive_nesting(tmp_path: Path) -> None:
    path = tmp_path / "deep.yaml"
    path.write_text(("[" * 140) + "null" + ("]" * 140), encoding="utf-8")

    with pytest.raises(CatalogValidationError, match="nesting exceeds"):
        NodeCatalog(path)


def test_recomputed_catalog_cannot_add_runner_import_metadata(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        data["entries"][0]["runners"]["py_trees"]["module"] = "unsafe.module"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="unknown fields.*module"):
        NodeCatalog(path)


def test_distinct_nodes_may_share_one_registered_implementation(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        data["entries"][1]["runners"]["py_trees"]["implementation_id"] = data["entries"][0][
            "runners"
        ]["py_trees"]["implementation_id"]

    path = _write_recomputed(tmp_path, mutate)
    digest = json.loads(path.read_text(encoding="utf-8"))["catalog_digest"]
    monkeypatch.setattr(
        catalog_module,
        "EXPECTED_BUILTIN_CATALOG_DIGEST",
        digest,
    )
    catalog = NodeCatalog(path)
    registry = build_node_registry(catalog)
    node_ids = [
        entry["id"] for entry in json.loads(path.read_text(encoding="utf-8"))["entries"][:2]
    ]

    assert (
        registry.record(node_ids[0]).implementation_id
        == registry.record(node_ids[1]).implementation_id
    )


def test_recomputed_catalog_rejects_wrong_capability_scope(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        move = next(entry for entry in data["entries"] if entry["id"] == "motion.move_to_pose")
        move["required_capabilities"][0]["scope"] = "cell_global"
        move["required_capabilities"][0].pop("target_parameter")

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="scope must be 'target_binding'"):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_effectful_non_target_requirement(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        move = next(entry for entry in data["entries"] if entry["id"] == "motion.move_to_pose")
        collision = next(
            requirement
            for requirement in move["required_capabilities"]
            if requirement["capability_id"] == "collision.query.pre_execution"
        )
        collision["effectful"] = True

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="valid only for target_binding"):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_source_joint_without_one_target_scope(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        move = next(
            entry for entry in data["entries"] if entry["id"] == "motion.move_to_joint_target"
        )
        target = next(
            parameter for parameter in move["parameters"] if parameter["id"] == "target_role"
        )
        target["schema"]["semantic_type"] = "component_role"
        # Frame roles are scoped by target_role; unscope them so this
        # mutation reaches the source_joint rule under test.
        for parameter in move["parameters"]:
            parameter.get("schema", {}).pop("semantic_scope_parameter", None)

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="source_joint semantics require exactly one target_role parameter",
    ):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_runtime_identity_parameter(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        timer = next(entry for entry in data["entries"] if entry["id"] == "timer")
        timer["parameters"][0]["id"] = "controller_name"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="forbidden runtime identity",
    ):
        NodeCatalog(path)


def test_cancellable_leaves_declare_interrupted_and_others_never_do() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    leaves = [entry for entry in catalog.entries if entry.kind == "leaf"]

    assert leaves
    for entry in leaves:
        # The timer's own work cannot fail: it succeeds or is interrupted.
        expected = ({"success"} if entry.id == "timer" else {"success", "failure"}) | (
            {"interrupted"} if entry.execution.cancellable else set()
        )
        assert set(entry.outcomes) == expected, entry.id
    assert catalog.get("gripper.wait_for_status").outcomes["interrupted"] == "cancelled"
    assert dict(catalog.get("timer").outcomes) == {
        "success": "elapsed",
        "interrupted": "cancelled",
    }


def test_recomputed_catalog_requires_a_leaf_success_outcome(tmp_path: Path) -> None:
    def mutate(data: dict[str, object]) -> None:
        entry = next(row for row in data["entries"] if row["id"] == "timer")
        entry["outcomes"] = {"failure": "timer_failed", "interrupted": "cancelled"}

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match=r"outcomes is missing required fields \['success'\]",
    ):
        NodeCatalog(path)


@pytest.mark.parametrize("node_id", ["gripper.wait_for_status", "timer"])
@pytest.mark.parametrize("mutation", ["omit_interrupted", "omit_outcomes"])
def test_recomputed_catalog_rejects_cancellable_leaf_without_interrupted(
    tmp_path: Path, node_id: str, mutation: str
) -> None:
    def mutate(data: dict[str, object]) -> None:
        entry = next(row for row in data["entries"] if row["id"] == node_id)
        if mutation == "omit_outcomes":
            entry.pop("outcomes")
        else:
            entry["outcomes"].pop("interrupted")

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match=r"outcomes is missing required fields \[.*'interrupted'.*\]",
    ):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_interrupted_on_non_cancellable_leaf(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        entry = next(row for row in data["entries"] if row["id"] == "gripper.check_status")
        assert entry["execution"]["cancellable"] is False
        entry["outcomes"]["interrupted"] = "cancelled"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match=r"outcomes has unknown fields \['interrupted'\]",
    ):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_authoring_schema_as_persisted_parameter(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        timer = next(entry for entry in data["entries"] if entry["id"] == "timer")
        timer["parameters"][0].pop("schema")
        timer["parameters"][0]["schema_ref"] = "#/schema_types/pose_authoring_input"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="authoring-only schema"):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_blackboard_control_protocol(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        getter = next(entry for entry in data["entries"] if entry["id"] == "io.get_input")
        getter["parameters"][2]["id"] = "stop_requested"
        getter["blackboard"]["writes"][0]["key_parameter"] = "stop_requested"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="forbidden execution-control blackboard protocol",
    ):
        NodeCatalog(path)


def test_legacy_catalog_model_fields_are_removed() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    parameter = catalog.get("timer").parameters[0]

    assert not hasattr(catalog, "version")
    assert not hasattr(parameter, "name")
    assert not hasattr(parameter, "type")
    assert not hasattr(parameter, "enum_values")
    assert not hasattr(catalog.get("timer"), "runtime_injections")


def test_parsed_schema_maps_are_deeply_read_only() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    stored_pose = catalog.schema_registry.types["stored_pose"]

    with pytest.raises(TypeError):
        catalog.schema_registry.types["forged"] = stored_pose  # type: ignore[index]
    with pytest.raises(TypeError):
        stored_pose.properties["forged"] = stored_pose  # type: ignore[index]

    shape = catalog.schema_registry.types["shape"]
    box_dimensions = shape.properties["box_dimensions_m"]
    with pytest.raises(TypeError):
        box_dimensions.required_when["type"] = "mesh"  # type: ignore[index]

    timer = catalog.get("timer")
    with pytest.raises(TypeError):
        timer.outcomes["success"] = "forged"  # type: ignore[index]


def test_nested_parameter_defaults_are_transitively_read_only(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        add = next(entry for entry in data["entries"] if entry["id"] == "scene.add_object")
        shape = next(parameter for parameter in add["parameters"] if parameter["id"] == "shape")
        shape["default"] = {
            "type": "box",
            "box_dimensions_m": [1.0, 2.0, 3.0],
        }

    path = _write_recomputed(tmp_path, mutate)
    digest = json.loads(path.read_text(encoding="utf-8"))["catalog_digest"]
    monkeypatch.setattr(
        catalog_module,
        "EXPECTED_BUILTIN_CATALOG_DIGEST",
        digest,
    )
    catalog = NodeCatalog(path)
    default = next(
        parameter.default
        for parameter in catalog.get("scene.add_object").parameters
        if parameter.id == "shape"
    )

    with pytest.raises(TypeError):
        default["type"] = "sphere"
    with pytest.raises(TypeError):
        default["box_dimensions_m"][0] = 99.0


def test_joint_goal_exposes_the_same_explicit_contact_inputs_as_pose_goal() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    joint = {row.id: row for row in catalog.get('motion.move_to_joint_target').parameters}
    pose = {row.id: row for row in catalog.get('motion.move_to_pose').parameters}
    for name in ('contact_policy_id', 'contact_scene_resource_id'):
        assert joint[name].required is False
        assert joint[name].has_default is False
        assert joint[name].schema == pose[name].schema
