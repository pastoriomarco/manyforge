"""Factory function for creating CppKernelClient wired to real RoboPlan objects."""

from __future__ import annotations

import os

from roboplan.roboplan_ext import core, rrt, toppra, simple_ik, example_models

from real_roboplan_bridge import (
    RealRoboPlanPlannerAdapter,
    RealRoboPlanPlanningContextAdapter,
    RealRoboPlanExecutionAdapter,
    WallClock,
    ThreadScheduler,
)

from manyforge_core_python.cpp_kernel_client import CppKernelClient


def load_scene(model_name: str = "ur5_gripper", group_name: str = "arm") -> tuple:
    """
    Load a real RoboPlan scene and return (scene, group_name).

    Args:
        model_name: Name of the robot model (e.g., "ur5_gripper", "fr3")
        group_name: Name of the joint group to use

    Returns:
        Tuple of (scene, group_name)
    """
    # Get package directories
    models_dir = str(example_models.get_package_models_dir())
    pkg_share = str(example_models.get_package_share_dir())

    # Map model names to URDF/SRDF paths and default group names
    model_config = {
        "ur5_gripper": {
            "urdf": os.path.join(models_dir, 'ur_robot_model/ur5_gripper.urdf'),
            "srdf": os.path.join(models_dir, 'ur_robot_model/ur5_gripper.srdf'),
            "default_group": "arm",
        },
        "fr3": {
            "urdf": os.path.join(models_dir, 'franka_robot_model/fr3.urdf'),
            "srdf": os.path.join(models_dir, 'franka_robot_model/fr3.srdf'),
            "default_group": "fr3_arm",
        },
        "kinova": {
            "urdf": os.path.join(models_dir, 'kinova_robot_model/kinova_robotiq.urdf'),
            "srdf": os.path.join(models_dir, 'kinova_robot_model/kinova_robotiq.srdf'),
            "default_group": "kinova_arm",
        },
        "so101": {
            "urdf": os.path.join(models_dir, 'so101_robot_model/so101.urdf'),
            "srdf": os.path.join(models_dir, 'so101_robot_model/so101.srdf'),
            "default_group": "arm",
        },
    }

    if model_name not in model_config:
        raise ValueError(f"Unknown model: {model_name}. Available: {list(model_config.keys())}")

    config = model_config[model_name]

    # Use provided group_name or default
    actual_group_name = group_name if group_name else config["default_group"]

    # Load the scene
    scene = core.Scene(model_name, config["urdf"], config["srdf"], [pkg_share])

    return scene, actual_group_name


def create_real_planning_components(scene: core.Scene, group_name: str = "arm"):
    """
    Create real RoboPlan planning components for a scene.

    Args:
        scene: The RoboPlan Scene object
        group_name: Name of the joint group

    Returns:
        Tuple of (rrt, shortcutter, toppra, ik)
    """
    # Create RRT options
    rrt_opts = rrt.RRTOptions()
    rrt_opts.group_name = group_name
    rrt_opts.max_planning_time = 5.0
    rrt_opts.max_nodes = 5000

    # Create RRT planner
    rrt_planner = rrt.RRT(scene, rrt_opts)

    # Create path shortcutter
    shortcutter_obj = core.PathShortcutter(scene, group_name)

    # Create TOPPRA time parameterizer
    toppra_param = toppra.PathParameterizerTOPPRA(scene)

    # Create IK options
    ik_opts = simple_ik.SimpleIkOptions()
    ik_opts.group_name = group_name
    ik_opts.max_restarts = 50

    # Create IK solver
    ik = simple_ik.SimpleIk(scene, ik_opts)

    return rrt_planner, shortcutter_obj, toppra_param, ik


def create_real_kernel_client(
    scene: core.Scene,
    rrt_planner: rrt.RRT,
    shortcutter_obj: core.PathShortcutter,
    toppra_param: toppra.PathParameterizerTOPPRA,
    ik: simple_ik.SimpleIk,
) -> CppKernelClient:
    """
    Build a CppKernelClient wired to real RoboPlan objects.

    This function creates adapter instances that wrap the real RoboPlan objects and
    implements the C++ port interfaces via pybind11 trampolines.

    Args:
        scene: The real RoboPlan Scene instance for planning context.
        rrt_planner: The real RRT instance for path planning.
        shortcutter_obj: The real PathShortcutter instance for path smoothing.
        toppra_param: The real TOPPRA instance for time parameterization.
        ik: The real SimpleIk instance for inverse kinematics.

    Returns:
        A CppKernelClient instance implementing the KernelClient ABC.
    """
    # Create the planning context adapter
    planning_context = RealRoboPlanPlanningContextAdapter(scene)

    # Create the planner adapter
    planner_backend = RealRoboPlanPlannerAdapter(
        scene, rrt_planner, shortcutter_obj, toppra_param, ik
    )

    # Create the execution backend adapter
    execution_backend = RealRoboPlanExecutionAdapter()

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


def create_full_real_kernel_client(
    model_name: str = "ur5_gripper", group_name: str = "arm"
) -> CppKernelClient:
    """
    Create a fully configured CppKernelClient with real RoboPlan objects.

    This is a convenience function that loads a scene, creates all planning
    components, and wires them together into a working kernel client.

    Args:
        model_name: Name of the robot model (e.g., "ur5_gripper", "fr3")
        group_name: Name of the joint group to use

    Returns:
        A fully configured CppKernelClient instance with real RoboPlan.
    """
    # Load the scene
    scene, actual_group_name = load_scene(model_name, group_name)

    # Create planning components
    rrt_planner, shortcutter_obj, toppra_param, ik = create_real_planning_components(
        scene, actual_group_name
    )

    # Create the kernel client
    return create_real_kernel_client(
        scene, rrt_planner, shortcutter_obj, toppra_param, ik
    )
