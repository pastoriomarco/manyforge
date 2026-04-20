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
        assert len(move.parameters) > 0
        move_id_param = next(p for p in move.parameters if p.name == "move_id")
        assert move_id_param.required is True

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
        assert len(v0_entries) == len(catalog.entries)  # all are min v0.0

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
