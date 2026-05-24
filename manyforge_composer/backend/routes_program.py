"""Program and deployment routes - /api/program/* and /api/deployment/*"""
from __future__ import annotations

import logging
from pathlib import Path

from fastapi import APIRouter, HTTPException, Query
from manyforge_behavior.deployment import Deployment, DeploymentValidationError
from manyforge_behavior.program import (
    BlackboardKey,
    BlackboardNamespaceField,
    BlackboardNamespace,
    Program,
    ProgramValidationError,
    collect_tree_validation_issues,
    tree_node_from_dict,
)

from .assistant_tool_schemas import enrich_assistant_tool_descriptor
from .models import (
    LoadProgramRequest,
    ProgramResponse,
    ProgramTreeResponse,
    TreeNodeResponse,
    RobotResponse,
    LoadDeploymentRequest,
    DeploymentResponse,
    RobotConfigResponse,
    SaveProgramRequest,
    UpdateTreeRequest,
    ProgramValidateRequest,
    ProgramValidateResponse,
    ProgramValidationIssueResponse,
    RuntimeOverrideRequest,
    RuntimeOverridePromoteRequest,
    RuntimeOverridePromoteResponse,
    RuntimeOverrideStatusResponse,
    UpdateDeploymentRequest,
    SaveDeploymentRequest,
    FullDeploymentResponse,
    ApplySessionRequest,
    ApplySessionResponse,
)
from .state import ComposerState
from .security import normalize_existing_file_path, normalize_writable_file_path

logger = logging.getLogger(__name__)

_PROGRAM_SUFFIXES = (".program.yaml", ".program.yml")
_DEPLOYMENT_SUFFIXES = (".deployment.yaml", ".deployment.yml")


def _deployment_capability_keys(state: ComposerState) -> set[str] | None:
    if state.deployment is None:
        return None
    capability_keys: set[str] = set()
    for adapter in state.deployment.adapters.values():
        capability_keys.update(adapter.capability_keys)
    return capability_keys


def _coerce_artifact_save_path(
    raw_path: str,
    *,
    allowed_roots: list[str],
    suffixes: tuple[str, ...],
    default_suffix: str,
    kind_label: str,
) -> str:
    candidate = Path(raw_path)
    name_lower = candidate.name.lower()
    if not name_lower.endswith(suffixes):
        candidate = candidate.parent / f"{candidate.name}{default_suffix}"
    return str(
        normalize_writable_file_path(
            str(candidate),
            allowed_roots,
            kind_label=kind_label,
        )
    )


def _coerce_artifact_load_path(
    raw_path: str,
    *,
    allowed_roots: list[str],
    suffixes: tuple[str, ...],
    kind_label: str,
) -> str:
    normalized = normalize_existing_file_path(
        raw_path,
        allowed_roots,
        kind_label=kind_label,
    )
    if not normalized.name.lower().endswith(suffixes):
        raise HTTPException(status_code=400, detail=f"Invalid {kind_label.lower()} suffix")
    return str(normalized)


def _tree_to_response(node) -> dict:
    """Convert a TreeNode to response dict."""
    return TreeNodeResponse(
        id=node.id,
        name=node.name,
        params=node.params,
        modificationPolicy=node.modification_policy,
        recoveryPolicy=node.recovery_policy,
        children=[_tree_to_response(child) for child in node.children],
    ).model_dump(by_alias=True)


def _runtime_override_status_response(state: ComposerState) -> dict:
    status = state.runtime_override_status()
    return RuntimeOverrideStatusResponse(
        active=status["active"],
        staged=status["staged"],
        stale=status["stale"],
        activeCount=status["active_count"],
        stagedCount=status["staged_count"],
        staleCount=status["stale_count"],
        hasActive=status["has_active"],
        hasStaged=status["has_staged"],
        hasStale=status["has_stale"],
        canActivateNow=status["can_activate_now"],
        requiresResetToActivate=status["requires_reset_to_activate"],
        currentTreeOrigin=status["current_tree_origin"],
        baseProgramHash=status["base_program_hash"],
        nextEffectiveTree=(
            status["next_effective_tree"]
            if status["next_effective_tree"] is not None
            else None
        ),
        timestampNs=status["timestamp_ns"],
    ).model_dump(by_alias=True)


def _model_endpoint_capabilities_payload(endpoint) -> dict:
    capabilities = endpoint.capabilities
    return {
        "supportsResponseTools": capabilities.supports_response_tools,
        "supportsJsonObject": capabilities.supports_json_object,
        "supportsImageInput": capabilities.supports_image_input,
        "supportedImageTransports": list(capabilities.supported_image_transports),
        "toolCallFamily": capabilities.tool_call_family,
        "latencyBudgetMs": capabilities.latency_budget_ms,
    }


def _assistant_config_payload(deployment: Deployment) -> dict | None:
    if deployment.assistant is None:
        return None
    return {
        "provider": deployment.assistant.provider,
        "endpointAlias": deployment.assistant.endpoint_alias,
        "enabled": deployment.assistant.enabled,
    }


def _assistant_modes_payload(deployment: Deployment) -> dict:
    return {
        mode_id: {
            "enabled": mode.enabled,
            "description": mode.description,
            "authority": {
                "sessionKind": mode.authority.session_kind,
                "approvalMode": mode.authority.approval_mode,
                "review": mode.authority.review,
            },
            "catalog": {
                "skills": list(mode.catalog.skills),
                "tools": list(mode.catalog.tools),
                "nodes": list(mode.catalog.nodes),
            },
        }
        for mode_id, mode in deployment.assistant_modes.items()
    }


def _tool_catalog_payload(deployment: Deployment) -> list[dict]:
    tools: list[dict] = []
    for tool in deployment.tool_catalog.values():
        raw = {
            "id": tool.tool_id,
            "effect": tool.effect,
            "description": tool.description,
        }
        raw.update(tool.extra)
        tools.append(enrich_assistant_tool_descriptor(raw))
    return tools


def _validate_deployment_against_catalogs(
    state: ComposerState,
    deployment: Deployment,
) -> None:
    deployment.validate_assistant_modes(state.catalog, state.skill_catalog)


def _gripper_to_serialized_dict(
    state: ComposerState | None,
    gripper_id: str,
    gripper,
) -> dict:
    """Serialize one gripper config for the API.

    When ``state`` is provided AND a URDF is loaded, also resolve the
    auto-discovered visualization root + tcp→root transform (used by
    the Move-To unreachable-ghost ADR-0018 path — see
    `routes_urdf.resolve_gripper_visualization`). Pure read-only; on
    any failure the visualization field is simply omitted (None),
    which the frontend handles via its floor-fallback marker.
    """
    payload: dict = {
        "commandMode": gripper.command_mode,
        "groupId": gripper.group_id,
        "jointName": gripper.joint_name,
        "motionTimeS": gripper.motion_time_s,
        "srdfPresets": dict(gripper.srdf_presets),
        "description": gripper.description,
        "touchLinks": list(gripper.touch_links),
        "mountFrame": gripper.mount_frame,
        "attachFrame": gripper.attach_frame,
        "tcpFrame": gripper.tcp_frame,
        "auxiliaryFrames": list(gripper.auxiliary_frames),
    }
    if state is None:
        return payload
    # Defer the import to avoid a circular dep at module load.
    try:
        from .routes_urdf import resolve_gripper_visualization
    except Exception:
        logger.debug(
            "routes_urdf not importable; gripper visualization disabled",
            exc_info=True,
        )
        return payload
    # SRDF gripper joint names tiebreak: prefer the subtree that
    # contains the gripper's SRDF group joints when mount_frame has
    # multiple children. For Robotiq with one declared finger joint
    # this gives exactly the expected attachment; for arms with no
    # SRDF gripper group it falls back to BFS-first.
    srdf_gripper_joints: tuple[str, ...] = ()
    joint_name = (gripper.joint_name or "").strip()
    if joint_name:
        srdf_gripper_joints = (joint_name,)
    try:
        resolution = resolve_gripper_visualization(
            state,
            mount_frame=gripper.mount_frame,
            tcp_frame=gripper.tcp_frame,
            srdf_gripper_joint_names=srdf_gripper_joints,
        )
    except Exception:
        logger.debug(
            "Gripper visualization resolution failed for %s", gripper_id,
            exc_info=True,
        )
        resolution = None
    if resolution is not None:
        payload["visualization"] = {
            "gripperRootLink": resolution.gripper_root_link,
            "tcpToRootPositionM": list(resolution.tcp_to_root_position_m),
            "tcpToRootOrientationXyzw": list(
                resolution.tcp_to_root_orientation_xyzw
            ),
            "subtreeLinks": [
                {
                    "name": link.name,
                    "positionM": list(link.position_m),
                    "orientationXyzw": list(link.orientation_xyzw),
                }
                for link in resolution.subtree_links
            ],
        }
    return payload


def _deployment_to_full_response(
    deployment: Deployment,
    source_path: str,
    state: ComposerState | None = None,
) -> dict:
    return FullDeploymentResponse(
        apiVersion=deployment.api_version,
        schemaVersion=deployment.schema_version,
        name=deployment.name,
        description=deployment.description,
        programProfile=deployment.program_profile,
        robotConfig=RobotConfigResponse(
            robotId=deployment.robot_config.robot_id,
            groupId=deployment.robot_config.group_id,
            urdfPath=deployment.robot_config.urdf_path,
            srdfPath=deployment.robot_config.srdf_path,
            packagePaths=deployment.robot_config.package_paths,
            globalMaxTcpLinearSpeedMps=deployment.robot_config.global_max_tcp_linear_speed_mps,
            streamingJointLimits={
                name: {
                    "maxVelocityRadS": limit.max_velocity_rad_s,
                    "maxAccelerationRadS2": limit.max_acceleration_rad_s2,
                }
                for name, limit in deployment.robot_config.streaming_joint_limits.items()
            },
        ),
        grippers={
            gripper_id: _gripper_to_serialized_dict(state, gripper_id, gripper)
            for gripper_id, gripper in deployment.grippers.items()
        },
        adapters={
            slot: {
                "adapterId": adapter.adapter_id,
                "adapterVersion": adapter.adapter_version,
                "config": adapter.config,
                "supportedProfiles": adapter.supported_profiles,
                "capabilityKeys": adapter.capability_keys,
            }
            for slot, adapter in deployment.adapters.items()
        },
        orchestrator={
            "engine": deployment.orchestrator.engine,
            "config": deployment.orchestrator.config,
        },
        launch={
            "runner": {
                "module": deployment.launch.runner.module,
                "args": deployment.launch.runner.args,
            },
            "uiBackend": (
                {
                    "module": deployment.launch.ui_backend.module,
                    "args": deployment.launch.ui_backend.args,
                    "enabled": deployment.launch.ui_backend.enabled,
                }
                if deployment.launch and deployment.launch.ui_backend
                else None
            ),
        },
        modelAuditPolicy=(
            {
                "storeFullPrompts": deployment.model_audit_policy.store_full_prompts,
                "storeFullResponses": deployment.model_audit_policy.store_full_responses,
                "hashAlgorithm": deployment.model_audit_policy.hash_algorithm,
                "redactArgumentValues": deployment.model_audit_policy.redact_argument_values,
            }
            if deployment.model_audit_policy is not None
            else None
        ),
        modelEndpointProfileRegistry=deployment.model_endpoint_profile_registry,
        modelEndpoints={
            alias: {
                "profileRef": endpoint.profile_ref,
                "provider": endpoint.provider,
                "modelId": endpoint.model_id,
                "baseUrl": endpoint.base_url,
                "imageFetchBaseUrl": endpoint.image_fetch_base_url,
                "apiKeyEnv": endpoint.api_key_env,
                "defaultTimeoutS": endpoint.default_timeout_s,
                "maxTokens": endpoint.max_tokens,
                "capabilities": _model_endpoint_capabilities_payload(endpoint),
            }
            for alias, endpoint in deployment.model_endpoints.items()
        },
        assistant=_assistant_config_payload(deployment),
        assistantModes=_assistant_modes_payload(deployment),
        toolCatalog=_tool_catalog_payload(deployment),
        promptTemplates={
            template_id: {
                "version": template.version,
                "templateText": template.template_text,
                "outputSchema": template.output_schema,
                "responseTool": template.response_tool,
            }
            for template_id, template in deployment.prompt_templates.items()
        },
        authorityMatrix={
            domain: {
                "owner": config.owner,
                "onDisconnect": config.on_disconnect,
                "description": config.description,
            }
            for domain, config in deployment.authority_matrix.items()
        },
        poseSources=[
            {
                "id": source.source_id,
                "kind": source.kind,
                "enabled": source.enabled,
                "maxUpdateHz": source.max_update_hz,
                "staleAfterS": source.stale_after_s,
                "frameId": source.frame_id,
                "servicePrefix": source.service_prefix,
                "topic": source.topic,
                "description": source.description,
                "objects": {
                    object_id: {
                        "entity": binding.entity,
                        "frameId": binding.frame_id,
                        "applyPolicy": binding.apply_policy,
                        "confidenceMin": binding.confidence_min,
                        "metadata": dict(binding.metadata),
                    }
                    for object_id, binding in source.objects.items()
                },
            }
            for source in deployment.pose_sources.values()
        ],
        sourcePath=source_path,
    ).model_dump(by_alias=True)


def _program_parameters_to_response(program: Program) -> list[dict]:
    return [
        {
            "name": parameter.name,
            "type": parameter.type,
            "default": parameter.default,
            "constraints": parameter.constraints,
            "description": parameter.description,
        }
        for parameter in program.parameters.values()
    ]


def _program_scene_to_response(program: Program) -> list[dict]:
    return [
        {
            "objectId": resource.object_id,
            "shape": resource.shape,
            "pose": resource.pose,
        }
        for resource in program.scene
    ]


def _program_blackboard_to_response(program: Program) -> dict:
    return {
        "namespaces": [
            {
                "id": namespace.id,
                "contract": namespace.contract,
                "prefix": namespace.prefix,
                "description": namespace.description,
                "fields": [
                    {
                        "name": field.name,
                        "type": field.type,
                        "description": field.description,
                    }
                    for field in namespace.fields
                ],
            }
            for namespace in program.blackboard_namespaces.values()
        ],
        "keys": [
            {
                "id": key.id,
                "type": key.type,
                "key": key.key,
                "description": key.description,
            }
            for key in program.blackboard_keys.values()
        ],
    }


def create_program_router(state: ComposerState) -> APIRouter:
    """Create the program router with the given state."""
    router = APIRouter()
    
    @router.post("/program/load", response_model=ProgramResponse, response_model_by_alias=True)
    def load_program(request: LoadProgramRequest) -> dict:
        """Load a program from the given path."""
        try:
            program_path = _coerce_artifact_load_path(
                request.path,
                allowed_roots=state.allowed_fs_roots,
                suffixes=_PROGRAM_SUFFIXES,
                kind_label="Program path",
            )
            deployment_path = None
            if request.deployment_path:
                deployment_path = _coerce_artifact_load_path(
                    request.deployment_path,
                    allowed_roots=state.allowed_fs_roots,
                    suffixes=_DEPLOYMENT_SUFFIXES,
                    kind_label="Deployment path",
                )
            program = state.load_program(
                program_path,
                deployment_path,
                force_discard_overrides=request.force_discard_overrides,
                defer_initial_runtime_reset=request.defer_initial_runtime_reset,
            )
            # Drop assistant delta baselines — a freshly loaded program
            # makes any prior conversation's baseline meaningless. Next
            # tool call re-baselines against the loaded state.
            from ._assistant_delta import reset_all as _reset_assistant_baselines
            _reset_assistant_baselines()
        except HTTPException:
            raise
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except (ProgramValidationError, DeploymentValidationError) as exc:
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.warning("Failed to load program: %s", e)
            raise HTTPException(status_code=400, detail="Failed to load program")

        robot = program.robot
        return ProgramResponse(
            name=program.name,
            description=program.description,
            programProfile=program.program_profile,
            robot=RobotResponse(
                robotId=robot.robot_id,
                groupId=robot.group_id,
                baseFrame=robot.base_frame,
                tcpFrame=robot.tcp_frame,
                basePose=robot.base_pose,
            ),
            sceneCount=len(program.scene),
            sceneResources=_program_scene_to_response(program),
            parameterCount=len(program.parameters),
            parameters=_program_parameters_to_response(program),
            blackboard=_program_blackboard_to_response(program),
            sourcePath=state.program_source_path,
        ).model_dump(by_alias=True)

    @router.get("/program", response_model=ProgramResponse, response_model_by_alias=True)
    def get_program() -> dict:
        """Get the currently loaded program."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")

        program = state.program
        robot = program.robot
        return ProgramResponse(
            name=program.name,
            description=program.description,
            programProfile=program.program_profile,
            robot=RobotResponse(
                robotId=robot.robot_id,
                groupId=robot.group_id,
                baseFrame=robot.base_frame,
                tcpFrame=robot.tcp_frame,
                basePose=robot.base_pose,
            ),
            sceneCount=len(program.scene),
            sceneResources=_program_scene_to_response(program),
            parameterCount=len(program.parameters),
            parameters=_program_parameters_to_response(program),
            blackboard=_program_blackboard_to_response(program),
            sourcePath=state.program_source_path,
        ).model_dump(by_alias=True)

    @router.get("/program/saved", response_model=ProgramResponse, response_model_by_alias=True)
    def get_saved_program() -> dict:
        """Get the saved program artifact from disk."""
        if not state.program_source_path:
            raise HTTPException(status_code=404, detail="No saved program path available")
        try:
            saved_program = Program(
                state.program_source_path,
                state.catalog,
                skill_catalog=state.skill_catalog,
                deployment_capability_keys=_deployment_capability_keys(state),
            )
        except Exception as e:
            logger.error("Failed to read saved program: %s", e)
            raise HTTPException(status_code=500, detail="Failed to read saved program")
        robot = saved_program.robot
        return ProgramResponse(
            name=saved_program.name,
            description=saved_program.description,
            programProfile=saved_program.program_profile,
            robot=RobotResponse(
                robotId=robot.robot_id,
                groupId=robot.group_id,
                baseFrame=robot.base_frame,
                tcpFrame=robot.tcp_frame,
                basePose=robot.base_pose,
            ),
            sceneCount=len(saved_program.scene),
            sceneResources=_program_scene_to_response(saved_program),
            parameterCount=len(saved_program.parameters),
            parameters=_program_parameters_to_response(saved_program),
            blackboard=_program_blackboard_to_response(saved_program),
            sourcePath=state.program_source_path,
        ).model_dump(by_alias=True)
    
    @router.get("/program/tree", response_model=ProgramTreeResponse, response_model_by_alias=True)
    def get_program_tree() -> dict:
        """Get the behavior tree of the currently loaded program."""
        if state.effective_program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        
        return ProgramTreeResponse(
            tree=_tree_to_response(state.effective_program.tree),
        ).model_dump(by_alias=True)

    @router.post("/program/validate", response_model=ProgramValidateResponse, response_model_by_alias=True)
    def validate_program_tree(request: ProgramValidateRequest) -> dict:
        """Validate a candidate tree against the catalog and current program context."""
        try:
            tree = tree_node_from_dict(request.tree.model_dump())
        except Exception as e:
            logger.warning("Invalid tree structure: %s", e)
            raise HTTPException(status_code=400, detail=f"Invalid tree structure: {e}")

        parameter_defaults = None
        blackboard_namespaces = None
        blackboard_keys = None
        if state.program is not None:
            parameter_defaults = state.program.get_parameter_defaults()
            blackboard_namespaces = state.program.blackboard_namespaces
            blackboard_keys = state.program.blackboard_keys
        if request.parameters is not None:
            parameter_defaults = {
                parameter.name: parameter.default
                for parameter in request.parameters
            }
        if request.blackboard is not None:
            blackboard_namespaces = {
                namespace.id: BlackboardNamespace(
                    id=namespace.id,
                    contract=namespace.contract,
                    prefix=namespace.prefix,
                    description=namespace.description,
                    fields=[
                        BlackboardNamespaceField(
                            name=field.name,
                            type=field.type,
                            description=field.description,
                        )
                        for field in namespace.fields
                    ],
                )
                for namespace in request.blackboard.namespaces
            }
            blackboard_keys = {
                key.id: BlackboardKey(
                    id=key.id,
                    type=key.type,
                    key=key.key,
                    description=key.description,
                )
                for key in request.blackboard.keys
            }
        errors, warnings = collect_tree_validation_issues(
            tree,
            state.catalog,
            parameter_defaults=parameter_defaults,
            blackboard_namespaces=blackboard_namespaces,
            blackboard_keys=blackboard_keys,
            skill_catalog=state.skill_catalog,
            program_profile=state.program.program_profile if state.program is not None else None,
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        return ProgramValidateResponse(
            valid=not errors,
            errors=[
                ProgramValidationIssueResponse(path=issue.path, message=issue.message)
                for issue in errors
            ],
            warnings=[
                ProgramValidationIssueResponse(path=issue.path, message=issue.message)
                for issue in warnings
            ],
        ).model_dump(by_alias=True)

    @router.get("/program/runtime-overrides", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def get_runtime_override_status() -> dict:
        """Get the current runtime override ledger."""
        return _runtime_override_status_response(state)

    @router.get("/program/runtime-overrides/materialized-tree", response_model=ProgramTreeResponse, response_model_by_alias=True)
    def get_materialized_runtime_override_tree(
        include_staged: bool = Query(default=False, alias="includeStaged"),
    ) -> dict:
        """Return a non-destructive tree snapshot with runtime overrides materialized."""
        try:
            program = state.materialized_runtime_override_program(
                include_staged=include_staged,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc
        return ProgramTreeResponse(
            tree=_tree_to_response(program.tree),
        ).model_dump(by_alias=True)

    @router.post("/program/runtime-overrides", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def apply_runtime_override(request: RuntimeOverrideRequest) -> dict:
        """Apply or stage a runtime override for a single node instance."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            state.apply_runtime_override(
                node_name=request.node_name,
                params=request.params,
                authority_source=request.authority_source,
                approval_mode=request.approval_mode,
                reason=request.reason,
                session_id=request.session_id,
                trigger_node_instance=request.trigger_node_instance,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except Exception as exc:
            logger.warning("Failed to apply runtime override: %s", exc)
            raise HTTPException(status_code=400, detail="Failed to apply runtime override")
        return _runtime_override_status_response(state)

    @router.post("/program/runtime-overrides/activate", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def activate_runtime_overrides() -> dict:
        """Activate any staged runtime overrides from IDLE."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            state.activate_staged_runtime_overrides()
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except Exception as exc:
            logger.warning("Failed to activate runtime overrides: %s", exc)
            raise HTTPException(status_code=400, detail="Failed to activate runtime overrides")
        return _runtime_override_status_response(state)

    @router.delete("/program/runtime-overrides/{node_name}", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def remove_runtime_override(node_name: str) -> dict:
        """Remove a runtime override by node instance name."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            removed = state.remove_runtime_override(node_name)
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        if not removed:
            raise HTTPException(status_code=404, detail="Runtime override not found")
        return _runtime_override_status_response(state)

    @router.post("/program/runtime-overrides/clear", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def clear_runtime_overrides() -> dict:
        """Clear all runtime overrides."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            state.clear_runtime_overrides()
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        return _runtime_override_status_response(state)

    @router.post("/program/runtime-overrides/revalidate", response_model=RuntimeOverrideStatusResponse, response_model_by_alias=True)
    def revalidate_runtime_overrides() -> dict:
        """Revalidate stale overrides against the current program.

        Overrides that are still valid get their base_program_hash updated.
        Overrides that are no longer valid are removed from the ledger.
        """
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            results = state.revalidate_stale_overrides()
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        response = _runtime_override_status_response(state)
        response["revalidation_results"] = results
        return response

    @router.post(
        "/program/runtime-overrides/promote",
        response_model=RuntimeOverridePromoteResponse,
        response_model_by_alias=True,
    )
    def promote_runtime_overrides(request: RuntimeOverridePromoteRequest) -> dict:
        """Materialize runtime overrides into the loaded program or save them to disk."""
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            if request.target == "loaded_program":
                program = state.promote_runtime_overrides_to_loaded_program(
                    include_staged=request.include_staged,
                )
                target_path = None
            else:
                if request.path:
                    target_path = _coerce_artifact_save_path(
                        request.path,
                        allowed_roots=state.allowed_fs_roots,
                        suffixes=_PROGRAM_SUFFIXES,
                        default_suffix=".program.yaml",
                        kind_label="Program path",
                    )
                elif state.program_source_path:
                    target_path = str(
                        normalize_writable_file_path(
                            state.program_source_path,
                            state.allowed_fs_roots,
                            kind_label="Program path",
                        )
                    )
                else:
                    raise HTTPException(
                        status_code=400,
                        detail="path is required when the loaded program has no source path",
                    )
                program = state.save_materialized_runtime_override_program(
                    target_path,
                    include_staged=request.include_staged,
                    authority_source=request.authority_source or "",
                    approval_mode=request.approval_mode or "",
                )
            return RuntimeOverridePromoteResponse(
                status="ok",
                target=request.target,
                includeStaged=request.include_staged,
                path=target_path,
                tree=_tree_to_response(program.tree),
                runtimeOverrideStatus=_runtime_override_status_response(state),
            ).model_dump(by_alias=True)
        except HTTPException:
            raise
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except Exception as exc:
            logger.warning("Failed to promote runtime overrides: %s", exc)
            raise HTTPException(
                status_code=400,
                detail="Failed to promote runtime overrides",
            )

    @router.get("/program/saved/tree", response_model=ProgramTreeResponse, response_model_by_alias=True)
    def get_saved_program_tree() -> dict:
        """Get the behavior tree from the saved program artifact on disk."""
        if not state.program_source_path:
            raise HTTPException(status_code=404, detail="No saved program path available")
        try:
            saved_program = Program(
                state.program_source_path,
                state.catalog,
                skill_catalog=state.skill_catalog,
                deployment_capability_keys=_deployment_capability_keys(state),
            )
        except Exception as e:
            logger.error("Failed to read saved program: %s", e)
            raise HTTPException(status_code=500, detail="Failed to read saved program")
        return ProgramTreeResponse(
            tree=_tree_to_response(saved_program.tree),
        ).model_dump(by_alias=True)
    
    @router.post("/deployment/load", response_model=DeploymentResponse, response_model_by_alias=True)
    def load_deployment(request: LoadDeploymentRequest) -> dict:
        """Load a deployment from the given path."""
        try:
            deployment_path = _coerce_artifact_load_path(
                request.path,
                allowed_roots=state.allowed_fs_roots,
                suffixes=_DEPLOYMENT_SUFFIXES,
                kind_label="Deployment path",
            )
            deployment = Deployment(deployment_path)
            _validate_deployment_against_catalogs(state, deployment)
            state.deployment = deployment
            state.deployment_source_path = deployment_path
        except HTTPException:
            raise
        except DeploymentValidationError as exc:
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.warning("Failed to load deployment: %s", e)
            raise HTTPException(status_code=400, detail="Failed to load deployment")
        
        deployment = state.deployment
        robot_config = deployment.robot_config
        return DeploymentResponse(
            name=deployment.name,
            description=deployment.description,
            programProfile=deployment.program_profile,
            robotConfig=RobotConfigResponse(
                robotId=robot_config.robot_id,
                groupId=robot_config.group_id,
                urdfPath=robot_config.urdf_path,
                srdfPath=robot_config.srdf_path,
                packagePaths=robot_config.package_paths,
                globalMaxTcpLinearSpeedMps=robot_config.global_max_tcp_linear_speed_mps,
                streamingJointLimits={
                    name: {
                        "maxVelocityRadS": limit.max_velocity_rad_s,
                        "maxAccelerationRadS2": limit.max_acceleration_rad_s2,
                    }
                    for name, limit in robot_config.streaming_joint_limits.items()
                },
            ),
            grippers={
                gripper_id: _gripper_to_serialized_dict(state, gripper_id, gripper)
                for gripper_id, gripper in deployment.grippers.items()
            },
        ).model_dump(by_alias=True)

    @router.get("/deployment", response_model=DeploymentResponse, response_model_by_alias=True)
    def get_deployment() -> dict:
        """Get the currently loaded deployment."""
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        
        deployment = state.deployment
        robot_config = deployment.robot_config
        return DeploymentResponse(
            name=deployment.name,
            description=deployment.description,
            programProfile=deployment.program_profile,
            robotConfig=RobotConfigResponse(
                robotId=robot_config.robot_id,
                groupId=robot_config.group_id,
                urdfPath=robot_config.urdf_path,
                srdfPath=robot_config.srdf_path,
                packagePaths=robot_config.package_paths,
                globalMaxTcpLinearSpeedMps=robot_config.global_max_tcp_linear_speed_mps,
                streamingJointLimits={
                    name: {
                        "maxVelocityRadS": limit.max_velocity_rad_s,
                        "maxAccelerationRadS2": limit.max_acceleration_rad_s2,
                    }
                    for name, limit in robot_config.streaming_joint_limits.items()
                },
            ),
            grippers={
                gripper_id: _gripper_to_serialized_dict(state, gripper_id, gripper)
                for gripper_id, gripper in deployment.grippers.items()
            },
        ).model_dump(by_alias=True)

    @router.get("/deployment/full", response_model=FullDeploymentResponse, response_model_by_alias=True)
    def get_full_deployment() -> dict:
        """Get the currently loaded full deployment artifact."""
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        return _deployment_to_full_response(
            state.deployment, state.deployment_source_path, state=state,
        )

    @router.get("/deployment/saved/full", response_model=FullDeploymentResponse, response_model_by_alias=True)
    def get_saved_full_deployment() -> dict:
        """Get the saved deployment artifact from disk."""
        if not state.deployment_source_path:
            raise HTTPException(status_code=404, detail="No saved deployment path available")
        try:
            saved_deployment = Deployment(state.deployment_source_path)
        except Exception as e:
            logger.error("Failed to read saved deployment: %s", e)
            raise HTTPException(status_code=500, detail="Failed to read saved deployment")
        # Resolve visualization against the LOADED URDF (state-side),
        # not from the saved-on-disk deployment: the URDF cache key is
        # the runtime URDF path, and the saved deployment shares it.
        return _deployment_to_full_response(
            saved_deployment, state.deployment_source_path, state=state,
        )

    @router.put("/program/tree")
    def update_program_tree(request: UpdateTreeRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply program changes while execution is active")
        try:
            program_data = state.program.to_dict()
            program_data["tree"] = request.tree.model_dump()
            updated_program = Program.from_dict(
                program_data,
                state.catalog,
                path=state.program_source_path or "<memory>",
                skill_catalog=state.skill_catalog,
                deployment_capability_keys=_deployment_capability_keys(state),
            )
            deployment = state.deployment
            if deployment is not None:
                deployment.validate_against_program(updated_program)
                deployment.validate_capabilities(updated_program)
            state.apply_models(
                updated_program,
                deployment,
                force_discard_overrides=request.force_discard_overrides,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except (ProgramValidationError, DeploymentValidationError) as exc:
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.warning("Failed to apply tree update: %s", e)
            raise HTTPException(status_code=400, detail="Failed to apply tree update")
        return {"status": "ok"}

    @router.post("/program/save")
    def save_program(request: SaveProgramRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        try:
            target_path = _coerce_artifact_save_path(
                request.path,
                allowed_roots=state.allowed_fs_roots,
                suffixes=_PROGRAM_SUFFIXES,
                default_suffix=".program.yaml",
                kind_label="Program path",
            )
            program_data = state.program.to_dict()
            if request.tree is not None:
                program_data["tree"] = request.tree.model_dump()
            if request.scene is not None:
                program_data["scene"] = [
                    resource.model_dump(by_alias=False)
                    for resource in request.scene
                ]
            if request.parameters is not None:
                program_data["parameters"] = [
                    parameter.model_dump()
                    for parameter in request.parameters
                ]
            if request.blackboard is not None:
                program_data["blackboard"] = request.blackboard.model_dump(by_alias=True)
            Program.from_dict(
                program_data,
                state.catalog,
                path=target_path,
                skill_catalog=state.skill_catalog,
                deployment_capability_keys=_deployment_capability_keys(state),
            ).save(target_path)
        except HTTPException:
            raise
        except (ProgramValidationError, DeploymentValidationError) as exc:
            logger.warning("Rejected program save: %s", exc)
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.error("Failed to save program: %s", e)
            raise HTTPException(status_code=500, detail="Failed to save program")
        return {"status": "ok", "path": target_path}

    @router.put("/deployment")
    def update_deployment(request: UpdateDeploymentRequest) -> dict:
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply deployment changes while execution is active")
        try:
            deployment_dict = state.deployment.to_dict()
            patch_dict = request.model_dump(exclude_unset=True)
            for key, value in patch_dict.items():
                deployment_dict[key] = value
            updated_deployment = Deployment.from_dict(
                deployment_dict,
                path=state.deployment_source_path or "<memory>",
            )
            _validate_deployment_against_catalogs(state, updated_deployment)
            if state.program is not None:
                updated_deployment.validate_against_program(state.program)
                updated_deployment.validate_capabilities(state.program)
                state.apply_models(
                    state.program,
                    updated_deployment,
                )
            else:
                state.deployment = updated_deployment
        except (ProgramValidationError, DeploymentValidationError) as exc:
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.warning("Failed to apply deployment update: %s", e)
            raise HTTPException(status_code=400, detail="Failed to apply deployment update")
        return {"status": "ok"}

    @router.post("/deployment/save")
    def save_deployment(request: SaveDeploymentRequest) -> dict:
        if state.deployment is None:
            raise HTTPException(status_code=404, detail="No deployment loaded")
        try:
            target_path = _coerce_artifact_save_path(
                request.path,
                allowed_roots=state.allowed_fs_roots,
                suffixes=_DEPLOYMENT_SUFFIXES,
                default_suffix=".deployment.yaml",
                kind_label="Deployment path",
            )
            if request.deployment is None:
                _validate_deployment_against_catalogs(state, state.deployment)
                state.deployment.save(target_path)
            else:
                deployment_data = request.deployment.model_dump()
                candidate = Deployment.from_dict(deployment_data, path=target_path)
                _validate_deployment_against_catalogs(state, candidate)
                if state.program is not None:
                    candidate.validate_against_program(state.program)
                    candidate.validate_capabilities(state.program)
                candidate.save(target_path)
        except HTTPException:
            raise
        except (ProgramValidationError, DeploymentValidationError) as exc:
            logger.warning("Rejected deployment save: %s", exc)
            raise HTTPException(status_code=400, detail=str(exc))
        except Exception as e:
            logger.error("Failed to save deployment: %s", e)
            raise HTTPException(status_code=500, detail="Failed to save deployment")
        return {"status": "ok", "path": target_path}

    @router.post("/session/apply", response_model=ApplySessionResponse)
    def apply_session(request: ApplySessionRequest) -> dict:
        if state.program is None:
            raise HTTPException(status_code=404, detail="No program loaded")
        if state.cycle_manager is not None and state.cycle_manager.state.value == "running":
            raise HTTPException(status_code=409, detail="Cannot apply changes while execution is active")

        try:
            program = state.program
            if (
                request.tree is not None
                or request.scene is not None
                or request.program_parameters is not None
                or request.blackboard is not None
            ):
                program_data = state.program.to_dict()
                if request.tree is not None:
                    program_data["tree"] = request.tree.model_dump()
                if request.scene is not None:
                    program_data["scene"] = [
                        resource.model_dump(by_alias=False)
                        for resource in request.scene
                    ]
                if request.program_parameters is not None:
                    program_data["parameters"] = [
                        parameter.model_dump()
                        for parameter in request.program_parameters
                    ]
                if request.blackboard is not None:
                    program_data["blackboard"] = request.blackboard.model_dump(by_alias=True)
                program = Program.from_dict(
                    program_data,
                    state.catalog,
                    path=state.program_source_path or "<memory>",
                    skill_catalog=state.skill_catalog,
                    deployment_capability_keys=_deployment_capability_keys(state),
                )

            deployment = state.deployment
            if request.deployment is not None:
                deployment = Deployment.from_dict(
                    request.deployment.model_dump(),
                    path=state.deployment_source_path or "<memory>",
                )
                _validate_deployment_against_catalogs(state, deployment)

            if deployment is not None:
                deployment.validate_against_program(program)
                deployment.validate_capabilities(program)

            state.apply_models(
                program,
                deployment,
                force_discard_overrides=request.force_discard_overrides,
            )

            # Spec/430 §3.2 cross-validation that requires the URDF to
            # be loaded — runs AFTER apply_models so we have a kernel
            # with a populated joint set. If validation fails we let
            # the DeploymentValidationError propagate to the 400
            # handler below. The state has already accepted the
            # deployment at this point; the operator's next load
            # attempt with a corrected deployment supersedes.
            if deployment is not None and state.kernel is not None:
                try:
                    drivable_joints = state.kernel.get_joint_names()
                except Exception:
                    drivable_joints = []
                if drivable_joints:
                    deployment.validate_streaming_joint_limits_against(drivable_joints)
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        except (ProgramValidationError, DeploymentValidationError) as exc:
            raise HTTPException(status_code=400, detail=f"Failed to apply session: {exc}")
        except Exception as e:
            logger.warning("Failed to apply session: %s", e)
            raise HTTPException(status_code=400, detail=f"Failed to apply session: {e}")

        return {"status": "ok"}

    return router
