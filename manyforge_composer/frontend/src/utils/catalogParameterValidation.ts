import type { CatalogParameter } from '../types/api';
import { firstCatalogSchemaValidationMessage } from './catalogSchema';

/**
 * Validate one v2 catalog parameter without assuming a Program-Binding shape.
 * Binding-reference semantics remain unavailable until I6 qualifies that
 * contract; this helper intentionally validates only presence and JSON Schema.
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
