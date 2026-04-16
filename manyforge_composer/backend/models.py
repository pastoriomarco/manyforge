"""Pydantic request/response models for ManyForge Composer API."""
from __future__ import annotations

from typing import Any, Optional, List, Dict, Literal
from pydantic import BaseModel, ConfigDict, Field, model_validator

_ALIAS_CFG = ConfigDict(populate_by_name=True)


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
class CatalogParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    required: bool
    default: Any = None
    enum_values: List[str] = Field(default_factory=list, alias="enumValues")
    constraints: Dict[str, Any] = Field(default_factory=dict)
    parameterizable: bool = False
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
    kind: str
    children_min: int = Field(alias="childrenMin")
    children_max: int = Field(alias="childrenMax")
    parameters: List[CatalogParameterResponse] = Field(default_factory=list)
    bb_reader_params: List[str] = Field(default_factory=list, alias="bbReaderParams")
    bb_writer_params: List[str] = Field(default_factory=list, alias="bbWriterParams")
    required_capabilities: List[str] = Field(default_factory=list, alias="requiredCapabilities")


class CatalogResponse(BaseModel):
    version: str
    entries: List[CatalogEntryResponse]


# --- Program ---
class LoadProgramRequest(BaseModel):
    model_config = _ALIAS_CFG
    path: str
    deployment_path: Optional[str] = Field(None, alias="deploymentPath")
    force_discard_overrides: bool = Field(default=False, alias="forceDiscardOverrides")


class RobotResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    base_frame: str = Field(alias="baseFrame")
    tcp_frame: str = Field(alias="tcpFrame")


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
    parameter_count: int = Field(alias="parameterCount")
    parameters: List["ProgramParameterResponse"] = Field(default_factory=list)
    blackboard: "ProgramBlackboardResponse" = Field(default_factory=lambda: ProgramBlackboardResponse())
    source_path: str = Field(alias="sourcePath", default="")


class ProgramParameterResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    type: str
    default: Any = None
    constraints: Dict[str, Any] = Field(default_factory=dict)
    description: str = ""


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


class ProgramTreeResponse(BaseModel):
    tree: TreeNodeResponse


class SaveProgramRequest(BaseModel):
    path: str
    tree: Optional[TreeNodeResponse] = None
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


class RobotConfigResponse(BaseModel):
    model_config = _ALIAS_CFG
    robot_id: str = Field(alias="robotId")
    group_id: str = Field(alias="groupId")
    urdf_path: str = Field(alias="urdfPath")
    global_max_tcp_linear_speed_mps: float = Field(alias="globalMaxTcpLinearSpeedMps")


class DeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")


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


class ModelEndpointResponse(BaseModel):
    model_config = _ALIAS_CFG
    provider: str
    model_id: str = Field(alias="modelId")
    base_url: Optional[str] = Field(default=None, alias="baseUrl")
    image_fetch_base_url: Optional[str] = Field(default=None, alias="imageFetchBaseUrl")
    api_key_env: Optional[str] = Field(default=None, alias="apiKeyEnv")
    default_timeout_s: float = Field(default=30.0, alias="defaultTimeoutS")
    max_tokens: int = Field(default=4096, alias="maxTokens")


class PromptTemplateResponse(BaseModel):
    model_config = _ALIAS_CFG
    version: str
    template_text: str = Field(alias="templateText")
    output_schema: Optional[Dict[str, Any]] = Field(default=None, alias="outputSchema")
    response_tool: Optional[Dict[str, Any]] = Field(default=None, alias="responseTool")


class FullDeploymentResponse(BaseModel):
    model_config = _ALIAS_CFG
    api_version: str = Field(alias="apiVersion")
    schema_version: str = Field(alias="schemaVersion")
    name: str
    description: str
    program_profile: str = Field(alias="programProfile")
    robot_config: RobotConfigResponse = Field(alias="robotConfig")
    adapters: Dict[str, AdapterConfigResponse]
    orchestrator: OrchestratorConfigResponse
    launch: LaunchConfigResponse
    model_audit_policy: Optional[ModelAuditPolicyResponse] = Field(
        default=None,
        alias="modelAuditPolicy",
    )
    model_endpoints: Dict[str, ModelEndpointResponse] = Field(
        default_factory=dict,
        alias="modelEndpoints",
    )
    prompt_templates: Dict[str, PromptTemplateResponse] = Field(
        default_factory=dict,
        alias="promptTemplates",
    )
    source_path: str = Field(alias="sourcePath", default="")


class ModelEndpointProbeResponse(BaseModel):
    model_config = _ALIAS_CFG
    alias: str
    provider: str
    configured_model_id: str = Field(alias="configuredModelId")
    base_url: Optional[str] = Field(default=None, alias="baseUrl")
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
    adapters: Optional[Dict[str, Dict[str, Any]]] = None
    orchestrator: Optional[Dict[str, Any]] = None
    launch: Optional[Dict[str, Any]] = None


class SaveDeploymentRequest(BaseModel):
    path: str
    deployment: Optional[FullDeploymentResponse] = None


class ApplySessionRequest(BaseModel):
    model_config = _ALIAS_CFG
    tree: Optional[TreeNodeResponse] = None
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


class SceneRobotResponse(BaseModel):
    robot_id: Optional[str] = None
    group_id: Optional[str] = None
    base_frame: str
    tcp_frame: str
    joint_names: List[str] = Field(default_factory=list)
    joint_positions: Dict[str, float] = Field(default_factory=dict)
    tcp_pose_in_base: Optional[Dict[str, Any]] = None


class SceneStateResponse(BaseModel):
    robot: Optional[SceneRobotResponse] = None
    objects: List[SceneObjectResponse] = Field(default_factory=list)
    timestamp_ns: int


class SceneEditApiRequest(BaseModel):
    operation: Literal["add", "remove", "update_pose", "attach", "detach"]
    object_id: str
    shape_type: Optional[Literal["box", "sphere"]] = None
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    pose_position_m: Optional[List[float]] = None
    pose_orientation_xyzw: Optional[List[float]] = None
    frame_id: Optional[str] = None

    @model_validator(mode="after")
    def validate_request(self) -> "SceneEditApiRequest":
        if not self.object_id:
            raise ValueError("object_id must not be empty")

        if self.operation == "add":
            if self.shape_type is None:
                raise ValueError("add requires shape_type")
            if self.shape_type == "box":
                if self.box_dimensions_m is None or len(self.box_dimensions_m) != 3:
                    raise ValueError("add with shape_type=box requires box_dimensions_m=[x, y, z]")
                if any(v <= 0.0 for v in self.box_dimensions_m):
                    raise ValueError("box_dimensions_m values must be > 0")
            if self.shape_type == "sphere":
                if self.sphere_radius_m is None or self.sphere_radius_m <= 0.0:
                    raise ValueError("add with shape_type=sphere requires sphere_radius_m > 0")

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
    timestamp_ns: int
    run_id: Optional[str] = None
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
    node_statuses: Dict[str, str] = Field(alias="nodeStatuses")
    intervention_session: Optional[Dict[str, Any]] = Field(default=None, alias="interventionSession")
    runtime_overrides: Dict[str, Any] = Field(default_factory=dict, alias="runtimeOverrides")
    model_events: List[Dict[str, Any]] = Field(default_factory=list, alias="modelEvents")


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
