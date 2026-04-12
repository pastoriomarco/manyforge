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
  sourcePath: string;
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

// WebSocket
export interface StateUpdate {
  state: 'idle' | 'running' | 'stopped' | 'direct_control';
  velocityScale: number;
  nodeStatuses?: Record<string, string>;
  programLoaded?: boolean;
  timestamp: number;
}
