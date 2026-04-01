"""Orchestrator runner — builds and ticks a py_trees behavior tree from a ManyForge program."""

from __future__ import annotations

from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest
from manyforge_behavior.nodes.geometry import CheckPoseDistance, GetLinkPose
from manyforge_behavior.nodes.logic import (
    CheckKeyBoolValue,
    SetKeyBoolValue,
    WaitForKeyBool,
)
from manyforge_behavior.nodes.motion import MoveManipulatorAction, ResetTrajectories
from manyforge_behavior.nodes.scene import (
    AddCollisionObject,
    RemoveCollisionObject,
    UpdateCollisionObjectPose,
)
from manyforge_behavior.parameters import ParameterResolver
from manyforge_behavior.program import Program, TreeNode


# Map catalog IDs to node constructors.
# Composites/decorators are handled separately since they need children.
PLUGIN_FACTORY: dict[str, type] = {
    "move_manipulator_action": MoveManipulatorAction,
    "reset_trajectories": ResetTrajectories,
    "add_collision_object": AddCollisionObject,
    "remove_collision_object": RemoveCollisionObject,
    "update_collision_object_pose": UpdateCollisionObjectPose,
    "check_key_bool_value": CheckKeyBoolValue,
    "set_key_bool_value": SetKeyBoolValue,
    "wait_for_key_bool": WaitForKeyBool,
    "get_link_pose": GetLinkPose,
    "check_pose_distance": CheckPoseDistance,
}


class OrchestratorRunner:
    """Main entry point: load, build, and run a ManyForge program."""

    def __init__(
        self,
        catalog_path: str,
        program_path: str,
        kernel: KernelClient,
        overrides: Optional[dict[str, Any]] = None,
        deployment_path: Optional[str] = None,
    ):
        self._catalog_path = catalog_path
        self._program_path = program_path
        self._kernel = kernel
        self._overrides = overrides or {}
        self._deployment_path = deployment_path
        self._catalog: Optional[NodeCatalog] = None
        self._program: Optional[Program] = None
        self._resolver: Optional[ParameterResolver] = None
        self._tree: Optional[py_trees.trees.BehaviourTree] = None
        self._deployment: Optional["Deployment"] = None

    def load(self) -> None:
        """Load catalog, program, validate, set up parameter resolver."""
        self._catalog = NodeCatalog(self._catalog_path)
        self._program = Program(self._program_path, self._catalog)
        self._resolver = ParameterResolver(
            self._program.get_parameter_defaults(),
            self._overrides,
        )

        # Load and validate deployment if provided
        if self._deployment_path:
            from manyforge_behavior.deployment import Deployment
            self._deployment = Deployment(self._deployment_path)
            self._deployment.validate_against_program(self._program)

    def build_tree(self) -> py_trees.trees.BehaviourTree:
        """Build a py_trees BehaviourTree from the loaded program."""
        if self._program is None or self._resolver is None:
            raise RuntimeError("Call load() first")

        root = self._build_node(self._program.tree)
        tree = py_trees.trees.BehaviourTree(root=root)
        tree.setup()

        # Store kernel on the blackboard
        bb = py_trees.blackboard.Client(name="runner")
        bb.register_key(key="kernel", access=py_trees.common.Access.WRITE)
        bb.kernel = self._kernel

        # Inject robot profile defaults for motion nodes
        bb.register_key(key="robot_id", access=py_trees.common.Access.WRITE)
        bb.register_key(key="group_id", access=py_trees.common.Access.WRITE)
        bb.register_key(key="base_frame", access=py_trees.common.Access.WRITE)
        bb.register_key(key="tcp_frame", access=py_trees.common.Access.WRITE)
        bb.robot_id = self._program.robot.robot_id
        bb.group_id = self._program.robot.group_id
        bb.base_frame = self._program.robot.base_frame
        bb.tcp_frame = self._program.robot.tcp_frame

        # Write global TCP speed cap from deployment if available
        if self._deployment:
            bb.register_key(key="global_max_tcp_linear_speed_mps", access=py_trees.common.Access.WRITE)
            bb.global_max_tcp_linear_speed_mps = self._deployment.robot_config.global_max_tcp_linear_speed_mps

        self._tree = tree
        return tree

    def apply_scene(self) -> None:
        """Apply program scene resources via kernel.scene_edit() before execution."""
        if self._program is None:
            raise RuntimeError("Call load() first")

        for resource in self._program.scene:
            shape_type = resource.shape.get("type")
            request = SceneEditRequest(
                operation="add",
                object_id=resource.object_id,
                shape_type=shape_type,
                box_dimensions_m=resource.shape.get("box_dimensions_m"),
                sphere_radius_m=resource.shape.get("sphere_radius_m"),
                pose_position_m=resource.pose.get("position_m"),
                pose_orientation_xyzw=resource.pose.get("orientation_xyzw"),
                frame_id=resource.pose.get("frame_id"),
            )
            result = self._kernel.scene_edit(request)
            if not result.success:
                raise RuntimeError(
                    f"Failed to add scene object '{resource.object_id}': "
                    f"{result.error}"
                )

    def run(self, max_ticks: int = 1000, tick_period_s: float = 0.0) -> bool:
        """Tick the tree until completion or max_ticks. Returns True if tree succeeded."""
        if self._tree is None:
            raise RuntimeError("Call build_tree() first")

        for _ in range(max_ticks):
            self._tree.tick()
            status = self._tree.root.status
            if status == py_trees.common.Status.SUCCESS:
                return True
            if status == py_trees.common.Status.FAILURE:
                return False
        return False

    @property
    def catalog(self) -> Optional[NodeCatalog]:
        return self._catalog

    @property
    def program(self) -> Optional[Program]:
        return self._program

    @property
    def deployment(self) -> Optional["Deployment"]:
        return self._deployment

    def _build_node(self, tree_node: TreeNode) -> py_trees.behaviour.Behaviour:
        """Recursively build a py_trees node from a TreeNode."""
        resolved_params = self._resolver.resolve(tree_node.params)
        children = [self._build_node(c) for c in tree_node.children]

        # Inject robot profile defaults for motion nodes
        if tree_node.id == "move_manipulator_action":
            if "robot_id" not in resolved_params:
                resolved_params["robot_id"] = self._program.robot.robot_id
            if "group_id" not in resolved_params:
                resolved_params["group_id"] = self._program.robot.group_id
            if "base_frame" not in resolved_params:
                resolved_params["base_frame"] = self._program.robot.base_frame
            if "tcp_frame" not in resolved_params:
                resolved_params["tcp_frame"] = self._program.robot.tcp_frame

        # Control flow composites
        if tree_node.id == "sequence":
            return py_trees.composites.Sequence(
                name=tree_node.name, memory=True, children=children
            )
        if tree_node.id == "fallback":
            return py_trees.composites.Selector(
                name=tree_node.name, memory=True, children=children
            )
        if tree_node.id == "parallel":
            return py_trees.composites.Parallel(
                name=tree_node.name,
                policy=py_trees.common.ParallelPolicy.SuccessOnAll(),
                children=children,
            )
        if tree_node.id == "inverter":
            if not children:
                raise ValueError(
                    f"Inverter '{tree_node.name}' requires exactly one child"
                )
            return py_trees.decorators.Inverter(
                name=tree_node.name, child=children[0]
            )
        if tree_node.id == "timer":
            duration = float(resolved_params.get("duration_s", 1.0))
            return py_trees.timers.Timer(
                name=tree_node.name, duration=duration
            )

        # Plugin nodes
        if tree_node.id in PLUGIN_FACTORY:
            cls = PLUGIN_FACTORY[tree_node.id]
            return cls(name=tree_node.name, **resolved_params)

        raise ValueError(f"Unknown node type: '{tree_node.id}'")
