import type { ReactNode } from 'react';
import type {
  BlackboardKey,
  BlackboardNamespace,
} from '../../types/api';

export interface BlackboardOutputOption {
  prefix: string;
  contract: string;
  declarationId: string;
  description?: string;
}

export interface BlackboardKeyOption {
  id: string;
  key: string;
  type: string;
  description: string;
}

export interface BlackboardFieldOption {
  namespaceId: string;
  namespacePrefix: string;
  namespaceContract: string;
  fieldName: string;
  fieldType: string;
  fieldDescription: string;
}

export function getNamespaceRefId(value: unknown): string | null {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    const namespaceRef = (value as Record<string, unknown>).namespace_ref;
    if (typeof namespaceRef === 'string' && namespaceRef.trim()) {
      return namespaceRef.trim();
    }
  }
  return null;
}

export function getKeyRefId(value: unknown): string | null {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    const keyRef = (value as Record<string, unknown>).key_ref;
    if (typeof keyRef === 'string' && keyRef.trim()) {
      return keyRef.trim();
    }
  }
  return null;
}

export function getFieldRefTarget(
  value: unknown,
): { namespaceId: string; fieldName: string } | null {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    const fieldRef = (value as Record<string, unknown>).field_ref;
    if (fieldRef && typeof fieldRef === 'object' && !Array.isArray(fieldRef)) {
      const namespaceId = (fieldRef as Record<string, unknown>).namespace;
      const fieldName = (fieldRef as Record<string, unknown>).field;
      if (
        typeof namespaceId === 'string'
        && namespaceId.trim()
        && typeof fieldName === 'string'
        && fieldName.trim()
      ) {
        return {
          namespaceId: namespaceId.trim(),
          fieldName: fieldName.trim(),
        };
      }
    }
  }
  return null;
}

export function collectDeclaredBlackboardOutputOptions(
  namespaces: BlackboardNamespace[],
): BlackboardOutputOption[] {
  return namespaces
    .map((namespace) => ({
      prefix: namespace.prefix,
      contract: namespace.contract,
      declarationId: namespace.id,
      description: namespace.description,
    }))
    .sort((left, right) => left.declarationId.localeCompare(right.declarationId));
}

export function collectBlackboardKeyOptions(
  keys: BlackboardKey[],
): BlackboardKeyOption[] {
  return [...keys]
    .sort((left, right) => left.id.localeCompare(right.id))
    .map((entry) => ({
      id: entry.id,
      key: entry.key,
      type: entry.type,
      description: entry.description ?? '',
    }));
}

export function collectBlackboardFieldOptions(
  namespaces: BlackboardNamespace[],
): BlackboardFieldOption[] {
  const options: BlackboardFieldOption[] = [];
  namespaces.forEach((namespace) => {
    namespace.fields.forEach((field) => {
      options.push({
        namespaceId: namespace.id,
        namespacePrefix: namespace.prefix,
        namespaceContract: namespace.contract,
        fieldName: field.name,
        fieldType: field.type,
        fieldDescription: field.description ?? '',
      });
    });
  });
  return options.sort((left, right) => {
    const namespaceCompare = left.namespaceId.localeCompare(right.namespaceId);
    if (namespaceCompare !== 0) {
      return namespaceCompare;
    }
    return left.fieldName.localeCompare(right.fieldName);
  });
}

function ArtifactBindingSelect({
  currentValue,
  isRunMode,
  onCommit,
  options,
  emptyMessage,
  helperText,
}: {
  currentValue: string;
  isRunMode: boolean;
  onCommit: (value: string) => void;
  options: Array<{ value: string; label: string; detail: string }>;
  emptyMessage: string;
  helperText: string;
}) {
  const selectedOption = options.find((option) => option.value === currentValue) ?? null;

  return (
    <div className="space-y-2">
      <select
        value={currentValue}
        disabled={isRunMode}
        onChange={(event) => {
          const nextValue = event.target.value;
          if (!nextValue) {
            return;
          }
          onCommit(nextValue);
        }}
        className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      >
        {options.map((option) => (
          <option key={option.value} value={option.value}>
            {option.label}
          </option>
        ))}
      </select>
      {selectedOption ? (
        <div className="text-[11px] text-gray-500">{selectedOption.detail}</div>
      ) : options.length === 0 ? (
        <div className="text-[11px] text-yellow-300">{emptyMessage}</div>
      ) : (
        <div className="text-[11px] text-gray-500">{helperText}</div>
      )}
    </div>
  );
}

export function NamespaceBindingEditor({
  currentValue,
  isRunMode,
  onCommit,
  options,
  emptyMessage,
  helperText,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
  options: BlackboardOutputOption[];
  emptyMessage: string;
  helperText: string;
}) {
  const currentNamespaceRefId = getNamespaceRefId(currentValue);
  return (
    <ArtifactBindingSelect
      currentValue={currentNamespaceRefId ?? ''}
      isRunMode={isRunMode}
      onCommit={(nextValue) => onCommit({ namespace_ref: nextValue })}
      options={options.map((option) => ({
        value: option.declarationId,
        label: `${option.prefix} · ${option.declarationId}`,
        detail: `Declared namespace ${option.declarationId}.`,
      }))}
      emptyMessage={emptyMessage}
      helperText={helperText}
    />
  );
}

export function BlackboardKeySelectorEditor({
  currentValue,
  isRunMode,
  onCommit,
  keyOptions,
  fieldOptions,
  expectedType,
  allowFieldRefs,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
  keyOptions: BlackboardKeyOption[];
  fieldOptions: BlackboardFieldOption[];
  expectedType?: string | null;
  allowFieldRefs: boolean;
}) {
  const compatibleKeys = expectedType
    ? keyOptions.filter((option) => option.type === expectedType)
    : keyOptions;
  const compatibleFields = allowFieldRefs
    ? fieldOptions.filter((option) => !expectedType || option.fieldType === expectedType)
    : [];
  const currentKeyRefId = getKeyRefId(currentValue);
  const currentFieldRef = getFieldRefTarget(currentValue);
  const selectValue = currentKeyRefId
    ? `key:${currentKeyRefId}`
    : currentFieldRef
      ? `field:${currentFieldRef.namespaceId}:${currentFieldRef.fieldName}`
      : '';
  return (
    <ArtifactBindingSelect
      currentValue={selectValue}
      isRunMode={isRunMode}
      onCommit={(nextValue) => {
        if (nextValue.startsWith('key:')) {
          onCommit({ key_ref: nextValue.slice('key:'.length) });
          return;
        }
        const [, namespaceId, fieldName] = nextValue.split(':');
        onCommit({ field_ref: { namespace: namespaceId, field: fieldName } });
      }}
      options={[
        ...compatibleKeys.map((option) => ({
          value: `key:${option.id}`,
          label: `${option.key} · ${option.type}`,
          detail: `Declared key ${option.id}${option.description ? ` · ${option.description}` : ''}.`,
        })),
        ...compatibleFields.map((option) => ({
          value: `field:${option.namespaceId}:${option.fieldName}`,
          label: `${option.namespacePrefix}/value/${option.fieldName} · ${option.fieldType}`,
          detail: `Declared field ${option.namespaceId}.${option.fieldName}${option.fieldDescription ? ` · ${option.fieldDescription}` : ''}.`,
        })),
      ]}
      emptyMessage={`No declared blackboard bindings${expectedType ? ` of type ${expectedType}` : ''} yet. Add them in the PARAMS tab.`}
      helperText={`Choose a declared blackboard binding${expectedType ? ` of type ${expectedType}` : ''}.`}
    />
  );
}

export function BlackboardFieldReferenceEditor({
  currentValue,
  isRunMode,
  onCommit,
  fieldOptions,
  expectedType,
  expectedContract,
  emptyMessage,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
  fieldOptions: BlackboardFieldOption[];
  expectedType?: string | null;
  expectedContract?: string | null;
  emptyMessage: string;
}) {
  const compatibleFields = fieldOptions.filter((option) => {
    if (expectedType && option.fieldType !== expectedType) {
      return false;
    }
    if (expectedContract && option.namespaceContract !== expectedContract) {
      return false;
    }
    return true;
  });
  const currentFieldRef = getFieldRefTarget(currentValue);
  const selectValue = currentFieldRef
    ? `${currentFieldRef.namespaceId}:${currentFieldRef.fieldName}`
    : '';
  return (
    <ArtifactBindingSelect
      currentValue={selectValue}
      isRunMode={isRunMode}
      onCommit={(nextValue) => {
        const separatorIndex = nextValue.indexOf(':');
        const namespaceId = nextValue.slice(0, separatorIndex);
        const fieldName = nextValue.slice(separatorIndex + 1);
        onCommit({ field_ref: { namespace: namespaceId, field: fieldName } });
      }}
      options={compatibleFields.map((option) => ({
        value: `${option.namespaceId}:${option.fieldName}`,
        label: `${option.namespacePrefix}/value/${option.fieldName} · ${option.fieldType}`,
        detail: `Declared field ${option.namespaceId}.${option.fieldName}${option.fieldDescription ? ` · ${option.fieldDescription}` : ''}.`,
      }))}
      emptyMessage={emptyMessage}
      helperText="Choose a declared field reference."
    />
  );
}

export function LiteralOrFieldReferenceEditor({
  currentValue,
  isRunMode,
  onCommitLiteral,
  onCommitFieldRef,
  fieldOptions,
  expectedType,
  expectedContract,
  emptyMessage,
  literalLabel,
  fieldLabel,
  createDefaultLiteral,
  renderLiteralEditor,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommitLiteral: (value: unknown) => void;
  onCommitFieldRef: (value: unknown) => void;
  fieldOptions: BlackboardFieldOption[];
  expectedType?: string | null;
  expectedContract?: string | null;
  emptyMessage: string;
  literalLabel: string;
  fieldLabel: string;
  createDefaultLiteral: () => unknown;
  renderLiteralEditor: (value: unknown, onCommit: (value: unknown) => void) => ReactNode;
}) {
  const currentFieldRef = getFieldRefTarget(currentValue);
  const mode = currentFieldRef ? 'field' : 'literal';
  const literalValue = currentFieldRef ? createDefaultLiteral() : currentValue;

  return (
    <div className="space-y-2">
      <select
        value={mode}
        disabled={isRunMode}
        onChange={(event) => {
          const nextMode = event.target.value;
          if (nextMode === 'literal') {
            onCommitLiteral(createDefaultLiteral());
            return;
          }
          const firstField = fieldOptions.find((option) => {
            if (expectedType && option.fieldType !== expectedType) {
              return false;
            }
            if (expectedContract && option.namespaceContract !== expectedContract) {
              return false;
            }
            return true;
          });
          if (firstField) {
            onCommitFieldRef({
              field_ref: { namespace: firstField.namespaceId, field: firstField.fieldName },
            });
          }
        }}
        className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      >
        <option value="literal">{literalLabel}</option>
        <option value="field">{fieldLabel}</option>
      </select>
      {mode === 'literal' ? (
        renderLiteralEditor(literalValue, onCommitLiteral)
      ) : (
        <BlackboardFieldReferenceEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={onCommitFieldRef}
          fieldOptions={fieldOptions}
          expectedType={expectedType}
          expectedContract={expectedContract}
          emptyMessage={emptyMessage}
        />
      )}
    </div>
  );
}
