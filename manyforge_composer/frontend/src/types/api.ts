import type { ProgramBindingDocument } from './programBindingTransport.generated';
import type { Nanoseconds } from './nanoseconds';
import type {
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from './robotControl';
export type { ProgramBindingDocument } from './programBindingTransport.generated';

export type ArtifactKind = 'program' | 'deployment' | 'mesh';

export interface RuntimeAvailability {
  code: string;
  message: string;
  restorationGates: string[];
}

export interface HealthResponse {
  status: string;
  mode: 'structural_only' | 'headless_client';
  components: Record<string, string>;
  runtimeAvailability: RuntimeAvailability | null;
  headless: Record<string, unknown> | null;
  timestamp: number;
}

export interface CatalogJsonSchema {
  [keyword: string]: unknown;
  type?: string;
  description?: string;
  properties?: Record<string, CatalogJsonSchema>;
  required?: string[];
  additionalProperties?: boolean | CatalogJsonSchema;
  items?: CatalogJsonSchema;
  enum?: unknown[];
  oneOf?: CatalogJsonSchema[];
  anyOf?: CatalogJsonSchema[];
  allOf?: CatalogJsonSchema[];
  not?: CatalogJsonSchema;
  if?: CatalogJsonSchema;
  then?: CatalogJsonSchema;
  const?: unknown;
  minimum?: number;
  maximum?: number;
  exclusiveMinimum?: number;
  exclusiveMaximum?: number;
  minItems?: number;
  maxItems?: number;
  default?: unknown;
  'x-manyforge-semantic-type'?: string;
  'x-manyforge-semantic-scope-parameter'?: string;
}

export interface CatalogParameter {
  id: string;
  required: boolean;
  schema: CatalogJsonSchema;
  schemaHash: string;
  hasDefault: boolean;
  default?: unknown;
}

export interface CatalogCapabilityRequirement {
  capabilityId: string;
  scope: string;
  effectful: boolean;
  targetParameter?: string;
  componentParameter?: string;
  whenParameter?: string;
  whenEquals?: unknown;
}

export interface CatalogBlackboardBinding {
  keyParameter: string;
  valueType: string;
  missingKey?: string;
}

export interface CatalogEntry {
  id: string;
  name: string;
  category: string;
  description: string;
  stability: string;
  kind: string;
  childrenMin: number;
  childrenMax: number;
  parameters: CatalogParameter[];
  requiredCapabilities: CatalogCapabilityRequirement[];
  execution: {
    blocking: boolean;
    cancellable: boolean;
    ownerApi: string;
  };
  runners: Record<string, string>;
  tags: string[];
  minProgramProfile: string | null;
  blackboard: {
    reads: CatalogBlackboardBinding[];
    writes: CatalogBlackboardBinding[];
  };
  ownerContext: {
    kind: string;
    required: boolean;
  } | null;
  outcomes: Record<string, unknown>;
  eventFamily: string | null;
}

export interface CatalogResponse {
  apiVersion: 'manyforge.node_catalog.v2';
  contentVersion: string;
  catalogId: string;
  entries: CatalogEntry[];
  catalogDigest: string;
  environmentDigest: string;
}

export interface FileBrowserEntry {
  name: string;
  path: string;
  entryType: 'directory' | 'file';
  selectable: boolean;
}

export interface FileBrowserResponse {
  currentPath: string;
  parentPath: string | null;
  roots: string[];
  entries: FileBrowserEntry[];
}

export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | { [key: string]: JsonValue };

export type ProgramSceneShape =
  | { type: 'box'; box_dimensions_m: Vector3Tuple }
  | { type: 'sphere'; sphere_radius_m: number }
  | {
      type: 'cylinder';
      cylinder_radius_m: number;
      cylinder_height_m: number;
    }
  | {
      type: 'mesh';
      mesh_resource_uri: string;
      mesh_scale?: Vector3Tuple;
    };

export type ProgramSceneFrameRef =
  | { kind: 'cell_frame' }
  | {
      kind: 'component_frame';
      component_role: string;
      model_role: string;
      frame_role: string;
    };

export interface ProgramScenePose {
  frame_ref: ProgramSceneFrameRef;
  position_m: Vector3Tuple;
  orientation:
    | { xyzw: QuaternionXyzwTuple }
    | { rpy_deg: Vector3Tuple };
}

export interface ProgramSceneResource {
  scene_resource_id: string;
  shape: ProgramSceneShape;
  pose: ProgramScenePose;
  resource_roles?: string[];
  metadata?: Record<string, JsonValue>;
  collision_policy_ids?: string[];
}

export interface ProgramViewerMetadata {
  frame_role_visibility?: Record<string, boolean>;
  frame_gizmo_size_m?: number;
  resource_role_visibility?: Record<string, boolean>;
  scene_resource_axes_visibility?: Record<string, boolean>;
  object_axes_gizmo_size_m?: number;
}

export interface ProgramValueSource {
  kind: 'program_parameter' | 'blackboard_key' | 'blackboard_field';
  reference: string;
  field?: string;
}

export type ProgramModificationPolicyValue =
  | 'forbidden'
  | 'human_approval'
  | 'llm_allowed';
export type ProgramInterventionDelegate =
  | 'forbidden'
  | 'human_only'
  | 'llm_allowed'
  | 'program_only';
export type ProgramRecoveryAction = 'intervention' | 'runtime_override';
export type ProgramPersistenceCeiling =
  | 'session_only'
  | 'runtime_override'
  | 'draft_patch';
export type ProgramOperatorStopPolicy = 'inherit' | 'hold' | 'continue';

export interface ProgramNodeModificationPolicy {
  runtime_override: ProgramModificationPolicyValue;
  persistent_edit: ProgramModificationPolicyValue;
  intervention_delegate: ProgramInterventionDelegate;
}

export interface ProgramNodeRecoveryPolicy {
  allow_intervention: boolean;
  allowed_recovery_actions: ProgramRecoveryAction[];
  max_retries: number;
  allowed_skills: string[];
  persistence_ceiling: ProgramPersistenceCeiling;
}

export interface ProgramTreeNode {
  node_id: string;
  instance_id: string;
  parameters: Record<string, JsonValue>;
  value_sources?: Record<string, ProgramValueSource>;
  children: ProgramTreeNode[];
  metadata?: Record<string, JsonValue>;
  modification_policy?: ProgramNodeModificationPolicy;
  recovery_policy?: ProgramNodeRecoveryPolicy;
  operator_stop_policy?: ProgramOperatorStopPolicy;
  execution_groups?: 'inherit' | 'all' | string[];
}

export type ProgramExecutionPolicy =
  | 'independent'
  | 'common_start'
  | 'common_start_and_finish'
  | 'sync_offset_traj';

export interface ProgramExecutionGroupDefinition {
  execution_policy: ProgramExecutionPolicy;
}

export interface ProgramParameterDeclaration {
  parameter_id: string;
  type: string;
  default: JsonValue;
  constraints?: Record<string, JsonValue>;
  runtime_mutability: 'immutable' | 'before_run' | 'between_operations';
  description?: string;
}

export interface BlackboardNamespaceDeclaration {
  namespace_id: string;
  owner: 'program';
  description?: string;
}

export interface BlackboardFieldDeclaration {
  field: string;
  type: string;
}

export interface BlackboardKeyDeclaration {
  key: string;
  type: string;
  access: 'read' | 'write' | 'read_write';
  persistence: 'node' | 'run' | 'persistent';
  fields?: BlackboardFieldDeclaration[];
  description?: string;
}

export interface ProgramDocument {
  api_version: 'manyforge.program.v2';
  program_id: string;
  program_revision: string;
  program_profile: 'composable-cell-v1';
  name: string;
  description?: string;
  catalog: {
    api_version: 'manyforge.node_catalog.v2';
    content_version: string;
  };
  compatibility?: Record<string, JsonValue>;
  viewer?: ProgramViewerMetadata;
  component_roles: Record<string, JsonValue>;
  target_roles: Record<string, JsonValue>;
  scene?: ProgramSceneResource[];
  parameters?: ProgramParameterDeclaration[];
  blackboard?: {
    namespaces: BlackboardNamespaceDeclaration[];
    keys: BlackboardKeyDeclaration[];
  };
  contact_policies?: JsonValue[];
  execution_groups?: Record<string, ProgramExecutionGroupDefinition>;
  tree: ProgramTreeNode;
}

export type ProgramAuthoringDocument = Omit<
  ProgramDocument,
  'program_revision' | 'tree'
> & {
  schema_version: 'manyforge.composer_authoring_forest.v1';
  forest: ProgramTreeNode[];
  positions: Record<string, { x: number; y: number }>;
};

export interface ProgramAuthoringDiagnostic {
  code: string;
  path: string;
  message: string;
  severity: 'error';
}

export interface ProgramDraftProjection {
  draftId: string;
  draftRevision: number;
  programLoaded: boolean;
  authoring: ProgramAuthoringDocument | null;
  candidateAvailable: boolean;
  programRevision: string | null;
  program: ProgramDocument | null;
  diagnostics: ProgramAuthoringDiagnostic[];
}

export interface ProgramDraftApplicability {
  applyReady: boolean;
  affectedInstanceIds: string[];
  unavailableReason: RuntimeAvailability | null;
}

export interface ProgramWorkspaceResponse {
  schemaVersion: 'manyforge.composer_program_workspace.v2';
  draft: ProgramDraftProjection;
  applicability: ProgramDraftApplicability;
  saved: {
    available: boolean;
    matchesDraft: boolean;
    path: string | null;
    programRevision: string | null;
  };
  applied: {
    available: boolean;
    matchesDraft: boolean | null;
    programRevision: string | null;
    unavailableReason: RuntimeAvailability | null;
  };
  hasUnsavedChanges: boolean;
  hasUnappliedChanges: boolean | null;
}

export type ScenarioDocumentDisplayStatus =
  | 'ready'
  | 'incomplete'
  | 'unknown'
  | 'changed'
  | 'version_changed'
  | 'unsupported';

export interface ScenarioDocumentEntry {
  sourceKind: 'saved' | 'qualified';
  path: string | null;
  scenarioId: string | null;
  name: string;
  description: string;
  baseScenarioId: string | null;
  savedManyforgeVersion: string | null;
  displayStatus: ScenarioDocumentDisplayStatus;
  statusDetail: string;
}

export interface ScenarioDocumentIndexResponse {
  apiVersion: 'manyforge.scenario_document_index.v1';
  directory: string;
  catalogRevision: string | null;
  entries: ScenarioDocumentEntry[];
}

export interface ScenarioDocumentFenceRequest extends DraftFenceRequest {
  deploymentDraftId: string;
  expectedDeploymentDraftRevision: number;
}

export interface ScenarioDocumentSaveResponse {
  status: 'saved';
  path: string;
  scenarioId: string;
  lastKnownStatus: 'ready' | 'incomplete' | 'unknown';
  workspace: ProgramWorkspaceResponse;
}

export interface ScenarioDocumentLoadResponse {
  status: 'loaded' | 'opened';
  path: string | null;
  scenarioId: string;
  activation: Record<string, JsonValue> | null;
  diagnostics: ProgramAuthoringDiagnostic[];
  workspace: ProgramWorkspaceResponse;
}

export interface DeploymentScenarioCandidate {
  scenarioId: string;
  scenarioCatalogRevision: string;
  deploymentArtifactId: string;
  deploymentRevision: string;
  retainedBundleId: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  description: string | null;
}

export interface DeploymentWorkspaceResponse {
  schemaVersion: 'manyforge.composer_deployment_workspace.v1';
  draft: {
    id: string;
    revision: number;
    document: Record<string, JsonValue> | null;
    deploymentArtifactId: string | null;
    deploymentRevision: string | null;
    graphRevision: string | null;
    differsFromSaved: boolean;
  };
  saved: {
    path: string | null;
    deploymentRevision: string | null;
  };
  managerApplied: {
    deploymentRevision: string | null;
    scenarioId: string | null;
  };
  runAdmission: {
    ready: boolean;
    reason: string | null;
  };
  binding: ProgramBindingDocument | null;
  scenarioCandidates: DeploymentScenarioCandidate[];
  savedScenarioCandidates: DeploymentScenarioCandidate[];
  unavailableReasons: string[];
}

export interface DeploymentCandidateCompilation {
  schemaVersion: 'manyforge.deployment_candidate_compilation.v1';
  scenarioId: string;
  scenarioCatalogRevision: string;
  deploymentArtifactId: string;
  deploymentRevision: string;
  retainedBundleId: string;
  graphRevision: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
}

export interface DeploymentCandidatePreparation {
  requestOwnerEpoch: string;
  requestId: string;
}

export interface DeploymentCandidateOperationResponse {
  schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1';
  workspace: DeploymentWorkspaceResponse;
  result: {
    status: 'previewed' | 'confirmation_required' | 'applied';
    confirmationToken: string | null;
    /** Authoritative identity of the journalized preparation, echoed on
     * confirmation_required so the confirmation can reference it. */
    preparation: DeploymentCandidatePreparation | null;
    compilation: DeploymentCandidateCompilation;
    candidatePreview: ManagerDeploymentCandidatePreviewResponse | null;
    managerApply: ManagerApplyResponse | null;
    selection: Omit<DeploymentCandidateCompilation, 'schemaVersion'>;
    /** Monotonic backend durations for the phases actually traversed. */
    phasesMs: Record<string, number>;
    applyId: string | null;
  };
}

export interface DeploymentControlledStopProfile {
  stop_mode: 'soft_on_path' | 'spring_back_hold';
  deceleration_time_s: number;
  allow_natural_completion_on_short_path: boolean;
  /** null is an older Draft that still requires an explicit migration choice. */
  jerk_limits_enabled: boolean | null;
  per_resource_limits: Array<{
    resource_id: string;
    max_deceleration: {
      coordinate_kind: string;
      value: number;
    };
    settle_velocity_threshold: {
      coordinate_kind: string;
      value: number;
    };
    max_jerk?: { coordinate_kind: string; value: number };
  }>;
}

export interface DeploymentStopTiming {
  command_latency_allowance_ns: number;
  settling_allowance_ns: number;
  operational_timeout_ns: number;
}

export interface DeploymentControlledStopUpdate {
  control_binding_id: string;
  control_binding_revision: string;
  /** null keeps the selected target's profile unchanged (including non-motion targets). */
  profile: DeploymentControlledStopProfile | null;
  timing: DeploymentStopTiming | null;
}

export interface ScenarioCatalogEntry {
  scenarioId: string;
  description: string | null;
  aliases: string[];
  deploymentArtifactId: string;
  deploymentRevision: string;
  retainedBundleId: string;
  programBindingId: string;
  assistantMode: 'required' | 'optional' | 'forbidden';
}

export interface ScenarioCatalogResponse {
  apiVersion: 'manyforge.scenario_catalog.v1';
  scenarioCatalogRevision: string;
  scenarios: ScenarioCatalogEntry[];
}

export interface CurrentScenarioSelection {
  scenarioId: string;
  scenarioCatalogRevision: string;
  deploymentArtifactId: string;
  deploymentRevision: string;
  retainedBundleId: string;
  graphRevision: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
}

export interface CurrentScenarioResponse {
  schemaVersion: 'manyforge.current_scenario.v1';
  ready: boolean;
  currentScenario: CurrentScenarioSelection | null;
  unavailableReasons: string[];
}

export interface AppliedProgramSelection {
  schemaVersion: 'manyforge.applied_program_selection.v1';
  scenarioId: string;
  scenarioCatalogRevision: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  retainedBundleId: string;
  graphRevision: string;
  nodeEnvironmentDigest: string;
  recordDigest: string;
  sourceOwnerId: string;
  sourceOwnerEpoch: string;
  sourceGeneration: number;
  program: ProgramDocument;
  programBinding: ProgramBindingDocument;
}

export type ProgramRunStateName =
  | 'running'
  | 'intervention_required'
  | 'full_stopping'
  | 'full_stopped'
  | 'stopping'
  | 'finalizing'
  | 'stopped'
  | 'succeeded'
  | 'failed';

export interface ProgramRunOwnerIdentity {
  ownerRole: string;
  ownerId: string;
  ownerEpoch: string;
}

export interface ProgramRunCapture {
  programRunId: string;
  appliedRecordDigest: string;
  scenarioId: string;
  scenarioCatalogRevision: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  retainedBundleId: string;
  graphRevision: string;
  nodeEnvironmentDigest: string;
  nodePolicies: JsonValue[];
  nodePolicyDigest: string;
  ownerIdentities: ProgramRunOwnerIdentity[];
}

export interface ProgramRunStatus {
  schemaVersion: 'manyforge.program_run_status.v2';
  ownerId: string;
  ownerEpoch: string;
  runRevision: number;
  state: ProgramRunStateName;
  capture: ProgramRunCapture;
  treeStatus: 'invalid' | 'running' | 'success' | 'failure';
  nodeStatusRevision: number;
  nodeStatuses: Record<string, 'idle' | 'running' | 'success' | 'failure'>;
  activeOperationCount: number | null;
  reasonCode: string | null;
  message: string;
}

export interface ProgramCycleStatus {
  programRunnerOwnerEpoch: string;
  cycleGeneration: number;
  state: 'idle_after_reset' | 'starting' | 'running' | 'stopping'
    | 'resetting' | 'terminal_awaiting_reset' | 'recovery_required';
  programRunId: string | null;
  cycleClaimGeneration: number | null;
  terminalOrSettlementReference: string | null;
  runRevision: number | null;
  nodeStatusRevision: number | null;
  runtimeControlRevision: number | null;
  executionControlRevision: number;
  causalCorrelationId: string | null;
  causalSessionId: string | null;
  causalClientSequence: string | null;
  currentCommandKind: string | null;
  failure: Record<string, JsonValue> | null;
  resetPhase: string | null;
  allowedCommands: Array<{
    commandKind: string;
    semanticTarget: Record<string, JsonValue>;
    allowed: boolean;
    disabledReason: string | null;
  }>;
  recoveryOffer: CycleRecoveryOffer | null;
  run: ProgramRunStatus | null;
  runtimeControls: ProgramRuntimeControlProjection | null;
}

export type RuntimeOverrideStatus = 'staged' | 'active' | 'cleared' | 'stale';

export interface RuntimeOverrideRecord {
  schemaVersion: 'manyforge.runtime_override.v1';
  overrideId: string;
  overrideRevision: number;
  status: RuntimeOverrideStatus;
  programRunId: string;
  programRevision: string;
  programBindingId: string;
  retainedBundleId: string;
  graphRevision: string;
  nodeEnvironmentDigest: string;
  nodeInstanceId: string;
  nodeAttemptId: string;
  parameterId: string;
  value: JsonValue;
  sourceObservationId: string | null;
  sourceOwnerGeneration: number | null;
}

export interface RuntimeOverridePromotionProposal {
  schemaVersion: 'manyforge.runtime_override_promotion.v1';
  proposalId: string;
  status: 'awaiting_reset' | 'ready';
  programRunId: string;
  overrideId: string;
  programRevision: string;
  programBindingId: string;
  nodeInstanceId: string;
  nodeId: string;
  parameterId: string;
  value: JsonValue;
  expectedDraftId: string;
  expectedDraftRevision: number;
}

export interface RuntimeControlAuditEvent {
  eventRevision: number;
  kind: string;
  facts: Record<string, JsonValue>;
}

export interface ProgramRuntimeControlsSnapshot {
  schemaVersion: 'manyforge.program_runtime_controls.v2';
  programRunId: string;
  runtimeControlRevision: number;
  recoveryFences: {
    programGeneration: number;
    deploymentGeneration: number;
    sceneAdmissionEpoch: number;
    poseSourceApplyGeneration: number;
  };
  hasActiveRuntimeOverrides: boolean;
  hasStagedRuntimeOverrides: boolean;
  hasStaleRuntimeOverrides: boolean;
  overrides: RuntimeOverrideRecord[];
  promotionProposals: RuntimeOverridePromotionProposal[];
  retryCounts: Record<string, number>;
  audit: RuntimeControlAuditEvent[];
}

export type CycleRecoveryActionId =
  | 'retry_motion_from_current_state'
  | 'acknowledge_and_continue'
  | 'retry_after_scene_repair'
  | 'replan_from_stopped_state'
  | 'retry_runtime_step'
  | 'reset_cycle';

export interface CycleRecoveryAction {
  actionId: CycleRecoveryActionId;
  labelKey: string;
  requiresConfirmation: boolean;
  performsBackendReset: boolean;
  enabled: boolean;
  disabledReasonCode: string | null;
}

export interface CycleRecoveryOffer {
  schemaVersion: 'manyforge.cycle_recovery_offer.v2';
  offerId: string;
  offerRevision: number;
  generatedAtNs: Nanoseconds;
  validUntilNs: Nanoseconds | null;
  kernelAssessmentRevision: number;
  controlCommandEpoch: number;
  operationId: string | null;
  operationRevision: number | null;
  programRunId: string;
  programBindingId: string;
  nodeInstanceId: string;
  nodeAttemptId: string;
  participants: JsonValue[];
  graphRevision: string;
  retainedBundleId: string;
  cycleGeneration: number;
  programGeneration: number;
  deploymentGeneration: number;
  sceneAdmissionEpoch: number;
  poseSourceApplyGeneration: number;
  latchId: string | null;
  stopId: string | null;
  stopGeneration: number | null;
  stopCertificateId: string | null;
  stopCertificateRevision: number | null;
  status: string;
  recoveryClass: string;
  actions: CycleRecoveryAction[];
  manualResult: {
    enabled: boolean;
    disabledReasonCode: string | null;
  };
  primaryAction: CycleRecoveryActionId | null;
  reasonCode: string;
  evidenceRefs: string[];
  policy: Record<string, JsonValue>;
}

export interface RuntimeOverrideObservationCandidate {
  schemaVersion: 'manyforge.runtime_override_observation_candidate.v1';
  available: boolean;
  reasonCode: string | null;
  reason: string | null;
  programRunId: string;
  nodeInstanceId: string | null;
  nodeAttemptId: string | null;
  nodeId: string | null;
  parameterId: string | null;
  value: JsonValue | null;
  sourceObservationId: string | null;
  sourceOwnerGeneration: number | null;
}

export interface ProgramExecutionSelectionControls {
  selectionRevision: number;
  groups: Array<{
    groupId: string;
    nodeCount: number;
    heldNodeCount: number;
    state: 'ready' | 'running' | 'held' | 'mixed' | 'stopping' | 'faulted' | 'unavailable';
    stopAllowed: boolean;
    startAllowed: boolean;
    startUnavailableReason: string | null;
    resumeAllowed: boolean;
    resumeUnavailableReason: string | null;
    /** Exact captured motion scope used by the runtime-scale owner. */
    runtimeScaleSelector: RuntimeScaleControlSelector | null;
    runtimeScaleUnavailableReason: string | null;
  }>;
}

export interface ProgramGroupRuntimeScaleState {
  selector: RuntimeScaleControlSelector | null;
  status: RuntimeScaleControlStatus | null;
  draft: number;
  pending: boolean;
  error: string | null;
}

export interface ProgramRuntimeControlProjection {
  schemaVersion: 'manyforge.program_runtime_control_projection.v6';
  ownerId: string;
  ownerEpoch: string;
  programRunId: string;
  runRevision: number;
  runtimeControls: ProgramRuntimeControlsSnapshot;
  recoveryOffer: CycleRecoveryOffer | null;
  executionSelections: ProgramExecutionSelectionControls;
  observationOverrideCandidate: RuntimeOverrideObservationCandidate;
}

export interface RuntimeOverrideMaterializationResponse {
  proposalId: string;
  mutation: string;
  details: Record<string, JsonValue>;
  workspace: ProgramWorkspaceResponse;
}

export type ProgramWorkspaceTransitionStatus =
  | 'applied'
  | 'loaded'
  | 'reverted';

export interface ProgramWorkspaceTransitionResponse {
  status: ProgramWorkspaceTransitionStatus;
  headless: Record<string, JsonValue> | null;
  workspace: ProgramWorkspaceResponse;
}

export interface NodeImplementationAvailability {
  nodeId: string;
  engineId: string;
  implementationId: string;
  executablePackageId: string;
  authoringAvailable: boolean;
  executionAvailable: boolean;
  reasonCodes: string[];
  restorationGates: string[];
}

export interface NodeEnvironmentResponse {
  environmentDigest: string;
  implementations: NodeImplementationAvailability[];
  portableAuthoringAvailable: boolean;
  executionAvailable: boolean;
  reasonCodes: string[];
  restorationGates: string[];
  active: {
    retainedBundleId: string;
    graphRevision: string;
  } | null;
}

export interface SourceModelProjectionSource {
  available: true;
  freshness: 'fresh';
  known_gaps: [];
  projection_generation: number;
  projection_owner_epoch: string;
  receiver_age_ns: number;
  selection_projection_generation: number;
  selection_source_content_digest: string;
  selection_source_generation: number;
  selection_source_owner_epoch: string;
  selection_source_owner_id: string;
  source_content_digest: string;
  source_generation: number;
  source_owner_epoch: string;
  source_owner_id: string;
  source_owner_kind: 'source_model_projection';
}

export interface SourceModelCoordinate {
  coordinate_kind: string;
  joint_type: string;
  mimic: {
    multiplier: number;
    offset: number;
    source_joint_name: string;
  } | null;
  source_child_link_name: string;
  source_joint_index: number;
  source_joint_name: string;
  source_parent_link_name: string;
  unit: string;
  viewer_child_link_name: string;
  viewer_joint_name: string;
  viewer_parent_link_name: string;
}

export interface SourceModelSrdfGroup {
  chains: Array<{
    source_base_link_name: string;
    source_tip_link_name: string;
  }>;
  source_group_name: string;
  source_joint_names: string[];
  source_link_names: string[];
  source_subgroup_names: string[];
}

export interface SourceModelOccurrence {
  component_id: string | null;
  coordinates: SourceModelCoordinate[];
  links: Array<{
    source_link_name: string;
    viewer_link_name: string;
  }>;
  model_asset_id: string;
  model_instance_id: string;
  model_revision: string;
  model_role_id: string | null;
  source_model_name: string;
  source_robot_name: string;
  srdf_end_effectors: Array<{
    source_end_effector_name: string;
    source_group_name: string;
    source_parent_group_name: string | null;
    source_parent_link_name: string;
  }>;
  srdf_group_states: Array<{
    source_group_name: string;
    source_group_state_name: string;
    source_joint_names: string[];
  }>;
  srdf_groups: SourceModelSrdfGroup[];
}

export interface SourceModelTarget {
  collision_bodies: Array<{
    collision_body_role_id: string;
    model_instance_id: string;
    source_link_names: string[];
  }>;
  component_id: string;
  frames: Array<{
    frame_kind: 'cell_frame' | 'component_model_frame' | 'passive_model_frame';
    frame_role_id: string;
    model_instance_id: string | null;
    source_link_name: string | null;
  }>;
  model_groups: Array<{
    commandable_source_joint_names: string[];
    model_group_role_id: string;
    model_instance_id: string;
    source_group_name: string;
    source_joint_names: string[];
    source_link_names: string[];
  }>;
  target_id: string;
}

export interface SourceModelProjection {
  schema_version: 'manyforge.source_model_projection.v2';
  cell_id: string;
  retained_bundle_id: string;
  graph_revision: string;
  model_map_contract: {
    contract_id: string;
    contract_revision: string;
  } | null;
  model_map_product_id: string | null;
  occurrences: SourceModelOccurrence[];
  targets: SourceModelTarget[];
  source: SourceModelProjectionSource;
}

export interface SceneRevision {
  scene_id: string;
  revision_id: number;
  content_hash: string;
}

export interface ProjectionRevision {
  retained_bundle_id: string;
  graph_revision: string;
  scene_revision: SceneRevision;
}

export interface LogicalScenePose {
  position_m: { x: number; y: number; z: number };
  orientation_xyzw: QuaternionXyzwTuple;
}

export type LogicalSceneShape =
  | { type: 'box'; dimensions_m: { x: number; y: number; z: number } }
  | { type: 'sphere'; radius_m: number }
  | { type: 'cylinder'; radius_m: number; height_m: number }
  | { type: 'mesh'; resource_uri: string; scale: { x: number; y: number; z: number } };

export interface SceneMeshAssetMetadata {
  schema_version: 'manyforge.scene_mesh_asset.v1';
  asset_hash: string;
  mesh_asset_url: string;
  mesh_asset_status: 'pending' | 'ok' | 'parse_failed';
  source_owner_epoch: string;
  source_generation: number;
  cell_id: string;
  last_error: string | null;
}

export interface ProgramDraftMeshAssetMetadata {
  schema_version: 'manyforge.program_draft_mesh_asset.v1';
  asset_hash: string;
  mesh_asset_url: string;
  mesh_asset_status: 'pending' | 'ok' | 'parse_failed';
  draft_id: string;
  draft_revision: number;
  last_error: string | null;
}

export type LogicalScenePlacement =
  | {
      kind: 'cell_fixed';
      cell_id: string;
      pose_in_cell: LogicalScenePose;
    }
  | {
      kind: 'carried';
      carried_object_attachment_id: string;
      carried_object_attachment_revision: string;
    };

export interface LogicalSceneObject {
  scene_resource_id: string;
  collision_shapes: LogicalSceneShape[];
  placement: LogicalScenePlacement;
  provider_evidence: Record<string, JsonValue> | null;
  support_contacts: Array<Record<string, JsonValue>>;
}

export interface CarriedObjectAttachment {
  carried_object_attachment_id: string;
  carried_object_attachment_revision: string;
  scene_resource_id: string;
  controllable_component_id: string;
  command_target_id: string;
  carrier_frame_ref: {
    kind: 'component';
    controllable_component_id: string;
    model_instance_id: string;
    frame_role_id: string;
  };
  pose_in_carrier_frame: {
    position_m: { x: number; y: number; z: number };
    orientation_xyzw: QuaternionXyzwTuple;
  };
  touch_body_roles: string[];
}

export interface LogicalSceneProjection {
  api_version: 'manyforge.logical_scene_source.v1';
  access: {
    active_projection: ProjectionRevision | null;
    phase: string;
    projection_leases_open: boolean;
    scene_writes_open: boolean;
  };
  snapshot: {
    carried_object_attachments: CarriedObjectAttachment[];
    cell_id: string;
    configured_collision_policy: Record<string, JsonValue>;
    objects: LogicalSceneObject[];
    policy_epoch: number;
    revision: SceneRevision;
    topology_epoch: number;
    transaction_sequence: number;
  } | null;
}

export interface SceneRuntimeObservationPose {
  frame_id: string;
  frame_kind: 'cell_frame';
  orientation_xyzw: QuaternionXyzwTuple;
  position_m: Vector3Tuple;
  source_stamp_ns: Nanoseconds | null;
}

export type SceneRuntimeGoodObservationPayload =
  | {
      api_version: 'manyforge.ros_pose_observation.v2';
      joint_names: string[];
      kind: 'joint_state';
      positions: number[];
      source_stamp_ns: Nanoseconds | null;
      topic: string;
    }
  | {
      api_version: 'manyforge.ros_pose_observation.v2';
      kind: 'simulation_entity_pose';
      pose: SceneRuntimeObservationPose;
      source: { entity: string };
    }
  | {
      api_version: 'manyforge.ros_pose_observation.v2';
      kind: 'detection3d_pose';
      pose: SceneRuntimeObservationPose;
      source: {
        class_id: string;
        confidence: number;
        detection_id: string;
        max_position_variance_m2: number;
        topic: string;
      };
    };

export interface SceneRuntimeUnknownObservationPayload {
  api_version: 'manyforge.ros_pose_observation.v2';
  kind: 'joint_state' | 'simulation_entity_pose' | 'detection3d_pose';
  source_error: {
    code: string;
    detail: string;
    source_key: string;
  };
}

interface SceneRuntimePoseSourceBase {
  buffered: boolean;
  expires_at_ns: Nanoseconds;
  object_id: string;
  observed_at_ns: Nanoseconds;
  policy_id: string;
  source_epoch: string;
  source_epoch_order: number;
  source_id: string;
  source_sequence: number;
}

export type SceneRuntimePoseSource =
  | (SceneRuntimePoseSourceBase & {
      eligible: boolean;
      fresh: boolean;
      payload: SceneRuntimeGoodObservationPayload;
      quality: 'good';
    })
  | (SceneRuntimePoseSourceBase & {
      buffered: false;
      eligible: false;
      fresh: false;
      payload: SceneRuntimeUnknownObservationPayload;
      quality: 'unknown';
    });

export interface SceneRuntimeProjection {
  api_version: 'manyforge.scene_runtime_source.v1';
  cell_id: string;
  state: {
    api_version: 'manyforge.scene_runtime_state.v1';
    active_replay_id: number | null;
    counters: Record<string, number>;
    deferred: boolean;
    error: string | null;
    initialization_generation: number;
    initialization_token: string | null;
    pose_sources: SceneRuntimePoseSource[];
    reason: string;
    runtime_identity: {
      deployment_generation: number;
      graph_revision: string;
      program_generation: number;
      retained_bundle_id: string;
      runtime_generation: number;
    } | null;
    started_at_ns: Nanoseconds;
    state: 'open' | 'closed' | 'resetting' | 'seeding' | 'replaying' | 'failed' | 'retired';
    updated_at_ns: Nanoseconds;
  };
}

export interface ScenePoseRequest {
  position_m: { x_m: number; y_m: number; z_m: number };
  orientation: { x: number; y: number; z: number; w: number };
}

export type SceneShapeRequest =
  | {
      type: 'box';
      box_dimensions_m: { x_m: number; y_m: number; z_m: number };
    }
  | { type: 'sphere'; sphere_radius_m: number }
  | {
      type: 'cylinder';
      cylinder_radius_m: number;
      cylinder_height_m: number;
    }
  | {
      type: 'mesh';
      mesh_resource_uri: string;
      mesh_scale: { x_m: number; y_m: number; z_m: number };
    };

export type SceneTransactionFrameRequest =
  | { kind: 'cell_frame'; cell_id: string }
  | {
      kind: 'component_frame';
      controllable_component_id: string;
      model_instance_id: string;
      frame_role_id: string;
    }
  | {
      kind: 'passive_model_frame';
      model_instance_id: string;
      frame_role_id: string;
    }
  | { kind: 'scene_resource'; scene_resource_id: string };

export type SceneTransactionOperation =
  | {
      kind: 'add_object' | 'upsert_object';
      scene_resource_id: string;
      shape: SceneShapeRequest;
      pose: ScenePoseRequest;
      frame_ref: SceneTransactionFrameRequest;
      support_contacts: Array<Record<string, JsonValue>>;
    }
  | { kind: 'remove_object'; scene_resource_id: string }
  | {
      kind: 'update_object_pose';
      scene_resource_id: string;
      pose: ScenePoseRequest;
      frame_ref: SceneTransactionFrameRequest;
      provider_evidence: null;
      expected_attached: false;
    }
  | {
      kind: 'attach_carried_object';
      carried_object_attachment_id: string;
      scene_resource_id: string;
      controllable_component_id: string;
      command_target_id: string;
      frame_role_id: string;
      touch_body_roles: string[];
      expected_component_attachment_revisions: string[];
    }
  | {
      kind: 'detach_carried_object_at_current_pose';
      carried_object_attachment_id: string;
      expected_carried_object_attachment_revision: string;
      expected_scene_resource_id: string;
      expected_controllable_component_id: string;
      expected_command_target_id: string;
    };

interface SceneTransactionRequestBase {
  request_context: {
    request_id: string;
    request_owner_epoch: string;
    source_label: 'composer_ui';
    correlation_id: string | null;
  };
  expected_projection_revision: ProjectionRevision;
}

type RuntimeOwnedSceneOperation = Extract<
  SceneTransactionOperation,
  { kind: 'attach_carried_object' | 'detach_carried_object_at_current_pose' }
>;
type AuthoredSceneOperation = Exclude<
  SceneTransactionOperation,
  RuntimeOwnedSceneOperation
>;

export type SceneTransactionRequest = SceneTransactionRequestBase & (
  | {
      expected_admission_generation: number;
      expected_draft_id: string;
      expected_draft_revision: number;
      operation: AuthoredSceneOperation;
    }
  | {
      expected_admission_generation?: never;
      expected_draft_id?: never;
      expected_draft_revision?: never;
      operation: RuntimeOwnedSceneOperation;
    }
);

export interface SceneTransactionResponse {
  api_version: 'manyforge.manager_command_response.v1';
  committed_projection_revision: ProjectionRevision;
  error: null;
  ok: true;
  projection_status: string;
  replay_status: string;
  request_id: string;
  request_owner_epoch: string;
  transaction_sequence: number;
  update_status: string;
}

export interface DraftFenceRequest {
  draftId: string;
  expectedDraftRevision: number;
  expectedAdmissionGeneration: number;
  requestOwnerEpoch: string;
  requestId: string;
}

export interface ManagerDeploymentCandidateReceipt {
  schema_version: 'manyforge.manager_deployment_candidate_receipt.v1';
  deployment_artifact_id: string;
  deployment_revision: string;
  scenario_catalog_revision: string;
  candidate_retained_bundle_id: string;
  candidate_graph_revision: string;
  expected_current: {
    graph_revision: string;
    retained_bundle_id: string;
  };
  expected_admission_generation: number;
  manager_owner_epoch: string;
  receipt_digest: string;
}

export interface ManagerDeploymentCandidatePreviewRequest {
  deployment_artifact_id: string;
  deployment_revision: string;
  expected_current: {
    graph_revision: string;
    retained_bundle_id: string;
  };
  expected_admission_generation: number;
  request_owner_epoch: string;
}

export interface ManagerDeploymentRecordDiff {
  added: string[];
  changed: string[];
  removed: string[];
}

export interface ManagerDeploymentCandidatePreviewResponse {
  apiVersion: 'manyforge.manager_command_response.v1';
  candidateReceipt: ManagerDeploymentCandidateReceipt;
  converged: boolean;
  diff: Record<string, ManagerDeploymentRecordDiff>;
}

export interface ManagerApplyRequest {
  request_id: string;
  request_owner_epoch: string;
  source_label: 'composer_ui';
  correlation_id: string | null;
  deployment_artifact_id: string;
  deployment_revision: string;
  expected_current: {
    graph_revision: string;
    retained_bundle_id: string;
  };
  reject_if_affected_busy: true;
  expected_admission_generation: number;
  candidate_receipt: ManagerDeploymentCandidateReceipt;
}

export interface ManagerSourceMetadata {
  projectionGeneration: number;
  projectionOwnerEpoch: string;
  available: boolean;
  freshness: 'fresh' | 'stale' | 'unavailable';
  knownGaps: string[];
  receiverAgeNs: number | null;
  sourceGeneration: number | null;
  sourceOwnerEpoch: string | null;
  sourceOwnerId: string | null;
  sourceContentDigest: string | null;
}

export interface ManagerActiveProjection {
  cellGraph: Record<string, JsonValue>;
  graphRevision: string;
  retainedBundleId: string;
  publication: Record<string, JsonValue>;
}

export type ManagerApplyPhase =
  | 'intent_persisted'
  | 'scope_persisted'
  | 'fence_acquire_pending'
  | 'fence_held'
  | 'runtime_convergence_pending'
  | 'runtime_staged_ready'
  | 'scene_stage_pending'
  | 'scene_staged'
  | 'activation_selected'
  | 'scene_activated'
  | 'private_materialized'
  | 'command_surfaces_retired'
  | 'active_bundle_published'
  | 'scene_finalized'
  | 'fence_release_pending'
  | 'fence_released'
  | 'terminal';

export interface ManagerApplyRecord {
  applyGeneration: number;
  applyId: string;
  candidateGraphRevision: string;
  candidateRetainedBundleId: string;
  committed: boolean;
  expectedActiveProjectionRevision: ProjectionRevision;
  fence: {
    acceptanceDigest: string | null;
    configurationFenceGeneration: number | null;
    configurationFenceId: string | null;
    fenceAttemptDigest: string | null;
    scope: Record<string, JsonValue> | null;
    state:
      | 'not_requested'
      | 'scope_persisted'
      | 'acquire_pending'
      | 'held'
      | 'release_pending'
      | 'released';
  };
  incomplete: boolean;
  intentDigest: string;
  phase: ManagerApplyPhase;
  request: {
    correlationId: string | null;
    requestId: string;
    requestOwnerEpoch: string;
    sourceLabel: string;
  };
  terminalState: 'succeeded' | null;
}

export interface ManagerApplyAvailability {
  mutationAvailable: boolean;
  records: ManagerApplyRecord[];
  unavailableReasonCode: string | null;
}

export interface ManagerRuntimeProjection {
  converged: boolean;
  desiredUnits: JsonValue[];
  observedUnits: JsonValue[];
}

export interface ManagerStatusProjection {
  schemaVersion: 'manyforge.manager_status_projection.v1';
  source: ManagerSourceMetadata;
  status: {
    apiVersion: 'manyforge.manager_source.v1';
    active: ManagerActiveProjection;
    apply: ManagerApplyAvailability;
    cellId: string;
    managerReady: boolean;
    runtime: ManagerRuntimeProjection;
  };
}

export interface ManagerGraphProjection {
  schemaVersion: 'manyforge.manager_graph_projection.v1';
  source: ManagerSourceMetadata;
  graphRevision: string;
  retainedBundleId: string;
  cellGraph: Record<string, JsonValue>;
}

export interface ManagerDiffProjection {
  schemaVersion: 'manyforge.manager_diff_projection.v1';
  source: ManagerSourceMetadata;
  converged: boolean;
  desiredUnits: JsonValue[];
  observedUnits: JsonValue[];
}

export type ManagerApplyReplayStatus = 'accepted_new' | 'replayed_exact';

export interface ManagerApplyPendingResponse {
  apiVersion: 'manyforge.manager_command_response.v1';
  applyGeneration: number;
  applyId: string;
  candidateProjectionRevision: null;
  error: null;
  ok: true;
  replayStatus: ManagerApplyReplayStatus;
  requestId: string;
  requestOwnerEpoch: string;
  state: 'pending';
}

export interface ManagerApplySucceededResponse {
  apiVersion: 'manyforge.manager_command_response.v1';
  applyGeneration: number;
  applyId: string;
  candidateProjectionRevision: ProjectionRevision;
  error: null;
  ok: true;
  replayStatus: ManagerApplyReplayStatus;
  requestId: string;
  requestOwnerEpoch: string;
  state: 'succeeded';
}

export type ManagerApplyResponse =
  | ManagerApplyPendingResponse
  | ManagerApplySucceededResponse;

export interface DraftMutationResponse {
  mutation: string;
  details: Record<string, JsonValue>;
  workspace: ProgramWorkspaceResponse;
}

export type Vector3Tuple = [number, number, number];
export type QuaternionXyzwTuple = [number, number, number, number];

export type SceneUrdfPrimitive =
  | { type: 'box'; size_m: Vector3Tuple }
  | { type: 'cylinder'; radius_m: number; length_m: number }
  | { type: 'sphere'; radius_m: number };

export interface SceneUrdfOrigin {
  position_m: Vector3Tuple;
  orientation_rpy_rad: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

export interface SceneUrdfVisual {
  index: number;
  mesh_url: string | null;
  primitive: SceneUrdfPrimitive | null;
  visual_origin: SceneUrdfOrigin;
}

export interface SceneUrdfCollision {
  index: number;
  mesh_url: string | null;
  primitive: SceneUrdfPrimitive | null;
  collision_origin: SceneUrdfOrigin;
}

export interface RetainedNeutralTransform {
  relative_to: string;
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

export interface SceneUrdfLink {
  name: string;
  neutral_transform: RetainedNeutralTransform;
  visuals: SceneUrdfVisual[];
  collisions: SceneUrdfCollision[];
}

export type SceneUrdfJointType =
  | 'revolute'
  | 'prismatic'
  | 'continuous'
  | 'fixed'
  | 'floating'
  | 'planar';

export interface SceneUrdfJoint {
  name: string;
  type: SceneUrdfJointType | string;
  axis: Vector3Tuple;
  parent_link: string;
  child_link: string;
  origin: SceneUrdfOrigin & { relative_to: string };
  coordinate_unit: 'rad' | 'm' | null;
  neutral_position: number | null;
  limit_lower: number | null;
  limit_upper: number | null;
  effort_limit: number | null;
  velocity_limit: number | null;
  mimic: {
    master: string;
    multiplier: number;
    offset: number;
  } | null;
  is_mimic_master: boolean;
}

export interface RetainedNeutralProjection {
  kind: 'retained_neutral';
  source: 'active_retained_viewer_model';
  root_frame: string;
  position_unit: 'm';
  angle_unit: 'rad';
  quaternion_order: 'xyzw';
  joint_coordinate_policy: 'zero_independent_with_retained_mimic';
  operational_validity: 'not_asserted';
  runtime_state_used: false;
}

export interface SceneUrdfManifest {
  cell_id: string;
  graph_revision: string;
  retained_bundle_id: string;
  viewer_model_revision: string;
  model_key: string;
  root_link: string;
  projection: RetainedNeutralProjection;
  links: SceneUrdfLink[];
  joints: SceneUrdfJoint[];
}
