"""Factory function for creating CppKernelClient wired to RoboPlan mock objects."""

from __future__ import annotations

from manyforge_planning.roboplan_mock import (
    MockScene,
    MockRRT,
    MockRRTOptions,
    MockPathShortcutter,
    MockTOPPRA,
    MockSimpleIk,
    MockSimpleIkOptions,
    MockRoboPlan,
)

from cpp_bridge import (
    RoboPlanPlannerAdapter,
    RoboPlanPlanningContextAdapter,
    RoboPlanExecutionAdapter,
    WallClock,
    ThreadScheduler,
)

from cpp_kernel_client import CppKernelClient


def create_cpp_kernel_client(
    scene: MockScene,
    rrt: MockRRT,
    shortcutter: MockPathShortcutter,
    toppra: MockTOPPRA,
    ik: MockSimpleIk,
    roboplan_module,
) -> CppKernelClient:
    """
    Build a CppKernelClient wired to RoboPlan mock objects.

    This function creates adapter instances that wrap the mock objects and
    implements the C++ port interfaces via pybind11 trampolines.

    Args:
        scene: The MockScene instance for planning context.
        rrt: The MockRRT instance for path planning.
        shortcutter: The MockPathShortcutter instance for path smoothing.
        toppra: The MockTOPPRA instance for time parameterization.
        ik: The MockSimpleIk instance for inverse kinematics.
        roboplan_module: The MockRoboPlan module (for reference).

    Returns:
        A CppKernelClient instance implementing the KernelClient ABC.
    """
    # Create the planning context adapter
    planning_context = RoboPlanPlanningContextAdapter(scene)

    # Create the planner adapter
    planner_backend = RoboPlanPlannerAdapter(
        scene, rrt, shortcutter, toppra, ik
    )

    # Create the execution backend adapter
    execution_backend = RoboPlanExecutionAdapter()

    # Create the clock adapter
    clock = WallClock()

    # Create the scheduler adapter
    scheduler = ThreadScheduler()

    # Construct the CppKernelClient with all adapters
    client = CppKernelClient(
        planner_backend=planner_backend,
        planning_context=planning_context,
        execution_backend=execution_backend,
        clock=clock,
        scheduler=scheduler,
    )

    return client


def create_default_mock_scene() -> MockScene:
    """Create a default mock scene with 6 joints."""
    joint_names = ["j1", "j2", "j3", "j4", "j5", "j6"]
    return MockScene(joint_names=joint_names, group_name="arm")


def create_default_planning_components(scene: MockScene):
    """Create default planning components for a scene."""
    rrt_opts = MockRRTOptions()
    rrt_opts.group_name = "arm"
    rrt_opts.max_planning_time = 5.0
    rrt_opts.max_nodes = 5000

    rrt = MockRRT(scene, rrt_opts)
    shortcutter = MockPathShortcutter(scene, "arm")
    toppra = MockTOPPRA(scene)

    ik_opts = MockSimpleIkOptions()
    ik_opts.group_name = "arm"
    ik_opts.max_restarts = 50

    ik = MockSimpleIk(scene, ik_opts)

    return rrt, shortcutter, toppra, ik


def create_full_kernel_client() -> CppKernelClient:
    """
    Create a fully configured CppKernelClient with default mock objects.

    This is a convenience function that creates all mock objects and
    wires them together into a working kernel client.

    Returns:
        A fully configured CppKernelClient instance.
    """
    scene = create_default_mock_scene()
    rrt, shortcutter, toppra, ik = create_default_planning_components(scene)

    return create_cpp_kernel_client(
        scene, rrt, shortcutter, toppra, ik, MockRoboPlan
    )
