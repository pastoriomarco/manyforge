import type { CatalogParameter } from '../types/api';
import { firstCatalogSchemaValidationMessage } from './catalogSchema';

/**
 * Validate one literal v2 catalog parameter value. Program value-source
 * compatibility is checked separately against the exact Program declaration
 * and destination schema before the literal is replaced.
 */
export function firstCatalogParameterValidationMessage(
  parameter: CatalogParameter,
  value: unknown,
): string | null {
  const omitted = value === undefined || value === null;
  if (omitted) {
    return parameter.required && !parameter.hasDefault
      ? `${parameter.id} is required`
      : null;
  }
  return firstCatalogSchemaValidationMessage(
    parameter.schema,
    value,
    parameter.id,
  );
}
