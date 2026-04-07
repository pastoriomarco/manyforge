#!/usr/bin/env python3
"""Pick-and-place demo with real RoboPlan planning and Viser 3D visualization.

Runs inside the Docker container:
    docker compose run --rm -p 8081:8081 dev bash -c \\
        './scripts/test-all.sh roboplan && python3 examples/pick_and_place_demo.py'

Then open http://localhost:8081 in your browser to see the 3D scene.
Click "Run" in the Viser control panel to start the pick-and-place sequence.

What it does:
  1. Creates a UR5 robot with real RoboPlan motion planning (RRT + TOPPRA)
  2. Starts a Viser 3D server showing the robot URDF, collision objects, and control panel
  3. Loads a pick-and-place behavior tree program with scene objects from manymove examples
  4. Waits for user to click "Run" — then executes through the CycleManager
  5. Keeps the server running so you can inspect the scene
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
import yaml

# Unbuffered output so prints appear immediately
os.environ["PYTHONUNBUFFERED"] = "1"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO_ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = str(REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
PROGRAM_PATH = str(REPO_ROOT / "examples" / "pick_and_place.program.yaml")
DEPLOYMENT_PATH = str(REPO_ROOT / "examples" / "pick_and_place.deployment.yaml")

# URDF from roboplan example models (installed in the container)
try:
    from roboplan import example_models
    MODELS_DIR = example_models.get_package_models_dir()
    URDF_PATH = str(Path(MODELS_DIR) / "ur_robot_model" / "ur5_gripper.urdf")
except ImportError:
    print("ERROR: roboplan not available. Run inside the Docker container:")
    print("  docker compose run --rm -p 8081:8081 dev python3 examples/pick_and_place_demo.py")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Check manyforge_roboplan_py
# ---------------------------------------------------------------------------
# The pybind11 .so lives in /tmp/manyforge-build/roboplan/ after test-all.sh builds it.
# The entrypoint may not have added it to PYTHONPATH if it didn't exist at startup.
_ROBOPLAN_BUILD = Path("/tmp/manyforge-build/roboplan")
_ROBOPLAN_PYTHON = REPO_ROOT / "manyforge_roboplan" / "python"
for _p in [_ROBOPLAN_BUILD, _ROBOPLAN_PYTHON]:
    if _p.is_dir() and str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

try:
    import manyforge_roboplan_py
    from native_kernel_client import NativeKernelClient
except ImportError:
    print("ERROR: manyforge_roboplan_py not built. Run tests first:")
    print("  ./scripts/test-all.sh roboplan")
    sys.exit(1)

# ---------------------------------------------------------------------------
# ManyForge imports
# ---------------------------------------------------------------------------
import py_trees
from manyforge_behavior.cycle_manager import CycleManager
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_hmi import ViserHmiServer

# UR5 joint names
JOINT_NAMES = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]


def _load_simulator_initial_joint_positions() -> list[float] | None:
    """Read explicit simulator bootstrap joint positions from the example deployment."""
    try:
        with open(DEPLOYMENT_PATH) as f:
            data = yaml.safe_load(f)
    except Exception:
        return None

    execution = (data or {}).get("adapters", {}).get("execution", {})
    if execution.get("adapter_id") != "execution.roboplan_native":
        return None
    if execution.get("config", {}).get("mode") != "simulated":
        return None
    positions = execution.get("config", {}).get("initial_joint_positions_rad")
    if not isinstance(positions, list) or not positions:
        return None
    return [float(v) for v in positions]


def main():
    # --- 1. Create the native kernel (real RoboPlan planning) ---
    print("Creating UR5 robot with RoboPlan backends...")
    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    kernel = NativeKernelClient(bundle)

    # --- 2. Load the pick-and-place program ---
    print(f"Loading program: {Path(PROGRAM_PATH).name}", flush=True)
    runner = OrchestratorRunner(
        catalog_path=CATALOG_PATH,
        program_path=PROGRAM_PATH,
        kernel=kernel,
        overrides={"velocity_scale": 0.5},
    )
    runner.load()
    cycle_manager = CycleManager(runner, kernel)

    # --- 3. Start Viser HMI ---
    print("Starting Viser 3D server on http://0.0.0.0:8081 ...", flush=True)
    hmi = ViserHmiServer(
        cycle_manager=cycle_manager,
        host="0.0.0.0",
        port=8081,
        urdf_path=URDF_PATH,
        joint_names=JOINT_NAMES,
    )

    # Load program into HMI (scene objects + program panel)
    hmi.load_program(runner.program)

    # --- 4. Register scene edit callback as backend-scene invalidation hint ---
    bb = py_trees.blackboard.Client(name="demo_scene_callback")

    def on_scene_edit(operation: str, request):
        """Request a backend-authoritative scene resync."""
        hmi.request_scene_sync()
        print(f"  Scene sync requested after {operation} '{request.object_id}'", flush=True)

    bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.WRITE)
    bb.scene_edit_callback = on_scene_edit

    def refresh_viser_scene():
        hmi.scene_renderer.clear()
        hmi.sync_scene_from_backend()
        hmi.sync_robot_from_backend(force=True)

    cycle_manager.reset()
    initial_positions = _load_simulator_initial_joint_positions()
    if initial_positions:
        bundle.set_joint_positions(initial_positions)
        print("Applied simulator-only initial joint positions from deployment.", flush=True)
    cycle_manager._on_run_from_idle = refresh_viser_scene
    hmi.sync_scene_from_backend()
    hmi.sync_robot_from_backend(force=True)

    # --- 5. Start HMI and wait ---
    hmi.start()
    print()
    print("Open the Viser URL in your browser to see the 3D scene.")
    print("Click 'Run Program' in the control panel to start.")
    print("Press Ctrl+C to exit.")

    if "--auto" in sys.argv:
        # Auto-run for testing (no user interaction)
        time.sleep(0.5)
        cycle_manager.run()

    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nShutting down...")
        hmi.stop()


if __name__ == "__main__":
    main()
