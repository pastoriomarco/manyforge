import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  ExpressionValueType,
  JsonValue,
  ProgramParameterDeclaration,
  ProgramValueSource,
} from '../types/api';
import { catalogParameterSourceable, schemaAcceptsProgramType } from './programAuthoring';
import { parseValuePath, valueAtPath } from './valuePaths';

/**
 * What the value-source menu offers for one value of a node
 * (docs/authoring/EXPRESSIONS.md §10): Type values, Use key…, Use
 * parameter… with type-matched choices, and formulas for a single number,
 * bool or string.
 */
export interface ValueSourceChoice {
  label: string;
  source: ProgramValueSource;
}

export interface ValueSourceMenuModel {
  path: string;
  current: ProgramValueSource | undefined;
  keyChoices: ValueSourceChoice[];
  parameterChoices: ValueSourceChoice[];
  /** The type a formula must produce here; null when this value takes no formula. */
  formulaType: ExpressionValueType | null;
  /** Why a source cannot be chosen yet, or null. */
  unavailable: string | null;
}

export interface ValueSourceMenuInput {
  entry: CatalogEntry;
  path: string;
  schema: CatalogJsonSchema;
  sources: Readonly<Record<string, ProgramValueSource>> | undefined;
  /** The node's literals, with catalog defaults for omitted parameters. */
  literals: Readonly<Record<string, JsonValue>>;
  parameters: readonly ProgramParameterDeclaration[];
  keys: readonly BlackboardKeyDeclaration[];
  /** A Program motion default: the only parameter a motion field offers. */
  preferredParameter?: string | null;
}

/** The formula result a leaf needs, or null for a value that is not a single bool, number or string. */
export function formulaDestinationType(schema: CatalogJsonSchema): ExpressionValueType | null {
  if (schema['x-manyforge-semantic-type'] !== undefined || schema.enum !== undefined) return null;
  if (schema.type === 'number') return 'float64';
  if (schema.type === 'integer') return 'int64';
  if (schema.type === 'boolean') return 'bool';
  if (schema.type === 'string') return 'string';
  return null;
}

/**
 * Values inside a parameter that are never sourced, as the Program
 * validators refuse them: text, choice and identifier fields stay literal,
 * and quaternion components are typed or bound only with their whole pose.
 */
function nestedValueSourceable(segments: readonly (string | number)[], schema: CatalogJsonSchema): boolean {
  return schema['x-manyforge-semantic-type'] === undefined
    && schema.enum === undefined
    && schema.type !== 'string'
    && !segments.includes('xyzw');
}

/** The menu for one value, or null when the value never comes from a source. */
export function valueSourceMenuModel(input: ValueSourceMenuInput): ValueSourceMenuModel | null {
  const segments = parseValuePath(input.path);
  if (segments === null || !catalogParameterSourceable(input.entry, String(segments[0]))) return null;
  const nested = segments.length > 1;
  if (nested && !nestedValueSourceable(segments, input.schema)) return null;
  const accepts = (type: string) => schemaAcceptsProgramType(input.schema, type);
  const keyChoices: ValueSourceChoice[] = [];
  for (const key of input.keys) {
    if (key.access === 'write') continue;
    if (accepts(key.type)) {
      keyChoices.push({ label: `key.${key.key}`, source: { kind: 'blackboard_key', reference: key.key } });
    }
    for (const field of key.fields ?? []) {
      if (accepts(field.type)) {
        keyChoices.push({
          label: `key.${key.key}.${field.field}`,
          source: { kind: 'blackboard_field', reference: key.key, field: field.field },
        });
      }
    }
  }
  const preferred = !nested && input.preferredParameter
    ? input.parameters.find((row) => row.parameter_id === input.preferredParameter)
    : undefined;
  const parameterChoices = (preferred !== undefined && accepts(preferred.type)
    ? [preferred]
    : input.parameters.filter((row) => accepts(row.type))).map((row) => ({
    label: `param.${row.parameter_id}`,
    source: { kind: 'program_parameter' as const, reference: row.parameter_id },
  }));
  const formulaType = formulaDestinationType(input.schema);
  const current = input.sources?.[input.path];
  if (
    current === undefined
    && formulaType === null
    && keyChoices.length === 0
    && parameterChoices.length === 0
  ) return null;
  return {
    path: input.path,
    current,
    keyChoices,
    parameterChoices,
    formulaType,
    unavailable: nested && valueAtPath(input.literals, input.path) === undefined
      ? 'Type a value here first; a source replaces a stored value, which returns when the source is removed.'
      : null,
  };
}

/** The same source, compared by kind and reference. */
export function sameValueSource(left: ProgramValueSource | undefined, right: ProgramValueSource): boolean {
  return left !== undefined && JSON.stringify(left) === JSON.stringify(right);
}
