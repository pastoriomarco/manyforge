// Cycle
export interface CycleStatus {
  state: 'idle' | 'running' | 'stopped' | 'direct_control';
  velocityScale: number;
  // Structured error surface: empty when no error is active. `errorCode` is
  // a stable machine token (e.g. "validation_collision",
  // "motion_interrupt_backend_stop_failed"); `errorDetails` carries the
  // kernel diagnostics map (collision pair, classification, waypoint index,
  // fault reason code, …).
  error?: string | null;
  errorCode?: string | null;
  errorDetails?: Record<string, unknown> | null;
}

export interface CycleCommand {
  state: string;
}

export interface VelocityScaleResponse {
  velocityScale: number;
}

// Catalog
export interface CatalogEnumOption {
  value: string;
  label?: string | null;
  description: string;
  profilesMin: string;
  requiredCapabilities: string[];
}

export interface CatalogVisibleWhen {
  param: string;
  equals: unknown;
}

export interface CatalogJsonSchema {
  type?: string;
  description?: string;
  properties?: Record<string, CatalogJsonSchema>;
  required?: string[];
  additionalProperties?: boolean | CatalogJsonSchema;
  items?: CatalogJsonSchema;
  enum?: string[];
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
}

export interface CatalogParameter {
  name: string;
  type: string;
  required: boolean;
  default: unknown;
  enumValues: string[];
  enumOptions: CatalogEnumOption[];
  constraints: Record<string, unknown>;
  parameterizable: boolean;
  profilesMin: string;
  visibleWhen?: CatalogVisibleWhen | null;
  uiEditor?: string | null;
  blackboardRole?: string | null;
  blackboardContract?: string | null;
  blackboardValueType?: string | null;
  schema?: CatalogJsonSchema | null;
  schemaHash?: string | null;
  description: string;
}

export interface CatalogEntry {
  id: string;
  name: string;
  category: string;
  description: string;
  stability: 'stable' | 'experimental' | 'deprecated';
  kind: string;
  childrenMin: number;
  childrenMax: number;
  parameters: CatalogParameter[];
  requiredCapabilities: string[];
  profilesMin: string;
}

export interface CatalogResponse {
  version: string;
  entries: CatalogEntry[];
}

// Program
export interface Robot {
  robotId: string;
  groupId: string;
  baseFrame: string;
  tcpFrame: string;
  basePose?: Record<string, unknown>;
}

export interface TreeNode {
  id: string;
  name: string;
  params: Record<string, unknown>;
  modificationPolicy?: Record<string, string>;
  recoveryPolicy?: Record<string, unknown>;
  children: TreeNode[];
}

export interface ProgramInfo {
  name: string;
  description: string;
  programProfile: string;
  robot: Robot;
  sceneCount: number;
  sceneResources: ProgramSceneResource[];
  parameterCount: number;
  parameters: ProgramParameter[];
  blackboard: ProgramBlackboard;
  sourcePath: string;
  programSessionGeneration: number;
  programTreeHash: string;
}

export interface ProgramSceneResource {
  objectId: string;
  shape: Record<string, unknown>;
  pose: Record<string, unknown>;
}

export interface ProgramParameter {
  name: string;
  type: string;
  default: unknown;
  constraints: Record<string, unknown>;
  description: string;
}

export interface BlackboardNamespace {
  id: string;
  contract: string;
  prefix: string;
  description: string;
  fields: BlackboardNamespaceField[];
}

export interface BlackboardNamespaceField {
  name: string;
  type: string;
  description: string;
}

export interface BlackboardKey {
  id: string;
  type: string;
  key: string;
  description: string;
}

export interface ProgramBlackboard {
  namespaces: BlackboardNamespace[];
  keys: BlackboardKey[];
}

export interface ProgramTree {
  tree: TreeNode;
}

export interface ProgramValidationIssue {
  path: string;
  message: string;
}

export interface ProgramValidateResponse {
  valid: boolean;
  errors: ProgramValidationIssue[];
  warnings: ProgramValidationIssue[];
}

export interface RuntimeOverrideEntry {
  nodeName: string;
  params: Record<string, unknown>;
  authoritySource: string;
  approvalMode: string;
  reason: string;
  sessionId?: string | null;
  triggerNodeInstance?: string | null;
  requestedAtNs: number;
  activationState: 'active' | 'staged';
  activatedAtNs?: number | null;
}

export interface RuntimeOverrideStatus {
  active: RuntimeOverrideEntry[];
  staged: RuntimeOverrideEntry[];
  activeCount: number;
  stagedCount: number;
  hasActive: boolean;
  hasStaged: boolean;
  canActivateNow: boolean;
  requiresResetToActivate: boolean;
  currentTreeOrigin: 'saved_program' | 'runtime_override';
  nextEffectiveTree?: TreeNode | null;
  timestampNs: number;
}

// Deployment
export interface AdapterConfig {
  adapterId: string;
  adapterVersion: string;
  config: Record<string, unknown>;
  supportedProfiles: string[];
  capabilityKeys: string[];
}

export interface OrchestratorConfig {
  engine: string;
  config: Record<string, unknown>;
}

export interface LaunchRunner {
  module: string;
  args: string[];
}

export interface LaunchUiBackend {
  module: string;
  args: string[];
  enabled: boolean;
}

export interface LaunchConfig {
  runner: LaunchRunner;
  uiBackend?: LaunchUiBackend;
}

export interface ModelAuditPolicy {
  storeFullPrompts: boolean;
  storeFullResponses: boolean;
  hashAlgorithm: string;
  redactArgumentValues: boolean;
}

export interface ModelEndpointCapabilities {
  supportsResponseTools: boolean;
  supportsJsonObject: boolean;
  supportsImageInput: boolean;
  supportedImageTransports: string[];
  toolCallFamily: string;
  latencyBudgetMs?: number | null;
}

export interface ModelEndpointConfig {
  profileRef?: string;
  provider: string;
  modelId: string;
  baseUrl?: string | null;
  imageFetchBaseUrl?: string | null;
  apiKeyEnv?: string | null;
  defaultTimeoutS: number;
  maxTokens: number;
  capabilities: ModelEndpointCapabilities;
}

export interface AssistantConfig {
  provider: string;
  endpointAlias: string;
  enabled: boolean;
}

export type AssistantToolEffect =
  | 'read_only'
  | 'proposal'
  | 'composer_draft_mutating'
  | 'runtime_mutating';

export interface AssistantToolDefinition {
  id: string;
  effect: AssistantToolEffect;
  description: string;
  [key: string]: unknown;
}

export interface AssistantModeAuthority {
  sessionKind: 'manual_intervention' | 'delegated_recovery' | 'not_applicable';
  approvalMode: 'human_approved' | 'policy_granted';
  review: 'required' | 'bounded_by_recovery_profile' | 'not_required';
}

export interface AssistantModeCatalog {
  skills: string[];
  tools: string[];
  nodes: string[];
}

export interface AssistantModeDefinition {
  enabled: boolean;
  description: string;
  authority: AssistantModeAuthority;
  catalog: AssistantModeCatalog;
}

export interface AssistantToolCall {
  name: string;
  status: 'proposed' | 'skipped' | 'completed' | 'failed';
  arguments: Record<string, unknown>;
  result?: Record<string, unknown> | null;
  error?: string | null;
}

export interface AssistantError {
  code: string;
  detail: string;
}

export interface AssistantChatRequest {
  message: string;
  mode: 'mock' | 'provider';
  requestId?: string;
  providerId?: string;
  conversationId?: string;
  assistantMode?: string;
  context?: Record<string, unknown>;
  requestedTools?: string[];
  fieldPolicy?: Record<string, unknown>;
}

export interface AssistantProposal {
  id: string;
  type: string;
  title: string;
  summary: string;
  status: 'draft';
  reviewRequired: boolean;
  payload: Record<string, unknown>;
  warnings: string[];
  source: Record<string, unknown>;
}

export interface AssistantChatResponse {
  requestId: string;
  conversationId: string;
  mode: 'mock' | 'provider';
  providerId: string;
  assistantMode?: string | null;
  message: string;
  proposals: AssistantProposal[];
  toolCalls: AssistantToolCall[];
  warnings: string[];
  mutated: boolean;
  draftMutated: boolean;
  requiresReview: boolean;
  programLoaded: boolean;
  cycleState: string;
  timestampNs: number;
  error?: AssistantError | null;
}

export interface AssistantProviderDescriptor {
  id: string;
  description: string;
  configured: boolean;
  requiresLiveModel: boolean;
  requiresLiveIsaacSim: boolean;
}

export interface AssistantProviderCatalog {
  providers: AssistantProviderDescriptor[];
  configuredProviderId?: string | null;
}

export interface LiveToolCall {
  toolId: string;
  status: 'running' | 'completed' | 'failed';
  startedAtNs: number;
  finishedAtNs?: number | null;
  argsSummary: string;
  resultSummary: string;
  error?: string | null;
  draftMutated: boolean;
}

export interface AssistantRequestStatus {
  requestId: string;
  conversationId: string;
  providerId: string;
  assistantMode?: string | null;
  status: 'running' | 'cancel_requested' | 'completed' | 'failed' | 'cancelled';
  message: string;
  error?: AssistantError | null;
  warnings: string[];
  draftMutated: boolean;
  toolCallCount: number;
  recentToolCalls?: LiveToolCall[];
  startedAtNs: number;
  updatedAtNs: number;
  finishedAtNs?: number | null;
}

export interface AssistantCancelResponse {
  requestId: string;
  cancelled: boolean;
  status: 'running' | 'cancel_requested' | 'completed' | 'failed' | 'cancelled' | 'unknown';
  detail: string;
  providerResponse: Record<string, unknown>;
  timestampNs: number;
}

export interface PromptTemplateConfig {
  version: string;
  templateText: string;
  outputSchema?: Record<string, unknown> | null;
  responseTool?: Record<string, unknown> | null;
}

export interface AuthorityDomainConfig {
  owner: string;
  onDisconnect: string;
  description: string;
}

export interface RobotControlJointLimit {
  maxVelocityRadS?: number | null;
  maxAccelerationRadS2?: number | null;
  // Phase 4-P5.1K: per-joint jerk cap. Carried so a loaded deployment's jerk is
  // preserved on edit/round-trip; the discrete planner rejects it as
  // unsupported, but the value must not be silently dropped here.
  maxJerkRadS3?: number | null;
}

export type PoseSourceKind =
  | 'simulation_interfaces'
  | 'pose_stamped_topic'
  | 'pose_array_topic'
  | 'detection3d_array_topic'
  | 'tf';

export type PoseSourceApplyPolicy =
  | 'observe_only'
  | 'when_idle'
  | 'continuous'
  | 'continuous_unattached'
  | 'before_planning'
  | 'between_nodes';

export interface PoseSourceObjectBindingConfig {
  entity?: string;
  frameId?: string;
  applyPolicy?: PoseSourceApplyPolicy;
  confidenceMin?: number | null;
  metadata?: Record<string, unknown>;
}

export interface PoseSourceConfig {
  id: string;
  kind: PoseSourceKind;
  enabled: boolean;
  maxUpdateHz: number;
  staleAfterS: number;
  frameId?: string;
  servicePrefix?: string;
  topic?: string;
  description?: string;
  objects: Record<string, PoseSourceObjectBindingConfig>;
}

export interface SimulationControlCapabilitiesConfig {
  reset: boolean;
  pause: boolean;
  play: boolean;
  step: boolean;
  setEntityState: boolean;
}

export interface SimulationResetConfig {
  defaultScope: 'state';
  closeCommandGate: boolean;
  waitReadyS: number;
  resumePlayAfterReset: boolean;
}

export interface SimulationControlConfig {
  kind: 'ros2_simulation_interfaces';
  servicePrefix: string;
  capabilities: SimulationControlCapabilitiesConfig;
  reset?: SimulationResetConfig | null;
}

export interface SimulationResetResponse {
  success: boolean;
  operation: 'reset';
  controlId: string;
  scope: 'state';
  serviceName?: string | null;
  durationS: number;
  commandGateClosed: boolean;
  commandGateReopened: boolean;
  resumePlayAfterReset: boolean;
  resumePlayResult?: Record<string, unknown> | null;
  readiness?: Record<string, unknown> | null;
}

export interface SimulationSetStateRequest {
  state: 'playing' | 'paused';
  reason?: string;
}

export interface SimulationStepRequest {
  steps?: number;
  reason?: string;
}

export interface SimulationSetEntityStateRequest {
  entity: string;
  pose: Record<string, unknown>;
  reason?: string;
}

export interface SimulationControlResponse {
  success: boolean;
  operation: string;
  controlId: string;
  serviceName?: string | null;
  durationS: number;
  resultCode?: number | null;
  resultMessage?: string;
  state?: string | null;
  steps?: number | null;
  entity?: string | null;
}

export interface RobotConfig {
  robotId: string;
  groupId: string;
  urdfPath: string;
  srdfPath: string;
  packagePaths: string[];
  globalMaxTcpLinearSpeedMps: number;
  /** Per-joint validated Robot Control operator caps. Empty = all joints use engine defaults. */
  robotControlJointLimits?: Record<string, RobotControlJointLimit>;
}

// One link in the gripper subtree, posed relative to the gripper
// root at the URDF default joint configuration. The unreachable
// ghost renders the full subtree so the operator sees a recognizable
// gripper silhouette, not just the root cube.
export interface GripperSubtreeLink {
  name: string;
  positionM: number[];
  orientationXyzw: number[];
}

// Auto-discovered Move-To unreachable-ghost visualization metadata
// (see ADR-0018 / `routes_urdf.resolve_gripper_visualization`).
// `gripperRootLink` is the first link reachable downstream of
// `mountFrame` that has visuals; the tcp→root SE(3) places the
// gripper mesh at the right offset from an unreachable TCP target;
// `subtreeLinks` enumerates the full gripper chain so the renderer
// can draw the whole shape, not just the root link.
// Null when the URDF graph walk does not reach a visual link — the
// frontend renders a red-axes marker instead.
export interface GripperVisualization {
  gripperRootLink: string;
  tcpToRootPositionM: number[];
  tcpToRootOrientationXyzw: number[];
  subtreeLinks: GripperSubtreeLink[];
}

export interface GripperConfig {
  commandMode: string;
  groupId: string;
  jointName: string;
  motionTimeS: number;
  srdfPresets: Record<string, number>;
  description: string;
  touchLinks: string[];
  mountFrame: string;
  attachFrame: string;
  tcpFrame: string;
  auxiliaryFrames: string[];
  visualization?: GripperVisualization | null;
}

export interface DeploymentInfo {
  name: string;
  description: string;
  programProfile: string;
  robotConfig: RobotConfig;
  grippers: Record<string, GripperConfig>;
}

export interface FullDeployment {
  apiVersion: string;
  schemaVersion: string;
  name: string;
  description: string;
  programProfile: string;
  robotConfig: RobotConfig;
  grippers: Record<string, GripperConfig>;
  adapters: Record<string, AdapterConfig>;
  orchestrator: OrchestratorConfig;
  launch: LaunchConfig;
  modelAuditPolicy?: ModelAuditPolicy | null;
  modelEndpointProfileRegistry?: string | null;
  modelEndpoints: Record<string, ModelEndpointConfig>;
  assistant?: AssistantConfig | null;
  assistantModes: Record<string, AssistantModeDefinition>;
  toolCatalog: AssistantToolDefinition[];
  promptTemplates: Record<string, PromptTemplateConfig>;
  authorityMatrix: Record<string, AuthorityDomainConfig>;
  poseSources: PoseSourceConfig[];
  simulationControl?: SimulationControlConfig | null;
  sourcePath: string;
}

export interface ApplySessionResponse {
  status: string;
}

// Filesystem browser
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

// Infrastructure
export interface InfraComponentStatus {
  status: string;
  port?: number | null;
  [key: string]: unknown;
}

export interface InfraStatus {
  status: string;
  components: Record<string, InfraComponentStatus>;
  timestamp: number;
}

export type RuntimeReadiness = 'ready' | 'waiting' | 'degraded' | 'fault' | 'offline';

export interface RuntimeComponentStatus {
  id: string;
  label: string;
  kind: string;
  status: RuntimeReadiness | 'disabled' | string;
  requiredFor: string[];
  detail?: string;
  [key: string]: unknown;
}

export interface RuntimeStatus {
  schemaVersion: string;
  profileId: string;
  profileKind: 'composer_only' | 'commanded_sim' | 'real_robot' | string;
  profileLabel: string;
  authority: 'backend_authored' | 'controller_observed' | string;
  readiness: RuntimeReadiness | string;
  summaryLabel: string;
  canRender: boolean;
  canPlan: boolean;
  canExecute: boolean;
  userMessage: string;
  components: RuntimeComponentStatus[];
  externalProviders: string[];
  supportedStopModes?: string[] | null;
  configuredStopModes?: Record<string, string>;
  configuredStopDecelerationTimes?: Record<string, number>;
  stopCapabilityProfileId?: string | null;
  hardStopLevel?: string | null;
  derivativeProofLevel?: string | null;
  scalingSourceVisibility?: string | null;
  scalingSourceProbeStatus?: string | null;
  stopProfileUnsupportedReasons?: Array<Record<string, unknown>>;
  backendUnsupportedStopReasons?: Record<string, string>;
  stopProfileUnsupported?: boolean;
  stopCapabilityQueryFailed?: boolean;
  timestampNs: number;
}

export interface SceneRobotState {
  robot_id?: string | null;
  group_id?: string | null;
  urdf_model_key?: string | null;
  base_frame: string;
  tcp_frame: string;
  base_pose?: Record<string, unknown>;
  joint_names: string[];
  joint_positions: Record<string, number>;
  tcp_pose_in_base?: Record<string, unknown> | null;
  state_source?: string | null;
  source_timestamp_ns?: number | null;
  source_age_ns?: number | null;
}

export type MeshAssetStatus =
  | 'ok'
  | 'pending'
  | 'missing'
  | 'unsupported_format'
  | 'parse_failed'
  | 'conversion_timeout'
  | 'too_large'
  | 'outside_allowed_roots';

export interface SceneShapeState {
  type?: string;
  box_dimensions_m?: number[];
  sphere_radius_m?: number;
  cylinder_radius_m?: number;
  cylinder_height_m?: number;
  mesh_resource_uri?: string;
  mesh_scale?: number[];
  mesh_asset_url?: string | null;
  mesh_asset_status?: MeshAssetStatus | string | null;
}

export interface SceneObjectState {
  object_id: string;
  shape?: SceneShapeState | null;
  pose_in_universe?: Record<string, unknown> | null;
  attached: boolean;
  attached_to?: string | null;
  source: 'authored' | 'runtime' | 'attached' | 'sensed';
  persistable: boolean;
  viewer_frame_visible: boolean;
}

export interface SceneGripperSubtreeLink {
  name: string;
  position_m: number[];
  orientation_xyzw: number[];
}

// Snake_case mirror of GripperVisualization, surfaced on the
// scene-state gripper block so the Move-To ghost renderer can
// consume it without an extra deployment fetch.
export interface SceneGripperVisualization {
  gripper_root_link: string;
  tcp_to_root_position_m: number[];
  tcp_to_root_orientation_xyzw: number[];
  subtree_links: SceneGripperSubtreeLink[];
}

export interface SceneGripperState {
  gripper_id: string;
  joint_name: string;
  state: string;
  current_position: number | null;
  target_position: number | null;
  limit_lower: number | null;
  limit_upper: number | null;
  motion_time_s: number;
  srdf_presets: Record<string, number>;
  visualization?: SceneGripperVisualization | null;
  error: string | null;
}

export interface PoseSourcePoseState {
  frame_id: string;
  stamp_sec?: number | null;
  position_xyz: number[];
  orientation_xyzw: number[];
}

export interface PoseSourceUpdateState {
  pose_source_id: string;
  object_id: string;
  entity: string;
  monotonic_s: number;
  apply_policy: string;
  pose: PoseSourcePoseState;
}

export interface PoseSourceRejectionState {
  pose_source_id: string;
  object_id: string;
  entity: string;
  reason: string;
  detail: string;
  monotonic_s: number;
}

export interface PoseSourceObjectState {
  object_id: string;
  entity: string;
  frame_id: string;
  apply_policy: string;
  status: 'disabled' | 'fresh' | 'stale' | 'rejected' | string;
  age_s?: number | null;
  update?: PoseSourceUpdateState | null;
  rejection?: PoseSourceRejectionState | null;
}

export interface PoseSourceStatusState {
  pose_source_id: string;
  kind: string;
  enabled: boolean;
  status: 'disabled' | 'fresh' | 'stale' | 'rejected' | string;
  max_update_hz: number;
  stale_after_s: number;
  frame_id: string;
  description: string;
  received_at_ns?: number | null;
  objects: PoseSourceObjectState[];
}

export interface PoseSourceApplyResponse {
  policy: string;
  success: boolean;
  error?: string | null;
  applied_count: number;
  skipped_count: number;
  rejected_count: number;
  details: Record<string, unknown>[];
}

export interface VisualStateRouteDiagnostics {
  mode: 'visual_snapshot' | 'visual_snapshot_pending' | string;
  served_from_visual_snapshot: boolean;
  fallback_reason?: string | null;
  total_ms: number;
  timings_ms: Record<string, number>;
  slow_steps: Record<string, number>;
  objects_source?: 'visual_snapshot' | 'visual_snapshot_pending' | 'visual_cache_pending' | string;
  objects_age_ms?: number | null;
  objects_refresh_running?: boolean;
  objects_refresh_error?: string | null;
  snapshot_age_ms?: number | null;
  snapshot_sequence_number?: number | null;
  snapshot_build_ms?: number | null;
  snapshot_publisher_running?: boolean;
  snapshot_publisher_error?: string | null;
  snapshot_publisher_error_detail?: string | null;
  snapshot_stale?: boolean;
}

export interface VisualSceneSnapshot {
  schema_version: string;
  sequence_number: number;
  timestamp_ns: number;
  source: string;
  world_frame: string;
  model_key?: string | null;
  stale: boolean;
  robot?: SceneRobotState | null;
  objects: SceneObjectState[];
  link_poses: Record<string, { position_m: number[]; orientation_xyzw: number[] }>;
  grippers: SceneGripperState[];
  pose_sources: PoseSourceStatusState[];
}

export interface SceneStateResponse {
  robot?: SceneRobotState | null;
  objects: SceneObjectState[];
  link_poses?: Record<string, { position_m: number[]; orientation_xyzw: number[] }>;
  grippers?: SceneGripperState[];
  pose_sources?: PoseSourceStatusState[];
  visual_state?: VisualSceneSnapshot | null;
  visual_state_diagnostics?: VisualStateRouteDiagnostics | null;
  object_axes_gizmo_size_m?: number;
  timestamp_ns: number;
}

export type SceneUrdfPrimitive =
  | { type: 'box'; size_m: number[] }
  | { type: 'cylinder'; radius_m: number; length_m: number }
  | { type: 'sphere'; radius_m: number };

export interface SceneUrdfVisual {
  index: number;
  mesh_url: string | null;
  primitive: SceneUrdfPrimitive | null;
  visual_origin: {
    position_m: number[];
    orientation_rpy_rad: number[];
    orientation_xyzw?: number[];
  };
}

export interface SceneUrdfLink {
  name: string;
  visuals: SceneUrdfVisual[];
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
  axis: number[];
  parent_link: string;
  child_link: string;
  limit_lower: number | null;
  limit_upper: number | null;
  effort_limit: number | null;
  velocity_limit: number | null;
  mimic?: { master: string; multiplier: number; offset: number } | null;
  is_mimic_master?: boolean;
}

export interface SceneUrdfManifest {
  robot_id: string | null;
  model_key?: string | null;
  root_link: string;
  links: SceneUrdfLink[];
  joints?: SceneUrdfJoint[];
}

export interface SceneEditRequest {
  operation: 'add' | 'upsert' | 'remove' | 'update_pose' | 'attach' | 'detach';
  object_id: string;
  shape_type?: 'box' | 'sphere' | 'cylinder' | 'mesh';
  box_dimensions_m?: number[];
  sphere_radius_m?: number;
  cylinder_radius_m?: number;
  cylinder_height_m?: number;
  mesh_resource_uri?: string;
  mesh_scale?: number[];
  pose_position_m?: number[];
  pose_orientation_xyzw?: number[];
  frame_id?: string;
}

export interface SceneEditResponse {
  success: boolean;
  error?: string | null;
  timestamp_ns: number;
}

export interface SceneAuthoredSyncRequest {
  programSessionGeneration: number;
  programTreeHash?: string | null;
  sceneSignature: string;
  sceneResources: ProgramSceneResource[];
}

export interface SceneAuthoredSyncResponse {
  success: boolean;
  syncId: string;
  programSessionGeneration: number;
  programTreeHash: string;
  sceneSignature: string;
  added: string[];
  updated: string[];
  removed: string[];
  unchanged: string[];
  failedObjectId?: string | null;
  error?: string | null;
  visualRefreshRequested: boolean;
  visualPublishRequested: boolean;
  startedNs: number;
  completedNs: number;
}

/**
 * Ghost-robot preview request — see /api/scene/preview-link-poses.
 * `kind: 'joint'` uses joint_names + joint_positions_rad directly;
 * `kind: 'cartesian'` runs IK against tcp_pose first (the seed
 * joint_names + joint_positions_rad are optional; if absent the
 * kernel uses the current robot state as seed).
 */
export interface PreviewLinkPosesRequest {
  kind: 'cartesian' | 'joint';
  base_frame?: string;
  link_names?: string[];
  joint_names?: string[];
  joint_positions_rad?: number[];
  tcp_frame?: string;
  tcp_position_m?: [number, number, number];
  tcp_orientation_xyzw?: [number, number, number, number];
}

export type PreviewLinkPosesResponse =
  | {
      reachable: true;
      joint_names: string[];
      joint_positions_rad: number[];
      link_poses: Record<string, { position_m: number[]; orientation_xyzw: number[] }>;
    }
  | {
      reachable: false;
      reason: string;
      tcp_pose?: { position_m: number[]; orientation_xyzw: number[] };
    };

export interface SceneFrameState {
  name: string;
  roles: string[];
  viewerVisible: boolean;
  poseAvailable: boolean;
  poseInWorld?: { position_m: number[]; orientation_xyzw: number[] } | null;
}

export interface SceneFramesResponse {
  frames: SceneFrameState[];
  viewerAvailable: boolean;
  frameGizmoSizeM: number;
  visualStateDiagnostics?: VisualStateRouteDiagnostics | null;
  timestampNs: number;
}

export interface SceneFrameVisibilityResponse {
  success: boolean;
  frameName: string;
  visible: boolean;
  viewerAvailable: boolean;
  timestampNs: number;
  error?: string | null;
}

export interface SceneFrameGizmoSizeResponse {
  success: boolean;
  axesLengthM: number;
  viewerAvailable: boolean;
  timestampNs: number;
  error?: string | null;
}

export interface SceneObjectFrameVisibilityResponse {
  success: boolean;
  objectId: string;
  visible: boolean;
  viewerAvailable: boolean;
  timestampNs: number;
  error?: string | null;
}

export interface SceneObjectAxesGizmoSizeResponse {
  success: boolean;
  axesLengthM: number;
  timestampNs: number;
  error?: string | null;
}

export interface SceneGripperCommandResponse {
  accepted: boolean;
  gripperId: string;
  targetPosition: number;
  error?: string | null;
  timestampNs: number;
}

export interface ModelEndpointProbe {
  alias: string;
  provider: string;
  configuredModelId: string;
  baseUrl?: string | null;
  capabilities: ModelEndpointCapabilities;
  probeStatus: string;
  availableModelIds: string[];
  error?: string | null;
}

export interface ModelEndpointCatalog {
  endpoints: Record<string, ModelEndpointProbe>;
  timestamp: number;
}

// WebSocket
export interface StateUpdate {
  state: 'idle' | 'running' | 'stopped' | 'direct_control';
  velocityScale: number;
  nodeStatuses?: Record<string, string>;
  nativeKernelEvents?: Record<string, unknown>[];
  runtimeOverrideStatus?: RuntimeOverrideStatus | null;
  error?: string | null;
  // Structured cycle-manager error surface pushed alongside the human-
  // readable message so the viewer can render a rich cause panel without
  // a separate poll.
  errorCode?: string | null;
  errorDetails?: Record<string, unknown> | null;
  programLoaded?: boolean;
  programSessionGeneration?: number;
  timestamp: number;
}

// --- Validated Robot Control candidates (Phase 4-P5) ----------------------

export type CandidateStatus =
  | 'planning'
  | 'ready'
  | 'blocked'
  | 'stale'
  | 'superseded'
  | 'expired'
  | 'approving'
  | 'executing'
  | 'completed'
  | 'stopped'
  | 'interrupted'
  | 'failed'
  | 'reverted'
  | 'error';

export type CandidateSourceSurface =
  | 'joint_slider'
  | 'joint_spinner'
  | 'joint_input'
  | 'cartesian_gizmo'
  | 'cartesian_input'
  | 'move_to'
  | 'gripper_slider'
  | 'gripper_preset';

export type CandidateInputMode = 'joint' | 'cartesian' | 'gripper';

export interface CandidatePose {
  positionM: number[];
  orientationXyzw: number[];
}

export interface CandidateJointTarget {
  jointNames: string[];
  positionsRad: number[];
}

export interface CandidateCartesianTarget {
  positionM: number[];
  orientationXyzw: number[];
  frameId?: string;
  tcpFrame?: string | null;
}

export interface CandidateGripperTarget {
  gripperId: string;
  targetPosition: number;
  maxEffort?: number | null;
  durationS?: number | null;
}

export interface CandidateDiagnostics {
  reasonCode: string;
  summary: string;
  details?: Record<string, unknown>;
}

export interface CandidateRobotState {
  jointNames: string[];
  positionsRad: number[];
}

export interface CandidateGhost {
  blocked: boolean;
  finalJointNames?: string[] | null;
  finalJointPositionsRad?: number[] | null;
  finalTcpPose?: CandidatePose | null;
  linkPoses?: Record<string, CandidatePose> | null;
  blockedTargetPose?: CandidatePose | null;
}

export interface CandidateTrajectorySample {
  timeFromStartS: number;
  positionsRad: number[];
}

export interface CandidatePreviewTrajectory {
  jointNames: string[];
  samples: CandidateTrajectorySample[];
  sampleCount: number;
  timeSpanS: number;
}

export interface CandidateSourceRevisions {
  stateSource: string;
  robotStateKnown: boolean;
  programLoaded: boolean;
  deploymentLoaded: boolean;
  // Scene/attached-object freshness signal (see backend CandidateSourceRevisions).
  // sceneFingerprint is opaque — compare only, never parse.
  sceneObjectCount: number;
  sceneFingerprint: string;
}

export interface CandidateExecution {
  moveId?: string | null;
  executionId: number;
  state?: string | null;
  progress: number;
  sessionId?: string | null;
  sessionStatus?: string | null;
  sessionActualOutcome?: string | null;
  sessionError?: string | null;
  error?: string | null;
  errorCode?: string | null;
  errorDetails?: Record<string, unknown> | null;
}

export interface CandidateResponse {
  candidateId: string;
  generation: number;
  sourceSurface: CandidateSourceSurface;
  inputMode: CandidateInputMode;
  status: CandidateStatus;
  commitEligible: boolean;
  jointTarget?: CandidateJointTarget | null;
  cartesianTarget?: CandidateCartesianTarget | null;
  gripperTarget?: CandidateGripperTarget | null;
  normalizedJointTarget?: CandidateJointTarget | null;
  clientRequestId?: string | null;
  diagnostics?: CandidateDiagnostics | null;
  startState?: CandidateRobotState | null;
  sourceRevisions?: CandidateSourceRevisions | null;
  ghost?: CandidateGhost | null;
  previewTrajectory?: CandidatePreviewTrajectory | null;
  execution?: CandidateExecution | null;
  trajectoryDurationS?: number | null;
  maxAgeNs: number;
  expiresAtNs: number;
  createdAtNs: number;
  updatedAtNs: number;
}

export interface CandidateCreateRequest {
  clientRequestId?: string;
  sourceSurface: CandidateSourceSurface;
  inputMode: CandidateInputMode;
  jointTarget?: CandidateJointTarget;
  cartesianTarget?: CandidateCartesianTarget;
  gripperTarget?: CandidateGripperTarget;
}

export interface CandidateSessionSummary {
  sessionId: string;
  status: string;
  entryState?: string | null;
  actualOutcome?: string | null;
  error?: string | null;
}

export interface CandidateExecuteResponse {
  candidate: CandidateResponse | null;
  // Arm candidates dispatch through an intervention session; gripper candidates
  // dispatch a single command through the bridge and carry no session.
  session: CandidateSessionSummary | null;
  timestamp_ns: number;
}

export interface CandidateExecuteRejection {
  reasonCode: string;
  detail: string;
  candidate: CandidateResponse | null;
  timestamp_ns: number;
}
