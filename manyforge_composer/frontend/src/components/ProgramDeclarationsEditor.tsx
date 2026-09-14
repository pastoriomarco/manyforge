import { useCallback, useEffect, useMemo, useState, type KeyboardEvent } from 'react';

import type {
  BlackboardFieldDeclaration,
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  CatalogEntry,
  DraftMutationResponse,
  JsonValue,
  ProgramAuthoringDiagnostic,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  collectProgramBindingCandidates,
  collectBlackboardUsages,
  collectProgramParameterUsages,
  flattenProgramForest,
  resolveProgramParameterLiteralValue,
} from '../utils/programAuthoring';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import type { ProgramDeclarationSelection } from './ProgramDeclarationsNavigator';
import { buildProgramParameterMutation } from './ProgramPropertyEditor';

const PROGRAM_TYPES = [
  'bool',
  'cartesian_path',
  'coordinate_position',
  'float64',
  'int64',
  'model_response',
  'pick_planning_result',
  'scene_resource',
  'scene_selection_context',
  'scene_selection_result',
  'stored_pose',
  'string',
  'string_list',
];

interface ProgramDeclarationsEditorProps {
  embedded?: boolean;
  selection?: ProgramDeclarationSelection | null;
  showNavigation?: boolean;
  roots: ProgramTreeNode[];
  parameters: ProgramParameterDeclaration[];
  namespaces: BlackboardNamespaceDeclaration[];
  keys: BlackboardKeyDeclaration[];
  catalogEntries?: CatalogEntry[];
  busy: boolean;
  creationExpectedType?: string | null;
  creationExpectedAccess?: 'read' | 'write' | 'read_write' | null;
  onMutate: (
    mutation: string,
    args: Record<string, JsonValue>,
  ) => Promise<DraftMutationResponse | null> | DraftMutationResponse | null | void;
  onSelectNode?: (instanceId: string) => void;
  onCreated?: (selection: ProgramDeclarationSelection) => void;
  onInvalidEditChange?: (invalid: boolean) => void;
  diagnostics?: ProgramAuthoringDiagnostic[];
}

function parseJson(text: string, label: string): JsonValue {
  try {
    return JSON.parse(text) as JsonValue;
  } catch (error) {
    throw new Error(
      `${label}: ${error instanceof Error ? error.message : 'invalid JSON'}`,
    );
  }
}

type ScalarParameterType = 'bool' | 'float64' | 'int64' | 'string';

function isScalarParameterType(value: string): value is ScalarParameterType {
  return value === 'bool' || value === 'float64' || value === 'int64' || value === 'string';
}

function defaultForParameterType(value: string): JsonValue {
  if (value === 'bool') return false;
  if (value === 'float64' || value === 'int64') return 0;
  if (value === 'string') return '';
  if (value === 'string_list') return [];
  return null;
}

function constraintObject(text: string): Record<string, JsonValue> {
  try {
    const value = JSON.parse(text) as JsonValue;
    return value !== null && typeof value === 'object' && !Array.isArray(value)
      ? value as Record<string, JsonValue>
      : {};
  } catch {
    return {};
  }
}

export function ProgramDeclarationsEditor({
  embedded = false,
  selection = null,
  showNavigation = true,
  roots,
  parameters,
  namespaces,
  keys,
  catalogEntries = [],
  creationExpectedType = null,
  creationExpectedAccess = null,
  busy,
  onMutate,
  onSelectNode,
  onCreated,
  onInvalidEditChange,
  diagnostics = [],
}: ProgramDeclarationsEditorProps) {
  const parameterUsages = useMemo(() => collectProgramParameterUsages(roots), [roots]);
  const blackboardUsages = useMemo(() => collectBlackboardUsages(roots), [roots]);
  const [parameterId, setParameterId] = useState('');
  const [parameterType, setParameterType] = useState('float64');
  const [defaultText, setDefaultText] = useState('0');
  const [constraintsText, setConstraintsText] = useState('{}');
  const [mutability, setMutability] = useState<'immutable' | 'before_run' | 'between_operations'>('before_run');
  const [parameterDescription, setParameterDescription] = useState('');
  const [namespaceId, setNamespaceId] = useState('');
  const [namespaceDescription, setNamespaceDescription] = useState('');
  const [keyName, setKeyName] = useState('');
  const [keyType, setKeyType] = useState('bool');
  const [keyAccess, setKeyAccess] = useState<'read' | 'write' | 'read_write'>('read_write');
  const [keyPersistence, setKeyPersistence] = useState<'node' | 'run' | 'persistent'>('run');
  const [fields, setFields] = useState<BlackboardFieldDeclaration[]>([]);
  const [keyDescription, setKeyDescription] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [errorTarget, setErrorTarget] = useState<string | null>(null);
  const clearError = useCallback(() => {
    setError(null);
    setErrorTarget(null);
  }, []);
  const reportError = (message: string, target: string | null = null) => {
    setError(message);
    setErrorTarget(target);
  };
  const flat = useMemo(() => flattenProgramForest(roots), [roots]);
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
  const selectedParameter = selection?.kind === 'parameter'
    ? parameters.find((row) => row.parameter_id === selection.id) ?? null
    : null;
  const selectedNamespace = selection?.kind === 'namespace'
    ? namespaces.find((row) => row.namespace_id === selection.id) ?? null
    : null;
  const selectedKey = selection?.kind === 'key'
    ? keys.find((row) => row.key === selection.id) ?? null
    : null;
  const parameterConstraints = useMemo(
    () => constraintObject(constraintsText),
    [constraintsText],
  );
  const selectedDiagnosticPrefix = selection?.kind === 'parameter'
    ? `program.parameters[${parameters.findIndex((row) => row.parameter_id === selection.id)}]`
    : selection?.kind === 'namespace'
      ? `program.blackboard.namespaces[${namespaces.findIndex((row) => row.namespace_id === selection.id)}]`
      : selection?.kind === 'key'
        ? `program.blackboard.keys[${keys.findIndex((row) => row.key === selection.id)}]`
        : null;
  const selectedDiagnostics = selectedDiagnosticPrefix === null
    ? []
    : diagnostics.filter((diagnostic) => diagnostic.path.startsWith(selectedDiagnosticPrefix));
  const scalarDefault = useMemo(() => {
    try {
      return JSON.parse(defaultText) as JsonValue;
    } catch {
      return defaultForParameterType(parameterType);
    }
  }, [defaultText, parameterType]);
  const minimumConstraint = 'min_inclusive' in parameterConstraints
    ? 'min_inclusive'
    : 'min_exclusive' in parameterConstraints
      ? 'min_exclusive'
      : '';
  const maximumConstraint = 'max_inclusive' in parameterConstraints
    ? 'max_inclusive'
    : 'max_exclusive' in parameterConstraints
      ? 'max_exclusive'
      : '';
  const parameterDirty = selectedParameter !== null && (
    parameterId !== selectedParameter.parameter_id
    || parameterType !== selectedParameter.type
    || defaultText !== JSON.stringify(selectedParameter.default, null, 2)
    || constraintsText !== JSON.stringify(selectedParameter.constraints ?? {}, null, 2)
    || mutability !== selectedParameter.runtime_mutability
    || parameterDescription !== (selectedParameter.description ?? '')
  );
  const namespaceDirty = selectedNamespace !== null && (
    namespaceId !== selectedNamespace.namespace_id
    || namespaceDescription !== (selectedNamespace.description ?? '')
  );
  const keyDirty = selectedKey !== null && (
    keyName !== selectedKey.key
    || keyType !== selectedKey.type
    || keyAccess !== selectedKey.access
    || keyPersistence !== selectedKey.persistence
    || JSON.stringify(fields) !== JSON.stringify(selectedKey.fields ?? [])
    || keyDescription !== (selectedKey.description ?? '')
  );
  const parameterIssue = (): { message: string; target: string } | null => {
    try {
      const defaultValue = parseJson(defaultText, 'Default');
      const constraints = parseJson(constraintsText, 'Constraints');
      if (constraints === null || typeof constraints !== 'object' || Array.isArray(constraints)) {
        return { message: 'Constraints must be one JSON object.', target: 'program-parameter-constraints' };
      }
      if (
        (parameterType === 'bool' && typeof defaultValue !== 'boolean')
        || (parameterType === 'float64' && (
          typeof defaultValue !== 'number' || !Number.isFinite(defaultValue)
        ))
        || (parameterType === 'int64' && (
          typeof defaultValue !== 'number' || !Number.isSafeInteger(defaultValue)
        ))
        || (parameterType === 'string' && typeof defaultValue !== 'string')
        || (parameterType === 'string_list' && (
          !Array.isArray(defaultValue) || defaultValue.some((item) => typeof item !== 'string')
        ))
      ) {
        return {
          message: `Default must match ${parameterType}.`,
          target: 'program-parameter-default',
        };
      }
      if (!parameterId.trim()) {
        return { message: 'Parameter ID is required.', target: 'program-parameter-id' };
      }
      return null;
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Parameter is invalid.';
      return {
        message,
        target: message.startsWith('Default')
          ? 'program-parameter-default'
          : 'program-parameter-constraints',
      };
    }
  };
  const namespaceIssue = () => namespaceId.trim()
    ? null
    : { message: 'Namespace ID is required.', target: 'program-namespace-id' };
  const keyIssue = (): { message: string; target: string } | null => {
    if (fields.some((field) => !field.field.trim() || !field.type.trim())) {
      return {
        message: 'Every blackboard field needs a field ID and type.',
        target: 'program-key-fields',
      };
    }
    if (new Set(fields.map((field) => field.field.trim())).size !== fields.length) {
      return { message: 'Blackboard field IDs must be unique.', target: 'program-key-fields' };
    }
    if (!keyName.trim()) {
      return { message: 'Blackboard key is required.', target: 'program-key-name' };
    }
    return null;
  };
  const guardCommitKey = (
    event: KeyboardEvent<HTMLDivElement>,
    issue: { message: string; target: string } | null,
  ) => {
    const target = event.target as HTMLElement;
    const enterCommit = event.key === 'Enter' && (
      target.tagName === 'INPUT'
      || ((event.ctrlKey || event.metaKey) && target.tagName === 'TEXTAREA')
    );
    if (event.key !== 'Tab' && !enterCommit) return;
    if (issue) {
      event.preventDefault();
      reportError(issue.message, issue.target);
      return;
    }
    if (enterCommit) {
      event.preventDefault();
      target.blur();
    }
  };
  const bindingCandidates = useMemo(
    () => selectedParameter
      ? collectProgramBindingCandidates(roots, catalogEntries, selectedParameter)
      : [],
    [catalogEntries, roots, selectedParameter],
  );

  const updateConstraint = (name: string, value: JsonValue | undefined) => {
    const next = { ...parameterConstraints };
    if (value === undefined) delete next[name];
    else next[name] = value;
    setConstraintsText(JSON.stringify(next, null, 2));
    clearError();
  };

  const updateNumericConstraint = (name: string, text: string) => {
    if (text.trim() === '') {
      updateConstraint(name, undefined);
      return;
    }
    const parsed = parameterType === 'int64' ? Number.parseInt(text, 10) : Number(text);
    if (!Number.isFinite(parsed)) return;
    updateConstraint(name, parsed);
  };

  const changeParameterType = (nextType: string) => {
    setParameterType(nextType);
    setDefaultText(JSON.stringify(defaultForParameterType(nextType), null, 2));
    setConstraintsText('{}');
    clearError();
  };

  const editParameter = useCallback((parameter: ProgramParameterDeclaration) => {
    setParameterId(parameter.parameter_id);
    setParameterType(parameter.type);
    setDefaultText(JSON.stringify(parameter.default, null, 2));
    setConstraintsText(JSON.stringify(parameter.constraints ?? {}, null, 2));
    setMutability(parameter.runtime_mutability);
    setParameterDescription(parameter.description ?? '');
    clearError();
  }, [clearError]);

  const saveParameter = (clearAfterSave = true) => {
    if (!clearAfterSave && selectedParameter && !parameterDirty) return;
    try {
      const defaultValue = parseJson(defaultText, 'Default');
      const constraints = parseJson(constraintsText, 'Constraints');
      if (constraints === null || typeof constraints !== 'object' || Array.isArray(constraints)) {
        throw new Error('Constraints must be one JSON object.');
      }
      const declaration: ProgramParameterDeclaration = {
        parameter_id: parameterId.trim(),
        type: parameterType,
        default: defaultValue,
        constraints: constraints as Record<string, JsonValue>,
        runtime_mutability: mutability,
        ...(parameterDescription.trim()
          ? { description: parameterDescription.trim() }
          : {}),
      };
      if (!declaration.parameter_id) throw new Error('Parameter ID is required.');
      void Promise.resolve(onMutate('program_draft_upsert_parameters', {
        parameters: [declaration] as unknown as JsonValue,
      })).then((response) => {
        if (response && clearAfterSave) {
          onCreated?.({ kind: 'parameter', id: declaration.parameter_id });
          setParameterId('');
        }
      });
      clearError();
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Parameter is invalid.';
      reportError(
        message,
        message.startsWith('Default')
          ? 'program-parameter-default'
          : message.startsWith('Constraints')
            ? 'program-parameter-constraints'
            : 'program-parameter-id',
      );
    }
  };

  const saveNamespace = (clearAfterSave = true) => {
    if (!clearAfterSave && selectedNamespace && !namespaceDirty) return;
    const id = namespaceId.trim();
    if (!id) {
      reportError('Namespace ID is required.', 'program-namespace-id');
      return;
    }
    const declaration: BlackboardNamespaceDeclaration = {
      namespace_id: id,
      owner: 'program',
      ...(namespaceDescription.trim()
        ? { description: namespaceDescription.trim() }
        : {}),
    };
    onMutate('blackboard_draft_upsert_namespaces', {
      namespaces: [declaration] as unknown as JsonValue,
    });
    if (clearAfterSave) setNamespaceId('');
    clearError();
  };

  const editKey = useCallback((key: BlackboardKeyDeclaration) => {
    setKeyName(key.key);
    setKeyType(key.type);
    setKeyAccess(key.access);
    setKeyPersistence(key.persistence);
    setFields(key.fields ?? []);
    setKeyDescription(key.description ?? '');
    clearError();
  }, [clearError]);

  useEffect(() => {
    if (selection?.kind === 'parameter') {
      const parameter = parameters.find((row) => row.parameter_id === selection.id);
      if (parameter) editParameter(parameter);
      else {
        setParameterId('');
        setParameterType(creationExpectedType ?? 'float64');
        setDefaultText(creationExpectedType === 'bool' ? 'false' : creationExpectedType === 'string' ? '""' : '0');
        setConstraintsText('{}');
        setMutability('before_run');
        setParameterDescription('');
        clearError();
      }
    } else if (selection?.kind === 'namespace') {
      const namespace = namespaces.find((row) => row.namespace_id === selection.id);
      if (namespace) {
        setNamespaceId(namespace.namespace_id);
        setNamespaceDescription(namespace.description ?? '');
        clearError();
      } else {
        setNamespaceId('');
        setNamespaceDescription('');
        clearError();
      }
    } else if (selection?.kind === 'key') {
      const key = keys.find((row) => row.key === selection.id);
      if (key) editKey(key);
      else {
        setKeyName('');
        setKeyType(creationExpectedType ?? 'bool');
        setKeyAccess(creationExpectedAccess ?? 'read_write');
        setKeyPersistence('run');
        setFields([]);
        setKeyDescription('');
        clearError();
      }
    }
  }, [
    clearError,
    creationExpectedAccess,
    creationExpectedType,
    editKey,
    editParameter,
    keys,
    namespaces,
    parameters,
    selection,
  ]);

  const saveKey = (clearAfterSave = true) => {
    if (!clearAfterSave && selectedKey && !keyDirty) return;
    try {
      const issue = keyIssue();
      if (issue) throw new Error(issue.message);
      const declaration: BlackboardKeyDeclaration = {
        key: keyName.trim(),
        type: keyType,
        access: keyAccess,
        persistence: keyPersistence,
        ...(fields.length > 0 ? { fields } : {}),
        ...(keyDescription.trim() ? { description: keyDescription.trim() } : {}),
      };
      if (!declaration.key) throw new Error('Blackboard key is required.');
      void Promise.resolve(onMutate('blackboard_draft_upsert_keys', {
        keys: [declaration] as unknown as JsonValue,
      })).then((response) => {
        if (response && clearAfterSave) {
          onCreated?.({ kind: 'key', id: declaration.key });
          setKeyName('');
        }
      });
      clearError();
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Blackboard key is invalid.';
      reportError(
        message,
        message.toLowerCase().includes('field')
          ? 'program-key-fields'
          : 'program-key-name',
      );
    }
  };

  useEffect(() => {
    onInvalidEditChange?.(error !== null);
    return () => onInvalidEditChange?.(false);
  }, [error, onInvalidEditChange]);

  const bindParameter = (nodeId: string, fieldName: string) => {
    if (!selectedParameter) return;
    const node = flat.find((row) => row.node.instance_id === nodeId)?.node;
    if (!node) return;
    const request = buildProgramParameterMutation(node, fieldName, {
      kind: 'source',
      source: {
        kind: 'program_parameter',
        reference: selectedParameter.parameter_id,
      },
    });
    onMutate(request.mutation, request.args);
    onSelectNode?.(nodeId);
  };

  const unbindUsage = (nodeId: string, fieldName: string) => {
    const node = flat.find((row) => row.node.instance_id === nodeId)?.node;
    const entry = node ? catalog.get(node.node_id) : undefined;
    const field = entry?.parameters.find((parameter) => parameter.id === fieldName);
    if (!node || !field) {
      reportError('That declaration usage is not present in the current catalog revision.');
      return;
    }
    const fallback = resolveProgramParameterLiteralValue(
      node.value_sources?.[fieldName],
      node.parameters[fieldName],
      field.hasDefault ? field.default as JsonValue : undefined,
      parameters,
    );
    const validation = fallback === undefined
      ? field.required ? `${fieldName}: a required field has no valid literal fallback.` : null
      : firstCatalogParameterValidationMessage(field, fallback);
    if (validation) {
      reportError(validation);
      return;
    }
    const request = buildProgramParameterMutation(
      node,
      fieldName,
      fallback === undefined ? { kind: 'clear' } : { kind: 'literal', value: fallback },
    );
    onMutate(request.mutation, request.args);
    onSelectNode?.(nodeId);
    clearError();
  };

  const discardInvalidEdit = () => {
    if (selectedParameter) editParameter(selectedParameter);
    else if (selectedNamespace) {
      setNamespaceId(selectedNamespace.namespace_id);
      setNamespaceDescription(selectedNamespace.description ?? '');
      clearError();
    } else if (selectedKey) editKey(selectedKey);
    else {
      setParameterId('');
      setNamespaceId('');
      setKeyName('');
      clearError();
    }
  };

  return (
    <section
      className="program-declarations"
      aria-label={embedded ? 'Program declarations' : undefined}
      aria-labelledby={embedded ? undefined : 'program-declarations-title'}
    >
      {!embedded && (
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Portable explicit sources</p>
            <h3 id="program-declarations-title">Declarations</h3>
          </div>
        </header>
      )}
      {!showNavigation && selection === null && (
        <div className="program-empty-guidance">
          <strong>Select a declaration</strong>
          <span>Choose an existing source or New in the Params navigator.</span>
        </div>
      )}
      {error && (
        <div className="program-invalid-edit-strip" role="alert">
          <span>{error}</span>
          <button
            type="button"
            onClick={() => document.querySelector<HTMLElement>(
              '.program-declaration-form.has-invalid-edit [aria-invalid="true"]',
            )?.focus()}
          >
            Review
          </button>
          <button type="button" onClick={discardInvalidEdit}>Discard edit</button>
        </div>
      )}
      {selectedDiagnostics.length > 0 && (
        <div className="program-authoring-diagnostics" role="status">
          <strong>Complete this declaration</strong>
          {selectedDiagnostics.slice(0, 6).map((diagnostic) => (
            <span key={`${diagnostic.code}:${diagnostic.path}`}>{diagnostic.message}</span>
          ))}
        </div>
      )}
      <div className={`program-declaration-columns${showNavigation ? '' : ' contextual'}`}>
        {(showNavigation || selection?.kind === 'parameter') && <section>
          <h4>Program parameters</h4>
          {showNavigation && <ul className="program-declaration-list">
            {parameters.map((parameter) => {
              const usages = parameterUsages[parameter.parameter_id] ?? [];
              return (
                <li key={parameter.parameter_id}>
                  <button type="button" onClick={() => editParameter(parameter)}>
                    <strong>{parameter.parameter_id}</strong>
                    <small>{parameter.type} · {parameter.runtime_mutability}</small>
                  </button>
                  <span>{usages.length} binding{usages.length === 1 ? '' : 's'}</span>
                  <button
                    type="button"
                    className="text-button"
                    disabled={busy || usages.length > 0}
                    title={usages.length > 0
                      ? usages.map((usage) => `${usage.nodeLabel}.${usage.fieldName}`).join(', ')
                      : 'Remove declaration'}
                    onClick={() => onMutate('program_draft_remove_parameters', {
                      parameter_ids: [parameter.parameter_id],
                    })}
                  >
                    Remove
                  </button>
                </li>
              );
            })}
          </ul>}
          <div className={`program-declaration-form${error && selection?.kind === 'parameter' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'parameter' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, parameterIssue())} onBlur={() => {
            if (selectedParameter && !busy) saveParameter(false);
          }}>
            <label>Parameter ID<input id="program-parameter-id" value={parameterId} aria-invalid={errorTarget === 'program-parameter-id'} title={errorTarget === 'program-parameter-id' ? error ?? undefined : undefined} onChange={(event) => setParameterId(event.target.value)} /></label>
            <label>Type<select value={parameterType} onChange={(event) => changeParameterType(event.target.value)}>{PROGRAM_TYPES.map((type) => <option key={type}>{type}</option>)}</select></label>
            <label>Runtime mutability<select value={mutability} onChange={(event) => setMutability(event.target.value as typeof mutability)}><option value="immutable">immutable</option><option value="before_run">before run</option><option value="between_operations">between operations</option></select></label>
            {parameterType === 'bool' && (
              <label>
                Default
                <select
                  id="program-parameter-default"
                  value={scalarDefault === true ? 'true' : 'false'}
                  aria-invalid={errorTarget === 'program-parameter-default'}
                  title={errorTarget === 'program-parameter-default' ? error ?? undefined : undefined}
                  onChange={(event) => setDefaultText(event.target.value)}
                >
                  <option value="true">True</option>
                  <option value="false">False</option>
                </select>
              </label>
            )}
            {(parameterType === 'float64' || parameterType === 'int64') && (
              <>
                <label>
                  Default
                  <input
                    id="program-parameter-default"
                    type="number"
                    step={parameterType === 'int64' ? '1' : 'any'}
                    value={typeof scalarDefault === 'number' ? scalarDefault : ''}
                    aria-invalid={errorTarget === 'program-parameter-default'}
                    title={errorTarget === 'program-parameter-default' ? error ?? undefined : undefined}
                    onChange={(event) => setDefaultText(event.currentTarget.value)}
                  />
                </label>
                <fieldset
                  id="program-parameter-constraints"
                  className="program-parameter-constraints"
                  aria-invalid={errorTarget === 'program-parameter-constraints'}
                  title={errorTarget === 'program-parameter-constraints' ? error ?? undefined : undefined}
                >
                  <legend>Numeric constraints</legend>
                  <label>
                    Minimum
                    <span>
                      <select
                        aria-label="Minimum bound kind"
                        value={minimumConstraint}
                        onChange={(event) => {
                          const next = event.currentTarget.value;
                          const current = minimumConstraint
                            ? parameterConstraints[minimumConstraint]
                            : 0;
                          const constraints = { ...parameterConstraints };
                          delete constraints.min_inclusive;
                          delete constraints.min_exclusive;
                          if (next) constraints[next] = current ?? 0;
                          setConstraintsText(JSON.stringify(constraints, null, 2));
                        }}
                      >
                        <option value="">None</option>
                        <option value="min_inclusive">Inclusive (≥)</option>
                        <option value="min_exclusive">Exclusive (&gt;)</option>
                      </select>
                      <input
                        aria-label="Minimum bound"
                        type="number"
                        step={parameterType === 'int64' ? '1' : 'any'}
                        value={minimumConstraint ? String(parameterConstraints[minimumConstraint] ?? '') : ''}
                        disabled={!minimumConstraint}
                        onChange={(event) => minimumConstraint
                          && updateNumericConstraint(minimumConstraint, event.currentTarget.value)}
                      />
                    </span>
                  </label>
                  <label>
                    Maximum
                    <span>
                      <select
                        aria-label="Maximum bound kind"
                        value={maximumConstraint}
                        onChange={(event) => {
                          const next = event.currentTarget.value;
                          const current = maximumConstraint
                            ? parameterConstraints[maximumConstraint]
                            : 0;
                          const constraints = { ...parameterConstraints };
                          delete constraints.max_inclusive;
                          delete constraints.max_exclusive;
                          if (next) constraints[next] = current ?? 0;
                          setConstraintsText(JSON.stringify(constraints, null, 2));
                        }}
                      >
                        <option value="">None</option>
                        <option value="max_inclusive">Inclusive (≤)</option>
                        <option value="max_exclusive">Exclusive (&lt;)</option>
                      </select>
                      <input
                        aria-label="Maximum bound"
                        type="number"
                        step={parameterType === 'int64' ? '1' : 'any'}
                        value={maximumConstraint ? String(parameterConstraints[maximumConstraint] ?? '') : ''}
                        disabled={!maximumConstraint}
                        onChange={(event) => maximumConstraint
                          && updateNumericConstraint(maximumConstraint, event.currentTarget.value)}
                      />
                    </span>
                  </label>
                </fieldset>
              </>
            )}
            {parameterType === 'string' && (
              <>
                <label>
                  Default
                  <input
                    id="program-parameter-default"
                    type="text"
                    value={typeof scalarDefault === 'string' ? scalarDefault : ''}
                    aria-invalid={errorTarget === 'program-parameter-default'}
                    title={errorTarget === 'program-parameter-default' ? error ?? undefined : undefined}
                    onChange={(event) => setDefaultText(JSON.stringify(event.currentTarget.value))}
                  />
                </label>
                <fieldset
                  id="program-parameter-constraints"
                  className="program-parameter-constraints"
                  aria-invalid={errorTarget === 'program-parameter-constraints'}
                  title={errorTarget === 'program-parameter-constraints' ? error ?? undefined : undefined}
                >
                  <legend>Length constraints</legend>
                  <label>Minimum length<input type="number" min="0" step="1" value={String(parameterConstraints.min_length ?? '')} onChange={(event) => updateNumericConstraint('min_length', event.currentTarget.value)} /></label>
                  <label>Maximum length<input type="number" min="0" step="1" value={String(parameterConstraints.max_length ?? '')} onChange={(event) => updateNumericConstraint('max_length', event.currentTarget.value)} /></label>
                </fieldset>
              </>
            )}
            {!isScalarParameterType(parameterType) && (
              <>
                <label>Default value<textarea id="program-parameter-default" rows={3} value={defaultText} aria-invalid={errorTarget === 'program-parameter-default'} title={errorTarget === 'program-parameter-default' ? error ?? undefined : undefined} onChange={(event) => setDefaultText(event.target.value)} spellCheck={false} /></label>
                <label>Advanced constraints<textarea id="program-parameter-constraints" rows={3} value={constraintsText} aria-invalid={errorTarget === 'program-parameter-constraints'} title={errorTarget === 'program-parameter-constraints' ? error ?? undefined : undefined} onChange={(event) => setConstraintsText(event.target.value)} spellCheck={false} /></label>
              </>
            )}
            <label>Description<input value={parameterDescription} onChange={(event) => setParameterDescription(event.target.value)} /></label>
            {!selectedParameter && <button type="button" disabled={busy} onClick={() => saveParameter()}>Add parameter</button>}
            {!showNavigation && selectedParameter && (
              <button
                type="button"
                className="danger-button"
                disabled={busy || (parameterUsages[selectedParameter.parameter_id] ?? []).length > 0}
                onClick={() => onMutate('program_draft_remove_parameters', {
                  parameter_ids: [selectedParameter.parameter_id],
                })}
              >
                Remove parameter
              </button>
            )}
          </div>
          {!showNavigation && selectedParameter && (
            <div className="program-declaration-usage">
              <h4>Bindings</h4>
              {(parameterUsages[selectedParameter.parameter_id] ?? []).length === 0
                ? <p>This parameter is not used by a tree node.</p>
                : (parameterUsages[selectedParameter.parameter_id] ?? []).map((usage) => (
                  <div key={`${usage.nodeId}:${usage.fieldName}`}>
                    <button type="button" onClick={() => onSelectNode?.(usage.nodeId)}>
                      {usage.nodeLabel}.{usage.fieldName}
                    </button>
                    <button type="button" disabled={busy} onClick={() => unbindUsage(usage.nodeId, usage.fieldName)}>
                      Use literal
                    </button>
                  </div>
                ))}
              {bindingCandidates.length > 0 && (
                <>
                  <h4>Compatible fields</h4>
                  {bindingCandidates.map((candidate) => (
                    <div key={`${candidate.nodeId}:${candidate.fieldName}`}>
                      <button type="button" onClick={() => onSelectNode?.(candidate.nodeId)}>
                        {candidate.nodeLabel}.{candidate.fieldName}
                      </button>
                      <button type="button" disabled={busy} onClick={() => bindParameter(candidate.nodeId, candidate.fieldName)}>
                        Bind
                      </button>
                    </div>
                  ))}
                </>
              )}
            </div>
          )}
        </section>}

        {(showNavigation || selection?.kind === 'namespace' || selection?.kind === 'key') && <section>
          {(showNavigation || selection?.kind === 'namespace') && <>
          <h4>Blackboard namespace</h4>
          {showNavigation && <ul className="program-declaration-list compact">
            {namespaces.map((namespace) => {
              const childKeys = keys.filter((key) => key.key.startsWith(`${namespace.namespace_id}.`));
              return (
                <li key={namespace.namespace_id}>
                  <button type="button" onClick={() => {
                    setNamespaceId(namespace.namespace_id);
                    setNamespaceDescription(namespace.description ?? '');
                  }}><strong>{namespace.namespace_id}</strong></button>
                  <span>{childKeys.length} key{childKeys.length === 1 ? '' : 's'}</span>
                  <button
                    type="button"
                    className="text-button"
                    disabled={busy || childKeys.length > 0}
                    onClick={() => onMutate('blackboard_draft_remove_namespaces', {
                      namespace_ids: [namespace.namespace_id],
                    })}
                  >Remove</button>
                </li>
              );
            })}
          </ul>}
          <div className={`program-declaration-form compact${error && selection?.kind === 'namespace' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'namespace' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, namespaceIssue())} onBlur={() => {
            if (selectedNamespace && !busy) saveNamespace(false);
          }}>
            <label>Namespace ID<input id="program-namespace-id" value={namespaceId} aria-invalid={errorTarget === 'program-namespace-id'} title={errorTarget === 'program-namespace-id' ? error ?? undefined : undefined} onChange={(event) => setNamespaceId(event.target.value)} /></label>
            <label>Description<input value={namespaceDescription} onChange={(event) => setNamespaceDescription(event.target.value)} /></label>
            {!selectedNamespace && <button type="button" disabled={busy} onClick={() => saveNamespace()}>Add namespace</button>}
            {!showNavigation && selectedNamespace && (
              <button
                type="button"
                className="danger-button"
                disabled={busy || keys.some((key) => key.key.startsWith(`${selectedNamespace.namespace_id}.`))}
                onClick={() => onMutate('blackboard_draft_remove_namespaces', {
                  namespace_ids: [selectedNamespace.namespace_id],
                })}
              >
                Remove namespace
              </button>
            )}
          </div>
          {!showNavigation && selectedNamespace && (
            <p className="program-policy-help">
              {keys.filter((key) => key.key.startsWith(`${selectedNamespace.namespace_id}.`)).length} declared key(s) use this namespace.
            </p>
          )}
          </>}

          {(showNavigation || selection?.kind === 'key') && <>
          <h4>Blackboard keys</h4>
          {showNavigation && <ul className="program-declaration-list">
            {keys.map((key) => {
              const usages = blackboardUsages[key.key] ?? [];
              return (
                <li key={key.key}>
                  <button type="button" onClick={() => editKey(key)}>
                    <strong>{key.key}</strong>
                    <small>{key.type} · {key.access}</small>
                  </button>
                  <span>{usages.length} binding{usages.length === 1 ? '' : 's'}</span>
                  <button
                    type="button"
                    className="text-button"
                    disabled={busy || usages.length > 0}
                    title={usages.length > 0
                      ? usages.map((usage) => `${usage.nodeLabel}.${usage.fieldName}`).join(', ')
                      : 'Remove declaration'}
                    onClick={() => onMutate('blackboard_draft_remove_keys', {
                      keys: [key.key],
                    })}
                  >Remove</button>
                </li>
              );
            })}
          </ul>}
          <div className={`program-declaration-form${error && selection?.kind === 'key' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'key' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, keyIssue())} onBlur={() => {
            if (selectedKey && !busy) saveKey(false);
          }}>
            <label>Fully-qualified key<input id="program-key-name" value={keyName} aria-invalid={errorTarget === 'program-key-name'} title={errorTarget === 'program-key-name' ? error ?? undefined : undefined} onChange={(event) => setKeyName(event.target.value)} placeholder="namespace.key" /></label>
            <label>Type<input value={keyType} onChange={(event) => setKeyType(event.target.value)} /></label>
            <label>Access<select value={keyAccess} onChange={(event) => setKeyAccess(event.target.value as typeof keyAccess)}><option value="read">read</option><option value="write">write</option><option value="read_write">read/write</option></select></label>
            <label>Persistence<select value={keyPersistence} onChange={(event) => setKeyPersistence(event.target.value as typeof keyPersistence)}><option value="node">node</option><option value="run">run</option><option value="persistent">persistent</option></select></label>
            <fieldset id="program-key-fields" className="program-blackboard-fields" title={errorTarget === 'program-key-fields' ? error ?? undefined : undefined}>
              <legend>Structured fields</legend>
              {fields.map((field, index) => (
                <div key={`${index}:${field.field}`}>
                  <input aria-label={`Blackboard field ${index + 1} ID`} aria-invalid={errorTarget === 'program-key-fields'} value={field.field} disabled={busy} placeholder="field_id" onChange={(event) => setFields((current) => current.map((row, rowIndex) => rowIndex === index ? { ...row, field: event.target.value } : row))} />
                  <input aria-label={`Blackboard field ${index + 1} type`} value={field.type} disabled={busy} placeholder="type" onChange={(event) => setFields((current) => current.map((row, rowIndex) => rowIndex === index ? { ...row, type: event.target.value } : row))} />
                  <button type="button" disabled={busy || index === 0} onClick={() => setFields((current) => {
                    const next = [...current];
                    [next[index - 1], next[index]] = [next[index], next[index - 1]];
                    return next;
                  })}>Earlier</button>
                  <button type="button" disabled={busy || index === fields.length - 1} onClick={() => setFields((current) => {
                    const next = [...current];
                    [next[index], next[index + 1]] = [next[index + 1], next[index]];
                    return next;
                  })}>Later</button>
                  <button type="button" className="text-button" disabled={busy} onClick={() => setFields((current) => current.filter((_row, rowIndex) => rowIndex !== index))}>Remove</button>
                </div>
              ))}
              <button type="button" className="text-button" disabled={busy} onClick={() => setFields((current) => [...current, { field: '', type: 'string' }])}>Add field</button>
            </fieldset>
            <label>Description<input value={keyDescription} onChange={(event) => setKeyDescription(event.target.value)} /></label>
            {!selectedKey && <button type="button" disabled={busy} onClick={() => saveKey()}>Add key</button>}
            {!showNavigation && selectedKey && (
              <button
                type="button"
                className="danger-button"
                disabled={busy || (blackboardUsages[selectedKey.key] ?? []).length > 0}
                onClick={() => onMutate('blackboard_draft_remove_keys', {
                  keys: [selectedKey.key],
                })}
              >
                Remove key
              </button>
            )}
          </div>
          {!showNavigation && selectedKey && (
            <div className="program-declaration-usage">
              <h4>Bindings</h4>
              {(blackboardUsages[selectedKey.key] ?? []).length === 0
                ? <p>This key is not used by a tree node.</p>
                : (blackboardUsages[selectedKey.key] ?? []).map((usage) => (
                  <div key={`${usage.nodeId}:${usage.fieldName}:${usage.field ?? ''}`}>
                    <button type="button" onClick={() => onSelectNode?.(usage.nodeId)}>
                      {usage.nodeLabel}.{usage.fieldName}
                    </button>
                    <button type="button" disabled={busy} onClick={() => unbindUsage(usage.nodeId, usage.fieldName)}>
                      Use literal
                    </button>
                  </div>
                ))}
            </div>
          )}
          </>}
        </section>}
      </div>
    </section>
  );
}
