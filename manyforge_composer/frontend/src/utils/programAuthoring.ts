import type {
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
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

export function flattenProgramTree(root: ProgramTreeNode): FlatProgramNode[] {
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
  visit(root, null, 0, 0);
  return result;
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
  root: ProgramTreeNode,
  entries: CatalogEntry[],
  parameter: ProgramParameterDeclaration,
): ProgramBindingCandidate[] {
  const catalog = new Map(entries.map((entry) => [entry.id, entry]));
  const candidates: ProgramBindingCandidate[] = [];
  for (const { node } of flattenProgramTree(root)) {
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
  root: ProgramTreeNode,
): Record<string, ProgramParameterUsage[]> {
  const usages = new Map<string, ProgramParameterUsage[]>();
  for (const { node } of flattenProgramTree(root)) {
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
  root: ProgramTreeNode,
): Record<string, BlackboardUsage[]> {
  const usages = new Map<string, BlackboardUsage[]>();
  for (const { node } of flattenProgramTree(root)) {
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
