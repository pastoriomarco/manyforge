// Cycle
export interface CycleStatus {
  state: 'idle' | 'running' | 'stopped' | 'interrupt_handler' | 'direct_control';
  velocityScale: number;
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

export interface InterruptSourceConfig {
  adapter: string;
  signal: string;
  kind: 'edge' | 'level';
  priority: number;
  maskable: boolean;
  description: string;
}

export interface AuthorityDomainConfig {
  owner: string;
  onDisconnect: string;
  description: string;
}

export interface RobotConfig {
  robotId: string;
  groupId: string;
  urdfPath: string;
  srdfPath: string;
  packagePaths: string[];
  globalMaxTcpLinearSpeedMps: number;
}

export interface GripperConfig {
  commandMode: string;
  groupId: string;
  jointName: string;
  openPosition: number;
  closedPosition: number;
  motionTimeS: number;
  description: string;
  touchLinks: string[];
  mountFrame: string;
  attachFrame: string;
  tcpFrame: string;
  auxiliaryFrames: string[];
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
  interruptSources: Record<string, InterruptSourceConfig>;
  authorityMatrix: Record<string, AuthorityDomainConfig>;
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

export interface SceneRobotState {
  robot_id?: string | null;
  group_id?: string | null;
  base_frame: string;
  tcp_frame: string;
  joint_names: string[];
  joint_positions: Record<string, number>;
  tcp_pose_in_base?: Record<string, unknown> | null;
}

export interface SceneObjectState {
  object_id: string;
  shape?: Record<string, unknown> | null;
  pose_in_universe?: Record<string, unknown> | null;
  attached: boolean;
  attached_to?: string | null;
  source: 'authored' | 'runtime' | 'attached' | 'sensed';
  persistable: boolean;
}

export interface SceneStateResponse {
  robot?: SceneRobotState | null;
  objects: SceneObjectState[];
  timestamp_ns: number;
}

export interface SceneEditRequest {
  operation: 'add' | 'upsert' | 'remove' | 'update_pose' | 'attach' | 'detach';
  object_id: string;
  shape_type?: 'box' | 'sphere';
  box_dimensions_m?: number[];
  sphere_radius_m?: number;
  pose_position_m?: number[];
  pose_orientation_xyzw?: number[];
  frame_id?: string;
}

export interface SceneEditResponse {
  success: boolean;
  error?: string | null;
  timestamp_ns: number;
}

export interface SceneFrameState {
  name: string;
  roles: string[];
  viewerVisible: boolean;
  poseAvailable: boolean;
}

export interface SceneFramesResponse {
  frames: SceneFrameState[];
  hmiAvailable: boolean;
  timestampNs: number;
}

export interface SceneFrameVisibilityResponse {
  success: boolean;
  frameName: string;
  visible: boolean;
  hmiAvailable: boolean;
  timestampNs: number;
  error?: string | null;
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
  state: 'idle' | 'running' | 'stopped' | 'interrupt_handler' | 'direct_control';
  velocityScale: number;
  nodeStatuses?: Record<string, string>;
  nativeKernelEvents?: Record<string, unknown>[];
  runtimeOverrideStatus?: RuntimeOverrideStatus | null;
  error?: string | null;
  programLoaded?: boolean;
  programSessionGeneration?: number;
  timestamp: number;
}
