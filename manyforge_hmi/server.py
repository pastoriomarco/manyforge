"""ViserHmiServer - Main entry point for the Viser-based HMI."""

from collections import deque
import logging
import threading
import time
from typing import Any, Optional

import viser

logger = logging.getLogger(__name__)

_BACKEND_SYNC_EXCEPTIONS = (
    AttributeError,
    KeyError,
    RuntimeError,
    TypeError,
    ValueError,
)

from .state_bridge import StateBridge
from .control_panel import ControlPanel
from .robot_visualizer import RobotVisualizer
from .scene_renderer import SceneRenderer
from .program_panel import ProgramPanel
from .snapshot_service import SceneSnapshotService

from manyforge_behavior.cycle_manager import CycleManager


class ViserHmiServer:
    """Main Viser HMI server — wires all components together.

    This class creates and manages the Viser server and all HMI components,
    providing a unified interface for the ManyForge Human-Machine Interface.
    """

    TRACE_SCOPE = {
        "included": [
            "scene invalidation hints reaching the HMI server",
            "per-object backend reconciliation decisions in the Viser HMI update loop",
            "chosen render parent frame paths, scene node names, and node-presence checks",
        ],
        "excluded": [
            "browser-side WebGL/client rendering and pixel output",
            "planner/collision internals beyond backend scene mutation results",
            "continuous robot joint telemetry beyond execution snapshots",
            "persistent log storage; current trace is bounded in-memory only",
        ],
    }

    def __init__(
        self,
        cycle_manager: CycleManager,
        host: str = "127.0.0.1",
        port: int = 8081,
        urdf_path: Optional[str] = None,
        joint_names: Optional[list[str]] = None,
        package_dirs: Optional[list[str]] = None,
    ) -> None:
        """Initialize the ViserHmiServer.

        Args:
            cycle_manager: The cycle manager for lifecycle control.
            host: Host address to bind to.
            port: Port to bind to (default 8081; use 0 for random port).
            urdf_path: Optional path to robot URDF file.
            joint_names: Optional list of joint names for the robot.
            package_dirs: Directories to search for package:// URI resolution.
        """
        # Create the Viser server
        self._viser_server = viser.ViserServer(host=host, port=port)

        # Create the StateBridge
        self._bridge = StateBridge(cycle_manager)

        # Create the ControlPanel
        self._control_panel = ControlPanel(self._viser_server, self._bridge)

        # Create the SceneRenderer
        self._scene_renderer = SceneRenderer(self._viser_server)

        # Create the ProgramPanel
        self._program_panel = ProgramPanel(self._viser_server)

        # Create RobotVisualizer if URDF path provided
        self._robot_visualizer: Optional[RobotVisualizer] = None
        if urdf_path:
            self._robot_visualizer = RobotVisualizer(
                self._viser_server, urdf_path, joint_names,
                package_dirs=package_dirs,
            )

        # Add grid floor
        self._viser_server.scene.add_grid(
            name="/grid",
            width=2.0,
            height=2.0,
            cell_size=0.1,
            position=(0.0, 0.0, 0.0),
        )

        # Show world axes
        self._viser_server.scene.world_axes.visible = True

        # Update loop state
        self._running = False
        self._update_thread: Optional[threading.Thread] = None
        self._last_snapshot_sequence = 0
        self._stale_count: int = 0
        self._program = None
        self._scene_specs: dict[str, object] = {}
        self._link_viser_paths: dict[str, str] = {}
        self._frame_handles: dict[str, Any] = {}
        self._frame_visibility_overrides: dict[str, bool] = {}
        self._scene_trace: deque[dict[str, Any]] = deque(maxlen=4096)
        self._scene_snapshot_service = SceneSnapshotService(self._render_scene_sync)
        self._rebuild_link_viser_paths()

    def load_program(self, program) -> None:
        """Load a program: display in panel + render scene objects.

        Args:
            program: The Program object to load.
        """
        self._program = program

        # Display in program panel
        self._program_panel.load_program(program)

        # Cache static scene metadata. Runtime pose/frame always comes from backend.
        self._scene_specs = {
            resource.object_id: resource
            for resource in (program.scene or [])
        }
        self._rebuild_link_viser_paths()
        self._sync_tool_frames_from_backend()
        self._scene_renderer.clear()
        self.sync_scene_from_backend()
        self.sync_robot_from_backend(force=True)

    def start(self) -> None:
        """Start the update loop (poll status, update UI)."""
        if self._running:
            return

        self._running = True
        self._update_thread = threading.Thread(target=self._update_loop, daemon=True)
        self._update_thread.start()

    @property
    def is_stale(self) -> bool:
        """True if recent updates have been failing."""
        return self._stale_count > 3

    def _update_loop(self) -> None:
        """Background update loop that polls status and updates UI."""
        while self._running:
            try:
                self._update_once()
                self._stale_count = 0
            except Exception:
                logger.warning("HMI update error", exc_info=True)
                self._stale_count += 1
            time.sleep(0.1)  # 100ms update rate

    def _update_once(self) -> None:
        """Poll runtime state once and update the UI."""
        snapshot = self._bridge.execution_snapshot

        if self._control_panel:
            state = self._bridge.state
            velocity_scale = self._bridge.velocity_scale
            self._control_panel.update(state, velocity_scale, snapshot)

        self._sync_robot_from_snapshot(snapshot)
        self._sync_grippers_from_backend()
        self._sync_tool_frames_from_backend()

        if self._scene_specs:
            self.sync_scene_from_backend(snapshot_sequence=snapshot.sequence_number)

    def _sync_robot_from_snapshot(self, snapshot, *, force: bool = False) -> None:
        """Apply backend joint state to the robot visualizer."""
        if not self._robot_visualizer:
            return
        if not force and snapshot.sequence_number <= self._last_snapshot_sequence:
            return

        joint_names = self._robot_visualizer.joint_names
        if joint_names and snapshot.joint_positions_rad:
            if len(snapshot.joint_positions_rad) == len(joint_names):
                self._robot_visualizer.update_joints(
                    dict(zip(joint_names, snapshot.joint_positions_rad))
                )
            elif len(snapshot.joint_positions_rad) < len(joint_names):
                gripper_joint_names = self._configured_gripper_joint_names()
                arm_joint_names = [
                    joint_name
                    for joint_name in joint_names
                    if joint_name not in gripper_joint_names
                ]
                if len(snapshot.joint_positions_rad) == len(arm_joint_names):
                    self._robot_visualizer.update_joints(
                        dict(zip(arm_joint_names, snapshot.joint_positions_rad))
                    )
                else:
                    logger.debug(
                        "Skipping partial joint snapshot with %s values for visualizer joints=%s arm_joints=%s",
                        len(snapshot.joint_positions_rad),
                        len(joint_names),
                        len(arm_joint_names),
                    )
            self._last_snapshot_sequence = max(
                self._last_snapshot_sequence, snapshot.sequence_number
            )

    def _configured_gripper_joint_names(self) -> set[str]:
        """Return actuated gripper joints declared by deployment config."""
        try:
            gripper_configs = self._bridge.get_gripper_configs()
        except _BACKEND_SYNC_EXCEPTIONS:
            logger.debug("Could not fetch gripper configs from backend", exc_info=True)
            return set()

        joint_names: set[str] = set()
        for config in gripper_configs.values():
            joint_name = str(config.get("joint_name", "")).strip()
            if joint_name:
                joint_names.add(joint_name)
        return joint_names

    def _sync_grippers_from_backend(self) -> None:
        """Apply current actuated gripper positions to the robot visualizer."""
        if not self._robot_visualizer:
            return
        try:
            gripper_configs = self._bridge.get_gripper_configs()
        except _BACKEND_SYNC_EXCEPTIONS:
            logger.debug("Could not fetch gripper configs from backend", exc_info=True)
            return
        joint_updates: dict[str, float] = {}
        for gripper_id, config in gripper_configs.items():
            joint_name = str(config.get("joint_name", "")).strip()
            if not joint_name:
                continue
            try:
                status = self._bridge.get_gripper_status(gripper_id)
            except _BACKEND_SYNC_EXCEPTIONS:
                logger.debug("Could not fetch gripper status for '%s'", gripper_id, exc_info=True)
                continue
            if status.current_position is None:
                continue
            joint_updates[joint_name] = float(status.current_position)
        if joint_updates:
            self._robot_visualizer.update_joints(joint_updates)

    def sync_robot_from_backend(self, *, force: bool = False) -> None:
        """Synchronize the robot visualizer from backend state immediately."""
        self._sync_robot_from_snapshot(self._bridge.execution_snapshot, force=force)

    def _semantic_tool_frames(self) -> set[str]:
        """Collect deployment/program frames with semantic meaning."""
        frames: set[str] = set()
        if self._program is not None:
            tcp_frame = str(getattr(self._program.robot, "tcp_frame", "")).strip()
            if tcp_frame:
                frames.add(tcp_frame)
        try:
            gripper_configs = self._bridge.get_gripper_configs()
        except _BACKEND_SYNC_EXCEPTIONS:
            logger.debug("Could not fetch gripper configs for tool frame tracking", exc_info=True)
            return frames

        for config in gripper_configs.values():
            for key in ("mount_frame", "attach_frame", "tcp_frame"):
                frame_name = str(config.get(key, "")).strip()
                if frame_name:
                    frames.add(frame_name)
            for frame_name in config.get("auxiliary_frames", []) or []:
                frame_name_str = str(frame_name).strip()
                if frame_name_str:
                    frames.add(frame_name_str)
        return frames

    def _tracked_tool_frames(self) -> set[str]:
        """Collect all frames currently tracked in the viewer."""
        semantic_frames = self._semantic_tool_frames()
        extra_visible_frames = {
            frame_name
            for frame_name, visible in self._frame_visibility_overrides.items()
            if visible and frame_name not in semantic_frames
        }
        return semantic_frames | extra_visible_frames

    @staticmethod
    def _xyzw_to_wxyz(
        quaternion_xyzw: tuple[float, float, float, float],
    ) -> tuple[float, float, float, float]:
        x, y, z, w = quaternion_xyzw
        return (w, x, y, z)

    def _rebuild_link_viser_paths(self) -> None:
        """Build link-name -> Viser scene path map for attached-object rendering."""
        self._link_viser_paths = {}
        tracked_frames = sorted(self._tracked_tool_frames())
        try:
            if self._robot_visualizer:
                import viser.extras._urdf as _urdf_module

                _urdf_scene = self._robot_visualizer.viser_urdf._urdf.scene
                if _urdf_scene is not None:
                    for frame_name in tracked_frames:
                        try:
                            self._link_viser_paths[frame_name] = _urdf_module._viser_name_from_frame(
                                _urdf_scene, frame_name, "/robot/visual"
                            )
                        except Exception:
                            pass
        except Exception:
            logger.debug("Could not rebuild Viser link path map", exc_info=True)
        for frame_name in tracked_frames:
            self._link_viser_paths.setdefault(frame_name, f"/frames/{frame_name}")

    def _sync_tool_frames_from_backend(self) -> None:
        """Render configured tool/end-effector frames as standalone axes."""
        tracked_frames = self._tracked_tool_frames()
        for frame_name in list(self._frame_handles):
            if frame_name not in tracked_frames:
                self._frame_handles.pop(frame_name).remove()
                self._frame_visibility_overrides.pop(frame_name, None)
        self._rebuild_link_viser_paths()
        for frame_name in sorted(tracked_frames):
            handle = self._frame_handles.get(frame_name)
            if handle is None:
                handle = self._viser_server.scene.add_frame(
                    f"/frames/{frame_name}",
                    axes_length=0.08,
                    axes_radius=0.004,
                    origin_radius=0.01,
                    visible=False,
                )
                self._frame_handles[frame_name] = handle
            try:
                pose = self._bridge.get_link_pose(frame_name, "universe")
            except _BACKEND_SYNC_EXCEPTIONS:
                logger.debug("Could not fetch tool frame pose for '%s'", frame_name, exc_info=True)
                handle.visible = False
                continue
            if pose is None:
                handle.visible = False
                continue
            handle.position = tuple(float(value) for value in pose["position_m"])
            handle.wxyz = self._xyzw_to_wxyz(tuple(float(value) for value in pose["orientation_xyzw"]))
            if frame_name in self._semantic_tool_frames():
                visible = self._frame_visibility_overrides.get(frame_name, True)
            else:
                visible = self._frame_visibility_overrides.get(frame_name, False)
            handle.visible = bool(visible)

    def get_frame_visibility(self, frame_name: str) -> bool:
        """Return the current viewer visibility for a named frame."""
        if frame_name in self._semantic_tool_frames():
            return self._frame_visibility_overrides.get(frame_name, True)
        return self._frame_visibility_overrides.get(frame_name, False)

    def set_frame_visibility(self, frame_name: str, visible: bool) -> None:
        """Set viewer visibility for a named frame without mutating deployment semantics."""
        frame_name = str(frame_name).strip()
        if not frame_name:
            raise ValueError("frame_name must not be empty")

        semantic_frames = self._semantic_tool_frames()
        if frame_name in semantic_frames:
            if visible:
                self._frame_visibility_overrides.pop(frame_name, None)
            else:
                self._frame_visibility_overrides[frame_name] = False
        else:
            if visible:
                self._frame_visibility_overrides[frame_name] = True
            else:
                self._frame_visibility_overrides.pop(frame_name, None)

        self._sync_tool_frames_from_backend()

    def request_scene_sync(
        self,
        *,
        reason: str = "callback",
        operation: Optional[str] = None,
        object_id: Optional[str] = None,
    ) -> None:
        """Compatibility hook for callback-based invalidation paths."""
        self._record_scene_trace(
            event="invalidation_hint",
            object_id=object_id,
            reason=reason,
            operation=operation,
        )

    def _record_scene_trace(self, *, event: str, object_id: Optional[str] = None, **data: Any) -> None:
        entry = {
            "timestamp_ns": time.monotonic_ns(),
            "source": "hmi",
            "event": event,
            "object_id": object_id,
        }
        entry.update(data)
        self._scene_trace.append(entry)
        logger.debug("scene_trace[%s] object=%s", event, object_id, extra={"scene_trace": entry})

    def get_scene_trace(self, limit: Optional[int] = None) -> list[dict]:
        """Return recent HMI-side scene reconciliation events."""
        trace = list(self._scene_trace)
        if limit is not None and limit >= 0:
            return trace[-limit:]
        return trace

    def clear_scene_trace(self) -> None:
        """Clear retained HMI-side scene reconciliation events."""
        self._scene_trace.clear()

    def sync_scene_from_backend(self, *, snapshot_sequence: Optional[int] = None) -> None:
        """Render known program objects from backend-authoritative state."""
        if not self._scene_specs:
            return

        for object_id in self._scene_renderer.object_ids:
            if object_id not in self._scene_specs:
                render = self._scene_renderer.remove_object(object_id)
                self._record_scene_trace(
                    event="remove_orphaned_render",
                    object_id=object_id,
                    snapshot_sequence=snapshot_sequence,
                    render=render,
                )

        for object_id, resource in self._scene_specs.items():
            state = self._bridge.get_object_state(object_id)
            if not state.exists:
                render = self._scene_renderer.remove_object(object_id)
                self._record_scene_trace(
                    event="reconcile_absent",
                    object_id=object_id,
                    snapshot_sequence=snapshot_sequence,
                    backend_state={
                        "exists": state.exists,
                        "attached": state.attached,
                        "frame_id": state.frame_id,
                    },
                    render=render,
                )
                continue

            frame_id = state.frame_id or "universe"
            frame_path = self._link_viser_paths.get(frame_id)
            if frame_path:
                pose = self._bridge.get_object_pose(object_id, frame_id)
            else:
                frame_id = "universe"
                pose = self._bridge.get_object_pose(object_id, frame_id)
            world_pose = self._bridge.get_object_pose(object_id, "universe")
            if pose is None:
                self._record_scene_trace(
                    event="reconcile_pose_missing",
                    object_id=object_id,
                    snapshot_sequence=snapshot_sequence,
                    backend_state={
                        "exists": state.exists,
                        "attached": state.attached,
                        "frame_id": state.frame_id,
                    },
                    query_frame=frame_id,
                    frame_path=frame_path,
                    pose_in_frame=None,
                    pose_in_universe=world_pose,
                )
                continue

            position = tuple(pose["position_m"])
            orientation = tuple(pose["orientation_xyzw"])
            shape_type = resource.shape.get("type")
            if shape_type == "box":
                dimensions = tuple(resource.shape.get("box_dimensions_m", [0.1, 0.1, 0.1]))
                render = self._scene_renderer.sync_box(
                    object_id,
                    dimensions,
                    position,
                    orientation,
                    frame_path=frame_path,
                )
            elif shape_type == "sphere":
                radius = resource.shape.get("sphere_radius_m", 0.05)
                render = self._scene_renderer.sync_sphere(
                    object_id,
                    radius,
                    position,
                    frame_path=frame_path,
                )
            else:
                self._record_scene_trace(
                    event="reconcile_unsupported_shape",
                    object_id=object_id,
                    snapshot_sequence=snapshot_sequence,
                    backend_state={
                        "exists": state.exists,
                        "attached": state.attached,
                        "frame_id": state.frame_id,
                    },
                    query_frame=frame_id,
                    frame_path=frame_path,
                    pose_in_frame={
                        "position_m": list(position),
                        "orientation_xyzw": list(orientation),
                    },
                    pose_in_universe=world_pose,
                    shape_type=shape_type,
                )
                continue

            if render.get("action") != "unchanged":
                self._record_scene_trace(
                    event="reconcile_render",
                    object_id=object_id,
                    snapshot_sequence=snapshot_sequence,
                    backend_state={
                        "exists": state.exists,
                        "attached": state.attached,
                        "frame_id": state.frame_id,
                    },
                    query_frame=frame_id,
                    frame_path=frame_path,
                    pose_in_frame={
                        "position_m": list(position),
                        "orientation_xyzw": list(orientation),
                    },
                    pose_in_universe=world_pose,
                    shape_type=shape_type,
                    render=render,
                )

    def get_object_render_diagnostics(self, object_id: str) -> dict:
        """Return backend-vs-render diagnostics for a scene object."""
        state = self._bridge.get_object_state(object_id)
        if not state.exists:
            return {
                "object_id": object_id,
                "exists": False,
                "state": {
                    "exists": state.exists,
                    "attached": state.attached,
                    "frame_id": state.frame_id,
                },
                "backend_pose_in_frame": None,
                "backend_pose_in_universe": None,
                "frame_path": None,
                "render": self._scene_renderer.get_render_info(object_id),
            }

        frame_id = state.frame_id or "universe"
        frame_path = self._link_viser_paths.get(frame_id)
        query_frame = frame_id if frame_path else "universe"
        return {
            "object_id": object_id,
            "exists": True,
            "state": {
                "exists": state.exists,
                "attached": state.attached,
                "frame_id": state.frame_id,
            },
            "backend_pose_in_frame": self._bridge.get_object_pose(object_id, query_frame),
            "backend_pose_in_universe": self._bridge.get_object_pose(object_id, "universe"),
            "frame_path": frame_path,
            "render": self._scene_renderer.get_render_info(object_id),
        }

    def stop(self) -> None:
        """Stop the server."""
        self._running = False
        if self._update_thread:
            self._update_thread.join(timeout=1.0)
            self._update_thread = None
        self._scene_snapshot_service.shutdown()
        self._viser_server.stop()

    @property
    def viser_server(self) -> viser.ViserServer:
        """Access the underlying Viser server (for testing)."""
        return self._viser_server

    @property
    def bridge(self) -> StateBridge:
        """Access the StateBridge."""
        return self._bridge

    @property
    def control_panel(self) -> ControlPanel:
        """Access the ControlPanel."""
        return self._control_panel

    @property
    def scene_renderer(self) -> SceneRenderer:
        """Access the SceneRenderer."""
        return self._scene_renderer

    @property
    def program_panel(self) -> ProgramPanel:
        """Access the ProgramPanel."""
        return self._program_panel

    @property
    def robot_visualizer(self) -> Optional[RobotVisualizer]:
        """Access the RobotVisualizer if created."""
        return self._robot_visualizer

    def _render_scene_sync(
        self,
        width: int = 640,
        height: int = 480,
        format: str = "jpeg",
    ) -> Optional[bytes]:
        """Render the current Viser scene as an image.

        Requires at least one connected Viser client. Returns JPEG or PNG
        bytes, or None if no client is connected.
        """
        clients = self._viser_server.get_clients()
        if not clients:
            return None

        # Use the first connected client
        client = next(iter(clients.values()))
        try:
            import io
            import numpy as np
            from PIL import Image

            rgb_array = client.get_render(height=height, width=width)
            img = Image.fromarray(rgb_array)
            buf = io.BytesIO()
            img_format = "JPEG" if format.lower() == "jpeg" else "PNG"
            img.save(buf, format=img_format)
            return buf.getvalue()
        except Exception:
            logger.warning("Failed to render scene", exc_info=True)
            return None

    def render_scene(
        self,
        width: int = 640,
        height: int = 480,
        format: str = "jpeg",
    ) -> Optional[bytes]:
        """Blocking one-shot render for debug/manual API routes."""
        return self._render_scene_sync(width=width, height=height, format=format)

    def get_cached_scene_snapshot(
        self,
        width: int = 640,
        height: int = 480,
        format: str = "jpeg",
    ) -> Optional[bytes]:
        """Return a cached scene snapshot without blocking the caller."""
        return self._scene_snapshot_service.get_snapshot(
            width=width,
            height=height,
            format=format,
        )
