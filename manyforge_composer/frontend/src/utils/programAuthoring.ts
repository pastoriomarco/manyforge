import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramValueSource,
} from '../types/api';

const FORBIDDEN_DYNAMIC_PARAMETERS = new Set([
  'contact_policy_id',
  'model_endpoint_alias',
  'prompt_template_id',
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

function jsonRecord(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value
    : null;
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
    && (writes.length === 0 || key.access !== 'read')
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
    for (const key of compatibleBlackboardKeys(
      entry,
      parameterId,
      program.blackboard?.keys ?? [],
    )) {
      choices.push({ value: key.key, label: key.key, group: key.type });
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
  if (programType === 'float64') return schema.type === 'number';
  if (programType === 'int64') return schema.type === 'integer';
  if (programType === 'bool') return schema.type === 'boolean';
  if (programType === 'string') return schema.type === 'string';
  if (programType === 'string_list') {
    return schema.type === 'array' && schema.items?.type === 'string';
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

export function catalogFieldAcceptsValueSource(
  entry: CatalogEntry,
  fieldName: string,
  sourceType: string,
): boolean {
  const parameter = entry.parameters.find((field) => field.id === fieldName);
  return parameter !== undefined
    && !FORBIDDEN_DYNAMIC_PARAMETERS.has(fieldName)
    && !selectorParameterIds(entry).has(fieldName)
    && schemaAcceptsProgramType(parameter.schema, sourceType);
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
    const selectors = selectorParameterIds(entry);
    for (const field of entry.parameters) {
      if (
        node.value_sources?.[field.id] !== undefined
        || FORBIDDEN_DYNAMIC_PARAMETERS.has(field.id)
        || selectors.has(field.id)
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
