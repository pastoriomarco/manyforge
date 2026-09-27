import { useEffect, useState } from 'react';

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
              onInvalidChange={onInvalidChange}
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
  const [draft, setDraft] = useState<JsonValue[]>(incoming);
  const [error, setError] = useState<string | null>(null);
  const [expanded, setExpanded] = useState(incoming.length <= 4);
  useEffect(() => {
    setDraft(Array.isArray(value) ? value : []);
    setError(null);
  }, [value]);
  useEffect(() => {
    onInvalidChange?.(error);
    return () => onInvalidChange?.(null);
  }, [error, onInvalidChange]);
  const submit = (candidate: JsonValue[]) => {
    setDraft(candidate);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setError(message);
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
        {draft.map((item, index) => (
          <div className="program-schema-list-row" key={index}>
            <ProgramSchemaValueEditor
              schema={itemSchema}
              value={item}
              disabled={disabled}
              context={context}
              path={`${path}[${index}]`}
              onCommit={(nextItem) => {
                const candidate = [...draft];
                candidate[index] = nextItem;
                submit(candidate);
              }}
              onInvalidChange={(message) => setError(
                message ? `${path}[${index}]: ${message}` : firstCatalogSchemaValidationMessage(schema, draft, path),
              )}
            />
            <div className="program-schema-row-actions">
              <button type="button" disabled={disabled || index === 0} onClick={() => {
                const candidate = [...draft];
                [candidate[index - 1], candidate[index]] = [candidate[index], candidate[index - 1]];
                submit(candidate);
              }}>Earlier</button>
              <button type="button" disabled={disabled || index === draft.length - 1} onClick={() => {
                const candidate = [...draft];
                [candidate[index], candidate[index + 1]] = [candidate[index + 1], candidate[index]];
                submit(candidate);
              }}>Later</button>
              <button type="button" className="text-button" disabled={disabled} onClick={() => {
                submit(draft.filter((_row, rowIndex) => rowIndex !== index));
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
            const candidate = [...draft, defaultValue(itemSchema)];
            if (itemSchema['x-manyforge-semantic-type']) {
              setDraft(candidate);
              setError(`${path}[${candidate.length - 1}]: select a declared ${itemSchema['x-manyforge-semantic-type'].replaceAll('_', ' ')}.`);
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
  const incoming = objectDraftValue(schema, value);
  const [draft, setDraft] = useState<Record<string, JsonValue>>(incoming);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    setDraft(objectDraftValue(schema, value));
    setError(null);
  }, [schema, value]);
  useEffect(() => {
    onInvalidChange?.(error);
    return () => onInvalidChange?.(null);
  }, [error, onInvalidChange]);
  const submit = (candidate: Record<string, JsonValue>) => {
    setDraft(candidate);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setError(message);
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
                  submit(candidate);
                }}>Remove</button>
              )}
            </div>
            <ProgramSchemaValueEditor
              schema={childSchema}
              value={present ? draft[name] : defaultValue(childSchema)}
              disabled={disabled}
              context={context}
              path={`${path}.${name}`}
              onCommit={(child) => submit({ ...draft, [name]: child })}
              onInvalidChange={(message) => setError(
                message ? `${path}.${name}: ${message}` : firstCatalogSchemaValidationMessage(schema, draft, path),
              )}
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
  const [draft, setDraft] = useState<Record<string, JsonValue>>(incoming);
  const [newKey, setNewKey] = useState('');
  const [error, setError] = useState<string | null>(null);
  const itemSchema = typeof schema.additionalProperties === 'object'
    ? schema.additionalProperties
    : {};
  useEffect(() => {
    setDraft(plainObject(value));
    setNewKey('');
    setError(null);
  }, [value]);
  useEffect(() => {
    onInvalidChange?.(error);
    return () => onInvalidChange?.(null);
  }, [error, onInvalidChange]);
  const submit = (candidate: Record<string, JsonValue>) => {
    setDraft(candidate);
    const message = firstCatalogSchemaValidationMessage(schema, candidate, path);
    setError(message);
    if (message === null) onCommit(candidate);
  };
  return (
    <fieldset className={`program-schema-map${error ? ' invalid' : ''}`}>
      <legend>{path.split('.').at(-1)?.replaceAll('_', ' ')}</legend>
      {Object.entries(draft).map(([key, item]) => (
        <div className="program-schema-map-row" key={key}>
          <DraftTextInput
            aria-label={`${path} key ${key}`}
            value={key}
            disabled={disabled}
            validate={(replacement) => {
              const normalized = replacement.trim();
              if (!normalized) return 'Map key is required.';
              return normalized !== key && Object.prototype.hasOwnProperty.call(draft, normalized)
                ? `Map key '${normalized}' already exists.`
                : null;
            }}
            onInvalidChange={setError}
            onCommit={(replacement) => {
              const normalized = replacement.trim();
              if (!normalized || normalized === key) return;
              const candidate = { ...draft };
              delete candidate[key];
              candidate[normalized] = item;
              submit(candidate);
            }}
          />
          <ProgramSchemaValueEditor
            schema={itemSchema}
            value={item}
            disabled={disabled}
            context={context}
            path={`${path}.${key}`}
            onCommit={(nextItem) => submit({ ...draft, [key]: nextItem })}
            onInvalidChange={(message) => setError(
              message ? `${path}.${key}: ${message}` : firstCatalogSchemaValidationMessage(schema, draft, path),
            )}
          />
          <button
            type="button"
            className="text-button"
            disabled={disabled}
            onClick={() => {
              const candidate = { ...draft };
              delete candidate[key];
              submit(candidate);
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
              setError(`Map key '${normalized}' already exists.`);
              return;
            }
            setNewKey('');
            submit({ ...draft, [normalized]: defaultValue(itemSchema) });
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
  const { schema, value, disabled, path = 'value', onCommit, onInvalidChange } = props;
  const semanticType = schema['x-manyforge-semantic-type'];
  if (semanticType && semanticType !== 'source_joint') {
    return <SemanticChoiceEditor {...props} />;
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
    return <FixedTupleEditor {...props} />;
  }
  if (supportsProgramSpecializedEditor(schema)) {
    return (
      <ProgramSpecializedEditor
        schema={schema}
        value={value ?? defaultValue(schema)}
        disabled={disabled}
        sourceJointInventory={props.context.sourceJointInventory}
        onCommit={onCommit}
        onInvalidChange={onInvalidChange}
      />
    );
  }
  if (schema.type === 'array') return <ArrayEditor {...props} />;
  if (schema.type === 'object' && typeof schema.additionalProperties === 'object') {
    return <MapEditor {...props} />;
  }
  if (schema.type === 'object' && schema.properties) return <ObjectEditor {...props} />;
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
