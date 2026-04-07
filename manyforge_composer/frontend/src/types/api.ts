// Cycle
export interface CycleStatus {
  state: 'idle' | 'running' | 'stopped';
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
}

// WebSocket
export interface StateUpdate {
  state: 'idle' | 'running' | 'stopped';
  velocityScale: number;
  nodeStatuses?: Record<string, string>;
  programLoaded?: boolean;
  timestamp: number;
}
