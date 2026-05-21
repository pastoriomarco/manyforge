"""Pydantic request/response models for ManyForge Composer API."""
from __future__ import annotations

from typing import Any, Optional, List, Dict, Literal
from pydantic import BaseModel, ConfigDict, Field, model_validator

_ALIAS_CFG = ConfigDict(populate_by_name=True)
_ALIAS_EXTRA_CFG = ConfigDict(populate_by_name=True, extra="allow")


# --- Cycle ---
class CycleStatusResponse(BaseModel):
    model_config = _ALIAS_CFG
    state: str
    velocity_scale: float = Field(alias="velocityScale")


class VelocityScaleRequest(BaseModel):
    value: float = Field(ge=0.0, le=1.0)


class VelocityScaleResponse(BaseModel):
    model_config = _ALIAS_CFG
    velocity_scale: float = Field(alias="velocityScale")


class CycleCommandResponse(BaseModel):
    state: str


# --- Catalog ---
class CatalogEnumOptionResponse(BaseModel):
    model_config = _ALIAS_CFG
    value: str
    label: Optional[str] = None
    description: str = ""
    profiles_min: str = Field(default="v0.0", alias="profilesMin")
    required_capabilities: List[str] = Field(default_factory=list, alias="requiredCapabilities")


class CatalogVisibleWhenResponse(BaseModel):
    param: str
    equals: Any


class CatalogParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    required: bool
    default: Any = None
    enum_values: List[str] = Field(default_factory=list, alias="enumValues")
    enum_options: List[CatalogEnumOptionResponse] = Field(
        default_factory=list,
        alias="enumOptions",
    )
    constraints: Dict[str, Any] = Field(default_factory=dict)
    parameterizable: bool = False
    profiles_min: str = Field(default="v0.0", alias="profilesMin")
    visible_when: Optional[CatalogVisibleWhenResponse] = Field(
        default=None,
        alias="visibleWhen",
    )
    ui_editor: Optional[str] = Field(default=None, alias="uiEditor")
    blackboard_role: Optional[str] = Field(default=None, alias="blackboardRole")
    blackboard_contract: Optional[str] = Field(default=None, alias="blackboardContract")
    blackboard_value_type: Optional[str] = Field(default=None, alias="blackboardValueType")
    description: str = ""


class CatalogEntryResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    name: str
    category: str
    description: str
    stability: str = "stable"
    kind: str
    children_min: int = Field(alias="childrenMin")
    children_max: int = Field(alias="childrenMax")
    parameters: List[CatalogParameterResponse] = Field(default_factory=list)
    bb_reader_params: List[str] = Field(default_factory=list, alias="bbReaderParams")
    bb_writer_params: List[str] = Field(default_factory=list, alias="bbWriterParams")
    required_capabilities: List[str] = Field(default_factory=list, alias="requiredCapabilities")
    profiles_min: str = Field(default="v0.0", alias="profilesMin")
    interruptible: bool = False
    default_interrupt_scope: Optional[str] = Field(
        default=None,
        alias="defaultInterruptScope",
    )


class CatalogResponse(BaseModel):
    version: str
    entries: List[CatalogEntryResponse]


# --- Program ---
class LoadProgramRequest(BaseModel):
    model_config = _ALIAS_CFG
    path: str
    deployment_path: Optional[str] = Field(None, alias="deploymentPath")
    force_discard_overrides: bool = Field(default=False, alias="forceDiscardOverrides")
    defer_initial_runtime_reset: bool = Field(default=False, alias="deferInitialRuntimeReset")


class RobotResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    base_frame: str = Field(alias="baseFrame")
    tcp_frame: str = Field(alias="tcpFrame")
    base_pose: Dict[str, Any] = Field(default_factory=dict, alias="basePose")


class TreeNodeResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    name: str
    params: Dict[str, Any] = Field(default_factory=dict)
    modification_policy: Dict[str, str] = Field(
        default_factory=dict,
        alias="modificationPolicy",
    )
    recovery_policy: Dict[str, Any] = Field(
        default_factory=dict,
        alias="recoveryPolicy",
    )
    children: List["TreeNodeResponse"] = Field(default_factory=list)


class ProgramResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot: RobotResponse
    scene_count: int = Field(alias="sceneCount")
    scene_resources: List["ProgramSceneResourceResponse"] = Field(
        default_factory=list,
        alias="sceneResources",
    )
    parameter_count: int = Field(alias="parameterCount")
    parameters: List["ProgramParameterResponse"] = Field(default_factory=list)
    blackboard: "ProgramBlackboardResponse" = Field(default_factory=lambda: ProgramBlackboardResponse())
    interrupt_handlers: List["ProgramInterruptHandlerResponse"] = Field(
        default_factory=list,
        alias="interruptHandlers",
    )
    interrupt_policies: List["ProgramInterruptPolicyResponse"] = Field(
        default_factory=list,
        alias="interruptPolicies",
    )
    source_path: str = Field(alias="sourcePath", default="")


class ProgramParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    default: Any = None
    constraints: Dict[str, Any] = Field(default_factory=dict)
    description: str = ""


class ProgramSceneResourceResponse(BaseModel):
    model_config = _ALIAS_CFG
    object_id: str = Field(alias="objectId")
    shape: Dict[str, Any] = Field(default_factory=dict)
    pose: Dict[str, Any] = Field(default_factory=dict)


class BlackboardNamespaceResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    contract: str
    prefix: str
    description: str = ""
    fields: List["BlackboardNamespaceFieldResponse"] = Field(default_factory=list)


class BlackboardNamespaceFieldResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    description: str = ""


class BlackboardKeyResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    type: str
    key: str
    description: str = ""


class ProgramBlackboardResponse(BaseModel):
    model_config = _ALIAS_CFG
    namespaces: List[BlackboardNamespaceResponse] = Field(default_factory=list)
    keys: List[BlackboardKeyResponse] = Field(default_factory=list)


class ProgramInterruptHandlerResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    description: str = ""
    root: TreeNodeResponse


class ProgramInterruptPolicyResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    source: str
    applies_to: Dict[str, Any] = Field(default_factory=dict, alias="appliesTo")
    preemption_mode: str = Field(alias="preemptionMode")
    handler: str
    after_handler: str = Field(alias="afterHandler")
    condition: Dict[str, Any] = Field(default_factory=dict)
    description: str = ""


class ProgramTreeResponse(BaseModel):
    tree: TreeNodeResponse


class SaveProgramRequest(BaseModel):
    path: str
    tree: Optional[TreeNodeResponse] = None
    scene: Optional[List[ProgramSceneResourceResponse]] = None
    parameters: Optional[List[ProgramParameterResponse]] = None
    blackboard: Optional[ProgramBlackboardResponse] = None


class UpdateTreeRequest(BaseModel):
    model_config = _ALIAS_CFG
    tree: TreeNodeResponse
    force_discard_overrides: bool = Field(default=False, alias="forceDiscardOverrides")


class ProgramValidationIssueResponse(BaseModel):
    path: str
    message: str


class ProgramValidateRequest(BaseModel):
    tree: TreeNodeResponse
    parameters: Optional[List[ProgramParameterResponse]] = None
    blackboard: Optional[ProgramBlackboardResponse] = None


class ProgramValidateResponse(BaseModel):
    valid: bool
    errors: List[ProgramValidationIssueResponse] = Field(default_factory=list)
    warnings: List[ProgramValidationIssueResponse] = Field(default_factory=list)


class RuntimeOverrideRequest(BaseModel):
    model_config = _ALIAS_CFG
    node_name: str = Field(alias="nodeName")
    params: Dict[str, Any]
    authority_source: Literal["human", "llm"] = Field(alias="authoritySource")
    approval_mode: Literal["human_approved", "not_required"] = Field(alias="approvalMode")
    reason: str = ""
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")


class RuntimeOverrideEntryResponse(BaseModel):
    model_config = _ALIAS_CFG
    node_name: str = Field(alias="nodeName")
    params: Dict[str, Any] = Field(default_factory=dict)
    authority_source: str = Field(alias="authoritySource")
    approval_mode: str = Field(alias="approvalMode")
    reason: str = ""
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")
    base_program_hash: str = Field(default="", alias="baseProgramHash")
    requested_at_ns: int = Field(alias="requestedAtNs")
    activation_state: str = Field(alias="activationState")
    activated_at_ns: Optional[int] = Field(default=None, alias="activatedAtNs")
    is_stale: bool = Field(default=False, alias="isStale")


class RuntimeOverrideStatusResponse(BaseModel):
    model_config = _ALIAS_CFG
    active: List[RuntimeOverrideEntryResponse] = Field(default_factory=list)
    staged: List[RuntimeOverrideEntryResponse] = Field(default_factory=list)
    stale: List[RuntimeOverrideEntryResponse] = Field(default_factory=list)
    active_count: int = Field(alias="activeCount")
    staged_count: int = Field(alias="stagedCount")
    stale_count: int = Field(default=0, alias="staleCount")
    has_active: bool = Field(alias="hasActive")
    has_staged: bool = Field(alias="hasStaged")
    has_stale: bool = Field(default=False, alias="hasStale")
    can_activate_now: bool = Field(alias="canActivateNow")
    requires_reset_to_activate: bool = Field(alias="requiresResetToActivate")
    current_tree_origin: str = Field(alias="currentTreeOrigin")
    base_program_hash: str = Field(default="", alias="baseProgramHash")
    next_effective_tree: Optional[TreeNodeResponse] = Field(default=None, alias="nextEffectiveTree")
    revalidation_results: Optional[Dict[str, str]] = Field(
        default=None, alias="revalidation_results",
    )
    timestamp_ns: int = Field(alias="timestampNs")


class RuntimeOverridePromoteRequest(BaseModel):
    model_config = _ALIAS_CFG
    target: Literal["loaded_program", "save"]
    include_staged: bool = Field(default=False, alias="includeStaged")
    path: Optional[str] = None
    authority_source: Optional[Literal["human", "llm"]] = Field(
        default=None,
        alias="authoritySource",
    )
    approval_mode: Optional[Literal["human_approved", "not_required"]] = Field(
        default=None,
        alias="approvalMode",
    )


class RuntimeOverridePromoteResponse(BaseModel):
    model_config = _ALIAS_CFG
    status: str
    target: str
    include_staged: bool = Field(alias="includeStaged")
    path: Optional[str] = None
    tree: TreeNodeResponse
    runtime_override_status: RuntimeOverrideStatusResponse = Field(alias="runtimeOverrideStatus")


class CapabilitiesResponse(BaseModel):
    model_config = _ALIAS_CFG
    planning: List[str] = Field(default_factory=list)
    context: List[str] = Field(default_factory=list)
    scene: List[str] = Field(default_factory=list)
    execution: List[str] = Field(default_factory=list)
    sensing: List[str] = Field(default_factory=list)
    other: List[str] = Field(default_factory=list)
    adapter_ids: Dict[str, str] = Field(default_factory=dict, alias="adapterIds")
    program_profile: Optional[str] = Field(default=None, alias="programProfile")
    contact_tier: str = Field(alias="contactTier")
    timestamp_ns: int = Field(alias="timestampNs")


# --- Skills ---
class SkillParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    required: bool
    default: Any = None
    description: str = ""


class SkillResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    version: str
    name: str
    description: str
    compatible_profiles: List[str] = Field(alias="compatibleProfiles")
    requires: List[str]
    provides: List[str]
    modifies: List[str]
    requires_capabilities: List[str] = Field(alias="requiresCapabilities")
    requires_tools: List[str] = Field(default_factory=list, alias="requiresTools")
    parameters: List[SkillParameterResponse]
    body: Dict[str, Any]
    expands_to: Dict[str, Any] = Field(alias="expandsTo")
    example: Dict[str, Any]


class SkillCatalogResponse(BaseModel):
    model_config = _ALIAS_CFG
    skills: List[SkillResponse]
    timestamp_ns: int = Field(alias="timestampNs")


# --- Direct Control / Intervention ---
class InterventionExecuteRequest(BaseModel):
    model_config = _ALIAS_CFG
    tree: "TreeNodeResponse"
    lease_timeout_ms: int = Field(default=60_000, alias="leaseTimeoutMs", gt=0)
    resume_target: Optional[str] = Field(default=None, alias="resumeTarget")
    session_kind: Optional[
        Literal["manual_intervention", "delegated_recovery"]
    ] = Field(default=None, alias="sessionKind")
    authority_source: Optional[
        Literal["human", "llm", "program_node", "policy"]
    ] = Field(default=None, alias="authoritySource")
    principal_id: str = Field(alias="principalId", min_length=1)
    grant_type: Optional[
        Literal["human_approved", "policy_granted"]
    ] = Field(default=None, alias="grantType")
    trigger_reason: Optional[str] = Field(default=None, alias="triggerReason")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")
    approval_mode: Optional[
        Literal["human_approved", "policy_granted", "not_required"]
    ] = Field(default=None, alias="approvalMode")
    artifact_origin: Literal["saved_program", "runtime_override"] = Field(
        default="saved_program",
        alias="artifactOrigin",
    )


class JointJogRequest(BaseModel):
    """Operator-driven single-joint jog from the robot-control overlay.

    Day 5b of the native-viewer migration: the panel's slider commits
    POST one of these. The backend reads the current full joint state
    from the kernel, applies the requested partial update, and submits
    a one-leaf intervention tree (single `move_manipulator_action` with
    `motion_type=joint_goal`) through the existing intervention
    machinery. No new kernel API is needed because the kernel already
    drives joints exclusively through behavior-tree execution.
    """

    model_config = _ALIAS_CFG
    joint_names: List[str] = Field(alias="jointNames", min_length=1)
    joint_positions_rad: List[float] = Field(alias="jointPositionsRad", min_length=1)
    # Optional lease + auth fields. Defaults are sensible for an
    # operator dragging a slider with no preset principal.
    lease_timeout_ms: int = Field(default=10_000, alias="leaseTimeoutMs", gt=0)
    principal_id: str = Field(default="composer-viewer", alias="principalId", min_length=1)


class CartesianJogRequest(BaseModel):
    """Operator-driven Cartesian end-effector jog from the robot-control
    overlay's Cartesian tab (Day 5c.2).

    The viewer side sends a target TCP pose expressed in the robot's
    base frame (the same frame the catalog's `pose_goal` parameter
    speaks). For now, the frontend converts world-frame gizmo poses
    to base frame using the kernel's known base pose before POSTing
    — keeps the backend small. The backend builds a one-leaf
    intervention tree with `move_manipulator_action(motion_type=
    pose_goal, pose_goal=<pose>)` and submits via the existing
    cycle_manager.execute_intervention machinery.
    """

    model_config = _ALIAS_CFG
    position_m: List[float] = Field(alias="positionM", min_length=3, max_length=3)
    orientation_xyzw: List[float] = Field(
        alias="orientationXyzw", min_length=4, max_length=4
    )
    lease_timeout_ms: int = Field(default=10_000, alias="leaseTimeoutMs", gt=0)
    principal_id: str = Field(default="composer-viewer", alias="principalId", min_length=1)


class InterventionPromoteRequest(BaseModel):
    model_config = _ALIAS_CFG
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    promote_keys: Dict[str, str] = Field(alias="promoteKeys")


class InterventionSessionResponse(BaseModel):
    model_config = _ALIAS_CFG
    session_id: str = Field(alias="sessionId")
    session_kind: str = Field(alias="sessionKind")
    authority_source: str = Field(alias="authoritySource")
    principal_id: str = Field(alias="principalId")
    grant_type: str = Field(alias="grantType")
    parent_run_id: Optional[str] = Field(default=None, alias="parentRunId")
    entry_state: str = Field(alias="entryState")
    lease_timeout_ms: int = Field(alias="leaseTimeoutMs")
    lease_expires_at_ns: int = Field(alias="leaseExpiresAtNs")
    resume_target: str = Field(alias="resumeTarget")
    trigger_reason: str = Field(alias="triggerReason")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")
    approval_mode: str = Field(alias="approvalMode")
    artifact_origin: str = Field(alias="artifactOrigin")
    status: str
    actual_outcome: Optional[str] = Field(default=None, alias="actualOutcome")
    error: Optional[str] = None
    started_at_ns: int = Field(alias="startedAtNs")
    finished_at_ns: Optional[int] = Field(default=None, alias="finishedAtNs")
    events: List[Dict[str, Any]] = Field(default_factory=list)
    promotions: List[Dict[str, Any]] = Field(default_factory=list)
    timestamp_ns: int = Field(alias="timestampNs")


class DelegatedRecoveryRequestResponse(BaseModel):
    model_config = _ALIAS_CFG
    request_id: str = Field(alias="requestId")
    parent_run_id: Optional[str] = Field(default=None, alias="parentRunId")
    requested_from_state: str = Field(alias="requestedFromState")
    trigger_node_instance: str = Field(alias="triggerNodeInstance")
    trigger_node_id: str = Field(alias="triggerNodeId")
    trigger_reason: str = Field(alias="triggerReason")
    delegate_policy: str = Field(alias="delegatePolicy")
    allowed_authority_sources: List[str] = Field(alias="allowedAuthoritySources")
    allowed_actions: List[str] = Field(alias="allowedActions")
    allowed_resume_targets: List[str] = Field(alias="allowedResumeTargets")
    default_resume_target: str = Field(alias="defaultResumeTarget")
    runtime_override_policy: str = Field(alias="runtimeOverridePolicy")
    persistent_edit_policy: str = Field(alias="persistentEditPolicy")
    requested_at_ns: int = Field(alias="requestedAtNs")
    timestamp_ns: int = Field(alias="timestampNs")


# --- Deployment ---
class LoadDeploymentRequest(BaseModel):
    path: str


class StreamingJointLimitResponse(BaseModel):
    """Per-joint streaming caps (ADR-0016, spec/430 §3.2)."""

    model_config = _ALIAS_CFG
    max_velocity_rad_s: Optional[float] = Field(default=None, alias="maxVelocityRadS")
    max_acceleration_rad_s2: Optional[float] = Field(
        default=None, alias="maxAccelerationRadS2"
    )


class RobotConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    urdf_path: str = Field(alias="urdfPath")
    srdf_path: str = Field(default="", alias="srdfPath")
    package_paths: List[str] = Field(default_factory=list, alias="packagePaths")
    global_max_tcp_linear_speed_mps: float = Field(alias="globalMaxTcpLinearSpeedMps")
    # Per-joint streaming caps keyed by joint name. Empty mapping = all
    # joints use engine defaults (0.5 rad/s, 2.0 rad/s²).
    streaming_joint_limits: Dict[str, StreamingJointLimitResponse] = Field(
        default_factory=dict, alias="streamingJointLimits"
    )


class GripperSubtreeLinkResponse(BaseModel):
    """One link in the gripper subtree, posed relative to
    `gripperRootLink` at the URDF default configuration. Operator
    sees the WHOLE gripper at the unreachable target, not just the
    root cube — the fingers and knuckles are what make the shape
    recognizable as a gripper."""

    model_config = _ALIAS_CFG
    name: str
    position_m: List[float] = Field(alias="positionM")
    orientation_xyzw: List[float] = Field(alias="orientationXyzw")


class GripperVisualizationResponse(BaseModel):
    """Auto-discovered visualization metadata for the Move-To
    unreachable-ghost fallback. None means the URDF graph walk from
    `mount_frame` did not reach a link with visuals; the frontend
    renders the floor-fallback red-axes marker instead. See
    `docs/operations/ADDING_A_ROBOT.md` and `routes_urdf.py
    resolve_gripper_visualization`."""

    model_config = _ALIAS_CFG
    gripper_root_link: str = Field(alias="gripperRootLink")
    tcp_to_root_position_m: List[float] = Field(alias="tcpToRootPositionM")
    tcp_to_root_orientation_xyzw: List[float] = Field(alias="tcpToRootOrientationXyzw")
    subtree_links: List[GripperSubtreeLinkResponse] = Field(
        default_factory=list, alias="subtreeLinks"
    )


class GripperConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    command_mode: str = Field(alias="commandMode")
    group_id: str = Field(alias="groupId")
    joint_name: str = Field(alias="jointName")
    motion_time_s: float = Field(alias="motionTimeS")
    srdf_presets: Dict[str, float] = Field(default_factory=dict, alias="srdfPresets")
    description: str = ""
    touch_links: List[str] = Field(default_factory=list, alias="touchLinks")
    mount_frame: str = Field(default="", alias="mountFrame")
    attach_frame: str = Field(default="", alias="attachFrame")
    tcp_frame: str = Field(default="", alias="tcpFrame")
    auxiliary_frames: List[str] = Field(default_factory=list, alias="auxiliaryFrames")
    visualization: Optional[GripperVisualizationResponse] = Field(
        default=None, alias="visualization"
    )


class DeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")
    grippers: Dict[str, GripperConfigResponse] = Field(default_factory=dict)


class AdapterConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    adapter_id: str = Field(alias="adapterId")
    adapter_version: str = Field(alias="adapterVersion")
    config: Dict[str, Any] = Field(default_factory=dict)
    supported_profiles: List[str] = Field(default_factory=list, alias="supportedProfiles")
    capability_keys: List[str] = Field(default_factory=list, alias="capabilityKeys")


class OrchestratorConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    engine: str
    config: Dict[str, Any] = Field(default_factory=dict)


class LaunchRunnerResponse(BaseModel):
    model_config = _ALIAS_CFG
    module: str
    args: List[str] = Field(default_factory=list)


class LaunchUiBackendResponse(BaseModel):
    model_config = _ALIAS_CFG
    module: str = ""
    args: List[str] = Field(default_factory=list)
    enabled: bool = True


class LaunchConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    runner: LaunchRunnerResponse
    ui_backend: Optional[LaunchUiBackendResponse] = Field(None, alias="uiBackend")


class ModelAuditPolicyResponse(BaseModel):
    model_config = _ALIAS_CFG
    store_full_prompts: bool = Field(default=False, alias="storeFullPrompts")
    store_full_responses: bool = Field(default=False, alias="storeFullResponses")
    hash_algorithm: str = Field(default="sha256", alias="hashAlgorithm")
    redact_argument_values: bool = Field(default=True, alias="redactArgumentValues")


class ModelEndpointCapabilitiesResponse(BaseModel):
    model_config = _ALIAS_CFG
    supports_response_tools: bool = Field(default=False, alias="supportsResponseTools")
    supports_json_object: bool = Field(default=False, alias="supportsJsonObject")
    supports_image_input: bool = Field(default=False, alias="supportsImageInput")
    supported_image_transports: List[str] = Field(
        default_factory=list,
        alias="supportedImageTransports",
    )
    tool_call_family: str = Field(default="", alias="toolCallFamily")
    latency_budget_ms: Optional[int] = Field(default=None, alias="latencyBudgetMs")


class ModelEndpointResponse(BaseModel):
    model_config = _ALIAS_CFG
    profile_ref: str = Field(default="", alias="profileRef")
    provider: str
    model_id: str = Field(alias="modelId")
    base_url: Optional[str] = Field(default=None, alias="baseUrl")
    image_fetch_base_url: Optional[str] = Field(default=None, alias="imageFetchBaseUrl")
    api_key_env: Optional[str] = Field(default=None, alias="apiKeyEnv")
    default_timeout_s: float = Field(default=30.0, alias="defaultTimeoutS")
    max_tokens: int = Field(default=4096, alias="maxTokens")
    capabilities: ModelEndpointCapabilitiesResponse = Field(
        default_factory=ModelEndpointCapabilitiesResponse,
    )


class AssistantConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    provider: str
    endpoint_alias: str = Field(default="", alias="endpointAlias")
    enabled: bool = True


class AssistantToolDefinitionResponse(BaseModel):
    model_config = _ALIAS_EXTRA_CFG
    id: str
    effect: Literal[
        "read_only",
        "proposal",
        "composer_draft_mutating",
        "runtime_mutating",
    ]
    description: str = ""


class AssistantModeAuthorityResponse(BaseModel):
    model_config = _ALIAS_CFG
    session_kind: Literal[
        "manual_intervention",
        "delegated_recovery",
        "not_applicable",
    ] = Field(alias="sessionKind")
    approval_mode: Literal[
        "human_approved",
        "policy_granted",
    ] = Field(alias="approvalMode")
    review: Literal[
        "required",
        "bounded_by_recovery_profile",
        "not_required",
    ]


class AssistantModeCatalogResponse(BaseModel):
    model_config = _ALIAS_CFG
    skills: List[str] = Field(default_factory=list)
    tools: List[str] = Field(default_factory=list)
    nodes: List[str] = Field(default_factory=list)


class AssistantModeResponse(BaseModel):
    model_config = _ALIAS_CFG
    enabled: bool = True
    description: str
    authority: AssistantModeAuthorityResponse
    catalog: AssistantModeCatalogResponse


class PromptTemplateResponse(BaseModel):
    model_config = _ALIAS_CFG
    version: str
    template_text: str = Field(alias="templateText")
    output_schema: Optional[Dict[str, Any]] = Field(default=None, alias="outputSchema")
    response_tool: Optional[Dict[str, Any]] = Field(default=None, alias="responseTool")


class InterruptSourceResponse(BaseModel):
    model_config = _ALIAS_CFG
    adapter: str
    signal: str
    kind: Literal["edge", "level"]
    priority: int
    maskable: bool
    description: str = ""


class AuthorityDomainResponse(BaseModel):
    model_config = _ALIAS_CFG
    owner: str
    on_disconnect: str = Field(alias="onDisconnect")
    description: str = ""


class FullDeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    api_version: str = Field(alias="apiVersion")
    schema_version: str = Field(alias="schemaVersion")
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")
    grippers: Dict[str, GripperConfigResponse] = Field(default_factory=dict)
    adapters: Dict[str, AdapterConfigResponse]
    orchestrator: OrchestratorConfigResponse
    launch: LaunchConfigResponse
    model_audit_policy: Optional[ModelAuditPolicyResponse] = Field(
        default=None,
        alias="modelAuditPolicy",
    )
    model_endpoint_profile_registry: Optional[str] = Field(
        default=None,
        alias="modelEndpointProfileRegistry",
    )
    model_endpoints: Dict[str, ModelEndpointResponse] = Field(
        default_factory=dict,
        alias="modelEndpoints",
    )
    assistant: Optional[AssistantConfigResponse] = None
    assistant_modes: Dict[str, AssistantModeResponse] = Field(
        default_factory=dict,
        alias="assistantModes",
    )
    tool_catalog: List[AssistantToolDefinitionResponse] = Field(
        default_factory=list,
        alias="toolCatalog",
    )
    prompt_templates: Dict[str, PromptTemplateResponse] = Field(
        default_factory=dict,
        alias="promptTemplates",
    )
    interrupt_sources: Dict[str, InterruptSourceResponse] = Field(
        default_factory=dict,
        alias="interruptSources",
    )
    authority_matrix: Dict[str, AuthorityDomainResponse] = Field(
        default_factory=dict,
        alias="authorityMatrix",
    )
    source_path: str = Field(alias="sourcePath", default="")


class ModelEndpointProbeResponse(BaseModel):
    model_config = _ALIAS_CFG
    alias: str
    provider: str
    configured_model_id: str = Field(alias="configuredModelId")
    base_url: Optional[str] = Field(default=None, alias="baseUrl")
    capabilities: ModelEndpointCapabilitiesResponse = Field(
        default_factory=ModelEndpointCapabilitiesResponse,
    )
    probe_status: str = Field(alias="probeStatus")
    available_model_ids: List[str] = Field(default_factory=list, alias="availableModelIds")
    error: Optional[str] = None


class ModelEndpointCatalogResponse(BaseModel):
    model_config = _ALIAS_CFG
    endpoints: Dict[str, ModelEndpointProbeResponse]
    timestamp: float


class UpdateDeploymentRequest(BaseModel):
    model_config = _ALIAS_CFG
    name: Optional[str] = None
    description: Optional[str] = None
    program_profile: Optional[str] = Field(None, alias="programProfile")
    robot_config: Optional[Dict[str, Any]] = Field(None, alias="robotConfig")
    grippers: Optional[Dict[str, Dict[str, Any]]] = None
    adapters: Optional[Dict[str, Dict[str, Any]]] = None
    orchestrator: Optional[Dict[str, Any]] = None
    launch: Optional[Dict[str, Any]] = None
    model_endpoints: Optional[Dict[str, Dict[str, Any]]] = Field(
        None,
        alias="modelEndpoints",
    )
    assistant: Optional[Dict[str, Any]] = None
    assistant_modes: Optional[Dict[str, Dict[str, Any]]] = Field(
        None,
        alias="assistantModes",
    )
    tool_catalog: Optional[List[Dict[str, Any]]] = Field(
        None,
        alias="toolCatalog",
    )
    authority_matrix: Optional[Dict[str, Dict[str, Any]]] = Field(
        None,
        alias="authorityMatrix",
    )


class SaveDeploymentRequest(BaseModel):
    path: str
    deployment: Optional[FullDeploymentResponse] = None


class ApplySessionRequest(BaseModel):
    model_config = _ALIAS_CFG
    tree: Optional[TreeNodeResponse] = None
    scene: Optional[List[ProgramSceneResourceResponse]] = None
    program_parameters: Optional[List[ProgramParameterResponse]] = Field(
        default=None,
        alias="programParameters",
    )
    blackboard: Optional[ProgramBlackboardResponse] = None
    deployment: Optional[FullDeploymentResponse] = None
    force_discard_overrides: bool = Field(default=False, alias="forceDiscardOverrides")


class ApplySessionResponse(BaseModel):
    status: str


# --- Kernel / Scene observability ---
class KernelSnapshotResponse(BaseModel):
    state: str
    execution_id: int
    active_move_id: Optional[str] = None
    progress: float
    joint_positions_rad: Optional[List[float]] = None
    error: Optional[str] = None
    last_transition_ns: Optional[int] = None
    timestamp_ns: int


class SceneShapeResponse(BaseModel):
    type: str
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    cylinder_radius_m: Optional[float] = None
    cylinder_height_m: Optional[float] = None
    mesh_resource_uri: Optional[str] = None
    mesh_scale: Optional[List[float]] = None


class SceneObjectResponse(BaseModel):
    object_id: str
    shape: Optional[SceneShapeResponse] = None
    pose_in_universe: Optional[Dict[str, Any]] = None
    attached: bool
    attached_to: Optional[str] = None
    source: str = "runtime"
    persistable: bool = False
    viewer_frame_visible: bool = False


class SceneRobotResponse(BaseModel):
    robot_id: Optional[str] = None
    group_id: Optional[str] = None
    urdf_model_key: Optional[str] = None
    base_frame: str
    tcp_frame: str
    base_pose: Dict[str, Any] = Field(default_factory=dict)
    joint_names: List[str] = Field(default_factory=list)
    joint_positions: Dict[str, float] = Field(default_factory=dict)
    tcp_pose_in_base: Optional[Dict[str, Any]] = None


class SceneGripperSubtreeLink(BaseModel):
    """Snake_case mirror of GripperSubtreeLinkResponse for the
    scene-state surface."""

    name: str
    position_m: List[float]
    orientation_xyzw: List[float]


class SceneGripperVisualizationResponse(BaseModel):
    """Auto-discovered visualization root + tcp→root SE(3) + full
    gripper subtree for the Move-To unreachable-ghost. Surfaced on
    the scene-state gripper block so the frontend ghost renderer has
    it without an extra deployment fetch. None means the URDF walk
    did not reach a link with visuals; the frontend renders a
    red-axes marker instead."""

    gripper_root_link: str
    tcp_to_root_position_m: List[float]
    tcp_to_root_orientation_xyzw: List[float]
    subtree_links: List[SceneGripperSubtreeLink] = Field(default_factory=list)


class SceneGripperStateResponse(BaseModel):
    gripper_id: str
    joint_name: str
    state: str
    current_position: Optional[float] = None
    target_position: Optional[float] = None
    limit_lower: Optional[float] = None
    limit_upper: Optional[float] = None
    motion_time_s: float = 0.0
    srdf_presets: Dict[str, float] = Field(default_factory=dict)
    visualization: Optional[SceneGripperVisualizationResponse] = None
    error: Optional[str] = None


class SceneStateResponse(BaseModel):
    robot: Optional[SceneRobotResponse] = None
    objects: List[SceneObjectResponse] = Field(default_factory=list)
    link_poses: Dict[str, Dict[str, Any]] = Field(default_factory=dict)
    grippers: List[SceneGripperStateResponse] = Field(default_factory=list)
    object_axes_gizmo_size_m: float = 0.1
    timestamp_ns: int


class SceneFrameResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    roles: List[str] = Field(default_factory=list)
    viewer_visible: bool = Field(alias="viewerVisible")
    pose_available: bool = Field(alias="poseAvailable")
    # Day-4: native viewer reads frame poses from here to render axes
    # helpers per frame. None for frames the kernel can't FK-resolve
    # (e.g. before a deployment is loaded). Keyed by `position_m` +
    # `orientation_xyzw` to match the scene-state pose convention.
    pose_in_world: Optional[Dict[str, Any]] = Field(default=None, alias="poseInWorld")


class SceneFramesResponse(BaseModel):
    model_config = _ALIAS_CFG
    frames: List[SceneFrameResponse] = Field(default_factory=list)
    hmi_available: bool = Field(alias="hmiAvailable")
    frame_gizmo_size_m: float = Field(default=0.1, alias="frameGizmoSizeM")
    timestamp_ns: int = Field(alias="timestampNs")


class SceneFrameVisibilityRequest(BaseModel):
    model_config = _ALIAS_CFG
    frame_name: str = Field(alias="frameName", min_length=1)
    visible: bool


class SceneFrameVisibilityResponse(BaseModel):
    model_config = _ALIAS_CFG
    success: bool
    frame_name: str = Field(alias="frameName")
    visible: bool
    hmi_available: bool = Field(alias="hmiAvailable")
    timestamp_ns: int = Field(alias="timestampNs")
    error: Optional[str] = None


class SceneFrameGizmoSizeRequest(BaseModel):
    model_config = _ALIAS_CFG
    axes_length_m: float = Field(alias="axesLengthM", gt=0.0, le=10.0)


class SceneFrameGizmoSizeResponse(BaseModel):
    model_config = _ALIAS_CFG
    success: bool
    axes_length_m: float = Field(alias="axesLengthM")
    hmi_available: bool = Field(alias="hmiAvailable")
    timestamp_ns: int = Field(alias="timestampNs")
    error: Optional[str] = None


class SceneGripperCommandApiRequest(BaseModel):
    model_config = _ALIAS_CFG
    gripper_id: str = Field(alias="gripperId", min_length=1)
    target_position: float = Field(alias="targetPosition")
    max_effort: Optional[float] = Field(default=None, alias="maxEffort")
    duration_s: Optional[float] = Field(default=None, alias="durationS", ge=0.0)


class SceneGripperCommandApiResponse(BaseModel):
    model_config = _ALIAS_CFG
    accepted: bool
    gripper_id: str = Field(alias="gripperId")
    target_position: float = Field(alias="targetPosition")
    error: Optional[str] = None
    timestamp_ns: int = Field(alias="timestampNs")


class SceneObjectAxesGizmoSizeRequest(BaseModel):
    model_config = _ALIAS_CFG
    axes_length_m: float = Field(alias="axesLengthM", gt=0.0, le=10.0)


class SceneObjectAxesGizmoSizeResponse(BaseModel):
    model_config = _ALIAS_CFG
    success: bool
    axes_length_m: float = Field(alias="axesLengthM")
    timestamp_ns: int = Field(alias="timestampNs")
    error: Optional[str] = None


class SceneObjectFrameVisibilityRequest(BaseModel):
    model_config = _ALIAS_CFG
    object_id: str = Field(alias="objectId", min_length=1)
    visible: bool


class SceneObjectFrameVisibilityResponse(BaseModel):
    model_config = _ALIAS_CFG
    success: bool
    object_id: str = Field(alias="objectId")
    visible: bool
    hmi_available: bool = Field(alias="hmiAvailable")
    timestamp_ns: int = Field(alias="timestampNs")
    error: Optional[str] = None


class SceneEditApiRequest(BaseModel):
    operation: Literal["add", "upsert", "remove", "update_pose", "attach", "detach"]
    object_id: str
    shape_type: Optional[Literal["box", "sphere", "cylinder", "mesh"]] = None
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    cylinder_radius_m: Optional[float] = None
    cylinder_height_m: Optional[float] = None
    mesh_resource_uri: Optional[str] = None
    mesh_scale: Optional[List[float]] = None
    pose_position_m: Optional[List[float]] = None
    pose_orientation_xyzw: Optional[List[float]] = None
    frame_id: Optional[str] = None

    @model_validator(mode="after")
    def validate_request(self) -> "SceneEditApiRequest":
        if not self.object_id:
            raise ValueError("object_id must not be empty")

        if self.operation in {"add", "upsert"}:
            if self.shape_type is None:
                raise ValueError(f"{self.operation} requires shape_type")
            if self.shape_type == "box":
                if self.box_dimensions_m is None or len(self.box_dimensions_m) != 3:
                    raise ValueError(
                        f"{self.operation} with shape_type=box requires box_dimensions_m=[x, y, z]"
                    )
                if any(v <= 0.0 for v in self.box_dimensions_m):
                    raise ValueError("box_dimensions_m values must be > 0")
            if self.shape_type == "sphere":
                if self.sphere_radius_m is None or self.sphere_radius_m <= 0.0:
                    raise ValueError(
                        f"{self.operation} with shape_type=sphere requires sphere_radius_m > 0"
                    )
            if self.shape_type == "cylinder":
                if self.cylinder_radius_m is None or self.cylinder_radius_m <= 0.0:
                    raise ValueError(
                        f"{self.operation} with shape_type=cylinder requires cylinder_radius_m > 0"
                    )
                if self.cylinder_height_m is None or self.cylinder_height_m <= 0.0:
                    raise ValueError(
                        f"{self.operation} with shape_type=cylinder requires cylinder_height_m > 0"
                    )
            if self.shape_type == "mesh":
                if not self.mesh_resource_uri:
                    raise ValueError(
                        f"{self.operation} with shape_type=mesh requires mesh_resource_uri"
                    )
                if self.mesh_scale is not None:
                    if len(self.mesh_scale) != 3:
                        raise ValueError("mesh_scale must have exactly 3 elements")
                    if any(v <= 0.0 for v in self.mesh_scale):
                        raise ValueError("mesh_scale values must be > 0")

        if self.operation == "update_pose":
            if self.pose_position_m is None or len(self.pose_position_m) != 3:
                raise ValueError("update_pose requires pose_position_m=[x, y, z]")

        if self.pose_position_m is not None and len(self.pose_position_m) != 3:
            raise ValueError("pose_position_m must have exactly 3 elements")

        if self.pose_orientation_xyzw is not None and len(self.pose_orientation_xyzw) != 4:
            raise ValueError("pose_orientation_xyzw must have exactly 4 elements")

        if self.operation == "attach" and not self.frame_id:
            raise ValueError("attach requires frame_id")

        return self


class SceneEditResponse(BaseModel):
    success: bool
    error: Optional[str] = None
    timestamp_ns: int


class SceneDiagnosticBackendResponse(BaseModel):
    exists: bool
    attached: bool
    frame_id: Optional[str] = None
    pose_in_universe: Optional[Dict[str, Any]] = None


class SceneDiagnosticHmiResponse(BaseModel):
    rendered: bool
    scene_name: Optional[str] = None
    render_state: Optional[str] = None


class SceneDiagnosticObjectResponse(BaseModel):
    object_id: str
    backend: SceneDiagnosticBackendResponse
    hmi: SceneDiagnosticHmiResponse
    match: bool
    mismatch_reason: Optional[str] = None


class SceneDiagnosticsSummaryResponse(BaseModel):
    total: int
    matched: int
    mismatched: int
    hmi_available: bool = Field(alias="hmiAvailable")


class SceneDiagnosticsResponse(BaseModel):
    objects: List[SceneDiagnosticObjectResponse] = Field(default_factory=list)
    summary: SceneDiagnosticsSummaryResponse
    timestamp_ns: int


class ExecutionEventResponse(BaseModel):
    event_index: int = 0
    sequence: int = 0
    timestamp_ns: int
    timestamp: Optional[str] = None
    timestamp_wall: Optional[str] = None
    schema_version: Optional[str] = None
    trace_source: Optional[str] = None
    severity: Optional[str] = None
    component: Optional[str] = None
    message: Optional[str] = None
    event: Optional[str] = None
    run_id: Optional[str] = None
    program_run_id: Optional[str] = None
    intervention_run_id: Optional[str] = None
    session_id: Optional[str] = None
    session_kind: Optional[str] = None
    authority_source: Optional[str] = None
    principal_id: Optional[str] = None
    grant_type: Optional[str] = None
    parent_run_id: Optional[str] = None
    trigger_reason: Optional[str] = None
    trigger_node_instance: Optional[str] = None
    execution_mode: Optional[str] = None
    execution_id: int = 0
    type: str
    move_id: Optional[str] = None
    details: Dict[str, Any] = Field(default_factory=dict)


class NodeStatusTransitionResponse(BaseModel):
    model_config = _ALIAS_CFG
    timestamp_ns: int
    run_id: Optional[str] = None
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    session_kind: Optional[str] = Field(default=None, alias="sessionKind")
    authority_source: Optional[str] = Field(default=None, alias="authoritySource")
    principal_id: Optional[str] = Field(default=None, alias="principalId")
    grant_type: Optional[str] = Field(default=None, alias="grantType")
    parent_run_id: Optional[str] = Field(default=None, alias="parentRunId")
    trigger_reason: Optional[str] = Field(default=None, alias="triggerReason")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")
    execution_mode: Optional[str] = Field(default=None, alias="executionMode")
    execution_id: int = 0
    node: str
    from_status: Optional[str] = Field(default=None, alias="from")
    to_status: str = Field(alias="to")


class ExecutionHistoryResponse(BaseModel):
    model_config = _ALIAS_CFG
    run_id: Optional[str] = Field(default=None, alias="runId")
    execution_id: int = Field(default=0, alias="executionId")
    events: List[ExecutionEventResponse] = Field(default_factory=list)
    node_status_transitions: List[NodeStatusTransitionResponse] = Field(
        default_factory=list,
        alias="nodeStatusTransitions",
    )
    timestamp_ns: int = Field(alias="timestampNs")


class ExecutionDigestObjectResponse(BaseModel):
    id: str
    attached: bool
    position_m: Optional[List[float]] = None


class PendingInterruptResponse(BaseModel):
    model_config = _ALIAS_CFG
    interrupt_id: str = Field(alias="interruptId")
    source_id: str = Field(alias="sourceId")
    source_adapter: str = Field(alias="sourceAdapter")
    signal: str
    kind: str
    priority: int
    maskable: bool
    policy_id: Optional[str] = Field(default=None, alias="policyId")
    handler_id: Optional[str] = Field(default=None, alias="handlerId")
    preemption_mode: str = Field(alias="preemptionMode")
    after_handler: str = Field(alias="afterHandler")
    active_node_name: Optional[str] = Field(default=None, alias="activeNodeName")
    active_node_id: Optional[str] = Field(default=None, alias="activeNodeId")
    active_move_id: Optional[str] = Field(default=None, alias="activeMoveId")
    latched_at_ns: int = Field(alias="latchedAtNs")
    required_action: str = Field(alias="requiredAction")
    resume_allowed: bool = Field(alias="resumeAllowed")
    reset_required: bool = Field(alias="resetRequired")
    ack_required: bool = Field(alias="ackRequired")
    timing: Dict[str, Any] = Field(default_factory=dict)
    details: Dict[str, Any] = Field(default_factory=dict)


class ExecutionDigestResponse(BaseModel):
    model_config = _ALIAS_CFG
    state: str
    run_id: Optional[str] = Field(default=None, alias="runId")
    execution_id: int = Field(default=0, alias="executionId")
    current_node: Optional[str] = Field(default=None, alias="currentNode")
    current_node_status: Optional[str] = Field(default=None, alias="currentNodeStatus")
    active_move_id: Optional[str] = Field(default=None, alias="activeMoveId")
    progress: float
    elapsed_s: float = Field(alias="elapsedS")
    velocity_scale: float = Field(alias="velocityScale")
    completed_nodes: List[str] = Field(default_factory=list, alias="completedNodes")
    remaining_nodes: List[str] = Field(default_factory=list, alias="remainingNodes")
    object_summary: List[ExecutionDigestObjectResponse] = Field(default_factory=list, alias="objectSummary")
    error: Optional[str] = None
    warnings: List[str] = Field(default_factory=list)
    runtime_override_state: Dict[str, Any] = Field(default_factory=dict, alias="runtimeOverrideState")
    delegated_recovery_request: Optional[Dict[str, Any]] = Field(
        default=None,
        alias="delegatedRecoveryRequest",
    )
    pending_interrupt: Optional[PendingInterruptResponse] = Field(
        default=None,
        alias="pendingInterrupt",
    )
    required_operator_action: str = Field(alias="requiredOperatorAction")
    resume_allowed: bool = Field(alias="resumeAllowed")
    reset_required: bool = Field(alias="resetRequired")
    ack_required: bool = Field(alias="ackRequired")
    timestamp_ns: int = Field(alias="timestampNs")


class BlackboardValueResponse(BaseModel):
    model_config = _ALIAS_CFG
    value_json: Any = Field(default=None, alias="valueJson")
    value_repr: Optional[str] = Field(default=None, alias="valueRepr")
    type: str
    truncated: bool = False


class BlackboardSnapshotResponse(BaseModel):
    model_config = _ALIAS_CFG
    keys: Dict[str, BlackboardValueResponse] = Field(default_factory=dict)
    timestamp_ns: int = Field(alias="timestampNs")


class DebugExportProgramResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: Optional[str] = None
    source_path: str = Field(default="", alias="sourcePath")


class DebugExportDeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: Optional[str] = None
    source_path: str = Field(default="", alias="sourcePath")


class Ros2MedkitTraceRecordResponse(BaseModel):
    model_config = _ALIAS_CFG
    record_id: str = Field(alias="recordId")
    timeline_id: Optional[str] = Field(default=None, alias="timelineId")
    parent_timeline_id: Optional[str] = Field(default=None, alias="parentTimelineId")
    event_index: int = Field(alias="eventIndex")
    monotonic_ns: int = Field(alias="monotonicNs")
    wall_time: Optional[str] = Field(default=None, alias="wallTime")
    level: str
    component: str
    event_type: str = Field(alias="eventType")
    message: str
    execution_mode: Optional[str] = Field(default=None, alias="executionMode")
    execution_id: int = Field(default=0, alias="executionId")
    move_id: Optional[str] = Field(default=None, alias="moveId")
    node: Optional[str] = None
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    principal_id: Optional[str] = Field(default=None, alias="principalId")
    trigger_reason: Optional[str] = Field(default=None, alias="triggerReason")
    trigger_node_instance: Optional[str] = Field(default=None, alias="triggerNodeInstance")
    attributes: Dict[str, Any] = Field(default_factory=dict)


class Ros2MedkitTraceResponse(BaseModel):
    model_config = _ALIAS_CFG
    schema_version: str = Field(alias="schemaVersion")
    trace_source: str = Field(alias="traceSource")
    exported_at_ns: int = Field(alias="exportedAtNs")
    run_id: Optional[str] = Field(default=None, alias="runId")
    program_run_id: Optional[str] = Field(default=None, alias="programRunId")
    intervention_run_ids: List[str] = Field(default_factory=list, alias="interventionRunIds")
    parent_run_ids: List[str] = Field(default_factory=list, alias="parentRunIds")
    records: List[Ros2MedkitTraceRecordResponse] = Field(default_factory=list)


class DebugExportResponse(BaseModel):
    model_config = _ALIAS_CFG
    exported_at_ns: int = Field(alias="exportedAtNs")
    run_id: Optional[str] = Field(default=None, alias="runId")
    program: DebugExportProgramResponse
    deployment: DebugExportDeploymentResponse
    kernel_snapshot: Dict[str, Any] = Field(alias="kernelSnapshot")
    kernel_metrics: Dict[str, int] = Field(alias="kernelMetrics")
    execution_digest: Dict[str, Any] = Field(alias="executionDigest")
    execution_history: Dict[str, Any] = Field(alias="executionHistory")
    scene_state: Dict[str, Any] = Field(alias="sceneState")
    scene_diagnostics: Dict[str, Any] = Field(alias="sceneDiagnostics")
    scene_trace: Dict[str, Any] = Field(alias="sceneTrace")
    blackboard: Dict[str, Any]
    signal_snapshot: Dict[str, Any] = Field(default_factory=dict, alias="signalSnapshot")
    node_statuses: Dict[str, str] = Field(alias="nodeStatuses")
    intervention_session: Optional[Dict[str, Any]] = Field(default=None, alias="interventionSession")
    runtime_overrides: Dict[str, Any] = Field(default_factory=dict, alias="runtimeOverrides")
    model_events: List[Dict[str, Any]] = Field(default_factory=list, alias="modelEvents")
    medkit_trace: Dict[str, Any] = Field(default_factory=dict, alias="medkitTrace")


class DebugSignalPublishRequest(BaseModel):
    model_config = _ALIAS_CFG
    adapter: str
    signal: str
    value: Any
    stale_after_ms: Optional[int] = Field(default=1500, alias="staleAfterMs", ge=0)
    mirror_key: Optional[str] = Field(default=None, alias="mirrorKey")
    clear_on_stale: bool = Field(default=True, alias="clearOnStale")
    metadata: Dict[str, Any] = Field(default_factory=dict)


# --- Assistant ---
class AssistantChatRequest(BaseModel):
    model_config = _ALIAS_CFG
    message: str = Field(min_length=1)
    mode: Literal["mock", "provider"] = "mock"
    request_id: Optional[str] = Field(default=None, alias="requestId")
    provider_id: Optional[str] = Field(default=None, alias="providerId")
    conversation_id: Optional[str] = Field(default=None, alias="conversationId")
    assistant_mode: Optional[str] = Field(default=None, alias="assistantMode")
    context: Dict[str, Any] = Field(default_factory=dict)
    requested_tools: List[str] = Field(default_factory=list, alias="requestedTools")

    @model_validator(mode="after")
    def _validate_message(self) -> "AssistantChatRequest":
        if not self.message.strip():
            raise ValueError("message must not be blank")
        return self


class AssistantProposalResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    type: str
    title: str
    summary: str
    status: Literal["draft"] = "draft"
    review_required: bool = Field(default=True, alias="reviewRequired")
    payload: Dict[str, Any] = Field(default_factory=dict)
    warnings: List[str] = Field(default_factory=list)
    source: Dict[str, Any] = Field(default_factory=dict)


class AssistantToolCallResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    status: Literal["proposed", "skipped", "completed", "failed"]
    arguments: Dict[str, Any] = Field(default_factory=dict)
    result: Optional[Dict[str, Any]] = None
    error: Optional[str] = None


class AssistantErrorResponse(BaseModel):
    model_config = _ALIAS_CFG
    code: str
    detail: str


class AssistantChatResponse(BaseModel):
    model_config = _ALIAS_CFG
    request_id: str = Field(alias="requestId")
    conversation_id: str = Field(alias="conversationId")
    mode: Literal["mock", "provider"]
    provider_id: str = Field(alias="providerId")
    assistant_mode: Optional[str] = Field(default=None, alias="assistantMode")
    message: str
    proposals: List[AssistantProposalResponse] = Field(default_factory=list)
    tool_calls: List[AssistantToolCallResponse] = Field(default_factory=list, alias="toolCalls")
    warnings: List[str] = Field(default_factory=list)
    mutated: bool = False
    draft_mutated: bool = Field(default=False, alias="draftMutated")
    requires_review: bool = Field(default=True, alias="requiresReview")
    program_loaded: bool = Field(alias="programLoaded")
    cycle_state: str = Field(alias="cycleState")
    timestamp_ns: int = Field(alias="timestampNs")
    error: Optional[AssistantErrorResponse] = None


class LiveToolCallResponse(BaseModel):
    """A live tool-call entry surfaced on an in-flight assistant request.

    Populated by the bridge endpoint as each tool runs so the UI can
    render activity in real time. The audit log keeps the authoritative
    record; this is a UX feed.
    """
    model_config = _ALIAS_CFG
    tool_id: str = Field(alias="toolId")
    status: Literal["running", "completed", "failed"]
    started_at_ns: int = Field(alias="startedAtNs")
    finished_at_ns: Optional[int] = Field(default=None, alias="finishedAtNs")
    args_summary: str = Field(default="", alias="argsSummary")
    result_summary: str = Field(default="", alias="resultSummary")
    error: Optional[str] = None
    draft_mutated: bool = Field(default=False, alias="draftMutated")


class AssistantRequestStatusResponse(BaseModel):
    model_config = _ALIAS_CFG
    request_id: str = Field(alias="requestId")
    conversation_id: str = Field(alias="conversationId")
    provider_id: str = Field(alias="providerId")
    assistant_mode: Optional[str] = Field(default=None, alias="assistantMode")
    status: Literal["running", "cancel_requested", "completed", "failed", "cancelled"]
    message: str = ""
    error: Optional[AssistantErrorResponse] = None
    warnings: List[str] = Field(default_factory=list)
    draft_mutated: bool = Field(default=False, alias="draftMutated")
    tool_call_count: int = Field(default=0, alias="toolCallCount")
    recent_tool_calls: List[LiveToolCallResponse] = Field(
        default_factory=list, alias="recentToolCalls"
    )
    started_at_ns: int = Field(alias="startedAtNs")
    updated_at_ns: int = Field(alias="updatedAtNs")
    finished_at_ns: Optional[int] = Field(default=None, alias="finishedAtNs")


class AssistantCancelResponse(BaseModel):
    model_config = _ALIAS_CFG
    request_id: str = Field(alias="requestId")
    cancelled: bool = False
    status: Literal["running", "cancel_requested", "completed", "failed", "cancelled", "unknown"]
    detail: str = ""
    provider_response: Dict[str, Any] = Field(default_factory=dict, alias="providerResponse")
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantToolDescriptorResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    mutates_state: bool = Field(alias="mutatesState")
    requires_review: bool = Field(alias="requiresReview")
    input_schema: Dict[str, Any] = Field(default_factory=dict, alias="inputSchema")


class AssistantToolCatalogResponse(BaseModel):
    model_config = _ALIAS_CFG
    tools: List[AssistantToolDescriptorResponse] = Field(default_factory=list)


class AssistantBridgeToolRequest(BaseModel):
    model_config = _ALIAS_CFG
    request_id: str = Field(alias="requestId", min_length=1)
    conversation_id: Optional[str] = Field(default=None, alias="conversationId")
    assistant_mode: str = Field(alias="assistantMode", min_length=1)
    catalog_hash: str = Field(alias="catalogHash", min_length=1)
    # Sent by the OpenClaw MCP wrapper (see manyforge-mcp-bridge.py
    # `envelope = {... "principal": PRINCIPAL ...}`) so the live
    # tool-call streaming layer in routes_assistant.py can correlate
    # tool calls back to the in-flight chat. Optional: the in-process
    # direct-lane bridge does not always set it.
    principal: Optional[str] = None
    arguments: Dict[str, Any] = Field(default_factory=dict)


class AssistantBridgeToolResponse(BaseModel):
    model_config = _ALIAS_CFG
    tool_id: str = Field(alias="toolId")
    success: bool
    result: Dict[str, Any] = Field(default_factory=dict)
    error: Optional[str] = None
    mutated: bool = False
    draft_mutated: bool = Field(default=False, alias="draftMutated")
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProviderDescriptorResponse(BaseModel):
    model_config = _ALIAS_CFG
    id: str
    description: str
    configured: bool = False
    requires_live_model: bool = Field(default=False, alias="requiresLiveModel")
    requires_live_isaac_sim: bool = Field(default=False, alias="requiresLiveIsaacSim")


class AssistantProviderCatalogResponse(BaseModel):
    model_config = _ALIAS_CFG
    providers: List[AssistantProviderDescriptorResponse] = Field(default_factory=list)
    configured_provider_id: Optional[str] = Field(default=None, alias="configuredProviderId")


class AssistantInspectIsaacSceneRequest(BaseModel):
    model_config = _ALIAS_CFG
    include_subtrees: List[str] = Field(default_factory=list, alias="includeSubtrees")
    exclude_subtrees: List[str] = Field(default_factory=list, alias="excludeSubtrees")
    probe_prims: List[str] = Field(default_factory=list, alias="probePrims")
    include_hidden: bool = Field(default=False, alias="includeHidden")
    include_cooked_convex: bool = Field(default=True, alias="includeCookedConvex")
    include_cooked_samples: bool = Field(default=False, alias="includeCookedSamples")
    max_colliders: Optional[int] = Field(default=None, alias="maxColliders", ge=1)
    purpose: List[Literal["default", "render", "proxy", "guide", "all"]] = Field(
        default_factory=list,
    )
    output_profile: Literal["llm_scene_authoring", "raw_inventory"] = Field(
        default="llm_scene_authoring",
        alias="outputProfile",
    )
    mesh_export_dir: Optional[str] = Field(default=None, alias="meshExportDir")
    mesh_source_preference: Literal["auto", "original", "collider", "primitive"] = Field(
        default="auto",
        alias="meshSourcePreference",
    )
    timeout_s: float = Field(default=10.0, alias="timeoutS", gt=0.0, le=120.0)
    request_topic: Optional[str] = Field(default=None, alias="requestTopic")
    response_topic: Optional[str] = Field(default=None, alias="responseTopic")
    node_name: str = Field(default="manyforge_composer_inspect_isaac_scene", alias="nodeName")


class AssistantInspectIsaacSceneResponse(BaseModel):
    model_config = _ALIAS_CFG
    tool_name: str = Field(alias="toolName")
    success: bool
    report: Dict[str, Any]
    summary: Dict[str, Any] = Field(default_factory=dict)
    warnings: List[str] = Field(default_factory=list)
    mutated: bool = False
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProposeSceneObjectsRequest(BaseModel):
    model_config = _ALIAS_CFG
    inspection_report: Dict[str, Any] = Field(alias="inspectionReport")
    source_roots: List[str] = Field(default_factory=list, alias="sourceRoots")
    object_id_prefix: str = Field(default="", alias="objectIdPrefix")
    mesh_resource_overrides: Dict[str, str] = Field(default_factory=dict, alias="meshResourceOverrides")
    max_proposals: Optional[int] = Field(default=None, alias="maxProposals", ge=1)


class AssistantProposeSceneObjectsResponse(BaseModel):
    model_config = _ALIAS_CFG
    tool_name: str = Field(alias="toolName")
    proposals: List[AssistantProposalResponse] = Field(default_factory=list)
    scene_resources: List[ProgramSceneResourceResponse] = Field(
        default_factory=list,
        alias="sceneResources",
    )
    warnings: List[str] = Field(default_factory=list)
    mutated: bool = False
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProposeSceneObjectNodesRequest(BaseModel):
    model_config = _ALIAS_CFG
    scene_resources: List[ProgramSceneResourceResponse] = Field(alias="sceneResources")
    operations: List[
        Literal[
            "add_collision_object",
            "upsert_collision_object",
            "remove_collision_object",
            "attach_object_to_link",
            "detach_object_from_link",
        ]
    ] = Field(default_factory=lambda: ["add_collision_object"])
    link_frame: str = Field(default="tool0", alias="linkFrame")
    gripper_id: Optional[str] = Field(default=None, alias="gripperId")
    name_prefix: str = Field(default="", alias="namePrefix")


class AssistantProposeSceneObjectNodesResponse(BaseModel):
    model_config = _ALIAS_CFG
    tool_name: str = Field(alias="toolName")
    proposals: List[AssistantProposalResponse] = Field(default_factory=list)
    tree_nodes: List[TreeNodeResponse] = Field(default_factory=list, alias="treeNodes")
    warnings: List[str] = Field(default_factory=list)
    mutated: bool = False
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProposalStageRequest(BaseModel):
    model_config = _ALIAS_CFG
    conversation_id: str = Field(alias="conversationId", min_length=1)
    proposals: List[AssistantProposalResponse] = Field(min_length=1)
    source: Dict[str, Any] = Field(default_factory=dict)


class AssistantProposalRecordResponse(BaseModel):
    model_config = _ALIAS_CFG
    proposal_id: str = Field(alias="proposalId")
    conversation_id: str = Field(alias="conversationId")
    lifecycle_status: Literal["staged", "discarded", "applied"] = Field(alias="lifecycleStatus")
    proposal: AssistantProposalResponse
    source: Dict[str, Any] = Field(default_factory=dict)
    created_at_ns: int = Field(alias="createdAtNs")
    updated_at_ns: int = Field(alias="updatedAtNs")


class AssistantProposalStageResponse(BaseModel):
    model_config = _ALIAS_CFG
    conversation_id: str = Field(alias="conversationId")
    proposals: List[AssistantProposalRecordResponse] = Field(default_factory=list)
    mutated: bool = False
    program_mutated: bool = Field(default=False, alias="programMutated")
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProposalListResponse(BaseModel):
    model_config = _ALIAS_CFG
    proposals: List[AssistantProposalRecordResponse] = Field(default_factory=list)
    timestamp_ns: int = Field(alias="timestampNs")


class AssistantProposalDiscardRequest(BaseModel):
    model_config = _ALIAS_CFG
    reason: str = ""


class AssistantProposalMaterializeResponse(BaseModel):
    model_config = _ALIAS_CFG
    proposal_id: str = Field(alias="proposalId")
    proposal: AssistantProposalResponse
    session_patch: Dict[str, Any] = Field(alias="sessionPatch")
    mutated: bool = False
    program_mutated: bool = Field(default=False, alias="programMutated")
    timestamp_ns: int = Field(alias="timestampNs")


# --- File Browser ---
class FileBrowserEntryResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    path: str
    entry_type: str = Field(alias="entryType")
    selectable: bool


class FileBrowserResponse(BaseModel):
    model_config = _ALIAS_CFG
    current_path: str = Field(alias="currentPath")
    parent_path: Optional[str] = Field(default=None, alias="parentPath")
    roots: List[str] = Field(default_factory=list)
    entries: List[FileBrowserEntryResponse]


# --- WebSocket ---
class StateUpdate(BaseModel):
    model_config = _ALIAS_CFG
    state: str
    velocity_scale: float = Field(alias="velocityScale")
    node_statuses: Dict[str, str] = Field(default_factory=dict, alias="nodeStatuses")
    kernel_events: List[ExecutionEventResponse] = Field(default_factory=list, alias="kernelEvents")
    native_kernel_events: List[Dict[str, Any]] = Field(default_factory=list, alias="nativeKernelEvents")
    program_loaded: bool = Field(default=False, alias="programLoaded")
    program_session_generation: int = Field(default=0, alias="programSessionGeneration")
    runtime_override_status: Optional[RuntimeOverrideStatusResponse] = Field(
        default=None, alias="runtimeOverrideStatus"
    )
    error: Optional[str] = None
    timestamp: float
