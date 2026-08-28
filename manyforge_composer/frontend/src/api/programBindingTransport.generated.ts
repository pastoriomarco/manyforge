/* GENERATED FILE -- program_binding_transport.v1.json; DO NOT EDIT. */
import type {
  CatalogRecordKind, ProgramBindingDocument,
} from '../types/programBindingTransport.generated';

const DIGEST = /^sha256:[0-9a-f]{64}$/;
const IDENTITY = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
const KINDS = new Set<CatalogRecordKind>([
  'model_asset', 'runtime_profile', 'component_type',
  'control_interface_contract', 'controller_topology', 'provider_manifest',
  'command_schema', 'capability_definition', 'contact_policy',
  'stop_evidence_policy', 'runtime_policy',
]);
const FIELDS = {
  binding: ['api_version', 'program_binding_id', 'program_id', 'program_revision', 'retained_bundle_id', 'graph_revision', 'node_environment', 'component_role_bindings', 'target_role_bindings', 'node_policies', 'contact_grasp_bindings', 'provenance'],
  node_environment: ['catalog_id', 'content_version', 'catalog_digest', 'environment_digest'],
  component: ['component_role_id', 'controllable_component_id', 'component_type_id', 'component_type_revision', 'model_role_bindings'],
  model: ['model_role_id', 'model_instance_id', 'model_revision'],
  target: ['target_role_id', 'component_role_id', 'controllable_component_id', 'command_target_id', 'frame_role_bindings', 'resource_role_bindings', 'collision_body_role_bindings', 'capability_bindings'],
  frame: ['frame_role_id', 'model_instance_id', 'model_revision', 'compiled_frame_name'],
  resource: ['resource_role_id', 'resource_ids', 'compiled_resource_names'],
  collision: ['collision_body_role_id', 'model_instance_id', 'model_revision', 'compiled_collision_body_names'],
  capability: ['capability_id', 'capability_definition_revision', 'control_binding', 'capability_provider_binding', 'command_schema'],
  node_policy: ['instance_id', 'modification_policy', 'recovery_policy', 'operator_stop_policy'],
  modification_policy: ['runtime_override', 'persistent_edit', 'intervention_delegate'],
  recovery_policy: ['allow_intervention', 'allowed_recovery_actions', 'max_retries', 'allowed_skills', 'persistence_ceiling'],
  control: ['control_binding_id', 'control_binding_revision'],
  provider: ['capability_provider_binding_id', 'capability_provider_binding_revision'],
  command_schema: ['command_schema_id', 'command_schema_revision'],
  contact: ['contact_policy_id', 'contact_policy_revision', 'runtime_object_id', 'allowed_body_bindings', 'max_penetration_m', 'terminal_window_start_fraction'],
  contact_body: ['component_role_id', 'target_role_id', 'controllable_component_id', 'command_target_id', 'model_role_id', 'collision_body_role_id', 'model_instance_id', 'model_revision', 'collision_body_ids'],
  provenance: ['binding_compiler_contract_digest', 'role_assignment_digest', 'source_records', 'resolved_output_records'],
  provenance_record: ['record_kind', 'record_id', 'revision'],
} as const;
const MODIFICATION_VALUES = new Set(['forbidden', 'human_approval', 'llm_allowed']);
const INTERVENTION_DELEGATES = new Set(['forbidden', 'human_only', 'llm_allowed', 'program_only']);
const RECOVERY_ACTIONS = new Set(['intervention', 'runtime_override']);
const PERSISTENCE_CEILINGS = new Set(['session_only', 'runtime_override', 'draft_patch']);
const STOP_POLICIES = new Set(['inherit', 'hold', 'continue']);
const COLLISION_BODY_ID = 'component/{controllable_component_id}/model/{model_instance_id}/body/{collision_body_role_id}/collision/{index}';
const MAX_ROWS = 4096;
const ROLE_MAX_ROWS = 4096;
const NODE_POLICY_MAX_ROWS = 100000;
const RECOVERY_ACTION_MAX = 2;
const SKILL_MAX = 256;
const SKILL_ID_MAX_BYTES = 128;
const RETRY_MAX = 255;
const PROVENANCE_MAX_ROWS = 16384;
const TEXT_MAX_BYTES = 4096;
const IDENTITY_MAX_BYTES = 128;
const BINDING_API_VERSION = 'manyforge.program_binding.v1';
type Row = Record<string, unknown>;

export class ProgramBindingTransportError extends Error {}
const bad = (path: string, expected: string): never => {
  throw new ProgramBindingTransportError(`${path}: ${expected}`);
};
const record = (value: unknown, keys: readonly string[], path: string): Row => {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) bad(path, 'closed record');
  const row = value as Row;
  const actual = Object.keys(row).sort();
  const wanted = [...keys].sort();
  if (actual.length !== wanted.length || actual.some((key, i) => key !== wanted[i])) bad(path, 'closed record');
  return row;
};
const array = (value: unknown, path: string, minimum = 0, maximum = MAX_ROWS): unknown[] => {
  if (!Array.isArray(value)) bad(path, 'bounded array');
  const result = value as unknown[];
  if (result.length < minimum || result.length > maximum) bad(path, 'bounded array');
  return result;
};
const text = (value: unknown, path: string, maximum = TEXT_MAX_BYTES): string => {
  if (typeof value !== 'string') bad(path, 'bounded non-empty string');
  const result = value as string;
  if (result.length < 1) bad(path, 'bounded non-empty string');
  try {
    encodeURIComponent(result);
  } catch {
    bad(path, 'valid Unicode text');
  }
  if (new TextEncoder().encode(result).length > maximum) bad(path, 'bounded non-empty string');
  return result;
};
const matchesExactly = (pattern: RegExp, value: string): boolean => (
  pattern.exec(value)?.[0] === value
);
const identity = (value: unknown, path: string): string => {
  const result = text(value, path, IDENTITY_MAX_BYTES);
  if (!matchesExactly(IDENTITY, result)) bad(path, 'portable identity');
  return result;
};
const digest = (value: unknown, path: string): string => {
  if (typeof value !== 'string') bad(path, 'lowercase SHA-256');
  const result = value as string;
  if (!matchesExactly(DIGEST, result)) bad(path, 'lowercase SHA-256');
  return result;
};
const number = (value: unknown, path: string): number => {
  if (typeof value !== 'number') bad(path, 'finite number');
  const result = value as number;
  if (!Number.isFinite(result)) bad(path, 'finite number');
  return result;
};
const sorted = <T>(rows: T[], key: (row: T) => string, path: string): void => {
  const keys = rows.map(key);
  if (keys.some((keyValue, i) => (i > 0 && keys[i - 1] >= keyValue))) bad(path, 'canonical sorted unique keys');
};
const strings = (value: unknown, path: string, asText = false): string[] => {
  const result = array(value, path, 1).map((item, i) => (
    asText ? text(item, `${path}[${i}]`) : identity(item, `${path}[${i}]`)
  ));
  if (new Set(result).size !== result.length) bad(path, 'duplicate-free values');
  return result;
};

const policyId = (value: unknown, path: string): string => {
  const result = identity(value, path);
  if (!/^[a-z][a-z0-9_.-]*$/.test(result)) bad(path, 'portable lower-case ID');
  if (new TextEncoder().encode(result).length > SKILL_ID_MAX_BYTES) {
    bad(path, 'bounded skill ID');
  }
  return result;
};
const nodePolicy = (value: unknown, path: string): Row => {
  const row = record(value, FIELDS.node_policy, path);
  policyId(row.instance_id, `${path}.instance_id`);
  const modification = record(row.modification_policy, FIELDS.modification_policy,
    `${path}.modification_policy`);
  if (!MODIFICATION_VALUES.has(modification.runtime_override as string)
      || !MODIFICATION_VALUES.has(modification.persistent_edit as string)
      || !INTERVENTION_DELEGATES.has(modification.intervention_delegate as string)) {
    bad(path, 'closed modification-policy enums');
  }
  const recovery = record(row.recovery_policy, FIELDS.recovery_policy,
    `${path}.recovery_policy`);
  if (typeof recovery.allow_intervention !== 'boolean') {
    bad(`${path}.recovery_policy.allow_intervention`, 'boolean');
  }
  const actions = array(recovery.allowed_recovery_actions,
    `${path}.recovery_policy.allowed_recovery_actions`, 0, RECOVERY_ACTION_MAX)
    .map((item, i) => {
      const action = text(
        item,
        `${path}.recovery_policy.allowed_recovery_actions[${i}]`,
        IDENTITY_MAX_BYTES,
      );
      if (!RECOVERY_ACTIONS.has(action)) {
        bad(`${path}.recovery_policy.allowed_recovery_actions[${i}]`, 'closed enum');
      }
      return action;
    });
  sorted(actions.map((item) => ({value: item})), (item) => item.value,
    `${path}.recovery_policy.allowed_recovery_actions`);
  const retries = recovery.max_retries;
  if (typeof retries !== 'number' || !Number.isInteger(retries)
      || retries < 0 || retries > RETRY_MAX) {
    bad(`${path}.recovery_policy.max_retries`, 'bounded integer');
  }
  const skills = array(recovery.allowed_skills,
    `${path}.recovery_policy.allowed_skills`, 0, SKILL_MAX).map((item, i) => {
      const skill = policyId(item, `${path}.recovery_policy.allowed_skills[${i}]`);
      return skill;
    });
  sorted(skills.map((item) => ({value: item})), (item) => item.value,
    `${path}.recovery_policy.allowed_skills`);
  if (!PERSISTENCE_CEILINGS.has(recovery.persistence_ceiling as string)
      || !STOP_POLICIES.has(row.operator_stop_policy as string)) {
    bad(path, 'closed recovery/Stop enums');
  }
  const intervention = actions.includes('intervention');
  if (recovery.allow_intervention !== intervention) bad(path, 'intervention cross-field rule');
  if (skills.length > 0 && !intervention) bad(path, 'allowed_skills requires intervention');
  if (actions.includes('runtime_override') && modification.runtime_override === 'forbidden') {
    bad(path, 'runtime_override requires modification permission');
  }
  if (recovery.persistence_ceiling === 'runtime_override'
      && modification.runtime_override === 'forbidden') {
    bad(path, 'runtime_override persistence requires modification permission');
  }
  if (recovery.persistence_ceiling === 'draft_patch'
      && modification.persistent_edit === 'forbidden') {
    bad(path, 'draft_patch persistence requires persistent-edit permission');
  }
  if (actions.length === 0 && (
      recovery.allow_intervention || retries !== 0 || skills.length !== 0
      || recovery.persistence_ceiling !== 'session_only')) {
    bad(path, 'non-default recovery policy requires an action');
  }
  if (skills.length > 0) {
    bad(`${path}.recovery_policy.allowed_skills`,
      'requires an exact revision-qualified retained skill referent');
  }
  if (modification.runtime_override === 'forbidden'
      && modification.persistent_edit === 'forbidden'
      && modification.intervention_delegate === 'forbidden'
      && actions.length === 0 && retries === 0 && skills.length === 0
      && recovery.persistence_ceiling === 'session_only'
      && row.operator_stop_policy === 'inherit') {
    bad(path, 'default node-policy rows must be omitted');
  }
  return row;
};

export function parseProgramBindingTransport(value: unknown, path: string): ProgramBindingDocument {
  const root = record(value, FIELDS.binding, path);
  if (root.api_version !== BINDING_API_VERSION) bad(`${path}.api_version`, BINDING_API_VERSION);
  digest(root.program_binding_id, `${path}.program_binding_id`);
  identity(root.program_id, `${path}.program_id`);
  ['program_revision', 'retained_bundle_id', 'graph_revision'].forEach(
    (field) => digest(root[field], `${path}.${field}`),
  );
  const env = record(root.node_environment,
    FIELDS.node_environment,
    `${path}.node_environment`);
  identity(env.catalog_id, `${path}.node_environment.catalog_id`);
  text(env.content_version, `${path}.node_environment.content_version`, IDENTITY_MAX_BYTES);
  digest(env.catalog_digest, `${path}.node_environment.catalog_digest`);
  digest(env.environment_digest, `${path}.node_environment.environment_digest`);

  const components = array(root.component_role_bindings, `${path}.component_role_bindings`, 1, ROLE_MAX_ROWS).map(
    (item, i) => {
      const cp = `${path}.component_role_bindings[${i}]`;
      const component = record(item, FIELDS.component, cp);
      ['component_role_id', 'controllable_component_id', 'component_type_id'].forEach(
        (field) => identity(component[field], `${cp}.${field}`),
      );
      digest(component.component_type_revision, `${cp}.component_type_revision`);
      const models = array(component.model_role_bindings, `${cp}.model_role_bindings`).map(
        (modelItem, mi) => {
          const mp = `${cp}.model_role_bindings[${mi}]`;
          const model = record(modelItem, FIELDS.model, mp);
          identity(model.model_role_id, `${mp}.model_role_id`);
          identity(model.model_instance_id, `${mp}.model_instance_id`);
          digest(model.model_revision, `${mp}.model_revision`);
          return model;
        },
      );
      sorted(models, (row) => row.model_role_id as string, `${cp}.model_role_bindings`);
      return component;
    },
  );
  sorted(components, (row) => row.component_role_id as string, `${path}.component_role_bindings`);
  const componentByRole = new Map(components.map((row) => [row.component_role_id, row]));

  const targets = array(root.target_role_bindings, `${path}.target_role_bindings`, 0, ROLE_MAX_ROWS).map(
    (item, i) => {
      const tp = `${path}.target_role_bindings[${i}]`;
      const target = record(item, FIELDS.target, tp);
      ['target_role_id', 'component_role_id', 'controllable_component_id', 'command_target_id'].forEach(
        (field) => identity(target[field], `${tp}.${field}`),
      );
      const component = componentByRole.get(target.component_role_id);
      if (!component || component.controllable_component_id !== target.controllable_component_id) bad(tp, 'target/component correspondence');
      const frames = array(target.frame_role_bindings, `${tp}.frame_role_bindings`).map((item2, j) => {
        const fp = `${tp}.frame_role_bindings[${j}]`;
        const frame = record(item2, FIELDS.frame, fp);
        identity(frame.frame_role_id, `${fp}.frame_role_id`); identity(frame.model_instance_id, `${fp}.model_instance_id`);
        digest(frame.model_revision, `${fp}.model_revision`); text(frame.compiled_frame_name, `${fp}.compiled_frame_name`);
        return frame;
      });
      sorted(frames, (row) => row.frame_role_id as string, `${tp}.frame_role_bindings`);
      const resources = array(target.resource_role_bindings, `${tp}.resource_role_bindings`).map((item2, j) => {
        const rp = `${tp}.resource_role_bindings[${j}]`;
        const resource = record(item2, FIELDS.resource, rp);
        identity(resource.resource_role_id, `${rp}.resource_role_id`);
        const ids = strings(resource.resource_ids, `${rp}.resource_ids`);
        const names = strings(resource.compiled_resource_names, `${rp}.compiled_resource_names`, true);
        if (ids.length !== names.length) bad(rp, 'equal correspondence vectors');
        return resource;
      });
      sorted(resources, (row) => row.resource_role_id as string, `${tp}.resource_role_bindings`);
      const collisions = array(target.collision_body_role_bindings, `${tp}.collision_body_role_bindings`).map((item2, j) => {
        const cp = `${tp}.collision_body_role_bindings[${j}]`;
        const collision = record(item2, FIELDS.collision, cp);
        identity(collision.collision_body_role_id, `${cp}.collision_body_role_id`);
        identity(collision.model_instance_id, `${cp}.model_instance_id`);
        digest(collision.model_revision, `${cp}.model_revision`);
        strings(collision.compiled_collision_body_names, `${cp}.compiled_collision_body_names`, true);
        return collision;
      });
      sorted(collisions, (row) => row.collision_body_role_id as string, `${tp}.collision_body_role_bindings`);
      const capabilities = array(target.capability_bindings, `${tp}.capability_bindings`).map((item2, j) => {
        const cp = `${tp}.capability_bindings[${j}]`;
        const capability = record(item2, FIELDS.capability, cp);
        identity(capability.capability_id, `${cp}.capability_id`);
        digest(capability.capability_definition_revision, `${cp}.capability_definition_revision`);
        type ReferenceRecord = 'control' | 'provider' | 'command_schema';
        const refs: [string, ReferenceRecord][] = [
          ['control_binding', 'control'],
          ['capability_provider_binding', 'provider'],
          ['command_schema', 'command_schema'],
        ];
        refs.forEach(([field, recordName]) => {
          if (capability[field] !== null) {
            const [id, revision] = FIELDS[recordName];
            const ref = record(capability[field], FIELDS[recordName], `${cp}.${field}`);
            identity(ref[id], `${cp}.${field}.${id}`); digest(ref[revision], `${cp}.${field}.${revision}`);
          }
        });
        if (capability.control_binding === null && capability.capability_provider_binding === null) bad(cp, 'control or provider resolution');
        return capability;
      });
      sorted(capabilities, (row) => row.capability_id as string, `${tp}.capability_bindings`);
      return target;
    },
  );
  sorted(targets, (row) => row.target_role_id as string, `${path}.target_role_bindings`);
  const targetByRole = new Map(targets.map((row) => [row.target_role_id, row]));

  const nodePolicies = array(root.node_policies, `${path}.node_policies`, 0, NODE_POLICY_MAX_ROWS)
    .map((item, i) => nodePolicy(item, `${path}.node_policies[${i}]`));
  sorted(nodePolicies, (row) => row.instance_id as string, `${path}.node_policies`);

  const contacts = array(root.contact_grasp_bindings, `${path}.contact_grasp_bindings`, 0, ROLE_MAX_ROWS).map((item, i) => {
    const pp = `${path}.contact_grasp_bindings[${i}]`;
    const policy = record(item, FIELDS.contact, pp);
    identity(policy.contact_policy_id, `${pp}.contact_policy_id`);
    digest(policy.contact_policy_revision, `${pp}.contact_policy_revision`);
    identity(policy.runtime_object_id, `${pp}.runtime_object_id`);
    const penetration = number(policy.max_penetration_m, `${pp}.max_penetration_m`);
    const fraction = number(policy.terminal_window_start_fraction, `${pp}.terminal_window_start_fraction`);
    if (penetration <= 0 || fraction < 0 || fraction > 1) bad(pp, 'contact numeric ranges');
    const bodies = array(policy.allowed_body_bindings, `${pp}.allowed_body_bindings`, 1).map((item2, j) => {
      const bp = `${pp}.allowed_body_bindings[${j}]`;
      const body = record(item2, FIELDS.contact_body, bp);
      ['component_role_id', 'target_role_id', 'controllable_component_id', 'command_target_id',
        'collision_body_role_id', 'model_instance_id'].forEach((field) => identity(body[field], `${bp}.${field}`));
      if (body.model_role_id !== null) identity(body.model_role_id, `${bp}.model_role_id`);
      digest(body.model_revision, `${bp}.model_revision`);
      const ids = strings(body.collision_body_ids, `${bp}.collision_body_ids`, true);
      const expected = ids.map((_, k) => COLLISION_BODY_ID
        .replace('{controllable_component_id}', body.controllable_component_id as string)
        .replace('{model_instance_id}', body.model_instance_id as string)
        .replace('{collision_body_role_id}', body.collision_body_role_id as string)
        .replace('{index}', String(k)));
      if (ids.some((id, k) => id !== expected[k])) bad(bp, 'generated collision body IDs');
      const target = targetByRole.get(body.target_role_id);
      if (!target) bad(bp, 'exact target correspondence');
      const resolvedTarget = target as Row;
      if (['component_role_id', 'controllable_component_id', 'command_target_id']
        .some((field) => resolvedTarget[field] !== body[field])) bad(bp, 'exact target correspondence');
      const collisionRows = resolvedTarget.collision_body_role_bindings as Row[];
      const collisions = collisionRows.filter((row) => row.collision_body_role_id === body.collision_body_role_id);
      if (collisions.length !== 1 || collisions[0].model_instance_id !== body.model_instance_id
          || collisions[0].model_revision !== body.model_revision
          || (collisions[0].compiled_collision_body_names as unknown[]).length !== ids.length) bad(bp, 'collision correspondence');
      if (body.model_role_id !== null) {
        const component = componentByRole.get(body.component_role_id)!;
        const models = component.model_role_bindings as Row[];
        if (models.filter((row) => row.model_role_id === body.model_role_id
          && row.model_instance_id === body.model_instance_id && row.model_revision === body.model_revision).length !== 1) bad(bp, 'model correspondence');
      }
      return body;
    });
    sorted(bodies, (row) => `${row.component_role_id}\u0000${row.target_role_id}\u0000${row.model_role_id === null ? '\u0000' : `\u0001${row.model_role_id}`}\u0000${row.collision_body_role_id}`, `${pp}.allowed_body_bindings`);
    return policy;
  });
  sorted(contacts, (row) => row.contact_policy_id as string, `${path}.contact_grasp_bindings`);
  const provenance = record(root.provenance, FIELDS.provenance, `${path}.provenance`);
  ['binding_compiler_contract_digest', 'role_assignment_digest']
    .forEach((field) => digest(provenance[field], `${path}.provenance.${field}`));
  ['source_records', 'resolved_output_records'].forEach((field) => {
    const records = array(provenance[field], `${path}.provenance.${field}`, 0, PROVENANCE_MAX_ROWS).map((item, i) => {
      const rp = `${path}.provenance.${field}[${i}]`;
      const source = record(item, FIELDS.provenance_record, rp);
      if (typeof source.record_kind !== 'string' || !KINDS.has(source.record_kind as CatalogRecordKind)) bad(`${rp}.record_kind`, 'CatalogRecordKind');
      identity(source.record_id, `${rp}.record_id`); digest(source.revision, `${rp}.revision`);
      return source;
    });
    sorted(records, (row) => `${row.record_kind}\u0000${row.record_id}\u0000${row.revision}`, `${path}.provenance.${field}`);
  });
  // Deliberately no browser hash: Headless and HeadlessHttpClient verify the
  // exact owner-issued document before forwarding; this synchronous parser is
  // on the 500-ms polling path and enforces every self-contained wire rule.
  return value as ProgramBindingDocument;
}
