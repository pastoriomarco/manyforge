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
  object_axes_gizmo_size_m?: number;
}

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
  model_asset_id: string;
  model_instance_id: string;
  model_revision: string;
  model_role_id: string | null;
  source_model_name: string;
  source_robot_name: string;
  source_link_names: string[];
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
  schema_version: 'manyforge.source_model_projection.v1';
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

export interface LogicalSceneProjection {
  api_version: 'manyforge.logical_scene_source.v1';
  access: {
    active_projection: ProjectionRevision | null;
    phase: string;
    projection_leases_open: boolean;
    scene_writes_open: boolean;
  };
  snapshot: {
    carried_object_attachments: Array<Record<string, JsonValue>>;
    cell_id: string;
    configured_collision_policy: Record<string, JsonValue>;
    objects: LogicalSceneObject[];
    policy_epoch: number;
    revision: SceneRevision;
    topology_epoch: number;
    transaction_sequence: number;
  } | null;
}

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
    pose_sources: Array<Record<string, JsonValue>>;
    reason: string;
    runtime_identity: {
      deployment_generation: number;
      graph_revision: string;
      program_generation: number;
      retained_bundle_id: string;
      runtime_generation: number;
    } | null;
    started_at_ns: number;
    state: 'open' | 'closed' | 'resetting' | 'seeding' | 'replaying' | 'failed' | 'retired';
    updated_at_ns: number;
  };
}

export interface SceneTransactionRequest {
  request_context: {
    request_id: string;
    request_owner_epoch: string;
    source_label: 'composer_ui';
    correlation_id: string | null;
  };
  expected_projection_revision: ProjectionRevision;
  operation: {
    kind: 'update_object_pose';
    scene_resource_id: string;
    pose: {
      position_m: { x_m: number; y_m: number; z_m: number };
      orientation: { x: number; y: number; z: number; w: number };
    };
    frame_ref: { kind: 'cell_frame'; cell_id: string };
    provider_evidence: null;
    expected_attached: false;
  };
}

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
