import { useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';

import type {
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../../types/api';
import { firstCatalogSchemaValidationMessage } from '../../utils/catalogSchema';
import { keyValueTyping, type KeyValueTyping } from '../../utils/keyValueAuthoring';
import {
  programBlackboardKeys,
  programSemanticChoices,
  programSemanticCreateTarget,
  programSemanticExpectedAccess,
  programSemanticExpectedType,
  type ProgramSemanticChoice,
  type ProgramSemanticCreateTarget,
} from '../../utils/programAuthoring';
import type { FrameScope } from '../../utils/programFrames';
import type { ProgramSourceJointInventory } from '../../utils/programSourceJoints';
import {
  listDeleteMove,
  listReorderMove,
  parseValuePath,
  type ListMove,
} from '../../utils/valuePaths';
import {
  BlackboardValueEditor,
  blackboardValueDraft,
  blackboardValueDraftIssue,
  parseBlackboardValueDraft,
} from '../BlackboardValueEditor';
import { DraftTextInput, DraftTextarea } from '../common/DraftInputs';
import {
  defaultMotionPoseValue,
  ProgramManualFillPreviewContext,
  ProgramSpecializedEditor,
  supportsProgramSpecializedEditor,
  type ValueCommit,
} from './ProgramSpecializedEditors';
import { SourceableLeaf, SourceableValue } from './ValueSourceField';
import { NamespaceGroupOptions, useNamespaceGroups } from '../common/NamespaceGroupOptions';

/** The node parameter a reference created from a picker is written to. */
export interface ProgramReferenceSource {
  nodeId: string;
  parameterId: string;
  semanticType: string;
  /**
   * The whole parameter value naming the created reference, from the current
   * one; when omitted the parameter becomes the created id itself. A picker
   * inside an object parameter (`on_grasp_lost.set_key`) sets only its field.
   */
  parameterValue?: (current: JsonValue | undefined, createdId: string) => JsonValue;
}

/** The literal a created reference writes to its node parameter. */
export function createdReferenceValue(
  source: ProgramReferenceSource,
  current: JsonValue | undefined,
  createdId: string,
): JsonValue {
  return source.parameterValue === undefined ? createdId : source.parameterValue(current, createdId);
}

export interface ProgramSchemaValueEditorContext {
  entry: CatalogEntry;
  node: ProgramTreeNode;
  parameterId: string;
  program: ProgramAuthoringDocument;
  frameScope?: FrameScope;
  directReferenceChoices?: Record<string, Array<{ value: JsonValue; label: string; reason?: string }>>;
  /** The Deployment Draft's global rule ids (`interrupt_state.<rule_id>` keys). */
  globalRuleIds?: readonly string[];
  sourceJointInventory?: ProgramSourceJointInventory | null;
  /** Replaces the Draft-derived choices of this semantic parameter (an io node's bound input signals). */
  semanticChoices?: ProgramSemanticChoice[] | null;
  /** Enum values shown but not selectable for this parameter, with why. */
  unsupportedChoices?: { values: readonly JsonValue[]; hint: string };
  /** An empty number omits this optional parameter instead of being refused. */
  emptyOmits?: { placeholder: string; onOmit: () => void };
  onCreateReference?: (
    target: ProgramSemanticCreateTarget,
    expectedType: string | null,
    expectedAccess: 'read' | 'write' | 'read_write' | null,
    source: ProgramReferenceSource,
  ) => void;
}

interface ProgramSchemaValueEditorProps {
  schema: CatalogJsonSchema;
  value: JsonValue | undefined;
  disabled: boolean;
  context: ProgramSchemaValueEditorContext;
  /** The value's path in the node (`goal.position_m[2]`), where its values may come from sources. */
  path?: string;
  onCommit: ValueCommit;
  onInvalidChange?: (message: string | null) => void;
}

const DIRECT_REFERENCE_FIELDS: Record<string, readonly string[]> = {
  target_reference: ['component_id', 'command_target_id'],
  frame_reference: ['component_id', 'frame_id'],
  collision_body_reference: ['component_id', 'collision_body_id'],
  signal_reference: ['component_id', 'signal_name'],
};

function referenceKey(value: JsonValue | undefined, fields: readonly string[]): string {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return '';
  const parts = fields.map((field) => value[field]);
  return parts.every((part) => typeof part === 'string') ? JSON.stringify(parts) : '';
}

function DirectReferenceEditor({ schema, value, disabled, context, path = 'value', onCommit, onInvalidChange }: ProgramSchemaValueEditorProps) {
  const semanticType = schema['x-manyforge-semantic-type']!;
  const fields = DIRECT_REFERENCE_FIELDS[semanticType];
  const choices = context.directReferenceChoices?.[semanticType] ?? [];
  const selected = referenceKey(value, fields);
  const current = choices.find((choice) => referenceKey(choice.value, fields) === selected);
  const issue = selected && (!current || current.reason) ? current?.reason ?? 'This reference is unavailable in the current Draft candidate.' : null;
  useInvalidReport(issue || null, onInvalidChange);
  return <select
    aria-label={`Choose ${semanticType.replaceAll('_', ' ')} for ${path.replaceAll('_', ' ')}`}
    aria-invalid={Boolean(issue) || undefined}
    title={issue || undefined}
    value={selected}
    disabled={disabled}
    onChange={(event) => {
      const choice = choices.find((row) => referenceKey(row.value, fields) === event.target.value);
      if (choice && !choice.reason) onCommit(choice.value);
    }}
  >
    {!selected && <option value="" disabled>Select {semanticType.replaceAll('_', ' ')}</option>}
    {selected && !current && <option value={selected} disabled>{selected} · unavailable in the current Draft candidate</option>}
    {choices.map((choice) => <option key={referenceKey(choice.value, fields)} value={referenceKey(choice.value, fields)} disabled={Boolean(choice.reason)}>
      {choice.label}{choice.reason ? ` · ${choice.reason}` : ''}
    </option>)}
  </select>;
}

/** The Program's frames as one node sees them: its own robot's frames first. */
function nodeFrameScope(context: ProgramSchemaValueEditorContext): FrameScope {
  const target = context.node.parameters.target;
  const targetObject = target !== null && typeof target === 'object' && !Array.isArray(target) ? target : null;
  const componentId = typeof targetObject?.component_id === 'string' ? targetObject.component_id : undefined;
  return {
    program: context.program,
    ...context.frameScope,
    ...(componentId === undefined ? {} : { movingComponentId: componentId }),
  };
}

// A relative motion states its goal from target_reference (motion_target_resolution).
function nodeHasRelativeGoal(context: ProgramSchemaValueEditorContext): boolean {
  const targetMode = context.node.parameters.target_mode
    ?? context.entry.parameters.find((parameter) => parameter.id === 'target_mode')?.default;
  return targetMode === 'relative';
}

// Reporting belongs to a mounted field, not to the parent's callback identity.
function useInvalidReport(message: string | null, onChange?: (message: string | null) => void) {
  const callback = useRef(onChange);
  useEffect(() => { callback.current = onChange; }, [onChange]);
  useEffect(() => { callback.current?.(message); }, [message]);
  useEffect(() => () => callback.current?.(null), []);
}

function useChildErrors() {
  const [errors, setErrors] = useState<Record<string, string>>({});
  const report = useCallback((key: string, message: string | null) => {
    setErrors((current) => {
      if ((current[key] ?? null) === message) return current;
      const next = { ...current };
      if (message === null) delete next[key];
      else next[key] = message;
      return next;
    });
  }, []);
  return { firstError: Object.values(errors)[0] ?? null, report };
}

interface EditorRow {
  id: number;
  value: JsonValue;
}

function plainObject(value: JsonValue | undefined): Record<string, JsonValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value
    : {};
}

function objectDraftValue(
  schema: CatalogJsonSchema,
  value: JsonValue | undefined,
): Record<string, JsonValue> {
  if (value !== undefined) return plainObject(value);
  return plainObject(defaultSchemaValue(schema));
}

function numericValidationMessage(
  schema: CatalogJsonSchema,
  text: string,
  path: string,
): string | null {
  if (text.trim() === '') return `${path} requires a number`;
  const candidate = Number(text);
  if (!Number.isFinite(candidate)) return `${path} requires a finite number`;
  return firstCatalogSchemaValidationMessage(schema, candidate, path);
}

/** The neutral starting value of one schema: its default, else its first choice or zero value. */
export function defaultSchemaValue(schema: CatalogJsonSchema): JsonValue {
  if (schema.default !== undefined) return schema.default as JsonValue;
  if (schema.enum?.length) return schema.enum[0] as JsonValue;
  const pose = defaultMotionPoseValue(schema);
  if (pose !== undefined) return pose;
  if (schema.type === 'boolean') return false;
  if (schema.type === 'number' || schema.type === 'integer') return 0;
  if (schema.type === 'array') return [];
  if (schema.type === 'object') {
    return Object.fromEntries((schema.required ?? []).map((name) => [
      name,
      defaultSchemaValue(schema.properties?.[name] ?? {}),
    ])) as JsonValue;
  }
  return '';
}

function numericLabels(path: string, length: number): string[] {
  if (/rpy|rotation/i.test(path) && length === 3) return ['Roll', 'Pitch', 'Yaw'];
  if (/quaternion|xyzw|orientation/i.test(path) && length === 4) {
    return ['X', 'Y', 'Z', 'W'];
  }
  if (length === 3) return ['X', 'Y', 'Z'];
  return Array.from({ length }, (_unused, index) => String(index + 1));
}

function SemanticChoiceEditor({
  schema,
  value,
  disabled,
  context,
  path = 'value',
  onCommit,
}: ProgramSchemaValueEditorProps) {
  const overridden = context.semanticChoices != null;
  const choices = context.semanticChoices ?? programSemanticChoices(
    schema,
    context.node,
    context.entry,
    context.parameterId,
    context.program,
    context.globalRuleIds,
  );
  const semanticType = schema['x-manyforge-semantic-type'];
  const namespaces = useNamespaceGroups();
  const createTarget = programSemanticCreateTarget(semanticType);
  const canCreate = createTarget !== null && context.onCreateReference !== undefined;
  const expectedType = programSemanticExpectedType(
    context.entry,
    context.parameterId,
    semanticType,
  );
  const expectedAccess = semanticType === 'blackboard_key'
    ? programSemanticExpectedAccess(context.entry, context.parameterId)
    : null;
  const current = typeof value === 'string' ? value : '';
  const currentMissing = current !== ''
    && !choices.some((choice) => choice.value === current);
  const grouped = new Map<string, typeof choices>();
  for (const choice of choices) {
    const group = choice.group ?? 'Available';
    grouped.set(group, [...(grouped.get(group) ?? []), choice]);
  }
  const renderChoice = (choice: (typeof choices)[number]) => (
    <option key={choice.value} value={choice.value}>{choice.label}</option>
  );
  return (
    <select
      aria-label={`Choose ${semanticType?.replaceAll('_', ' ')} for ${path.replaceAll('_', ' ')}`}
      className={currentMissing ? 'program-reference-invalid' : undefined}
      value={current}
      disabled={disabled}
      aria-invalid={currentMissing || undefined}
      title={currentMissing
        ? overridden
          ? semanticType === 'blackboard_key'
            ? 'Current blackboard key is not a declared writable bool key.'
            : `Current ${semanticType} is not a bound input signal of this target.`
          : `Current ${semanticType} is not declared in this Draft.`
        : undefined}
      onChange={(event) => {
        if (namespaces.handleToggle(event.target.value)) return;
        if (event.target.value === '__create__') {
          if (canCreate && createTarget) {
            context.onCreateReference?.(createTarget, expectedType, expectedAccess, {
              nodeId: context.node.instance_id,
              parameterId: context.parameterId,
              semanticType: semanticType ?? '',
            });
          }
          return;
        }
        onCommit(event.target.value);
      }}
    >
      <option value="" disabled>Select {semanticType?.replaceAll('_', ' ')}</option>
      {currentMissing && (
        <option value={current}>
          {overridden
            ? semanticType === 'blackboard_key'
              ? 'Current (not a declared writable bool key)'
              : 'Current (not a bound input)'
            : 'Current (not declared)'} · {current}
        </option>
      )}
      {semanticType === 'blackboard_key' ? (
        // Keys are grouped by namespace, each group collapsed by default.
        <NamespaceGroupOptions
          groups={[...grouped.entries()].map(([group, rows]) => ({ id: group, label: group, items: rows }))}
          expanded={namespaces.expanded}
          current={current}
          valueOf={(choice) => choice.value}
          option={renderChoice}
        />
      ) : [...grouped.entries()].map(([group, rows]) => (
        <optgroup key={group} label={group}>{rows.map(renderChoice)}</optgroup>
      ))}
      {canCreate && <option value="__create__">Create new…</option>}
    </select>
  );
}

function FixedTupleEditor({
  schema,
  value,
  disabled,
  path = 'value',
  onCommit,
  onInvalidChange,
}: ProgramSchemaValueEditorProps) {
  const length = schema.minItems ?? schema.maxItems ?? 0;
  const values = Array.isArray(value) ? value : Array.from({ length }, () => 0);
  const labels = numericLabels(path, length);
  const { firstError, report } = useChildErrors();
  useInvalidReport(firstError, onInvalidChange);
  return (
    <fieldset className="program-vector-editor program-schema-tuple">
      <legend>{path.split('.').at(-1)?.replaceAll('_', ' ')}</legend>
      <div>
        {labels.map((label, index) => (
          <label className="program-specialized-number" key={label}>
            <span>{label}</span>
            <SourceableLeaf path={`${path}[${index}]`} schema={schema.items ?? {}} label={label}>
              <DraftTextInput
                type="number"
                step={schema.items?.type === 'integer' ? 1 : 'any'}
                value={typeof values[index] === 'number' ? values[index] as number : ''}
                disabled={disabled}
                validate={(text) => {
                  const candidate = [...values];
                  const itemMessage = numericValidationMessage(
                    schema.items ?? {},
                    text,
                    `${path}[${index}]`,
                  );
                  if (itemMessage) return itemMessage;
                  candidate[index] = Number(text);
                  return firstCatalogSchemaValidationMessage(schema, candidate, path);
                }}
                onInvalidChange={(message) => report(String(index), message)}
                onCommit={(text) => {
                  const candidate = [...values];
                  candidate[index] = Number(text);
                  onCommit(candidate);
                }}
              />
            </SourceableLeaf>
          </label>
        ))}
      </div>
    </fieldset>
  );
}

function ArrayEditor(props: ProgramSchemaValueEditorProps) {
  const { schema, value, disabled, context, path = 'value', onCommit, onInvalidChange } = props;
  const incoming = Array.isArray(value) ? value : [];
  const [rows, setRows] = useState<EditorRow[]>(() => incoming.map((value, id) => ({ id, value })));
  const nextId = useRef(incoming.length);
  const draft = rows.map((row) => row.value);
  const [ownError, setOwnError] = useState<string | null>(null);
  const { firstError, report } = useChildErrors();
  const error = firstError ?? ownError;
  const [expanded, setExpanded] = useState(incoming.length <= 4);
  useEffect(() => {
    const values = Array.isArray(value) ? value : [];
    setRows((current) => values.map((item, index) => ({
      id: current[index]?.id ?? nextId.current++, value: item,
    })));
    setOwnError(null);
  }, [value]);
  useInvalidReport(error, onInvalidChange);
  const submit = (nextRows: EditorRow[], moves: readonly ListMove[] = []) => {
    setRows(nextRows);
    const candidate = nextRows.map((row) => row.value);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate, moves);
  };
  const itemSchema = schema.items ?? {};
  return (
    <details
      className={`program-schema-list${error ? ' invalid' : ''}`}
      open={expanded || error !== null}
      onToggle={(event) => setExpanded(event.currentTarget.open)}
    >
      <summary>{draft.length} item(s){error ? ' · needs attention' : ''}</summary>
      <div className="program-schema-list-rows">
        {rows.map((row, index) => (
          <div className="program-schema-list-row" key={row.id}>
            <ProgramSchemaValueEditor
              schema={itemSchema}
              value={row.value}
              disabled={disabled}
              context={context}
              path={`${path}[${index}]`}
              onCommit={(nextItem, moves) => {
                const candidate = [...rows];
                candidate[index] = { ...row, value: nextItem };
                submit(candidate, moves);
              }}
              onInvalidChange={(message) => report(String(row.id), message)}
            />
            <div className="program-schema-row-actions">
              <button type="button" disabled={disabled || index === 0} onClick={() => {
                const candidate = [...rows];
                [candidate[index - 1], candidate[index]] = [candidate[index], candidate[index - 1]];
                submit(candidate, [listReorderMove(path, rows.length, index, index - 1)]);
              }}>Earlier</button>
              <button type="button" disabled={disabled || index === draft.length - 1} onClick={() => {
                const candidate = [...rows];
                [candidate[index], candidate[index + 1]] = [candidate[index + 1], candidate[index]];
                submit(candidate, [listReorderMove(path, rows.length, index, index + 1)]);
              }}>Later</button>
              <button type="button" className="text-button" disabled={disabled} onClick={() => {
                report(String(row.id), null);
                submit(
                  rows.filter((item) => item.id !== row.id),
                  [listDeleteMove(path, rows.length, index)],
                );
              }}>Remove</button>
            </div>
          </div>
        ))}
        <button
          type="button"
          className="text-button"
          disabled={disabled || (schema.maxItems !== undefined && draft.length >= schema.maxItems)}
          onClick={() => {
            setExpanded(true);
            const candidate = [...rows, { id: nextId.current++, value: defaultSchemaValue(itemSchema) }];
            if (itemSchema['x-manyforge-semantic-type']) {
              setRows(candidate);
              setOwnError(`${path}[${candidate.length - 1}]: select a declared ${itemSchema['x-manyforge-semantic-type'].replaceAll('_', ' ')}.`);
            } else {
              submit(candidate);
            }
          }}
        >
          Add row
        </button>
        {error && <p className="program-form-error inline-message severity-error" role="alert">{error}</p>}
      </div>
    </details>
  );
}

function ObjectEditor(props: ProgramSchemaValueEditorProps) {
  const { schema, value, disabled, context, path = 'value', onCommit, onInvalidChange } = props;
  const previewValues = useContext(ProgramManualFillPreviewContext);
  const defaults = useMemo(() => Object.fromEntries(
    Object.entries(schema.properties ?? {}).map(([name, child]) => [name, defaultSchemaValue(child)]),
  ), [schema]);
  const incoming = objectDraftValue(schema, value);
  const [draft, setDraft] = useState<Record<string, JsonValue>>(incoming);
  const [ownError, setOwnError] = useState<string | null>(null);
  const { firstError, report } = useChildErrors();
  const error = firstError ?? ownError;
  useEffect(() => {
    setDraft(objectDraftValue(schema, value));
    setOwnError(null);
  }, [schema, value]);
  useInvalidReport(error, onInvalidChange);
  const submit = (candidate: Record<string, JsonValue>, moves?: readonly ListMove[]) => {
    setDraft(candidate);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate, moves);
  };
  const properties = schema.properties ?? {};
  const required = new Set(schema.required ?? []);
  return (
    <fieldset className={`program-schema-object${error ? ' invalid' : ''}`}>
      <legend>{path.split('.').at(-1)?.replaceAll('_', ' ')}</legend>
      {Object.entries(properties).map(([name, childSchema]) => {
        const present = Object.prototype.hasOwnProperty.call(draft, name);
        const previewValue = previewValues.get(`${path}.${name}`);
        if (!present && !required.has(name)) {
          return (
            <button
              type="button"
              className={`text-button${previewValue === undefined ? '' : ' program-manual-fill-preview'}`}
              key={name}
              disabled={disabled}
              onClick={() => {
                submit({ ...draft, [name]: defaultSchemaValue(childSchema) });
              }}
            >
              Add {name.replaceAll('_', ' ')}
              {previewValue !== undefined && (
                <span className="program-manual-fill-new-value" aria-hidden="true"> → {previewValue}</span>
              )}
            </button>
          );
        }
        return (
          <div
            className={`program-schema-object-field${previewValue === undefined ? '' : ' program-manual-fill-changed'}`}
            key={name}
          >
            <div className={`program-property-heading${previewValue === undefined ? '' : ' program-manual-fill-changed'}`}>
              <span>
                {name}
                {previewValue !== undefined && (
                  <span className="program-manual-fill-new-value" aria-hidden="true">
                    {' '}→ {previewValue}
                  </span>
                )}
              </span>
              {!required.has(name) && (
                <button type="button" className="text-button" disabled={disabled} onClick={() => {
                  const candidate = { ...draft };
                  delete candidate[name];
                  report(name, null);
                  submit(candidate);
                }}>Remove</button>
              )}
            </div>
            <ProgramSchemaValueEditor
              schema={childSchema}
              value={present ? draft[name] : defaults[name]}
              disabled={disabled}
              context={context}
              path={`${path}.${name}`}
              onCommit={(child, moves) => submit({ ...draft, [name]: child }, moves)}
              onInvalidChange={(message) => report(name, message)}
            />
          </div>
        );
      })}
      {error && <p className="program-form-error inline-message severity-error" role="alert">{error}</p>}
    </fieldset>
  );
}

function MapEditor(props: ProgramSchemaValueEditorProps) {
  const { schema, value, disabled, context, path = 'value', onCommit, onInvalidChange } = props;
  const previewValues = useContext(ProgramManualFillPreviewContext);
  const incoming = plainObject(value);
  const [rows, setRows] = useState(() => Object.entries(incoming).map(
    ([key, value], id) => ({ id, key, value }),
  ));
  const nextId = useRef(rows.length);
  const draft = Object.fromEntries(rows.map((row) => [row.key, row.value]));
  const [newKey, setNewKey] = useState('');
  const [ownError, setOwnError] = useState<string | null>(null);
  const { firstError, report } = useChildErrors();
  const error = firstError ?? ownError;
  const itemSchema = typeof schema.additionalProperties === 'object'
    ? schema.additionalProperties
    : {};
  useEffect(() => {
    setRows((current) => Object.entries(plainObject(value)).map(([key, item]) => ({
      id: current.find((row) => row.key === key)?.id ?? nextId.current++, key, value: item,
    })));
    setOwnError(null);
  }, [value]);
  useInvalidReport(error, onInvalidChange);
  // Entries a value source supplies alone, without a typed value in the map.
  const prefix = parseValuePath(path);
  const sourcedEntries = prefix === null ? [] : Object.keys(context.node.value_sources ?? {})
    .map((entry) => parseValuePath(entry))
    .filter((segments): segments is NonNullable<typeof segments> => segments !== null
      && segments.length === prefix.length + 1
      && prefix.every((segment, index) => segments[index] === segment))
    .map((segments) => segments[prefix.length])
    .filter((key): key is string => typeof key === 'string'
      && !Object.prototype.hasOwnProperty.call(draft, key))
    .sort();
  const previewOnlyEntries = new Map<string, Array<[string, string]>>();
  const previewPrefix = `${path}.`;
  for (const [fieldPath, previewValue] of previewValues) {
    if (!fieldPath.startsWith(previewPrefix)) continue;
    const remainder = fieldPath.slice(previewPrefix.length);
    const separator = remainder.indexOf('.');
    if (separator <= 0) continue;
    const key = remainder.slice(0, separator);
    if (Object.prototype.hasOwnProperty.call(draft, key) || sourcedEntries.includes(key)) continue;
    const fields = previewOnlyEntries.get(key) ?? [];
    fields.push([remainder.slice(separator + 1), previewValue]);
    previewOnlyEntries.set(key, fields);
  }
  const submit = (nextRows: typeof rows, moves?: readonly ListMove[]) => {
    setRows(nextRows);
    const candidate = Object.fromEntries(nextRows.map((row) => [row.key, row.value]));
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate, moves);
  };
  return (
    <fieldset className={`program-schema-map${error ? ' invalid' : ''}`}>
      <legend>{path.split('.').at(-1)?.replaceAll('_', ' ')}</legend>
      {rows.map((row) => (
        <div className="program-schema-map-row" key={row.id}>
          <DraftTextInput
            aria-label={`${path} key ${row.key}`}
            value={row.key}
            disabled={disabled}
            validate={(replacement) => {
              const normalized = replacement.trim();
              if (!normalized) return 'Map key is required.';
              return normalized !== row.key && Object.prototype.hasOwnProperty.call(draft, normalized)
                ? `Map key '${normalized}' already exists.`
                : null;
            }}
            onInvalidChange={(message) => report(`${row.id}:key`, message)}
            onCommit={(replacement) => {
              const normalized = replacement.trim();
              if (!normalized || normalized === row.key) return;
              submit(rows.map((item) => item.id === row.id ? { ...item, key: normalized } : item));
            }}
          />
          <ProgramSchemaValueEditor
            schema={itemSchema}
            value={row.value}
            disabled={disabled}
            context={context}
            path={`${path}.${row.key}`}
            onCommit={(value, moves) => submit(
              rows.map((item) => item.id === row.id ? { ...item, value } : item),
              moves,
            )}
            onInvalidChange={(message) => report(`${row.id}:value`, message)}
          />
          <button
            type="button"
            className="text-button"
            disabled={disabled}
            onClick={() => {
              report(`${row.id}:key`, null);
              report(`${row.id}:value`, null);
              submit(rows.filter((item) => item.id !== row.id));
            }}
          >
            Remove
          </button>
        </div>
      ))}
      {sourcedEntries.map((key) => (
        <div className="program-schema-map-row" key={`sourced:${key}`} data-map-entry-source={key}>
          <input aria-label={`${path} key ${key}`} value={key} readOnly disabled={disabled} />
          <ProgramSchemaValueEditor
            schema={itemSchema}
            value={defaultSchemaValue(itemSchema)}
            disabled={disabled}
            context={context}
            path={`${path}.${key}`}
            onCommit={(value) => submit([...rows, { id: nextId.current++, key, value }])}
            onInvalidChange={(message) => report(`sourced:${key}`, message)}
          />
        </div>
      ))}
      {[...previewOnlyEntries].map(([key, fields]) => (
        <div className="program-schema-map-row program-schema-map-preview-row" key={`preview:${key}`}>
          <span className="program-schema-map-preview-key">{key}</span>
          <div className="program-schema-map-preview-fields">
            {fields.map(([field, previewValue]) => (
              <span key={field}>
                {field.replaceAll('.', ' › ')}
                <span className="program-manual-fill-new-value"> → {previewValue}</span>
              </span>
            ))}
          </div>
        </div>
      ))}
      <div className="program-schema-map-add">
        <input
          value={newKey}
          disabled={disabled}
          aria-label={`New ${path} key`}
          placeholder="New key"
          onChange={(event) => setNewKey(event.target.value)}
        />
        <button
          type="button"
          disabled={disabled || !newKey.trim()}
          onClick={() => {
            const normalized = newKey.trim();
            if (
              Object.prototype.hasOwnProperty.call(draft, normalized)
              || sourcedEntries.includes(normalized)
            ) {
              setOwnError(`Map key '${normalized}' already exists.`);
              return;
            }
            setNewKey('');
            submit([...rows, { id: nextId.current++, key: normalized, value: defaultSchemaValue(itemSchema) }]);
          }}
        >
          Add
        </button>
      </div>
      {error && <p className="program-form-error inline-message severity-error" role="alert">{error}</p>}
    </fieldset>
  );
}

/** The JSON of the value a draft stands for, or null while it is incomplete. */
function draftValueJson(type: string, draft: string, sceneResourceIds: readonly string[]): string | null {
  try {
    return JSON.stringify(parseBlackboardValueDraft(type, draft, sceneResourceIds));
  } catch {
    return null;
  }
}

function KeyValueInput({
  typing,
  type,
  value,
  disabled,
  context,
  path = 'value',
  onCommit,
  onInvalidChange,
}: ProgramSchemaValueEditorProps & { typing: KeyValueTyping; type: string }) {
  const sceneResourceIds = useMemo(
    () => (context.program.scene ?? []).map((resource) => resource.scene_resource_id),
    [context.program.scene],
  );
  const shownDraft = blackboardValueDraft(type, value, sceneResourceIds);
  const [draft, setDraft] = useState(shownDraft);
  // A new stored value (undo, another editor) replaces a draft that no longer
  // stands for it; this field's own commit keeps the draft and the mounted
  // fields, so a sibling's unfinished text survives it.
  const [shownValue, setShownValue] = useState(value);
  if (JSON.stringify(value ?? null) !== JSON.stringify(shownValue ?? null)) {
    setShownValue(value);
    if (draftValueJson(type, draft, sceneResourceIds) !== JSON.stringify(value ?? null)) {
      setDraft(shownDraft);
    }
  }
  // An unfinished pose field is not in the draft yet: it is reported by the field.
  const [fieldIssue, setFieldIssue] = useState<string | null>(null);
  // An untouched neutral draft (no stored value yet) is not an invalid edit.
  useInvalidReport(
    fieldIssue
      ?? (draft === shownDraft ? null : blackboardValueDraftIssue(type, draft, sceneResourceIds)),
    onInvalidChange,
  );
  const commit = (next: string) => {
    if (blackboardValueDraftIssue(type, next, sceneResourceIds) !== null) return;
    const parsed = parseBlackboardValueDraft(type, next, sceneResourceIds);
    if (JSON.stringify(parsed) !== JSON.stringify(value)) onCommit(parsed);
  };
  const label = `${path.split('.').at(-1)} (${typing.key?.key ?? ''}: ${type})`;
  const editor = (
    <div
      onBlur={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget as Node | null)) commit(draft);
      }}
      onKeyDown={(event) => {
        if (event.key === 'Enter' && event.target instanceof HTMLInputElement) commit(draft);
      }}
    >
      <BlackboardValueEditor
        id={`property-${context.node.instance_id}-${path}-value`}
        label={label}
        type={type}
        draft={draft}
        sceneResourceIds={sceneResourceIds}
        frameScope={{
          ...nodeFrameScope(context),
          ...(typing.key?.allowed_frames === undefined
            ? {}
            : { allowedFrames: typing.key.allowed_frames }),
        }}
        disabled={disabled}
        path={type === 'stored_pose' ? path : undefined}
        onInvalidChange={setFieldIssue}
        wrapControl={(control) => (
          <SourceableLeaf path={path} schema={typing.schema ?? {}} label={label}>{control}</SourceableLeaf>
        )}
        onDraftChange={(next) => {
          setDraft(next);
          // Choices and pose fields are complete edits; typed text commits on leaving the field.
          if (type === 'bool' || type === 'scene_resource' || type === 'stored_pose') commit(next);
        }}
      />
    </div>
  );
  return type === 'stored_pose'
    ? <SourceableValue path={path} schema={typing.schema ?? {}}>{editor}</SourceableValue>
    : editor;
}

/** A value typed by the key another parameter names (Set value, Check value, Wait for value). */
function KeyValueEditor(props: ProgramSchemaValueEditorProps & { typing: KeyValueTyping }) {
  const { typing } = props;
  if (typing.key === null || typing.schema === null) {
    return <p className="program-form-hint">{typing.issue}</p>;
  }
  // Another key starts a new draft; a new value of the same key does not remount the fields.
  return (
    <KeyValueInput
      key={`${typing.key.key}:${typing.key.type}`}
      {...props}
      type={typing.key.type}
    />
  );
}

function leafLabel(path: string): string {
  return path.split('.').at(-1)?.replaceAll('_', ' ') ?? path;
}

export function ProgramSchemaValueEditor(props: ProgramSchemaValueEditorProps) {
  const { schema, value: incoming, disabled, context, path = 'value', onCommit, onInvalidChange } = props;
  // Owner readback may reconstruct equal JSON. It is not a local-input reset.
  // Keep only this mounted field's current value, not a shared history/cache.
  const [value, setValue] = useState(incoming);
  if (incoming !== value && JSON.stringify(incoming) !== JSON.stringify(value)) setValue(incoming);
  const fallback = useMemo(() => defaultSchemaValue(schema), [schema]);
  const stableProps = { ...props, value };
  const whole = (editor: ReactNode) => (
    <SourceableValue path={path} schema={schema}>{editor}</SourceableValue>
  );
  const leaf = (editor: ReactNode) => (
    <SourceableLeaf path={path} schema={schema} label={leafLabel(path)}>{editor}</SourceableLeaf>
  );
  const typing = keyValueTyping(schema, context.node, programBlackboardKeys(context.program, context.globalRuleIds));
  if (typing !== null) return <KeyValueEditor {...stableProps} typing={typing} />;
  const semanticType = schema['x-manyforge-semantic-type'];
  if (semanticType && DIRECT_REFERENCE_FIELDS[semanticType]) {
    return whole(<DirectReferenceEditor {...stableProps} />);
  }
  if (semanticType && semanticType !== 'source_joint') {
    return whole(<SemanticChoiceEditor {...stableProps} />);
  }
  if (schema.enum) {
    const encoded = value === undefined ? '' : JSON.stringify(value);
    const unsupported = path === context.parameterId ? context.unsupportedChoices : undefined;
    const unsupportedValues = new Set(unsupported?.values.map((option) => JSON.stringify(option)));
    return leaf(
      <select value={encoded} disabled={disabled} onChange={(event) => {
        onCommit(JSON.parse(event.target.value) as JsonValue);
      }}>
        <option value="" disabled>Select a value</option>
        {schema.enum.map((option) => {
          const encodedOption = JSON.stringify(option);
          const refused = unsupportedValues.has(encodedOption);
          return (
            <option
              key={encodedOption}
              value={encodedOption}
              disabled={refused}
              title={refused ? unsupported?.hint : undefined}
            >
              {refused ? `${String(option)} (not supported yet)` : String(option)}
            </option>
          );
        })}
      </select>,
    );
  }
  if (schema.type === 'boolean') {
    return leaf(
      <select value={typeof value === 'boolean' ? String(value) : ''} disabled={disabled} onChange={(event) => {
        onCommit(event.target.value === 'true');
      }}>
        <option value="" disabled>Select true or false</option>
        <option value="true">true</option>
        <option value="false">false</option>
      </select>,
    );
  }
  if (schema.type === 'number' || schema.type === 'integer') {
    const emptyOmits = path === context.parameterId ? context.emptyOmits : undefined;
    return leaf(
      <DraftTextInput
        type="number"
        step={schema.type === 'integer' ? 1 : 'any'}
        value={typeof value === 'number' ? value : ''}
        disabled={disabled}
        placeholder={emptyOmits?.placeholder}
        validate={(text) => emptyOmits !== undefined && text.trim() === ''
          ? null
          : numericValidationMessage(schema, text, path)}
        onInvalidChange={onInvalidChange}
        onCommit={(text) => {
          if (emptyOmits !== undefined && text.trim() === '') emptyOmits.onOmit();
          else onCommit(Number(text));
        }}
      />,
    );
  }
  if (
    schema.type === 'array'
    && schema.minItems !== undefined
    && schema.minItems === schema.maxItems
    && ['number', 'integer'].includes(schema.items?.type ?? '')
  ) {
    return whole(<FixedTupleEditor {...stableProps} />);
  }
  if (supportsProgramSpecializedEditor(schema)) {
    return whole(
      <ProgramSpecializedEditor
        schema={schema}
        value={value === undefined ? fallback : value}
        disabled={disabled}
        path={path}
        sourceJointInventory={props.context.sourceJointInventory}
        frameScope={nodeFrameScope(props.context)}
        relativeGoal={nodeHasRelativeGoal(props.context)}
        onCommit={onCommit}
        onInvalidChange={onInvalidChange}
      />,
    );
  }
  if (schema.type === 'array') return whole(<ArrayEditor {...stableProps} />);
  if (schema.type === 'object' && typeof schema.additionalProperties === 'object') {
    return whole(<MapEditor {...stableProps} />);
  }
  if (schema.type === 'object' && schema.properties) return whole(<ObjectEditor {...stableProps} />);
  if (schema.type === 'string' || semanticType === 'source_joint') {
    return leaf(
      <DraftTextInput
        value={typeof value === 'string' ? value : ''}
        disabled={disabled}
        validate={(text) => firstCatalogSchemaValidationMessage(schema, text, path)}
        onInvalidChange={onInvalidChange}
        onCommit={onCommit}
      />,
    );
  }
  return whole(
    <DraftTextarea
      rows={5}
      value={value === undefined ? '' : JSON.stringify(value, null, 2)}
      disabled={disabled}
      spellCheck={false}
      aria-label={`${path} strict JSON repair`}
      validate={(text) => {
        try {
          return firstCatalogSchemaValidationMessage(schema, JSON.parse(text), path);
        } catch (error) {
          return error instanceof Error ? error.message : 'Value must be strict JSON.';
        }
      }}
      onInvalidChange={onInvalidChange}
      onCommit={(text) => onCommit(JSON.parse(text) as JsonValue)}
    />,
  );
}
