#!/usr/bin/env python3
"""ManyForge launcher — reads deployment + program artifacts and starts all components."""

from __future__ import annotations

import argparse
import sys
import time

import py_trees

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.cycle_manager import CycleManager, CycleState
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import KernelClient, MockKernelClient
from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_robot_model,
)
from manyforge_behavior.program import Program
from manyforge_behavior.runner import OrchestratorRunner

_ROS2_CONTROL_EXECUTION_ADAPTER_IDS = {
    "execution.ros2_control_fjt",
    "execution.ros2_control_follow_joint_trajectory",
}


def _native_kernel_config_from_deployment(deployment: Deployment) -> dict[str, int]:
    config: dict[str, int] = {}
    period_ms = deployment.orchestrator.config.get("collision_monitor_period_ms")
    if period_ms is not None:
        period_ms = float(period_ms)
        if period_ms <= 0.0:
            raise ValueError(
                "orchestrator.config.collision_monitor_period_ms must be > 0"
            )
        config["collision_monitor_period_ns"] = int(period_ms * 1_000_000.0)
    timeout_ms = deployment.orchestrator.config.get(
        "runtime_collision_query_timeout_ms"
    )
    if timeout_ms is not None:
        timeout_ms = float(timeout_ms)
        if timeout_ms <= 0.0:
            raise ValueError(
                "orchestrator.config.runtime_collision_query_timeout_ms must be > 0"
            )
        config["runtime_collision_query_timeout_ns"] = int(timeout_ms * 1_000_000.0)
    cap_ms = deployment.orchestrator.config.get(
        "runtime_dynamic_body_max_age_cap_ms"
    )
    if cap_ms is not None:
        cap_ms = float(cap_ms)
        # Zero is a sentinel: disable the kernel-side cap while keeping
        # body-local freshness checks active. Negative values are invalid.
        if cap_ms < 0.0:
            raise ValueError(
                "orchestrator.config.runtime_dynamic_body_max_age_cap_ms must be >= 0"
            )
        config["runtime_dynamic_body_max_age_cap_ns"] = int(cap_ms * 1_000_000.0)
    return config


def create_kernel(deployment: Deployment) -> KernelClient:
    """Create a kernel based on deployment adapter configuration.

    Tries to import the native kernel (manyforge_roboplan) first.
    Falls back to MockKernelClient for testing.
    """
    execution_adapter = deployment.adapters.get("execution")
    if (
        execution_adapter is not None
        and execution_adapter.adapter_id in _ROS2_CONTROL_EXECUTION_ADAPTER_IDS
    ):
        raise RuntimeError(
            "Deployment selected ROS 2 controller execution "
            f"({execution_adapter.adapter_id}), but manyforge_behavior.scripts.launch "
            "does not own ROS runtime wiring. Use Composer's ROS-capable "
            "runtime profile or the future ROS kernel factory instead."
        )

    planning_adapter = deployment.adapters.get("planning")
    if planning_adapter and planning_adapter.adapter_id == "planning.roboplan":
        try:
            # Try native kernel path
            import manyforge_roboplan_py
            from native_kernel_client import NativeKernelClient

            robot_config = deployment.robot_config
            group = robot_config.group_id
            if robot_config.srdf_path:
                extra_package_paths = (
                    []
                    if robot_config.package_paths
                    else default_package_paths()
                )
                resolved_model = resolve_robot_model(
                    urdf_path=robot_config.urdf_path,
                    srdf_path=robot_config.srdf_path,
                    package_paths=robot_config.package_paths,
                    deployment_path=deployment.source_path,
                    extra_package_paths=extra_package_paths,
                )
                bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
                    resolved_model.model_name,
                    group,
                    resolved_model.urdf_path,
                    resolved_model.srdf_path,
                    resolved_model.package_paths,
                    _native_kernel_config_from_deployment(deployment),
                )
            else:
                model_name = robot_config.urdf_path.rsplit("/", 1)[-1].replace(".urdf", "")
                bundle = manyforge_roboplan_py.create_native_kernel(
                    model_name, group, _native_kernel_config_from_deployment(deployment)
                )
            return NativeKernelClient(bundle)
        except ImportError:
            print("WARNING: manyforge_roboplan_py not available, using MockKernelClient")

    # Fallback
    return MockKernelClient()


def start_hmi(cycle_manager: CycleManager, runner: OrchestratorRunner):
    """Start the HMI server if available."""
    try:
        from manyforge_hmi import ViserHmiServer
        hmi = ViserHmiServer(
            cycle_manager=cycle_manager,
            host="127.0.0.1",
            port=8081,
        )
        if runner.program:
            hmi.load_program(runner.program)
        hmi.start()
        runner.set_scene_snapshot_provider(
            getattr(hmi, "get_cached_scene_snapshot", None) or hmi.render_scene
        )
        print("HMI started on http://127.0.0.1:8081")
        return hmi
    except ImportError:
        print("WARNING: manyforge_hmi not available, skipping HMI")
        return None


def main():
    parser = argparse.ArgumentParser(description="Launch a ManyForge program")
    parser.add_argument("--program", required=True, help="Path to program YAML")
    parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
    parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
    parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
    parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
    args = parser.parse_args()

    # 1. Load and validate artifacts
    print(f"Loading deployment: {args.deployment}")
    deployment = Deployment(args.deployment)

    print(f"Loading program: {args.program}")
    # Use catalog for program validation
    catalog = NodeCatalog(args.catalog)

    # Cross-validate
    program = Program(args.program, catalog)
    deployment.validate_against_program(program)
    deployment.validate_capabilities(program)

    # 2. Create kernel based on deployment adapter config
    kernel = create_kernel(deployment)

    # 3. Create runner
    runner = OrchestratorRunner(
        catalog_path=args.catalog,
        program_path=args.program,
        kernel=kernel,
        deployment_path=args.deployment,
    )
    runner.load()
    cycle_manager = CycleManager(runner, kernel)

    # 4. Optionally start HMI
    hmi = None
    if deployment.launch and deployment.launch.ui_backend and deployment.launch.ui_backend.enabled:
        hmi = start_hmi(cycle_manager, runner)

    # 5. Run
    if args.auto_run:
        print("Running program...")
        cycle_manager.run()
        while cycle_manager.state == CycleState.RUNNING:
            time.sleep(0.01)
        success = bool(
            runner._tree is not None
            and runner._tree.root.status == py_trees.common.Status.SUCCESS
        )
        status = "SUCCESS" if success else "FAILED"
        print(f"Program {status}")
        sys.exit(0 if success else 1)
    else:
        print("Ready. Waiting for HMI 'Run' button or Ctrl+C to exit.")
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            print("\nShutting down...")
            if hmi:
                hmi.stop()


if __name__ == "__main__":
    main()
