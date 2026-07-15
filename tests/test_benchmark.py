"""Benchmark tests for the native C++ kernel with real RoboPlan backends.

Single execution path: NativeKernelClient (C++ kernel + C++ backends).
"""

import time
import pytest

from manyforge_behavior.kernel_client import ExecuteRequest, ExecutionState

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
        motion_type="joint",
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


# ============================================================================
# Real RoboPlan Benchmarks
# ============================================================================

class TestNativeBenchmark:
    """Benchmark tests for the native C++ kernel with real RoboPlan."""

    GOAL_A = [0.1, -0.5, 0.5, -0.5, -0.5, 0.0]
    GOAL_B = [0.3, -0.8, 0.8, -0.3, -0.8, 0.1]

    def test_benchmark_single_plan(self, native_kernel):
        """Time a single uncached plan+execute cycle."""
        num_trials = 3

        # Warmup
        native_kernel.execute(make_request("warmup", self.GOAL_A))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        times = []
        for i in range(num_trials):
            req = make_request(f"bench_single_{i}", self.GOAL_A, cache_mode="disable")
            start = time.perf_counter()
            r = native_kernel.execute(req)
            terminal = wait_for_terminal(native_kernel)
            times.append(time.perf_counter() - start)
            assert r.ok is True, f"Trial {i} failed: {r.error}"
            assert terminal.state == ExecutionState.COMPLETED
            native_kernel.reset()

        avg = sum(times) / len(times) * 1000
        print(f"\n=== Single Plan ({num_trials} trials) ===")
        print(f"  Avg: {avg:.1f}ms  [{', '.join(f'{t*1000:.1f}' for t in times)}]")

    def test_benchmark_cached_vs_uncached(self, native_kernel):
        """Compare cached vs uncached planning."""
        req = make_request("bench_cache", self.GOAL_B)

        # First execute (uncached)
        start1 = time.perf_counter()
        r1 = native_kernel.execute(req)
        terminal1 = wait_for_terminal(native_kernel)
        uncached_time = time.perf_counter() - start1
        assert r1.ok is True
        assert terminal1.state == ExecutionState.COMPLETED
        native_kernel.reset()

        # Second execute (cached)
        start2 = time.perf_counter()
        r2 = native_kernel.execute(req)
        terminal2 = wait_for_terminal(native_kernel)
        cached_time = time.perf_counter() - start2
        assert r2.ok is True
        assert terminal2.state == ExecutionState.COMPLETED
        native_kernel.reset()

        speedup = uncached_time / cached_time if cached_time > 0 else float("inf")
        print(f"\n=== Cached vs Uncached ===")
        print(f"  Uncached: {uncached_time*1000:.1f}ms")
        print(f"  Cached:   {cached_time*1000:.2f}ms")
        print(f"  Speedup:  {speedup:.1f}x")

    def test_benchmark_five_moves(self, native_kernel):
        """Time 5 different joint goals with real planning (no caching)."""
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
                make_request(f"bench_multi_{i}", goal, cache_mode="disable")
            )
            terminal = wait_for_terminal(native_kernel)
            assert r.ok is True, f"Move {i} failed: {r.error}"
            assert terminal.state == ExecutionState.COMPLETED
            native_kernel.reset()
        elapsed = time.perf_counter() - start

        n = len(goals)
        print(f"\n=== {n} Moves (no cache) ===")
        print(f"  Total: {elapsed*1000:.0f}ms")
        print(f"  Per move: {elapsed/n*1000:.1f}ms")


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])
