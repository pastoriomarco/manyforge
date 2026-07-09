"""Tests for the node catalog loader and validator."""

import os
import tempfile
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.catalog import (
    CatalogValidationError,
    NodeCatalog,
)
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.catalog_schema import CatalogSchemaRegistry, parse_schema

CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"


class TestCatalogLoading:
    def test_loads_shipped_catalog(self):
        catalog = NodeCatalog(CATALOG_PATH)
        assert len(catalog.entries) > 0
        assert catalog.version == "0.2.0"
        assert catalog.api_version == "manyforge.node_catalog.v1"
        assert all(
            param.schema is not None
            for entry in catalog.entries
            for param in entry.parameters
        )

    def test_all_entries_have_ids(self):
        catalog = NodeCatalog(CATALOG_PATH)
        for entry in catalog.entries:
            assert entry.id
            assert entry.name

    def test_get_sequence(self):
        catalog = NodeCatalog(CATALOG_PATH)
        seq = catalog.get("sequence")
        assert seq.name == "Sequence"
        assert seq.kind == "composite"
        assert seq.children_min == 1
        assert seq.children_max == -1  # unbounded
        assert "py_trees" in seq.runners

    def test_parallel_exposes_success_policy_without_failure_thresholds(self):
        catalog = NodeCatalog(CATALOG_PATH)
        parallel = catalog.get("parallel")
        success_policy = next(
            param for param in parallel.parameters if param.name == "success_policy"
        )

        assert success_policy.default == "success_on_all"
        assert success_policy.profiles_min == "v0.1"
        assert success_policy.enum_values == ["success_on_all", "success_on_one"]
        assert "fail-on-any-child-failure" in success_policy.description

    def test_get_move_manipulator(self):
        catalog = NodeCatalog(CATALOG_PATH)
        move = catalog.get("move_manipulator_action")
        assert move.kind == "leaf"
        assert move.children_max == 0
        assert move.stability == "stable"
        assert len(move.parameters) > 0
        move_id_param = next(p for p in move.parameters if p.name == "move_id")
        assert move_id_param.required is True
        motion_type_param = next(p for p in move.parameters if p.name == "motion_type")
        relative_move_option = next(
            option
            for option in motion_type_param.enum_options
            if option.value == "relative_move"
        )
        assert relative_move_option.profiles_min == "v0.1"
        assert relative_move_option.required_capabilities == [
            "context.pose.resolve_reference"
        ]
        relative_move_param = next(p for p in move.parameters if p.name == "relative_move")
        assert relative_move_param.profiles_min == "v0.1"
        assert relative_move_param.visible_when is not None
        assert relative_move_param.visible_when.param == "motion_type"
        assert relative_move_param.visible_when.equals == "relative_move"

    def test_blackboard_metadata_loaded_for_shipped_entries(self):
        catalog = NodeCatalog(CATALOG_PATH)
        set_key = catalog.get("set_key_bool_value")
        check_pose = catalog.get("check_pose_distance")

        assert set_key.bb_writer_params == ["key"]
        assert set_key.bb_reader_params == []
        assert check_pose.bb_reader_params == ["pose_key_a", "pose_key_b"]
        assert check_pose.bb_writer_params == []

    def test_shipped_catalog_open_object_params_are_intentional(self):
        catalog = NodeCatalog(CATALOG_PATH)
        open_params = []
        for entry in catalog.entries:
            for param in entry.parameters:
                if param.schema is None:
                    continue
                resolved = catalog.schema_registry.resolve(param.schema)
                if (
                    resolved.schema_type == "object"
                    and resolved.additional_properties is True
                    and resolved.additional_property_schema is None
                ):
                    open_params.append(f"{entry.id}.{param.name}")

        assert open_params == [
            "fallback_override_node.fallback_params",
            "recovery_skill_node.recovery_params",
        ]

    def test_interrupt_metadata_loaded_for_shipped_entries(self):
        catalog = NodeCatalog(CATALOG_PATH)
        move = catalog.get("move_manipulator_action")
        wait_signal = catalog.get("wait_for_signal_edge")

        assert move.interruptible is True
        assert move.default_interrupt_scope == "kernel_motion"
        assert wait_signal.interruptible is True
        assert wait_signal.default_interrupt_scope == "active_node"

    def test_get_nonexistent_raises(self):
        catalog = NodeCatalog(CATALOG_PATH)
        with pytest.raises(KeyError):
            catalog.get("nonexistent_node_xyz")

    def test_filter_by_profile_v0(self):
        catalog = NodeCatalog(CATALOG_PATH)
        v0_entries = catalog.filter_by_profile("v0.0")
        assert all(entry.profiles_min == "v0.0" for entry in v0_entries)
        v0_ids = {entry.id for entry in v0_entries}
        # v0.1 entries (post-v0.0)
        assert "command_gripper" not in v0_ids
        assert "upsert_collision_object" not in v0_ids
        assert "attach_object_to_link" not in v0_ids
        assert "detach_object_from_link" not in v0_ids
        assert "reset_simulation" not in v0_ids
        assert "set_simulation_state" not in v0_ids
        assert "step_simulation" not in v0_ids
        assert "set_entity_state" not in v0_ids
        # v0.2 entries: motion-scoped trip nodes
        assert "move_until_signal" not in v0_ids
        assert "probe_search_move" not in v0_ids
        assert "confirm_gripper_grasp" not in v0_ids
        assert len(v0_entries) == len(catalog.entries) - 11

    def test_command_gripper_is_marked_experimental(self):
        catalog = NodeCatalog(CATALOG_PATH)
        command_gripper = catalog.get("command_gripper")
        assert command_gripper.stability == "experimental"

    def test_all_entries_have_py_trees_runner(self):
        catalog = NodeCatalog(CATALOG_PATH)
        for entry in catalog.entries:
            assert "py_trees" in entry.runners, (
                f"Entry '{entry.id}' missing py_trees runner"
            )


class TestCatalogValidation:
    def _write_catalog(self, data: dict) -> str:
        f = tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", delete=False
        )
        yaml.dump(data, f)
        f.close()
        return f.name

    def _minimal_v1_catalog(self) -> dict:
        return {
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "schema_types": {
                "stored_pose": {
                    "type": "object",
                    "additional_properties": False,
                    "required": ["frame_id", "position_m"],
                    "properties": {
                        "frame_id": {"type": "string"},
                        "position_m": {"type": "array", "items": "float", "length": 3},
                        "orientation_xyzw": {"type": "array", "items": "float", "length": 4},
                        "orientation_rpy_deg": {"type": "array", "items": "float", "length": 3},
                    },
                    "exactly_one": [["orientation_xyzw", "orientation_rpy_deg"]],
                },
                "shape": {
                    "type": "object",
                    "additional_properties": False,
                    "required": ["type"],
                    "properties": {
                        "type": {"type": "enum", "values": ["box", "sphere"]},
                        "box_dimensions_m": {
                            "type": "array",
                            "items": "float",
                            "length": 3,
                            "required_when": {"type": "box"},
                            "allowed_when": {"type": "box"},
                        },
                        "sphere_radius_m": {
                            "type": "float",
                            "constraints": {"min_exclusive": 0.0},
                            "required_when": {"type": "sphere"},
                            "allowed_when": {"type": "sphere"},
                        },
                    },
                },
            },
            "entries": [
                {
                    "id": "upsert_collision_object",
                    "name": "UpsertCollisionObject",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "object_id",
                            "required": True,
                            "schema": {"type": "string"},
                        },
                        {
                            "name": "pose",
                            "required": True,
                            "schema_ref": "#/schema_types/stored_pose",
                            "ui_editor": "pose",
                        },
                        {
                            "name": "shape",
                            "required": True,
                            "schema_ref": "#/schema_types/shape",
                            "ui_editor": "shape",
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        }

    def test_loads_v1_schema_catalog_and_derives_legacy_labels(self):
        path = self._write_catalog(self._minimal_v1_catalog())
        try:
            catalog = NodeCatalog(path)
            assert catalog.api_version == "manyforge.node_catalog.v1"
            assert catalog.version == "0.2.0"
            entry = catalog.get("upsert_collision_object")
            pose_param = next(param for param in entry.parameters if param.name == "pose")
            shape_param = next(param for param in entry.parameters if param.name == "shape")
            assert pose_param.schema is not None
            assert pose_param.type == "dict"
            assert shape_param.type == "dict"
            projected = catalog.schema_registry.to_json_schema(shape_param.schema)
            assert projected["type"] == "object"
            assert projected["additionalProperties"] is False
            assert "allOf" in projected
        finally:
            os.unlink(path)

    def test_v1_rejects_legacy_parameter_keys(self):
        data = self._minimal_v1_catalog()
        data["entries"][0]["parameters"][0]["type"] = "string"
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="remove legacy keys"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_rejects_non_boolean_required_flag(self):
        data = self._minimal_v1_catalog()
        data["entries"][0]["parameters"][0]["required"] = "false - stray"
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="required must be a boolean"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_schema_hash_preserves_description_property_name(self):
        registry = CatalogSchemaRegistry()
        empty_schema = parse_schema(
            {
                "type": "object",
                "additional_properties": False,
                "properties": {},
            },
            path="empty",
        )
        description_string_schema = parse_schema(
            {
                "type": "object",
                "additional_properties": False,
                "properties": {
                    "description": {"type": "string", "description": "prose"},
                },
            },
            path="with_description",
        )
        description_int_schema = parse_schema(
            {
                "type": "object",
                "additional_properties": False,
                "properties": {
                    "description": {"type": "int", "description": "prose"},
                },
            },
            path="with_description",
        )

        assert registry.schema_hash(empty_schema) != registry.schema_hash(
            description_string_schema
        )
        assert registry.schema_hash(description_string_schema) != registry.schema_hash(
            description_int_schema
        )

    def test_v1_schema_validator_rejects_unknown_and_missing_pose_fields(self):
        path = self._write_catalog(self._minimal_v1_catalog())
        try:
            catalog = NodeCatalog(path)
            pose_schema = catalog.get("upsert_collision_object").parameters[1].schema
            assert pose_schema is not None
            issues = validate_value(
                pose_schema,
                {
                    "frame_id": "base_link",
                    "position_m": [0.0, 0.0, 0.0],
                    "unexpected": True,
                },
                registry=catalog.schema_registry,
                path="params.pose",
            )
            messages = [issue.message for issue in issues]
            assert any("unknown fields" in message for message in messages)
            assert any("exactly one" in message for message in messages)
        finally:
            os.unlink(path)

    def test_v1_requires_parameter_schema(self):
        data = self._minimal_v1_catalog()
        del data["entries"][0]["parameters"][0]["schema"]
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="v1 parameters require schema"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_v1_rejects_invalid_schema_ref(self):
        data = self._minimal_v1_catalog()
        data["entries"][0]["parameters"][1]["schema_ref"] = "#/schema_types/missing"
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="Unknown schema reference"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_v1_rejects_invalid_schema_default(self):
        data = self._minimal_v1_catalog()
        data["entries"][0]["parameters"][0]["default"] = 123
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="default.*must be a string"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_v1_rejects_unknown_schema_keys(self):
        data = self._minimal_v1_catalog()
        data["schema_types"]["stored_pose"]["additionalProperties"] = False
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="unsupported schema keys"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_v1_rejects_invalid_allowed_when_ref(self):
        data = self._minimal_v1_catalog()
        data["schema_types"]["shape"]["properties"]["sphere_radius_m"]["allowed_when"] = {
            "missing": "sphere",
        }
        path = self._write_catalog(data)
        try:
            with pytest.raises(CatalogValidationError, match="allowed_when references unknown field"):
                NodeCatalog(path)
        finally:
            os.unlink(path)

    def test_v1_schema_validator_rejects_disallowed_conditional_fields(self):
        path = self._write_catalog(self._minimal_v1_catalog())
        try:
            catalog = NodeCatalog(path)
            shape_schema = catalog.get("upsert_collision_object").parameters[2].schema
            assert shape_schema is not None
            issues = validate_value(
                shape_schema,
                {
                    "type": "box",
                    "box_dimensions_m": [0.1, 0.2, 0.3],
                    "sphere_radius_m": 0.05,
                },
                registry=catalog.schema_registry,
                path="params.shape",
            )
            assert any(
                issue.path == "params.shape.sphere_radius_m"
                and "only allowed when type=sphere" in issue.message
                for issue in issues
            )
            projected = catalog.schema_registry.to_json_schema(shape_schema)
            assert any(
                branch.get("then") == {"not": {"required": ["sphere_radius_m"]}}
                for branch in projected.get("allOf", [])
            )
        finally:
            os.unlink(path)

    def test_v1_schema_validator_checks_additional_property_schema(self):
        data = self._minimal_v1_catalog()
        data["entries"][0]["parameters"].append(
            {
                "name": "per_joint_limits",
                "required": False,
                "schema": {
                    "type": "object",
                    "additional_properties": {
                        "type": "float",
                        "constraints": {"min_exclusive": 0.0},
                    },
                },
            }
        )
        path = self._write_catalog(data)
        try:
            catalog = NodeCatalog(path)
            schema = catalog.get("upsert_collision_object").parameters[3].schema
            assert schema is not None
            issues = validate_value(
                schema,
                {"joint_1": 1.0, "joint_2": -0.1},
                registry=catalog.schema_registry,
                path="params.per_joint_limits",
            )
            assert any(
                issue.path == "params.per_joint_limits.joint_2"
                and "must be > 0.0" in issue.message
                for issue in issues
            )
            projected = catalog.schema_registry.to_json_schema(schema)
            assert projected["additionalProperties"] == {
                "type": "number",
                "exclusiveMinimum": 0.0,
            }
        finally:
            os.unlink(path)

    def test_rejects_bad_api_version(self):
        path = self._write_catalog({
            "api_version": "wrong.version",
            "catalog_version": "0.2.0",
            "entries": [],
        })
        with pytest.raises(CatalogValidationError, match="api_version"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_v0_runtime_catalog(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [],
        })
        with pytest.raises(CatalogValidationError, match="api_version"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_bad_catalog_version(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "not-semver",
            "entries": [],
        })
        with pytest.raises(CatalogValidationError, match="catalog_version"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_duplicate_ids(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "dup",
                    "name": "Dup1",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
                {
                    "id": "dup",
                    "name": "Dup2",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="Duplicate"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_leaf_with_children(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_leaf",
                    "name": "BadLeaf",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 5},
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="leaf"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_decorator_wrong_children(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_dec",
                    "name": "BadDec",
                    "kind": "decorator",
                    "children": {"min": 0, "max": 3},
                    "runners": {"py_trees": {"implementation_kind": "native"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="decorator"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_enum_without_values(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_enum",
                    "name": "BadEnum",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "mode", "schema": {"type": "enum"}, "required": True},
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="enum"):
            NodeCatalog(path)
        os.unlink(path)

    def test_derives_enum_values_from_enum_options(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "enum_options_node",
                    "name": "EnumOptionsNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "schema": {"type": "enum", "values": ["a", "b"]},
                            "required": True,
                            "enum_options": [
                                {"value": "a"},
                                {"value": "b", "profiles": {"min_profile": "v0.1"}},
                            ],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        try:
            param = NodeCatalog(path).get("enum_options_node").parameters[0]
            assert param.enum_values == ["a", "b"]
            assert [option.value for option in param.enum_options] == ["a", "b"]
            assert param.enum_options[1].profiles_min == "v0.1"
        finally:
            os.unlink(path)

    def test_rejects_enum_values_that_do_not_match_enum_options(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_enum_options",
                    "name": "BadEnumOptions",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "schema": {"type": "enum", "values": ["a", "b"]},
                            "required": True,
                            "enum_options": [{"value": "a"}, {"value": "c"}],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="enum_options values must match"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_duplicate_enum_options(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_duplicate_enum_options",
                    "name": "BadDuplicateEnumOptions",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "schema": {"type": "enum", "values": ["a", "b"]},
                            "required": True,
                            "enum_options": [{"value": "a"}, {"value": "a"}],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="enum_options values must match"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_unknown_enum_option_required_capability(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_enum_option_capability",
                    "name": "BadEnumOptionCapability",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "schema": {"type": "enum", "values": ["a"]},
                            "required": True,
                            "enum_options": [
                                {
                                    "value": "a",
                                    "required_capabilities": ["unknown.capability.key"],
                                },
                            ],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="unknown required_capabilities"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_visible_when_unknown_parameter(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_visible_when",
                    "name": "BadVisibleWhen",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "value",
                            "schema": {"type": "string"},
                            "required": False,
                            "visible_when": {"param": "missing", "equals": "x"},
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="visible_when.param"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_missing_py_trees_runner(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "no_runner",
                    "name": "NoRunner",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "runners": {"btcpp": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="py_trees"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_unknown_required_capability(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_capability",
                    "name": "BadCapability",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "required_capabilities": ["unknown.capability.key"],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="unknown required_capabilities"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_unknown_stability(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bad_stability",
                    "name": "BadStability",
                    "stability": "preview",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="stability"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_blackboard_metadata_for_unknown_parameter(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bb_bad",
                    "name": "BadBlackboardNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "known", "schema": {"type": "string"}, "required": True},
                    ],
                    "bb_reader_params": ["missing"],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="unknown parameter 'missing'"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_duplicate_blackboard_metadata_names(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "bb_dup",
                    "name": "DuplicateBlackboardNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "key", "schema": {"type": "string"}, "required": True},
                    ],
                    "bb_writer_params": ["key", "key"],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="duplicate parameter names"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_default_interrupt_scope_without_interruptible(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v1",
            "catalog_version": "0.2.0",
            "entries": [
                {
                    "id": "interrupt_bad",
                    "name": "InterruptBad",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "default_interrupt_scope": "active_node",
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="requires interruptible=true"):
            NodeCatalog(path)
        os.unlink(path)
