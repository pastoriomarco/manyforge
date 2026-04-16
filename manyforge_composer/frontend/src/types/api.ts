// Cycle
export interface CycleStatus {
  state: 'idle' | 'running' | 'stopped' | 'direct_control';
  velocityScale: number;
}

export interface CycleCommand {
  state: string;
}

export interface VelocityScaleResponse {
  velocityScale: number;
}

// Catalog
export interface CatalogParameter {
  name: string;
  type: string;
  required: boolean;
  default: unknown;
  enumValues: string[];
  constraints: Record<string, unknown>;
  parameterizable: boolean;
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
  kind: string;
  childrenMin: number;
  childrenMax: number;
  parameters: CatalogParameter[];
  requiredCapabilities: string[];
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
  parameterCount: number;
  parameters: ProgramParameter[];
  blackboard: ProgramBlackboard;
  sourcePath: string;
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

export interface ModelEndpointConfig {
  provider: string;
  modelId: string;
  baseUrl?: string | null;
  imageFetchBaseUrl?: string | null;
  apiKeyEnv?: string | null;
  defaultTimeoutS: number;
  maxTokens: number;
}

export interface PromptTemplateConfig {
  version: string;
  templateText: string;
  outputSchema?: Record<string, unknown> | null;
  responseTool?: Record<string, unknown> | null;
}

export interface RobotConfig {
  robotId: string;
  groupId: string;
  urdfPath: string;
  globalMaxTcpLinearSpeedMps: number;
}

export interface DeploymentInfo {
  name: string;
  description: string;
  programProfile: string;
  robotConfig: RobotConfig;
}

export interface FullDeployment {
  apiVersion: string;
  schemaVersion: string;
  name: string;
  description: string;
  programProfile: string;
  robotConfig: RobotConfig;
  adapters: Record<string, AdapterConfig>;
  orchestrator: OrchestratorConfig;
  launch: LaunchConfig;
  modelAuditPolicy?: ModelAuditPolicy | null;
  modelEndpoints: Record<string, ModelEndpointConfig>;
  promptTemplates: Record<string, PromptTemplateConfig>;
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
}

export interface SceneStateResponse {
  robot?: SceneRobotState | null;
  objects: SceneObjectState[];
  timestamp_ns: number;
}

export interface ModelEndpointProbe {
  alias: string;
  provider: string;
  configuredModelId: string;
  baseUrl?: string | null;
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
  programLoaded?: boolean;
  programSessionGeneration?: number;
  timestamp: number;
}
