import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramTreeNode,
} from '../types/api';
import { schemaAcceptsProgramType } from './programAuthoring';

/**
 * Nodes whose `value` takes the type of the key they name (Set value, Check
 * value, Wait for value). The catalog projects such a parameter as `anyOf`
 * the accepted types plus `x-manyforge-value-type-from-key`, naming the
 * parameter that selects the key.
 */
export interface KeyValueTyping {
  keyParameter: string;
  key: BlackboardKeyDeclaration | null;
  /** The accepted branch for the key's type; null until a usable key is chosen. */
  schema: CatalogJsonSchema | null;
  issue: string | null;
}

const NUMERIC_TYPES = new Set(['int64', 'float64']);
const ALL_OPERATORS = ['==', '!=', '<', '<=', '>', '>='] as const;
const EQUALITY_OPERATORS = ['==', '!='] as const;

export function keyValueTyping(
  schema: CatalogJsonSchema,
  node: ProgramTreeNode,
  keys: readonly BlackboardKeyDeclaration[],
): KeyValueTyping | null {
  const keyParameter = schema['x-manyforge-value-type-from-key'];
  if (keyParameter === undefined) return null;
  const name = node.parameters[keyParameter];
  if (typeof name !== 'string' || name === '') {
    return { keyParameter, key: null, schema: null, issue: `Choose the ${keyParameter} first; the value takes its type.` };
  }
  const key = keys.find((row) => row.key === name) ?? null;
  if (key === null) {
    return { keyParameter, key: null, schema: null, issue: `Key ${name} is not declared.` };
  }
  const branch = schema.anyOf?.find((candidate) => schemaAcceptsProgramType(candidate, key.type)) ?? null;
  return {
    keyParameter,
    key,
    schema: branch,
    issue: branch === null ? `${key.type} keys cannot be used by this node.` : null,
  };
}

function keyValueParameter(entry: CatalogEntry): string | null {
  return entry.parameters.find((parameter) =>
    parameter.schema['x-manyforge-value-type-from-key'] !== undefined)?.schema[
    'x-manyforge-value-type-from-key'] ?? null;
}

function keyTypeOf(
  entry: CatalogEntry,
  node: ProgramTreeNode,
  keys: readonly BlackboardKeyDeclaration[],
): string | null {
  const keyParameter = keyValueParameter(entry);
  const name = keyParameter === null ? undefined : node.parameters[keyParameter];
  return keys.find((row) => row.key === name)?.type ?? null;
}

/** Comparison operators for a key type: numbers compare by order, bool and string only by equality. */
export function keyValueOperators(keyType: string | null): readonly string[] {
  if (keyType === null || NUMERIC_TYPES.has(keyType)) return ALL_OPERATORS;
  return EQUALITY_OPERATORS;
}

export interface KeyValueParameterRule {
  /** The enum values that fit the chosen key, when narrower than the catalog's. */
  choices?: readonly string[];
  /** Why the parameter does not apply to the current key and operator. */
  inapplicable?: string;
}

/**
 * How a comparison node's `operator` and `tolerance` follow its key's type,
 * and how a wait's `timeout_while_held` follows its `timeout_s`.
 */
export function keyValueParameterRule(
  entry: CatalogEntry,
  node: ProgramTreeNode,
  keys: readonly BlackboardKeyDeclaration[],
  parameterId: string,
): KeyValueParameterRule {
  if (keyValueParameter(entry) === null) return {};
  if (parameterId === 'timeout_while_held') {
    return node.parameters.timeout_s === undefined && node.value_sources?.timeout_s === undefined
      ? { inapplicable: 'Held time matters only with a timeout_s; without one the wait has no deadline.' }
      : {};
  }
  const operator = entry.parameters.find((parameter) => parameter.id === 'operator');
  if (operator === undefined) return {};
  const keyType = keyTypeOf(entry, node, keys);
  if (parameterId === 'operator') return { choices: keyValueOperators(keyType) };
  if (parameterId !== 'tolerance') return {};
  const current: JsonValue | undefined = node.parameters.operator ?? operator.default as JsonValue;
  if (keyType !== null && !NUMERIC_TYPES.has(keyType)) {
    return { inapplicable: `A tolerance applies only to number keys; ${keyType} keys compare exactly.` };
  }
  if (current !== '==' && current !== '!=') {
    return { inapplicable: 'A tolerance applies only to == and != comparisons.' };
  }
  return {};
}
