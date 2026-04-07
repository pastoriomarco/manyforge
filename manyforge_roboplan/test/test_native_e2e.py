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


def wait_for_terminal(native_kernel, timeout_s=15.0):
    deadline = time.monotonic() + timeout_s
    last_status = native_kernel.get_status()
    while time.monotonic() < deadline:
        last_status = native_kernel.get_status()
        if last_status.state in {
            ExecutionState.COMPLETED,
            ExecutionState.FAILED,
            ExecutionState.STOPPED,
        }:
            return last_status
        time.sleep(0.01)
    pytest.fail(
        f"Timed out waiting for terminal state; last status was {last_status.state}"
    )


def wait_for_non_idle(native_kernel, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    last_status = native_kernel.get_status()
    while time.monotonic() < deadline:
        last_status = native_kernel.get_status()
        if last_status.state != ExecutionState.IDLE:
            return last_status
        time.sleep(0.005)
    pytest.fail("Timed out waiting for execution to leave IDLE")


def get_cached_duration_s(native_kernel, move_id):
    traj = native_kernel.get_cached_trajectory(move_id)
    assert traj is not None, f"No cached trajectory for move_id={move_id}"
    assert traj.waypoints, f"Cached trajectory for move_id={move_id} has no waypoints"
    return traj.waypoints[-1].time_from_start_s


def reset_to_home(native_kernel):
    native_kernel.reset()
    native_kernel._bundle.set_joint_positions([0.0] * len(UR5_JOINTS))


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
        wait_for_terminal(native_kernel)

    def test_state_after_execute(self, native_kernel):
        native_kernel.execute(make_request("native_state", GOAL_A))
        status = wait_for_non_idle(native_kernel)
        assert status.active_move_id == "native_state"
        assert status.execution_id > 0

    def test_execution_snapshot_exposes_ids_and_progress(self, native_kernel):
        result = native_kernel.execute(make_request("native_snapshot", GOAL_A))
        assert result.ok is True
        snapshot = native_kernel.get_execution_snapshot()
        assert snapshot.execution_id == result.execution_id
        assert snapshot.move_id == "native_snapshot"
        assert snapshot.sequence_number > 0
        terminal = wait_for_terminal(native_kernel)
        final_snapshot = native_kernel.get_execution_snapshot()
        assert final_snapshot.execution_id == result.execution_id
        assert final_snapshot.move_id == "native_snapshot"
        assert final_snapshot.progress >= 0.0
        assert terminal.state in {
            ExecutionState.COMPLETED,
            ExecutionState.FAILED,
            ExecutionState.STOPPED,
        }

    def test_metrics_track_snapshot_publication_and_observer_lag(self, native_kernel):
        result = native_kernel.execute(make_request("native_metrics_snapshot", GOAL_A))
        assert result.ok is True
        wait_for_non_idle(native_kernel)
        time.sleep(0.05)
        native_kernel.get_execution_snapshot()
        wait_for_terminal(native_kernel)
        metrics = native_kernel.get_metrics()
        assert metrics["snapshots_published"] > 0
        assert metrics["observer_lag_max_ns"] > 0

    def test_reset_to_idle(self, native_kernel):
        native_kernel.execute(make_request("native_reset", GOAL_A))
        wait_for_terminal(native_kernel)
        result = native_kernel.reset()
        assert result.accepted is True
        assert native_kernel.get_status().state == ExecutionState.IDLE

    def test_execute_reset_cycle(self, native_kernel):
        for i in range(5):
            result = native_kernel.execute(
                make_request(f"native_cycle_{i}", GOAL_A, cache_mode="disable"))
            assert result.ok is True, f"Cycle {i} failed: {result.error}"
            wait_for_terminal(native_kernel)
            native_kernel.reset()

    def test_auto_reset_on_sequential_execute(self, native_kernel):
        """Second execute auto-resets from COMPLETED."""
        r1 = native_kernel.execute(make_request("native_auto_1", GOAL_A))
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        # Don't reset — should auto-reset
        r2 = native_kernel.execute(make_request("native_auto_2", GOAL_B))
        assert r2.ok is True
        wait_for_terminal(native_kernel)


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
        wait_for_terminal(native_kernel)

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
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        # Second execute should hit cache
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)

    def test_cache_disabled(self, native_kernel):
        req = make_request("native_nocache", GOAL_A, cache_mode="disable")
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        native_kernel.reset()
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)

    def test_invalidate_trajectories(self, native_kernel):
        native_kernel.execute(make_request("native_inval", GOAL_A))
        wait_for_terminal(native_kernel)
        native_kernel.reset()
        native_kernel.invalidate_trajectories()
        # Should still work after invalidation (plans fresh)
        r = native_kernel.execute(make_request("native_inval", GOAL_A))
        assert r.ok is True
        wait_for_terminal(native_kernel)


# ============================================================================
# Native kernel: benchmarks
# ============================================================================

class TestNativeBenchmark:

    def test_single_plan_timing(self, native_kernel):
        """Single uncached plan timing."""
        num_trials = 3

        # Warmup
        native_kernel.execute(make_request("warmup", GOAL_A))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        times = []
        for i in range(num_trials):
            req = make_request(f"native_{i}", GOAL_A, cache_mode="disable")
            start = time.perf_counter()
            r = native_kernel.execute(req)
            wait_for_terminal(native_kernel)
            times.append(time.perf_counter() - start)
            assert r.ok is True, f"Trial {i} failed: {r.error}"
            native_kernel.reset()

        avg = sum(times) / len(times) * 1000
        print(f"\n=== Single Plan ({num_trials} trials) ===")
        print(f"  Native C++: {avg:.1f}ms avg")

    def test_cached_replay_timing(self, native_kernel):
        """Cached replay timing."""
        native_kernel.execute(make_request("n_cache", GOAL_B))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        req = make_request("n_cache", GOAL_B)
        start = time.perf_counter()
        native_kernel.execute(req)
        wait_for_terminal(native_kernel)
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
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        start = time.perf_counter()
        for i, goal in enumerate(goals):
            r = native_kernel.execute(
                make_request(f"n5_{i}", goal, cache_mode="disable")
            )
            assert r.ok is True, f"Move {i} failed: {r.error}"
            wait_for_terminal(native_kernel)
            native_kernel.reset()
        elapsed = time.perf_counter() - start

        n = len(goals)
        print(f"\n=== {n} Moves (no cache) ===")
        print(f"  Native C++: {elapsed*1000:.0f}ms total, {elapsed/n*1000:.1f}ms/move")


# ============================================================================
# Native kernel: TCP speed enforcement
# ============================================================================

class TestTcpSpeedEnforcement:

    def _make_speed_limited_request(self, move_id, goal, tcp_speed_mps,
                                      cache_mode="default"):
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
            max_tcp_linear_speed_mps=tcp_speed_mps,
            planning_timeout_s=10.0,
            execution_timeout_s=30.0,
            cache_mode=cache_mode,
        )

    def test_speed_limited_succeeds(self, native_kernel):
        """Plan with TCP speed limit succeeds."""
        req = self._make_speed_limited_request("tcp_limit", GOAL_A, 0.25)
        result = native_kernel.execute(req)
        assert result.ok is True
        wait_for_terminal(native_kernel)

    def test_speed_limited_slower_than_unlimited(self, native_kernel):
        """Speed-limited trajectory takes longer than unlimited."""
        # Unlimited (use default cache so we can read trajectory duration)
        r1 = native_kernel.execute(make_request("tcp_fast", GOAL_A))
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        t_unlimited = get_cached_duration_s(native_kernel, "tcp_fast")
        assert t_unlimited > 0
        reset_to_home(native_kernel)

        # Speed-limited (0.10 m/s is very restrictive)
        req = self._make_speed_limited_request("tcp_slow", GOAL_A, 0.10)
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)
        t_limited = get_cached_duration_s(native_kernel, "tcp_slow")
        assert t_limited > 0

        print(f"\n=== TCP Speed Enforcement ===")
        print(f"  Unlimited traj duration: {t_unlimited:.3f}s")
        print(f"  Limited (0.10 m/s) traj duration: {t_limited:.3f}s")
        assert t_limited > t_unlimited, \
            f"Limited trajectory ({t_limited:.3f}s) should be slower than unlimited ({t_unlimited:.3f}s)"

    def test_no_limit_when_zero(self, native_kernel):
        """TCP speed = 0 means no limit (same as no field)."""
        req = self._make_speed_limited_request("tcp_zero_a", GOAL_A, 0.0)
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        t_zero = get_cached_duration_s(native_kernel, "tcp_zero_a")
        reset_to_home(native_kernel)

        r2 = native_kernel.execute(make_request("tcp_none_a", GOAL_A))
        assert r2.ok is True
        wait_for_terminal(native_kernel)
        t_none = get_cached_duration_s(native_kernel, "tcp_none_a")

        # Should be approximately the same (both unlimited)
        assert abs(t_zero - t_none) < 0.01, \
            f"Zero limit ({t_zero:.3f}s) should match no limit ({t_none:.3f}s)"


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])
