"""Tests for program artifact loading and validation."""

import os
import tempfile
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    TreeNode,
    apply_tree_param_overrides,
    compute_program_tree_hash,
)
from manyforge_behavior.skill_loader import SkillCatalog

CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
FIXTURE_PATH = Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"


@pytest.fixture
def catalog():
    return NodeCatalog(CATALOG_PATH)


def _write_program(data: dict) -> str:
    f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.dump(data, f)
    f.close()
    return f.name


def _write_catalog(data: dict) -> str:
    f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.dump(data, f, sort_keys=False)
    f.close()
    return f.name


def _minimal_v1_catalog() -> dict:
    return {
        "api_version": "manyforge.node_catalog.v1",
        "catalog_version": "0.2.0",
        "schema_types": {
            "stored_pose": {
                "type": "object",
                "additional_properties": False,
                "required": ["frame_id", "position_m"],
                "exactly_one": [["orientation_xyzw", "orientation_rpy_deg"]],
                "properties": {
                    "frame_id": {"type": "string"},
                    "position_m": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                    "orientation_xyzw": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 4,
                    },
                    "orientation_rpy_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                },
            },
            "shape": {
                "type": "object",
                "additional_properties": False,
                "required": ["type"],
                "properties": {
                    "type": {
                        "type": "enum",
                        "values": ["box", "sphere", "cylinder", "mesh"],
                    },
                    "box_dimensions_m": {
                        "type": "array",
                        "items": {
                            "type": "float",
                            "constraints": {"min_exclusive": 0},
                        },
                        "length": 3,
                        "required_when": {"type": "box"},
                        "allowed_when": {"type": "box"},
                    },
                    "sphere_radius_m": {
                        "type": "float",
                        "constraints": {"min_exclusive": 0},
                        "required_when": {"type": "sphere"},
                        "allowed_when": {"type": "sphere"},
                    },
                    "cylinder_radius_m": {
                        "type": "float",
                        "constraints": {"min_exclusive": 0},
                        "required_when": {"type": "cylinder"},
                        "allowed_when": {"type": "cylinder"},
                    },
                    "cylinder_height_m": {
                        "type": "float",
                        "constraints": {"min_exclusive": 0},
                        "required_when": {"type": "cylinder"},
                        "allowed_when": {"type": "cylinder"},
                    },
                    "mesh_resource_uri": {
                        "type": "string",
                        "required_when": {"type": "mesh"},
                        "allowed_when": {"type": "mesh"},
                    },
                    "mesh_scale": {
                        "type": "array",
                        "items": {
                            "type": "float",
                            "constraints": {"min_exclusive": 0},
                        },
                        "length": 3,
                        "allowed_when": {"type": "mesh"},
                    },
                },
            },
        },
        "entries": [
            {
                "id": "sequence",
                "name": "Sequence",
                "kind": "composite",
                "children": {"min": 0, "max": "unbounded"},
                "parameters": [],
                "runners": {"py_trees": {"class": "py_trees.composites.Sequence"}},
            },
            {
                "id": "upsert_collision_object",
                "name": "Upsert Collision Object",
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
                    },
                    {
                        "name": "shape",
                        "required": True,
                        "schema_ref": "#/schema_types/shape",
                    },
                ],
                "runners": {
                    "py_trees": {
                        "class": "manyforge_behavior.nodes.scene.UpsertCollisionObject",
                    }
                },
            },
        ],
    }


def _load_minimal_v1_catalog() -> NodeCatalog:
    path = _write_catalog(_minimal_v1_catalog())
    try:
        return NodeCatalog(path)
    finally:
        os.unlink(path)


def _raw_blackboard_reference(path: str) -> str:
    return "$" + "{bb:" + path + "}"


def _minimal_program(**overrides) -> dict:
    prog = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "test",
        "catalog_version": "0.2.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        "tree": {
            "id": "sequence",
            "name": "root",
            "children": [],
        },
    }
    prog.update(overrides)
    return prog


class TestProgramLoading:
    def test_loads_fixture(self, catalog):
        prog = Program(FIXTURE_PATH, catalog)
        assert prog.name == "test_pick_and_place"
        assert prog.program_profile == "v0.0"
        assert prog.robot.robot_id == "ur5"
        assert len(prog.scene) == 1
        assert prog.scene[0].object_id == "table"
        assert len(prog.parameters) == 1
        assert "pick_velocity_scale" in prog.parameters
        assert prog.tree is not None
        assert prog.tree.id == "sequence"
        assert len(prog.tree.children) == 2

    def test_robot_base_pose_round_trips(self, catalog):
        data = yaml.safe_load(FIXTURE_PATH.read_text())
        data["robot"]["base_pose"] = {
            "frame_id": "universe",
            "position_m": [0.1, -0.2, 0.3],
            "orientation_rpy_deg": [0, 0, 90],
        }
        path = _write_program(data)

        try:
            prog = Program(path, catalog)
        finally:
            os.unlink(path)

        assert prog.robot.base_pose == data["robot"]["base_pose"]
        assert prog.to_dict()["robot"]["base_pose"] == data["robot"]["base_pose"]

    def test_robot_base_pose_requires_orientation(self, catalog):
        data = yaml.safe_load(FIXTURE_PATH.read_text())
        data["robot"]["base_pose"] = {
            "frame_id": "universe",
            "position_m": [0.1, -0.2, 0.3],
        }
        path = _write_program(data)

        try:
            with pytest.raises(ProgramValidationError, match="base_pose requires orientation"):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_tree_node_names(self, catalog):
        prog = Program(FIXTURE_PATH, catalog)
        names = []
        self._collect_names(prog.tree, names)
        assert "main_sequence" in names
        assert "move_to_pick" in names
        assert "remove_table" in names

    def _collect_names(self, node, names):
        names.append(node.name)
        for c in node.children:
            self._collect_names(c, names)

    def test_parameter_defaults(self, catalog):
        prog = Program(FIXTURE_PATH, catalog)
        defaults = prog.get_parameter_defaults()
        assert defaults["pick_velocity_scale"] == 0.5


class TestTreeParamOverrides:
    def test_nested_param_patch_preserves_discriminator(self):
        node = TreeNode(
            id="upsert_collision_object",
            name="upsert_box",
            params={
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.1, 0.1, 0.1],
                },
                "object_id": "box1",
            },
        )

        updated = apply_tree_param_overrides(
            node,
            {
                "upsert_box": {
                    "shape": {
                        "box_dimensions_m": [0.2, 0.3, 0.4],
                    }
                }
            },
        )

        assert updated.params["shape"] == {
            "type": "box",
            "box_dimensions_m": [0.2, 0.3, 0.4],
        }
        assert node.params["shape"]["box_dimensions_m"] == [0.1, 0.1, 0.1]

    def test_nested_param_patch_replaces_when_discriminator_changes(self):
        node = TreeNode(
            id="upsert_collision_object",
            name="upsert_box",
            params={
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.1, 0.1, 0.1],
                },
                "object_id": "box1",
            },
        )

        updated = apply_tree_param_overrides(
            node,
            {
                "upsert_box": {
                    "shape": {
                        "type": "sphere",
                        "sphere_radius_m": 0.05,
                    }
                }
            },
        )

        assert updated.params["shape"] == {
            "type": "sphere",
            "sphere_radius_m": 0.05,
        }

    def test_discriminator_change_patch_validates_without_stale_fields(self):
        catalog = _load_minimal_v1_catalog()
        base_child = TreeNode(
            id="upsert_collision_object",
            name="upsert_box",
            params={
                "object_id": "box1",
                "pose": {
                    "frame_id": "base_link",
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.1, 0.2, 0.3],
                },
            },
        )
        updated_child = apply_tree_param_overrides(
            base_child,
            {
                "upsert_box": {
                    "shape": {
                        "type": "sphere",
                        "sphere_radius_m": 0.05,
                    }
                }
            },
        )
        data = _minimal_program(
            catalog_version="0.2.0",
            tree={
                "id": "sequence",
                "name": "root",
                "children": [updated_child.to_dict()],
            },
        )

        program = Program.from_dict(data, catalog, skill_catalog=SkillCatalog())

        child = program.tree.children[0]
        assert child.params["shape"] == {
            "type": "sphere",
            "sphere_radius_m": 0.05,
        }


class TestProgramValidation:
    def test_rejects_bad_api_version(self, catalog):
        path = _write_program(_minimal_program(api_version="wrong"))
        with pytest.raises(ProgramValidationError, match="api_version"):
            Program(path, catalog)
        os.unlink(path)

    @pytest.mark.parametrize("field_name", ["interrupt_handlers", "interrupt_policies"])
    def test_rejects_removed_cooperative_interrupt_fields(self, catalog, field_name):
        data = _minimal_program(**{field_name: []})
        with pytest.raises(
            ProgramValidationError,
            match=rf"Cooperative interrupt fields.*{field_name}",
        ):
            Program.from_dict(data, catalog)

    def test_rejects_missing_profile(self, catalog):
        path = _write_program(_minimal_program(program_profile=""))
        with pytest.raises(ProgramValidationError, match="program_profile"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_unknown_profile(self, catalog):
        path = _write_program(_minimal_program(program_profile="v9.9"))
        with pytest.raises(ProgramValidationError, match="Unknown program profile"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_name(self, catalog):
        path = _write_program(_minimal_program(name=""))
        with pytest.raises(ProgramValidationError, match="name"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_catalog_version(self, catalog):
        path = _write_program(_minimal_program(catalog_version=""))
        with pytest.raises(ProgramValidationError, match="catalog_version"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_invalid_catalog_version(self, catalog):
        path = _write_program(_minimal_program(catalog_version="not-semver"))
        with pytest.raises(ProgramValidationError, match="catalog_version must match X.Y.Z"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_catalog_version_mismatch(self, catalog):
        path = _write_program(_minimal_program(catalog_version="0.1.0"))
        with pytest.raises(
            ProgramValidationError,
            match="catalog_version must match loaded catalog version",
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_robot(self, catalog):
        data = _minimal_program()
        del data["robot"]
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="robot"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_duplicate_scene_ids(self, catalog):
        data = _minimal_program()
        data["scene"] = [
            {
                "object_id": "dup",
                "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                "pose": {
                    "frame_id": "base_link",
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            },
            {
                "object_id": "dup",
                "shape": {"type": "sphere", "sphere_radius_m": 0.05},
                "pose": {
                    "frame_id": "base_link",
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            },
        ]
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Duplicate scene"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_scene_resource_missing_object_id(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "base_link",
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ]
        )
        with pytest.raises(ProgramValidationError, match=r"scene\[0\] object_id is required"):
            Program.from_dict(data, catalog)

    def test_rejects_scene_resource_missing_pose_frame_id(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ]
        )
        with pytest.raises(ProgramValidationError, match=r"scene\[0\] pose\.frame_id is required"):
            Program.from_dict(data, catalog)

    def test_rejects_scene_resource_invalid_orientation_arity(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "base_link",
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_xyzw": [0.0, 0.0, 1.0],
                    },
                }
            ]
        )
        with pytest.raises(
            ProgramValidationError,
            match=r"scene\[0\] pose\.orientation_xyzw must be a list of 4 numeric values",
        ):
            Program.from_dict(data, catalog)

    def test_rejects_scene_resource_with_both_orientation_encodings(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "base_link",
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        "orientation_rpy_deg": [0.0, 0.0, 0.0],
                    },
                }
            ]
        )
        with pytest.raises(
            ProgramValidationError,
            match=r"scene\[0\] pose must define exactly one of orientation_xyzw or orientation_rpy_deg",
        ):
            Program.from_dict(data, catalog)

    def test_allows_scene_resource_with_orientation_rpy_deg(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "base_link",
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_rpy_deg": [0.0, 0.0, 45.0],
                    },
                }
            ],
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "reset_trajectories",
                        "name": "reset",
                    }
                ],
            },
        )
        program = Program.from_dict(data, catalog)
        assert program.scene[0].pose["orientation_rpy_deg"] == [0.0, 0.0, 45.0]

    def test_allows_scene_resource_support_contact_metadata(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.0, 0.0, 0.05],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "metadata": {
                        "support_contacts": [
                            {
                                "support_object_id": "table",
                                "max_start_penetration_m": 0.005,
                                "require_separation_by_end": True,
                            }
                        ]
                    },
                },
                {
                    "object_id": "table",
                    "shape": {"type": "box", "box_dimensions_m": [1.0, 1.0, 0.1]},
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.0, 0.0, -0.05],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                },
            ],
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "reset_trajectories",
                        "name": "reset",
                    }
                ],
            },
        )
        program = Program.from_dict(data, catalog)
        assert program.scene[0].metadata["support_contacts"][0] == {
            "support_object_id": "table",
            "max_start_penetration_m": 0.005,
            "require_separation_by_end": True,
        }
        assert program.to_dict()["scene"][0]["metadata"]["support_contacts"][0][
            "support_object_id"
        ] == "table"

    def test_v1_catalog_schema_accepts_pose_shape_node_params(self):
        catalog = _load_minimal_v1_catalog()
        data = _minimal_program(
            catalog_version="0.2.0",
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "upsert_collision_object",
                        "name": "upsert_box",
                        "params": {
                            "object_id": "box1",
                            "pose": {
                                "frame_id": "base_link",
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_rpy_deg": [0.0, 0.0, 0.0],
                            },
                            "shape": {
                                "type": "box",
                                "box_dimensions_m": [0.1, 0.2, 0.3],
                            },
                        },
                    },
                ],
            },
        )

        program = Program.from_dict(data, catalog, skill_catalog=SkillCatalog())

        assert program.tree.children[0].params["shape"]["type"] == "box"

    def test_v1_catalog_schema_rejects_unknown_pose_fields(self):
        catalog = _load_minimal_v1_catalog()
        data = _minimal_program(
            catalog_version="0.2.0",
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "upsert_collision_object",
                        "name": "upsert_box",
                        "params": {
                            "object_id": "box1",
                            "pose": {
                                "frame_id": "base_link",
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                                "confidence": 0.9,
                            },
                            "shape": {
                                "type": "box",
                                "box_dimensions_m": [0.1, 0.2, 0.3],
                            },
                        },
                    },
                ],
            },
        )

        with pytest.raises(
            ProgramValidationError,
            match="pose must not contain unknown fields: confidence",
        ):
            Program.from_dict(data, catalog, skill_catalog=SkillCatalog())

    def test_v1_catalog_schema_rejects_missing_conditional_shape_fields(self):
        catalog = _load_minimal_v1_catalog()
        data = _minimal_program(
            catalog_version="0.2.0",
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "upsert_collision_object",
                        "name": "upsert_box",
                        "params": {
                            "object_id": "box1",
                            "pose": {
                                "frame_id": "base_link",
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                            },
                            "shape": {"type": "box"},
                        },
                    },
                ],
            },
        )

        with pytest.raises(
            ProgramValidationError,
            match=r"shape\.box_dimensions_m is required by condition",
        ):
            Program.from_dict(data, catalog, skill_catalog=SkillCatalog())

    def test_v1_catalog_schema_rejects_pose_array_arity(self):
        catalog = _load_minimal_v1_catalog()
        data = _minimal_program(
            catalog_version="0.2.0",
            tree={
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "upsert_collision_object",
                        "name": "upsert_box",
                        "params": {
                            "object_id": "box1",
                            "pose": {
                                "frame_id": "base_link",
                                "position_m": [0.0, 0.0],
                                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                            },
                            "shape": {
                                "type": "box",
                                "box_dimensions_m": [0.1, 0.2, 0.3],
                            },
                        },
                    },
                ],
            },
        )

        with pytest.raises(
            ProgramValidationError,
            match=r"pose\.position_m must contain exactly 3 items",
        ):
            Program.from_dict(data, catalog, skill_catalog=SkillCatalog())

    def test_rejects_scene_resource_support_contact_invalid_boolean_policy_field(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.0, 0.0, 0.05],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "metadata": {
                        "support_contacts": [
                            {
                                "support_object_id": "table",
                                "require_separation_by_end": "no",
                            }
                        ]
                    },
                }
            ]
        )
        with pytest.raises(
            ProgramValidationError,
            match=r"metadata\.support_contacts\[0\]\.require_separation_by_end must be boolean",
        ):
            Program.from_dict(data, catalog)

    def test_rejects_scene_resource_support_contact_unknown_object(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.0, 0.0, 0.05],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "metadata": {
                        "support_contacts": [{"support_object_id": "missing_table"}]
                    },
                }
            ]
        )
        with pytest.raises(
            ProgramValidationError,
            match=(
                r"scene resource 'box1' support_contacts references unknown "
                r"object_id 'missing_table'"
            ),
        ):
            Program.from_dict(data, catalog)

    def test_rejects_scene_resource_support_contact_self_reference(self, catalog):
        data = _minimal_program(
            scene=[
                {
                    "object_id": "box1",
                    "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.0, 0.0, 0.05],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "metadata": {
                        "support_contacts": [{"support_object_id": "box1"}]
                    },
                }
            ]
        )
        with pytest.raises(
            ProgramValidationError,
            match="scene resource support_contacts cannot reference the same object",
        ):
            Program.from_dict(data, catalog)

    def test_rejects_duplicate_param_names(self, catalog):
        data = _minimal_program()
        data["parameters"] = [
            {"name": "dup", "type": "float", "default": 1.0},
            {"name": "dup", "type": "int", "default": 2},
        ]
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Duplicate parameter"):
            Program(path, catalog)
        os.unlink(path)

    def test_allows_declared_blackboard_namespaces_and_keys(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {
                        "id": "selection_context",
                        "contract": "selection_context_v1",
                        "prefix": "selection/context",
                    },
                ],
                "keys": [
                    {
                        "id": "gripper_closed",
                        "type": "bool",
                        "key": "gripper/closed",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "build_scene_selection_context",
                    "name": "build_selection_context",
                    "params": {
                        "selection_request_text": "Select the sphere object.",
                        "output_key_prefix": {"namespace_ref": "selection_context"},
                    },
                },
                {
                    "id": "set_key_bool_value",
                    "name": "mark_gripper_closed",
                    "params": {
                        "key": {"key_ref": "gripper_closed"},
                        "value": True,
                    },
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert "selection_context" in program.blackboard_namespaces
            assert "gripper_closed" in program.blackboard_keys
        finally:
            os.unlink(path)

    def test_rejects_undeclared_blackboard_key_when_declarations_exist(self, catalog):
        data = _minimal_program(
            blackboard={
                "keys": [
                    {
                        "id": "other_key",
                        "type": "bool",
                        "key": "other/key",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "set_key_bool_value",
                    "name": "mark_gripper_closed",
                    "params": {
                        "key": {"key_ref": "gripper_closed"},
                        "value": True,
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match="key_ref 'gripper_closed' is not declared in program.blackboard.keys",
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_allows_add_collision_object_object_id_from_declared_blackboard_key(self, catalog):
        data = _minimal_program(
            blackboard={
                "keys": [
                    {
                        "id": "target_object_id",
                        "type": "string",
                        "key": "scene/target_object_id",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "add_collision_object",
                    "name": "spawn_selected",
                    "params": {
                        "object_id": {"key_ref": "target_object_id"},
                        "shape": {
                            "type": "box",
                            "box_dimensions_m": [0.1, 0.1, 0.1],
                        },
                        "pose": {
                            "frame_id": "base_link",
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation_rpy_deg": [0.0, 0.0, 0.0],
                        },
                    },
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert program.tree.children[0].params["object_id"] == {"key_ref": "target_object_id"}
        finally:
            os.unlink(path)

    def test_rejects_add_collision_object_object_id_from_undeclared_blackboard_key(self, catalog):
        data = _minimal_program(
            blackboard={
                "keys": [
                    {
                        "id": "other_object_id",
                        "type": "string",
                        "key": "scene/other_object_id",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "add_collision_object",
                    "name": "spawn_selected",
                    "params": {
                        "object_id": {"key_ref": "target_object_id"},
                        "shape": {
                            "type": "box",
                            "box_dimensions_m": [0.1, 0.1, 0.1],
                        },
                        "pose": {
                            "frame_id": "base_link",
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation_rpy_deg": [0.0, 0.0, 0.0],
                        },
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match="key_ref 'target_object_id' is not declared in program.blackboard.keys",
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_declared_namespace_contract_mismatch(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {
                        "id": "selection_context",
                        "contract": "validated_selection_v1",
                        "prefix": "selection/context",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "build_scene_selection_context",
                    "name": "build_selection_context",
                    "params": {
                        "selection_request_text": "Select the sphere object.",
                        "output_key_prefix": {"namespace_ref": "selection_context"},
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match=(
                "output_key_prefix namespace_ref 'selection_context' is declared as contract "
                "'validated_selection_v1', expected 'selection_context_v1'"
            ),
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_unknown_node_id(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "nonexistent_node_abc",
            "name": "bad_node",
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Unknown catalog"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_node_when_program_profile_below_catalog_min_profile(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "command_gripper",
                    "name": "close_gripper",
                    "params": {
                        "gripper_id": "primary",
                        "position": 0.7,
                    },
                }
            ],
        }
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match=r"requires program profile >= v0\.1, got v0\.0",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_rejects_relative_move_when_program_profile_below_v0_1(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "relative_move_step",
                    "params": {
                        "move_id": "move_1",
                        "motion_type": "relative_move",
                        "relative_move": {
                            "target_reference": {
                                "type": "frame",
                                "id": "base_link",
                            },
                            "offset": {
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_rpy_deg": [0.0, 0.0, 0.0],
                            },
                        },
                    },
                }
            ],
        }
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match=r"motion_type value 'relative_move' requires program profile >= v0\.1, got v0\.0",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_rejects_node_when_deployment_capability_missing(self, catalog):
        data = _minimal_program(program_profile="v0.1")
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "command_gripper",
                    "name": "close_gripper",
                    "params": {
                        "gripper_id": "primary",
                        "position": 0.7,
                    },
                }
            ],
        }
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match=(
                    r"requires capabilities "
                    r"\['tool\.gripper\.command', 'tool\.gripper\.feedback'\]"
                ),
            ):
                Program(path, catalog, deployment_capability_keys=set())
        finally:
            os.unlink(path)

    def test_allows_node_when_deployment_capability_present(self, catalog):
        data = _minimal_program(program_profile="v0.1")
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "command_gripper",
                    "name": "close_gripper",
                    "params": {
                        "gripper_id": "primary",
                        "position": 0.7,
                    },
                }
            ],
        }
        path = _write_program(data)
        try:
            program = Program(
                path,
                catalog,
                deployment_capability_keys={
                    "tool.gripper.command",
                    "tool.gripper.feedback",
                },
            )
            assert "tool.gripper.command" in program.required_capabilities()
            assert "tool.gripper.feedback" in program.required_capabilities()
        finally:
            os.unlink(path)

    def test_rejects_duplicate_node_names(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "reset_trajectories",
                    "name": "same_name",
                },
                {
                    "id": "reset_trajectories",
                    "name": "same_name",
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Duplicate node"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_tree(self, catalog):
        data = _minimal_program()
        del data["tree"]
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="tree"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_leaf_with_children(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "reset_trajectories",
            "name": "root_leaf",
            "children": [
                {"id": "reset_trajectories", "name": "child"},
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="at most 0 children"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_required_param(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "bad_move",
                    "params": {},  # missing required move_id, motion_type
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="missing required"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_invalid_parameter_reference(self, catalog):
        data = _minimal_program(
            parameters=[
                {"name": "pick_velocity_scale", "type": "float", "default": 0.5},
            ]
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "bad_move",
                    "params": {
                        "move_id": "pick",
                        "motion_type": "joint_goal",
                        "joint_goal_names": ["j1"],
                        "joint_goal_positions_rad": [0.1],
                        "velocity_scale": "${param:missing_velocity_scale}",
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Unknown parameter reference"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_duplicate_output_key_prefixes(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {"id": "shared_output", "contract": "selection_context_v1", "prefix": "shared/output"},
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "build_scene_selection_context",
                    "name": "build_selection_context_a",
                    "params": {
                        "selection_request_text": "Select the sphere object.",
                        "output_key_prefix": {"namespace_ref": "shared_output"},
                    },
                },
                {
                    "id": "build_scene_selection_context",
                    "name": "build_selection_context_b",
                    "params": {
                        "selection_request_text": "Select the cube object.",
                        "output_key_prefix": {"namespace_ref": "shared_output"},
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match="output_key_prefix namespace_ref 'shared_output' is used by multiple producer nodes",
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_selection_response_prefix_from_wrong_producer_type(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {"id": "selection_context", "contract": "selection_context_v1", "prefix": "selection/context"},
                    {"id": "validated_selection", "contract": "validated_selection_v1", "prefix": "selection/validated"},
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "build_scene_selection_context",
                    "name": "build_selection_context",
                    "params": {
                        "selection_request_text": "Select the sphere object.",
                        "output_key_prefix": {"namespace_ref": "selection_context"},
                    },
                },
                {
                    "id": "validate_scene_selection",
                    "name": "validate_selected_object",
                    "params": {
                        "selection_context_prefix": {"namespace_ref": "selection_context"},
                        "selection_response_prefix": {"namespace_ref": "selection_context"},
                        "output_key_prefix": {"namespace_ref": "validated_selection"},
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match=(
                "selection_response_prefix namespace_ref 'selection_context' is declared as contract "
                "'selection_context_v1', expected 'query_model_output_v1'"
            ),
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_out_of_range_parameter_value(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "bad_move",
                    "params": {
                        "move_id": "pick",
                        "motion_type": "joint_goal",
                        "joint_goal_names": ["j1"],
                        "joint_goal_positions_rad": [0.1],
                        "velocity_scale": -0.1,
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="velocity_scale must be > 0.0"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_joint_goal_without_joint_targets(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "bad_move",
                    "params": {
                        "move_id": "pick",
                        "motion_type": "joint_goal",
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="joint_goal_names is required"):
            Program(path, catalog)
        os.unlink(path)

    def test_allows_pose_goal_blackboard_reference_for_motion_nodes(self, catalog):
        data = _minimal_program()
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "dynamic_pose_goal",
                    "params": {
                        "move_id": "pick_pose",
                        "motion_type": "pose_goal",
                        "pose_goal": {
                            "field_ref": {
                                "namespace": "grasp_plan",
                                "field": "grasp_pose",
                            }
                        },
                    },
                },
            ],
        }
        data["blackboard"] = {
            "namespaces": [
                {
                    "id": "grasp_plan",
                    "contract": "query_model_output_v1",
                    "prefix": "perception/grasp_plan",
                    "fields": [
                        {"name": "grasp_pose", "type": "pose"},
                    ],
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert program.tree.children[0].params["pose_goal"] == {
                "field_ref": {"namespace": "grasp_plan", "field": "grasp_pose"},
            }
        finally:
            os.unlink(path)

    def test_allows_relative_move_field_ref_for_motion_nodes(self, catalog):
        data = _minimal_program(
            program_profile="v0.1",
            blackboard={
                "namespaces": [
                    {
                        "id": "approach_pose",
                        "contract": "pick_plan_v1",
                        "prefix": "perception/approach",
                        "fields": [
                            {"name": "pose", "type": "pose"},
                        ],
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "dynamic_relative_move",
                    "params": {
                        "move_id": "pick_pose",
                        "motion_type": "relative_move",
                        "relative_move": {
                            "target_reference": {
                                "type": "pose",
                                "pose": {
                                    "field_ref": {
                                        "namespace": "approach_pose",
                                        "field": "pose",
                                    },
                                },
                            },
                            "offset": {
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_rpy_deg": [0.0, 0.0, 0.0],
                            },
                        },
                    },
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            relative_move = program.tree.children[0].params["relative_move"]
            assert relative_move["target_reference"]["pose"] == {
                "field_ref": {"namespace": "approach_pose", "field": "pose"}
            }
        finally:
            os.unlink(path)

    def test_relative_move_required_capabilities_include_reference_resolution(self, catalog):
        data = _minimal_program(program_profile="v0.1")
        data["tree"] = {
            "id": "move_manipulator_action",
            "name": "relative_move_step",
            "params": {
                "move_id": "move_1",
                "motion_type": "relative_move",
                "relative_move": {
                    "target_reference": {
                        "type": "frame",
                        "id": "base_link",
                    },
                    "offset": {
                        "position_m": [0.0, 0.0, 0.1],
                        "orientation_rpy_deg": [0.0, 0.0, 0.0],
                    },
                },
            },
        }
        program = Program.from_dict(data, catalog)
        assert "context.pose.resolve_reference" in program.required_capabilities()

    def test_allows_typed_query_model_template_variable_bindings(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {
                        "id": "selection_context",
                        "contract": "selection_context_v1",
                        "prefix": "selection/context",
                        "fields": [
                            {"name": "scene_context_json", "type": "string"},
                        ],
                    },
                    {
                        "id": "object_selection",
                        "contract": "query_model_output_v1",
                        "prefix": "selection/output",
                        "fields": [],
                    },
                ],
                "keys": [
                    {
                        "id": "selection_request_text",
                        "type": "string",
                        "key": "selection_request_text",
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "query_model",
                    "name": "choose_scene_object",
                    "params": {
                        "model_endpoint_alias": "selection_model",
                        "prompt_template_id": "select_scene_object_v1",
                        "template_variables": {
                            "selection_request_text": {
                                "key_ref": "selection_request_text",
                            },
                            "scene_context_json": {
                                "field_ref": {
                                    "namespace": "selection_context",
                                    "field": "scene_context_json",
                                },
                            },
                        },
                        "output_key_prefix": {
                            "namespace_ref": "object_selection",
                        },
                    },
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert program.tree.children[0].params["template_variables"] == {
                "selection_request_text": {"key_ref": "selection_request_text"},
                "scene_context_json": {
                    "field_ref": {
                        "namespace": "selection_context",
                        "field": "scene_context_json",
                    }
                },
            }
        finally:
            os.unlink(path)

    def test_rejects_raw_query_model_template_variable_blackboard_string(self, catalog):
        data = _minimal_program(
            blackboard={
                "namespaces": [
                    {
                        "id": "object_selection",
                        "contract": "query_model_output_v1",
                        "prefix": "selection/output",
                        "fields": [],
                    },
                ],
            },
        )
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "query_model",
                    "name": "choose_scene_object",
                    "params": {
                        "model_endpoint_alias": "selection_model",
                        "prompt_template_id": "select_scene_object_v1",
                        "template_variables": {
                            "scene_context_json": _raw_blackboard_reference(
                                "selection/context/value/scene_context_json"
                            ),
                        },
                        "output_key_prefix": {"namespace_ref": "object_selection"},
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match=(
                r"template_variables\.scene_context_json must use a key_ref or field_ref "
                r"binding"
            ),
        ):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_relative_move_raw_blackboard_reference_for_motion_nodes(self, catalog):
        data = _minimal_program(program_profile="v0.1")
        data["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "dynamic_relative_move",
                    "params": {
                        "move_id": "pick_pose",
                        "motion_type": "relative_move",
                        "relative_move": {
                            "target_reference": {
                                "type": "pose",
                                "pose": _raw_blackboard_reference("perception/approach_pose"),
                            },
                            "offset": {
                                "position_m": [0.0, 0.0, 0.1],
                                "orientation_rpy_deg": [0.0, 0.0, 0.0],
                            },
                        },
                    },
                },
            ],
        }
        path = _write_program(data)
        with pytest.raises(
            ProgramValidationError,
            match=r"relative_move\.target_reference\.pose must be an object",
        ):
            Program(path, catalog)
        os.unlink(path)

    # ----- Scoped motion node validation parity -----

    @staticmethod
    def _scoped_program(*, node_id: str, params: dict, profile: str = "v0.2") -> dict:
        """Minimal program containing a single motion-scoped node."""
        prog = _minimal_program(program_profile=profile)
        prog["tree"] = {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": node_id,
                    "name": f"{node_id}_under_test",
                    "params": params,
                },
            ],
        }
        return prog

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_rejects_joint_goal_without_joint_targets(
        self, catalog, node_id
    ):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "joint_goal",
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError, match="joint_goal_names is required"
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_rejects_joint_goal_mismatched_array_lengths(
        self, catalog, node_id
    ):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "joint_goal",
                "joint_goal_names": ["j1", "j2"],
                "joint_goal_positions_rad": [0.1],
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="joint_goal_names and joint_goal_positions_rad must have the same length",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_rejects_pose_goal_without_pose(self, catalog, node_id):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "pose_goal",
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="pose_goal is required when motion_type=pose_goal",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_rejects_relative_move_without_request(
        self, catalog, node_id
    ):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "relative_move",
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="relative_move is required when motion_type=relative_move",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_rejects_malformed_relative_move(
        self, catalog, node_id
    ):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "relative_move",
                # Missing target_reference is a structural defect that
                # would surface at runtime in the kernel without
                # validation parity.
                "relative_move": {
                    "offset": {"position_m": [0.0, 0.0, 0.01]},
                },
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError, match="target_reference is required"
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        "node_id", ["move_until_signal", "probe_search_move"]
    )
    def test_scoped_node_loads_when_joint_goal_is_well_formed(
        self, catalog, node_id
    ):
        data = self._scoped_program(
            node_id=node_id,
            params={
                "move_id": "probe_a",
                "motion_type": "joint_goal",
                "joint_goal_names": ["j1", "j2"],
                "joint_goal_positions_rad": [0.1, -0.2],
                "policy_id": "trip",
                "stop_mode": "spring_back_hold",
            },
        )
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert program.tree.children[0].id == node_id
        finally:
            os.unlink(path)


class TestRecoveryParameterValidation:
    def test_rejects_unknown_regular_node_param(self, catalog):
        data = _minimal_program(
            tree={
                "id": "reset_trajectories",
                "name": "reset",
                "params": {"unexpected": True},
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="Unexpected parameter 'unexpected'",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_fallback_params_reject_unknown_child_param(self, catalog):
        data = _minimal_program(
            tree={
                "id": "fallback_override_node",
                "name": "fallback",
                "params": {
                    "fallback_params": {"unexpected": True},
                    "max_retries": 1,
                },
                "children": [
                    {
                        "id": "reset_trajectories",
                        "name": "reset",
                    }
                ],
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="Unknown fallback override parameter 'unexpected'",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_fallback_params_validate_child_param_schema(self, catalog):
        data = _minimal_program(
            tree={
                "id": "fallback_override_node",
                "name": "fallback",
                "params": {
                    "fallback_params": {"num_cycles": "twice"},
                    "max_retries": 1,
                },
                "children": [
                    {
                        "id": "repeat",
                        "name": "repeat_child",
                        "params": {"num_cycles": 1},
                        "children": [
                            {
                                "id": "reset_trajectories",
                                "name": "reset",
                            }
                        ],
                    }
                ],
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="num_cycles must be an integer",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)

    def test_recovery_params_validate_selected_skill_params(self, catalog):
        data = _minimal_program(
            tree={
                "id": "recovery_skill_node",
                "name": "recover",
                "params": {
                    "recovery_skill_id": "move_to_named_config",
                    "recovery_params": {
                        "config_name": "home",
                        "joint_names": "joint_1",
                        "joint_positions_rad": [0.0],
                    },
                    "max_retries": 1,
                },
                "children": [
                    {
                        "id": "reset_trajectories",
                        "name": "reset",
                    }
                ],
            },
        )
        path = _write_program(data)
        try:
            with pytest.raises(
                ProgramValidationError,
                match="Parameter 'joint_names' for skill 'move_to_named_config' must be a list of strings",
            ):
                Program(path, catalog)
        finally:
            os.unlink(path)


class TestComputeProgramTreeHash:
    def test_deterministic(self):
        tree = TreeNode(id="move_to_waypoint", name="move_to_pick", params={"speed": 1.0})
        assert compute_program_tree_hash(tree) == compute_program_tree_hash(tree)

    def test_different_params_produce_different_hash(self):
        t1 = TreeNode(id="move_to_waypoint", name="move_to_pick", params={"speed": 1.0})
        t2 = TreeNode(id="move_to_waypoint", name="move_to_pick", params={"speed": 2.0})
        assert compute_program_tree_hash(t1) != compute_program_tree_hash(t2)

    def test_different_children_produce_different_hash(self):
        child_a = TreeNode(id="move_to_waypoint", name="a")
        child_b = TreeNode(id="move_to_waypoint", name="b")
        t1 = TreeNode(id="sequence", name="root", children=[child_a])
        t2 = TreeNode(id="sequence", name="root", children=[child_b])
        assert compute_program_tree_hash(t1) != compute_program_tree_hash(t2)

    def test_same_structure_same_hash(self):
        t1 = TreeNode(
            id="sequence", name="root",
            children=[TreeNode(id="move_to_waypoint", name="m", params={"x": 1})],
        )
        t2 = TreeNode(
            id="sequence", name="root",
            children=[TreeNode(id="move_to_waypoint", name="m", params={"x": 1})],
        )
        assert compute_program_tree_hash(t1) == compute_program_tree_hash(t2)

    def test_modification_policy_affects_hash(self):
        t1 = TreeNode(id="move_to_waypoint", name="m")
        t2 = TreeNode(id="move_to_waypoint", name="m", modification_policy={"runtime_override": "llm_allowed"})
        assert compute_program_tree_hash(t1) != compute_program_tree_hash(t2)

    def test_hash_is_hex_sha256(self):
        tree = TreeNode(id="move_to_waypoint", name="m")
        h = compute_program_tree_hash(tree)
        assert len(h) == 64
        assert all(c in "0123456789abcdef" for c in h)
