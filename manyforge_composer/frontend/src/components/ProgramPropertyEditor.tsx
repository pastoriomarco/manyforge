import { useEffect, useMemo, useState } from 'react';

import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogParameter,
  JsonValue,
  ProgramAuthoringDiagnostic,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramValueSource,
} from '../types/api';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import {
  catalogFieldAcceptsValueSource,
  flattenProgramForest,
  programDiagnosticNodeId,
  resolveProgramParameterLiteralValue,
} from '../utils/programAuthoring';
import { DraftTextInput, DraftTextarea } from './common/DraftInputs';
import {
  ProgramSpecializedEditor,
  supportsProgramSpecializedEditor,
} from './property/ProgramSpecializedEditors';
import { ProgramPolicyEditor } from './ProgramPolicyEditor';

interface ProgramPropertyEditorProps {
  embedded?: boolean;
  roots: ProgramTreeNode[];
  selectedId: string | null;
  catalogEntries: CatalogEntry[];
  programParameters: ProgramParameterDeclaration[];
  blackboardKeys: BlackboardKeyDeclaration[];
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  onUpdateNodePolicies: (
    instanceId: string,
    modificationPolicy: ProgramNodeModificationPolicy,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
  ) => void;
  onInvalidEditChange?: (invalid: boolean) => void;
  diagnostics?: ProgramAuthoringDiagnostic[];
}

interface SourceChoice {
  label: string;
  source: ProgramValueSource;
}

export type ProgramParameterTransition =
  | { kind: 'literal'; value: JsonValue }
  | { kind: 'source'; source: ProgramValueSource }
  | { kind: 'clear' };

export interface ProgramParameterMutation {
  mutation: 'tree_draft_update_node_params';
  args: Record<string, JsonValue>;
}

/**
 * Build the one full-replacement Draft mutation used by every property
 * interaction. The selected field changes representation atomically while
 * every unrelated literal and source is retained.
 */
export function buildProgramParameterMutation(
  node: ProgramTreeNode,
  parameterId: string,
  transition: ProgramParameterTransition,
): ProgramParameterMutation {
  const parameters = { ...node.parameters };
  const sources = { ...(node.value_sources ?? {}) };
  if (transition.kind === 'literal') {
    parameters[parameterId] = transition.value;
    delete sources[parameterId];
  } else if (transition.kind === 'source') {
    sources[parameterId] = transition.source;
    delete parameters[parameterId];
  } else {
    delete parameters[parameterId];
    delete sources[parameterId];
  }
  return {
    mutation: 'tree_draft_update_node_params',
    args: {
      instance_id: node.instance_id,
      expected_node_id: node.node_id,
      merge: false,
      parameters,
      value_sources: sources as unknown as JsonValue,
    },
  };
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
  onInvalidChange,
}: {
  parameter: CatalogParameter;
  value: JsonValue | undefined;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
  onInvalidChange: (message: string | null) => void;
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
        validate={(text) => firstCatalogParameterValidationMessage(
          parameter,
          text.trim() === '' ? Number.NaN : Number(text),
        )}
        onInvalidChange={onInvalidChange}
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
      validate={(text) => firstCatalogParameterValidationMessage(parameter, text)}
      onInvalidChange={onInvalidChange}
      onCommit={(text) => onCommit(text)}
    />
  );
}

export function ProgramPropertyEditor({
  embedded = false,
  roots,
  selectedId,
  catalogEntries,
  programParameters,
  blackboardKeys,
  busy,
  onMutate,
  onUpdateNodePolicies,
  onInvalidEditChange,
  diagnostics = [],
}: ProgramPropertyEditorProps) {
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [discardEpoch, setDiscardEpoch] = useState(0);
  const selected = useMemo(
    () => flattenProgramForest(roots).find(
      ({ node }) => node.instance_id === selectedId,
    )?.node ?? null,
    [roots, selectedId],
  );
  const entry = selected
    ? catalogEntries.find((candidate) => candidate.id === selected.node_id) ?? null
    : null;
  const selectedDiagnostics = useMemo(() => diagnostics.filter((diagnostic) =>
    programDiagnosticNodeId(roots, diagnostic.path) === selectedId,
  ), [diagnostics, roots, selectedId]);
  useEffect(() => {
    setErrors({});
    setDiscardEpoch((current) => current + 1);
  }, [selectedId]);
  useEffect(() => {
    onInvalidEditChange?.(Object.keys(errors).length > 0);
    return () => onInvalidEditChange?.(false);
  }, [errors, onInvalidEditChange]);

  if (!selected || !entry) {
    return (
      <section
        className="program-properties"
        aria-label={embedded ? 'Node properties' : undefined}
        aria-labelledby={embedded ? undefined : 'program-properties-title'}
      >
        {!embedded && (
          <header className="program-subpanel-header">
            <div>
              <p className="eyebrow">Exact catalog schema</p>
              <h3 id="program-properties-title">Properties</h3>
            </div>
          </header>
        )}
        <div className="panel-state">Select one Program node to edit it.</div>
      </section>
    );
  }

  const applyTransition = (
    parameterId: string,
    transition: ProgramParameterTransition,
  ) => {
    const request = buildProgramParameterMutation(selected, parameterId, transition);
    onMutate(request.mutation, request.args);
  };

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
    applyTransition(parameter.id, { kind: 'literal', value });
  };

  const removeLiteral = (parameter: CatalogParameter) => {
    applyTransition(parameter.id, { kind: 'clear' });
  };

  const selectSource = (parameter: CatalogParameter, encoded: string) => {
    if (encoded) {
      applyTransition(parameter.id, {
        kind: 'source',
        source: JSON.parse(encoded) as ProgramValueSource,
      });
    } else {
      const fallback = resolveProgramParameterLiteralValue(
        selected.value_sources?.[parameter.id],
        selected.parameters[parameter.id],
        parameter.hasDefault ? parameter.default as JsonValue : undefined,
        programParameters,
      );
      const fallbackMessage = fallback === undefined
        ? parameter.required ? `${parameter.id} has no valid literal fallback` : null
        : firstCatalogParameterValidationMessage(parameter, fallback);
      if (fallbackMessage) {
        setErrors((current) => ({ ...current, [parameter.id]: fallbackMessage }));
        return;
      }
      applyTransition(
        parameter.id,
        fallback === undefined
          ? { kind: 'clear' }
          : { kind: 'literal', value: fallback },
      );
    }
    setErrors((current) => {
      const next = { ...current };
      delete next[parameter.id];
      return next;
    });
  };

  return (
    <section
      className="program-properties"
      aria-label={embedded ? `Properties for ${selected.instance_id}` : undefined}
      aria-labelledby={embedded ? undefined : 'program-properties-title'}
    >
      {!embedded && (
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Exact catalog schema</p>
            <h3 id="program-properties-title">Properties</h3>
          </div>
          <code>{selected.instance_id}</code>
        </header>
      )}
      {Object.keys(errors).length > 0 && (
        <div className="program-invalid-edit-strip" role="alert">
          <span>One field has an invalid local edit.</span>
          <button
            type="button"
            onClick={() => {
              const first = Object.keys(errors)[0];
              const field = first === '_policy'
                ? document.querySelector('.program-policy-editor [aria-invalid="true"]')
                : document.getElementById(`property-${selected.instance_id}-${first}`);
              const control = (field?.matches('input, textarea, select')
                ? field
                : field?.querySelector('input, textarea, select')) as HTMLElement | null;
              control?.focus();
            }}
          >
            Review
          </button>
          <button
            type="button"
            onClick={() => {
              setErrors({});
              setDiscardEpoch((current) => current + 1);
            }}
          >
            Discard edit
          </button>
        </div>
      )}
      {selectedDiagnostics.length > 0 && (
        <div className="program-authoring-diagnostics" role="status">
          <strong>Complete this node</strong>
          {selectedDiagnostics.slice(0, 6).map((diagnostic) => (
            <span key={`${diagnostic.code}:${diagnostic.path}`}>{diagnostic.message}</span>
          ))}
          {selectedDiagnostics.length > 6 && (
            <span>{selectedDiagnostics.length - 6} more issue(s)</span>
          )}
        </div>
      )}
      <div className="program-property-list">
        <div className="program-node-identity">
          <span><strong>{entry.name}</strong><small>{entry.kind} · {entry.category}</small></span>
          <span><code>{selected.node_id}</code><small>Profile {entry.minProgramProfile ?? 'portable'}</small></span>
        </div>
        {entry.requiredCapabilities.length > 0 && (
          <details className="program-node-capabilities">
            <summary>{entry.requiredCapabilities.length} required capability binding(s)</summary>
            {entry.requiredCapabilities.map((requirement) => (
              <p key={`${requirement.capabilityId}:${requirement.scope}`}>
                <code>{requirement.capabilityId}</code> · {requirement.scope}
              </p>
            ))}
          </details>
        )}
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
          const hasLiteral = Object.prototype.hasOwnProperty.call(
            selected.parameters,
            parameter.id,
          );
          const literal = hasLiteral
            ? selected.parameters[parameter.id]
            : parameter.hasDefault ? parameter.default as JsonValue : undefined;
          const complex = !['string', 'number', 'integer', 'boolean']
            .includes(parameter.schema.type ?? '');
          const fallback = resolveProgramParameterLiteralValue(
            source,
            selected.parameters[parameter.id],
            parameter.hasDefault ? parameter.default as JsonValue : undefined,
            programParameters,
          );
          const fallbackValid = fallback !== undefined
            && firstCatalogParameterValidationMessage(parameter, fallback) === null;
          const canSelectLiteral = !parameter.required || fallbackValid;
          const literalValidation = hasLiteral
            ? firstCatalogParameterValidationMessage(parameter, literal)
            : null;
          const repairMode = literalValidation !== null;
          const specialized = literal !== undefined
            && !repairMode
            && supportsProgramSpecializedEditor(parameter.schema);
          const currentSourceIsOffCatalog = source !== undefined
            && !choices.some((choice) =>
              JSON.stringify(choice.source) === JSON.stringify(source));
          const backendDiagnostic = selectedDiagnostics.find((diagnostic) =>
            diagnostic.path.endsWith(`.parameters.${parameter.id}`)
            || diagnostic.path.endsWith(`.value_sources.${parameter.id}`));
          return (
            <div
              className={`program-property${backendDiagnostic ? ' backend-invalid' : ''}`}
              title={backendDiagnostic?.message}
              key={`${parameter.id}:${discardEpoch}`}
            >
              <div className="program-property-heading">
                <label htmlFor={`property-${selected.instance_id}-${parameter.id}`}>
                  {parameter.id}
                </label>
                <span>{parameter.required ? 'required' : 'optional'}</span>
              </div>
              {(choices.length > 0 || source !== undefined) && (
                <label className="program-source-select">
                  Value source
                  <select
                    value={source ? JSON.stringify(source) : ''}
                    disabled={busy}
                    onChange={(event) => selectSource(parameter, event.target.value)}
                  >
                    <option
                      value=""
                      disabled={source !== undefined && !canSelectLiteral}
                    >
                      Literal value
                    </option>
                    {currentSourceIsOffCatalog && (
                      <option value={JSON.stringify(source)}>
                        Current binding (not compatible with this catalog)
                      </option>
                    )}
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
                  {!canSelectLiteral && (
                    <span>A required parameter cannot become literal until a schema-valid fallback exists.</span>
                  )}
                </div>
              ) : repairMode ? (
                <>
                  <p className="program-form-error" role="alert">
                    Catalog-invalid value: {literalValidation}. Repair it as strict JSON; specialized controls are disabled to preserve every represented field.
                  </p>
                  <DraftTextarea
                    id={`property-${selected.instance_id}-${parameter.id}`}
                    rows={7}
                    value={JSON.stringify(literal, null, 2)}
                    disabled={busy}
                    spellCheck={false}
                    aria-label={`${parameter.id} strict JSON repair`}
                    validate={(text) => {
                      try {
                        return firstCatalogParameterValidationMessage(
                          parameter,
                          JSON.parse(text) as JsonValue,
                        );
                      } catch (error) {
                        return error instanceof Error ? error.message : 'Value must be strict JSON.';
                      }
                    }}
                    onInvalidChange={(message) => setErrors((current) => {
                      const next = { ...current };
                      if (message) next[parameter.id] = message;
                      else delete next[parameter.id];
                      return next;
                    })}
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
                </>
              ) : specialized ? (
                <ProgramSpecializedEditor
                  schema={parameter.schema}
                  value={literal}
                  disabled={busy}
                  onCommit={(value) => commitLiteral(parameter, value)}
                  onInvalidChange={(message) => setErrors((current) => {
                    const next = { ...current };
                    if (message) next[parameter.id] = message;
                    else delete next[parameter.id];
                    return next;
                  })}
                />
              ) : complex ? (
                <DraftTextarea
                  id={`property-${selected.instance_id}-${parameter.id}`}
                  rows={5}
                  value={literal === undefined ? '' : JSON.stringify(literal, null, 2)}
                  disabled={busy}
                  spellCheck={false}
                  validate={(text) => {
                    try {
                      return firstCatalogParameterValidationMessage(
                        parameter,
                        JSON.parse(text) as JsonValue,
                      );
                    } catch (error) {
                      return error instanceof Error ? error.message : 'Value must be strict JSON.';
                    }
                  }}
                  onInvalidChange={(message) => setErrors((current) => {
                    const next = { ...current };
                    if (message) next[parameter.id] = message;
                    else delete next[parameter.id];
                    return next;
                  })}
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
                    onInvalidChange={(message) => setErrors((current) => {
                      const next = { ...current };
                      if (message) next[parameter.id] = message;
                      else delete next[parameter.id];
                      return next;
                    })}
                  />
                </div>
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
      <ProgramPolicyEditor
        key={`${selected.instance_id}:${discardEpoch}`}
        node={selected}
        busy={busy}
        onUpdate={onUpdateNodePolicies}
        onInvalidChange={(message) => setErrors((current) => {
          const next = { ...current };
          if (message) next._policy = message;
          else delete next._policy;
          return next;
        })}
      />
    </section>
  );
}
