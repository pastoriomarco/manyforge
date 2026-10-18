import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramFormulaReferences,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramValueSource,
} from '../types/api';
import {
  groupByNamespace,
  NODE_OUTCOME_NAMESPACE,
  NODE_OUTCOME_VALUES,
  INTERRUPT_NODE_OUTCOME_VALUES,
  INTERRUPT_STATE_NAMESPACE,
  INTERRUPT_STATE_VALUES,
} from './blackboardNamespaces';
import { hasSourcesWithin } from './valuePaths';

/**
 * Motion nodes that retry automatically, and the parameters that tune it. The
 * parameters are literal-only (contract item 12). Mirrors the backend's
 * AUTOMATIC_RETRY_NODE_IDS and AUTOMATIC_RETRY_LITERAL_PARAMETERS.
 */
export const AUTOMATIC_RETRY = {
  nodeIds: ['motion.move_to_joint_target', 'motion.move_to_pose', 'motion.follow_cartesian_path'],
  parameterIds: ['automatic_retries', 'automatic_retry_delay_s'],
} as const;

/** The two motion nodes with a per-node interrupt rule (contract 63). */
export const INTERRUPT_MOTION_NODE_IDS = ['motion.move_until_signal', 'motion.probe_search'] as const;

/**
 * The keys a Draft's nodes may read: its declared keys plus the system-owned
 * outcome key `node_outcome.<instance_id>` of every motion node (the plain
 * motion nodes and the two interrupt nodes; contract items 42, 46(f)) and the
 * read-only `interrupt_state.<rule_id>` key of every known global rule
 * (contracts 63, 64). All are declared by the Program validator, never
 * authored.
 */
export function programBlackboardKeys(
  program: Pick<ProgramAuthoringDocument, 'blackboard' | 'forest'>,
  globalRuleIds: readonly string[] = [],
): BlackboardKeyDeclaration[] {
  const keys = [...(program.blackboard?.keys ?? [])];
  const motion: readonly string[] = AUTOMATIC_RETRY.nodeIds;
  const interrupt: readonly string[] = INTERRUPT_MOTION_NODE_IDS;
  const pending = [...(program.forest ?? [])];
  for (let node = pending.pop(); node !== undefined; node = pending.pop()) {
    if (motion.includes(node.node_id) || interrupt.includes(node.node_id)) {
      const values = interrupt.includes(node.node_id)
        ? INTERRUPT_NODE_OUTCOME_VALUES : NODE_OUTCOME_VALUES;
      keys.push({
        key: `${NODE_OUTCOME_NAMESPACE}.${node.instance_id}`,
        type: 'string',
        access: 'read',
        persistence: 'run',
        description: `Last outcome of motion node ${node.instance_id}: ${values.join(' | ')}`,
      });
    }
    pending.push(...node.children);
  }
  for (const ruleId of new Set(globalRuleIds)) {
    keys.push({
      key: `${INTERRUPT_STATE_NAMESPACE}.${ruleId}`,
      type: 'string',
      access: 'read',
      persistence: 'persistent',
      description: `Current state of global interrupt rule ${ruleId}: ${INTERRUPT_STATE_VALUES.join(' | ')}`,
    });
  }
  return keys.sort((left, right) => (left.key < right.key ? -1 : left.key > right.key ? 1 : 0));
}

const RELATIVE_RETRY_REASON = 'Not retried automatically: relative to the robot\'s own position.';

// Literal-only parameters: no value-source binding is offered (contract items 12, 33).
const FORBIDDEN_DYNAMIC_PARAMETERS = new Set<string>([
  ...AUTOMATIC_RETRY.parameterIds,
  'preemption_priority',
  'preempted_result',
  'contact_policy_id',
  'model_endpoint_alias',
  'prompt_template_id',
  // Interrupt rule fields of the two interrupt motion nodes (schema04 §2).
  'interrupt_condition',
  'stop_mode',
  'activation_mode',
  'on_signal',
  'on_no_signal',
  'priority',
  'stop_entry_stage_id',
  'success_through_stage_id',
  // Node Mock simulation rows are literal-only (contract 133).
  'mock_simulation',
]);

export interface FlatProgramNode {
  node: ProgramTreeNode;
  parentId: string | null;
  index: number;
  depth: number;
}

export interface ProgramBindingCandidate {
  nodeId: string;
  nodeLabel: string;
  fieldName: string;
}

export interface ProgramParameterUsage {
  nodeId: string;
  nodeLabel: string;
  fieldName: string;
}

export interface BlackboardUsage extends ProgramParameterUsage {
  field: string | null;
}

/** One formula that reads a declaration: a node field's or a computed key's. */
export type FormulaUsage =
  | { kind: 'node'; nodeId: string; path: string }
  | { kind: 'computed_key'; key: string };

export interface FormulaUsages {
  /** By parameter ID. */
  parameters: Record<string, FormulaUsage[]>;
  /** By blackboard key. */
  keys: Record<string, FormulaUsage[]>;
}

export const PROGRAM_MOTION_FIELDS = [
  'velocity_scale',
  'acceleration_scale',
  'translation_tol_m',
  'max_blend_deviation_rad',
] as const;

export const PROGRAM_MOTION_CEILING_FIELDS = [
  'velocity_scale',
  'acceleration_scale',
] as const;

export type ProgramMotionField = typeof PROGRAM_MOTION_FIELDS[number];

export interface ProgramMotionValues {
  velocity_scale: number;
  acceleration_scale: number;
  translation_tol_m: number;
  max_blend_deviation_rad: number;
}

export interface ProgramMotionConfiguration {
  bindings: Record<ProgramMotionField, string>;
  values: ProgramMotionValues;
  usageCounts: Record<ProgramMotionField, number>;
  applicableCounts: Record<ProgramMotionField, number>;
}

export type ProgramMotionConfigurationResult =
  | { available: true; configuration: ProgramMotionConfiguration }
  | { available: false; reason: string };

export interface ProgramSemanticChoice {
  value: string;
  label: string;
  group?: string;
}

export type ProgramSemanticCreateTarget =
  | 'blackboard_key'
  | 'program_parameter'
  | 'program_requirements'
  | 'scene_resource';

/**
 * Resolve Program-wide motion semantics from the exact catalog fields and
 * their authored Program-parameter sources. Parameter IDs are deliberately
 * not part of this contract: a Program may call its velocity parameter
 * `cycle_speed` as long as motion-node velocity_scale fields bind to it.
 */
export function deriveProgramMotionConfiguration(
  program: Pick<ProgramAuthoringDocument, 'parameters' | 'forest'>,
  entries: CatalogEntry[],
): ProgramMotionConfigurationResult {
  if (entries.length === 0) {
    return {
      available: false,
      reason: 'The node catalog is unavailable, so motion bindings cannot be identified.',
    };
  }
  const catalog = new Map(entries.map((entry) => [entry.id, entry]));
  const references = new Map<ProgramMotionField, Set<string>>(
    PROGRAM_MOTION_FIELDS.map((field) => [field, new Set<string>()]),
  );
  const usageCounts = Object.fromEntries(
    PROGRAM_MOTION_FIELDS.map((field) => [field, 0]),
  ) as Record<ProgramMotionField, number>;
  const applicableCounts = Object.fromEntries(
    PROGRAM_MOTION_FIELDS.map((field) => [field, 0]),
  ) as Record<ProgramMotionField, number>;
  const unknownNodes = new Set<string>();
  const declarations = new Map(
    (program.parameters ?? []).map((parameter) => [parameter.parameter_id, parameter]),
  );

  for (const { node } of flattenProgramForest(program.forest)) {
    const entry = catalog.get(node.node_id);
    if (!entry) {
      unknownNodes.add(node.node_id);
      continue;
    }
    const fields = new Set(entry.parameters.map((parameter) => parameter.id));
    for (const field of PROGRAM_MOTION_FIELDS) {
      if (!fields.has(field)) continue;
      applicableCounts[field] += 1;
      const source = node.value_sources?.[field];
      if (source?.kind !== 'program_parameter') continue;
      references.get(field)?.add(source.reference);
      usageCounts[field] += 1;
    }
  }

  if (unknownNodes.size > 0) {
    return {
      available: false,
      reason: `Catalog definitions are missing for ${[...unknownNodes].sort().join(', ')}.`,
    };
  }
  const missingFields = PROGRAM_MOTION_FIELDS.filter(
    (field) => applicableCounts[field] === 0 && !declarations.has(field),
  );
  if (missingFields.length > 0) {
    return {
      available: false,
      reason: `This Program has no catalog motion fields for ${missingFields.join(', ')}.`,
    };
  }

  const bindings = {} as Record<ProgramMotionField, string>;
  for (const field of PROGRAM_MOTION_FIELDS) {
    const candidates = references.get(field) ?? new Set<string>();
    if (candidates.size === 0 && applicableCounts[field] === 0 && declarations.has(field)) {
      bindings[field] = field;
      continue;
    }
    if (candidates.size !== 1) {
      return {
        available: false,
        reason: candidates.size === 0
          ? `No ${field} field is bound to a Program parameter.`
          : `${field} fields use more than one Program parameter.`,
      };
    }
    bindings[field] = [...candidates][0];
  }
  if (new Set(Object.values(bindings)).size !== PROGRAM_MOTION_FIELDS.length) {
    return {
      available: false,
      reason: 'Program motion defaults must use distinct Program parameters.',
    };
  }

  const values = {} as ProgramMotionValues;
  for (const field of PROGRAM_MOTION_FIELDS) {
    const declaration = declarations.get(bindings[field]);
    if (
      declaration === undefined
      || !['float64', 'int64'].includes(declaration.type)
      || typeof declaration.default !== 'number'
      || !Number.isFinite(declaration.default)
    ) {
      return {
        available: false,
        reason: `${field} is not bound to a finite numeric Program parameter.`,
      };
    }
    values[field] = declaration.default;
  }
  return {
    available: true,
    configuration: { bindings, values, usageCounts, applicableCounts },
  };
}

/** Program motion defaults that also seed manual Cartesian moves. */
export const PROGRAM_MANUAL_CARTESIAN_FIELDS: readonly ProgramMotionField[] = [
  'translation_tol_m',
  'max_blend_deviation_rad',
];

/** What makes one Program parameter a Program motion default. */
export interface ProgramMotionDefaultRole {
  field: ProgramMotionField;
  /** Motion-node fields bound to it, out of all that accept this field. */
  bound: number;
  applicable: number;
  /** Velocity and acceleration: node values may not exceed it. */
  ceiling: boolean;
  /** Also the default for manual Cartesian moves. */
  manualDefault: boolean;
}

/**
 * Program parameters acting as Program motion defaults, by parameter ID,
 * from the same resolver as the Motion defaults settings and the manual
 * Cartesian defaults. Empty when that resolver cannot identify them.
 */
export function programMotionDefaultRoles(
  result: ProgramMotionConfigurationResult,
): Map<string, ProgramMotionDefaultRole> {
  const roles = new Map<string, ProgramMotionDefaultRole>();
  if (!result.available) return roles;
  const { bindings, usageCounts, applicableCounts } = result.configuration;
  for (const field of PROGRAM_MOTION_FIELDS) {
    roles.set(bindings[field], {
      field,
      bound: usageCounts[field],
      applicable: applicableCounts[field],
      ceiling: (PROGRAM_MOTION_CEILING_FIELDS as readonly string[]).includes(field),
      manualDefault: PROGRAM_MANUAL_CARTESIAN_FIELDS.includes(field),
    });
  }
  return roles;
}

function jsonRecord(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value
    : null;
}

function jsonArray(value: JsonValue | undefined): JsonValue[] {
  return Array.isArray(value) ? value : [];
}

function compatibleBlackboardKeys(
  entry: CatalogEntry,
  parameterId: string,
  keys: BlackboardKeyDeclaration[],
): BlackboardKeyDeclaration[] {
  const reads = entry.blackboard.reads.filter(
    (binding) => binding.keyParameter === parameterId,
  );
  const writes = entry.blackboard.writes.filter(
    (binding) => binding.keyParameter === parameterId,
  );
  const expectedTypes = new Set([...reads, ...writes].map((binding) => binding.valueType));
  return keys.filter((key) => (
    (expectedTypes.size === 0 || expectedTypes.has(key.type))
    && (reads.length === 0 || key.access !== 'write')
    && (writes.length === 0 || (key.access !== 'read' && key.expression === undefined))
  ));
}

/**
 * Project one semantic field's choices from the immutable catalog plus the
 * exact current authoring Draft. This is presentation data only; the backend
 * validates the submitted identity again under the Draft fence.
 */
export function programSemanticChoices(
  schema: CatalogJsonSchema,
  node: ProgramTreeNode,
  entry: CatalogEntry,
  parameterId: string,
  program: ProgramAuthoringDocument,
  globalRuleIds: readonly string[] = [],
): ProgramSemanticChoice[] {
  const semanticType = schema['x-manyforge-semantic-type'];
  const choices: ProgramSemanticChoice[] = [];
  if (semanticType === 'parameter_ref') {
    for (const parameter of program.parameters ?? []) {
      choices.push({
        value: parameter.parameter_id,
        label: parameter.parameter_id,
        group: 'Program parameters',
      });
    }
  } else if (semanticType === 'blackboard_key') {
    const compatible = compatibleBlackboardKeys(entry, parameterId, programBlackboardKeys(program, globalRuleIds));
    for (const group of groupByNamespace(compatible, (key) => key.key)) {
      for (const key of group.items) {
        choices.push({ value: key.key, label: key.key, group: group.label });
      }
    }
  } else if (semanticType === 'scene_resource') {
    for (const resource of program.scene ?? []) {
      choices.push({
        value: resource.scene_resource_id,
        label: resource.scene_resource_id,
        group: 'Scene resources',
      });
    }
  } else if (semanticType === 'component_role') {
    for (const role of Object.keys(program.component_roles)) {
      choices.push({ value: role, label: role, group: 'Component roles' });
    }
  } else if (semanticType === 'target_role') {
    for (const role of Object.keys(program.target_roles)) {
      choices.push({ value: role, label: role, group: 'Target roles' });
    }
  } else {
    const roleField = {
      frame_role: 'required_frame_roles',
      resource_role: 'required_resource_roles',
      collision_body_role: 'required_collision_body_roles',
      model_role: 'required_model_roles',
    }[semanticType ?? ''];
    if (roleField) {
      const scopeParameter = schema['x-manyforge-semantic-scope-parameter'];
      const selectedScope = scopeParameter === undefined
        ? null
        : node.parameters[scopeParameter]
          ?? entry.parameters.find((parameter) => parameter.id === scopeParameter)?.default;
      for (const [targetId, rawTarget] of Object.entries(program.target_roles)) {
        if (typeof selectedScope === 'string' && targetId !== selectedScope) continue;
        const target = jsonRecord(rawTarget);
        const values = target?.[roleField];
        if (!Array.isArray(values)) continue;
        for (const role of values) {
          if (typeof role === 'string') {
            choices.push({ value: role, label: role, group: targetId });
          }
        }
      }
    }
  }
  // Keys keep their namespace order: root first, node outcomes last.
  if (semanticType === 'blackboard_key') return choices;
  const unique = new Map(choices.map((choice) => [choice.value, choice]));
  return [...unique.values()].sort((left, right) =>
    (left.group ?? '').localeCompare(right.group ?? '')
      || left.label.localeCompare(right.label));
}

export function programSemanticCreateTarget(
  semanticType: string | undefined,
): ProgramSemanticCreateTarget | null {
  if (semanticType === 'blackboard_key') return 'blackboard_key';
  if (semanticType === 'parameter_ref') return 'program_parameter';
  if (semanticType === 'scene_resource') return 'scene_resource';
  if (semanticType === 'component_role' || semanticType === 'target_role') {
    return 'program_requirements';
  }
  return null;
}

export function programSemanticExpectedType(
  entry: CatalogEntry,
  parameterId: string,
  semanticType: string | undefined,
): string | null {
  if (semanticType === 'scene_resource') return 'scene_resource';
  if (semanticType !== 'blackboard_key') return null;
  const types = new Set([
    ...entry.blackboard.reads,
    ...entry.blackboard.writes,
  ].filter((binding) => binding.keyParameter === parameterId)
    .map((binding) => binding.valueType));
  return types.size === 1 ? [...types][0] as string : null;
}

export function programSemanticExpectedAccess(
  entry: CatalogEntry,
  parameterId: string,
): 'read' | 'write' | 'read_write' | null {
  const reads = entry.blackboard.reads.some(
    (binding) => binding.keyParameter === parameterId,
  );
  const writes = entry.blackboard.writes.some(
    (binding) => binding.keyParameter === parameterId,
  );
  if (reads && writes) return 'read_write';
  if (reads) return 'read';
  if (writes) return 'write';
  return null;
}

export function flattenProgramTree(root: ProgramTreeNode): FlatProgramNode[] {
  return flattenProgramForest([root]);
}

export function flattenProgramForest(roots: ProgramTreeNode[]): FlatProgramNode[] {
  const result: FlatProgramNode[] = [];
  const visit = (
    node: ProgramTreeNode,
    parentId: string | null,
    index: number,
    depth: number,
  ) => {
    result.push({ node, parentId, index, depth });
    node.children.forEach((child, childIndex) =>
      visit(child, node.instance_id, childIndex, depth + 1),
    );
  };
  roots.forEach((root, index) => visit(root, null, index, 0));
  return result;
}

/** Draft-only inheritance preview; runtime uses the backend's captured selection. */
export function inheritedProgramExecutionGroups(
  roots: ProgramTreeNode[],
  instanceId: string | null,
): string[] {
  if (instanceId === null) return [];
  const visit = (node: ProgramTreeNode, inherited: string[]): string[] | null => {
    if (node.instance_id === instanceId) return inherited;
    const groups = Array.isArray(node.execution_groups) ? node.execution_groups
      : node.execution_groups === 'all' ? [] : inherited;
    for (const child of node.children) {
      const found = visit(child, groups);
      if (found !== null) return found;
    }
    return null;
  };
  for (const root of roots) {
    const found = visit(root, []);
    if (found !== null) return found;
  }
  return [];
}

/** Resolve one backend diagnostic path to its retained node, without deriving validity. */
export function programDiagnosticNodeId(
  roots: ProgramTreeNode[],
  path: string,
): string | null {
  const rootMatch = /^forest\[(\d+)]/.exec(path);
  if (!rootMatch) return null;
  let node = roots[Number(rootMatch[1])];
  if (!node) return null;
  let remainder = path.slice(rootMatch[0].length);
  while (remainder.startsWith('.children[')) {
    const childMatch = /^\.children\[(\d+)]/.exec(remainder);
    if (!childMatch) return null;
    node = node.children[Number(childMatch[1])];
    if (!node) return null;
    remainder = remainder.slice(childMatch[0].length);
  }
  return node.instance_id;
}

function flattenProgramSource(
  source: ProgramTreeNode | ProgramTreeNode[],
): FlatProgramNode[] {
  return Array.isArray(source)
    ? flattenProgramForest(source)
    : flattenProgramTree(source);
}

export function nextProgramInstanceId(
  nodeId: string,
  existingIds: Iterable<string>,
): string {
  const stem = nodeId
    .toLocaleLowerCase()
    .replace(/[^a-z0-9_.-]+/g, '_')
    .replace(/^[^a-z]+/, '')
    .replace(/_+$/g, '') || 'node';
  const existing = new Set(existingIds);
  if (!existing.has(stem)) return stem;
  let suffix = 2;
  while (existing.has(`${stem}_${suffix}`)) suffix += 1;
  return `${stem}_${suffix}`;
}

export function catalogDefaultParameters(entry: CatalogEntry): Record<string, JsonValue> {
  return Object.fromEntries(
    entry.parameters
      .filter((parameter) => parameter.hasDefault)
      .map((parameter) => [parameter.id, parameter.default as JsonValue]),
  );
}

export function schemaAcceptsProgramType(
  schema: CatalogJsonSchema,
  programType: string,
): boolean {
  const semanticType = schema['x-manyforge-semantic-type'];
  if (semanticType === 'scene_resource') return programType === 'scene_resource';
  if (semanticType) return false;
  // A value of one of several Program types (a prompt-template variable);
  // a key-typed value is narrowed to its key's type before it gets here.
  if (schema.anyOf !== undefined && schema['x-manyforge-value-type-from-key'] === undefined) {
    return schema.anyOf.some((branch) => schemaAcceptsProgramType(branch, programType));
  }
  if (programType === 'float64') return schema.type === 'number';
  if (programType === 'int64') return schema.type === 'integer';
  if (programType === 'bool') return schema.type === 'boolean';
  if (programType === 'string') return schema.type === 'string';
  if (programType === 'string_list') {
    return schema.type === 'array' && schema.items?.type === 'string';
  }
  if (programType === 'cartesian_path') {
    return schema.type === 'array'
      && schema.items?.type === 'object'
      && schema.items.properties?.position_m !== undefined
      && schema.items.properties?.orientation !== undefined;
  }
  if (programType === 'coordinate_position') {
    return schema.type === 'object'
      && schema.properties?.kind !== undefined
      && schema.properties?.value_m !== undefined
      && schema.properties?.value_rad !== undefined;
  }
  if (programType === 'stored_pose') {
    return schema.type === 'object'
      && schema.properties?.position_m !== undefined
      && schema.properties?.orientation !== undefined;
  }
  return false;
}

function selectorParameterIds(entry: CatalogEntry): Set<string> {
  return new Set(entry.requiredCapabilities.flatMap((requirement) => [
    requirement.targetParameter,
    requirement.componentParameter,
    requirement.whenParameter,
  ].filter((value): value is string => value !== undefined)));
}

/** Whether any value of this parameter may come from a parameter, a key or a formula; effect selectors never do. */
export function catalogParameterSourceable(entry: CatalogEntry, parameterId: string): boolean {
  return entry.parameters.some((field) => field.id === parameterId)
    && !FORBIDDEN_DYNAMIC_PARAMETERS.has(parameterId)
    && !selectorParameterIds(entry).has(parameterId);
}

export function collectProgramBindingCandidates(
  root: ProgramTreeNode | ProgramTreeNode[],
  entries: CatalogEntry[],
  parameter: ProgramParameterDeclaration,
): ProgramBindingCandidate[] {
  const catalog = new Map(entries.map((entry) => [entry.id, entry]));
  const candidates: ProgramBindingCandidate[] = [];
  for (const { node } of flattenProgramSource(root)) {
    const entry = catalog.get(node.node_id);
    if (!entry) continue;
    for (const field of entry.parameters) {
      if (
        hasSourcesWithin(node.value_sources, field.id)
        || !catalogParameterSourceable(entry, field.id)
        || !schemaAcceptsProgramType(field.schema, parameter.type)
      ) {
        continue;
      }
      candidates.push({
        nodeId: node.instance_id,
        nodeLabel: node.instance_id,
        fieldName: field.id,
      });
    }
  }
  return candidates.sort((left, right) =>
    left.nodeLabel === right.nodeLabel
      ? left.fieldName.localeCompare(right.fieldName)
      : left.nodeLabel.localeCompare(right.nodeLabel),
  );
}

export function collectProgramParameterUsages(
  root: ProgramTreeNode | ProgramTreeNode[],
): Record<string, ProgramParameterUsage[]> {
  const usages = new Map<string, ProgramParameterUsage[]>();
  for (const { node } of flattenProgramSource(root)) {
    for (const [fieldName, source] of Object.entries(node.value_sources ?? {})) {
      if (source.kind !== 'program_parameter') continue;
      usages.set(source.reference, [
        ...(usages.get(source.reference) ?? []),
        {
          nodeId: node.instance_id,
          nodeLabel: node.instance_id,
          fieldName,
        },
      ]);
    }
  }
  return Object.fromEntries(
    [...usages.entries()].map(([parameterId, rows]) => [
      parameterId,
      rows.sort((left, right) =>
        left.nodeLabel === right.nodeLabel
          ? left.fieldName.localeCompare(right.fieldName)
          : left.nodeLabel.localeCompare(right.nodeLabel),
      ),
    ]),
  );
}

export function collectBlackboardUsages(
  root: ProgramTreeNode | ProgramTreeNode[],
): Record<string, BlackboardUsage[]> {
  const usages = new Map<string, BlackboardUsage[]>();
  for (const { node } of flattenProgramSource(root)) {
    for (const [fieldName, source] of Object.entries(node.value_sources ?? {})) {
      if (source.kind !== 'blackboard_key' && source.kind !== 'blackboard_field') {
        continue;
      }
      usages.set(source.reference, [
        ...(usages.get(source.reference) ?? []),
        {
          nodeId: node.instance_id,
          nodeLabel: node.instance_id,
          fieldName,
          field: source.kind === 'blackboard_field' ? source.field ?? null : null,
        },
      ]);
    }
  }
  return Object.fromEntries(
    [...usages.entries()].map(([key, rows]) => [
      key,
      rows.sort((left, right) =>
        left.nodeLabel === right.nodeLabel
          ? left.fieldName.localeCompare(right.fieldName)
          : left.nodeLabel.localeCompare(right.nodeLabel),
      ),
    ]),
  );
}

/**
 * Every Draft reference that keeps each component role, one label per use. A
 * role with no label is free to remove. Counts what the backend validator
 * counts: node parameters of the component_role semantic type, target roles,
 * scene component frames, and contact policy component_model_body selectors.
 */
export function collectComponentRoleUsages(
  program: ProgramAuthoringDocument,
  catalogEntries: CatalogEntry[],
): Map<string, string[]> {
  const usages = new Map<string, Set<string>>();
  const add = (role: JsonValue | undefined, label: string) => {
    if (typeof role !== 'string') return;
    usages.set(role, (usages.get(role) ?? new Set<string>()).add(label));
  };
  const catalog = new Map(catalogEntries.map((entry) => [entry.id, entry]));
  for (const { node } of flattenProgramForest(program.forest)) {
    const entry = catalog.get(node.node_id);
    for (const field of entry?.parameters ?? []) {
      if (field.schema['x-manyforge-semantic-type'] === 'component_role') {
        add(node.parameters[field.id], `node ${node.instance_id}`);
      }
    }
  }
  for (const [targetId, target] of Object.entries(program.target_roles)) {
    add(jsonRecord(target)?.component_role, `target role ${targetId}`);
  }
  for (const resource of program.scene ?? []) {
    const frame = resource.pose.frame_ref;
    if (frame.kind === 'component_frame') {
      add(frame.component_role, `scene frame ${resource.scene_resource_id}`);
    }
  }
  for (const policy of program.contact_policies ?? []) {
    const record = jsonRecord(policy);
    const policyId = typeof record?.policy_id === 'string' ? record.policy_id : 'unnamed';
    const label = `contact policy ${policyId}`;
    for (const zone of jsonArray(record?.zones)) {
      for (const pair of jsonArray(jsonRecord(zone)?.body_pairs)) {
        for (const side of ['body_a', 'body_b']) {
          const body = jsonRecord(jsonRecord(pair)?.[side]);
          if (body?.kind === 'component_model_body') {
            add(body.component_role_id, label);
          }
        }
      }
    }
  }
  return new Map([...usages].map(([role, labels]) => [role, [...labels]]));
}

/** The title of a Remove button guarded by its usages: the usages it would break, or the plain action when free. */
export function removeGuardTitle(usages: readonly string[], freeTitle: string): string {
  return usages.length > 0 ? usages.join(', ') : freeTitle;
}

/**
 * Why a motion node's automatic-retry field is locked, or null (contract items
 * 7 and 13). A literal relative joint move is never retried; a relative pose or
 * path is not retried unless it names a target_reference, literal or bound. A
 * bound target_mode is resolved per attempt by the runner, so nothing locks.
 */
export function automaticRetryLockReason(
  nodeId: string,
  node: Pick<ProgramTreeNode, 'parameters' | 'value_sources'>,
  parameterId: string,
): string | null {
  if (!(AUTOMATIC_RETRY.parameterIds as readonly string[]).includes(parameterId)) return null;
  if (!(AUTOMATIC_RETRY.nodeIds as readonly string[]).includes(nodeId)) return null;
  if (node.value_sources?.target_mode !== undefined) return null;
  if (node.parameters.target_mode !== 'relative') return null;
  const hasReference = (node.parameters.target_reference ?? node.value_sources?.target_reference ?? null) !== null;
  return nodeId === 'motion.move_to_joint_target' || !hasReference ? RELATIVE_RETRY_REASON : null;
}

/** Group the backend's per-formula names by the declaration each one reads. */
export function collectFormulaUsages(references: ProgramFormulaReferences): FormulaUsages {
  const usages: FormulaUsages = { parameters: {}, keys: {} };
  const add = (names: string[], usage: FormulaUsage) => {
    for (const name of names) {
      const separator = name.indexOf('.');
      const group = name.slice(0, separator) === 'param' ? usages.parameters : usages.keys;
      const id = name.slice(separator + 1);
      group[id] = [...(group[id] ?? []), usage];
    }
  };
  for (const [key, names] of Object.entries(references.computedKeys)) {
    add(names, { kind: 'computed_key', key });
  }
  for (const [nodeId, formulas] of Object.entries(references.nodes)) {
    for (const [path, names] of Object.entries(formulas)) {
      add(names, { kind: 'node', nodeId, path });
    }
  }
  return usages;
}

export function formulaUsageLabel(usage: FormulaUsage): string {
  return usage.kind === 'node'
    ? `formula in ${usage.nodeId} at ${usage.path}`
    : `formula of key ${usage.key}`;
}

export function resolveProgramParameterLiteralValue(
  source: ProgramValueSource | undefined,
  currentLiteral: JsonValue | undefined,
  catalogDefault: JsonValue | undefined,
  programParameters: ProgramParameterDeclaration[],
): JsonValue | undefined {
  if (source?.kind !== 'program_parameter') {
    return currentLiteral ?? catalogDefault;
  }
  const declaration = programParameters.find(
    (parameter) => parameter.parameter_id === source.reference,
  );
  return declaration?.default ?? catalogDefault;
}
