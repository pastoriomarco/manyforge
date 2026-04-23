"""MCP (Model Context Protocol) server for ManyForge Composer.

Exposes the ManyForge REST API surface as MCP tools for LLM agent consumption.
Tools operate on the same in-process ComposerState — no HTTP round-trips.

Usage:
    python -m manyforge_composer --mcp --catalog-path <path>

The server communicates over stdio using the MCP protocol.
"""
from __future__ import annotations

import base64
import json
import logging
import re
import time
from pathlib import Path
from typing import Any, Optional

from .intervention_validation import merged_intervention_blackboard_keys
from .state import ComposerState
from .security import is_valid_principal_id

logger = logging.getLogger(__name__)


def _valid_trigger_reasons() -> set[str]:
    from manyforge_behavior.cycle_manager import RecoveryTrigger

    reasons = {member.value for member in RecoveryTrigger}
    reasons.add("manual_request")
    return reasons


def create_mcp_server(state: ComposerState):
    """Create an MCP server wrapping the given ComposerState.

    Each tool maps to one or more REST endpoints, delegating to the same
    builder functions used by the FastAPI routes.
    """
    try:
        from mcp.server.fastmcp import FastMCP
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "The ManyForge MCP server requires the 'mcp' package in the active image."
        ) from exc

    mcp = FastMCP(
        "ManyForge Composer",
        instructions=(
            "ManyForge is an industrial robotics control system. "
            "Use these tools to observe the robot scene, compose behavior trees, "
            "execute programs, and monitor execution. "
            "Always validate trees before applying them. "
            "Always call reset_trajectories after scene changes."
        ),
    )

    # ── Scene State ──────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_scene_state() -> str:
        """Get the full scene state including robot joint positions, TCP pose, and all collision objects.

        Returns a JSON object with:
        - robot: joint positions, TCP pose in base frame, robot/group IDs
        - objects: list of scene objects with poses, shapes, and attachment status
        - timestamp_ns: monotonic timestamp
        """
        from .routes_scene import build_scene_state_payload
        return json.dumps(_snakecase_keys(build_scene_state_payload(state)))

    # ── Scene Edit ───────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_scene_edit(
        operation: str,
        object_id: str,
        shape_type: Optional[str] = None,
        box_dimensions_m: Optional[list[float]] = None,
        sphere_radius_m: Optional[float] = None,
        pose_position_m: Optional[list[float]] = None,
        pose_orientation_xyzw: Optional[list[float]] = None,
        frame_id: Optional[str] = None,
    ) -> str:
        """Edit the collision scene at runtime.

        Operations:
        - "add": Add a collision object (requires shape_type, pose)
        - "remove": Remove a collision object
        - "update_pose": Move an existing object
        - "attach": Attach object to a robot link (frame_id required)
        - "detach": Detach object from robot link (frame_id required)

        Args:
            operation: One of "add", "remove", "update_pose", "attach", "detach"
            object_id: Unique identifier for the scene object
            shape_type: Shape type for "add" — "box" or "sphere"
            box_dimensions_m: [x, y, z] dimensions for box shapes
            sphere_radius_m: Radius for sphere shapes
            pose_position_m: [x, y, z] position in meters
            pose_orientation_xyzw: [x, y, z, w] quaternion orientation
            frame_id: Robot link frame for attach/detach operations
        """
        from manyforge_behavior.cycle_manager import CycleState
        from manyforge_behavior.kernel_client import SceneEditRequest

        if state.cycle_manager is not None and state.cycle_manager.state in {
            CycleState.RUNNING,
            CycleState.INTERRUPT_HANDLER,
        }:
            cycle_state = state.cycle_manager.state
            return json.dumps(
                {
                    "success": False,
                    "error": (
                        "Scene edits not allowed while execution is active "
                        f"(state={cycle_state.name})"
                    ),
                    "cycle_state": cycle_state.value,
                }
            )

        kernel = state.kernel
        if kernel is None:
            return json.dumps({"success": False, "error": "Kernel not available"})

        with state.scene_edit_lock:
            request = SceneEditRequest(
                operation=operation,
                object_id=object_id,
                shape_type=shape_type,
                box_dimensions_m=box_dimensions_m,
                sphere_radius_m=sphere_radius_m,
                pose_position_m=pose_position_m,
                pose_orientation_xyzw=pose_orientation_xyzw,
                frame_id=frame_id,
            )
            result = kernel.scene_edit(request)

        return json.dumps({
            "success": result.success,
            "error": result.error,
            "timestamp_ns": time.monotonic_ns(),
        })

    # ── Program Load ─────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_program_load(
        program_path: str,
        deployment_path: Optional[str] = None,
        force_discard_overrides: bool = False,
    ) -> str:
        """Load a program (and optional deployment) from YAML files on disk.

        This creates a new CycleManager and resets execution state.
        Paths must be within the configured backend allowlist roots.

        Args:
            program_path: Absolute path to the .program.yaml file
            deployment_path: Optional absolute path to the .deployment.yaml file
            force_discard_overrides: If true, discard any existing runtime overrides
        """
        from pathlib import Path
        from .security import normalize_existing_file_path

        allowed_roots = state.allowed_fs_roots
        try:
            program_path = str(
                normalize_existing_file_path(
                    program_path,
                    allowed_roots,
                    kind_label="Program path",
                )
            )
            if deployment_path:
                deployment_path = str(
                    normalize_existing_file_path(
                        deployment_path,
                        allowed_roots,
                        kind_label="Deployment path",
                    )
                )
        except Exception:
            return json.dumps({"error": "Artifact path is outside allowed roots or invalid"})

        try:
            program = state.load_program(
                program_path,
                deployment_path,
                force_discard_overrides=force_discard_overrides,
            )
        except RuntimeError as exc:
            return json.dumps({"error": str(exc)})
        except Exception:
            return json.dumps({"error": "Failed to load program"})

        robot = program.robot
        return json.dumps({
            "name": program.name,
            "description": program.description,
            "robot": {
                "robot_id": robot.robot_id,
                "group_id": robot.group_id,
                "base_frame": robot.base_frame,
                "tcp_frame": robot.tcp_frame,
            },
            "scene_count": len(program.scene),
            "parameter_count": len(program.parameters),
            "source_path": state.program_source_path,
        })

    # ── Program Tree ─────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_program_tree(
        action: str = "read",
        tree: Optional[dict] = None,
        force_discard_overrides: bool = False,
    ) -> str:
        """Read or replace the current behavior tree.

        Args:
            action: "read" to get the current tree, "write" to replace it
            tree: The new tree (required when action is "write"). Must be a
                  nested dict with id, name, params, children fields.
            force_discard_overrides: If true, discard any existing runtime
                  overrides when writing a new tree

        The tree cannot be modified while execution is RUNNING (returns error).
        Always call manyforge_program_validate before writing.
        """
        if state.program is None:
            return json.dumps({"error": "No program loaded"})

        if action == "read":
            from .routes_program import _tree_to_response
            runtime_program = state.effective_program or state.program
            return json.dumps(_snakecase_keys({"tree": _tree_to_response(runtime_program.tree)}))

        if action == "write":
            if tree is None:
                return json.dumps({"error": "tree is required for write action"})
            if state.cycle_manager is not None and state.cycle_manager.state.value in {"running", "interrupt_handler"}:
                return json.dumps({"error": "Cannot modify tree while execution is active"})
            try:
                from manyforge_behavior.program import Program
                capability_keys: set[str] | None = None
                if state.deployment is not None:
                    capability_keys = set()
                    for adapter in state.deployment.adapters.values():
                        capability_keys.update(adapter.capability_keys)
                program_data = state.program.to_dict()
                program_data["tree"] = tree
                updated_program = Program.from_dict(
                    program_data,
                    state.catalog,
                    path=state.program_source_path or "<memory>",
                    skill_catalog=state.skill_catalog,
                    deployment_capability_keys=capability_keys,
                )
                deployment = state.deployment
                if deployment is not None:
                    deployment.validate_against_program(updated_program)
                    deployment.validate_capabilities(updated_program)
                state.apply_models(
                    updated_program,
                    deployment,
                    force_discard_overrides=force_discard_overrides,
                )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            except Exception:
                return json.dumps({"error": "Failed to apply tree"})
            return json.dumps({"status": "ok"})

        return json.dumps({"error": f"Unknown action: {action}"})

    # ── Program Validate ─────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_program_validate(tree: dict) -> str:
        """Validate a candidate behavior tree without applying it.

        This is a dry-run — no side effects. Always call this before
        manyforge_program_tree(action="write").

        Args:
            tree: The candidate tree as a nested dict with id, name, params, children.

        Returns JSON with:
        - valid: boolean
        - errors: list of {path, message} for structural errors
        - warnings: list of {path, message} for non-fatal issues
        """
        try:
            from manyforge_behavior.program import tree_node_from_dict, collect_tree_validation_issues
            tree_node = tree_node_from_dict(tree)
        except Exception as e:
            return json.dumps({"valid": False, "errors": [{"path": "/", "message": str(e)}], "warnings": []})

        parameter_defaults = (
            state.program.get_parameter_defaults()
            if state.program is not None
            else None
        )
        errors, warnings = collect_tree_validation_issues(
            tree_node,
            state.catalog,
            parameter_defaults=parameter_defaults,
            skill_catalog=state.skill_catalog,
            program_profile=(
                state.program.program_profile
                if state.program is not None
                else None
            ),
            deployment_capability_keys=(
                {
                    capability
                    for adapter in state.deployment.adapters.values()
                    for capability in adapter.capability_keys
                }
                if state.deployment is not None
                else None
            ),
        )
        return json.dumps({
            "valid": not errors,
            "errors": [{"path": issue.path, "message": issue.message} for issue in errors],
            "warnings": [{"path": issue.path, "message": issue.message} for issue in warnings],
        })

    # ── Cycle Control ────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_cycle_control(
        command: str,
        velocity_scale: Optional[float] = None,
    ) -> str:
        """Control program execution lifecycle.

        Args:
            command: One of "run", "stop", "reset", "status", "velocity"
            velocity_scale: Required when command is "velocity" (0.0 to 1.0).
                           0.0 will stall the robot — use "stop" to pause instead.

        State machine: IDLE → RUNNING → STOPPED → IDLE (via reset)
        """
        if command == "status":
            if state.cycle_manager is None:
                return json.dumps({"state": "idle", "velocity_scale": 1.0})
            return json.dumps({
                "state": state.cycle_manager.state.value,
                "velocity_scale": state.cycle_manager.velocity_scale,
            })

        if state.cycle_manager is None:
            return json.dumps({"error": "No program loaded"})

        if command == "run":
            override_status = state.runtime_override_status()
            if (
                state.cycle_manager.state.value == "stopped"
                and override_status["has_staged"]
            ):
                return json.dumps({
                    "error": (
                        "Staged runtime overrides cannot be activated while resuming from STOPPED; "
                        "reset the cycle first"
                    )
                })
            if (
                state.cycle_manager.state.value == "idle"
                and override_status["has_staged"]
            ):
                try:
                    state.activate_staged_runtime_overrides()
                except RuntimeError as exc:
                    return json.dumps({"error": str(exc)})
            state.cycle_manager.run()
            return json.dumps({"state": state.cycle_manager.state.value})
        elif command == "stop":
            state.cycle_manager.stop()
            return json.dumps({"state": state.cycle_manager.state.value})
        elif command == "reset":
            override_status = state.runtime_override_status()
            if override_status["has_staged"]:
                try:
                    state.activate_staged_runtime_overrides()
                except RuntimeError as exc:
                    return json.dumps({"error": str(exc)})
            else:
                state.cycle_manager.reset()
            return json.dumps({"state": state.cycle_manager.state.value})
        elif command == "velocity":
            if velocity_scale is None:
                return json.dumps({"error": "velocity_scale required for velocity command"})
            try:
                state.cycle_manager.set_velocity_scale(velocity_scale)
            except ValueError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps({"velocity_scale": state.cycle_manager.velocity_scale})
        else:
            return json.dumps({"error": f"Unknown command: {command}"})

    # ── Capabilities ─────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_capabilities() -> str:
        """Query the deployment-declared capability surface.

        Returns capability keys grouped by category (planning, execution,
        scene, sensing, context), adapter IDs, program profile, and
        contact-rich execution tier.
        """
        from .routes_skills import build_capabilities_payload
        try:
            return json.dumps(_snakecase_keys(build_capabilities_payload(state)))
        except Exception:
            return json.dumps({"error": "No deployment loaded"})

    # ── Skills ───────────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_skills() -> str:
        """Get the skill catalog — validated subtree templates for common operations.

        Each skill includes:
        - id, name, description
        - parameters with types and defaults
        - pre/postconditions (requires/provides/modifies)
        - body: the raw node subtree the skill produces
        - expands_to: legacy compatibility alias for body
        - example: a concrete usage example

        Prefer skills over raw nodes for standard operations like pick/place.
        """
        skills = []
        for skill in state.skill_catalog.skills:
            skills.append({
                "id": skill.id,
                "version": skill.version,
                "name": skill.name,
                "description": skill.description,
                "compatible_profiles": skill.compatible_profiles,
                "requires": skill.requires,
                "provides": skill.provides,
                "modifies": skill.modifies,
                "requires_capabilities": skill.requires_capabilities,
                "parameters": [
                    {
                        "name": p.name,
                        "type": p.type,
                        "required": p.required,
                        "default": p.default,
                        "description": p.description,
                    }
                    for p in skill.parameters
                ],
                "body": skill.body,
                "expands_to": skill.expands_to,
                "example": skill.example,
            })
        return json.dumps({"skills": skills, "timestamp_ns": time.time_ns()})

    # ── Intervention ─────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_intervention(
        action: str,
        tree: Optional[dict] = None,
        lease_timeout_ms: Optional[int] = None,
        resume_target: Optional[str] = None,
        promote_keys: Optional[dict[str, str]] = None,
        session_kind: Optional[str] = None,
        authority_source: Optional[str] = None,
        principal_id: Optional[str] = None,
        grant_type: Optional[str] = None,
        trigger_reason: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
        approval_mode: Optional[str] = None,
        artifact_origin: str = "saved_program",
    ) -> str:
        """Execute, monitor, or abort a temporary intervention subtree.

        Interventions run a validated one-shot subtree without modifying the
        saved program. The cycle must be IDLE or STOPPED to start an
        intervention.

        Args:
            action: One of "execute", "status", "pending_recovery", "abort", "heartbeat", "promote"
            tree: The subtree to execute (required for "execute").
                  Nested dict with id, name, params, children fields.
        """
        if state.cycle_manager is None:
            return json.dumps({"error": "No program loaded"})

        if action == "execute":
            if tree is None:
                return json.dumps({"error": "tree is required for execute action"})
            if not principal_id or not principal_id.strip():
                return json.dumps({"error": "principal_id is required for execute action"})
            principal_id = principal_id.strip()
            if not is_valid_principal_id(principal_id):
                return json.dumps(
                    {
                        "error": (
                            "principal_id must be 1-128 printable ASCII characters"
                        )
                    }
                )
            if grant_type is not None and grant_type not in ("human_approved", "policy_granted"):
                return json.dumps({"error": "grant_type must be one of: human_approved, policy_granted"})
            if trigger_reason is not None and trigger_reason not in _valid_trigger_reasons():
                return json.dumps(
                    {
                        "error": (
                            "trigger_reason must be one of: "
                            + ", ".join(sorted(_valid_trigger_reasons()))
                        )
                    }
                )
            from manyforge_behavior.program import (
                collect_tree_validation_issues,
                tree_node_from_dict,
            )

            try:
                tree_node = tree_node_from_dict(tree)
            except Exception as exc:
                return json.dumps({"error": f"Invalid intervention tree: {exc}"})

            capability_keys = None
            if state.deployment is not None:
                capability_keys = {
                    capability
                    for adapter in state.deployment.adapters.values()
                    for capability in adapter.capability_keys
                }
            errors, _warnings = collect_tree_validation_issues(
                tree_node,
                state.catalog,
                parameter_defaults=(
                    state.program.get_parameter_defaults()
                    if state.program is not None
                    else None
                ),
                blackboard_namespaces=(
                    state.program.blackboard_namespaces
                    if state.program is not None
                    else None
                ),
                blackboard_keys=merged_intervention_blackboard_keys(
                    tree_node,
                    state.catalog,
                    base_keys=(
                        state.program.blackboard_keys
                        if state.program is not None
                        else None
                    ),
                ),
                skill_catalog=state.skill_catalog,
                program_profile=(
                    state.program.program_profile
                    if state.program is not None
                    else None
                ),
                deployment_capability_keys=capability_keys,
            )
            if errors:
                return json.dumps(
                    {
                        "error": errors[0].message,
                        "details": [
                            {"path": issue.path, "message": issue.message}
                            for issue in errors
                        ],
                    }
                )

            parent_run_id = state.event_recorder.current_run_id
            if parent_run_id is None and state.cycle_manager.state.value == "stopped":
                parent_run_id = state.event_recorder.last_run_id
            try:
                session = state.cycle_manager.execute_intervention(
                    tree,
                    parent_run_id=parent_run_id,
                    lease_timeout_ms=lease_timeout_ms or 60_000,
                    resume_target=resume_target,
                    session_kind=session_kind,
                    authority_source=authority_source,
                    principal_id=principal_id,
                    grant_type=grant_type,
                    trigger_reason=trigger_reason,
                    trigger_node_instance=trigger_node_instance,
                    approval_mode=approval_mode,
                    artifact_origin=artifact_origin,
                )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(_intervention_session_dict(session))

        elif action == "status":
            session = state.cycle_manager.intervention
            if session is None:
                return json.dumps({"error": "No intervention session"})
            return json.dumps(_intervention_session_dict(session))

        elif action == "pending_recovery":
            request = state.cycle_manager.pending_delegated_recovery
            if request is None:
                return json.dumps({"error": "No delegated recovery request"})
            return json.dumps(_delegated_recovery_request_dict(request))

        elif action == "abort":
            session = state.cycle_manager.intervention
            if session is None or session.status != "running":
                return json.dumps({"error": "No running intervention to abort"})
            state.cycle_manager.abort_intervention()
            session = state.cycle_manager.intervention
            return json.dumps(_intervention_session_dict(session))

        elif action == "heartbeat":
            try:
                session = state.cycle_manager.renew_intervention_lease(
                    lease_timeout_ms=lease_timeout_ms,
                )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(_intervention_session_dict(session))

        elif action == "promote":
            if not isinstance(promote_keys, dict) or not promote_keys:
                return json.dumps({"error": "promote_keys is required for promote action"})
            try:
                session = state.cycle_manager.promote_intervention_values(promote_keys)
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(_intervention_session_dict(session))

        return json.dumps({"error": f"Unknown action: {action}"})

    # ── Runtime Overrides ────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_runtime_override(
        action: str,
        node_name: Optional[str] = None,
        params: Optional[dict[str, Any]] = None,
        include_staged: bool = False,
        target: Optional[str] = None,
        path: Optional[str] = None,
        authority_source: Optional[str] = None,
        approval_mode: Optional[str] = None,
        reason: str = "",
        session_id: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
    ) -> str:
        """Inspect or mutate the runtime override ledger.

        Actions:
        - "status": return active/staged/stale runtime overrides
        - "materialized_tree": return the tree produced by materializing runtime overrides
        - "apply": apply or stage an override for one node instance
        - "promote": materialize selected overrides into the loaded program or save them
        - "activate": activate staged overrides from IDLE
        - "revalidate": re-check stale overrides against the current program
        - "remove": remove one override by node instance name
        - "clear": clear all overrides
        """
        if state.program is None:
            return json.dumps({"error": "No program loaded"})

        if action == "status":
            return json.dumps(state.runtime_override_status())

        if action == "materialized_tree":
            from .routes_program import _tree_to_response
            try:
                program = state.materialized_runtime_override_program(
                    include_staged=include_staged,
                )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps({"tree": _tree_to_response(program.tree)})

        if action == "apply":
            if not node_name:
                return json.dumps({"error": "node_name is required for apply"})
            if not isinstance(params, dict) or not params:
                return json.dumps({"error": "params must be a non-empty mapping for apply"})
            try:
                state.apply_runtime_override(
                    node_name=node_name,
                    params=params,
                    authority_source=authority_source or "",
                    approval_mode=approval_mode or "",
                    reason=reason,
                    session_id=session_id,
                    trigger_node_instance=trigger_node_instance,
                )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(state.runtime_override_status())

        if action == "promote":
            if target not in {"loaded_program", "save"}:
                return json.dumps(
                    {"error": "target must be one of: loaded_program, save"}
                )
            try:
                if target == "loaded_program":
                    program = state.promote_runtime_overrides_to_loaded_program(
                        include_staged=include_staged,
                    )
                    saved_path = None
                else:
                    if not path:
                        path = state.program_source_path or None
                    if not path:
                        return json.dumps(
                            {"error": "path is required when no program source path is loaded"}
                        )
                    from .routes_program import _coerce_artifact_save_path
                    saved_path = _coerce_artifact_save_path(
                        path,
                        allowed_roots=state.allowed_fs_roots,
                        suffixes=(".program.yaml", ".program.yml"),
                        default_suffix=".program.yaml",
                        kind_label="Program path",
                    )
                    program = state.save_materialized_runtime_override_program(
                        saved_path,
                        include_staged=include_staged,
                        authority_source=authority_source or "",
                        approval_mode=approval_mode or "",
                    )
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            from .routes_program import _tree_to_response
            return json.dumps(
                {
                    "status": "ok",
                    "target": target,
                    "include_staged": include_staged,
                    "path": saved_path,
                    "tree": _tree_to_response(program.tree),
                    "runtime_override_status": state.runtime_override_status(),
                }
            )

        if action == "activate":
            try:
                state.activate_staged_runtime_overrides()
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(state.runtime_override_status())

        if action == "revalidate":
            try:
                results = state.revalidate_stale_overrides()
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            status = state.runtime_override_status()
            status["revalidation_results"] = results
            return json.dumps(status)

        if action == "remove":
            if not node_name:
                return json.dumps({"error": "node_name is required for remove"})
            try:
                removed = state.remove_runtime_override(node_name)
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            if not removed:
                return json.dumps({"error": "Runtime override not found"})
            return json.dumps(state.runtime_override_status())

        if action == "clear":
            try:
                state.clear_runtime_overrides()
            except RuntimeError as exc:
                return json.dumps({"error": str(exc)})
            return json.dumps(state.runtime_override_status())

        return json.dumps({"error": f"Unknown action: {action}"})

    # ── Execution Status ─────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_execution_status(
        include: str = "digest",
    ) -> str:
        """Get execution status information.

        Args:
            include: What to return — "digest", "history", "blackboard", or "all"

        - digest: High-level polling summary (state, current node, progress,
          completed/remaining nodes, object positions, errors, warnings)
        - history: Event log for current/last run
        - blackboard: py_trees blackboard key-value state
        - all: Everything combined
        """
        from .routes_execution import (
            build_execution_digest_payload,
            build_execution_history_payload,
            build_blackboard_snapshot_payload,
        )

        result: dict[str, Any] = {}
        if include in ("digest", "all"):
            result["digest"] = _snakecase_keys(build_execution_digest_payload(state))
        if include in ("history", "all"):
            result["history"] = _snakecase_keys(build_execution_history_payload(state))
        if include in ("blackboard", "all"):
            result["blackboard"] = _snakecase_keys(build_blackboard_snapshot_payload(state))

        if not result:
            return json.dumps({"error": f"Unknown include value: {include}"})
        return json.dumps(result)

    # ── Debug Export ─────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_debug_export(run_id: Optional[str] = None) -> str:
        """Export a full debug/trace bundle for post-mortem analysis.

        Includes kernel snapshot, metrics, execution digest, history,
        scene state, diagnostics, scene trace, blackboard, and node statuses.

        Args:
            run_id: Optional specific run ID. Defaults to the most recent run.
        """
        from .routes_debug import build_debug_export_payload
        return json.dumps(_snakecase_keys(build_debug_export_payload(state, run_id=run_id)))

    # ── Catalog ──────────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_catalog(entry_id: Optional[str] = None) -> str:
        """Query the behavior tree node catalog.

        Without entry_id: returns all available node types with their schemas.
        With entry_id: returns a single node type's full schema.

        Each entry includes: id, name, category, description, kind,
        children constraints, parameters with types/constraints/defaults,
        and required capabilities.

        Args:
            entry_id: Optional node type ID to query a single entry.
        """
        if entry_id is not None:
            try:
                entry = state.catalog.get(entry_id)
            except KeyError:
                return json.dumps({"error": f"Entry '{entry_id}' not found"})
            return json.dumps(_catalog_entry_dict(entry))

        entries = [_catalog_entry_dict(entry) for entry in state.catalog.entries]
        return json.dumps({"version": state.catalog.version, "entries": entries})

    # ── Scene Snapshot ──────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_scene_snapshot(
        width: int = 640,
        height: int = 480,
        format: str = "jpeg",
    ) -> str:
        """Render the 3D scene as a base64-encoded image.

        Requires HMI server running (--hmi-port). Returns base64 string
        and metadata, or error if no Viser client is connected.

        Args:
            width: Image width in pixels (64-1920, default 640)
            height: Image height in pixels (64-1080, default 480)
            format: Image format - "jpeg" or "png" (default "jpeg")
        """
        if format not in ("jpeg", "png"):
            return json.dumps({"error": "format must be 'jpeg' or 'png'"})
        width = max(64, min(1920, width))
        height = max(64, min(1080, height))

        hmi = state.hmi_server
        if hmi is None:
            return json.dumps({"error": "HMI server not running (start with --hmi-port)"})

        image_bytes = hmi.render_scene(width=width, height=height, format=format)
        if image_bytes is None:
            return json.dumps({"error": "No Viser client connected for rendering"})

        return json.dumps({
            "image_base64": base64.b64encode(image_bytes).decode("ascii"),
            "width": width,
            "height": height,
            "format": format,
            "timestamp_ns": time.monotonic_ns(),
        })

    # ── Program Save ────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_program_save(path: Optional[str] = None) -> str:
        """Save the currently loaded program to a YAML file.

        If path is omitted, saves to the original source path.
        Path must be within allowed filesystem roots.

        Args:
            path: Optional target path for the saved file.
        """
        if state.program is None:
            return json.dumps({"error": "No program loaded"})

        from .routes_program import _PROGRAM_SUFFIXES, _coerce_artifact_save_path

        try:
            target_path = _coerce_artifact_save_path(
                path,
                allowed_roots=state.allowed_fs_roots,
                suffixes=_PROGRAM_SUFFIXES,
                default_suffix=".program.yaml",
                kind_label="Program path",
            )
            state.program.save(target_path)
        except Exception as e:
            return json.dumps({"error": str(e)})

        return json.dumps({"status": "ok", "path": target_path})

    # ── File Browser ────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_files(
        path: Optional[str] = None,
        kind: Optional[str] = None,
    ) -> str:
        """Browse files on disk. Returns directory listing with file metadata.

        Use kind="program" or kind="deployment" to filter for artifact files.
        Path must be within allowed filesystem roots.

        Args:
            path: Directory path to browse (default: working directory)
            kind: Filter type - "program" or "deployment" (default: "program")
        """
        from .routes_filesystem import (
            _matches_kind,
            _normalize_browse_path,
            _root_shortcuts,
        )
        from .security import normalize_allowed_roots

        kind = kind or "program"
        if kind not in ("program", "deployment"):
            return json.dumps({"error": "kind must be 'program' or 'deployment'"})

        roots = normalize_allowed_roots(state.allowed_fs_roots)
        try:
            current_dir = _normalize_browse_path(path, roots)
        except Exception as e:
            return json.dumps({"error": str(e)})

        try:
            children = list(current_dir.iterdir())
        except PermissionError:
            return json.dumps({"error": "Permission denied"})
        except OSError:
            return json.dumps({"error": "Filesystem error"})

        entries = []
        for child in children:
            try:
                is_dir = child.is_dir()
            except OSError:
                continue
            if is_dir:
                entries.append({"name": child.name, "path": str(child), "type": "directory"})
            elif _matches_kind(child, kind):
                entries.append({"name": child.name, "path": str(child), "type": "file"})

        entries.sort(key=lambda e: (e["type"] != "directory", e["name"].casefold()))

        parent_path = None
        parent_dir = current_dir.parent
        if parent_dir != current_dir:
            from .security import is_path_within_roots
            if is_path_within_roots(parent_dir, roots):
                parent_path = str(parent_dir)

        return json.dumps({
            "path": str(current_dir),
            "parent_path": parent_path,
            "entries": entries,
            "root_shortcuts": _root_shortcuts(roots),
        })

    # ── Deployment Reader ───────────────────────────────────────────────

    @mcp.tool()
    def manyforge_deployment() -> str:
        """Get the currently loaded deployment configuration.

        Includes robot adapters, capability keys, model endpoints,
        prompt templates, and audit policy.
        """
        if state.deployment is None:
            return json.dumps({"error": "No deployment loaded"})

        from .routes_program import _deployment_to_full_response

        try:
            result = _deployment_to_full_response(
                state.deployment, state.deployment_source_path
            )
        except Exception as e:
            return json.dumps({"error": str(e)})

        return json.dumps(_snakecase_keys(result))

    # ── Kernel ─────────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_kernel(include: str = "snapshot") -> str:
        """Get kernel execution state and metrics.

        Args:
            include: What to return — "snapshot", "metrics", or "all"

        - snapshot: Current execution state (joint positions, active move, progress)
        - metrics: Accumulated counters (moves executed, planning time, etc.)
        - all: Both combined
        """
        from .routes_kernel import (
            build_kernel_snapshot_payload,
            build_kernel_metrics_payload,
        )

        result: dict[str, Any] = {}
        if include in ("snapshot", "all"):
            result["snapshot"] = _snakecase_keys(build_kernel_snapshot_payload(state))
        if include in ("metrics", "all"):
            try:
                result["metrics"] = build_kernel_metrics_payload(state)
            except Exception as e:
                result["metrics"] = {"error": str(e)}

        if not result:
            return json.dumps({"error": f"Unknown include value: {include}"})
        return json.dumps(result)

    # ── Session ────────────────────────────────────────────────────────

    @mcp.tool()
    def manyforge_session(action: str = "status") -> str:
        """Manage the composer session state.

        Args:
            action: One of "status", "reset"

        - status: Return current session info (loaded program, deployment,
          cycle state, override count, event count)
        - reset: Unload the current program and reset all state. Use with
          caution — this discards all runtime overrides and execution history.
        """
        if action == "status":
            override_status = (
                state.runtime_override_status()
                if state.program is not None
                else None
            )
            return json.dumps({
                "program_loaded": state.program is not None,
                "program_name": state.program.name if state.program else None,
                "program_source_path": state.program_source_path,
                "deployment_loaded": state.deployment is not None,
                "deployment_source_path": state.deployment_source_path,
                "cycle_state": (
                    state.cycle_manager.state.value
                    if state.cycle_manager
                    else "none"
                ),
                "runtime_overrides": override_status,
                "event_count": (
                    len(state.event_recorder.history())
                    if state.event_recorder
                    else 0
                ),
            })

        if action == "reset":
            if (
                state.cycle_manager is not None
                and state.cycle_manager.state.value in {"running", "interrupt_handler"}
            ):
                return json.dumps(
                    {"error": "Cannot reset session while execution is active — stop or wait for the handler first"}
                )
            # Reset cycle and overrides; program stays loaded
            if state.cycle_manager is not None:
                state.cycle_manager.reset()
            state.clear_runtime_overrides()
            return json.dumps({"status": "ok", "message": "Session reset (cycle + overrides cleared)"})

        return json.dumps({"error": f"Unknown action: {action}"})

    return mcp


# ── Helpers ──────────────────────────────────────────────────────────────

def _intervention_session_dict(session) -> dict[str, Any]:
    """Convert an InterventionSession to a JSON-serializable dict."""
    return {
        "session_id": session.session_id,
        "session_kind": session.session_kind,
        "authority_source": session.authority_source,
        "principal_id": session.principal_id,
        "grant_type": session.grant_type,
        "parent_run_id": session.parent_run_id,
        "entry_state": session.entry_state,
        "lease_timeout_ms": session.lease_timeout_ms,
        "lease_expires_at_ns": session.lease_expires_at_ns,
        "resume_target": session.resume_target,
        "trigger_reason": session.trigger_reason,
        "trigger_node_instance": session.trigger_node_instance,
        "approval_mode": session.approval_mode,
        "artifact_origin": session.artifact_origin,
        "status": session.status,
        "actual_outcome": session.actual_outcome,
        "error": session.error,
        "started_at_ns": session.started_at_ns,
        "finished_at_ns": session.finished_at_ns,
        "events": list(session.events),
        "promotions": list(session.promotions),
        "timestamp_ns": time.monotonic_ns(),
    }


def _delegated_recovery_request_dict(request) -> dict[str, Any]:
    """Convert a delegated recovery request to a JSON-serializable dict."""
    return {
        "request_id": request.request_id,
        "parent_run_id": request.parent_run_id,
        "requested_from_state": request.requested_from_state,
        "trigger_node_instance": request.trigger_node_instance,
        "trigger_node_id": request.trigger_node_id,
        "trigger_reason": request.trigger_reason.value if hasattr(request.trigger_reason, "value") else request.trigger_reason,
        "delegate_policy": request.delegate_policy,
        "allowed_authority_sources": list(request.allowed_authority_sources),
        "allowed_actions": list(request.allowed_actions),
        "allowed_resume_targets": list(request.allowed_resume_targets),
        "default_resume_target": request.default_resume_target,
        "runtime_override_policy": request.runtime_override_policy,
        "persistent_edit_policy": request.persistent_edit_policy,
        "requested_at_ns": request.requested_at_ns,
        "timestamp_ns": time.monotonic_ns(),
    }


def _catalog_entry_dict(entry) -> dict[str, Any]:
    """Convert a catalog entry to a JSON-serializable dict."""
    return {
        "id": entry.id,
        "name": entry.name,
        "category": entry.category,
        "description": entry.description,
        "stability": entry.stability,
        "kind": entry.kind,
        "children_min": entry.children_min,
        "children_max": entry.children_max,
        "parameters": [
            {
                "name": p.name,
                "type": p.type,
                "required": p.required,
                "default": p.default,
                "enum_values": p.enum_values,
                "constraints": p.constraints,
                "parameterizable": p.parameterizable,
                "ui_editor": p.ui_editor,
                "description": p.description,
            }
            for p in entry.parameters
        ],
        "bb_reader_params": list(entry.bb_reader_params),
        "bb_writer_params": list(entry.bb_writer_params),
        "required_capabilities": entry.required_capabilities,
    }


_CAMEL_TO_SNAKE_RE_1 = re.compile(r"(.)([A-Z][a-z]+)")
_CAMEL_TO_SNAKE_RE_2 = re.compile(r"([a-z0-9])([A-Z])")


def _camel_to_snake(value: str) -> str:
    value = _CAMEL_TO_SNAKE_RE_1.sub(r"\1_\2", value)
    value = _CAMEL_TO_SNAKE_RE_2.sub(r"\1_\2", value)
    return value.lower()


def _snakecase_keys(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            _camel_to_snake(str(key)): _snakecase_keys(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [_snakecase_keys(item) for item in value]
    return value
