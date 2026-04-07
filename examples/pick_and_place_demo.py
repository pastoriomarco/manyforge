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

# Unbuffered output so prints appear immediately
os.environ["PYTHONUNBUFFERED"] = "1"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO_ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = str(REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
PROGRAM_PATH = str(REPO_ROOT / "examples" / "pick_and_place.program.yaml")

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

# UR rest pose from manymove bt_client_ur.cpp
REST_POSE = {
    "shoulder_pan_joint": 0.0,
    "shoulder_lift_joint": -1.57,
    "elbow_joint": 1.57,
    "wrist_1_joint": -1.57,
    "wrist_2_joint": -1.57,
    "wrist_3_joint": 0.0,
}


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

    # --- 4. Set robot to rest position (both visualization AND planner) ---
    bundle.set_joint_positions(list(REST_POSE.values()))
    if hmi.robot_visualizer:
        hmi.robot_visualizer.update_joints(REST_POSE)

    # --- 5. Register scene edit callback for Viser sync ---
    bb = py_trees.blackboard.Client(name="demo_scene_callback")
    # Build link-name → Viser scene path map for attaching objects to robot frames
    _link_viser_paths: dict[str, str] = {}
    if hmi.robot_visualizer:
        import viser.extras._urdf as _urdf_module
        _urdf_scene = hmi.robot_visualizer.viser_urdf._urdf.scene
        if _urdf_scene is not None:
            for frame_name in ("tool0", "wrist_3_link"):
                try:
                    _link_viser_paths[frame_name] = _urdf_module._viser_name_from_frame(
                        _urdf_scene, frame_name, "/robot/visual"
                    )
                except Exception:
                    pass

    def on_scene_edit(operation: str, request):
        """Update Viser scene when planning scene changes."""
        renderer = hmi.scene_renderer
        if operation == "remove":
            renderer.remove_object(request.object_id)
            print(f"  Scene: removed '{request.object_id}'", flush=True)
        elif operation == "add":
            frame = request.frame_id or "universe"
            orient = tuple(request.pose_orientation_xyzw or [0, 0, 0, 1])
            pos = tuple(request.pose_position_m or [0, 0, 0])
            if frame != "universe":
                # Attach to robot link frame in Viser scene graph
                viser_path = _link_viser_paths.get(frame)
                if viser_path and request.shape_type == "box" and request.box_dimensions_m:
                    renderer.add_box_on_frame(
                        request.object_id, viser_path,
                        tuple(request.box_dimensions_m), pos, orient,
                    )
                    print(f"  Scene: '{request.object_id}' attached to {frame}", flush=True)
                else:
                    print(f"  Scene: '{request.object_id}' attached to {frame} (no Viser path)", flush=True)
                return
            if request.shape_type == "box" and request.box_dimensions_m:
                renderer.add_box(request.object_id, tuple(request.box_dimensions_m),
                                 pos, orient)
            elif request.shape_type == "sphere" and request.sphere_radius_m:
                renderer.add_sphere(request.object_id, request.sphere_radius_m, pos)
            print(f"  Scene: added '{request.object_id}' at {pos}", flush=True)

    bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.WRITE)
    bb.scene_edit_callback = on_scene_edit

    def refresh_viser_scene():
        hmi.scene_renderer.clear()
        if runner.program:
            hmi.scene_renderer.load_from_program(runner.program.scene)

    cycle_manager._on_run_from_idle = refresh_viser_scene

    # --- 6. Start HMI and wait ---
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
