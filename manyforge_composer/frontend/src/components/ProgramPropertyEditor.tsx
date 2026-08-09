import { useMemo, useState } from 'react';

import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogParameter,
  JsonValue,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramValueSource,
} from '../types/api';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import {
  catalogFieldAcceptsValueSource,
  flattenProgramTree,
  resolveProgramParameterLiteralValue,
} from '../utils/programAuthoring';
import { DraftTextInput, DraftTextarea } from './common/DraftInputs';

interface ProgramPropertyEditorProps {
  root: ProgramTreeNode;
  selectedId: string | null;
  catalogEntries: CatalogEntry[];
  programParameters: ProgramParameterDeclaration[];
  blackboardKeys: BlackboardKeyDeclaration[];
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
}

interface SourceChoice {
  label: string;
  source: ProgramValueSource;
}

function sourceChoices(
  entry: CatalogEntry,
  fieldName: string,
  parameters: ProgramParameterDeclaration[],
  keys: BlackboardKeyDeclaration[],
): SourceChoice[] {
  const result: SourceChoice[] = [];
  for (const parameter of parameters) {
    if (catalogFieldAcceptsValueSource(entry, fieldName, parameter.type)) {
      result.push({
        label: `Program parameter · ${parameter.parameter_id}`,
        source: { kind: 'program_parameter', reference: parameter.parameter_id },
      });
    }
  }
  for (const key of keys) {
    if (key.access === 'write') continue;
    if (catalogFieldAcceptsValueSource(entry, fieldName, key.type)) {
      result.push({
        label: `Blackboard key · ${key.key}`,
        source: { kind: 'blackboard_key', reference: key.key },
      });
    }
    for (const field of key.fields ?? []) {
      if (catalogFieldAcceptsValueSource(entry, fieldName, field.type)) {
        result.push({
          label: `Blackboard field · ${key.key}.${field.field}`,
          source: {
            kind: 'blackboard_field',
            reference: key.key,
            field: field.field,
          },
        });
      }
    }
  }
  return result;
}

function ScalarLiteralEditor({
  parameter,
  value,
  disabled,
  onCommit,
}: {
  parameter: CatalogParameter;
  value: JsonValue | undefined;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  if (parameter.schema.enum) {
    const encoded = value === undefined ? '' : JSON.stringify(value);
    return (
      <select
        value={encoded}
        disabled={disabled}
        onChange={(event) => onCommit(JSON.parse(event.target.value) as JsonValue)}
      >
        <option value="" disabled>Select a value</option>
        {parameter.schema.enum.map((option) => {
          const optionValue = JSON.stringify(option);
          return <option key={optionValue} value={optionValue}>{String(option)}</option>;
        })}
      </select>
    );
  }
  if (parameter.schema.type === 'boolean') {
    return (
      <select
        value={typeof value === 'boolean' ? String(value) : ''}
        disabled={disabled}
        onChange={(event) => onCommit(event.target.value === 'true')}
      >
        <option value="" disabled>Select true or false</option>
        <option value="true">true</option>
        <option value="false">false</option>
      </select>
    );
  }
  if (parameter.schema.type === 'number' || parameter.schema.type === 'integer') {
    return (
      <DraftTextInput
        type="number"
        step={parameter.schema.type === 'integer' ? 1 : 'any'}
        value={typeof value === 'number' ? value : ''}
        disabled={disabled}
        onCommit={(text) => onCommit(
          text.trim() === '' ? Number.NaN : Number(text),
        )}
      />
    );
  }
  return (
    <DraftTextInput
      value={typeof value === 'string' ? value : ''}
      disabled={disabled}
      onCommit={(text) => onCommit(text)}
    />
  );
}

export function ProgramPropertyEditor({
  root,
  selectedId,
  catalogEntries,
  programParameters,
  blackboardKeys,
  busy,
  onMutate,
}: ProgramPropertyEditorProps) {
  const [errors, setErrors] = useState<Record<string, string>>({});
  const selected = useMemo(
    () => flattenProgramTree(root).find(
      ({ node }) => node.instance_id === selectedId,
    )?.node ?? null,
    [root, selectedId],
  );
  const entry = selected
    ? catalogEntries.find((candidate) => candidate.id === selected.node_id) ?? null
    : null;

  if (!selected || !entry) {
    return (
      <section className="program-properties" aria-labelledby="program-properties-title">
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Exact catalog schema</p>
            <h3 id="program-properties-title">Properties</h3>
          </div>
        </header>
        <div className="panel-state">Select one Program node to edit it.</div>
      </section>
    );
  }

  const replaceFields = (
    parameters: Record<string, JsonValue>,
    sources: Record<string, ProgramValueSource>,
  ) => onMutate('tree_draft_update_node_params', {
    instance_id: selected.instance_id,
    expected_node_id: selected.node_id,
    merge: false,
    parameters,
    value_sources: sources as unknown as JsonValue,
  });

  const commitLiteral = (parameter: CatalogParameter, value: JsonValue) => {
    const message = firstCatalogParameterValidationMessage(parameter, value);
    if (message) {
      setErrors((current) => ({ ...current, [parameter.id]: message }));
      return;
    }
    setErrors((current) => {
      const next = { ...current };
      delete next[parameter.id];
      return next;
    });
    const parameters = { ...selected.parameters, [parameter.id]: value };
    const sources = { ...(selected.value_sources ?? {}) };
    delete sources[parameter.id];
    replaceFields(parameters, sources);
  };

  const removeLiteral = (parameter: CatalogParameter) => {
    const parameters = { ...selected.parameters };
    const sources = { ...(selected.value_sources ?? {}) };
    delete parameters[parameter.id];
    delete sources[parameter.id];
    replaceFields(parameters, sources);
  };

  const selectSource = (parameter: CatalogParameter, encoded: string) => {
    const parameters = { ...selected.parameters };
    const sources = { ...(selected.value_sources ?? {}) };
    if (encoded) {
      sources[parameter.id] = JSON.parse(encoded) as ProgramValueSource;
      delete parameters[parameter.id];
    } else {
      delete sources[parameter.id];
      const fallback = resolveProgramParameterLiteralValue(
        selected.value_sources?.[parameter.id],
        selected.parameters[parameter.id],
        parameter.hasDefault ? parameter.default as JsonValue : undefined,
        programParameters,
      );
      if (fallback !== undefined) parameters[parameter.id] = fallback;
    }
    replaceFields(parameters, sources);
  };

  return (
    <section className="program-properties" aria-labelledby="program-properties-title">
      <header className="program-subpanel-header">
        <div>
          <p className="eyebrow">Exact catalog schema</p>
          <h3 id="program-properties-title">Properties</h3>
        </div>
        <code>{selected.instance_id}</code>
      </header>
      <div className="program-property-list">
        <div className="program-node-identity">
          <span>{entry.name}</span>
          <code>{selected.node_id}</code>
        </div>
        {entry.parameters.length === 0 && (
          <p className="muted">This node has no authored parameters.</p>
        )}
        {entry.parameters.map((parameter) => {
          const source = selected.value_sources?.[parameter.id];
          const choices = sourceChoices(
            entry,
            parameter.id,
            programParameters,
            blackboardKeys,
          );
          const literal = selected.parameters[parameter.id]
            ?? (parameter.hasDefault ? parameter.default as JsonValue : undefined);
          const complex = !['string', 'number', 'integer', 'boolean']
            .includes(parameter.schema.type ?? '');
          const fallback = resolveProgramParameterLiteralValue(
            source,
            selected.parameters[parameter.id],
            parameter.hasDefault ? parameter.default as JsonValue : undefined,
            programParameters,
          );
          return (
            <div className="program-property" key={parameter.id}>
              <div className="program-property-heading">
                <label htmlFor={`property-${selected.instance_id}-${parameter.id}`}>
                  {parameter.id}
                </label>
                <span>{parameter.required ? 'required' : 'optional'}</span>
              </div>
              {choices.length > 0 && (
                <label className="program-source-select">
                  Value source
                  <select
                    value={source ? JSON.stringify(source) : ''}
                    disabled={busy}
                    onChange={(event) => selectSource(parameter, event.target.value)}
                  >
                    <option
                      value=""
                      disabled={
                        source !== undefined
                        && fallback === undefined
                        && parameter.required
                      }
                    >
                      Literal value
                    </option>
                    {choices.map((choice) => {
                      const encoded = JSON.stringify(choice.source);
                      return <option key={encoded} value={encoded}>{choice.label}</option>;
                    })}
                  </select>
                </label>
              )}
              {source ? (
                <div className="program-source-summary">
                  <code>{source.kind}:{source.reference}{source.field ? `.${source.field}` : ''}</code>
                  <span>The literal is absent while this exact source is bound.</span>
                </div>
              ) : complex ? (
                <DraftTextarea
                  id={`property-${selected.instance_id}-${parameter.id}`}
                  rows={5}
                  value={literal === undefined ? '' : JSON.stringify(literal, null, 2)}
                  disabled={busy}
                  spellCheck={false}
                  onCommit={(text) => {
                    try {
                      commitLiteral(parameter, JSON.parse(text) as JsonValue);
                    } catch (error) {
                      setErrors((current) => ({
                        ...current,
                        [parameter.id]: error instanceof Error
                          ? error.message
                          : 'Value must be strict JSON.',
                      }));
                    }
                  }}
                />
              ) : (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <ScalarLiteralEditor
                    parameter={parameter}
                    value={literal}
                    disabled={busy}
                    onCommit={(value) => commitLiteral(parameter, value)}
                  />
                </div>
              )}
              {errors[parameter.id] && (
                <p className="program-form-error" role="alert">{errors[parameter.id]}</p>
              )}
              {!source && (!parameter.required || parameter.hasDefault) && (
                <button
                  type="button"
                  className="text-button"
                  disabled={busy || selected.parameters[parameter.id] === undefined}
                  onClick={() => removeLiteral(parameter)}
                >
                  Use catalog default / omit
                </button>
              )}
            </div>
          );
        })}
      </div>
    </section>
  );
}
