"""ViserHmiServer - Main entry point for the Viser-based HMI."""

from collections import deque
import logging
import threading
import time
from typing import Any, Optional

import viser

logger = logging.getLogger(__name__)

from .state_bridge import StateBridge
from .control_panel import ControlPanel
from .robot_visualizer import RobotVisualizer
from .scene_renderer import SceneRenderer
from .program_panel import ProgramPanel

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
        host: str = "0.0.0.0",
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
        self._scene_trace: deque[dict[str, Any]] = deque(maxlen=4096)
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

        if self._scene_specs:
            self.sync_scene_from_backend(snapshot_sequence=snapshot.sequence_number)

    def _sync_robot_from_snapshot(self, snapshot, *, force: bool = False) -> None:
        """Apply backend joint state to the robot visualizer."""
        if not self._robot_visualizer:
            return
        if not force and snapshot.sequence_number <= self._last_snapshot_sequence:
            return

        joint_names = self._robot_visualizer.joint_names
        if joint_names and len(snapshot.joint_positions_rad) == len(joint_names):
            self._robot_visualizer.update_joints(
                dict(zip(joint_names, snapshot.joint_positions_rad))
            )
            self._last_snapshot_sequence = max(
                self._last_snapshot_sequence, snapshot.sequence_number
            )

    def sync_robot_from_backend(self, *, force: bool = False) -> None:
        """Synchronize the robot visualizer from backend state immediately."""
        self._sync_robot_from_snapshot(self._bridge.execution_snapshot, force=force)

    def _rebuild_link_viser_paths(self) -> None:
        """Build link-name -> Viser scene path map for attached-object rendering."""
        self._link_viser_paths = {}
        if not self._robot_visualizer:
            return
        try:
            import viser.extras._urdf as _urdf_module

            _urdf_scene = self._robot_visualizer.viser_urdf._urdf.scene
            if _urdf_scene is None:
                return
            for frame_name in ("tool0", "wrist_3_link"):
                try:
                    self._link_viser_paths[frame_name] = _urdf_module._viser_name_from_frame(
                        _urdf_scene, frame_name, "/robot/visual"
                    )
                except Exception:
                    pass
        except Exception:
            logger.debug("Could not rebuild Viser link path map", exc_info=True)

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
