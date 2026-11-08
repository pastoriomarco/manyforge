import { useEffect, useMemo, useRef, useState } from 'react';

import type { JsonValue } from '../types/api';
import { groupByNamespace, type KeyNamespaceGroup } from '../utils/blackboardNamespaces';
import {
  conditionKind,
  conditionProblem,
  defaultLiteral,
  newCompare,
  operandId,
  operatorsFor,
  unguardedNodeOutcomeKeys,
  type ConditionOperandChoice,
  type ConditionOperator,
} from '../utils/interruptAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import { NamespaceGroupOptions, useNamespaceGroups } from './common/NamespaceGroupOptions';

type Kind = 'compare' | 'all' | 'any' | 'not';

const KIND_LABELS: Record<Kind, string> = {
  compare: 'Comparison',
  all: 'All of (AND)',
  any: 'Any of (OR)',
  not: 'Not',
};

function objectOf(value: JsonValue | undefined): Record<string, JsonValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

/** Convert one node to another operator, keeping what can be kept. */
function convert(value: JsonValue, kind: Kind, first: ConditionOperandChoice | undefined): JsonValue {
  const current = conditionKind(value);
  if (current === kind) return value;
  const body = objectOf(value)[current ?? 'compare'];
  if (kind === 'all' || kind === 'any') {
    if (current === 'all' || current === 'any') return { [kind]: body };
    return { [kind]: [current === null ? newCompare(first) : value] };
  }
  if (kind === 'not') return { not: current === null ? newCompare(first) : value };
  // To a comparison: the first comparison inside, else a new one.
  const children = current === 'not' ? [body] : Array.isArray(body) ? body : [];
  return children.find((child) => conditionKind(child) === 'compare') ?? newCompare(first);
}

interface NodeProps {
  value: JsonValue;
  choices: readonly ConditionOperandChoice[];
  groups: KeyNamespaceGroup<ConditionOperandChoice>[];
  disabled: boolean;
  path: string;
  onChange: (next: JsonValue) => void;
  onRemove?: () => void;
}

function LiteralField({ choice, value, disabled, label, onChange }: {
  choice: ConditionOperandChoice | undefined;
  value: JsonValue;
  disabled: boolean;
  label: string;
  onChange: (next: JsonValue) => void;
}) {
  if (choice?.valueType === 'bool') {
    return (
      <select aria-label={label} value={String(value)} disabled={disabled}
        onChange={(event) => onChange(event.target.value === 'true')}>
        <option value="true">true</option>
        <option value="false">false</option>
      </select>
    );
  }
  if (choice?.valueType === 'string' && choice.values !== undefined) {
    const known = typeof value === 'string' && choice.values.includes(value);
    return (
      <select aria-label={label} value={typeof value === 'string' ? value : ''} disabled={disabled}
        onChange={(event) => onChange(event.target.value)}>
        {!known && <option value={typeof value === 'string' ? value : ''}>{JSON.stringify(value)}</option>}
        {choice.values.map((item) => <option key={item} value={item}>{item}</option>)}
      </select>
    );
  }
  const numeric = choice?.valueType === 'int64' || choice?.valueType === 'float64';
  return (
    <DraftTextInput
      aria-label={label}
      value={typeof value === 'string' || typeof value === 'number' ? value : JSON.stringify(value)}
      disabled={disabled}
      inputMode={numeric ? 'decimal' : undefined}
      validate={(text) => {
        if (!numeric) return null;
        const number = Number(text.trim());
        if (text.trim() === '' || !Number.isFinite(number)) return 'Enter a finite number.';
        if (choice?.valueType === 'int64' && (!Number.isInteger(number) || !Number.isSafeInteger(number))) {
          return 'Enter an integer within ±(2^53−1).';
        }
        return null;
      }}
      onCommit={(text) => onChange(numeric ? Number(text.trim()) : text)}
    />
  );
}

function ConditionNode({ value, choices, groups, disabled, path, onChange, onRemove }: NodeProps) {
  const { expanded, handleToggle } = useNamespaceGroups();
  const kind = conditionKind(value);
  const record = objectOf(value);
  const kindSelect = (
    <select aria-label={`${path} operator`} value={kind ?? ''} disabled={disabled}
      onChange={(event) => onChange(convert(value, event.target.value as Kind, choices.find((choice) => !choice.disabled)))}>
      {kind === null && <option value="">(invalid)</option>}
      {(Object.keys(KIND_LABELS) as Kind[]).map((item) => (
        <option key={item} value={item}>{KIND_LABELS[item]}</option>
      ))}
    </select>
  );
  const remove = onRemove === undefined ? null : (
    <button type="button" className="text-button" disabled={disabled}
      aria-label={`Remove ${path}`} onClick={onRemove}>Remove</button>
  );
  if (kind === 'compare') {
    const compare = objectOf(record.compare);
    const id = operandId(compare.operand ?? null);
    const choice = choices.find((item) => item.id === id);
    const operators = choice === undefined ? [] : operatorsFor(choice.valueType);
    const update = (patch: Record<string, JsonValue>) => onChange({ compare: { ...compare, ...patch } });
    return (
      <div className="interrupt-condition-compare" role="group" aria-label={path}>
        {kindSelect}
        <select aria-label={`${path} operand`} value={choice === undefined ? '' : id} disabled={disabled}
          onChange={(event) => {
            if (handleToggle(event.target.value)) return;
            const next = choices.find((item) => item.id === event.target.value);
            if (next === undefined || next.disabled) return;
            const op = operatorsFor(next.valueType).includes(compare.op as ConditionOperator) ? compare.op : '==';
            const keepValue = choice?.valueType === next.valueType
              && (next.values === undefined || next.values.includes(String(compare.value)));
            update({ operand: next.operand, op, value: keepValue ? compare.value : defaultLiteral(next) });
          }}>
          {choice === undefined && <option value="">{compare.operand === undefined ? 'Choose an input' : `Unknown: ${id}`}</option>}
          <NamespaceGroupOptions groups={groups} expanded={expanded} current={id}
            valueOf={(item) => item.id}
            option={(item) => <option key={item.id} value={item.id} disabled={item.disabled}
              title={item.reason}>{item.label}{item.reason === undefined ? '' : ` · ${item.reason}`}</option>} />
        </select>
        <select aria-label={`${path} comparison`} value={String(compare.op ?? '')} disabled={disabled || choice === undefined || choice.disabled}
          onChange={(event) => update({ op: event.target.value })}>
          {!operators.includes(compare.op as ConditionOperator) && <option value={String(compare.op ?? '')}>{String(compare.op ?? '—')}</option>}
          {operators.map((op) => <option key={op} value={op}>{op}</option>)}
        </select>
        <LiteralField choice={choice} value={compare.value ?? null} disabled={disabled || choice === undefined || choice.disabled === true}
          label={`${path} value`} onChange={(next) => update({ value: next })} />
        {remove}
      </div>
    );
  }
  if (kind === 'not') {
    return (
      <fieldset className="interrupt-condition-group" aria-label={path}>
        <legend>{kindSelect}{remove}</legend>
        <ConditionNode value={record.not ?? null} choices={choices} groups={groups} disabled={disabled}
          path={`${path}.not`} onChange={(next) => onChange({ not: next })} />
      </fieldset>
    );
  }
  if (kind === 'all' || kind === 'any') {
    const children = Array.isArray(record[kind]) ? record[kind] as JsonValue[] : [];
    return (
      <fieldset className="interrupt-condition-group" aria-label={path}>
        <legend>{kindSelect}{remove}</legend>
        {children.map((child, index) => (
          <ConditionNode key={index} value={child} choices={choices} groups={groups} disabled={disabled}
            path={`${path}.${kind}[${index}]`}
            onChange={(next) => onChange({ [kind]: children.map((item, at) => (at === index ? next : item)) })}
            onRemove={() => onChange({ [kind]: children.filter((_, at) => at !== index) })} />
        ))}
        <button type="button" className="text-button" disabled={disabled}
          onClick={() => onChange({ [kind]: [...children, newCompare(choices.find((choice) => !choice.disabled))] })}>Add condition</button>
      </fieldset>
    );
  }
  return (
    <div className="interrupt-condition-compare" role="group" aria-label={path}>
      {kindSelect}
      <code>{JSON.stringify(value)}</code>
      {remove}
    </div>
  );
}

/**
 * Structured editor of one interrupt condition: compare / all /
 * any / not, operands from the given choices (signals and declared scalar
 * keys grouped by namespace), operators by value type and a typed literal.
 * A change commits only when the whole condition is valid; otherwise it stays
 * a local draft and is reported invalid.
 */
export function InterruptConditionEditor({ value, choices, disabled, label, onCommit, onInvalidChange }: {
  value: JsonValue | undefined;
  choices: readonly ConditionOperandChoice[];
  disabled: boolean;
  label: string;
  onCommit: (next: JsonValue) => void;
  onInvalidChange?: (message: string | null) => void;
}) {
  const [draft, setDraft] = useState<JsonValue | undefined>(value);
  const valueKey = JSON.stringify(value ?? null);
  useEffect(() => { setDraft(value); }, [valueKey]); // eslint-disable-line react-hooks/exhaustive-deps
  const groups = useMemo(() => {
    // Change-only signals are labelled "liveness unproven" and refused here;
    // they stay listed so an existing operand is explained.
    const signals = choices.filter((choice) => choice.key === '');
    const keys = groupByNamespace(choices.filter((choice) => choice.key !== ''), (choice) => choice.key);
    return [
      ...(signals.length === 0 ? [] : [{ id: '__signals', label: 'Input signals', items: signals }]),
      ...keys,
    ];
  }, [choices]);
  const problem = draft === undefined ? null : conditionProblem(draft, choices);
  const unguarded = unguardedNodeOutcomeKeys(draft);
  const report = useRef(onInvalidChange);
  useEffect(() => { report.current = onInvalidChange; }, [onInvalidChange]);
  useEffect(() => { report.current?.(problem); }, [problem]);
  useEffect(() => () => report.current?.(null), []);
  const change = (next: JsonValue) => {
    setDraft(next);
    if (conditionProblem(next, choices) === null) onCommit(next);
  };
  if (draft === undefined) {
    return (
      <div className="interrupt-condition-editor" aria-label={label}>
        <p className="muted">No condition yet.</p>
        {choices.length === 0 && (
          <p className="program-form-hint">No bound input signal or declared scalar key is available.</p>
        )}
        <button type="button" disabled={disabled || choices.length === 0}
          onClick={() => change(newCompare(choices.find((choice) => !choice.disabled)))}>Add condition</button>
      </div>
    );
  }
  return (
    <div className="interrupt-condition-editor" aria-label={label}>
      <ConditionNode value={draft} choices={choices} groups={groups} disabled={disabled}
        path="condition" onChange={change} />
      {problem !== null && (
        <p className="program-form-error inline-message severity-error" role="alert">{problem} Not saved.</p>
      )}
      {unguarded.length > 0 && (
        <p className="program-form-hint" role="note" aria-label={`${label} none hint`}>
          <code>none</code> means the node has no result yet in this Run; add <code>!= none</code> if
          {' '}‘not completed’ should only count after the node ran ({unguarded.join(', ')}).
        </p>
      )}
    </div>
  );
}
