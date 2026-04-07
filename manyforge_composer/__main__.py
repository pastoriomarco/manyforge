"""python -m manyforge_composer --catalog-path <path> [--host 0.0.0.0] [--port 9000] [--hmi-port 8081]"""
from __future__ import annotations

import argparse
import logging

import py_trees
import uvicorn

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app

logger = logging.getLogger(__name__)

# UR5 joint names and rest pose (from manymove bt_client_ur.cpp)
JOINT_NAMES = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]
REST_POSE = {
    "shoulder_pan_joint": 0.0,
    "shoulder_lift_joint": -1.57,
    "elbow_joint": 1.57,
    "wrist_1_joint": -1.57,
    "wrist_2_joint": -1.57,
    "wrist_3_joint": 0.0,
}


def build_program_loaded_callback(
    *,
    host: str,
    hmi_port: int,
    urdf_path: str | None,
    package_dirs: list[str],
    bundle,
    hmi_server_factory=None,
):
    """Build the Composer program-load callback that wires Viser HMI state.

    Extracted for testability so backend tests can validate the callback
    without spawning uvicorn or a real Viser server.
    """
    if hmi_server_factory is None:
        from manyforge_hmi.server import ViserHmiServer
        hmi_server_factory = ViserHmiServer

    _hmi_server = None

    def on_program_loaded(st: ComposerState):
        nonlocal _hmi_server

        # Stop previous HMI if reloading a program
        if _hmi_server is not None:
            _hmi_server.stop()

        _hmi_server = hmi_server_factory(
            cycle_manager=st.cycle_manager,
            host=host,
            port=hmi_port,
            urdf_path=urdf_path,
            joint_names=JOINT_NAMES,
            package_dirs=package_dirs or None,
        )
        _hmi_server.load_program(st.program)

        # Set initial joint positions (both planner and visualization)
        if bundle is not None:
            bundle.set_joint_positions(list(REST_POSE.values()))
            logger.info("Set planner joint positions to rest pose")
        if _hmi_server.robot_visualizer:
            _hmi_server.robot_visualizer.update_joints(REST_POSE)

        # --- Register scene_edit_callback for Viser scene sync ---
        # Build link-name -> Viser scene path map for frame-attached objects
        _link_viser_paths: dict[str, str] = {}
        if _hmi_server.robot_visualizer:
            try:
                import viser.extras._urdf as _urdf_module
                _urdf_scene = _hmi_server.robot_visualizer.viser_urdf._urdf.scene
                if _urdf_scene is not None:
                    for frame_name in ("tool0", "wrist_3_link"):
                        try:
                            _link_viser_paths[frame_name] = _urdf_module._viser_name_from_frame(
                                _urdf_scene, frame_name, "/robot/visual"
                            )
                        except Exception:
                            pass
            except Exception:
                pass

        def on_scene_edit(operation: str, request):
            renderer = _hmi_server.scene_renderer
            if operation == "remove":
                renderer.remove_object(request.object_id)
                logger.debug("Scene: removed '%s'", request.object_id)
                return
            # Add operation
            frame = request.frame_id or "universe"
            pos = tuple(request.pose_position_m or [0, 0, 0])
            orient = tuple(request.pose_orientation_xyzw or [0, 0, 0, 1])
            if frame != "universe":
                viser_path = _link_viser_paths.get(frame)
                if viser_path and request.shape_type == "box" and request.box_dimensions_m:
                    renderer.add_box_on_frame(
                        request.object_id, viser_path,
                        tuple(request.box_dimensions_m), pos, orient,
                    )
                logger.debug("Scene: '%s' attached to %s", request.object_id, frame)
                return
            if request.shape_type == "box" and request.box_dimensions_m:
                renderer.add_box(request.object_id, tuple(request.box_dimensions_m), pos, orient)
            elif request.shape_type == "sphere" and request.sphere_radius_m:
                renderer.add_sphere(request.object_id, request.sphere_radius_m, pos)
            logger.debug("Scene: added '%s' at %s", operation, request.object_id)

        bb = py_trees.blackboard.Client(name="composer_scene_sync")
        bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.WRITE)
        bb.scene_edit_callback = on_scene_edit

        # Reset Viser scene on each run from IDLE (e.g. after reset)
        # so objects return to their initial YAML positions.
        def _refresh_viser_scene():
            if _hmi_server and _hmi_server.scene_renderer:
                _hmi_server.scene_renderer.clear()
                if st.program:
                    _hmi_server.scene_renderer.load_from_program(st.program.scene)

        st.cycle_manager._on_run_from_idle = _refresh_viser_scene

        _hmi_server.start()
        logger.info("Viser HMI started on port %d", hmi_port)

    return on_program_loaded


def _make_kernel():
    """Try NativeKernelClient backed by real RoboPlan C++; fall back to Mock.

    Returns:
        (kernel, bundle) — bundle is None when using MockKernelClient.
    """
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient

        bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
        kernel = NativeKernelClient(bundle)
        logger.info("Using NativeKernelClient (real RoboPlan planning)")
        return kernel, bundle
    except Exception as e:
        logger.info("NativeKernelClient unavailable (%s), using MockKernelClient", e)
        return MockKernelClient(execute_duration_s=1.5), None


def _resolve_roboplan_paths():
    """Get URDF path and package share dir from installed roboplan.

    Returns:
        (urdf_path, package_share_dir) or (None, None) if not available.
    """
    try:
        from roboplan import example_models

        models_dir = example_models.get_package_models_dir()
        share_dir = example_models.get_package_share_dir()
        urdf_path = str(models_dir / "ur_robot_model" / "ur5_gripper.urdf")
        return urdf_path, str(share_dir)
    except Exception:
        return None, None


def main():
    parser = argparse.ArgumentParser(description="ManyForge Composer Backend")
    parser.add_argument("--catalog-path", required=True, help="Path to node_catalog.yaml")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--hmi-port", type=int, default=None,
                        help="Enable Viser 3D HMI on this port (e.g. 8081)")
    parser.add_argument("--urdf", default=None,
                        help="Path to robot URDF (auto-resolved from roboplan if omitted)")
    parser.add_argument("--package-dir", action="append", default=[],
                        help="Directory for package:// URI resolution (repeatable)")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    args = parser.parse_args()

    setup_logging(args.log_level)

    kernel, bundle = _make_kernel()

    # Auto-resolve URDF and package dirs from roboplan if not specified via CLI
    urdf_path = args.urdf
    package_dirs = args.package_dir or []
    if not urdf_path or not package_dirs:
        roboplan_urdf, roboplan_share = _resolve_roboplan_paths()
        if not urdf_path and roboplan_urdf:
            urdf_path = roboplan_urdf
            logger.info("URDF auto-resolved from roboplan: %s", urdf_path)
        if not package_dirs and roboplan_share:
            package_dirs = [roboplan_share]
            logger.info("Package dir auto-resolved from roboplan: %s", roboplan_share)

    state = ComposerState(catalog_path=args.catalog_path, kernel=kernel)

    # Wire up Viser HMI if --hmi-port is set
    if args.hmi_port is not None:
        state._on_program_loaded = build_program_loaded_callback(
            host=args.host,
            hmi_port=args.hmi_port,
            urdf_path=urdf_path,
            package_dirs=package_dirs,
            bundle=bundle,
        )

    app = create_app(state)
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
