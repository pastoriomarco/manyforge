import { useCallback, useEffect, useRef, useState } from 'react';

import type {
  BlackboardFieldOption,
  BlackboardKeyOption,
} from './BlackboardBindingEditors';
import {
  getFieldRefTarget,
  getKeyRefId,
} from './BlackboardBindingEditors';
import { primitiveListValuesEqual } from './utils';
import { DraftTextInput } from '../common/DraftInputs';

type ConstraintMap = Record<string, unknown>;

interface TemplateVariableRow {
  id: string;
  name: string;
  binding: unknown;
}

interface ListEditorRow {
  id: string;
  value: string;
}

function nextTemplateVariableRowId(): string {
  return `template-variable-${Math.random().toString(36).slice(2, 10)}`;
}

function nextListRowId(): string {
  return `list-row-${Math.random().toString(36).slice(2, 10)}`;
}

function normalizeTemplateVariableRows(value: unknown): TemplateVariableRow[] {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    return Object.entries(value as Record<string, unknown>).map(([name, binding]) => ({
      id: nextTemplateVariableRowId(),
      name,
      binding,
    }));
  }
  return [];
}

function formatStructuredValue(value: unknown): string {
  if (value === undefined) {
    return '';
  }
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

function normalizeListRows(paramType: 'string_list' | 'float_list', value: unknown): ListEditorRow[] {
  if (!Array.isArray(value)) {
    return [];
  }
  if (paramType === 'string_list') {
    return value.map((item) => ({
      id: nextListRowId(),
      value: typeof item === 'string' ? item : String(item ?? ''),
    }));
  }
  return value.map((item) => ({
    id: nextListRowId(),
    value: typeof item === 'number' && Number.isFinite(item) ? String(item) : String(item ?? ''),
  }));
}

function normalizePrimitiveListValue(
  paramType: 'string_list' | 'float_list',
  value: unknown,
): string[] | number[] {
  if (!Array.isArray(value)) {
    return [];
  }
  if (paramType === 'string_list') {
    return value.map((item) => (typeof item === 'string' ? item : String(item ?? '')));
  }
  return value.map((item) => (typeof item === 'number' && Number.isFinite(item) ? item : Number(item)));
}

function normalizeFixedNumericDrafts(
  currentValue: unknown,
  length: number,
): string[] {
  if (!Array.isArray(currentValue)) {
    return Array.from({ length }, () => '');
  }
  return Array.from({ length }, (_, index) => {
    const rawValue = currentValue[index];
    return typeof rawValue === 'number' && Number.isFinite(rawValue)
      ? String(rawValue)
      : rawValue !== undefined && rawValue !== null
        ? String(rawValue)
        : '';
  });
}

function parseStructuredValue(paramType: string, text: string): unknown {
  const parsed = JSON.parse(text);
  if (paramType === 'dict' || paramType === 'pose' || paramType === 'object') {
    if (parsed === null || Array.isArray(parsed) || typeof parsed !== 'object') {
      throw new Error('Value must be a JSON object');
    }
    return parsed;
  }
  if (paramType === 'float_list') {
    if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'number')) {
      throw new Error('Value must be a JSON array of numbers');
    }
    return parsed;
  }
  if (paramType === 'string_list') {
    if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'string')) {
      throw new Error('Value must be a JSON array of strings');
    }
    return parsed;
  }
  return parsed;
}

function getNumericConstraintError(value: number, constraints: ConstraintMap): string | null {
  const minInclusive = typeof constraints.min_inclusive === 'number'
    ? constraints.min_inclusive
    : undefined;
  const minExclusive = typeof constraints.min_exclusive === 'number'
    ? constraints.min_exclusive
    : undefined;
  const maxInclusive = typeof constraints.max_inclusive === 'number'
    ? constraints.max_inclusive
    : undefined;
  const maxExclusive = typeof constraints.max_exclusive === 'number'
    ? constraints.max_exclusive
    : undefined;

  if (minInclusive !== undefined && value < minInclusive) {
    return `Value must be >= ${minInclusive}`;
  }
  if (minExclusive !== undefined && value <= minExclusive) {
    return `Value must be > ${minExclusive}`;
  }
  if (maxInclusive !== undefined && value > maxInclusive) {
    return `Value must be <= ${maxInclusive}`;
  }
  if (maxExclusive !== undefined && value >= maxExclusive) {
    return `Value must be < ${maxExclusive}`;
  }
  return null;
}

export function NumericParamEditor({
  currentValue,
  isRunMode,
  onCommit,
  paramType,
  constraints,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: number) => void;
  paramType: 'float' | 'int';
  constraints: ConstraintMap;
}) {
  const [draft, setDraft] = useState(() =>
    currentValue !== undefined && currentValue !== null ? String(currentValue) : ''
  );
  const [error, setError] = useState<string | null>(null);
  const [isFocused, setIsFocused] = useState(false);
  // Set to true by the Escape handler before it calls .blur(). onBlur then
  // skips its tryCommit call so the in-progress draft is discarded instead
  // of committed. Matches the skipNextCommit pattern in
  // ../common/DraftInputs.tsx so behavior is consistent across all
  // editable inputs in the property panel.
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(currentValue !== undefined && currentValue !== null ? String(currentValue) : '');
    setError(null);
  }, [currentValue, isFocused]);

  const tryCommit = (rawValue: string) => {
    const trimmed = rawValue.trim();
    if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
      setError(null);
      return;
    }
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) {
      setError('Value must be a finite number');
      return;
    }
    if (paramType === 'int' && !Number.isInteger(parsed)) {
      setError('Value must be an integer');
      return;
    }
    const constraintError = getNumericConstraintError(parsed, constraints);
    if (constraintError) {
      setError(constraintError);
      return;
    }
    setError(null);
    onCommit(parsed);
  };

  return (
    <div>
      <input
        type="text"
        inputMode="decimal"
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => {
          const nextDraft = event.target.value;
          setDraft(nextDraft);
          if (error) {
            setError(null);
          }
        }}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            // Escape just fired: drop the in-progress draft without
            // committing. The next render's useEffect re-syncs `draft`
            // to `currentValue` because isFocused is now false.
            skipNextCommit.current = false;
            setDraft(currentValue !== undefined && currentValue !== null ? String(currentValue) : '');
            setError(null);
          } else {
            tryCommit(draft);
          }
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            // Mark the impending blur as a revert (not a commit). The
            // setDraft below is queued asynchronously; the
            // synchronous blur+onBlur path uses skipNextCommit to
            // decide whether to commit `draft` (which still holds the
            // typed value at this point) or discard it.
            skipNextCommit.current = true;
            setDraft(currentValue !== undefined && currentValue !== null ? String(currentValue) : '');
            setError(null);
            event.currentTarget.blur();
          }
        }}
        disabled={isRunMode}
        className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      />
      {error && <div className="mt-1 text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

export function TemplateVariableMapEditor({
  currentValue,
  isRunMode,
  onCommit,
  keyOptions,
  fieldOptions,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: Record<string, unknown>) => void;
  keyOptions: BlackboardKeyOption[];
  fieldOptions: BlackboardFieldOption[];
}) {
  const [rows, setRows] = useState<TemplateVariableRow[]>(() => normalizeTemplateVariableRows(currentValue));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setRows(normalizeTemplateVariableRows(currentValue));
    setError(null);
  }, [currentValue]);

  const validateAndCommit = useCallback((nextRows: TemplateVariableRow[]) => {
    const normalizedRows = nextRows
      .map((row) => ({
        ...row,
        name: row.name,
        binding: row.binding,
      }));
    setRows(normalizedRows);

    const seen = new Set<string>();
    for (const row of normalizedRows) {
      const trimmedName = row.name.trim();
      if (!trimmedName) {
        setError('Variable names must not be empty');
        return;
      }
      const keyRefId = getKeyRefId(row.binding);
      const fieldRef = getFieldRefTarget(row.binding);
      if (!keyRefId && !fieldRef) {
        setError('Each template variable must select a declared key or namespace field');
        return;
      }
      if (seen.has(trimmedName)) {
        setError(`Duplicate variable name: ${trimmedName}`);
        return;
      }
      seen.add(trimmedName);
    }

    setError(null);
    onCommit(
      Object.fromEntries(
        normalizedRows.map((row) => [row.name.trim(), row.binding])
      ),
    );
  }, [onCommit]);

  const bindingOptions = [
    ...keyOptions.map((option) => ({
      value: `key:${option.id}`,
      label: `${option.key} · ${option.type}`,
      detail: `Declared key ${option.id}${option.description ? ` · ${option.description}` : ''}.`,
    })),
    ...fieldOptions.map((option) => ({
      value: `field:${option.namespaceId}:${option.fieldName}`,
      label: `${option.namespacePrefix}/value/${option.fieldName} · ${option.fieldType}`,
      detail: `Declared field ${option.namespaceId}.${option.fieldName}${option.fieldDescription ? ` · ${option.fieldDescription}` : ''}.`,
    })),
  ];

  return (
    <div className="space-y-2">
      {rows.length === 0 && (
        <div className="text-xs text-gray-500">No template variables configured</div>
      )}
      {rows.map((row) => (
        <div key={row.id} className="grid grid-cols-[1fr_1fr_auto] gap-2">
          <DraftTextInput
            type="text"
            value={row.name}
            onCommit={(nextName) => {
              const nextRows = rows.map((candidate) =>
                candidate.id === row.id
                  ? { ...candidate, name: nextName }
                  : candidate
              );
              validateAndCommit(nextRows);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
            placeholder="variable name"
          />
          <select
            value={(() => {
              const keyRefId = getKeyRefId(row.binding);
              if (keyRefId) {
                return `key:${keyRefId}`;
              }
              const fieldRef = getFieldRefTarget(row.binding);
              if (fieldRef) {
                return `field:${fieldRef.namespaceId}:${fieldRef.fieldName}`;
              }
              return '';
            })()}
            onChange={(event) => {
              const nextRows = rows.map((candidate) =>
                candidate.id === row.id
                  ? {
                    ...candidate,
                    binding: event.target.value.startsWith('key:')
                      ? { key_ref: event.target.value.slice('key:'.length) }
                      : (() => {
                        const [, namespaceId, fieldName] = event.target.value.split(':');
                        return { field_ref: { namespace: namespaceId, field: fieldName } };
                      })(),
                  }
                  : candidate
              );
              validateAndCommit(nextRows);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
          >
            <option value="">Select binding</option>
            {bindingOptions.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
          <button
            type="button"
            onClick={() => validateAndCommit(rows.filter((candidate) => candidate.id !== row.id))}
            disabled={isRunMode}
            className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
            title="Remove variable"
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
          onClick={() => {
            const nextRows = [
              ...rows,
              {
                id: nextTemplateVariableRowId(),
                name: `var_${rows.length + 1}`,
                binding: null,
              },
            ];
            setRows(nextRows);
            setError('Each template variable must select a declared key or namespace field');
          }}
          disabled={isRunMode}
          className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
        >
          Add variable
      </button>
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

export function ListParamEditor({
  currentValue,
  isRunMode,
  onCommit,
  paramType,
  extraError,
  itemPlaceholder,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: string[] | number[]) => void;
  paramType: 'string_list' | 'float_list';
  extraError?: string | null;
  itemPlaceholder?: string;
}) {
  const [rows, setRows] = useState<ListEditorRow[]>(() => normalizeListRows(paramType, currentValue));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setRows(normalizeListRows(paramType, currentValue));
    setError(null);
  }, [currentValue, paramType]);

  const validateAndCommit = useCallback((nextRows: ListEditorRow[]) => {
    setRows(nextRows);
    const baselineValue = normalizePrimitiveListValue(paramType, currentValue);

    if (paramType === 'string_list') {
      const normalizedValues: string[] = [];
      for (const row of nextRows) {
        const trimmed = row.value.trim();
        if (!trimmed) {
          setError('List items must not be empty');
          return;
        }
        normalizedValues.push(trimmed);
      }
      setError(null);
      if (!primitiveListValuesEqual(normalizedValues, baselineValue)) {
        onCommit(normalizedValues);
      }
      return;
    }

    const parsedValues: number[] = [];
    for (const row of nextRows) {
      const trimmed = row.value.trim();
      if (!trimmed) {
        setError('List items must not be empty');
        return;
      }
      const parsed = Number(trimmed);
      if (!Number.isFinite(parsed)) {
        setError('All list items must be valid numbers');
        return;
      }
      parsedValues.push(parsed);
    }
    setError(null);
    if (!primitiveListValuesEqual(parsedValues, baselineValue)) {
      onCommit(parsedValues);
    }
  }, [currentValue, onCommit, paramType]);

  return (
    <div className="space-y-2">
      {rows.length === 0 && (
        <div className="text-xs text-gray-500">
          No values configured.
        </div>
      )}
      {rows.map((row, index) => (
        <div key={row.id} className="grid grid-cols-[auto_1fr_auto] items-center gap-2">
          <div className="text-[11px] text-gray-500">{index}</div>
          <DraftTextInput
            type="text"
            inputMode={paramType === 'float_list' ? 'decimal' : 'text'}
            value={row.value}
            onCommit={(nextValue) => {
              const nextRows = rows.map((candidate) =>
                candidate.id === row.id
                  ? { ...candidate, value: nextValue }
                  : candidate
              );
              validateAndCommit(nextRows);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
            placeholder={itemPlaceholder ?? (paramType === 'float_list' ? '0.0' : 'value')}
          />
          <button
            type="button"
            onClick={() => validateAndCommit(rows.filter((candidate) => candidate.id !== row.id))}
            disabled={isRunMode}
            className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
            title="Remove item"
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          const nextRows = [
            ...rows,
            {
              id: nextListRowId(),
              value: '',
            },
          ];
          setRows(nextRows);
          setError('List items must not be empty');
        }}
        disabled={isRunMode}
        className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
      >
        Add item
      </button>
      {(error || extraError) && (
        <div className="text-[11px] text-red-400">
          {error ?? extraError}
        </div>
      )}
    </div>
  );
}

export function FixedFloatListEditor({
  currentValue,
  isRunMode,
  onCommit,
  labels,
  requirePositive = false,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: number[]) => void;
  labels: string[];
  requirePositive?: boolean;
}) {
  const labelsSignature = labels.join('|');
  const [drafts, setDrafts] = useState<string[]>(() =>
    normalizeFixedNumericDrafts(currentValue, labels.length),
  );
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setDrafts(normalizeFixedNumericDrafts(currentValue, labels.length));
    setError(null);
  }, [currentValue, labels.length, labelsSignature]);

  const validateAndCommit = useCallback((nextDrafts: string[]) => {
    setDrafts(nextDrafts);
    const parsedValues: number[] = [];
    for (let index = 0; index < nextDrafts.length; index += 1) {
      const trimmed = nextDrafts[index].trim();
      if (!trimmed) {
        setError(`${labels[index]} is required`);
        return;
      }
      const parsed = Number(trimmed);
      if (!Number.isFinite(parsed)) {
        setError(`${labels[index]} must be a finite number`);
        return;
      }
      if (requirePositive && parsed <= 0) {
        setError(`${labels[index]} must be > 0`);
        return;
      }
      parsedValues.push(parsed);
    }
    setError(null);
    const baseline = Array.isArray(currentValue)
      ? currentValue.filter((value): value is number => typeof value === 'number' && Number.isFinite(value))
      : [];
    if (!primitiveListValuesEqual(parsedValues, baseline)) {
      onCommit(parsedValues);
    }
  }, [currentValue, labels, onCommit, requirePositive]);

  return (
    <div className="space-y-2">
      {labels.map((label, index) => (
        <div key={label} className="grid grid-cols-[72px_1fr] items-center gap-2">
          <div className="text-[11px] text-gray-400">{label}</div>
          <DraftTextInput
            type="text"
            inputMode="decimal"
            value={drafts[index] ?? ''}
            onCommit={(nextValue) => {
              const nextDrafts = drafts.map((value, draftIndex) =>
                draftIndex === index ? nextValue : value,
              );
              validateAndCommit(nextDrafts);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
          />
        </div>
      ))}
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

export function StructuredParamEditor({
  paramName,
  paramType,
  currentValue,
  isRunMode,
  onCommit,
}: {
  paramName: string;
  paramType: string;
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (paramName: string, value: unknown) => void;
}) {
  const [draft, setDraft] = useState(() => formatStructuredValue(currentValue));
  const [error, setError] = useState<string | null>(null);
  const [isFocused, setIsFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(formatStructuredValue(currentValue));
    setError(null);
  }, [currentValue, isFocused]);

  const disabledClasses = isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : '';

  const commit = useCallback(() => {
    if (isRunMode) {
      return;
    }
    const trimmed = draft.trim();
    if (trimmed === '') {
      setError('Value must not be empty');
      return;
    }
    try {
      const parsed = parseStructuredValue(paramType, trimmed);
      setError(null);
      onCommit(paramName, parsed);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Invalid JSON value');
    }
  }, [draft, isRunMode, onCommit, paramName, paramType]);

  return (
    <div>
      <textarea
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => {
          setDraft(event.target.value);
          if (error) {
            setError(null);
          }
        }}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            skipNextCommit.current = false;
            setDraft(formatStructuredValue(currentValue));
            setError(null);
          } else {
            commit();
          }
        }}
        onKeyDown={(event) => {
          // Ctrl/Cmd+Enter commits (mirrors DraftTextarea in
          // ../common/DraftInputs.tsx). Plain Enter is a newline,
          // which is what operators expect inside a JSON editor.
          if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
            return;
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            skipNextCommit.current = true;
            setDraft(formatStructuredValue(currentValue));
            setError(null);
            event.currentTarget.blur();
          }
        }}
        disabled={isRunMode}
        rows={8}
        spellCheck={false}
        className={`w-full resize-y rounded border border-gray-600 bg-gray-700 px-2 py-1 font-mono text-xs text-white ${disabledClasses}`}
      />
      <div className="mt-1 flex items-center justify-between">
        <span className="text-[11px] text-gray-500">JSON · Ctrl/Cmd+Enter commits · Esc reverts</span>
        {error && <span className="text-[11px] text-red-400">{error}</span>}
      </div>
    </div>
  );
}
