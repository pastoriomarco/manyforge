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
}

export interface ProgramTree {
  tree: TreeNode;
}

// WebSocket
export interface StateUpdate {
  state: 'idle' | 'running' | 'stopped';
  velocityScale: number;
  timestamp: number;
}
