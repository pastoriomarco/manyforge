import { useCardPanelLayout } from '../hooks/useCardPanelLayout';
import { BreakableId } from './BreakableId';
import { SettingsSubheading } from './SettingsScope';
import { useCallback, useEffect, useMemo, useState, type KeyboardEvent } from 'react';

import { PROGRAM_AUTHORED_VALUE_TYPES } from '../types/api';
import type {
  BlackboardFieldDeclaration,
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  CatalogEntry,
  DraftMutationResponse,
  JsonValue,
  ProgramAuthoringDiagnostic,
  ProgramFormulaReferences,
  ProgramDocument,
  ProgramParameterDeclaration,
  ProgramPoseFrame,
  ProgramTreeNode,
} from '../types/api';
import {
  collectProgramBindingCandidates,
  collectBlackboardUsages,
  collectFormulaUsages,
  collectProgramParameterUsages,
  flattenProgramForest,
  formulaUsageLabel,
  type FormulaUsage,
  type ProgramMotionDefaultRole,
} from '../utils/programAuthoring';
import { requestOpenSettings } from '../state/settingsNavigation';
import { canonicalFrames, frameKey, frameLabel, programFrameChoices } from '../utils/programFrames';
import { valuePathParameter } from '../utils/valuePaths';
import type { ProgramDeclarationSelection } from './ProgramDeclarationsNavigator';
import { buildProgramParameterMutation, typeValuesTransition } from './ProgramPropertyEditor';
import { FormulaEditor } from './property/FormulaEditor';
import { FrameSelector } from './property/FrameSelector';

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
  /** The names each Draft formula reads; formulas count as usages. */
  formulaReferences: ProgramFormulaReferences;
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
  /** Parameters acting as Program motion defaults, by parameter ID. */
  motionDefaults?: ReadonlyMap<string, ProgramMotionDefaultRole>;
  /** The declared robots and Scene objects a pose key's `allowed_frames` name. */
  frameProgram?: Pick<ProgramDocument, 'target_roles' | 'scene'>;
}

function usageTitle(bindings: string[], formulas: FormulaUsage[]): string {
  return bindings.length + formulas.length > 0
    ? [...bindings, ...formulas.map(formulaUsageLabel)].join(', ')
    : 'Remove declaration';
}

function FormulaUsages({
  usages,
  onSelectNode,
}: {
  usages: FormulaUsage[];
  onSelectNode?: (instanceId: string) => void;
}) {
  if (usages.length === 0) return null;
  return (
    <>
      <SettingsSubheading title="Formulas" />
      {usages.map((usage) => (
        <div key={formulaUsageLabel(usage)}>
          {usage.kind === 'node' ? (
            <button type="button" onClick={() => onSelectNode?.(usage.nodeId)}>
              <BreakableId text={formulaUsageLabel(usage)} />
            </button>
          ) : <BreakableId text={formulaUsageLabel(usage)} />}
        </div>
      ))}
    </>
  );
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

/** Key types a formula can compute (docs/authoring/EXPRESSIONS.md §3). */
function isComputableKeyType(value: string): value is ScalarParameterType {
  return isScalarParameterType(value);
}

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

function programValueMismatch(type: string, value: JsonValue): boolean {
  return (type === 'bool' && typeof value !== 'boolean')
    || (type === 'float64' && (typeof value !== 'number' || !Number.isFinite(value)))
    || (type === 'int64' && (typeof value !== 'number' || !Number.isSafeInteger(value)))
    || (type === 'string' && typeof value !== 'string')
    || (type === 'string_list' && (
      !Array.isArray(value) || value.some((item) => typeof item !== 'string')
    ));
}

/** The authored initial value, or undefined for a key that is empty until written. */
function parseInitialValue(type: string, text: string | null): Exclude<JsonValue, null> | undefined {
  if (text === null || !PROGRAM_AUTHORED_VALUE_TYPES.has(type)) return undefined;
  const value = parseJson(text, 'Initial value');
  if (value === null || programValueMismatch(type, value)) {
    throw new Error(`Initial value must match ${type}.`);
  }
  return value;
}

export interface BlackboardKeyForm {
  key: string;
  type: string;
  access: BlackboardKeyDeclaration['access'];
  persistence: BlackboardKeyDeclaration['persistence'];
  fields: BlackboardFieldDeclaration[];
  description: string;
  /** JSON text of the initial value; null omits it. */
  initialText: string | null;
  /** A computed key's formula; null for a key that nodes and the operator write. */
  expression: string | null;
  /** A pose key's allowed frames; null allows every declared frame. */
  allowedFrames?: ProgramPoseFrame[] | null;
}

export function buildBlackboardKeyDeclaration(form: BlackboardKeyForm): BlackboardKeyDeclaration {
  const initial = parseInitialValue(form.type, form.initialText);
  const key = form.key.trim();
  if (!key) throw new Error('Blackboard key is required.');
  if (form.expression !== null) {
    if (initial !== undefined) throw new Error('Formula: a computed key has no initial value.');
    if (form.persistence === 'persistent') throw new Error('Formula: a persistent key cannot be computed.');
    if (!isComputableKeyType(form.type)) throw new Error(`Formula: ${form.type} keys cannot be computed.`);
  }
  return {
    key,
    type: form.type,
    access: form.access,
    persistence: form.persistence,
    ...(initial === undefined ? {} : { initial }),
    ...(form.expression === null ? {} : { expression: form.expression }),
    ...(form.type === 'stored_pose' && form.allowedFrames != null && form.allowedFrames.length > 0
      ? { allowed_frames: canonicalFrames(form.allowedFrames) }
      : {}),
    ...(form.fields.length > 0 ? { fields: form.fields } : {}),
    ...(form.description.trim() ? { description: form.description.trim() } : {}),
  };
}

interface ProgramValueInputProps {
  id: string;
  label: string;
  type: string;
  text: string;
  error: string | null;
  onChange: (text: string) => void;
}

/** One authored value (parameter default or key initial value) edited per Program type. */
function ProgramValueInput({ id, label, type, text, error, onChange }: ProgramValueInputProps) {
  let value: JsonValue;
  try {
    value = JSON.parse(text) as JsonValue;
  } catch {
    value = defaultForParameterType(type);
  }
  const invalid = error !== null;
  const title = error ?? undefined;
  if (type === 'bool') {
    return (
      <label>
        {label}
        <select id={id} value={value === true ? 'true' : 'false'} aria-invalid={invalid} title={title} onChange={(event) => onChange(event.target.value)}>
          <option value="true">True</option>
          <option value="false">False</option>
        </select>
      </label>
    );
  }
  if (type === 'float64' || type === 'int64') {
    return (
      <label>
        {label}
        <input
          id={id}
          type="number"
          step={type === 'int64' ? '1' : 'any'}
          value={typeof value === 'number' ? value : ''}
          aria-invalid={invalid}
          title={title}
          onChange={(event) => onChange(event.currentTarget.value)}
        />
      </label>
    );
  }
  if (type === 'string') {
    return (
      <label>
        {label}
        <input
          id={id}
          type="text"
          value={typeof value === 'string' ? value : ''}
          aria-invalid={invalid}
          title={title}
          onChange={(event) => onChange(JSON.stringify(event.currentTarget.value))}
        />
      </label>
    );
  }
  return <label>{label}<textarea id={id} rows={3} value={text} aria-invalid={invalid} title={title} onChange={(event) => onChange(event.target.value)} spellCheck={false} /></label>;
}

function initialText(key: BlackboardKeyDeclaration): string | null {
  return key.initial === undefined ? null : JSON.stringify(key.initial, null, 2);
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
  formulaReferences,
  catalogEntries = [],
  creationExpectedType = null,
  creationExpectedAccess = null,
  busy,
  onMutate,
  onSelectNode,
  onCreated,
  onInvalidEditChange,
  diagnostics = [],
  motionDefaults = new Map(),
  frameProgram,
}: ProgramDeclarationsEditorProps) {
  const cardLayout = useCardPanelLayout();
  const parameterUsages = useMemo(() => collectProgramParameterUsages(roots), [roots]);
  const blackboardUsages = useMemo(() => collectBlackboardUsages(roots), [roots]);
  const formulaUsages = useMemo(
    () => collectFormulaUsages(formulaReferences),
    [formulaReferences],
  );
  const [parameterId, setParameterId] = useState('');
  const [parameterType, setParameterType] = useState('float64');
  const [defaultText, setDefaultText] = useState('0');
  const [constraintsText, setConstraintsText] = useState('{}');
  const [parameterDescription, setParameterDescription] = useState('');
  const [namespaceId, setNamespaceId] = useState('');
  const [namespaceDescription, setNamespaceDescription] = useState('');
  const [keyName, setKeyName] = useState('');
  const [keyType, setKeyType] = useState('bool');
  const [keyAccess, setKeyAccess] = useState<'read' | 'write' | 'read_write'>('read_write');
  const [keyPersistence, setKeyPersistence] = useState<BlackboardKeyDeclaration['persistence']>('run');
  const [keyInitialText, setKeyInitialText] = useState<string | null>(null);
  const [keyExpression, setKeyExpression] = useState<string | null>(null);
  const [keyAllowedFrames, setKeyAllowedFrames] = useState<ProgramPoseFrame[] | null>(null);
  // The formula dialog is open; the field that opened it gets the focus back.
  const [formulaOpen, setFormulaOpen] = useState<{ returnFocus: HTMLElement | null } | null>(null);
  const openFormula = () => setFormulaOpen({
    returnFocus: document.activeElement instanceof HTMLElement ? document.activeElement : null,
  });
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
  const selectedMotionDefault = selectedParameter === null
    ? null
    : motionDefaults.get(selectedParameter.parameter_id) ?? null;
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
    || keyInitialText !== initialText(selectedKey)
    || keyExpression !== (selectedKey.expression ?? null)
    || JSON.stringify(keyAllowedFrames) !== JSON.stringify(selectedKey.allowed_frames ?? null)
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
      if (programValueMismatch(parameterType, defaultValue)) {
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
    try {
      parseInitialValue(keyType, keyInitialText);
    } catch (caught) {
      return {
        message: caught instanceof Error ? caught.message : 'Initial value is invalid.',
        target: 'program-key-initial',
      };
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
    setKeyInitialText(initialText(key));
    setKeyExpression(key.expression ?? null);
    setKeyAllowedFrames(key.allowed_frames ?? null);
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
        setKeyInitialText(null);
        setKeyExpression(null);
        setKeyAllowedFrames(null);
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

  // A formula saved from its dialog is passed in: the form state updates later.
  const saveKey = (
    clearAfterSave = true,
    value = { expression: keyExpression, initialText: keyInitialText },
  ) => {
    if (
      !clearAfterSave && selectedKey && !keyDirty
      && value.expression === keyExpression && value.initialText === keyInitialText
    ) return;
    try {
      const issue = keyIssue();
      if (issue) throw new Error(issue.message);
      const declaration = buildBlackboardKeyDeclaration({
        key: keyName,
        type: keyType,
        access: keyAccess,
        persistence: keyPersistence,
        fields,
        description: keyDescription,
        initialText: value.initialText,
        expression: value.expression,
        allowedFrames: keyAllowedFrames,
      });
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
        message.startsWith('Initial value')
          ? 'program-key-initial'
          : message.startsWith('Formula')
            ? 'program-key-expression'
          : message.toLowerCase().includes('field')
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

  const unbindUsage = (nodeId: string, path: string) => {
    const node = flat.find((row) => row.node.instance_id === nodeId)?.node;
    const entry = node ? catalog.get(node.node_id) : undefined;
    const field = entry?.parameters.find((parameter) => parameter.id === valuePathParameter(path));
    if (!node || !field) {
      reportError('That declaration usage is not present in the current catalog revision.');
      return;
    }
    const result = typeValuesTransition(node, path, field, parameters);
    if ('error' in result) {
      reportError(`${path}: ${result.error}`);
      return;
    }
    const request = buildProgramParameterMutation(node, path, result.transition);
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
      ref={cardLayout}
      className="program-declarations dpanel"
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
        {(showNavigation || selection?.kind === 'parameter') && <section className="dcard">
          <h4>Program parameters</h4>
          {!showNavigation && selectedMotionDefault !== null && (
            <div className="program-motion-default-role">
              <SettingsSubheading title="Program motion default" />
              <p>
                The default <code>{selectedMotionDefault.field}</code> for every motion node:{' '}
                {selectedMotionDefault.bound} of {selectedMotionDefault.applicable} nodes that
                accept it are bound to it, and new ones bind automatically.
                {selectedMotionDefault.ceiling && ' A node may set a lower value, never a higher one.'}
                {selectedMotionDefault.applicable === 0
                  && ' No node in this Program accepts it yet, so it changes no Program move.'}
              </p>
              {selectedMotionDefault.manualDefault && (
                <p>
                  Also the default for manual Cartesian moves once applied; a stricter
                  unapplied value applies to them at once.
                </p>
              )}
              <div className="dactions">
                <button type="button" onClick={() => requestOpenSettings('program')}>
                  Motion defaults
                </button>
              </div>
            </div>
          )}
          {showNavigation && <ul className="program-declaration-list">
            {parameters.map((parameter) => {
              const usages = parameterUsages[parameter.parameter_id] ?? [];
              const formulas = formulaUsages.parameters[parameter.parameter_id] ?? [];
              return (
                <li key={parameter.parameter_id}>
                  <button type="button" onClick={() => editParameter(parameter)}>
                    <strong>{parameter.parameter_id}</strong>
                    <small>{parameter.type}</small>
                  </button>
                  <span>
                    {usages.length} binding{usages.length === 1 ? '' : 's'}
                    {formulas.length > 0 && ` · ${formulas.length} formula${formulas.length === 1 ? '' : 's'}`}
                  </span>
                  <button
                    type="button"
                    className="text-button"
                    disabled={busy || usages.length + formulas.length > 0}
                    title={usageTitle(
                      usages.map((usage) => `${usage.nodeLabel}.${usage.fieldName}`),
                      formulas,
                    )}
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
          <div className={`program-declaration-form drows${error && selection?.kind === 'parameter' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'parameter' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, parameterIssue())} onBlur={() => {
            if (selectedParameter && !busy) saveParameter(false);
          }}>
            <label>Parameter ID<input id="program-parameter-id" value={parameterId} aria-invalid={errorTarget === 'program-parameter-id'} title={errorTarget === 'program-parameter-id' ? error ?? undefined : undefined} onChange={(event) => setParameterId(event.target.value)} /></label>
            <label>Type<select value={parameterType} onChange={(event) => changeParameterType(event.target.value)}>{PROGRAM_TYPES.map((type) => <option key={type}>{type}</option>)}</select></label>
            <ProgramValueInput
              id="program-parameter-default"
              label="Default"
              type={parameterType}
              text={defaultText}
              error={errorTarget === 'program-parameter-default' ? error : null}
              onChange={setDefaultText}
            />
            {(parameterType === 'float64' || parameterType === 'int64') && (
              <fieldset
                id="program-parameter-constraints"
                className="program-parameter-constraints"
                aria-invalid={errorTarget === 'program-parameter-constraints'}
                title={errorTarget === 'program-parameter-constraints' ? error ?? undefined : undefined}
              >
                <legend>Numeric constraints</legend>
                <label>
                  Minimum
                  <span className="dunit">
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
                  <span className="dunit">
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
            )}
            {parameterType === 'string' && (
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
            )}
            {!isScalarParameterType(parameterType) && (
              <label>Advanced constraints<textarea id="program-parameter-constraints" rows={3} value={constraintsText} aria-invalid={errorTarget === 'program-parameter-constraints'} title={errorTarget === 'program-parameter-constraints' ? error ?? undefined : undefined} onChange={(event) => setConstraintsText(event.target.value)} spellCheck={false} /></label>
            )}
            <label>Description<input value={parameterDescription} onChange={(event) => setParameterDescription(event.target.value)} /></label>
            {!selectedParameter && <button type="button" disabled={busy} onClick={() => saveParameter()}>Add parameter</button>}
            {!showNavigation && selectedParameter && (
              <button
                type="button"
                className="danger-button"
                disabled={busy
                  || (parameterUsages[selectedParameter.parameter_id] ?? []).length > 0
                  || (formulaUsages.parameters[selectedParameter.parameter_id] ?? []).length > 0}
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
              <SettingsSubheading title="Program-node bindings" />
              {(parameterUsages[selectedParameter.parameter_id] ?? []).length === 0
                && (formulaUsages.parameters[selectedParameter.parameter_id] ?? []).length === 0
                ? <p>This parameter is not used by a tree node.</p>
                : (parameterUsages[selectedParameter.parameter_id] ?? []).map((usage) => (
                  <div key={`${usage.nodeId}:${usage.fieldName}`}>
                    <button type="button" onClick={() => onSelectNode?.(usage.nodeId)}>
                      <BreakableId text={`${usage.nodeLabel}.${usage.fieldName}`} />
                    </button>
                    <button type="button" disabled={busy} onClick={() => unbindUsage(usage.nodeId, usage.fieldName)}>
                      Type values
                    </button>
                  </div>
                ))}
              <FormulaUsages
                usages={formulaUsages.parameters[selectedParameter.parameter_id] ?? []}
                onSelectNode={onSelectNode}
              />
              {bindingCandidates.length > 0 && (
                <>
                  <SettingsSubheading title="Compatible fields" />
                  {bindingCandidates.map((candidate) => (
                    <div key={`${candidate.nodeId}:${candidate.fieldName}`}>
                      <button type="button" onClick={() => onSelectNode?.(candidate.nodeId)}>
                        <BreakableId text={`${candidate.nodeLabel}.${candidate.fieldName}`} />
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

        {(showNavigation || selection?.kind === 'namespace' || selection?.kind === 'key') && <section className="dcard">
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
          <div className={`program-declaration-form drows compact${error && selection?.kind === 'namespace' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'namespace' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, namespaceIssue())} onBlur={() => {
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
              const formulas = formulaUsages.keys[key.key] ?? [];
              return (
                <li key={key.key}>
                  <button type="button" onClick={() => editKey(key)}>
                    <strong>{key.key}</strong>
                    <small>{key.type} · {key.access}{key.expression === undefined ? '' : ' · computed'}</small>
                  </button>
                  <span>
                    {usages.length} binding{usages.length === 1 ? '' : 's'}
                    {formulas.length > 0 && ` · ${formulas.length} formula${formulas.length === 1 ? '' : 's'}`}
                  </span>
                  <button
                    type="button"
                    className="text-button"
                    disabled={busy || usages.length + formulas.length > 0}
                    title={usageTitle(
                      usages.map((usage) => `${usage.nodeLabel}.${usage.fieldName}`),
                      formulas,
                    )}
                    onClick={() => onMutate('blackboard_draft_remove_keys', {
                      keys: [key.key],
                    })}
                  >Remove</button>
                </li>
              );
            })}
          </ul>}
          <div className={`program-declaration-form drows${error && selection?.kind === 'key' ? ' has-invalid-edit' : ''}${selectedDiagnostics.length > 0 && selection?.kind === 'key' ? ' has-backend-diagnostic' : ''}`} onKeyDown={(event) => guardCommitKey(event, keyIssue())} onBlur={() => {
            if (selectedKey && !busy) saveKey(false);
          }}>
            <label>Fully-qualified key<input id="program-key-name" value={keyName} aria-invalid={errorTarget === 'program-key-name'} title={errorTarget === 'program-key-name' ? error ?? undefined : undefined} onChange={(event) => setKeyName(event.target.value)} placeholder="namespace.key" /></label>
            <label>Type<input value={keyType} onChange={(event) => {
              setKeyType(event.target.value);
              setKeyInitialText(null);
              setKeyExpression(null);
              setKeyAllowedFrames(null);
            }} /></label>
            <label>Access<select value={keyAccess} onChange={(event) => setKeyAccess(event.target.value as typeof keyAccess)}><option value="read">read</option><option value="write">write</option><option value="read_write">read/write</option></select></label>
            <label>Persistence<select value={keyPersistence} onChange={(event) => setKeyPersistence(event.target.value as typeof keyPersistence)}><option value="run">run</option><option value="persistent" disabled={keyExpression !== null}>persistent{keyExpression !== null ? ' (not for computed keys)' : ''}</option></select></label>
            <p className="program-policy-help">
              {keyExpression !== null
                ? 'Always holds its formula\'s result, recomputed when a node that needs it starts.'
                : keyPersistence === 'run'
                  ? 'Lives for one run: starts from the initial value at each Run, otherwise empty.'
                  : 'Survives runs, Reset and restarts: starts from the stored value, otherwise the initial value.'}
            </p>
            {PROGRAM_AUTHORED_VALUE_TYPES.has(keyType) && (
              <label>
                Value
                <select
                  value={keyExpression !== null ? 'computed' : keyInitialText === null ? 'none' : 'value'}
                  onChange={(event) => {
                    const choice = event.target.value;
                    if (choice === 'computed') {
                      openFormula();
                      return;
                    }
                    setKeyExpression(null);
                    setKeyInitialText(choice === 'none'
                      ? null
                      : JSON.stringify(defaultForParameterType(keyType), null, 2));
                  }}
                >
                  <option value="none">empty until written</option>
                  <option value="value">initial value</option>
                  {isComputableKeyType(keyType) && (
                    <option value="computed" disabled={keyPersistence === 'persistent'}>
                      computed by a formula{keyPersistence === 'persistent' ? ' (not for persistent keys)' : ''}
                    </option>
                  )}
                </select>
              </label>
            )}
            {keyExpression !== null && (
              <label>
                Formula
                <span className="program-value-field sourced">
                  <input
                    id="program-key-expression"
                    readOnly
                    value={keyExpression}
                    title={`f(x) = ${keyExpression}\nNodes and the operator cannot write a computed key.`}
                    aria-invalid={errorTarget === 'program-key-expression'}
                    onClick={openFormula}
                    onKeyDown={(event) => {
                      if (event.key !== 'Enter') return;
                      event.preventDefault();
                      openFormula();
                    }}
                  />
                  <button
                    type="button"
                    className="program-value-marker"
                    tabIndex={-1}
                    aria-label="Edit the key's formula"
                    onClick={openFormula}
                  >
                    f(x)
                  </button>
                </span>
              </label>
            )}
            {keyType === 'stored_pose' && frameProgram !== undefined && (
              <fieldset id="program-key-allowed-frames" className="program-blackboard-fields">
                <legend>Allowed frames</legend>
                {keyAllowedFrames === null
                  ? <p className="program-policy-help">Any frame the Program declares.</p>
                  : keyAllowedFrames.map((frame) => (
                    <div key={frameKey(frame)}>
                      <span>{frameLabel(frame)}</span>
                      <button
                        type="button"
                        className="text-button"
                        disabled={busy}
                        aria-label={`Remove allowed frame ${frameLabel(frame)}`}
                        onClick={() => setKeyAllowedFrames((current) => {
                          const next = (current ?? []).filter((row) => frameKey(row) !== frameKey(frame));
                          // No list means every declared frame; an empty list is never stored.
                          return next.length === 0 ? null : next;
                        })}
                      >Remove</button>
                    </div>
                  ))}
                <FrameSelector
                  scope={{
                    program: frameProgram,
                    allowedFrames: programFrameChoices({ program: frameProgram })
                      .map((choice) => choice.frame)
                      .filter((frame) => !(keyAllowedFrames ?? [])
                        .some((row) => frameKey(row) === frameKey(frame))),
                  }}
                  value={null}
                  disabled={busy}
                  ariaLabel="Add allowed frame"
                  onChange={(frame) => setKeyAllowedFrames((current) => canonicalFrames([
                    ...(current ?? []),
                    frame,
                  ]))}
                />
              </fieldset>
            )}
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
                disabled={busy
                  || (blackboardUsages[selectedKey.key] ?? []).length > 0
                  || (formulaUsages.keys[selectedKey.key] ?? []).length > 0}
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
              <SettingsSubheading title="Bindings" />
              {(blackboardUsages[selectedKey.key] ?? []).length === 0
                && (formulaUsages.keys[selectedKey.key] ?? []).length === 0
                ? <p>This key is not used by a tree node.</p>
                : (blackboardUsages[selectedKey.key] ?? []).map((usage) => (
                  <div key={`${usage.nodeId}:${usage.fieldName}:${usage.field ?? ''}`}>
                    <button type="button" onClick={() => onSelectNode?.(usage.nodeId)}>
                      <BreakableId text={`${usage.nodeLabel}.${usage.fieldName}`} />
                    </button>
                    <button type="button" disabled={busy} onClick={() => unbindUsage(usage.nodeId, usage.fieldName)}>
                      Type values
                    </button>
                  </div>
                ))}
              <FormulaUsages
                usages={formulaUsages.keys[selectedKey.key] ?? []}
                onSelectNode={onSelectNode}
              />
            </div>
          )}
          </>}
        </section>}
      </div>
      {formulaOpen !== null && isComputableKeyType(keyType) && (
        <FormulaEditor
          title={`Formula for key ${keyName.trim() || 'new key'}`}
          expression={keyExpression ?? ''}
          destinationType={keyType}
          // Only a key the Draft already computes can be excluded from its own names.
          excludeComputedKey={selectedKey?.expression === undefined ? null : selectedKey.key}
          onSave={(expression) => {
            setKeyExpression(expression);
            setKeyInitialText(null);
            if (selectedKey && !busy) saveKey(false, { expression, initialText: null });
          }}
          onClose={() => setFormulaOpen(null)}
          returnFocus={formulaOpen.returnFocus}
        />
      )}
    </section>
  );
}
