import { describe, expect, it } from 'vitest';
import nativeFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { parseProgramBindingTransport } from './programBindingTransport.generated';

type Row = Record<string, unknown>;
const row = (value: unknown): Row => value as Row;
const rows = (value: unknown): Row[] => value as Row[];
const fixture = (): Row => {
  const value = structuredClone(nativeFixture) as Row;
  const component = rows(value.component_role_bindings)[0];
  const target = rows(value.target_role_bindings)[0];
  const collision = rows(target.collision_body_role_bindings)[0];
  const model = rows(component.model_role_bindings).find(
    (candidate) => candidate.model_instance_id === collision.model_instance_id,
  );
  if (!model) throw new Error('native fixture model correspondence is absent');
  const bodyCount = (collision.compiled_collision_body_names as string[]).length;
  value.contact_grasp_bindings = [{
    contact_policy_id: 'fixture_contact',
    contact_policy_revision: `sha256:${'c'.repeat(64)}`,
    runtime_object_id: 'fixture_object',
    allowed_body_bindings: [{
      component_role_id: target.component_role_id,
      target_role_id: target.target_role_id,
      controllable_component_id: target.controllable_component_id,
      command_target_id: target.command_target_id,
      model_role_id: model.model_role_id,
      collision_body_role_id: collision.collision_body_role_id,
      model_instance_id: collision.model_instance_id,
      model_revision: collision.model_revision,
      collision_body_ids: Array.from({ length: bodyCount }, (_, index) => (
        `component/${target.controllable_component_id}/model/${collision.model_instance_id}`
        + `/body/${collision.collision_body_role_id}/collision/${index}`
      )),
    }],
    max_penetration_m: 0.001,
    terminal_window_start_fraction: 0.8,
  }];
  value.node_policies = [nodePolicy()];
  return value;
};

function nodePolicy(instance_id = 'root'): Row {
  return {
    instance_id,
    recovery_policy: { allow_intervention: false, max_retries: 1 },
    operator_stop_policy: 'hold',
  };
}

function context(value: Row) {
  const component = rows(value.component_role_bindings)[0];
  const target = rows(value.target_role_bindings)[0];
  const contact = rows(value.contact_grasp_bindings)[0];
  const resource = rows(target.resource_role_bindings)[0];
  const collision = rows(target.collision_body_role_bindings)[0];
  const capability = rows(target.capability_bindings)[0];
  const body = rows(contact.allowed_body_bindings)[0];
  const provenance = rows(row(value.provenance).source_records);
  return {
    component, target, contact, resource, collision, capability, body, provenance,
  };
}

function mutate(name: string, value: Row): void {
  const {
    component, target, contact, resource, collision, capability, body, provenance,
  } = context(value);
  const policy = rows(value.node_policies)[0];
  const recovery = row(policy.recovery_policy);
  const duplicate = (owner: Row, field: string): void => {
    const values = rows(owner[field]);
    owner[field] = [...values, structuredClone(values[0])];
  };
  switch (name) {
    case 'null': value.program_id = null; break;
    case 'wrong-type': value.target_role_bindings = {}; break;
    case 'identity': value.program_id = 'not portable'; break;
    case 'identity-bytes': value.program_id = 'a'.repeat(129); break;
    case 'text-bytes': rows(target.frame_role_bindings)[0].compiled_frame_name = 'é'.repeat(2049); break;
    case 'invalid-unicode': rows(target.frame_role_bindings)[0].compiled_frame_name = '\ud800'; break;
    case 'root-minimum': value.component_role_bindings = []; break;
    case 'root-maximum': value.component_role_bindings = Array(4097).fill(component); break;
    case 'nested-maximum': target.frame_role_bindings = Array(4097).fill(rows(target.frame_role_bindings)[0]); break;
    case 'provenance-maximum': row(value.provenance).source_records = Array(16385).fill(provenance[0]); break;
    case 'component-order': duplicate(value, 'component_role_bindings'); break;
    case 'model-order': duplicate(component, 'model_role_bindings'); break;
    case 'target-order': duplicate(value, 'target_role_bindings'); break;
    case 'frame-order': duplicate(target, 'frame_role_bindings'); break;
    case 'resource-order': duplicate(target, 'resource_role_bindings'); break;
    case 'collision-order': duplicate(target, 'collision_body_role_bindings'); break;
    case 'capability-order': duplicate(target, 'capability_bindings'); break;
    case 'contact-order': duplicate(value, 'contact_grasp_bindings'); break;
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
    case 'target-component': target.controllable_component_id = 'mismatch'; break;
    case 'contact-penetration': contact.max_penetration_m = 0; break;
    case 'contact-fraction': contact.terminal_window_start_fraction = 1.01; break;
    case 'contact-nonfinite': contact.max_penetration_m = Number.NaN; break;
    case 'body-minimum': contact.allowed_body_bindings = []; break;
    case 'body-order': duplicate(contact, 'allowed_body_bindings'); break;
    case 'generated-body-id': (body.collision_body_ids as string[])[0] = 'wrong'; break;
    case 'body-target': body.command_target_id = 'mismatch'; break;
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
  'resource-order', 'collision-order', 'capability-order', 'contact-order',
  'resource-size', 'resource-id-duplicate', 'resource-name-duplicate',
  'collision-name-duplicate', 'all-null-capability', 'target-component',
  'contact-penetration', 'contact-fraction', 'contact-nonfinite',
  'body-minimum', 'body-order', 'generated-body-id', 'body-target',
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

describe('generated Program Binding transport parser', () => {
  it('returns the exact native fixture reference with concrete nested rows', () => {
    const document = structuredClone(nativeFixture) as Row;
    const parsed = parseProgramBindingTransport(document, 'binding');
    expect(parsed).toBe(document);
    expect(parsed.component_role_bindings[0].component_role_id).toBeTypeOf('string');
    expect(parsed.target_role_bindings[0].capability_bindings).toBeInstanceOf(Array);
  });

  it('preserves absent and empty robot TCP inventories and rejects null', () => {
    const document = fixture();
    const target = rows(document.target_role_bindings)[0];
    delete target.robot_tcps;
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(Object.hasOwn(target, 'robot_tcps')).toBe(false);
    target.robot_tcps = [];
    expect(parseProgramBindingTransport(document, 'binding')).toBe(document);
    expect(target.robot_tcps).toEqual([]);
    target.robot_tcps = null;
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
    (value: Row) => { rows(context(value).component.model_role_bindings)[0].unknown = true; },
    (value: Row) => { context(value).target.unknown = true; },
    (value: Row) => { rows(context(value).target.frame_role_bindings)[0].unknown = true; },
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
    (value: Row) => { context(value).contact.unknown = true; },
    (value: Row) => { context(value).body.unknown = true; },
    (value: Row) => { row(value.provenance).unknown = true; },
    (value: Row) => { context(value).provenance[0].unknown = true; },
  ])('rejects every nested open shape', (mutation) => {
    const document = fixture();
    mutation(document);
    expect(() => parseProgramBindingTransport(document, 'binding')).toThrow(/closed record/);
  });
});

describe('logical endpoint transport', () => {
  function endpointFixture(): Row {
    const value = fixture();
    const target = rows(value.target_role_bindings)[0];
    const body = rows(target.collision_body_role_bindings)[0];
    const frame = rows(target.frame_role_bindings).find((candidate) => candidate.model_instance_id === body.model_instance_id)!;
    frame.contact_body_role_ids = [body.collision_body_role_id];
    const policy = rows(value.contact_grasp_bindings)[0];
    policy.bodies_from_motion_endpoint = true;
    policy.allowed_body_bindings = [];
    return value;
  }

  it('preserves the selected frame association and endpoint policy', () => {
    const value = endpointFixture();
    expect(parseProgramBindingTransport(value, 'binding')).toBe(value);
  });

  it.each(['missing_body', 'wrong_model', 'duplicate_body', 'false_selector', 'mixed_bodies'])('rejects %s', (fault) => {
    const value = endpointFixture();
    const target = rows(value.target_role_bindings)[0];
    const frame = rows(target.frame_role_bindings).find((candidate) => 'contact_body_role_ids' in candidate)!;
    const policy = rows(value.contact_grasp_bindings)[0];
    if (fault === 'missing_body') frame.contact_body_role_ids = ['absent'];
    else if (fault === 'wrong_model') frame.model_revision = `sha256:${'f'.repeat(64)}`;
    else if (fault === 'duplicate_body') {
      const roles = frame.contact_body_role_ids as string[];
      frame.contact_body_role_ids = [...roles, ...roles];
    } else if (fault === 'false_selector') policy.bodies_from_motion_endpoint = false;
    else policy.allowed_body_bindings = rows(fixture().contact_grasp_bindings)[0].allowed_body_bindings;
    expect(() => parseProgramBindingTransport(value, 'binding')).toThrow();
  });
});

describe('contact release transport', () => {
  function releaseFixture(binary: boolean, endpoint = true): Row {
    const value = fixture();
    const policy = rows(value.contact_grasp_bindings)[0];
    delete policy.max_penetration_m;
    delete policy.terminal_window_start_fraction;
    Object.assign(policy, {
      mode: 'contact_release',
      release_evidence_mode: binary ? 'binary_contact' : 'signed_penetration',
      enforce_max_start_penetration: !binary,
      max_start_penetration_m: binary ? 0 : 0.002,
      min_release_progress_m: 0,
      release_tolerance_m: binary ? 0 : 1e-5,
      require_strictly_decreasing_penetration: !binary,
      require_separation_by_end: true,
      allow_recontact: false,
    });
    if (endpoint) {
      policy.bodies_from_motion_endpoint = true;
      policy.allowed_body_bindings = [];
    }
    return value;
  }

  it.each([
    [false, false], [false, true], [true, false], [true, true],
  ])('accepts normalized binary=%s endpoint=%s release without rewriting it', (binary, endpoint) => {
    const value = releaseFixture(binary, endpoint);
    expect(parseProgramBindingTransport(value, 'binding')).toBe(value);
  });

  it.each([
    'wrong_mode', 'missing_mode', 'unknown', 'grasp_field', 'missing_release_field',
    'wrong_evidence', 'number_boolean', 'negative_metric', 'oversized_metric',
    'nonfinite_metric', 'binary_enforce', 'binary_strict', 'binary_metric',
  ])('rejects %s in a release binding', (fault) => {
    const binary = fault.startsWith('binary_');
    const value = releaseFixture(binary);
    const policy = rows(value.contact_grasp_bindings)[0];
    if (fault === 'wrong_mode') policy.mode = 'contact_grasp';
    else if (fault === 'missing_mode') delete policy.mode;
    else if (fault === 'unknown') policy.extra = true;
    else if (fault === 'grasp_field') policy.max_penetration_m = 0.002;
    else if (fault === 'missing_release_field') delete policy.require_separation_by_end;
    else if (fault === 'wrong_evidence') policy.release_evidence_mode = 'distance';
    else if (fault === 'number_boolean') policy.allow_recontact = 1;
    else if (fault === 'negative_metric') policy.max_start_penetration_m = -0.001;
    else if (fault === 'oversized_metric') policy.min_release_progress_m = 1000.001;
    else if (fault === 'nonfinite_metric') policy.release_tolerance_m = Number.POSITIVE_INFINITY;
    else if (fault === 'binary_enforce') policy.enforce_max_start_penetration = true;
    else if (fault === 'binary_strict') policy.require_strictly_decreasing_penetration = true;
    else policy.release_tolerance_m = 1e-5;
    expect(() => parseProgramBindingTransport(value, 'binding')).toThrow();
  });
});
