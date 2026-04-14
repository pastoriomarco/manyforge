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
    compute_program_tree_hash,
)

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


def _minimal_program(**overrides) -> dict:
    prog = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "test",
        "catalog_version": "0.1.0",
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


class TestProgramValidation:
    def test_rejects_bad_api_version(self, catalog):
        path = _write_program(_minimal_program(api_version="wrong"))
        with pytest.raises(ProgramValidationError, match="api_version"):
            Program(path, catalog)
        os.unlink(path)

    def test_rejects_missing_profile(self, catalog):
        path = _write_program(_minimal_program(program_profile=""))
        with pytest.raises(ProgramValidationError, match="program_profile"):
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
            {"object_id": "dup", "shape": {"type": "box"}, "pose": {}},
            {"object_id": "dup", "shape": {"type": "sphere"}, "pose": {}},
        ]
        path = _write_program(data)
        with pytest.raises(ProgramValidationError, match="Duplicate scene"):
            Program(path, catalog)
        os.unlink(path)

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
                        "pose_goal": "${bb:perception/grasp_pose}",
                    },
                },
            ],
        }
        path = _write_program(data)
        try:
            program = Program(path, catalog)
            assert program.tree.children[0].params["pose_goal"] == "${bb:perception/grasp_pose}"
        finally:
            os.unlink(path)

    def test_allows_relative_move_blackboard_reference_for_motion_nodes(self, catalog):
        data = _minimal_program()
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
                                "pose": "${bb:perception/approach_pose}",
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
            assert relative_move["target_reference"]["pose"] == "${bb:perception/approach_pose}"
        finally:
            os.unlink(path)


# ── compute_program_tree_hash ────────────────────────────────────────────


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
