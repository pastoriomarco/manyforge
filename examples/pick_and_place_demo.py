#!/usr/bin/env python3
"""Pick-and-place demo with real RoboPlan planning and Viser 3D visualization.

Runs inside the Docker container:
    docker compose run --rm -p 8081:8080 dev bash -c \\
        './scripts/test-all.sh roboplan && python3 examples/pick_and_place_demo.py'

Then open http://localhost:8081 in your browser to see the 3D scene.
Click "Run Program" in the Viser control panel to start the pick-and-place sequence.

What it does:
  1. Creates a UR5 robot with real RoboPlan motion planning (RRT + TOPPRA)
  2. Starts a Viser 3D server showing the robot URDF, collision objects, and control panel
  3. Loads a pick-and-place behavior tree program with scene objects from manymove examples
  4. Waits for user to click "Run Program" — then executes with animated trajectory playback
  5. Keeps the server running so you can inspect the scene
"""

from __future__ import annotations

import os
import sys
import threading
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
    print("  docker compose run --rm -p 8081:8080 dev python3 examples/pick_and_place_demo.py")
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

    # --- 2. Start Viser HMI ---
    print("Starting Viser 3D server on http://0.0.0.0:8080 ...", flush=True)
    hmi = ViserHmiServer(
        kernel=kernel,
        host="0.0.0.0",
        port=8080,
        urdf_path=URDF_PATH,
        joint_names=JOINT_NAMES,
    )

    # --- 3. Load the pick-and-place program ---
    print(f"Loading program: {Path(PROGRAM_PATH).name}", flush=True)
    runner = OrchestratorRunner(
        catalog_path=CATALOG_PATH,
        program_path=PROGRAM_PATH,
        kernel=kernel,
        overrides={"velocity_scale": 0.5},
    )
    runner.load()
    runner.build_tree()

    # Load program into HMI (scene objects + program panel)
    hmi.load_program(runner._program)

    # Apply scene to the planning context (collision objects)
    runner.apply_scene()

    # --- 4. Set robot to rest position (both visualization AND planner) ---
    bundle.set_joint_positions(list(REST_POSE.values()))
    if hmi.robot_visualizer:
        hmi.robot_visualizer.update_joints(REST_POSE)

    # --- 5. Register trajectory animation callback ---
    # When a move completes, retrieve the trajectory and animate the robot.
    def on_move_complete(move_id: str, kernel_client):
        traj = kernel_client.get_cached_trajectory(move_id)
        if traj and hmi.robot_visualizer:
            print(f"  Animating: {move_id} ({len(traj.waypoints)} waypoints, "
                  f"{traj.waypoints[-1].time_from_start_s:.2f}s)", flush=True)
            hmi.robot_visualizer.animate_trajectory(
                traj.joint_names, traj.waypoints, speed_scale=1.0
            )

    # Store callback on blackboard for MoveManipulatorAction to find
    bb = py_trees.blackboard.Client(name="demo_callback")
    bb.register_key(key="move_complete_callback", access=py_trees.common.Access.WRITE)
    bb.move_complete_callback = on_move_complete

    # --- 5b. Register scene edit callback for Viser sync ---
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

    # --- 6. Wire up the Run Program button ---
    def run_program():
        """Run the behavior tree in a background thread."""
        def _run():
            print("Running pick-and-place program...", flush=True)
            t0 = time.time()
            success = runner.run(max_ticks=300, tick_period_s=0.0)
            elapsed = time.time() - t0

            final_status = kernel.get_status()
            print(f"  {final_status.execution_id} moves planned and executed "
                  f"in {elapsed:.2f}s", flush=True)

            # Show final TCP pose
            pose = bundle.get_link_pose("tool0", "base_link")
            if pose:
                p = pose["position_m"]
                print(f"  Final TCP position: ({p[0]:.3f}, {p[1]:.3f}, {p[2]:.3f})",
                      flush=True)

            hmi.control_panel.update()
            status = "succeeded" if success else "FAILED"
            print(f"Program {status} ({final_status.state.value})", flush=True)

            # Re-enable Run button for another run
            hmi.control_panel.run_button.disabled = False

        threading.Thread(target=_run, daemon=True).start()

    hmi.control_panel.set_run_callback(run_program)

    # --- 7. Start HMI and wait ---
    hmi.start()
    print()
    print("Open the Viser URL in your browser to see the 3D scene.")
    print("Click 'Run Program' in the control panel to start.")
    print("Press Ctrl+C to exit.")

    if "--auto" in sys.argv:
        # Auto-run for testing (no user interaction)
        time.sleep(0.5)
        run_program()

    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nShutting down...")
        hmi.stop()


if __name__ == "__main__":
    main()
