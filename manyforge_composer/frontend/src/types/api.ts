export type ArtifactKind = 'program' | 'deployment';

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

export interface ProgramValueSource {
  kind: 'program_parameter' | 'blackboard_key' | 'blackboard_field';
  reference: string;
  field?: string;
}

export interface ProgramTreeNode {
  node_id: string;
  instance_id: string;
  parameters: Record<string, JsonValue>;
  value_sources?: Record<string, ProgramValueSource>;
  children: ProgramTreeNode[];
  metadata?: Record<string, JsonValue>;
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
  api_version: 'manyforge.program.v1';
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
  viewer?: Record<string, JsonValue>;
  component_roles: Record<string, JsonValue>;
  target_roles: Record<string, JsonValue>;
  scene?: JsonValue[];
  parameters?: ProgramParameterDeclaration[];
  blackboard?: {
    namespaces: BlackboardNamespaceDeclaration[];
    keys: BlackboardKeyDeclaration[];
  };
  contact_policies?: JsonValue[];
  tree: ProgramTreeNode;
}

export interface ProgramWorkspaceResponse {
  schemaVersion: 'manyforge.composer_program_workspace.v1';
  draft: {
    draftId: string;
    draftRevision: number;
    programLoaded: boolean;
    programRevision: string | null;
    program: ProgramDocument | null;
  };
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

export interface DraftFenceRequest {
  draftId: string;
  expectedDraftRevision: number;
}

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

export interface AuthoredNeutralTransform {
  relative_to: string;
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

export interface SceneUrdfLink {
  name: string;
  neutral_transform: AuthoredNeutralTransform;
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

export interface AuthoredNeutralProjection {
  kind: 'authored_neutral';
  source: 'authored_urdf';
  root_frame: string;
  position_unit: 'm';
  angle_unit: 'rad';
  quaternion_order: 'xyzw';
  joint_coordinate_policy: 'zero_independent_with_authored_mimic';
  operational_validity: 'not_asserted';
  runtime_state_used: false;
}

export interface SceneUrdfManifest {
  robot_id: string | null;
  model_key: string;
  root_link: string;
  projection: AuthoredNeutralProjection;
  links: SceneUrdfLink[];
  joints: SceneUrdfJoint[];
}
