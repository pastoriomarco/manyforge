import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import type {
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../../types/api';
import { firstCatalogSchemaValidationMessage } from '../../utils/catalogSchema';
import {
  programSemanticChoices,
  programSemanticCreateTarget,
  programSemanticExpectedAccess,
  programSemanticExpectedType,
  type ProgramSemanticCreateTarget,
} from '../../utils/programAuthoring';
import type { ProgramSourceJointInventory } from '../../utils/programSourceJoints';
import { DraftTextInput, DraftTextarea } from '../common/DraftInputs';
import {
  ProgramSpecializedEditor,
  supportsProgramSpecializedEditor,
} from './ProgramSpecializedEditors';

export interface ProgramSchemaValueEditorContext {
  entry: CatalogEntry;
  node: ProgramTreeNode;
  parameterId: string;
  program: ProgramAuthoringDocument;
  sourceJointInventory?: ProgramSourceJointInventory | null;
  onCreateReference?: (
    target: ProgramSemanticCreateTarget,
    expectedType: string | null,
    expectedAccess: 'read' | 'write' | 'read_write' | null,
    source: { nodeId: string; parameterId: string; semanticType: string },
  ) => void;
}

interface ProgramSchemaValueEditorProps {
  schema: CatalogJsonSchema;
  value: JsonValue | undefined;
  disabled: boolean;
  context: ProgramSchemaValueEditorContext;
  path?: string;
  onCommit: (value: JsonValue) => void;
  onInvalidChange?: (message: string | null) => void;
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
  return plainObject(defaultValue(schema));
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

function defaultValue(schema: CatalogJsonSchema): JsonValue {
  if (schema.default !== undefined) return schema.default as JsonValue;
  if (schema.enum?.length) return schema.enum[0] as JsonValue;
  if (schema.type === 'boolean') return false;
  if (schema.type === 'number' || schema.type === 'integer') return 0;
  if (schema.type === 'array') return [];
  if (schema.type === 'object') {
    return Object.fromEntries((schema.required ?? []).map((name) => [
      name,
      defaultValue(schema.properties?.[name] ?? {}),
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
  onCommit,
}: ProgramSchemaValueEditorProps) {
  const choices = programSemanticChoices(
    schema,
    context.node,
    context.entry,
    context.parameterId,
    context.program,
  );
  const semanticType = schema['x-manyforge-semantic-type'];
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
  return (
    <select
      className={currentMissing ? 'program-reference-invalid' : undefined}
      value={current}
      disabled={disabled}
      aria-invalid={currentMissing || undefined}
      title={currentMissing ? `Current ${semanticType} is not declared in this Draft.` : undefined}
      onChange={(event) => {
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
      {currentMissing && <option value={current}>Current (not declared) · {current}</option>}
      {[...grouped.entries()].map(([group, rows]) => (
        <optgroup key={group} label={group}>
          {rows.map((choice) => (
            <option key={choice.value} value={choice.value}>{choice.label}</option>
          ))}
        </optgroup>
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
  const submit = (nextRows: EditorRow[]) => {
    setRows(nextRows);
    const candidate = nextRows.map((row) => row.value);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate);
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
              onCommit={(nextItem) => {
                const candidate = [...rows];
                candidate[index] = { ...row, value: nextItem };
                submit(candidate);
              }}
              onInvalidChange={(message) => report(String(row.id), message)}
            />
            <div className="program-schema-row-actions">
              <button type="button" disabled={disabled || index === 0} onClick={() => {
                const candidate = [...rows];
                [candidate[index - 1], candidate[index]] = [candidate[index], candidate[index - 1]];
                submit(candidate);
              }}>Earlier</button>
              <button type="button" disabled={disabled || index === draft.length - 1} onClick={() => {
                const candidate = [...rows];
                [candidate[index], candidate[index + 1]] = [candidate[index + 1], candidate[index]];
                submit(candidate);
              }}>Later</button>
              <button type="button" className="text-button" disabled={disabled} onClick={() => {
                report(String(row.id), null);
                submit(rows.filter((item) => item.id !== row.id));
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
            const candidate = [...rows, { id: nextId.current++, value: defaultValue(itemSchema) }];
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
  const defaults = useMemo(() => Object.fromEntries(
    Object.entries(schema.properties ?? {}).map(([name, child]) => [name, defaultValue(child)]),
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
  const submit = (candidate: Record<string, JsonValue>) => {
    setDraft(candidate);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate);
  };
  const properties = schema.properties ?? {};
  const required = new Set(schema.required ?? []);
  return (
    <fieldset className={`program-schema-object${error ? ' invalid' : ''}`}>
      <legend>{path.split('.').at(-1)?.replaceAll('_', ' ')}</legend>
      {Object.entries(properties).map(([name, childSchema]) => {
        const present = Object.prototype.hasOwnProperty.call(draft, name);
        if (!present && !required.has(name)) {
          return (
            <button type="button" className="text-button" key={name} disabled={disabled} onClick={() => {
              submit({ ...draft, [name]: defaultValue(childSchema) });
            }}>Add {name.replaceAll('_', ' ')}</button>
          );
        }
        return (
          <div className="program-schema-object-field" key={name}>
            <div className="program-property-heading">
              <span>{name}</span>
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
              onCommit={(child) => submit({ ...draft, [name]: child })}
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
  const submit = (nextRows: typeof rows) => {
    setRows(nextRows);
    const candidate = Object.fromEntries(nextRows.map((row) => [row.key, row.value]));
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setOwnError(message);
    if (message === null) onCommit(candidate);
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
            onCommit={(value) => submit(rows.map((item) => item.id === row.id ? { ...item, value } : item))}
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
            if (Object.prototype.hasOwnProperty.call(draft, normalized)) {
              setOwnError(`Map key '${normalized}' already exists.`);
              return;
            }
            setNewKey('');
            submit([...rows, { id: nextId.current++, key: normalized, value: defaultValue(itemSchema) }]);
          }}
        >
          Add
        </button>
      </div>
      {error && <p className="program-form-error inline-message severity-error" role="alert">{error}</p>}
    </fieldset>
  );
}

export function ProgramSchemaValueEditor(props: ProgramSchemaValueEditorProps) {
  const { schema, value: incoming, disabled, path = 'value', onCommit, onInvalidChange } = props;
  // Owner readback may reconstruct equal JSON. It is not a local-input reset.
  // Keep only this mounted field's current value, not a shared history/cache.
  const [value, setValue] = useState(incoming);
  if (incoming !== value && JSON.stringify(incoming) !== JSON.stringify(value)) setValue(incoming);
  const fallback = useMemo(() => defaultValue(schema), [schema]);
  const stableProps = { ...props, value };
  const semanticType = schema['x-manyforge-semantic-type'];
  if (semanticType && semanticType !== 'source_joint') {
    return <SemanticChoiceEditor {...stableProps} />;
  }
  if (schema.enum) {
    const encoded = value === undefined ? '' : JSON.stringify(value);
    return (
      <select value={encoded} disabled={disabled} onChange={(event) => {
        onCommit(JSON.parse(event.target.value) as JsonValue);
      }}>
        <option value="" disabled>Select a value</option>
        {schema.enum.map((option) => {
          const encodedOption = JSON.stringify(option);
          return <option key={encodedOption} value={encodedOption}>{String(option)}</option>;
        })}
      </select>
    );
  }
  if (schema.type === 'boolean') {
    return (
      <select value={typeof value === 'boolean' ? String(value) : ''} disabled={disabled} onChange={(event) => {
        onCommit(event.target.value === 'true');
      }}>
        <option value="" disabled>Select true or false</option>
        <option value="true">true</option>
        <option value="false">false</option>
      </select>
    );
  }
  if (schema.type === 'number' || schema.type === 'integer') {
    return (
      <DraftTextInput
        type="number"
        step={schema.type === 'integer' ? 1 : 'any'}
        value={typeof value === 'number' ? value : ''}
        disabled={disabled}
        validate={(text) => numericValidationMessage(schema, text, path)}
        onInvalidChange={onInvalidChange}
        onCommit={(text) => onCommit(Number(text))}
      />
    );
  }
  if (
    schema.type === 'array'
    && schema.minItems !== undefined
    && schema.minItems === schema.maxItems
    && ['number', 'integer'].includes(schema.items?.type ?? '')
  ) {
    return <FixedTupleEditor {...stableProps} />;
  }
  if (supportsProgramSpecializedEditor(schema)) {
    return (
      <ProgramSpecializedEditor
        schema={schema}
        value={value === undefined ? fallback : value}
        disabled={disabled}
        sourceJointInventory={props.context.sourceJointInventory}
        onCommit={onCommit}
        onInvalidChange={onInvalidChange}
      />
    );
  }
  if (schema.type === 'array') return <ArrayEditor {...stableProps} />;
  if (schema.type === 'object' && typeof schema.additionalProperties === 'object') {
    return <MapEditor {...stableProps} />;
  }
  if (schema.type === 'object' && schema.properties) return <ObjectEditor {...stableProps} />;
  if (schema.type === 'string' || semanticType === 'source_joint') {
    return (
      <DraftTextInput
        value={typeof value === 'string' ? value : ''}
        disabled={disabled}
        validate={(text) => firstCatalogSchemaValidationMessage(schema, text, path)}
        onInvalidChange={onInvalidChange}
        onCommit={onCommit}
      />
    );
  }
  return (
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
    />
  );
}
