import type { CatalogJsonSchema } from '../types/api';

export interface CatalogSchemaValidationIssue {
  path: string;
  message: string;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function valueMatchesType(value: unknown, type: string): boolean {
  if (type === 'string') return typeof value === 'string';
  if (type === 'number') return typeof value === 'number' && Number.isFinite(value);
  if (type === 'integer') return typeof value === 'number' && Number.isInteger(value);
  if (type === 'boolean') return typeof value === 'boolean';
  if (type === 'array') return Array.isArray(value);
  if (type === 'object') return isPlainObject(value);
  if (type === 'null') return value === null;
  return true;
}

function childPath(path: string, key: string): string {
  return path ? `${path}.${key}` : key;
}

function itemPath(path: string, index: number): string {
  return `${path}[${index}]`;
}

function valuesEqual(left: unknown, right: unknown): boolean {
  if (left === right) return true;
  try {
    return JSON.stringify(left) === JSON.stringify(right);
  } catch {
    return false;
  }
}

function validateSubschemaPasses(
  schema: CatalogJsonSchema,
  value: unknown,
): boolean {
  return validateCatalogJsonSchemaValue(schema, value, '').length === 0;
}

export function validateCatalogJsonSchemaValue(
  schema: CatalogJsonSchema | null | undefined,
  value: unknown,
  path = 'value',
): CatalogSchemaValidationIssue[] {
  if (!schema) return [];

  const issues: CatalogSchemaValidationIssue[] = [];

  if (schema.const !== undefined && !valuesEqual(value, schema.const)) {
    issues.push({ path, message: `${path} must be ${JSON.stringify(schema.const)}` });
  }
  if (schema.enum && !schema.enum.some((option) => valuesEqual(value, option))) {
    issues.push({
      path,
      message: `${path} must be one of: ${schema.enum
        .map((option) => JSON.stringify(option))
        .join(', ')}`,
    });
  }

  if (schema.type && !valueMatchesType(value, schema.type)) {
    issues.push({ path, message: `${path} must be a ${schema.type}` });
    return issues;
  }

  if (typeof value === 'number') {
    if (schema.minimum !== undefined && value < schema.minimum) {
      issues.push({ path, message: `${path} must be >= ${schema.minimum}` });
    }
    if (schema.maximum !== undefined && value > schema.maximum) {
      issues.push({ path, message: `${path} must be <= ${schema.maximum}` });
    }
    if (schema.exclusiveMinimum !== undefined && value <= schema.exclusiveMinimum) {
      issues.push({ path, message: `${path} must be > ${schema.exclusiveMinimum}` });
    }
    if (schema.exclusiveMaximum !== undefined && value >= schema.exclusiveMaximum) {
      issues.push({ path, message: `${path} must be < ${schema.exclusiveMaximum}` });
    }
  }

  if (Array.isArray(value)) {
    if (schema.minItems !== undefined && value.length < schema.minItems) {
      issues.push({
        path,
        message: `${path} must contain at least ${schema.minItems} item(s)`,
      });
    }
    if (schema.maxItems !== undefined && value.length > schema.maxItems) {
      issues.push({
        path,
        message: `${path} must contain at most ${schema.maxItems} item(s)`,
      });
    }
    if (schema.items) {
      value.forEach((item, index) => {
        issues.push(
          ...validateCatalogJsonSchemaValue(
            schema.items,
            item,
            itemPath(path, index),
          ),
        );
      });
    }
  }

  if (isPlainObject(value)) {
    const properties = schema.properties ?? {};
    for (const requiredKey of schema.required ?? []) {
      if (!(requiredKey in value)) {
        const requiredPath = childPath(path, requiredKey);
        issues.push({ path: requiredPath, message: `${requiredPath} is required` });
      }
    }

    for (const [key, nestedValue] of Object.entries(value)) {
      const nestedSchema = properties[key];
      if (nestedSchema) {
        issues.push(
          ...validateCatalogJsonSchemaValue(
            nestedSchema,
            nestedValue,
            childPath(path, key),
          ),
        );
      } else if (schema.additionalProperties === false) {
        const unexpectedPath = childPath(path, key);
        issues.push({
          path: unexpectedPath,
          message: `${unexpectedPath} is not allowed`,
        });
      } else if (isPlainObject(schema.additionalProperties)) {
        issues.push(
          ...validateCatalogJsonSchemaValue(
            schema.additionalProperties,
            nestedValue,
            childPath(path, key),
          ),
        );
      }
    }
  }

  for (const subSchema of schema.allOf ?? []) {
    issues.push(...validateCatalogJsonSchemaValue(subSchema, value, path));
  }

  if (schema.anyOf && schema.anyOf.length > 0) {
    const matches = schema.anyOf.some((subSchema) =>
      validateSubschemaPasses(subSchema, value),
    );
    if (!matches) {
      issues.push({ path, message: `${path} must match at least one allowed schema` });
    }
  }

  if (schema.oneOf && schema.oneOf.length > 0) {
    const matchCount = schema.oneOf.filter((subSchema) =>
      validateSubschemaPasses(subSchema, value),
    ).length;
    if (matchCount !== 1) {
      issues.push({ path, message: `${path} must match exactly one allowed schema` });
    }
  }

  if (schema.not && validateSubschemaPasses(schema.not, value)) {
    const disallowedField =
      schema.not.required && schema.not.required.length === 1
        ? schema.not.required[0]
        : null;
    issues.push({
      path: disallowedField ? childPath(path, disallowedField) : path,
      message: disallowedField
        ? `${childPath(path, disallowedField)} is not allowed in this context`
        : `${path} must not match a disallowed schema`,
    });
  }

  if (schema.if && schema.then && validateSubschemaPasses(schema.if, value)) {
    issues.push(...validateCatalogJsonSchemaValue(schema.then, value, path));
  }

  return issues;
}

export function firstCatalogSchemaValidationMessage(
  schema: CatalogJsonSchema | null | undefined,
  value: unknown,
  path = 'value',
): string | null {
  return validateCatalogJsonSchemaValue(schema, value, path)[0]?.message ?? null;
}
