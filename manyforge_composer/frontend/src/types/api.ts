import type { ProgramBindingDocument } from './programBindingTransport.generated';
import type { Nanoseconds } from './nanoseconds';
import type { AuthoringWorkspaceIdentity } from './headless';
import type {
  MotionPreviewReceipt,
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
  workbenchSessionNotice?: WorkbenchSessionNotice | null;
}

export interface WorkbenchSessionNotice {
  schemaVersion: 'manyforge.workbench_previous_session.v1';
  sessionId: string;
  archiveRelativePath: string;
  shutdown: 'clean' | 'unknown';
  effects: 'closed' | 'unresolved' | 'unknown';
  historyCoverage: 'not_imported';
  journalFamilies: string[];
  message: string;
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
  /** A `key_value` parameter: its type is the declared type of the key named by this parameter. */
  'x-manyforge-value-type-from-key'?: string;
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
  /** Execution groups the Motion panel does not show (absent: default). */
  hidden_execution_groups?: string[];
}

/**
 * Where one value of a node comes from, keyed in `value_sources` by the
 * value's path (`utils/valuePaths.ts`). The literal stays in `parameters`
 * and is restored when the source is removed.
 */
export type ProgramValueSource =
  | { kind: 'program_parameter' | 'blackboard_key'; reference: string }
  | { kind: 'blackboard_field'; reference: string; field: string }
  | { kind: 'expression'; expression: string };

export type ProgramOperatorStopPolicy = 'inherit' | 'hold' | 'continue';

/** Which motion groups stop while a run waits for the operator. */
export type ProgramInterventionStopScope = 'all' | 'own_groups';

/**
 * The captured failure-recovery policy of one node. `max_retries` 0 is
 * Unlimited, never "disabled"; intervention stays an explicit opt-in. The
 * stop scope is omitted when it is the default `all`.
 */
export interface ProgramNodeRecoveryPolicy {
  allow_intervention: boolean;
  max_retries: number;
  intervention_stop_scope?: 'own_groups';
}

export interface ProgramTreeNode {
  node_id: string;
  instance_id: string;
  parameters: Record<string, JsonValue>;
  value_sources?: Record<string, ProgramValueSource>;
  children: ProgramTreeNode[];
  metadata?: Record<string, JsonValue>;
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

/** Program types that accept an authored parameter default or key initial value. */
export const PROGRAM_AUTHORED_VALUE_TYPES: ReadonlySet<string> = new Set([
  'bool',
  'float64',
  'int64',
  'scene_resource',
  'stored_pose',
  'string',
  'string_list',
]);

export interface ProgramParameterDeclaration {
  parameter_id: string;
  type: string;
  default: JsonValue;
  constraints?: Record<string, JsonValue>;
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

/** A pose frame a Program declares: the cell, a robot's frame role or a Scene object. */
export type ProgramPoseFrame =
  | { kind: 'cell_frame' }
  | { kind: 'target_frame'; target_role: string; frame_role: string }
  | { kind: 'scene_resource'; scene_resource_id: string };

export interface BlackboardKeyDeclaration {
  key: string;
  type: string;
  access: 'read' | 'write' | 'read_write';
  persistence: 'run' | 'persistent';
  /** Omitted for a key that is empty until written; never null. */
  initial?: Exclude<JsonValue, null>;
  /** A computed key's formula; never with `initial` or `persistent`. */
  expression?: string;
  /**
   * Pose keys only: the declared frames a value may be stated in, non-empty
   * and in canonical order; omitted allows every declared frame.
   */
  allowed_frames?: ProgramPoseFrame[];
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

/**
 * The `param.<id>` and `key.<key>` names each Draft formula reads, sorted, as
 * the backend's expression engine resolved them. A formula that does not
 * check is absent; its diagnostics say why.
 */
export interface ProgramFormulaReferences {
  /** Computed keys by key. */
  computedKeys: Record<string, string[]>;
  /** Node field formulas by node instance, then value path. */
  nodes: Record<string, Record<string, string[]>>;
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
  formulaReferences: ProgramFormulaReferences;
}

export interface ProgramDraftApplicability {
  applyReady: boolean;
  unavailableReason: RuntimeAvailability | null;
}

export interface ProgramWorkspaceResponse {
  schemaVersion: 'manyforge.composer_program_workspace.v2';
  authoringWorkspace: AuthoringWorkspaceIdentity;
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
  sourceKind: 'saved';
  path: string;
  scenarioId: string | null;
  name: string;
  description: string;
  savedManyforgeVersion: string | null;
  displayStatus: ScenarioDocumentDisplayStatus;
  statusDetail: string;
}

export interface ScenarioDocumentIndexResponse {
  apiVersion: 'manyforge.scenario_document_index.v1';
  directory: string;
  entries: ScenarioDocumentEntry[];
}

export interface ScenarioDocumentSaveResponse {
  status: 'saved';
  path: string;
  scenarioId: string;
  lastKnownStatus: 'ready' | 'incomplete' | 'unknown';
  workspace: ProgramWorkspaceResponse;
}

export interface NewScenarioDocumentInput {
  path: string;
  name: string;
  deploymentChoice: 'empty';
  expectedRetainedBundleId: string;
  expectedGraphRevision: string;
  confirmDiscard: boolean;
}

export type ScenarioDocumentActivation =
  | (Record<string, JsonValue> & {
      status: 'accepted';
      apply_id: string;
      apply_generation: number;
    })
  | (Record<string, JsonValue> & {
      status: 'applied';
      apply_id: null;
    });

interface ScenarioDocumentLoadResponseBase {
  path: string;
  scenarioId: string;
  incomplete: boolean;
  diagnostics: ProgramAuthoringDiagnostic[];
  sourceDiagnostics: ScenarioSourceDiagnostic[];
  workspace: ProgramWorkspaceResponse;
  authoringWorkspace: AuthoringWorkspaceSnapshot;
}

export type ScenarioDocumentLoadResponse = ScenarioDocumentLoadResponseBase & (
  | {
      status: 'accepted';
      activation: Extract<ScenarioDocumentActivation, { status: 'accepted' }>;
    }
  | {
      status: 'applied';
      activation: Extract<ScenarioDocumentActivation, { status: 'applied' }>;
    }
);

/**
 * The retained Scenario project facts of the one shared authoring workspace.
 *
 * These are the facts an operator cannot re-derive from a Draft alone: the
 * selected project path, the Saved baseline identity, the source diagnostics and
 * the Program role assignment that was captured from the
 * document. They are read once per observed workspace identity, together with
 * both Draft projections, so no consumer has to combine two reads.
 */
export interface AuthoringWorkspaceProjectFacts {
  documentPath: string | null;
  documentId: string | null;
  documentName: string | null;
  documentDescription: string | null;
  sourceDiagnostics: ScenarioSourceDiagnostic[];
  sourceDocumentDigest: string | null;
  programRoleAssignment: Record<string, JsonValue> | null;
  sourceProgramDigest: string | null;
  sourceDeploymentDigest: string | null;
}

export interface AuthoringWorkspaceProgramSaved {
  available: boolean;
  matchesDraft: boolean;
  path: string | null;
  programRevision: string | null;
}

export interface AuthoringWorkspaceDeploymentDraft {
  draftId: string;
  draftRevision: number;
  document: Record<string, JsonValue> | null;
  savedPath: string | null;
  savedDocument: Record<string, JsonValue> | null;
  differsFromSaved: boolean;
}

/**
 * One atomic composite authoring snapshot.
 *
 * The closed response of `GET /api/authoring/workspace`: the workspace identity,
 * the Program Draft and Saved projection, the Deployment Draft and Saved
 * projection, and the Scenario project facts, all captured at one revision.
 * Runtime, Applied and Manager truth is deliberately absent; those remain in
 * their existing owner projections.
 */
export interface AuthoringWorkspaceSnapshot {
  schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v2';
  workspaceEpoch: string;
  workspaceRevision: number;
  program: {
    draft: ProgramDraftProjection;
    saved: AuthoringWorkspaceProgramSaved;
  };
  deployment: AuthoringWorkspaceDeploymentDraft;
  project: AuthoringWorkspaceProjectFacts;
}

export interface ScenarioSourceDiagnostic {
  code: 'scenario_source_changed';
  sourceId: string;
  message: string;
}

export interface ScenarioDocumentExportResponse {
  status: 'exported';
  path: string;
}

export interface DeploymentScenarioCandidate {
  scenarioId: string;
  sourceDocumentDigest: string;
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
  authoringWorkspace: AuthoringWorkspaceIdentity;
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
    activationOrder: {
      retainedBundleId: string;
      graphRevision: string;
      componentIds: string[];
    } | null;
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

export type PreparedActivationOrigin = {
  kind: 'scenario'; scenarioId: string; sourceDocumentDigest: string;
} | {
  kind: 'sealed_canonical_empty'; cellId: string; deploymentArtifactId: string;
  deploymentRevision: string; retainedBundleId: string; graphRevision: string;
  admissionDigest: string;
};

export interface PreparedActivationInput {
  schemaVersion: 'manyforge.prepared_activation_input.v6';
  preparedActivationId: string;
  origin: PreparedActivationOrigin;
  capturedInputDigest: string;
  deploymentArtifactId: string;
  deploymentRevision: string;
  retainedBundleId: string;
  graphRevision: string;
  programId: string | null;
  programRevision: string | null;
  /** Null when no Program is selected or the preserved Program is non-executable in this
   * candidate; the named references are in the preview impact (H1a). */
  programBindingId: string | null;
  nodeEnvironmentDigest: string;
  assistantMode: 'required' | 'optional' | 'forbidden';
}

export interface DeploymentCandidatePreparation {
  requestOwnerEpoch: string;
  requestId: string;
}

export interface DeploymentCandidateOperationResponse {
  schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1';
  workspace: DeploymentWorkspaceResponse;
  result: {
    status: 'previewed' | 'confirmation_required' | 'accepted' | 'applied';
    confirmationToken: string | null;
    /** Authoritative identity of the journalized preparation, echoed on
     * confirmation_required so the confirmation can reference it. */
    preparation: DeploymentCandidatePreparation | null;
    preparedActivation: PreparedActivationInput;
    candidatePreview: ManagerDeploymentCandidatePreviewResponse | null;
    managerApply: ManagerApplyResponse | null;
    /** Monotonic backend durations for the phases actually traversed. */
    phasesMs: Record<string, number>;
    applyId: string | null;
  };
}

export interface DeploymentControlledStopProfile {
  stop_mode: 'soft_on_path' | 'spring_back_hold' | 'spring_back_fast';
  deceleration_time_s: number;
  /** Q28/Q51 (`main`): this profile's controlled-Stop deadline from the Stop
   * request; absent uses the deceleration time + 0.5 s. */
  max_stop_time_s?: number;
  /** Ordered fallbacks after `stop_mode`, strictly below it in the fixed order
   * (utils/stopFallbackSequence.ts); hard deceleration always follows and is
   * never listed. Absent is []. */
  fallback_stop_modes?: Array<'spring_back_hold' | 'spring_back_fast'>;
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

export interface DeploymentControlledStopUpdate {
  controllable_component_id: string;
  command_target_id: string;
  /** null keeps the selected target's profile unchanged (including non-motion targets). */
  profile: DeploymentControlledStopProfile | null;
}

export interface CurrentScenarioSelection {
  scenarioId: string;
  sourceDocumentDigest: string;
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
  schemaVersion: 'manyforge.applied_program_selection.v3';
  scenarioId: string;
  sourceDocumentDigest: string;
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
  assistantProjection: Record<string, JsonValue> | null;
  ownerIdentities: ProgramRunOwnerIdentity[];
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

/** Where a failure happened (spec error contract `origin`). */
export interface FailureOrigin {
  kind: 'program_node' | 'owner' | 'program_runner';
  nodeInstanceId: string | null;
  ownerApi: string | null;
}

/** Bounded flat failure details: one scalar value per registered key. */
export type FailureDetails = Record<string, string | number | boolean | null>;

/**
 * One failure as produced at its source: the stable code, the human message
 * and, when carried, its bounded details and origin. Run records and API
 * errors map into this one shape; it is for display only and never selects a
 * recovery action.
 */
export interface StructuredError {
  code: string;
  message: string;
  details: FailureDetails | null;
  origin: FailureOrigin | null;
}

export interface ProgramRunOwnerIdentity {
  ownerRole: string;
  ownerId: string;
  ownerEpoch: string;
}

export interface ProgramRunCapture {
  programRunId: string;
  appliedRecordDigest: string;
  scenarioId: string;
  sourceDocumentDigest: string;
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

/** Exact owner-published operation failure provenance; display/history data only. */
export interface ProgramRunOperationFailure {
  nodeInstanceId: string;
  nodeAttemptId: string;
  operatorFacingRunRevision: number;
  operation: { sourceOwnerEpoch: string; operationId: string; operationRevision: number };
  resourceIds: string[];
  failure: { code: string; message: string };
  recoveryClass: string;
  terminalFacts: Array<{ kind: string; effect: string }>;
}

export interface ProgramRunStatus {
  schemaVersion: 'manyforge.program_run_status.v3';
  ownerId: string;
  ownerEpoch: string;
  runRevision: number;
  state: ProgramRunStateName;
  capture: ProgramRunCapture;
  treeStatus: 'invalid' | 'running' | 'success' | 'failure';
  nodeStatusRevision: number;
  nodeStatuses: Record<
    string,
    'idle' | 'running' | 'held' | 'success' | 'failure' | 'interrupted'
  >;
  activeOperationCount: number | null;
  operationFailures: ProgramRunOperationFailure[];
  /**
   * Why the run is in its state (a failure, a Stop or a pause), null when the
   * owner names no reason. Its message is the status `message`.
   */
  reason: StructuredError | null;
  message: string;
}

/** The immutable selectors accepted with one Program Reset intent. */
export interface ProgramResetSelection {
  reloadInitialScene: boolean;
  resetRobotPosition: boolean;
}

export interface ProgramResetRobotPositionAvailability {
  available: boolean;
  reason: string | null;
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
  resetOptions: ProgramResetSelection | null;
  resetRobotPosition: ProgramResetRobotPositionAvailability;
  allowedCommands: Array<{
    commandKind: string;
    semanticTarget: Record<string, JsonValue>;
    allowed: boolean;
    disabledReason: string | null;
  }>;
  run: ProgramRunStatus | null;
  runtimeControls: ProgramRuntimeControlProjection | null;
}

/** The value types a formula can produce (docs/authoring/EXPRESSIONS.md §3). */
export type ExpressionValueType = 'bool' | 'int64' | 'float64' | 'string';

export interface ExpressionCheckRequest {
  expression: string;
  destination_type: ExpressionValueType;
  /** The computed key being edited, so its own formula is not a cycle. */
  exclude_computed_key: string | null;
}

/** One backend finding; position and length count Unicode code points. */
export interface ExpressionDiagnostic {
  code: string;
  message: string;
  position: number | null;
  length: number | null;
  name: string | null;
}

export interface ExpressionCheckResult {
  resultType: ExpressionValueType | null;
  diagnostics: ExpressionDiagnostic[];
  /** Computed from parameter defaults and key `initial:` values, never a runtime value. */
  preview: { value: Exclude<JsonValue, null> } | { unavailableReason: string };
  /** `param.` and `key.` names a formula may use, with their types. */
  names: Array<{ name: string; type: string }>;
}

/** One declared persistent key of the Applied Program and its stored value. */
export interface PersistentKeyValue {
  key: string;
  type: string;
  access: BlackboardKeyDeclaration['access'];
  /** Undefined for a key without `initial:`; it then starts empty. */
  initial: Exclude<JsonValue, null> | undefined;
  /** Whether the Program runner holds a stored value for the key. */
  stored: boolean;
  value: JsonValue;
  sequence: number | null;
}

/**
 * One attributed change: an operator request, a write from a run's node, a
 * value dropped because the committed Program no longer keeps it, or a fault
 * (a committed Apply the Program runner never observed).
 */
export interface PersistentKeyValueEdit {
  sequence: number;
  /** Null when a change covers every key of the Scenario. */
  key: string | null;
  action: 'set' | 'reset' | 'clear' | 'dropped' | 'fault';
  requestId: string | null;
  sourceLabel: string;
  programRunId: string | null;
  nodeInstanceId: string | null;
  nodeAttemptId: string | null;
  /** Why a value was dropped, or the fault (`applied_program_not_observed`). */
  reason: string | null;
  detail: string | null;
}

/** Stored persistent values of one Scenario, owned by the Program runner. */
export interface PersistentKeyValues {
  scenarioId: string;
  programRevision: string;
  /** The live run that currently holds the values, if any. */
  programRunId: string | null;
  /** Register sequence when read: advances with every stored-value change. */
  sequence: number;
  keys: PersistentKeyValue[];
  edits: PersistentKeyValueEdit[];
  /**
   * The stored values could not be brought in line with the committed Apply;
   * until they are, no stored value is shown or restored.
   */
  storageFault: { code: string; message: string } | null;
  /**
   * Committed Applies of this Scenario the Program runner never observed.
   * Values were kept and checked against the current Program; the missed
   * notification is a defect to find and fix.
   */
  applyObservationFault: { code: string; recordDigests: string[] } | null;
}

export interface PersistentKeyValuesCleared {
  requestId: string;
  scenarioId: string;
  removed: number;
}

/** The facts of one automatic motion retry, parsed strictly from its audit event. */
export interface AutomaticRetryAuditFacts {
  nodeInstanceId: string;
  /** 1-based attempt that is starting; the first retry is 2. */
  attempt: number;
  maxAttempts: number;
  /** The cause as the runner names it, for example collision_stop_nominal. */
  cause: string;
  delayS: number;
  /** The failed attempt's Kernel failure; null when the owner has none. */
  failure: StructuredError | null;
}

export interface AutomaticRetryAuditEvent {
  eventRevision: number;
  kind: 'program_node_automatic_retry';
  facts: AutomaticRetryAuditFacts;
}

/**
 * A motion takeover that did not complete as a moving replacement: the
 * newcomer node's Kernel decision, parsed strictly from its audit event.
 */
export interface TakeoverFallbackAuditFacts {
  /** The newcomer node whose takeover fell back. */
  nodeInstanceId: string;
  incumbentOperationId: string;
  path: 'spring_back_hold' | 'ordinary_containment';
  /** The Kernel's reason; null when it named none. */
  code: string | null;
  message: string | null;
  /** The stage where the preparation time ran out; null when not named. */
  failureStage: string | null;
  preparationMs: number;
  budgetMs: number;
}

export interface TakeoverFallbackAuditEvent {
  eventRevision: number;
  kind: 'program_node_takeover_fallback';
  facts: TakeoverFallbackAuditFacts;
}

/** Every other runtime-control event: its facts are kept as the owner sent them. */
export interface GenericRuntimeControlAuditEvent {
  eventRevision: number;
  kind: string;
  facts: Record<string, JsonValue>;
}

export type RuntimeControlAuditEvent =
  | AutomaticRetryAuditEvent
  | TakeoverFallbackAuditEvent
  | GenericRuntimeControlAuditEvent;

export interface ProgramRuntimeControlsSnapshot {
  schemaVersion: 'manyforge.program_runtime_controls.v3';
  programRunId: string;
  runtimeControlRevision: number;
  recoveryFences: {
    programGeneration: number;
    deploymentGeneration: number;
    sceneAdmissionEpoch: number;
    poseSourceApplyGeneration: number;
  };
  audit: RuntimeControlAuditEvent[];
}

export type CycleRecoveryActionId =
  | 'retry_motion_from_current_state'
  | 'acknowledge_and_continue'
  | 'retry_after_scene_repair'
  | 'replan_from_stopped_state'
  | 'retry_runtime_step'
  | 'continue_with_failure'
  | 'reset_cycle';

export interface CycleRecoveryAction {
  actionId: CycleRecoveryActionId;
  requiresConfirmation: boolean;
  performsBackendReset: boolean;
  enabled: boolean;
  disabledReasonCode: string | null;
}

export interface CycleRecoveryOffer {
  schemaVersion: 'manyforge.cycle_recovery_offer.v4';
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
  /**
   * The held node's own failure (or the Stop that holds it). Several nodes can
   * be held at once (see `recoveryHolds`), so each offer carries its full reason.
   */
  reason: StructuredError;
  evidenceRefs: string[];
  /** The captured node policy; `maxRetries` 0 is Unlimited. */
  policy: {
    allowIntervention: boolean;
    maxRetries: number;
    interventionStopScope?: 'own_groups';
  };
  /** Explicit failure retries of this node already used in the run. */
  retriesUsed: number;
}

export interface ProgramExecutionSelectionControls {
  selectionRevision: number;
  /** Owner proof that every effectful node is held, with no Stop pending or faulted. */
  motionHeld: boolean;
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
    /** Exact resources the Kernel reserves to release this selection. */
    resourceIds: string[];
  }>;
}

export interface ProgramGroupRuntimeScaleState {
  selector: RuntimeScaleControlSelector | null;
  status: RuntimeScaleControlStatus | null;
  draft: number;
  pending: boolean;
  error: string | null;
}

/** One active failure hold: the held node's own motion groups and its offer. */
export interface ProgramRecoveryHold {
  /** The groups this hold owns; `['all']` for a node in no named group. */
  motionGroupIds: string[];
  interventionStopScope: ProgramInterventionStopScope;
  offer: CycleRecoveryOffer;
}

export interface ProgramRuntimeControlProjection {
  schemaVersion: 'manyforge.program_runtime_control_projection.v9';
  ownerId: string;
  ownerEpoch: string;
  programRunId: string;
  runRevision: number;
  runtimeControls: ProgramRuntimeControlsSnapshot;
  /** The focused offer (also covers an operator Stop); Retry/Continue use `recoveryHolds`. */
  recoveryOffer: CycleRecoveryOffer | null;
  /** Every active failure hold, in arrival order. */
  recoveryHolds: ProgramRecoveryHold[];
  executionSelections: ProgramExecutionSelectionControls;
}

export type ProgramWorkspaceTransitionStatus =
  | 'accepted'
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
  schema_version: 'manyforge.scene_mesh_asset.v2';
  asset_hash: string;
  mesh_asset_url: string;
  mesh_asset_status: 'pending' | 'ok' | 'parse_failed';
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
      payload_quality: 'good';
    })
  | (SceneRuntimePoseSourceBase & {
      buffered: false;
      eligible: false;
      fresh: false;
      payload: SceneRuntimeUnknownObservationPayload;
      payload_quality: 'unknown';
    });

export interface SceneRuntimeProjection {
  api_version: 'manyforge.scene_runtime_source.v1';
  cell_id: string;
  state: {
    api_version: 'manyforge.scene_runtime_state.v2';
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
      kind: 'set_collision_policy';
      configured_policy: Record<string, JsonValue>;
      expected_configured_policy_revision: { revision_id: number; content_hash: string };
    }
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
  /** Opt-in composite fence for the temporary standalone Close cleanup. */
  expected_authoring_workspace?: AuthoringWorkspaceIdentity;
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

/**
 * The fence every migrated ordinary authoring request sends.
 *
 * The composite workspace epoch/revision is mandatory, not optional: the
 * backend rechecks it atomically with the Draft fence, so a request that
 * omitted it would either be rejected or would leave a stale edit unguarded.
 * Apply, Revert, runtime Scenario activation and Deployment candidate
 * Preview/Apply/Revert stay on the external path and keep the plain
 * `DraftFenceRequest` shape.
 */
export interface LocalWorkspaceFenceRequest extends DraftFenceRequest {
  expectedWorkspaceEpoch: string;
  expectedWorkspaceRevision: number;
}

export interface ScenarioDocumentFenceRequest extends LocalWorkspaceFenceRequest {
  deploymentDraftId: string;
  expectedDeploymentDraftRevision: number;
}

export interface ManagerDeploymentCandidateReceipt {
  schema_version: 'manyforge.manager_deployment_candidate_receipt.v2';
  deployment_artifact_id: string;
  deployment_revision: string;
  prepared_activation_id: string;
  candidate_retained_bundle_id: string;
  candidate_graph_revision: string;
  expected_current: {
    graph_revision: string;
    retained_bundle_id: string;
  };
  expected_admission_generation: number;
  manager_owner_epoch: string;
  /** Digest of the Manager's authoritative impact; destructive changes or
   * required component bring-up are accepted only by echoing it. */
  impact_digest: string;
  destructive_impact: boolean;
  receipt_digest: string;
}

/** One component's runtime decision in a Deployment Apply (H1a). */
export interface ManagerDeploymentImpactComponent {
  controllable_component_id: string;
  disposition: 'retain' | 'add' | 'remove' | 'replace';
  replacement_reasons: string[];
}

/** One authored Program role that no longer resolves in the candidate. */
export interface ProgramUnresolvedReference {
  component_role_id: string;
  target_role_id: string | null;
  controllable_component_id: string;
  command_target_id: string | null;
  reason: string;
  detail: string;
}

/** The Applied Program's derived status (H1a). A non-executable Program is
 * preserved as authored; the backend refuses Execute by these references. */
export interface AppliedProgramStatus {
  deploymentArtifactId: string;
  deploymentRevision: string;
  executable: boolean;
  programId: string | null;
  programRevision: string | null;
  origin: PreparedActivationOrigin;
  message: string | null;
  unresolvedReferences: ProgramUnresolvedReference[];
  affectedNodes: string[];
  affectedExecutionGroups: string[];
}

/** Derived, never stored: the Manager's one authoritative Apply impact. */
export interface ManagerDeploymentImpact {
  destructive: boolean;
  components: ManagerDeploymentImpactComponent[];
  attachmentChangedComponents: string[];
  /** Kept in the cell, detached at their authored root pose. */
  detachedComponents: string[];
  sharedSceneProjectionChanged: boolean;
  componentDeltaDigest: string;
  program: {
    executable: boolean;
    programId: string | null;
    programRevision: string | null;
    unresolvedReferences: ProgramUnresolvedReference[];
    affectedNodes: string[];
    affectedExecutionGroups: string[];
  };
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
  impact: ManagerDeploymentImpact;
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
  canonicalEmpty: boolean;
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
  sceneInitialization: 'preserve' | 'initialize';
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
  terminalState: 'succeeded' | 'failed' | 'unknown_effect' | null;
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

export type ManagerApplyLifecyclePhase =
  | 'accepted'
  | 'fence_acquired'
  | 'fence_accepted'
  | 'runtime_converged'
  | 'scene_staged'
  | 'scene_activated'
  | 'composed'
  | 'published'
  | 'settled';

export interface ManagerApplyPreparedActivation {
  capturedInputDigest: string;
  deploymentArtifactId: string;
  deploymentRevision: string;
  /** Null exactly when the Apply left the preserved Program non-executable;
   * reported separately from the runtime outcome in `state` (H1a). */
  programBindingId: string | null;
  preparedActivationId: string;
}

export interface ManagerApplyFailure {
  code: string;
  message: string;
  phase: ManagerApplyLifecyclePhase;
  effectDisposition: 'known' | 'unknown';
}

export interface ManagerApplyStatus {
  sceneInitialization: 'preserve' | 'initialize';
  apiVersion: 'manyforge.manager_command_response.v1';
  applyGeneration: number;
  applyId: string;
  candidateProjectionRevision: ProjectionRevision | null;
  error: null;
  failure: ManagerApplyFailure | null;
  lastProgress: {
    phase: ManagerApplyLifecyclePhase;
    recordDigest: string;
  };
  ok: true;
  phase: ManagerApplyLifecyclePhase;
  preparedActivation: ManagerApplyPreparedActivation;
  requestId: string;
  requestOwnerEpoch: string;
  state: 'pending' | 'succeeded' | 'failed' | 'unknown_effect';
}

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

export interface ManagerApplyTerminalFailureResponse {
  apiVersion: 'manyforge.manager_command_response.v1';
  applyGeneration: number;
  applyId: string;
  candidateProjectionRevision: null;
  error: null;
  ok: true;
  replayStatus: ManagerApplyReplayStatus;
  requestId: string;
  requestOwnerEpoch: string;
  state: 'failed' | 'unknown_effect';
}

export type ManagerApplyResponse =
  | ManagerApplyPendingResponse
  | ManagerApplySucceededResponse
  | ManagerApplyTerminalFailureResponse;

/** One proposed value at one parameter value path of the held node. */
export interface ProposedNodeInput {
  path: string;
  value: JsonValue;
}

export interface ProgramNodeCorrectionPreviewRequest {
  schema_version: 'manyforge.program_node_correction_preview_request.v1';
  request_id: string;
  program_run_id: string;
  /** The Program runner's owner epoch of the held run. */
  expected_owner_epoch: string;
  program_id: string;
  program_revision: string;
  program_binding_id: string;
  node_instance_id: string;
  /** The held attempt the proposal corrects. */
  node_attempt_id: string;
  proposed_inputs: ProposedNodeInput[];
}

/**
 * Advisory, effect-free preview of one held motion node with proposed inputs.
 * It grants no Retry, Reset, Draft or Apply authority (D4).
 */
export interface ProgramNodeCorrectionPreview {
  schemaVersion: 'manyforge.program_node_correction_preview.v1';
  requestId: string;
  programRunId: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  nodeInstanceId: string;
  nodeAttemptId: string;
  programRunnerOwnerEpoch: string;
  runRevision: number;
  retainedBundleId: string;
  graphRevision: string;
  nodeId: string | null;
  proposedPaths: string[];
  parametersDigest: string | null;
  target: {
    targetRoleId: string;
    componentRoleId: string;
    controllableComponentId: string;
    commandTargetId: string;
  } | null;
  unavailableReason: { code: string; message: string } | null;
  previewCheck: 'trajectory_planned_and_collision_validated' | null;
  /** The unchanged native motion-preview result, when one was requested. */
  preview:
    | { ok: true; receipt: MotionPreviewReceipt }
    | { ok: false; error: StructuredError }
    | null;
}

/** Composer's bounded log record of one confirmed Apply after Reset. */
export interface ResetApplyContext {
  schemaVersion: 'manyforge.composer_reset_apply_context.v1';
  correlationId: string;
  document: 'program' | 'deployment';
  programRunId: string;
  programRunnerOwnerEpoch: string;
  cycleGeneration: number;
  draftId: string;
  draftRevision: number;
  /** The Program revision or Deployment revision of the confirmed Draft. */
  draftContentRevision: string | null;
  appliedRevision: string | null;
}

export interface DraftMutationResponse {
  mutation: string;
  details: Record<string, JsonValue>;
  workspace: ProgramWorkspaceResponse;
  authoringWorkspace: AuthoringWorkspaceSnapshot;
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
