import { useEffect, useMemo, useState } from 'react';

import type {
  BlackboardFieldDeclaration,
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  JsonValue,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  collectBlackboardUsages,
  collectProgramParameterUsages,
} from '../utils/programAuthoring';
import type { ProgramDeclarationSelection } from './ProgramDeclarationsNavigator';

const PROGRAM_TYPES = [
  'bool',
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
  root: ProgramTreeNode;
  parameters: ProgramParameterDeclaration[];
  namespaces: BlackboardNamespaceDeclaration[];
  keys: BlackboardKeyDeclaration[];
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
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

export function ProgramDeclarationsEditor({
  embedded = false,
  selection = null,
  showNavigation = true,
  root,
  parameters,
  namespaces,
  keys,
  busy,
  onMutate,
}: ProgramDeclarationsEditorProps) {
  const parameterUsages = useMemo(() => collectProgramParameterUsages(root), [root]);
  const blackboardUsages = useMemo(() => collectBlackboardUsages(root), [root]);
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
  const [fieldsText, setFieldsText] = useState('[]');
  const [keyDescription, setKeyDescription] = useState('');
  const [error, setError] = useState<string | null>(null);

  const editParameter = (parameter: ProgramParameterDeclaration) => {
    setParameterId(parameter.parameter_id);
    setParameterType(parameter.type);
    setDefaultText(JSON.stringify(parameter.default, null, 2));
    setConstraintsText(JSON.stringify(parameter.constraints ?? {}, null, 2));
    setMutability(parameter.runtime_mutability);
    setParameterDescription(parameter.description ?? '');
    setError(null);
  };

  const saveParameter = () => {
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
      onMutate('program_draft_upsert_parameters', {
        parameters: [declaration] as unknown as JsonValue,
      });
      setParameterId('');
      setError(null);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : 'Parameter is invalid.');
    }
  };

  const saveNamespace = () => {
    const id = namespaceId.trim();
    if (!id) {
      setError('Namespace ID is required.');
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
    setNamespaceId('');
    setError(null);
  };

  const editKey = (key: BlackboardKeyDeclaration) => {
    setKeyName(key.key);
    setKeyType(key.type);
    setKeyAccess(key.access);
    setKeyPersistence(key.persistence);
    setFieldsText(JSON.stringify(key.fields ?? [], null, 2));
    setKeyDescription(key.description ?? '');
    setError(null);
  };

  useEffect(() => {
    if (selection?.kind === 'parameter') {
      const parameter = parameters.find((row) => row.parameter_id === selection.id);
      if (parameter) editParameter(parameter);
    } else if (selection?.kind === 'namespace') {
      const namespace = namespaces.find((row) => row.namespace_id === selection.id);
      if (namespace) {
        setNamespaceId(namespace.namespace_id);
        setNamespaceDescription(namespace.description ?? '');
        setError(null);
      }
    } else if (selection?.kind === 'key') {
      const key = keys.find((row) => row.key === selection.id);
      if (key) editKey(key);
    }
  }, [keys, namespaces, parameters, selection]);

  const saveKey = () => {
    try {
      const parsedFields = parseJson(fieldsText, 'Fields');
      if (!Array.isArray(parsedFields)) throw new Error('Fields must be a JSON array.');
      const fields = parsedFields as unknown as BlackboardFieldDeclaration[];
      if (fields.some((field) =>
        field === null
        || typeof field !== 'object'
        || typeof field.field !== 'string'
        || typeof field.type !== 'string'
      )) {
        throw new Error('Every field must contain string field and type values.');
      }
      const declaration: BlackboardKeyDeclaration = {
        key: keyName.trim(),
        type: keyType,
        access: keyAccess,
        persistence: keyPersistence,
        ...(fields.length > 0 ? { fields } : {}),
        ...(keyDescription.trim() ? { description: keyDescription.trim() } : {}),
      };
      if (!declaration.key) throw new Error('Blackboard key is required.');
      onMutate('blackboard_draft_upsert_keys', {
        keys: [declaration] as unknown as JsonValue,
      });
      setKeyName('');
      setError(null);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : 'Blackboard key is invalid.');
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
      <div className="program-declaration-columns">
        <section>
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
          <div className="program-declaration-form">
            <label>Parameter ID<input value={parameterId} onChange={(event) => setParameterId(event.target.value)} /></label>
            <label>Type<select value={parameterType} onChange={(event) => setParameterType(event.target.value)}>{PROGRAM_TYPES.map((type) => <option key={type}>{type}</option>)}</select></label>
            <label>Runtime mutability<select value={mutability} onChange={(event) => setMutability(event.target.value as typeof mutability)}><option value="immutable">immutable</option><option value="before_run">before run</option><option value="between_operations">between operations</option></select></label>
            <label>Default (strict JSON)<textarea rows={3} value={defaultText} onChange={(event) => setDefaultText(event.target.value)} spellCheck={false} /></label>
            <label>Constraints (strict JSON)<textarea rows={3} value={constraintsText} onChange={(event) => setConstraintsText(event.target.value)} spellCheck={false} /></label>
            <label>Description<input value={parameterDescription} onChange={(event) => setParameterDescription(event.target.value)} /></label>
            <button type="button" disabled={busy} onClick={saveParameter}>Add or replace parameter</button>
          </div>
        </section>

        <section>
          <h4>Blackboard namespaces</h4>
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
          <div className="program-declaration-form compact">
            <label>Namespace ID<input value={namespaceId} onChange={(event) => setNamespaceId(event.target.value)} /></label>
            <label>Description<input value={namespaceDescription} onChange={(event) => setNamespaceDescription(event.target.value)} /></label>
            <button type="button" disabled={busy} onClick={saveNamespace}>Add or replace namespace</button>
          </div>

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
          <div className="program-declaration-form">
            <label>Fully-qualified key<input value={keyName} onChange={(event) => setKeyName(event.target.value)} placeholder="namespace.key" /></label>
            <label>Type<input value={keyType} onChange={(event) => setKeyType(event.target.value)} /></label>
            <label>Access<select value={keyAccess} onChange={(event) => setKeyAccess(event.target.value as typeof keyAccess)}><option value="read">read</option><option value="write">write</option><option value="read_write">read/write</option></select></label>
            <label>Persistence<select value={keyPersistence} onChange={(event) => setKeyPersistence(event.target.value as typeof keyPersistence)}><option value="node">node</option><option value="run">run</option><option value="persistent">persistent</option></select></label>
            <label>Fields (strict JSON array)<textarea rows={3} value={fieldsText} onChange={(event) => setFieldsText(event.target.value)} spellCheck={false} /></label>
            <label>Description<input value={keyDescription} onChange={(event) => setKeyDescription(event.target.value)} /></label>
            <button type="button" disabled={busy} onClick={saveKey}>Add or replace key</button>
          </div>
        </section>
      </div>
      {error && <p className="program-form-error declaration-error" role="alert">{error}</p>}
    </section>
  );
}
