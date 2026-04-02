#!/usr/bin/env python3
"""ManyForge launcher — reads deployment + program artifacts and starts all components."""

from __future__ import annotations

import argparse
import sys
import time

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import KernelClient, MockKernelClient
from manyforge_behavior.program import Program
from manyforge_behavior.runner import OrchestratorRunner


def create_kernel(deployment: Deployment) -> KernelClient:
    """Create a kernel based on deployment adapter configuration.

    Tries to import the native kernel (manyforge_roboplan) first.
    Falls back to MockKernelClient for testing.
    """
    planning_adapter = deployment.adapters.get("planning")
    if planning_adapter and planning_adapter.adapter_id == "planning.roboplan":
        try:
            # Try native kernel path
            import manyforge_roboplan_py
            from native_kernel_client import NativeKernelClient

            # Extract model/group from robot config
            urdf_stem = deployment.robot_config.urdf_path.rsplit("/", 1)[-1].replace(".urdf", "")
            group = deployment.robot_config.group_id
            bundle = manyforge_roboplan_py.create_native_kernel(urdf_stem, group)
            return NativeKernelClient(bundle)
        except ImportError:
            print("WARNING: manyforge_roboplan_py not available, using MockKernelClient")

    # Fallback
    return MockKernelClient()


def start_hmi(deployment: Deployment, kernel: KernelClient, runner: OrchestratorRunner):
    """Start the HMI server if available."""
    try:
        from manyforge_hmi import ViserHmiServer
        hmi = ViserHmiServer(
            kernel=kernel,
            host="0.0.0.0",
            port=8080,
        )
        if runner.program:
            hmi.load_program(runner.program)
        hmi.start()
        print("HMI started on http://0.0.0.0:8080")
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
    deployment.validate_against_program(Program(args.program, catalog))
    deployment.validate_capabilities()

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
    runner.build_tree()
    runner.apply_scene()

    # 4. Optionally start HMI
    hmi = None
    if deployment.launch and deployment.launch.ui_backend and deployment.launch.ui_backend.enabled:
        hmi = start_hmi(deployment, kernel, runner)

    # 5. Run
    if args.auto_run:
        print("Running program...")
        success = runner.run(max_ticks=args.max_ticks)
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
