"""End-to-end tests for the native C++ kernel (no trampolines).

Tests: NativeKernelClient through the full stack — execute, scene edits,
error recovery, caching, and benchmarks.
"""

import time
import pytest

from manyforge_behavior.kernel_client import (
    ExecuteRequest, ExecutionState, SceneEditRequest,
)

try:
    import manyforge_roboplan_py
    from native_kernel_client import NativeKernelClient
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False


UR5_JOINTS = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]

GOAL_A = [0.1, -0.5, 0.5, -0.5, -0.5, 0.0]
GOAL_B = [0.3, -0.8, 0.8, -0.3, -0.8, 0.1]


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def native_kernel():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")
    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    return NativeKernelClient(bundle)


def make_request(move_id, goal, cache_mode="default"):
    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=UR5_JOINTS,
        joint_goal_positions_rad=goal,
        velocity_scale=1.0,
        acceleration_scale=1.0,
        planning_timeout_s=10.0,
        execution_timeout_s=30.0,
        cache_mode=cache_mode,
    )


# ============================================================================
# Native kernel: basic operations
# ============================================================================

class TestNativeKernelBasics:

    def test_initial_state_idle(self, native_kernel):
        status = native_kernel.get_status()
        assert status.state == ExecutionState.IDLE

    def test_execute_joint_goal(self, native_kernel):
        result = native_kernel.execute(make_request("native_basic", GOAL_A))
        assert result.ok is True
        assert result.execution_id > 0

    def test_state_after_execute(self, native_kernel):
        native_kernel.execute(make_request("native_state", GOAL_A))
        status = native_kernel.get_status()
        assert status.state == ExecutionState.COMPLETED

    def test_reset_to_idle(self, native_kernel):
        native_kernel.execute(make_request("native_reset", GOAL_A))
        result = native_kernel.reset()
        assert result.accepted is True
        assert native_kernel.get_status().state == ExecutionState.IDLE

    def test_execute_reset_cycle(self, native_kernel):
        for i in range(5):
            result = native_kernel.execute(
                make_request(f"native_cycle_{i}", GOAL_A, cache_mode="disable"))
            assert result.ok is True, f"Cycle {i} failed: {result.error}"
            native_kernel.reset()

    def test_auto_reset_on_sequential_execute(self, native_kernel):
        """Second execute auto-resets from COMPLETED."""
        r1 = native_kernel.execute(make_request("native_auto_1", GOAL_A))
        assert r1.ok is True
        # Don't reset — should auto-reset
        r2 = native_kernel.execute(make_request("native_auto_2", GOAL_B))
        assert r2.ok is True


# ============================================================================
# Native kernel: scene edits
# ============================================================================

class TestNativeSceneEdits:

    def test_add_box(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="box1", shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

    def test_add_sphere(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="sphere1", shape_type="sphere",
            sphere_radius_m=0.05,
            pose_position_m=[0.3, 0.1, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

    def test_add_remove(self, native_kernel):
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="temp", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="remove", object_id="temp",
        ))
        assert result.success is True

    def test_scene_edit_then_motion(self, native_kernel):
        """Add a small obstacle far from the goal — planner should still succeed."""
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="obstacle", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.8, 0.8, 0.8],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        result = native_kernel.execute(make_request("native_obstacle", GOAL_A))
        assert result.ok is True

    def test_get_link_pose(self, native_kernel):
        pose = native_kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert "position_m" in pose
        assert "orientation_xyzw" in pose
        assert len(pose["position_m"]) == 3
        assert len(pose["orientation_xyzw"]) == 4


# ============================================================================
# Native kernel: caching
# ============================================================================

class TestNativeCaching:

    def test_cache_hit(self, native_kernel):
        req = make_request("native_cache_hit", GOAL_A)
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        native_kernel.reset()

        # Second execute should hit cache
        r2 = native_kernel.execute(req)
        assert r2.ok is True

    def test_cache_disabled(self, native_kernel):
        req = make_request("native_nocache", GOAL_A, cache_mode="disable")
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        native_kernel.reset()
        r2 = native_kernel.execute(req)
        assert r2.ok is True

    def test_invalidate_trajectories(self, native_kernel):
        native_kernel.execute(make_request("native_inval", GOAL_A))
        native_kernel.reset()
        native_kernel.invalidate_trajectories()
        # Should still work after invalidation (plans fresh)
        r = native_kernel.execute(make_request("native_inval", GOAL_A))
        assert r.ok is True


# ============================================================================
# Native kernel: benchmarks
# ============================================================================

class TestNativeBenchmark:

    def test_single_plan_timing(self, native_kernel):
        """Single uncached plan timing."""
        num_trials = 3

        # Warmup
        native_kernel.execute(make_request("warmup", GOAL_A))
        native_kernel.reset()

        times = []
        for i in range(num_trials):
            req = make_request(f"native_{i}", GOAL_A, cache_mode="disable")
            start = time.perf_counter()
            r = native_kernel.execute(req)
            times.append(time.perf_counter() - start)
            assert r.ok is True, f"Trial {i} failed: {r.error}"
            native_kernel.reset()

        avg = sum(times) / len(times) * 1000
        print(f"\n=== Single Plan ({num_trials} trials) ===")
        print(f"  Native C++: {avg:.1f}ms avg")

    def test_cached_replay_timing(self, native_kernel):
        """Cached replay timing."""
        native_kernel.execute(make_request("n_cache", GOAL_B))
        native_kernel.reset()

        req = make_request("n_cache", GOAL_B)
        start = time.perf_counter()
        native_kernel.execute(req)
        t = time.perf_counter() - start

        print(f"\n=== Cached Replay ===")
        print(f"  Native C++: {t*1000:.2f}ms")

    def test_five_moves_timing(self, native_kernel):
        """5 sequential moves timing."""
        goals = [
            [0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            [0.3, -0.8, 0.8, -0.3, -0.8, 0.1],
            [-0.2, -1.0, 1.0, -0.5, -1.0, 0.0],
            [0.5, -0.6, 0.6, -0.6, -0.6, 0.2],
            [0.0, -0.7, 0.7, -0.7, -0.7, 0.0],
        ]

        # Warmup
        native_kernel.execute(make_request("warmup5", goals[0]))
        native_kernel.reset()

        start = time.perf_counter()
        for i, goal in enumerate(goals):
            r = native_kernel.execute(
                make_request(f"n5_{i}", goal, cache_mode="disable")
            )
            assert r.ok is True, f"Move {i} failed: {r.error}"
            native_kernel.reset()
        elapsed = time.perf_counter() - start

        n = len(goals)
        print(f"\n=== {n} Moves (no cache) ===")
        print(f"  Native C++: {elapsed*1000:.0f}ms total, {elapsed/n*1000:.1f}ms/move")


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])
