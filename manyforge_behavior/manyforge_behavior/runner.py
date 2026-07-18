"""Orchestrator runner — builds and ticks a py_trees behavior tree from a ManyForge program."""

from __future__ import annotations

import time
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.blackboard_bindings import resolve_runtime_blackboard_bindings
from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest
from manyforge_behavior.motion_runtime import MotionExecutionCoordinator
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.node_registry import NodeFactoryContext
from manyforge_behavior.runtime_pause import RuntimePauseRegistry
from manyforge_behavior.runtime_resources import RuntimeResourceRegistry
from manyforge_behavior.nodes.motion import _rpy_deg_to_xyzw
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopPolicy
from manyforge_behavior.parameters import ParameterResolver
from manyforge_behavior.program import Program, TreeNode, tree_node_from_dict
from manyforge_behavior.skill_loader import (
    SkillCatalog,
    default_skills_dir,
    load_skill_catalog,
)


class OrchestratorRunner:
    """Main entry point: load, build, and run a ManyForge program."""

    def __init__(
        self,
        node_environment: NodeEnvironment,
        program_path: str,
        kernel: KernelClient,
        overrides: Optional[dict[str, Any]] = None,
        deployment_path: Optional[str] = None,
        program: Optional[Program] = None,
        deployment: Optional["Deployment"] = None,
        skill_catalog: Optional[SkillCatalog] = None,
        event_recorder: Optional[Any] = None,
        scene_snapshot_provider: Optional[Callable[[int, int, str], Optional[bytes]]] = None,
        scene_snapshot_url_publisher: Optional[
            Callable[..., Optional[dict[str, Any]]]
        ] = None,
        pose_source_apply_callback: Optional[
            Callable[..., dict[str, Any]]
        ] = None,
        pose_source_between_nodes_callback: Optional[
            Callable[..., dict[str, Any]]
        ] = None,
        simulation_control_callback: Optional[
            Callable[..., dict[str, Any]]
        ] = None,
        simulation_reset_callback: Optional[
            Callable[..., dict[str, Any]]
        ] = None,
        runtime_resources: RuntimeResourceRegistry | None = None,
    ):
        self._node_environment = node_environment
        self._program_path = program_path
        self._kernel = kernel
        self._overrides = overrides or {}
        self._deployment_path = deployment_path
        self._provided_program = program
        self._provided_deployment = deployment
        self._provided_skill_catalog = skill_catalog
        self._catalog = node_environment.catalog
        self._node_registry = node_environment.registry
        self._program: Optional[Program] = None
        self._resolver: Optional[ParameterResolver] = None
        self._tree: Optional[py_trees.trees.BehaviourTree] = None
        self._deployment: Optional["Deployment"] = None
        self._skill_catalog: Optional[SkillCatalog] = skill_catalog
        self._event_recorder = event_recorder
        self._scene_snapshot_provider = scene_snapshot_provider
        self._scene_snapshot_url_publisher = scene_snapshot_url_publisher
        self._pose_source_apply_callback = pose_source_apply_callback
        self._pose_source_between_nodes_callback = pose_source_between_nodes_callback
        self._simulation_control_callback = simulation_control_callback
        self._simulation_reset_callback = simulation_reset_callback
        self._runtime_resources = runtime_resources or RuntimeResourceRegistry.empty()
        self._motion_coordinator = MotionExecutionCoordinator(kernel)
        self._runtime_pause_registry = RuntimePauseRegistry()
        # Intervention context — set by cycle_manager before building
        # intervention subtrees so nodes like QueryModelNode can attribute
        # audit events to the active session.
        self._intervention_context: Optional[dict[str, str]] = None

    @property
    def runtime_pause_registry(self) -> RuntimePauseRegistry:
        return self._runtime_pause_registry

    def load(self) -> None:
        """Load catalog, program, validate, set up parameter resolver."""
        if self._provided_skill_catalog is None:
            self._skill_catalog = load_skill_catalog(
                default_skills_dir(),
                catalog=self._catalog,
                strict=True,
            )
        if self._provided_program is not None:
            self._program = self._provided_program
        else:
            self._program = Program(
                self._program_path,
                self._catalog,
                skill_catalog=self._skill_catalog,
            )
        self._resolver = ParameterResolver(
            self._program.get_parameter_defaults(),
            self._overrides,
        )

        # Load and validate deployment if provided
        if self._provided_deployment is not None:
            self._deployment = self._provided_deployment
            self._deployment.validate_against_program(self._program)
            self._deployment.validate_capabilities(self._program)
            self._deployment.validate_assistant_modes(self._catalog, self._skill_catalog)
        elif self._deployment_path:
            from manyforge_behavior.deployment import Deployment
            self._deployment = Deployment(self._deployment_path)
            self._deployment.validate_against_program(self._program)
            self._deployment.validate_capabilities(self._program)
            self._deployment.validate_assistant_modes(self._catalog, self._skill_catalog)

    def build_tree(self) -> py_trees.trees.BehaviourTree:
        """Build a py_trees BehaviourTree from the loaded program."""
        if self._program is None or self._resolver is None:
            raise RuntimeError("Call load() first")

        root = self._build_node(self._program.tree)
        tree = py_trees.trees.BehaviourTree(root=root)

        # Publish setup-time services before calling BehaviourTree.setup().
        # External nodes are explicitly allowed to acquire deployment-owned
        # handles in setup(), so creating these keys afterward would make their
        # lifecycle depend on an incidental first-tick fallback.
        bb = py_trees.blackboard.Client(name="runner")
        bb.register_key(key="kernel", access=py_trees.common.Access.WRITE)
        bb.kernel = self._kernel
        bb.register_key(key="motion_coordinator", access=py_trees.common.Access.WRITE)
        bb.motion_coordinator = self._motion_coordinator
        bb.register_key(key="runtime_resources", access=py_trees.common.Access.WRITE)
        bb.runtime_resources = self._runtime_resources
        tree.setup()

        # Inject robot profile defaults for motion nodes
        bb.register_key(key="robot_id", access=py_trees.common.Access.WRITE)
        bb.register_key(key="group_id", access=py_trees.common.Access.WRITE)
        bb.register_key(key="base_frame", access=py_trees.common.Access.WRITE)
        bb.register_key(key="tcp_frame", access=py_trees.common.Access.WRITE)
        bb.robot_id = self._program.robot.robot_id
        bb.group_id = self._program.robot.group_id
        bb.base_frame = self._program.robot.base_frame
        bb.tcp_frame = self._program.robot.tcp_frame

        # Build static scene metadata registry for attach/detach nodes.
        # Runtime object pose/frame always comes from backend queries.
        bb.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        scene_registry = {}
        for resource in (self._program.scene or []):
            scene_registry[resource.object_id] = {
                "shape_type": resource.shape.get("type"),
                "box_dimensions_m": resource.shape.get("box_dimensions_m"),
                "sphere_radius_m": resource.shape.get("sphere_radius_m"),
                "cylinder_radius_m": resource.shape.get("cylinder_radius_m"),
                "cylinder_height_m": resource.shape.get("cylinder_height_m"),
                "mesh_resource_uri": resource.shape.get("mesh_resource_uri"),
                "mesh_scale": resource.shape.get("mesh_scale"),
                "support_contacts": resource.metadata.get("support_contacts"),
            }
        bb.scene_objects = scene_registry

        bb.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        if self._deployment is not None:
            bb.gripper_configs = {
                gripper_id: {
                    "command_mode": config.command_mode,
                    "group_id": config.group_id,
                    "joint_name": config.joint_name,
                    "motion_time_s": config.motion_time_s,
                    "srdf_presets": dict(config.srdf_presets),
                    "actuation_kind": config.actuation_kind,
                    "state_authority": config.state_authority,
                    "completion_policy": config.completion_policy,
                    "grasp_confirmation_policy": config.grasp_confirmation_policy,
                    "transport": dict(config.transport),
                    "open_state_ref": config.open_state_ref,
                    "closed_state_ref": config.closed_state_ref,
                    "semantic_configured": config.semantic_configured,
                    "migration_notes": list(config.migration_notes),
                    "description": config.description,
                    "touch_links": list(config.touch_links),
                }
                for gripper_id, config in self._deployment.grippers.items()
            }
        else:
            bb.gripper_configs = {}

        # Write global TCP speed cap from deployment if available
        if self._deployment:
            bb.register_key(key="global_max_tcp_linear_speed_mps", access=py_trees.common.Access.WRITE)
            bb.global_max_tcp_linear_speed_mps = self._deployment.robot_config.global_max_tcp_linear_speed_mps

        # Register stop flag and velocity scale keys for CycleManager
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.stop_requested = False
        bb.register_key(key="motion_stop_active", access=py_trees.common.Access.WRITE)
        bb.motion_stop_active = False
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.velocity_scale = 1.0
        bb.register_key(
            key="pose_source_apply_callback",
            access=py_trees.common.Access.WRITE,
        )
        bb.pose_source_apply_callback = self._pose_source_apply_callback
        bb.register_key(
            key="pose_source_between_nodes_callback",
            access=py_trees.common.Access.WRITE,
        )
        bb.pose_source_between_nodes_callback = self._pose_source_between_nodes_callback
        bb.register_key(
            key="simulation_control_callback",
            access=py_trees.common.Access.WRITE,
        )
        bb.simulation_control_callback = self._simulation_control_callback
        if (
            bb.simulation_control_callback is None
            and self._simulation_reset_callback is not None
        ):
            bb.simulation_control_callback = self._legacy_simulation_reset_adapter

        self._tree = tree
        return tree

    def _legacy_simulation_reset_adapter(self, **kwargs: Any) -> dict[str, Any]:
        if kwargs.get("operation") != "reset":
            raise RuntimeError("legacy simulation reset callback only supports reset")
        payload = dict(kwargs)
        payload.pop("operation", None)
        if self._simulation_reset_callback is None:
            raise RuntimeError("simulation reset callback is not configured")
        return self._simulation_reset_callback(**payload)

    def apply_scene(self) -> None:
        """Apply program scene resources via kernel.scene_edit() before execution."""
        if self._program is None:
            raise RuntimeError("Call load() first")

        self._kernel.set_robot_base_pose(self._program.robot.base_pose)

        for resource in self._program.scene:
            shape_type = resource.shape.get("type")
            request = SceneEditRequest(
                operation="add",
                object_id=resource.object_id,
                shape_type=shape_type,
                box_dimensions_m=resource.shape.get("box_dimensions_m"),
                sphere_radius_m=resource.shape.get("sphere_radius_m"),
                cylinder_radius_m=resource.shape.get("cylinder_radius_m"),
                cylinder_height_m=resource.shape.get("cylinder_height_m"),
                mesh_resource_uri=resource.shape.get("mesh_resource_uri"),
                mesh_scale=resource.shape.get("mesh_scale"),
                pose_position_m=resource.pose.get("position_m"),
                pose_orientation_xyzw=resource.pose.get("orientation_xyzw")
                    or (_rpy_deg_to_xyzw(resource.pose["orientation_rpy_deg"])
                        if "orientation_rpy_deg" in resource.pose else None),
                frame_id=resource.pose.get("frame_id"),
                support_contacts=resource.metadata.get("support_contacts"),
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
            if status == py_trees.common.Status.RUNNING:
                # Yield to background workers used by non-blocking nodes such as
                # QueryModelNode. A zero-period run is still allowed, but should not
                # busy-spin so tightly that futures never get CPU time.
                time.sleep(tick_period_s if tick_period_s > 0.0 else 0.001)
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

    @property
    def skill_catalog(self) -> Optional[SkillCatalog]:
        return self._skill_catalog

    def set_scene_snapshot_provider(
        self,
        provider: Optional[Callable[[int, int, str], Optional[bytes]]],
    ) -> None:
        """Update the viewer-backed render provider used by query_model nodes."""
        self._scene_snapshot_provider = provider

    def set_scene_snapshot_url_publisher(
        self,
        publisher: Optional[Callable[..., Optional[dict[str, Any]]]],
    ) -> None:
        """Update the URL publisher used for HTTP-served snapshot experiments."""
        self._scene_snapshot_url_publisher = publisher

    def _expand_tree_node(self, tree_node: TreeNode) -> TreeNode:
        """Expand a skill invocation into a concrete subtree when needed."""
        if self._skill_catalog is None or not self._skill_catalog.contains(tree_node.id):
            return tree_node

        resolved_params = self._resolver.resolve(tree_node.params)
        expanded_dict = self._skill_catalog.expand_node_dict(
            {
                "id": tree_node.id,
                "name": tree_node.name,
                "params": resolved_params,
                "children": [child.to_dict() for child in tree_node.children],
            }
        )
        expanded = tree_node_from_dict(expanded_dict)
        expanded.operator_stop_policy = tree_node.operator_stop_policy
        return expanded

    def _make_skill_expander(self):
        """Return a callable that expands a skill ID into a py_trees subtree."""
        def _expand(skill_id: str, params: dict):
            if self._skill_catalog is None:
                return None
            if not self._skill_catalog.contains(skill_id):
                return None
            expanded_dict = self._skill_catalog.expand_node_dict(
                {"id": skill_id, "name": f"_recovery_{skill_id}", "params": params, "children": []}
            )
            recovery_tree_node = tree_node_from_dict(expanded_dict)
            return self._build_node(recovery_tree_node)
        return _expand

    def _build_node(
        self,
        tree_node: TreeNode,
        *,
        inherited_operator_stop_policy: str = "continue",
        inherited_operator_stop_policy_explicit: bool = False,
    ) -> py_trees.behaviour.Behaviour:
        """Recursively build a py_trees node from a TreeNode."""
        tree_node = self._expand_tree_node(tree_node)
        entry = self._catalog.get(tree_node.id)
        if entry.kind == "authoring_macro":
            raise ValueError(
                f"Authoring macro '{tree_node.id}' reached the runtime tree; "
                "Composer must expand it before apply/save"
            )
        explicit_operator_stop_policy = tree_node.operator_stop_policy != "inherit"
        if explicit_operator_stop_policy:
            effective_operator_stop_policy = tree_node.operator_stop_policy
        elif inherited_operator_stop_policy_explicit:
            effective_operator_stop_policy = inherited_operator_stop_policy
        else:
            effective_operator_stop_policy = (
                inherited_operator_stop_policy
                if entry.operator_stop_policy == "inherit"
                else entry.operator_stop_policy
            )
        n_children = len(tree_node.children)
        if n_children < entry.children_min:
            raise ValueError(
                f"Node '{tree_node.name}' ({tree_node.id}) requires at least "
                f"{entry.children_min} child{'ren' if entry.children_min != 1 else ''}, got {n_children}"
            )
        if entry.children_max >= 0 and n_children > entry.children_max:
            raise ValueError(
                f"Node '{tree_node.name}' ({tree_node.id}) allows at most "
                f"{entry.children_max} child{'ren' if entry.children_max != 1 else ''}, got {n_children}"
            )
        resolved_params = self._resolver.resolve(tree_node.params)
        resolved_params = resolve_runtime_blackboard_bindings(
            entry,
            resolved_params,
            blackboard_namespaces=self._program.blackboard_namespaces,
            blackboard_keys=self._program.blackboard_keys,
        )
        children = [
            self._build_node(
                c,
                inherited_operator_stop_policy=effective_operator_stop_policy,
                inherited_operator_stop_policy_explicit=(
                    explicit_operator_stop_policy
                    or inherited_operator_stop_policy_explicit
                ),
            )
            for c in tree_node.children
        ]

        def finalize(behavior: py_trees.behaviour.Behaviour) -> py_trees.behaviour.Behaviour:
            if effective_operator_stop_policy == "continue":
                return behavior
            return OperatorMotionStopPolicy(
                name=tree_node.name,
                child=behavior,
                policy=effective_operator_stop_policy,
            )

        for injection in entry.runtime_injections:
            if injection == "runtime_pause_registry":
                resolved_params["runtime_pause_registry"] = (
                    self._runtime_pause_registry
                )
            elif injection == "robot_defaults":
                resolved_params.setdefault("robot_id", self._program.robot.robot_id)
                resolved_params.setdefault("group_id", self._program.robot.group_id)
                resolved_params.setdefault("base_frame", self._program.robot.base_frame)
                resolved_params.setdefault("tcp_frame", self._program.robot.tcp_frame)
            elif injection == "base_frame_default":
                resolved_params.setdefault("base_frame", self._program.robot.base_frame)
            elif injection == "scene_snapshot_provider":
                resolved_params["scene_snapshot_provider"] = self._scene_snapshot_provider

        services = {
            "deployment": self._deployment,
            "event_recorder": self._event_recorder,
            "intervention_context": self._intervention_context,
            "runtime_pause_registry": self._runtime_pause_registry,
            "scene_snapshot_provider": self._scene_snapshot_provider,
            "scene_snapshot_url_publisher": self._scene_snapshot_url_publisher,
            "skill_expander": self._make_skill_expander(),
        }
        behavior = self._node_registry.instantiate(
            tree_node.id,
            NodeFactoryContext(
                name=tree_node.name,
                params=resolved_params,
                children=tuple(children),
                services=services,
            ),
        )
        return finalize(behavior)
