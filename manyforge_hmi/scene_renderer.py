"""SceneRenderer - Renders collision objects in the 3D scene."""

import math
import os
from pathlib import Path
from typing import Optional

import numpy as np
import trimesh
import viser


class SceneRenderer:
    """Renders collision objects from program scene in Viser.

    This class handles rendering boxes, spheres, and other collision objects
    in the Viser 3D scene based on the program's scene resources.
    """

    def __init__(self, server: viser.ViserServer) -> None:
        """Initialize the SceneRenderer.

        Args:
            server: The Viser server instance.
        """
        self._server = server
        self._objects: dict[str, str] = {}  # object_id -> scene name
        self._render_state: dict[str, tuple] = {}

    @staticmethod
    def _position_close(
        lhs: tuple[float, float, float],
        rhs: tuple[float, float, float],
        *,
        atol: float = 1e-9,
    ) -> bool:
        return bool(np.allclose(lhs, rhs, atol=atol, rtol=0.0))

    @staticmethod
    def _orientation_close(
        lhs: tuple[float, float, float, float],
        rhs: tuple[float, float, float, float],
        *,
        atol: float = 1e-9,
    ) -> bool:
        lhs_arr = np.asarray(lhs, dtype=float)
        rhs_arr = np.asarray(rhs, dtype=float)
        return bool(
            np.allclose(lhs_arr, rhs_arr, atol=atol, rtol=0.0)
            or np.allclose(lhs_arr, -rhs_arr, atol=atol, rtol=0.0)
        )

    @staticmethod
    def _scene_name_for(object_id: str, frame_path: Optional[str]) -> str:
        return f"{frame_path}/{object_id}" if frame_path else f"/scene/{object_id}"

    @staticmethod
    def _split_env_paths(value: Optional[str]) -> list[Path]:
        if not value:
            return []
        return [Path(item) for item in value.split(":") if item]

    @classmethod
    def _resolve_mesh_resource_uri(cls, resource_uri: str) -> Path:
        if resource_uri.startswith("file://"):
            resource_uri = resource_uri[len("file://"):]
        elif resource_uri.startswith("package://"):
            rest = resource_uri[len("package://"):]
            package, sep, rel = rest.partition("/")
            if not package or not sep or not rel:
                raise ValueError(f"Invalid package mesh URI: {resource_uri}")
            candidates: list[Path] = []
            for prefix in cls._split_env_paths(os.getenv("AMENT_PREFIX_PATH")):
                candidates.append(prefix / "share" / package / rel)
            for prefix in cls._split_env_paths(os.getenv("COLCON_PREFIX_PATH")):
                candidates.append(prefix / "share" / package / rel)
            for root in cls._split_env_paths(os.getenv("ROS_PACKAGE_PATH")):
                candidates.append(root / package / rel)
                if root.name == package:
                    candidates.append(root / rel)
            candidates.extend(
                [
                    Path("/opt/ros/jazzy/share") / package / rel,
                    Path("/usr/local/share") / package / rel,
                    Path("/workspace") / package / rel,
                ]
            )
            for candidate in candidates:
                if candidate.exists():
                    return candidate.resolve()
            raise FileNotFoundError(f"Could not resolve mesh URI: {resource_uri}")

        path = Path(resource_uri)
        candidates = [path]
        if not path.is_absolute():
            candidates.extend([Path.cwd() / path, Path("/workspace") / path])
        for candidate in candidates:
            if candidate.exists():
                return candidate.resolve()
        raise FileNotFoundError(f"Mesh resource does not exist: {resource_uri}")

    @classmethod
    def mesh_extents_m(
        cls,
        resource_uri: str,
        mesh_scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ) -> tuple[float, float, float]:
        """Return a mesh resource's scaled local axis-aligned extents in meters."""
        mesh_path = cls._resolve_mesh_resource_uri(resource_uri)
        mesh = trimesh.load(mesh_path, force="mesh")
        if isinstance(mesh, trimesh.Scene):
            mesh = mesh.dump(concatenate=True)
        if not isinstance(mesh, trimesh.Trimesh):
            raise ValueError(f"Unsupported mesh resource: {resource_uri}")
        extents = np.asarray(mesh.extents, dtype=float)
        scale = np.abs(np.asarray(mesh_scale, dtype=float))
        if extents.shape != (3,) or scale.shape != (3,):
            raise ValueError(f"Invalid mesh extents or scale for resource: {resource_uri}")
        return tuple(float(value) for value in extents * scale)

    @property
    def object_ids(self) -> list[str]:
        """Return currently-rendered object ids."""
        return list(self._objects.keys())

    @staticmethod
    def _rpy_deg_to_xyzw(rpy_deg: list[float]) -> tuple[float, float, float, float]:
        """Convert roll-pitch-yaw (degrees, ZYX convention) to quaternion xyzw."""
        r, p, y = [math.radians(v) for v in rpy_deg]
        cr, sr = math.cos(r / 2), math.sin(r / 2)
        cp, sp = math.cos(p / 2), math.sin(p / 2)
        cy, sy = math.cos(y / 2), math.sin(y / 2)
        return (
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy,
        )

    def _xyzw_to_wxyz(self, xyzw: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
        """Convert quaternion from xyzw to wxyz format.

        ManyForge uses xyzw, Viser uses wxyz.

        Args:
            xyzw: Quaternion in xyzw format (x, y, z, w).

        Returns:
            Quaternion in wxyz format (w, x, y, z).
        """
        x, y, z, w = xyzw
        return (w, x, y, z)

    def add_box(
        self,
        object_id: str,
        dimensions_m: tuple[float, float, float],
        position_m: tuple[float, float, float],
        orientation_xyzw: tuple[float, float, float, float],
        color: tuple[int, int, int] = (200, 100, 50),
    ) -> None:
        """Add a box to the scene.

        Args:
            object_id: Unique identifier for the object.
            dimensions_m: Box dimensions (length, width, height) in meters.
            position_m: Position (x, y, z) in meters.
            orientation_xyzw: Orientation as quaternion in xyzw format.
            color: RGB color tuple (0-255).
        """
        scene_name = f"/scene/{object_id}"
        wxyz = self._xyzw_to_wxyz(orientation_xyzw)

        self._server.scene.add_box(
            name=scene_name,
            dimensions=dimensions_m,
            color=color,
            position=position_m,
            wxyz=wxyz,
        )
        self._objects[object_id] = scene_name

    def add_sphere(
        self,
        object_id: str,
        radius_m: float,
        position_m: tuple[float, float, float],
        color: tuple[int, int, int] = (50, 200, 50),
    ) -> None:
        """Add a sphere to the scene.

        Args:
            object_id: Unique identifier for the object.
            radius_m: Sphere radius in meters.
            position_m: Position (x, y, z) in meters.
            color: RGB color tuple (0-255).
        """
        scene_name = f"/scene/{object_id}"

        self._server.scene.add_icosphere(
            name=scene_name,
            radius=radius_m,
            color=color,
            position=position_m,
        )
        self._objects[object_id] = scene_name

    def add_sphere_on_frame(
        self,
        object_id: str,
        frame_path: str,
        radius_m: float,
        position_m: tuple[float, float, float],
        color: tuple[int, int, int] = (50, 200, 50),
    ) -> None:
        """Add a sphere as a child of a Viser scene frame."""
        scene_name = f"{frame_path}/{object_id}"
        self._server.scene.add_icosphere(
            name=scene_name,
            radius=radius_m,
            color=color,
            position=position_m,
        )
        self._objects[object_id] = scene_name

    def add_mesh(
        self,
        object_id: str,
        resource_uri: str,
        mesh_scale: tuple[float, float, float],
        position_m: tuple[float, float, float],
        orientation_xyzw: tuple[float, float, float, float],
        color: tuple[int, int, int] = (90, 200, 255),
        *,
        frame_path: Optional[str] = None,
    ) -> None:
        """Add a mesh to the scene."""
        scene_name = self._scene_name_for(object_id, frame_path)
        mesh_path = self._resolve_mesh_resource_uri(resource_uri)
        mesh = trimesh.load(mesh_path, force="mesh")
        if isinstance(mesh, trimesh.Scene):
            mesh = mesh.dump(concatenate=True)
        if not isinstance(mesh, trimesh.Trimesh):
            raise ValueError(f"Unsupported mesh resource: {resource_uri}")
        self._server.scene.add_mesh_simple(
            name=scene_name,
            vertices=np.asarray(mesh.vertices, dtype=np.float32),
            faces=np.asarray(mesh.faces, dtype=np.uint32),
            color=color,
            scale=mesh_scale,
            position=position_m,
            wxyz=self._xyzw_to_wxyz(orientation_xyzw),
        )
        self._objects[object_id] = scene_name

    def add_box_on_frame(
        self,
        object_id: str,
        frame_path: str,
        dimensions_m: tuple[float, float, float],
        position_m: tuple[float, float, float],
        orientation_xyzw: tuple[float, float, float, float],
        color: tuple[int, int, int] = (200, 100, 50),
    ) -> None:
        """Add a box as a child of a Viser scene frame (e.g. a robot link).

        Args:
            object_id: Unique identifier for the object.
            frame_path: Parent frame path in the Viser scene graph.
            dimensions_m: Box dimensions (length, width, height) in meters.
            position_m: Position relative to the parent frame.
            orientation_xyzw: Orientation relative to the parent frame (xyzw).
            color: RGB color tuple (0-255).
        """
        scene_name = f"{frame_path}/{object_id}"
        wxyz = self._xyzw_to_wxyz(orientation_xyzw)

        self._server.scene.add_box(
            name=scene_name,
            dimensions=dimensions_m,
            color=color,
            position=position_m,
            wxyz=wxyz,
        )
        self._objects[object_id] = scene_name

    def remove_object(self, object_id: str) -> dict:
        """Remove an object from the scene.

        Args:
            object_id: The object identifier to remove.
        """
        scene_name = self._objects.get(object_id)
        removed = False
        if object_id in self._objects:
            self._server.scene.remove_by_name(scene_name)
            del self._objects[object_id]
            removed = True
        self._render_state.pop(object_id, None)
        return {
            "action": "remove" if removed else "noop",
            "object_id": object_id,
            "scene_name": scene_name,
            "scene_node_present": (
                self._server.scene.get_handle_by_name(scene_name) is not None
                if scene_name else False
            ),
        }

    def get_render_info(self, object_id: str) -> dict:
        """Return renderer-side diagnostics for an object."""
        scene_name = self._objects.get(object_id)
        handle = self._server.scene.get_handle_by_name(scene_name) if scene_name else None
        return {
            "object_id": object_id,
            "scene_name": scene_name,
            "render_state": self._render_state.get(object_id),
            "scene_node_present": handle is not None,
        }

    def sync_box(
        self,
        object_id: str,
        dimensions_m: tuple[float, float, float],
        position_m: tuple[float, float, float],
        orientation_xyzw: tuple[float, float, float, float],
        *,
        frame_path: Optional[str] = None,
        color: tuple[int, int, int] = (200, 100, 50),
    ) -> dict:
        """Render or update a box only when its authoritative state changes."""
        scene_name = self._scene_name_for(object_id, frame_path)
        render_state = ("box", frame_path, dimensions_m, position_m, orientation_xyzw, color)
        previous_state = self._render_state.get(object_id)
        previous_scene_name = self._objects.get(object_id)
        handle = (
            self._server.scene.get_handle_by_name(previous_scene_name)
            if previous_scene_name else None
        )

        if (
            previous_state is not None
            and previous_state[0] == "box"
            and previous_scene_name == scene_name
            and previous_state[2] == dimensions_m
            and previous_state[5] == color
            and handle is not None
        ):
            position_changed = not self._position_close(previous_state[3], position_m)
            orientation_changed = not self._orientation_close(previous_state[4], orientation_xyzw)
            if position_changed:
                handle.position = position_m
            if orientation_changed:
                handle.wxyz = self._xyzw_to_wxyz(orientation_xyzw)
            self._render_state[object_id] = render_state
            return {
                "action": "updated_pose" if position_changed or orientation_changed else "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": True,
            }

        if previous_state == render_state:
            return {
                "action": "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": handle is not None,
            }

        removal = self.remove_object(object_id)
        if frame_path:
            self.add_box_on_frame(
                object_id, frame_path, dimensions_m, position_m, orientation_xyzw, color
            )
        else:
            self.add_box(object_id, dimensions_m, position_m, orientation_xyzw, color)
        self._render_state[object_id] = render_state
        return {
            "action": "rendered",
            "object_id": object_id,
            "scene_name": scene_name,
            "previous_scene_name": removal.get("scene_name"),
            "scene_node_present": (
                self._server.scene.get_handle_by_name(scene_name) is not None
                if scene_name else False
            ),
        }

    def sync_sphere(
        self,
        object_id: str,
        radius_m: float,
        position_m: tuple[float, float, float],
        *,
        frame_path: Optional[str] = None,
        color: tuple[int, int, int] = (50, 200, 50),
    ) -> dict:
        """Render or update a sphere only when its authoritative state changes."""
        scene_name = self._scene_name_for(object_id, frame_path)
        render_state = ("sphere", frame_path, radius_m, position_m, color)
        previous_state = self._render_state.get(object_id)
        previous_scene_name = self._objects.get(object_id)
        handle = (
            self._server.scene.get_handle_by_name(previous_scene_name)
            if previous_scene_name else None
        )

        if (
            previous_state is not None
            and previous_state[0] == "sphere"
            and previous_scene_name == scene_name
            and previous_state[2] == radius_m
            and previous_state[4] == color
            and handle is not None
        ):
            position_changed = not self._position_close(previous_state[3], position_m)
            if position_changed:
                handle.position = position_m
            self._render_state[object_id] = render_state
            return {
                "action": "updated_pose" if position_changed else "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": True,
            }

        if previous_state == render_state:
            return {
                "action": "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": handle is not None,
            }

        removal = self.remove_object(object_id)
        if frame_path:
            self.add_sphere_on_frame(object_id, frame_path, radius_m, position_m, color)
        else:
            self.add_sphere(object_id, radius_m, position_m, color)
        self._render_state[object_id] = render_state
        return {
            "action": "rendered",
            "object_id": object_id,
            "scene_name": scene_name,
            "previous_scene_name": removal.get("scene_name"),
            "scene_node_present": (
                self._server.scene.get_handle_by_name(scene_name) is not None
                if scene_name else False
            ),
        }

    def sync_mesh(
        self,
        object_id: str,
        resource_uri: str,
        mesh_scale: tuple[float, float, float],
        position_m: tuple[float, float, float],
        orientation_xyzw: tuple[float, float, float, float],
        *,
        frame_path: Optional[str] = None,
        color: tuple[int, int, int] = (90, 200, 255),
    ) -> dict:
        """Render or update a mesh only when its authoritative state changes."""
        scene_name = self._scene_name_for(object_id, frame_path)
        render_state = (
            "mesh",
            frame_path,
            resource_uri,
            mesh_scale,
            position_m,
            orientation_xyzw,
            color,
        )
        previous_state = self._render_state.get(object_id)
        previous_scene_name = self._objects.get(object_id)
        handle = (
            self._server.scene.get_handle_by_name(previous_scene_name)
            if previous_scene_name else None
        )

        if (
            previous_state is not None
            and previous_state[0] == "mesh"
            and previous_scene_name == scene_name
            and previous_state[2] == resource_uri
            and previous_state[3] == mesh_scale
            and previous_state[6] == color
            and handle is not None
        ):
            position_changed = not self._position_close(previous_state[4], position_m)
            orientation_changed = not self._orientation_close(previous_state[5], orientation_xyzw)
            if position_changed:
                handle.position = position_m
            if orientation_changed:
                handle.wxyz = self._xyzw_to_wxyz(orientation_xyzw)
            self._render_state[object_id] = render_state
            return {
                "action": "updated_pose" if position_changed or orientation_changed else "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": True,
            }

        if previous_state == render_state:
            return {
                "action": "unchanged",
                "object_id": object_id,
                "scene_name": scene_name,
                "scene_node_present": handle is not None,
            }

        removal = self.remove_object(object_id)
        self.add_mesh(object_id, resource_uri, mesh_scale, position_m, orientation_xyzw, color, frame_path=frame_path)
        self._render_state[object_id] = render_state
        return {
            "action": "rendered",
            "object_id": object_id,
            "scene_name": scene_name,
            "previous_scene_name": removal.get("scene_name"),
            "scene_node_present": (
                self._server.scene.get_handle_by_name(scene_name) is not None
                if scene_name else False
            ),
        }

    def load_from_program(self, scene_resources: list) -> None:
        """Load all scene objects from a Program's scene list.

        Args:
            scene_resources: List of scene resource dictionaries or SceneResource objects from a Program.
        """
        for resource in scene_resources:
            # Handle both dict and SceneResource dataclass
            if hasattr(resource, "object_id"):
                # It's a SceneResource dataclass
                object_id = resource.object_id
                shape = resource.shape
                pose = resource.pose
            else:
                # It's a dictionary
                object_id = resource.get("object_id", "unknown")
                shape = resource.get("shape", {})
                pose = resource.get("pose", {})

            # Get shape type
            if hasattr(shape, "get"):
                shape_type = shape.get("type", "")
            else:
                shape_type = shape.get("type", "") if isinstance(shape, dict) else ""

            # Get position and orientation
            if hasattr(pose, "get"):
                position = tuple(pose.get("position_m", [0.0, 0.0, 0.0]))
                orient_xyzw = pose.get("orientation_xyzw")
                if not orient_xyzw and "orientation_rpy_deg" in pose:
                    orient_xyzw = list(self._rpy_deg_to_xyzw(pose["orientation_rpy_deg"]))
                orientation = tuple(orient_xyzw or [0.0, 0.0, 0.0, 1.0])
            else:
                position = tuple(pose.get("position_m", [0.0, 0.0, 0.0])) if isinstance(pose, dict) else (0.0, 0.0, 0.0)
                orientation = tuple(pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0])) if isinstance(pose, dict) else (0.0, 0.0, 0.0, 1.0)

            if shape_type == "box":
                if hasattr(shape, "get"):
                    dimensions = tuple(shape.get("box_dimensions_m", [0.1, 0.1, 0.1]))
                else:
                    dimensions = tuple(shape.get("box_dimensions_m", [0.1, 0.1, 0.1])) if isinstance(shape, dict) else (0.1, 0.1, 0.1)
                self.add_box(object_id, dimensions, position, orientation)
            elif shape_type == "sphere":
                if hasattr(shape, "get"):
                    radius = shape.get("sphere_radius_m", 0.05)
                else:
                    radius = shape.get("sphere_radius_m", 0.05) if isinstance(shape, dict) else 0.05
                self.add_sphere(object_id, radius, position)
            elif shape_type == "mesh":
                if hasattr(shape, "get"):
                    resource_uri = shape.get("mesh_resource_uri", "")
                    mesh_scale = tuple(shape.get("mesh_scale", [1.0, 1.0, 1.0]))
                else:
                    resource_uri = shape.get("mesh_resource_uri", "") if isinstance(shape, dict) else ""
                    mesh_scale = tuple(shape.get("mesh_scale", [1.0, 1.0, 1.0])) if isinstance(shape, dict) else (1.0, 1.0, 1.0)
                self.add_mesh(object_id, resource_uri, mesh_scale, position, orientation)

    def clear(self) -> None:
        """Remove all scene objects."""
        for object_id in list(self._objects.keys()):
            self.remove_object(object_id)
        self._objects.clear()

    @property
    def object_count(self) -> int:
        """Get the number of objects in the scene."""
        return len(self._objects)
