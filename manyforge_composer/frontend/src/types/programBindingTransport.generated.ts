/* GENERATED FILE -- program_binding_transport.v1.json; DO NOT EDIT. */

export type CatalogRecordKind = 'model_asset' | 'runtime_profile' | 'component_type' | 'control_interface_contract' | 'controller_topology' | 'provider_manifest' | 'command_schema' | 'capability_definition' | 'contact_policy' | 'stop_evidence_policy';
export type ProgramOperatorStopPolicy = 'inherit' | 'hold' | 'continue';
export type ProgramInterventionStopScope = 'all' | 'own_groups';
export interface ProgramBindingDocument {
  api_version: 'manyforge.program_binding.v2';
  program_binding_id: string;
  program_id: string;
  program_revision: string;
  retained_bundle_id: string;
  graph_revision: string;
  node_environment: ProgramBindingNodeEnvironment;
  component_role_bindings: ProgramComponentRoleBinding[];
  target_role_bindings: ProgramTargetRoleBinding[];
  node_policies: ProgramNodePolicy[];
  contact_grasp_bindings: ProgramContactGraspBinding[];
  provenance: ProgramBindingProvenance;
}
export interface ProgramBindingNodeEnvironment {
  catalog_id: string;
  content_version: string;
  catalog_digest: string;
  environment_digest: string;
}
export interface ProgramComponentRoleBinding {
  component_role_id: string;
  controllable_component_id: string;
  component_type_id: string;
  component_type_revision: string;
  model_role_bindings: ProgramModelRoleBinding[];
}
export interface ProgramModelRoleBinding {
  model_role_id: string;
  model_instance_id: string;
  model_revision: string;
}
export interface ProgramTargetRoleBinding {
  target_role_id: string;
  component_role_id: string;
  controllable_component_id: string;
  command_target_id: string;
  frame_role_bindings: ProgramFrameRoleBinding[];
  resource_role_bindings: ProgramResourceRoleBinding[];
  collision_body_role_bindings: ProgramCollisionBodyRoleBinding[];
  capability_bindings: ProgramCapabilityBinding[];
  signal_role_bindings?: ProgramSignalRoleBinding[];
  pneumatic_io?: ProgramPneumaticIoBinding;
  robot_tcps?: ProgramRobotTcpBinding[];
  default_tcp?: ProgramTcpEntry;
}
export interface ProgramFrameRoleBinding {
  frame_role_id: string;
  model_instance_id: string;
  model_revision: string;
  compiled_frame_name: string;
  contact_body_role_ids?: string[];
}
export interface ProgramTcpEntry {
  component_id: string;
  target_id: string;
  frame_role_id: string;
}
export interface ProgramRobotTcpBinding {
  entry: ProgramTcpEntry;
  frame: ProgramFrameRoleBinding;
  end_effector: boolean;
  component_default: boolean;
}
export interface ProgramResourceRoleBinding {
  resource_role_id: string;
  resource_ids: string[];
  compiled_resource_names: string[];
}
export interface ProgramCollisionBodyRoleBinding {
  collision_body_role_id: string;
  model_instance_id: string;
  model_revision: string;
  compiled_collision_body_names: string[];
}
export interface ProgramCapabilityBinding {
  capability_id: string;
  capability_definition_revision: string;
  control_binding: ProgramControlBindingRef | null;
  capability_provider_binding: ProgramCapabilityProviderBindingRef | null;
  command_schema: ProgramCommandSchemaRef | null;
}
export interface ProgramNodePolicy {
  instance_id: string;
  recovery_policy: ProgramNodeRecoveryPolicy;
  operator_stop_policy: ProgramOperatorStopPolicy;
}
export interface ProgramNodeRecoveryPolicy {
  allow_intervention: boolean;
  max_retries: number;
  intervention_stop_scope?: ProgramInterventionStopScope;
}
export interface ProgramControlBindingRef {
  control_binding_id: string;
  control_binding_revision: string;
}
export interface ProgramCapabilityProviderBindingRef {
  capability_provider_binding_id: string;
  capability_provider_binding_revision: string;
}
export interface ProgramCommandSchemaRef {
  command_schema_id: string;
  command_schema_revision: string;
}
export interface ProgramContactGraspEnvelopeBinding {
  contact_policy_id: string;
  contact_policy_revision: string;
  runtime_object_id: string;
  allowed_body_bindings: ProgramContactGraspBodyBinding[];
  max_penetration_m: number;
  terminal_window_start_fraction: number;
  bodies_from_motion_endpoint?: true;
}
export interface ProgramContactGraspBodyBinding {
  component_role_id: string;
  target_role_id: string;
  controllable_component_id: string;
  command_target_id: string;
  model_role_id: string | null;
  collision_body_role_id: string;
  model_instance_id: string;
  model_revision: string;
  collision_body_ids: string[];
}
export interface ProgramBindingProvenance {
  binding_compiler_contract_digest: string;
  role_assignment_digest: string;
  source_records: ProgramBindingProvenanceRecord[];
  resolved_output_records: ProgramBindingProvenanceRecord[];
}
export interface ProgramBindingProvenanceRecord {
  record_kind: CatalogRecordKind;
  record_id: string;
  revision: string;
}
export interface ProgramSignalRoleBinding {
  resource_role_id: string;
  signal_name: string;
  value_type: 'bool';
  direction: 'input' | 'output';
  transition_capability: 'sampled_only' | 'captured';
  signal_descriptor_digest: string;
}
export interface ProgramPneumaticIoBinding {
  outputs: ProgramPneumaticOutputBinding[];
  inputs: ProgramPneumaticInputBinding[];
}
export interface ProgramPneumaticOutputBinding {
  target_role_id: string;
  resource_role_id: string;
  open_value: boolean;
  close_value: boolean;
}
export interface ProgramPneumaticInputBinding {
  input: 'open' | 'closed' | 'pressure' | 'workpiece_grasped';
  target_role_id: string;
  resource_role_id: string;
}
export interface ProgramContactReleaseBinding {
  contact_policy_id: string;
  contact_policy_revision: string;
  runtime_object_id: string;
  allowed_body_bindings: ProgramContactGraspBodyBinding[];
  mode: 'contact_release';
  release_evidence_mode: 'signed_penetration' | 'binary_contact';
  enforce_max_start_penetration: boolean;
  max_start_penetration_m: number;
  min_release_progress_m: number;
  release_tolerance_m: number;
  require_strictly_decreasing_penetration: boolean;
  require_separation_by_end: boolean;
  allow_recontact: boolean;
  bodies_from_motion_endpoint?: true;
}
export type ProgramContactGraspBinding = ProgramContactGraspEnvelopeBinding | ProgramContactReleaseBinding;
