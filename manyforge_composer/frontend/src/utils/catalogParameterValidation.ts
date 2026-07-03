import type {
  BlackboardKey,
  BlackboardNamespace,
  CatalogParameter,
} from '../types/api';
import { firstCatalogSchemaValidationMessage } from './catalogSchema';

export interface BlackboardValidationContext {
  namespaces: BlackboardNamespace[];
  keys: BlackboardKey[];
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function namespaceRefId(value: unknown): string | null {
  if (!isRecord(value)) {
    return null;
  }
  const ref = value.namespace_ref;
  return typeof ref === 'string' && ref.trim().length > 0 ? ref.trim() : null;
}

function keyRefId(value: unknown): string | null {
  if (!isRecord(value)) {
    return null;
  }
  const ref = value.key_ref;
  return typeof ref === 'string' && ref.trim().length > 0 ? ref.trim() : null;
}

function fieldRefTarget(value: unknown): { namespaceId: string; fieldName: string } | null {
  if (!isRecord(value) || !isRecord(value.field_ref)) {
    return null;
  }
  const namespaceId = value.field_ref.namespace;
  const fieldName = value.field_ref.field;
  if (
    typeof namespaceId !== 'string'
    || namespaceId.trim().length === 0
    || typeof fieldName !== 'string'
    || fieldName.trim().length === 0
  ) {
    return null;
  }
  return { namespaceId: namespaceId.trim(), fieldName: fieldName.trim() };
}

function hasBindingProperty(value: unknown, propertyName: 'field_ref' | 'key_ref' | 'namespace_ref'): boolean {
  return isRecord(value) && propertyName in value;
}

function validateNamespaceRef(
  param: CatalogParameter,
  value: unknown,
  context: BlackboardValidationContext,
): string | null {
  const namespaceId = namespaceRefId(value);
  if (namespaceId === null) {
    return `${param.name} must be a namespace_ref binding`;
  }
  const namespace = context.namespaces.find((candidate) => candidate.id === namespaceId);
  if (!namespace) {
    return `${param.name} namespace_ref '${namespaceId}' is not declared in program.blackboard.namespaces`;
  }
  if (param.blackboardContract && namespace.contract !== param.blackboardContract) {
    return `${param.name} namespace_ref '${namespaceId}' is declared as contract '${namespace.contract}', expected '${param.blackboardContract}'`;
  }
  return null;
}

function validateKeyRef(
  param: CatalogParameter,
  value: unknown,
  context: BlackboardValidationContext,
): string | null {
  const keyId = keyRefId(value);
  if (keyId === null) {
    return `${param.name} must be a key_ref binding`;
  }
  const key = context.keys.find((candidate) => candidate.id === keyId);
  if (!key) {
    return `${param.name} key_ref '${keyId}' is not declared in program.blackboard.keys`;
  }
  if (param.blackboardValueType && key.type !== param.blackboardValueType) {
    return `${param.name} key_ref '${keyId}' is declared as type '${key.type}', expected '${param.blackboardValueType}'`;
  }
  return null;
}

function validateFieldRef(
  param: CatalogParameter,
  value: unknown,
  context: BlackboardValidationContext,
): string | null {
  const target = fieldRefTarget(value);
  if (target === null) {
    return `${param.name} must be a field_ref binding`;
  }
  const namespace = context.namespaces.find((candidate) => candidate.id === target.namespaceId);
  if (!namespace) {
    return `${param.name} field_ref namespace '${target.namespaceId}' is not declared in program.blackboard.namespaces`;
  }
  if (param.blackboardContract && namespace.contract !== param.blackboardContract) {
    return `${param.name} field_ref namespace '${target.namespaceId}' declares contract '${namespace.contract}', expected '${param.blackboardContract}'`;
  }
  const field = namespace.fields.find((candidate) => candidate.name === target.fieldName);
  if (!field) {
    return `${param.name} field_ref '${target.namespaceId}.${target.fieldName}' is not declared in program.blackboard.namespaces[].fields`;
  }
  const expectedType = param.blackboardValueType || (param.blackboardRole === 'field_reader' ? param.type : null);
  if (expectedType && field.type !== expectedType) {
    return `${param.name} field_ref '${target.namespaceId}.${target.fieldName}' is declared as type '${field.type}', expected '${expectedType}'`;
  }
  return null;
}

export function firstCatalogParameterValidationMessage(
  param: CatalogParameter,
  value: unknown,
  context: BlackboardValidationContext,
): string | null {
  if (param.blackboardRole === 'namespace_writer' || param.blackboardRole === 'namespace_reader') {
    return validateNamespaceRef(param, value, context);
  }

  if (param.blackboardRole === 'key_writer') {
    return validateKeyRef(param, value, context);
  }

  if (param.blackboardRole === 'key_reader') {
    if (keyRefId(value) !== null) {
      return validateKeyRef(param, value, context);
    }
    return validateFieldRef(param, value, context);
  }

  if (param.blackboardRole === 'literal_or_key_reader') {
    if (keyRefId(value) !== null) {
      return validateKeyRef(param, value, context);
    }
    if (fieldRefTarget(value) !== null || hasBindingProperty(value, 'field_ref')) {
      return validateFieldRef(param, value, context);
    }
  }

  if (
    param.blackboardRole === 'field_reader'
    && (fieldRefTarget(value) !== null || hasBindingProperty(value, 'field_ref'))
  ) {
    return validateFieldRef(param, value, context);
  }

  return firstCatalogSchemaValidationMessage(param.schema, value, param.name);
}
