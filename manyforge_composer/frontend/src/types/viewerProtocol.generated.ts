// THIS FILE IS GENERATED. DO NOT EDIT.
// Source: manyforge_composer/backend/models.py viewer v1 wire models.
// Regenerate: python3 scripts/regen-public-contracts.py

export interface PoseSourceAttachedDriftStateResponse {
  status: string;
  action?: string;
  signal_id?: string;
  baseline_captured?: boolean;
  translation_m?: number | null;
  rotation_rad?: number | null;
  translation_threshold_m?: number | null;
  rotation_threshold_rad?: number | null;
  debounce_s?: number;
  sample_time_delta_s?: number | null;
  unverifiable_reason?: string;
  exceeded_since_ns?: number | null;
  condition_raised?: boolean;
  metadata?: Record<string, unknown>;
}

export interface PoseSourceMeasuredRobotResponse {
  joint_state_topic: string;
  joint_names: Array<string>;
  joint_positions_rad: Array<number>;
  monotonic_s: number;
  age_s?: number | null;
  stamp_sec?: number | null;
  link_poses?: Record<string, Record<string, unknown>>;
}

export interface PoseSourceMeasuredTcpResponse {
  frame_id?: string;
  tcp_frame_id?: string;
  monotonic_s: number;
  age_s?: number | null;
  pose: PoseSourcePoseResponse;
}

export interface PoseSourcePoseResponse {
  frame_id: string;
  stamp_sec?: number | null;
  position_xyz: Array<number>;
  orientation_xyzw: Array<number>;
}

export interface PoseSourceRejectionResponse {
  pose_source_id: string;
  object_id: string;
  entity?: string;
  reason: string;
  detail?: string;
  monotonic_s: number;
}

export interface PoseSourceUpdateResponse {
  pose_source_id: string;
  object_id: string;
  entity?: string;
  monotonic_s: number;
  apply_policy?: string;
  pose: PoseSourcePoseResponse;
}

export interface ViewerClientDiagnosticsControlV1 {
  type: "client_diagnostics";
  schema_version: "manyforge.viewer.control.v1";
  request_id?: string | null;
  visibility: "focused" | "visible" | "collapsed" | "hidden";
  window_duration_ms: number;
  websocket_messages: number;
  websocket_bytes: number;
  manifest_messages: number;
  frame_messages: number;
  dropped_frame_gaps: number;
  resync_requests: number;
  materializations: number;
  materialization_total_ms: number;
  materialization_max_ms: number;
  react_commits: number;
  react_commit_total_ms: number;
  react_commit_max_ms: number;
  rendered_frames: number;
  draw_calls_max: number;
  triangles_max: number;
  canvas_pixels_max: number;
  render_interval_p95_ms?: number | null;
  render_interval_max_ms?: number | null;
  long_task_supported: boolean;
  long_tasks: number;
  long_task_total_ms: number;
  long_task_max_ms: number;
  heap_used_bytes?: number | null;
  topology_rebuilds: number;
  transform_applies: number;
  renderer_vendor?: string | null;
  renderer_name?: string | null;
  renderer_software: boolean;
  effective_dpr: number;
  requested_rate_hz: number;
}

export interface ViewerDiagnosticsV1 {
  type: "diagnostics";
  schema_version: "manyforge.viewer.diagnostics.v1";
  producer_epoch: string;
  timestamp_ns: number;
  worker_epoch?: string | null;
  producer_healthy?: boolean;
  producer_health_reason?: string | null;
  publisher_running?: boolean;
  worker_alive?: boolean;
  last_publication_age_ms?: number | null;
  worker_restarts?: number;
  worker_errors?: number;
  demand_client_count?: number;
  active_client_count?: number;
  build_stage_durations_ms?: Record<string, number>;
  input_replacements?: number;
  fanout_replacements?: number;
  fanout_errors?: number;
  unavailable_layers?: Array<string>;
  last_error?: string | null;
}

export interface ViewerEntityManifestV1 {
  object_id: string;
  shape: ViewerShapeV1 | null;
  attached: boolean;
  attached_to: string | null;
  source: "authored" | "runtime" | "attached";
  persistable: boolean;
  viewer_frame_visible: boolean;
  support_contacts: Array<Record<string, unknown>>;
}

export interface ViewerFrameManifestV1 {
  name: string;
  roles: Array<string>;
  viewer_visible: boolean;
}

export interface ViewerFrameV1 {
  type: "frame";
  schema_version: "manyforge.viewer.frame.v1";
  producer_epoch: string;
  manifest_revision: number;
  frame_sequence: number;
  render_revision: number;
  timestamp_ns: number;
  source_sequence: number;
  stale: boolean;
  robot_blocks: Array<ViewerRobotFrameV1>;
  entity_transforms: Array<ViewerPoseV1 | null>;
  frame_transforms: Array<ViewerPoseV1 | null>;
  gripper_states: Array<ViewerGripperFrameV1>;
  pose_source_states: Array<ViewerPoseSourceFrameV1>;
}

export interface ViewerGripperFrameV1 {
  gripper_id: string;
  joint_name: string;
  derived_joints: Array<string>;
  state: string;
  command_id: number;
  generation: number;
  revision: number;
  current_position: number | null;
  target_position: number | null;
  commanded_position: number | null;
  observed_position: number | null;
  commanded_state: string;
  actuation_kind: string;
  state_authority: string;
  completion_policy: string;
  grasp_confirmation_policy: string;
  transport_kind: string;
  evidence: Array<string>;
  evidence_state: string;
  projection_state: string;
  projection_reason: string;
  limit_lower: number | null;
  limit_upper: number | null;
  motion_time_s: number;
  srdf_presets: Record<string, number>;
  visualization: ViewerGripperVisualizationV1 | null;
  error: string | null;
  reason_code: string | null;
}

export interface ViewerGripperManifestV1 {
  gripper_id: string;
  joint_name: string;
  derived_joints: Array<string>;
  actuation_kind: string;
  state_authority: string;
  completion_policy: string;
  grasp_confirmation_policy: string;
  transport_kind: string;
  limit_lower: number | null;
  limit_upper: number | null;
  motion_time_s: number;
  srdf_presets: Record<string, number>;
  visualization: ViewerGripperVisualizationV1 | null;
}

export interface ViewerGripperSubtreeLinkV1 {
  name: string;
  position_m: Array<number>;
  orientation_xyzw: Array<number>;
}

export interface ViewerGripperVisualizationV1 {
  gripper_root_link: string;
  tcp_to_root_position_m: Array<number>;
  tcp_to_root_orientation_xyzw: Array<number>;
  subtree_links: Array<ViewerGripperSubtreeLinkV1>;
}

export interface ViewerLeaseAckV1 {
  type: "lease_ack";
  schema_version: "manyforge.viewer.diagnostics.v1";
  request_id?: string | null;
  client_id: string;
  timestamp_ns: number;
  expires_at_ns: number;
  lease_revision: number;
  visibility: "focused" | "visible" | "collapsed" | "hidden";
  granted_rate_hz: number;
  granted_layers?: Array<string>;
  server_max_rate_hz: number;
}

export interface ViewerLeaseControlV1 {
  type: "subscribe" | "lease" | "visibility";
  schema_version: "manyforge.viewer.control.v1";
  request_id?: string | null;
  visibility: "focused" | "visible" | "collapsed" | "hidden";
  requested_rate_hz: number;
  layers?: Array<string>;
}

export interface ViewerManifestV1 {
  type: "manifest";
  schema_version: "manyforge.viewer.manifest.v1";
  producer_epoch: string;
  manifest_revision: number;
  content_hash: string;
  world_frame: string;
  robots: Array<ViewerRobotManifestV1>;
  entities: Array<ViewerEntityManifestV1>;
  frames: Array<ViewerFrameManifestV1>;
  grippers: Array<ViewerGripperManifestV1>;
  pose_sources: Array<ViewerPoseSourceManifestV1>;
  orders: ViewerOrdersV1;
  object_axes_gizmo_size_m: number;
  frame_axes_gizmo_size_m: number;
  capabilities: Record<string, boolean>;
}

export interface ViewerOrdersV1 {
  links: Array<string>;
  entities: Array<string>;
  frames: Array<string>;
  grippers: Array<string>;
  pose_sources: Array<string>;
}

export interface ViewerPoseSourceFrameV1 {
  pose_source_id: string;
  status: string;
  received_at_ns: number | null;
  measured_tcp: PoseSourceMeasuredTcpResponse | null;
  measured_robot: PoseSourceMeasuredRobotResponse | null;
  objects: Array<ViewerPoseSourceObjectFrameV1>;
}

export interface ViewerPoseSourceManifestV1 {
  pose_source_id: string;
  kind: string;
  enabled: boolean;
  max_update_hz: number;
  stale_after_s: number;
  frame_id: string;
  description: string;
  objects: Array<ViewerPoseSourceObjectManifestV1>;
}

export interface ViewerPoseSourceObjectFrameV1 {
  object_id: string;
  status: string;
  age_s: number | null;
  update: PoseSourceUpdateResponse | null;
  rejection: PoseSourceRejectionResponse | null;
  attached_drift: PoseSourceAttachedDriftStateResponse | null;
}

export interface ViewerPoseSourceObjectManifestV1 {
  object_id: string;
  entity: string;
  frame_id: string;
  apply_policy: string;
  scene_admission: Record<string, unknown> | null;
  measured_state_ghost: Record<string, unknown> | null;
}

export interface ViewerPoseV1 {
  position_m: Array<number>;
  orientation_xyzw: Array<number>;
}

export interface ViewerResyncControlV1 {
  type: "resync";
  schema_version: "manyforge.viewer.control.v1";
  request_id?: string | null;
}

export interface ViewerRobotFrameV1 {
  robot_id: string | null;
  joint_positions: Array<number | null>;
  link_transforms: Array<ViewerPoseV1 | null>;
  tcp_pose_in_base: Record<string, unknown> | null;
  state_source: string | null;
  source_timestamp_ns: number | null;
  source_age_ns: number | null;
}

export interface ViewerRobotManifestV1 {
  robot_id: string | null;
  group_id: string | null;
  urdf_model_key: string | null;
  base_frame: string;
  tcp_frame: string;
  base_pose: Record<string, unknown>;
  joint_names: Array<string>;
  link_order: Array<string>;
}

export interface ViewerShapeV1 {
  type: string;
  box_dimensions_m: Array<number> | null;
  sphere_radius_m: number | null;
  cylinder_radius_m: number | null;
  cylinder_height_m: number | null;
  mesh_resource_uri: string | null;
  mesh_scale: Array<number> | null;
  mesh_asset_url: string | null;
  mesh_asset_status: string | null;
}

export type ViewerServerMessageV1 =
  | ViewerManifestV1
  | ViewerFrameV1
  | ViewerDiagnosticsV1
  | ViewerLeaseAckV1;

export type ViewerControlMessageV1 =
  | ViewerLeaseControlV1
  | ViewerClientDiagnosticsControlV1
  | ViewerResyncControlV1;
