import { expect, it } from 'vitest';
import { parseProgramBindingTransport } from './programBindingTransport.generated';

type Row = Record<string, unknown>;

// Deliberately incomplete, never a claimed native binding or valid fixture.
function partialBinding(policy: Row): Row {
  const digest = `sha256:${'0'.repeat(64)}`;
  return {
    api_version: 'manyforge.program_binding.v3', program_binding_id: digest,
    program_id: 'program', program_revision: digest, retained_bundle_id: digest,
    graph_revision: digest,
    node_environment: { catalog_id: 'catalog', content_version: '1', catalog_digest: digest, environment_digest: digest },
    component_bindings: [], target_bindings: [], frame_bindings: [], collision_body_bindings: [],
    signal_bindings: [], node_policies: [policy], contact_grasp_bindings: [], provenance: {},
  };
}

const policy = (): Row => ({
  instance_id: 'root', recovery_policy: { allow_intervention: true, max_retries: 1 },
  operator_stop_policy: 'hold',
});

it.each([
  ['instance_id', 'Root'], ['instance_id', 'a'.repeat(129)],
  ['operator_stop_policy', 'pause'], ['operator_stop_policy', null], ['operator_stop_policy', 1],
  ['recovery_policy', null], ['recovery_policy', {}],
])('rejects invalid policy field %s', (field, value) => {
  const input = policy();
  input[field as string] = value;
  expect(() => parseProgramBindingTransport(partialBinding(input), 'binding')).toThrow(/node_policies/);
});

it.each([
  ['allow_intervention', 0], ['max_retries', true], ['max_retries', -1], ['max_retries', 256],
  ['intervention_stop_scope', null], ['intervention_stop_scope', true], ['intervention_stop_scope', 0],
  ['intervention_stop_scope', []], ['intervention_stop_scope', {}],
  ['intervention_stop_scope', 'own_group'], ['intervention_stop_scope', 'selected'],
  ['allowed_recovery_actions', ['intervention']],
])('rejects invalid recovery field %s', (field, value) => {
  const input = policy();
  (input.recovery_policy as Row)[field as string] = value;
  expect(() => parseProgramBindingTransport(partialBinding(input), 'binding')).toThrow(/node_policies/);
});

it.each([null, 'all'])('rejects omitted default policy with scope %s', (scope) => {
  const recovery: Row = { allow_intervention: false, max_retries: 0 };
  if (scope !== null) recovery.intervention_stop_scope = scope;
  const input = { instance_id: 'root', recovery_policy: recovery, operator_stop_policy: 'inherit' };
  expect(() => parseProgramBindingTransport(partialBinding(input), 'binding'))
    .toThrow(/default node-policy rows must be omitted/);
});

it.each([
  ['program_id', null], ['program_id', 'not portable'], ['program_id', 'a'.repeat(129)],
  ['component_bindings', {}], ['component_bindings', [{}]],
  ['target_bindings', {}], ['target_bindings', [{}]],
  ['frame_bindings', [{}]], ['collision_body_bindings', [{}]], ['signal_bindings', [{}]],
  ['node_policies', {}], ['node_policies', [null]],
])('rejects invalid root field %s at its path', (field, value) => {
  const input = partialBinding(policy());
  input[field as string] = value;
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(new RegExp(field as string));
});

it.each(['duplicate', 'unsorted'])('rejects %s node-policy rows', (ordering) => {
  const first = policy();
  const second = structuredClone(first);
  if (ordering === 'unsorted') second.instance_id = 'a';
  const input = partialBinding(first);
  input.node_policies = [first, second];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/node_policies.*sorted unique/);
});

it.each([
  ['max_penetration_m', 0], ['max_penetration_m', true], ['max_penetration_m', Number.NaN],
  ['terminal_window_start_fraction', 1.01], ['bodies_from_motion_endpoint', false], ['unknown', true],
])('rejects invalid contact field %s', (field, value) => {
  const input = partialBinding(policy());
  const contact: Row = {
    contact_policy_id: 'policy', contact_policy_revision: `sha256:${'0'.repeat(64)}`,
    runtime_object_id: 'object', allowed_body_bindings: [], bodies_from_motion_endpoint: true,
    max_penetration_m: 0.001, terminal_window_start_fraction: 0.8,
  };
  contact[field as string] = value;
  input.contact_grasp_bindings = [contact];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/contact_grasp_bindings/);
});

it.each(['unknown_kind', [], {}])('rejects unrecognized provenance kind %j', (kind) => {
  const input = partialBinding(policy());
  input.provenance = {
    binding_compiler_contract_digest: `sha256:${'0'.repeat(64)}`,
    source_records: [{ record_kind: kind, record_id: 'record', revision: `sha256:${'0'.repeat(64)}` }],
    resolved_output_records: [],
  };
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/provenance.source_records.*record_kind/);
});

it.each([
  ['frame_bindings', 'frame_id', 'missing_component'], ['frame_bindings', 'frame_id', 'unknown_field'],
  ['frame_bindings', 'frame_id', 'invalid_id'], ['collision_body_bindings', 'collision_body_id', 'missing_component'],
  ['collision_body_bindings', 'collision_body_id', 'unknown_field'], ['collision_body_bindings', 'collision_body_id', 'invalid_id'],
])('rejects malformed %s reference (%s, %s)', (collection, idField, fault) => {
  const input = partialBinding(policy());
  const reference: Row = { component_id: 'station', [idField]: 'item' };
  if (fault === 'missing_component') delete reference.component_id;
  else if (fault === 'unknown_field') reference.unknown = true;
  else reference[idField] = 'invalid id';
  const geometry: Row = { reference, model_instance_id: 'model', model_revision: `sha256:${'0'.repeat(64)}` };
  if (collection === 'frame_bindings') geometry.compiled_frame_name = 'frame';
  else geometry.compiled_collision_body_names = ['body'];
  input[collection] = [geometry];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(new RegExp(`${collection}.*reference`));
});

it.each(['missing_component', 'unknown_field', 'invalid_id'])('rejects malformed direct target reference %s', (fault) => {
  const input = partialBinding(policy());
  const target: Row = { component_id: 'station', command_target_id: 'io' };
  if (fault === 'missing_component') delete target.component_id;
  else if (fault === 'unknown_field') target.unknown = true;
  else target.command_target_id = 'invalid id';
  input.target_bindings = [{ target, frame_references: [], frame_associations: [], collision_body_references: [],
    signal_references: [], resource_role_bindings: [], capability_bindings: [] }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/target_bindings\[0\]\.target/);
});

it.each(['missing_component', 'unknown_field', 'invalid_id'])('rejects malformed direct signal reference %s', (fault) => {
  const input = partialBinding(policy());
  const reference: Row = { component_id: 'station', signal_name: 'ready' };
  if (fault === 'missing_component') delete reference.component_id;
  else if (fault === 'unknown_field') reference.unknown = true;
  else reference.signal_name = 'invalid\0name';
  input.signal_bindings = [{ reference, target: { component_id: 'station', command_target_id: 'io' },
    runtime_unit_id: 'station', value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
    signal_descriptor_digest: `sha256:${'0'.repeat(64)}` }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/signal_bindings\[0\]\.reference/);
});

it.each([
  ['value_type', true], ['direction', 'bidirectional'], ['transition_capability', 'inferred'],
  ['signal_descriptor_digest', `sha256:${'A'.repeat(64)}`], ['unknown', true],
])('rejects malformed signal binding field %s', (field, value) => {
  const input = partialBinding(policy());
  const signal: Row = { reference: { component_id: 'station', signal_name: 'ready' },
    target: { component_id: 'station', command_target_id: 'io' }, runtime_unit_id: 'station',
    value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
    signal_descriptor_digest: `sha256:${'0'.repeat(64)}` };
  signal[field as string] = value;
  input.signal_bindings = [signal];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/signal_bindings\[0\]/);
});

it.each([
  ['enforce_max_start_penetration', true], ['require_strictly_decreasing_penetration', true],
  ['max_start_penetration_m', 0.002], ['min_release_progress_m', 0.001], ['release_tolerance_m', 0.00001],
])('rejects non-neutral binary release field %s', (field, value) => {
  const input = partialBinding(policy());
  const release: Row = { contact_policy_id: 'policy', contact_policy_revision: `sha256:${'0'.repeat(64)}`,
    runtime_object_id: 'object', allowed_body_bindings: [], bodies_from_motion_endpoint: true,
    mode: 'contact_release', release_evidence_mode: 'binary_contact', enforce_max_start_penetration: false,
    max_start_penetration_m: 0, min_release_progress_m: 0, release_tolerance_m: 0,
    require_strictly_decreasing_penetration: false, require_separation_by_end: true, allow_recontact: false };
  release[field as string] = value;
  input.contact_grasp_bindings = [release];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/contact_grasp_bindings\[0\].*binary/);
});

it.each(['missing_retry', 'duplicate_component', 'duplicate_model'])('rejects %s', (fault) => {
  const input = partialBinding(policy());
  let expected: RegExp;
  if (fault === 'missing_retry') {
    delete ((input.node_policies as Row[])[0].recovery_policy as Row).max_retries;
    expected = /node_policies\[0\]\.recovery_policy/;
  } else {
    const digest = `sha256:${'0'.repeat(64)}`;
    const model = { model_role_id: 'model', model_instance_id: 'model', model_revision: digest };
    const component = { component_id: 'station', component_type_id: 'type', component_type_revision: digest,
      model_bindings: fault === 'duplicate_model' ? [model, structuredClone(model)] : [model] };
    input.component_bindings = fault === 'duplicate_model' ? [component] : [component, structuredClone(component)];
    expected = fault === 'duplicate_model' ? /component_bindings\[0\]\.model_bindings/ : /component_bindings/;
  }
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(expected);
});

it.each(['mode', 'release_evidence_mode', 'enforce_max_start_penetration', 'max_start_penetration_m',
  'min_release_progress_m', 'release_tolerance_m', 'require_strictly_decreasing_penetration',
  'require_separation_by_end', 'allow_recontact'])('requires contact release field %s', (field) => {
  const input = partialBinding(policy());
  const release: Row = { contact_policy_id: 'policy', contact_policy_revision: `sha256:${'0'.repeat(64)}`,
    runtime_object_id: 'object', allowed_body_bindings: [], bodies_from_motion_endpoint: true,
    mode: 'contact_release', release_evidence_mode: 'binary_contact', enforce_max_start_penetration: false,
    max_start_penetration_m: 0, min_release_progress_m: 0, release_tolerance_m: 0,
    require_strictly_decreasing_penetration: false, require_separation_by_end: true, allow_recontact: false };
  delete release[field];
  input.contact_grasp_bindings = [release];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/contact_grasp_bindings\[0\]/);
});

it.each(['vector_size', 'resource_id_duplicate', 'resource_name_duplicate', 'unknown_field'])('rejects malformed target resource %s', (fault) => {
  const input = partialBinding(policy());
  const digest = `sha256:${'0'.repeat(64)}`;
  input.component_bindings = [{ component_id: 'station', component_type_id: 'type',
    component_type_revision: digest, model_bindings: [] }];
  const resource: Row = { resource_role_id: 'role', resource_ids: ['resource'], compiled_resource_names: ['name'] };
  if (fault === 'vector_size') (resource.compiled_resource_names as string[]).push('extra');
  else if (fault === 'resource_id_duplicate') {
    (resource.resource_ids as string[]).push('resource'); (resource.compiled_resource_names as string[]).push('extra');
  } else if (fault === 'resource_name_duplicate') {
    (resource.resource_ids as string[]).push('extra'); (resource.compiled_resource_names as string[]).push('name');
  } else resource.unknown = true;
  input.target_bindings = [{ target: { component_id: 'station', command_target_id: 'io' },
    frame_references: [], frame_associations: [], collision_body_references: [], signal_references: [],
    resource_role_bindings: [resource], capability_bindings: [] }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/target_bindings\[0\]\.resource_role_bindings/);
});

it.each(['no_provider', 'unknown_field', 'control_extra', 'provider_extra', 'schema_extra'])('rejects malformed target capability %s', (fault) => {
  const input = partialBinding(policy());
  const digest = `sha256:${'0'.repeat(64)}`;
  input.component_bindings = [{ component_id: 'station', component_type_id: 'type',
    component_type_revision: digest, model_bindings: [] }];
  const capability: Row = { capability_id: 'cap', capability_definition_revision: digest,
    control_binding: { control_binding_id: 'control', control_binding_revision: digest },
    capability_provider_binding: null, command_schema: null };
  if (fault === 'no_provider') capability.control_binding = null;
  else if (fault === 'unknown_field') capability.unknown = true;
  else if (fault === 'control_extra') (capability.control_binding as Row).unknown = true;
  else if (fault === 'provider_extra') capability.capability_provider_binding = {
    capability_provider_binding_id: 'provider', capability_provider_binding_revision: digest, unknown: true };
  else capability.command_schema = { command_schema_id: 'schema', command_schema_revision: digest, unknown: true };
  input.target_bindings = [{ target: { component_id: 'station', command_target_id: 'io' },
    frame_references: [], frame_associations: [], collision_body_references: [], signal_references: [],
    resource_role_bindings: [], capability_bindings: [capability] }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/target_bindings\[0\]\.capability_bindings/);
});

it.each(['route_extra', 'missing_capability', 'empty_outputs', 'bad_output_level'])('rejects malformed pneumatic route %s', (fault) => {
  const input = partialBinding(policy());
  const digest = `sha256:${'0'.repeat(64)}`;
  input.component_bindings = [{ component_id: 'station', component_type_id: 'type',
    component_type_revision: digest, model_bindings: [] }];
  const capability = { capability_id: 'tool.gripper.command', capability_definition_revision: digest,
    control_binding: { control_binding_id: 'control', control_binding_revision: digest },
    capability_provider_binding: null, command_schema: null };
  const route: Row = { outputs: [{ signal: { component_id: 'station', signal_name: 'output' },
    open_value: true, close_value: false }], inputs: [] };
  if (fault === 'route_extra') route.unknown = true;
  else if (fault === 'empty_outputs') route.outputs = [];
  else if (fault === 'bad_output_level') (route.outputs as Row[])[0].open_value = 1;
  input.target_bindings = [{ target: { component_id: 'station', command_target_id: 'io' },
    frame_references: [], frame_associations: [], collision_body_references: [], signal_references: [],
    resource_role_bindings: [], capability_bindings: fault === 'missing_capability' ? [] : [capability], pneumatic_io: route }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/target_bindings\[0\]\.pneumatic_io/);
});

it.each(['entry_extra', 'frame_extra', 'frame_mismatch', 'end_effector_type', 'default_type'])('rejects malformed TCP row %s', (fault) => {
  const input = partialBinding(policy());
  const digest = `sha256:${'0'.repeat(64)}`;
  input.component_bindings = [{ component_id: 'station', component_type_id: 'type',
    component_type_revision: digest, model_bindings: [] }];
  const reference = { component_id: 'station', frame_id: 'tcp' };
  const row: Row = { entry: { target: { component_id: 'station', command_target_id: 'io' }, frame: { ...reference } },
    frame: { reference, model_instance_id: 'model', model_revision: digest, compiled_frame_name: 'tcp' },
    contact_body_role_ids: [], end_effector: false, component_default: false };
  if (fault === 'entry_extra') (row.entry as Row).unknown = true;
  else if (fault === 'frame_extra') (row.frame as Row).unknown = true;
  else if (fault === 'frame_mismatch') ((row.frame as Row).reference as Row).frame_id = 'other';
  else if (fault === 'end_effector_type') row.end_effector = 1;
  else row.component_default = 1;
  input.target_bindings = [{ target: { component_id: 'station', command_target_id: 'io' },
    frame_references: [], frame_associations: [], collision_body_references: [], signal_references: [],
    resource_role_bindings: [], capability_bindings: [], robot_tcps: [row] }];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/target_bindings\[0\]\.robot_tcps\[0\]/);
});

it.each(['component_bindings', 'target_bindings', 'frame_bindings', 'collision_body_bindings', 'signal_bindings'])('enforces %s bound before examining rows', (collection) => {
  const input = partialBinding(policy());
  input[collection] = Array.from({ length: 4097 }, () => ({}));
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(new RegExp(`${collection}.*bounded array`));
});

it('enforces node-policy table bound before examining rows', () => {
  const input = partialBinding(policy());
  input.node_policies = Array.from({ length: 100001 }, policy);
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/node_policies.*bounded array/);
});

it.each([
  ['mode', 'contact_grasp'], ['release_evidence_mode', 'distance'],
  ['enforce_max_start_penetration', 1], ['max_start_penetration_m', -0.001],
  ['min_release_progress_m', 1000.001], ['release_tolerance_m', true],
  ['require_separation_by_end', null], ['max_penetration_m', 0.002],
  ['terminal_window_start_fraction', 0.5],
])('rejects invalid contact release field %s', (field, value) => {
  const input = partialBinding(policy());
  const release: Row = {
    contact_policy_id: 'policy', contact_policy_revision: `sha256:${'0'.repeat(64)}`,
    runtime_object_id: 'object', allowed_body_bindings: [], bodies_from_motion_endpoint: true,
    mode: 'contact_release', release_evidence_mode: 'signed_penetration',
    enforce_max_start_penetration: true, max_start_penetration_m: 0.002,
    min_release_progress_m: 0, release_tolerance_m: 0.00001,
    require_strictly_decreasing_penetration: true, require_separation_by_end: true, allow_recontact: false,
  };
  release[field as string] = value;
  input.contact_grasp_bindings = [release];
  expect(() => parseProgramBindingTransport(input, 'binding')).toThrow(/contact_grasp_bindings/);
});
