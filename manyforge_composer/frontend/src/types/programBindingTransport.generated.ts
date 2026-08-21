/* GENERATED FILE -- program_binding_transport.v1.json; DO NOT EDIT. */

export type CatalogRecordKind = 'model_asset' | 'runtime_profile' | 'component_type' | 'control_interface_contract' | 'controller_topology' | 'provider_manifest' | 'command_schema' | 'capability_definition' | 'contact_policy' | 'stop_evidence_policy' | 'runtime_policy';
export interface ProgramBindingDocument {
  api_version: 'manyforge.program_binding.v1';
  program_binding_id: string;
  program_id: string;
  program_revision: string;
  retained_bundle_id: string;
  graph_revision: string;
  node_environment: ProgramBindingNodeEnvironment;
  component_role_bindings: ProgramComponentRoleBinding[];
  target_role_bindings: ProgramTargetRoleBinding[];
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
}
export interface ProgramFrameRoleBinding {
  frame_role_id: string;
  model_instance_id: string;
  model_revision: string;
  compiled_frame_name: string;
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
export interface ProgramContactGraspBinding {
  contact_policy_id: string;
  contact_policy_revision: string;
  runtime_object_id: string;
  allowed_body_bindings: ProgramContactGraspBodyBinding[];
  max_penetration_m: number;
  terminal_window_start_fraction: number;
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
  binding_compiler_build_digest: string;
  role_assignment_digest: string;
  source_records: ProgramBindingProvenanceRecord[];
  resolved_output_records: ProgramBindingProvenanceRecord[];
}
export interface ProgramBindingProvenanceRecord {
  record_kind: CatalogRecordKind;
  record_id: string;
  revision: string;
}
