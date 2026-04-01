"""Tests for the orchestrator runner."""

from pathlib import Path

import pytest
import py_trees

from manyforge_behavior.kernel_client import ExecutionState, MockKernelClient
from manyforge_behavior.runner import OrchestratorRunner

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


class TestRunnerLoading:
    def test_load_succeeds(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        assert runner.catalog is not None
        assert runner.program is not None
        assert runner.program.name == "test_pick_and_place"

    def test_build_tree(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        tree = runner.build_tree()
        assert tree is not None
        assert tree.root.name == "main_sequence"


class TestRunnerSceneApplication:
    def test_apply_scene(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.apply_scene()
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].operation == "add"
        assert kernel.scene_edits[0].object_id == "table"

    def test_apply_scene_failure(self, kernel):
        kernel.fail_next_scene_edit = True
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        with pytest.raises(RuntimeError, match="Failed to add scene"):
            runner.apply_scene()


class TestRunnerExecution:
    def test_full_run(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        result = runner.run(max_ticks=100)
        assert result is True
        # Scene edit from tree (add_table) + scene edit from apply_scene (table)
        # + remove_table from tree = 3 total scene edits
        assert len(kernel.scene_edits) >= 2  # at least apply_scene + add_table
        assert kernel.execute_count >= 1  # move_to_pick

    def test_run_with_override(self, kernel):
        runner = OrchestratorRunner(
            CATALOG_PATH,
            FIXTURE_PATH,
            kernel,
            overrides={"pick_velocity_scale": 0.8},
        )
        runner.load()
        runner.build_tree()
        result = runner.run(max_ticks=100)
        assert result is True

    def test_run_fails_on_execute_failure(self, kernel):
        kernel.fail_next_execute = True
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        # Scene add will succeed, but move will fail (kernel rejects execute)
        # Actually the add_table scene node calls scene_edit, not execute
        # The move_to_pick calls execute which will fail
        # But we need kernel to be in IDLE for the scene edit (add_table), then
        # fail on the move. MockKernelClient.execute() sets state to FAILED on failure,
        # so we need reset to get back to IDLE for the move. Actually MockKernelClient
        # execute fails if not IDLE, and fail_next_execute triggers from IDLE.
        # The sequence: add_table (scene_edit, succeeds) -> move_to_pick (execute, fails)
        result = runner.run(max_ticks=100)
        assert result is False

    def test_run_without_load_raises(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        with pytest.raises(RuntimeError):
            runner.build_tree()

    def test_run_without_build_raises(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        with pytest.raises(RuntimeError):
            runner.run()

    def test_max_ticks_returns_false(self, kernel):
        """If tree doesn't complete within max_ticks, return False."""
        # Normal run completes quickly, so use max_ticks=0
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        result = runner.run(max_ticks=0)
        assert result is False
