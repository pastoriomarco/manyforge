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

CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"


class TestCatalogLoading:
    def test_loads_shipped_catalog(self):
        catalog = NodeCatalog(CATALOG_PATH)
        assert len(catalog.entries) > 0
        assert catalog.version == "0.1.0"

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
        # v0.2 entries: motion-scoped trip nodes
        assert "move_until_signal" not in v0_ids
        assert "probe_search_move" not in v0_ids
        assert len(v0_entries) == len(catalog.entries) - 6

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

    def test_rejects_bad_api_version(self):
        path = self._write_catalog({
            "api_version": "wrong.version",
            "catalog_version": "0.1.0",
            "entries": [],
        })
        with pytest.raises(CatalogValidationError, match="api_version"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_bad_catalog_version(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "not-semver",
            "entries": [],
        })
        with pytest.raises(CatalogValidationError, match="catalog_version"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_duplicate_ids(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bad_enum",
                    "name": "BadEnum",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "mode", "type": "enum", "required": True},
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "enum_options_node",
                    "name": "EnumOptionsNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "type": "enum",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bad_enum_options",
                    "name": "BadEnumOptions",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "type": "enum",
                            "required": True,
                            "enum_values": ["a", "b"],
                            "enum_options": [{"value": "a"}, {"value": "c"}],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="enum_values must match"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_duplicate_enum_options(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bad_duplicate_enum_options",
                    "name": "BadDuplicateEnumOptions",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "type": "enum",
                            "required": True,
                            "enum_options": [{"value": "a"}, {"value": "a"}],
                        },
                    ],
                    "runners": {"py_trees": {"implementation_kind": "plugin"}},
                },
            ],
        })
        with pytest.raises(CatalogValidationError, match="duplicate"):
            NodeCatalog(path)
        os.unlink(path)

    def test_rejects_unknown_enum_option_required_capability(self):
        path = self._write_catalog({
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bad_enum_option_capability",
                    "name": "BadEnumOptionCapability",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "mode",
                            "type": "enum",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bad_visible_when",
                    "name": "BadVisibleWhen",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {
                            "name": "value",
                            "type": "string",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bb_bad",
                    "name": "BadBlackboardNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "known", "type": "string", "required": True},
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
            "entries": [
                {
                    "id": "bb_dup",
                    "name": "DuplicateBlackboardNode",
                    "kind": "leaf",
                    "children": {"min": 0, "max": 0},
                    "parameters": [
                        {"name": "key", "type": "string", "required": True},
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
            "api_version": "manyforge.node_catalog.v0",
            "catalog_version": "0.1.0",
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
