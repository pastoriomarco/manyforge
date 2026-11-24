import { describe, expect, it } from 'vitest';
import nativeFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { parseProgramBindingTransport, ProgramBindingTransportError } from './programBindingTransport.generated';

it('refuses an old Program Binding without repairing it', () => {
  const document = { ...structuredClone(nativeFixture), api_version: 'manyforge.program_binding.v3' };
  const before = structuredClone(document);
  expect(() => parseProgramBindingTransport(document, 'binding')).toThrow(ProgramBindingTransportError);
  expect(document).toEqual(before);
});

it('requires the genuine native v4 Binding fixture for the cumulative gate', () => {
  expect(nativeFixture.api_version, 'native v4 Binding fixture and legacy corpus migration are required')
    .toBe('manyforge.program_binding.v4');
});

type Row = Record<string, unknown>;
const row = (value: unknown): Row => value as Row;
const rows = (value: unknown): Row[] => value as Row[];
// Synthetic schema-only input. It is not a native artifact or a substitute
// for the separately required genuine v4 native fixture above.
const fixture = (): Row => {
  const digest = `sha256:${'a'.repeat(64)}`;
  const target = { component_id: 'robot', command_target_id: 'arm' };
  const frame = { component_id: 'robot', frame_id: 'tcp' };
  const body = { component_id: 'robot', collision_body_id: 'finger' };
  return {
    api_version: 'manyforge.program_binding.v4', program_binding_id: digest,
    program_revision: digest, retained_bundle_id: digest,
    graph_revision: digest,
    node_environment: { catalog_id: 'catalog', content_version: '1', catalog_digest: digest, environment_digest: digest },
    component_bindings: [{ component_id: 'robot', component_type_id: 'type', component_type_revision: digest,
      model_bindings: [{ model_role_id: 'model', model_instance_id: 'instance', model_revision: digest }] }],
    target_bindings: [{ target, frame_references: [frame],
      frame_associations: [{ frame: structuredClone(frame), contact_body_role_ids: [] }],
      collision_body_references: [body], signal_references: [],
      resource_role_bindings: [{ resource_role_id: 'joints', resource_ids: ['joint'], compiled_resource_names: ['joint'] }],
      capability_bindings: [{ capability_id: 'motion', capability_definition_revision: digest,
        control_binding: { control_binding_id: 'control', control_binding_revision: digest },
        capability_provider_binding: null, command_schema: null }] }],
    frame_bindings: [{ reference: structuredClone(frame), model_instance_id: 'instance', model_revision: digest, compiled_frame_name: 'tcp' }],
    collision_body_bindings: [{ reference: structuredClone(body), model_instance_id: 'instance', model_revision: digest,
      compiled_collision_body_names: ['finger_collision'] }],
    signal_bindings: [], node_policies: [nodePolicy()],
    // The collision bodies that node collision rules select, with their generated body ids.
    collision_rule_bodies: [{ body: structuredClone(body), model_role_id: 'model',
      model_instance_id: 'instance', model_revision: digest,
      collision_body_ids: ['component/robot/model/instance/body/finger/collision/0'] }],
    provenance: { binding_compiler_contract_digest: digest,
      source_records: [{ record_kind: 'model_asset', record_id: 'model', revision: digest }], resolved_output_records: [] },
  };
};

function nodePolicy(instance_id = 'root'): Row {
  return {
    instance_id,
    recovery_policy: { allow_intervention: false, max_retries: 1 },
    operator_stop_policy: 'hold',
  };
}

function context(value: Row) {
  const component = rows(value.component_bindings)[0];
  const target = rows(value.target_bindings)[0];
  const resource = rows(target.resource_role_bindings)[0];
  const collision = rows(value.collision_body_bindings)[0];
  const capability = rows(target.capability_bindings)[0];
  const body = rows(value.collision_rule_bodies)[0];
  const provenance = rows(row(value.provenance).source_records);
  return {
    component, target, resource, collision, capability, body, provenance,
  };
}

function mutate(name: string, value: Row): void {
  const {
    component, target, resource, collision, capability, body, provenance,
  } = context(value);
  const policy = rows(value.node_policies)[0];
  const recovery = row(policy.recovery_policy);
  const duplicate = (owner: Row, field: string): void => {
    const values = rows(owner[field]);
    owner[field] = [...values, structuredClone(values[0])];
  };
  switch (name) {
    case 'null': component.component_type_id = null; break;
    case 'wrong-type': value.target_bindings = {}; break;
    case 'identity': component.component_type_id = 'not portable'; break;
    case 'identity-bytes': component.component_type_id = 'a'.repeat(129); break;
    case 'text-bytes': rows(value.frame_bindings)[0].compiled_frame_name = 'é'.repeat(2049); break;
    case 'invalid-unicode': rows(value.frame_bindings)[0].compiled_frame_name = '\ud800'; break;
    case 'root-minimum': value.component_bindings = []; break;
    case 'root-maximum': value.component_bindings = Array(4097).fill(component); break;
    case 'nested-maximum': value.frame_bindings = Array(4097).fill(rows(value.frame_bindings)[0]); break;
    case 'provenance-maximum': row(value.provenance).source_records = Array(16385).fill(provenance[0]); break;
    case 'component-order': duplicate(value, 'component_bindings'); break;
    case 'model-order': duplicate(component, 'model_bindings'); break;
    case 'target-order': duplicate(value, 'target_bindings'); break;
    case 'frame-order': duplicate(value, 'frame_bindings'); break;
    case 'resource-order': duplicate(target, 'resource_role_bindings'); break;
    case 'collision-order': duplicate(value, 'collision_body_bindings'); break;
    case 'capability-order': duplicate(target, 'capability_bindings'); break;
    case 'resource-size':
      resource.compiled_resource_names = [...resource.compiled_resource_names as string[], 'extra'];
      break;
    case 'resource-id-duplicate':
      resource.resource_ids = [...resource.resource_ids as string[], (resource.resource_ids as string[])[0]];
      resource.compiled_resource_names = [...resource.compiled_resource_names as string[], 'extra'];
      break;
    case 'resource-name-duplicate':
      resource.resource_ids = [...resource.resource_ids as string[], 'extra'];
      resource.compiled_resource_names = [...resource.compiled_resource_names as string[], (resource.compiled_resource_names as string[])[0]];
      break;
    case 'collision-name-duplicate':
      collision.compiled_collision_body_names = [
        ...collision.compiled_collision_body_names as string[],
        (collision.compiled_collision_body_names as string[])[0],
      ];
      break;
    case 'all-null-capability':
      capability.control_binding = null; capability.capability_provider_binding = null; break;
    case 'target-component': row(target.target).component_id = 'mismatch'; break;
    case 'body-minimum': body.collision_body_ids = []; break;
    case 'body-order': duplicate(value, 'collision_rule_bodies'); break;
    case 'removed-contact-bindings': value.contact_grasp_bindings = []; break;
    case 'generated-body-id': (body.collision_body_ids as string[])[0] = 'wrong'; break;
    case 'body-target': row(body.body).component_id = 'mismatch'; break;
    case 'body-collision': body.model_revision = `sha256:${'f'.repeat(64)}`; break;
    case 'body-model': body.model_role_id = 'mismatch'; break;
    case 'provenance-kind': provenance[0].record_kind = 'unknown_kind'; break;
    case 'provenance-order': row(value.provenance).source_records = [...provenance, structuredClone(provenance[0])]; break;
    case 'node-policies-missing': delete value.node_policies; break;
    case 'node-policies-wrong-type': value.node_policies = {}; break;
    case 'node-policy-null': value.node_policies = [null]; break;
    // Removed policy surfaces are refused, not ignored.
    case 'removed-modification-policy':
      policy.modification_policy = {
        runtime_override: 'forbidden',
        persistent_edit: 'forbidden',
        intervention_delegate: 'forbidden',
      };
      break;
    case 'removed-recovery-actions': recovery.allowed_recovery_actions = []; break;
    case 'removed-skills': recovery.allowed_skills = []; break;
    case 'removed-persistence-ceiling': recovery.persistence_ceiling = 'session_only'; break;
    case 'recovery-unknown': recovery.unknown = true; break;
    case 'recovery-null': policy.recovery_policy = null; break;
    case 'recovery-empty': policy.recovery_policy = {}; break;
    case 'recovery-partial': delete recovery.max_retries; break;
    case 'stop-enum': policy.operator_stop_policy = 'pause'; break;
    case 'stop-null': policy.operator_stop_policy = null; break;
    case 'stop-wrong-type': policy.operator_stop_policy = 1; break;
    case 'allow-intervention-type': recovery.allow_intervention = 0; break;
    case 'retry-bool': recovery.max_retries = true; break;
    case 'retry-low': recovery.max_retries = -1; break;
    case 'retry-high': recovery.max_retries = 256; break;
    case 'instance-upper': policy.instance_id = 'Root'; break;
    case 'instance-bytes': policy.instance_id = 'a'.repeat(129); break;
    case 'table-duplicate': value.node_policies = [policy, structuredClone(policy)]; break;
    case 'table-unsorted': value.node_policies = [policy, nodePolicy('a')]; break;
    case 'table-bound': value.node_policies = Array(100001).fill(policy); break;
    case 'default-row':
      value.node_policies = [{
        instance_id: 'root',
        recovery_policy: { allow_intervention: false, max_retries: 0 },
        operator_stop_policy: 'inherit',
      }];
      break;
    default: throw new Error(`unknown corpus case ${name}`);
  }
}

const cases = [
  'null', 'wrong-type', 'identity', 'identity-bytes', 'text-bytes',
  'invalid-unicode',
  'root-minimum', 'root-maximum', 'nested-maximum', 'provenance-maximum',
  'component-order', 'model-order', 'target-order', 'frame-order',
  'resource-order', 'collision-order', 'capability-order',
  'resource-size', 'resource-id-duplicate', 'resource-name-duplicate',
  'collision-name-duplicate', 'all-null-capability', 'target-component',
  'body-minimum', 'body-order', 'removed-contact-bindings', 'generated-body-id', 'body-target',
  'body-collision', 'body-model', 'provenance-kind', 'provenance-order',
  'node-policies-missing', 'node-policies-wrong-type', 'node-policy-null',
  'removed-modification-policy', 'removed-recovery-actions', 'removed-skills',
  'removed-persistence-ceiling',
  'recovery-unknown', 'recovery-null', 'recovery-empty',
  'recovery-partial', 'stop-enum', 'allow-intervention-type',
  'retry-bool',
  'stop-null', 'stop-wrong-type',
  'retry-low', 'retry-high',
  'instance-upper', 'instance-bytes', 'table-duplicate',
  'table-unsorted', 'table-bound', 'default-row',
];

describe('generated Program Binding transport parser (synthetic v4 schema cases)', () => {
  it.skipIf(nativeFixture.api_version !== 'manyforge.program_binding.v4')('returns the exact native fixture reference with concrete nested rows', () => {
    const document = structuredClone(nativeFixture) as Row;
    const parsed = parseProgramBindingTransport(document, 'binding');
    expect(parsed).toBe(document);
    expect(parsed.component_bindings[0].component_id).toBeTypeOf('string');
    expect(parsed.target_bindings[0].capability_bindings).toBeInstanceOf(Array);
  });

  it('preserves absent and empty robot TCP inventories and rejects null', () => {
    const document = fixture();
    const target = rows(document.target_bindings)[0];
    delete target.robot_tcps;
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(Object.hasOwn(target, 'robot_tcps')).toBe(false);
    target.robot_tcps = [];
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(target.robot_tcps).toEqual([]);
    target.robot_tcps = null;
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow();
  });

  it('checks direct signal identity against its exact bound target', () => {
    const document = fixture();
    const target = rows(document.target_bindings)[0];
    const reference = { component_id: 'robot', signal_name: 'DI0' };
    document.signal_bindings = [{ reference, target: structuredClone(target.target), runtime_unit_id: 'gpio',
      value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
      signal_descriptor_digest: `sha256:${'a'.repeat(64)}` }];
    target.signal_references = [structuredClone(reference)];
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    row(rows(document.signal_bindings)[0].target).command_target_id = 'other';
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow();
  });

  it('checks direct TCP frame correspondence and duplicate entries', () => {
    const document = fixture();
    const target = rows(document.target_bindings)[0];
    const frame = rows(document.frame_bindings)[0];
    const entry = { target: structuredClone(target.target), frame: structuredClone(frame.reference) };
    const tcp = { entry, frame: structuredClone(frame), contact_body_role_ids: [],
      end_effector: true, component_default: true };
    target.default_tcp = structuredClone(entry);
    target.robot_tcps = [tcp];
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    target.robot_tcps = [tcp, structuredClone(tcp)];
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow();
    target.robot_tcps = [tcp];
    row(tcp.entry.frame).frame_id = 'other';
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow();
  });

  it('does not infer native retained-bundle context', () => {
    const document = fixture();
    document.retained_bundle_id = `sha256:${'f'.repeat(64)}`;
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
  });

  it('accepts a nonempty sparse node-policy row', () => {
    const document = fixture();
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(rows(document.node_policies)[0].instance_id).toBe('root');
  });

  it('preserves omitted intervention scope without inserting a default', () => {
    const document = fixture();
    const before = JSON.stringify(document);
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(JSON.stringify(document)).toBe(before);
    expect(Object.hasOwn(row(rows(document.node_policies)[0].recovery_policy), 'intervention_stop_scope')).toBe(false);
  });

  it.each(['all', 'own_groups'])('preserves intervention scope %s', (scope) => {
    const document = fixture();
    row(rows(document.node_policies)[0].recovery_policy).intervention_stop_scope = scope;
    const before = JSON.stringify(document);
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(JSON.stringify(document)).toBe(before);
  });

  it('accepts own-groups scope as a nondefault policy on its own', () => {
    const document = fixture();
    document.node_policies = [{
      instance_id: 'root',
      recovery_policy: { allow_intervention: false, max_retries: 0, intervention_stop_scope: 'own_groups' },
      operator_stop_policy: 'inherit',
    }];
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
  });

  it('rejects an otherwise-default policy with explicit all scope', () => {
    const document = fixture();
    document.node_policies = [{
      instance_id: 'root',
      recovery_policy: { allow_intervention: false, max_retries: 0, intervention_stop_scope: 'all' },
      operator_stop_policy: 'inherit',
    }];
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow(/default node-policy rows must be omitted/);
  });

  it.each([null, true, 0, [], {}, 'own_group', 'selected', undefined])('rejects intervention scope %j', (scope) => {
    const document = fixture();
    row(rows(document.node_policies)[0].recovery_policy).intervention_stop_scope = scope;
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow(/intervention_stop_scope/);
  });

  it.each(cases)('rejects the %s mutation family', (name) => {
    const document = fixture();
    mutate(name, document);
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow();
  });

  it.each([
    (value: Row) => { value.unknown = true; },
    (value: Row) => { delete row(value.node_environment).catalog_id; },
    (value: Row) => { row(value.node_environment).unknown = true; },
    (value: Row) => { context(value).component.unknown = true; },
    (value: Row) => { rows(context(value).component.model_bindings)[0].unknown = true; },
    (value: Row) => { context(value).target.unknown = true; },
    (value: Row) => { rows(value.frame_bindings)[0].unknown = true; },
    (value: Row) => { context(value).resource.unknown = true; },
    (value: Row) => { context(value).collision.unknown = true; },
    (value: Row) => { context(value).capability.unknown = true; },
    (value: Row) => {
      context(value).capability.control_binding = {
        control_binding_id: 'control',
        control_binding_revision: `sha256:${'a'.repeat(64)}`,
        unknown: true,
      };
    },
    (value: Row) => {
      context(value).capability.capability_provider_binding = {
        capability_provider_binding_id: 'provider',
        capability_provider_binding_revision: `sha256:${'a'.repeat(64)}`,
        unknown: true,
      };
    },
    (value: Row) => {
      context(value).capability.command_schema = {
        command_schema_id: 'command',
        command_schema_revision: `sha256:${'a'.repeat(64)}`,
        unknown: true,
      };
    },
    (value: Row) => { context(value).body.unknown = true; },
    (value: Row) => { row(value.provenance).unknown = true; },
    (value: Row) => { context(value).provenance[0].unknown = true; },
  ])('rejects every nested open shape', (mutation) => {
    const document = fixture();
    mutation(document);
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow(/closed record/);
  });
});

describe('TCP contact bodies of a frame association (synthetic v4 schema cases)', () => {
  function endpointFixture(): Row {
    const value = fixture();
    const target = rows(value.target_bindings)[0];
    const association = rows(target.frame_associations)[0];
    association.contact_body_role_ids = ['finger'];
    return value;
  }

  it('preserves the contact bodies of the selected frame association', () => {
    const value = endpointFixture();
    expect(parseProgramBindingTransport(value, 'binding')).toBe(value);
  });

  it.each(['missing_body', 'wrong_model', 'duplicate_body'])('rejects %s', (fault) => {
    const value = endpointFixture();
    const target = rows(value.target_bindings)[0];
    const frame = rows(target.frame_associations)[0];
    if (fault === 'missing_body') frame.contact_body_role_ids = ['absent'];
    else if (fault === 'wrong_model') rows(value.frame_bindings)[0].model_revision = `sha256:${'f'.repeat(64)}`;
    else {
      const roles = frame.contact_body_role_ids as string[];
      frame.contact_body_role_ids = [...roles, ...roles];
    }
    expect(() => parseProgramBindingTransport(value, 'binding')).toThrow();
  });
});

describe('collision rule bodies (synthetic v4 schema cases)', () => {
  it('accepts an empty table and a body without a model role', () => {
    const empty = fixture();
    empty.collision_rule_bodies = [];
    expect(parseProgramBindingTransport(empty, 'binding')).toBe(empty);
    const roleless = fixture();
    rows(roleless.collision_rule_bodies)[0].model_role_id = null;
    expect(parseProgramBindingTransport(roleless, 'binding')).toBe(roleless);
  });

  it('requires the table', () => {
    const value = fixture();
    delete value.collision_rule_bodies;
    expect(() => parseProgramBindingTransport(value, 'binding')).toThrow(/closed record/);
  });

  it.each(['wrong_type', 'null_row'])('rejects %s at its path', (fault) => {
    const value = fixture();
    value.collision_rule_bodies = fault === 'wrong_type' ? {} : [null];
    expect(() => parseProgramBindingTransport(value, 'binding')).toThrow(/collision_rule_bodies/);
  });
});
