import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useCardPanelLayout } from '../hooks/useCardPanelLayout';

import type {
  AppliedProgramSelection,
  CatalogEntry,
  CatalogJsonSchema,
  CatalogParameter,
  DraftMutationResponse,
  DeploymentWorkspaceResponse,
  ExpressionValueType,
  JsonValue,
  ProgramAuthoringDiagnostic,
  ProgramAuthoringDocument,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramValueSource,
  SourceModelProjection,
} from '../types/api';
import type { ManualControlMotionDescriptor } from '../types/robotControl';
import type { CarriedObjectLostSignalChoice } from '../utils/systemSignalAuthoring';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import {
  ioEmptyOmits,
  ioSignalRoleChoices,
  ioTimeoutHint,
  ioUnsupportedChoices,
} from '../utils/ioAuthoring';
import {
  globalRuleIds,
  interruptKeyInitialProblem,
  INTERRUPT_NODE_IDS,
  nodeConditionChoices,
  operandId,
  stopStagesForTargetRole,
  successThroughStages,
  type TargetStopChain,
} from '../utils/interruptAuthoring';
import { keyValueParameterRule } from '../utils/keyValueAuthoring';
import {
  MOCK_SIMULATION_NODE_IDS,
  MOCK_SIMULATION_PARAMETER,
  MOCK_SIMULATION_SOURCE_REFUSED,
  mockSignalProfiles,
  mockSimulationProblem,
  nodeMockSignals,
} from '../utils/mockSimulation';
import {
  catalogDefaultParameters,
  deriveProgramMotionConfiguration,
  flattenProgramForest,
  inheritedProgramExecutionGroups,
  PROGRAM_MOTION_CEILING_FIELDS,
  PROGRAM_MOTION_FIELDS,
  programBlackboardKeys,
  programDiagnosticNodeId,
  resolveProgramParameterLiteralValue,
  type ProgramMotionField,
} from '../utils/programAuthoring';
import {
  programSourceJointInventory,
} from '../utils/programSourceJoints';
import { valueSourceMenuModel } from '../utils/valueSourceMenu';
import {
  buildMotionEndpointMutation,
  motionEndpointKeyOfNode,
  programMotionEndpointChoices,
} from '../utils/motionEndpointAuthoring';
import {
  hasSourcesWithin,
  setValueAtPath,
  sourcesAfterLiteral,
  valueAtPath,
  valuePathParameter,
  valuePathWithin,
  withValueSource,
  type ListMove,
} from '../utils/valuePaths';
import { formatBlackboardValue } from './BlackboardValueEditor';
import { CanvasContextMenu } from './CanvasContextMenu';
import { DraftTextInput, DraftTextarea } from './common/DraftInputs';
import { FormulaEditor } from './property/FormulaEditor';
import {
  defaultSchemaValue,
  ProgramSchemaValueEditor,
  type ProgramSchemaValueEditorContext,
} from './property/ProgramSchemaValueEditor';
import {
  ValueSourceContext,
  valueSourceMenuEntries,
  type ValueSourceController,
} from './property/ValueSourceField';
import { InterruptConditionEditor } from './InterruptConditionEditor';
import { MockSimulationEditor } from './MockSimulationEditor';
import { PneumaticRequiredInputsEditor } from './PneumaticRequiredInputsEditor';
import { declaredPneumaticInputs } from '../utils/pneumaticGripperAuthoring';
import { IO_LIVENESS_UNPROVEN_WARNING } from '../utils/ioSignalConfiguration';
import { precisionContactStopWarnings } from '../utils/precisionContactStopWarnings';
import { resolvedTargetRoles } from '../utils/targetRoleResolution';
import { ProgramPolicyEditor } from './ProgramPolicyEditor';
import { StopStagePicker } from './StopStagePicker';
import { GraspLostPolicyEditor } from './GraspLostPolicyEditor';

interface ProgramPropertyEditorProps {
  embedded?: boolean;
  roots: ProgramTreeNode[];
  selectedId: string | null;
  catalogEntries: CatalogEntry[];
  program: ProgramAuthoringDocument;
  appliedProgram?: AppliedProgramSelection | null;
  sourceModel?: SourceModelProjection | null;
  sourceModelUnavailableReason?: string | null;
  deploymentWorkspace?: DeploymentWorkspaceResponse | null;
  systemSignalChoices?: readonly CarriedObjectLostSignalChoice[];
  systemSignalsNeedApply?: boolean;
  /** The scenario's Program role assignment: resolves target roles while the workspace has no binding. */
  programRoleAssignment?: Record<string, JsonValue> | null;
  motionDescriptors?: ManualControlMotionDescriptor[];
  /** Published motion targets' qualified Stop chains (SX-2167); absent = "available after Apply". */
  stopChains?: TargetStopChain[];
  busy: boolean;
  onMutate: (
    mutation: string,
    args: Record<string, JsonValue>,
  ) => Promise<DraftMutationResponse | null> | DraftMutationResponse | null | void;
  onCreateReference?: ProgramSchemaValueEditorContext['onCreateReference'];
  onCreateGroup?: () => void;
  onRenamed?: (previousId: string, renamedId: string) => void;
  onUpdateNodePolicies: (
    instanceId: string,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
    executionGroups: NonNullable<ProgramTreeNode['execution_groups']>,
  ) => void;
  onUpdateNodePresentation?: (instanceId: string, collapsedByDefault: boolean) => void;
  onInvalidEditChange?: (invalid: boolean) => void;
  diagnostics?: ProgramAuthoringDiagnostic[];
}

function isProgramMotionField(field: string): field is ProgramMotionField {
  return PROGRAM_MOTION_FIELDS.includes(field as ProgramMotionField);
}

function isProgramMotionCeilingField(field: string): boolean {
  return PROGRAM_MOTION_CEILING_FIELDS.includes(
    field as typeof PROGRAM_MOTION_CEILING_FIELDS[number],
  );
}

/**
 * One change to a node's parameters and value sources, at a value path:
 * - `literal`: a typed value for a whole parameter. List moves carry the
 *   sources inside its items along; sources whose value is gone are dropped.
 * - `source`: the value at `path` comes from a key, a parameter or a formula.
 *   The stored literal is kept and overlapping sources are replaced.
 * - `type_values`: the value at `path` is typed again; `literal` is stored
 *   only when the parameter has none.
 * - `clear`: the parameter is omitted, with every source inside it.
 */
export type ProgramParameterTransition =
  | { kind: 'literal'; value: JsonValue; moves?: readonly ListMove[] }
  | { kind: 'source'; source: ProgramValueSource }
  | { kind: 'type_values'; literal?: JsonValue }
  | { kind: 'clear' };

export interface ProgramParameterMutation {
  mutation: 'tree_draft_update_node_params';
  args: Record<string, JsonValue>;
}

/**
 * Build the one full-replacement Draft mutation used by every property
 * interaction. The value at `path` changes representation atomically while
 * every unrelated literal and source is retained.
 */
export function buildProgramParameterMutation(
  node: ProgramTreeNode,
  path: string,
  transition: ProgramParameterTransition,
): ProgramParameterMutation {
  let parameters = { ...node.parameters };
  let sources = { ...(node.value_sources ?? {}) };
  if (transition.kind === 'literal') {
    sources = sourcesAfterLiteral(
      sources,
      path,
      transition.value,
      transition.moves,
      parameters[path],
    );
    parameters[path] = transition.value;
  } else if (transition.kind === 'source') {
    sources = withValueSource(sources, path, transition.source);
  } else if (transition.kind === 'type_values') {
    delete sources[path];
    if (transition.literal !== undefined) {
      parameters = setValueAtPath(parameters, path, transition.literal) ?? parameters;
    }
  } else {
    delete parameters[path];
    sources = Object.fromEntries(Object.entries(sources).filter(([entry]) =>
      !valuePathWithin(entry, path)));
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

/**
 * Return the value at `path` to its typed literal. A nested value always has
 * one; a whole parameter bound before it had a literal falls back to the
 * bound parameter's default or the catalog default, which must be valid.
 */
export function typeValuesTransition(
  node: ProgramTreeNode,
  path: string,
  field: CatalogParameter,
  programParameters: ProgramParameterDeclaration[],
  /** The typed value of a map entry a source supplied without a literal. */
  entryDefault?: JsonValue,
): { transition: ProgramParameterTransition } | { error: string } {
  if (path !== field.id) {
    return {
      transition: valueAtPath(node.parameters, path) === undefined && entryDefault !== undefined
        ? { kind: 'type_values', literal: entryDefault }
        : { kind: 'type_values' },
    };
  }
  if (Object.prototype.hasOwnProperty.call(node.parameters, field.id)) {
    return { transition: { kind: 'type_values' } };
  }
  const fallback = resolveProgramParameterLiteralValue(
    node.value_sources?.[path],
    undefined,
    field.hasDefault ? field.default as JsonValue : undefined,
    programParameters,
  );
  const message = fallback === undefined
    ? field.required ? `${field.id} has no valid literal fallback` : null
    : firstCatalogParameterValidationMessage(field, fallback);
  return message === null
    ? { transition: { kind: 'type_values', literal: fallback } }
    : { error: message };
}

/** Whether a backend diagnostic path names this parameter's literal or one of its sources. */
function diagnosticTargetsParameter(path: string, parameterId: string): boolean {
  return ['parameters', 'value_sources'].some((section) => {
    const prefix = `.${section}.${parameterId}`;
    const at = path.indexOf(prefix);
    const next = path[at + prefix.length];
    return at >= 0 && (next === undefined || next === '.' || next === '[');
  });
}

export function ProgramPropertyEditor({
  embedded = false,
  roots,
  selectedId,
  catalogEntries,
  program,
  appliedProgram = null,
  sourceModel = null,
  sourceModelUnavailableReason = null,
  deploymentWorkspace = null,
  systemSignalChoices = [],
  systemSignalsNeedApply = false,
  programRoleAssignment = null,
  motionDescriptors = [],
  stopChains = [],
  busy,
  onMutate,
  onCreateReference,
  onCreateGroup,
  onRenamed,
  onUpdateNodePolicies,
  onUpdateNodePresentation,
  onInvalidEditChange,
  diagnostics = [],
}: ProgramPropertyEditorProps) {
  const programParameters = program.parameters ?? [];
  const deploymentDocument = deploymentWorkspace?.draft?.document ?? null;
  // The Deployment Draft's global rules add read-only `interrupt_state.<rule_id>` keys.
  const ruleIds = useMemo(
    () => globalRuleIds(deploymentDocument as Record<string, JsonValue> | null),
    [deploymentDocument],
  );
  const blackboardKeys = useMemo(() => programBlackboardKeys(program, ruleIds), [program, ruleIds]);
  const conditionChoices = useMemo(() => nodeConditionChoices(
    program, deploymentWorkspace?.binding, ruleIds,
    deploymentDocument as Record<string, JsonValue> | null, programRoleAssignment, systemSignalChoices,
  ), [deploymentDocument, deploymentWorkspace?.binding, program, programRoleAssignment, ruleIds, systemSignalChoices]);
  const motionConfiguration = useMemo(
    () => deriveProgramMotionConfiguration(program, catalogEntries),
    [catalogEntries, program],
  );
  const motionBindings = motionConfiguration.available
    ? motionConfiguration.configuration.bindings
    : null;
  const [errors, setErrors] = useState<Record<string, { local?: string; commit?: string }>>({});
  const setFieldError = useCallback((field: string, message: string | null, source: 'local' | 'commit' = 'local') => {
    setErrors((current) => {
      if ((current[field]?.[source] ?? null) === message) return current;
      const nextField = { ...current[field] };
      if (message === null) delete nextField[source];
      else nextField[source] = message;
      const next = { ...current };
      if (Object.keys(nextField).length === 0) delete next[field];
      else next[field] = nextField;
      return next;
    });
  }, []);
  const [discardEpoch, setDiscardEpoch] = useState(0);
  const [sourceMenu, setSourceMenu] = useState<{
    path: string;
    schema: CatalogJsonSchema;
    x: number;
    y: number;
    returnFocus: HTMLElement | null;
  } | null>(null);
  const [formula, setFormula] = useState<{
    path: string;
    type: ExpressionValueType;
    expression: string;
    returnFocus: HTMLElement | null;
  } | null>(null);
  const cardLayout = useCardPanelLayout();
  const selected = useMemo(
    () => flattenProgramForest(roots).find(
      ({ node }) => node.instance_id === selectedId,
    )?.node ?? null,
    [roots, selectedId],
  );
  const inheritedGroups = useMemo(
    () => inheritedProgramExecutionGroups(roots, selectedId),
    [roots, selectedId],
  );
  const entry = selected
    ? catalogEntries.find((candidate) => candidate.id === selected.node_id) ?? null
    : null;
  const selectedDiagnostics = useMemo(() => diagnostics.filter((diagnostic) =>
    programDiagnosticNodeId(roots, diagnostic.path) === selectedId,
  ), [diagnostics, roots, selectedId]);
  // Warnings only: never part of the node's validity or Apply gating.
  const workspaceBinding = deploymentWorkspace?.binding;
  // The binding's target rows, or without one the Draft-resolved rows (targetRoleResolution).
  const targetRoleRows = useMemo(() => (workspaceBinding == null
    ? resolvedTargetRoles(program, programRoleAssignment)
    : workspaceBinding.target_role_bindings), [workspaceBinding, program, programRoleAssignment]);
  const draftTargetRoles = useMemo(() => (programRoleAssignment == null
    ? targetRoleRows : resolvedTargetRoles(program, programRoleAssignment)),
  [program, programRoleAssignment, targetRoleRows]);
  const stopWarnings = useMemo(() => (selected === null ? [] : precisionContactStopWarnings(
    selected, deploymentDocument as Record<string, JsonValue> | null, targetRoleRows,
  )), [deploymentDocument, targetRoleRows, selected]);
  // The bound robot's qualified Stop chain; null keeps "available after Apply".
  const qualifiedStopStages = useMemo(() => (selected === null ? null : stopStagesForTargetRole(
    stopChains, targetRoleRows, selected.parameters.target_role,
  )), [targetRoleRows, selected, stopChains]);
  const selectedDeploymentSpeed = useMemo(() => {
    if (selected === null) return null;
    const targetRole = selected.parameters.target_role;
    if (typeof targetRole !== 'string') return null;
    const target = targetRoleRows.find(
      (row) => row.target_role_id === targetRole,
    );
    if (target === undefined) return null;
    const descriptor = motionDescriptors.find(
      (candidate) => candidate.controllableComponentId === target.controllable_component_id
        && candidate.commandTargetId === target.command_target_id,
    );
    if (descriptor?.operationalLimit === null || descriptor?.operationalLimit === undefined) {
      return null;
    }
    return {
      target: `${target.controllable_component_id}/${target.command_target_id}`,
      maximum: descriptor.operationalLimit.maximumTcpLinearSpeedMps,
    };
  }, [targetRoleRows, motionDescriptors, selected]);
  const sourceJointInventory = useMemo(() => selected === null
    ? null
    : programSourceJointInventory(
      program,
      selected,
      appliedProgram,
      sourceModel,
      sourceModelUnavailableReason
        ?? 'The active source-model projection is unavailable.',
    ), [
      appliedProgram,
      program,
      selected,
      sourceModel,
      sourceModelUnavailableReason,
    ]);
  const motionEndpointChoices = useMemo(
    () => programMotionEndpointChoices(program),
    [program],
  );
  useEffect(() => {
    setErrors({});
    setDiscardEpoch((current) => current + 1);
    setSourceMenu(null);
    setFormula(null);
  }, [selectedId]);
  const hasInvalidEdit = Object.keys(errors).length > 0;
  const invalidEditCallback = useRef(onInvalidEditChange);
  useEffect(() => { invalidEditCallback.current = onInvalidEditChange; }, [onInvalidEditChange]);
  useEffect(() => { invalidEditCallback.current?.(hasInvalidEdit); }, [hasInvalidEdit]);
  useEffect(() => () => invalidEditCallback.current?.(false), []);

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
    path: string,
    transition: ProgramParameterTransition,
  ) => {
    const request = buildProgramParameterMutation(selected, path, transition);
    void onMutate(request.mutation, request.args);
  };

  const motionBindingOf = (parameterId: string) => isProgramMotionField(parameterId)
    ? motionBindings?.[parameterId] ?? null
    : null;

  const valueSourceModel = (path: string, schema: CatalogJsonSchema) => valueSourceMenuModel({
    entry,
    path,
    schema,
    sources: selected.value_sources,
    literals: { ...catalogDefaultParameters(entry), ...selected.parameters },
    parameters: programParameters,
    keys: blackboardKeys,
    preferredParameter: motionBindingOf(valuePathParameter(path)),
  });

  const typeValues = (path: string, schema: CatalogJsonSchema) => {
    const parameterId = valuePathParameter(path);
    const field = entry.parameters.find((parameter) => parameter.id === parameterId);
    if (field === undefined) return;
    const result = typeValuesTransition(
      selected,
      path,
      field,
      programParameters,
      defaultSchemaValue(schema),
    );
    if ('error' in result) {
      setFieldError(parameterId, result.error, 'commit');
      return;
    }
    setFieldError(parameterId, null, 'commit');
    applyTransition(path, result.transition);
  };

  const sourceController: ValueSourceController = {
    model: valueSourceModel,
    openMenu: (path, schema, anchor, point) => {
      // A whole-value scope has no box of its own; the focused field has.
      const focused = document.activeElement;
      const field = focused instanceof HTMLElement && anchor.contains(focused) ? focused : anchor;
      const rect = field.getBoundingClientRect();
      setSourceMenu({
        path,
        schema,
        x: point?.x ?? rect.left,
        y: point?.y ?? rect.bottom,
        returnFocus: field,
      });
    },
    editFormula: (path, schema) => {
      const model = valueSourceModel(path, schema);
      if (model?.formulaType == null) return;
      setFormula({
        path,
        type: model.formulaType,
        expression: model.current?.kind === 'expression' ? model.current.expression : '',
        returnFocus: document.activeElement instanceof HTMLElement ? document.activeElement : null,
      });
    },
    describe: (source) => {
      if (source.kind === 'expression') {
        return { label: source.expression, title: `f(x) = ${source.expression}` };
      }
      if (source.kind === 'program_parameter') {
        const row = programParameters.find((candidate) => candidate.parameter_id === source.reference);
        return {
          label: `param.${source.reference}`,
          title: row === undefined
            ? `param.${source.reference} is not declared.`
            : `param.${source.reference} = ${formatBlackboardValue(row.type, row.default)} (parameter default)`,
        };
      }
      if (source.kind === 'blackboard_field') {
        return {
          label: `key.${source.reference}.${source.field}`,
          title: `Field ${source.field} of key ${source.reference}; written while the Program runs.`,
        };
      }
      const row = blackboardKeys.find((candidate) => candidate.key === source.reference);
      return {
        label: `key.${source.reference}`,
        title: row === undefined
          ? `key.${source.reference} is not declared.`
          : row.expression !== undefined
            ? `key.${source.reference} = ${row.expression} (computed)`
            : row.initial !== undefined
              ? `key.${source.reference} starts at ${formatBlackboardValue(row.type, row.initial)}`
              : `key.${source.reference} is empty until written.`,
      };
    },
    hasSourcesWithin: (path) => hasSourcesWithin(selected.value_sources, path),
    disabled: busy,
  };
  const menuModel = sourceMenu === null ? null : valueSourceModel(sourceMenu.path, sourceMenu.schema);
  const closeSourceMenu = () => {
    sourceMenu?.returnFocus?.focus();
    setSourceMenu(null);
  };

  const renameNode = (replacement: string) => {
    const normalized = replacement.trim();
    const duplicate = flattenProgramForest(roots).some(
      ({ node }) => node.instance_id === normalized && node.instance_id !== selected.instance_id,
    );
    const message = !/^[a-z][a-z0-9_.-]*$/.test(normalized)
      ? 'Node name must be a portable lower-case ID.'
      : new TextEncoder().encode(normalized).length > 128
        ? 'Node name must be at most 128 bytes.'
        : duplicate ? 'Another node already uses this name.' : null;
    if (message) {
      setFieldError('_identity', message);
      return;
    }
    setFieldError('_identity', null);
    if (normalized === selected.instance_id) return;
    void Promise.resolve(onMutate('tree_draft_rename_node', {
      instance_id: selected.instance_id,
      new_instance_id: normalized,
    })).then((response) => {
      const previous = response?.details.previous_instance_id;
      const renamed = response?.details.renamed_instance_id;
      if (typeof previous === 'string' && typeof renamed === 'string') {
        onRenamed?.(previous, renamed);
      }
    });
  };

  const commitLiteral = (
    parameter: CatalogParameter,
    value: JsonValue,
    moves?: readonly ListMove[],
  ) => {
    const motionBinding = isProgramMotionField(parameter.id)
      ? motionBindings?.[parameter.id] ?? null
      : null;
    const ceiling = motionBinding !== null && isProgramMotionCeilingField(parameter.id)
      ? programParameters.find((row) => row.parameter_id === motionBinding)?.default
      : undefined;
    const ceilingMessage = typeof ceiling === 'number'
      && typeof value === 'number'
      && value > ceiling
      ? `${parameter.id}=${value} exceeds Program ${parameter.id}=${ceiling}.`
      : null;
    const message = ceilingMessage
      ?? firstCatalogParameterValidationMessage(parameter, value);
    if (message) {
      setFieldError(parameter.id, message, 'commit');
      return;
    }
    // A schema-valid sibling commit does not settle another child's local text.
    setFieldError(parameter.id, null, 'commit');
    applyTransition(parameter.id, { kind: 'literal', value, moves });
  };

  const removeLiteral = (parameter: CatalogParameter) => {
    const motionBinding = isProgramMotionField(parameter.id)
      ? motionBindings?.[parameter.id] ?? null
      : null;
    if (motionBinding !== null) {
      applyTransition(parameter.id, {
        kind: 'source',
        source: { kind: 'program_parameter', reference: motionBinding },
      });
      return;
    }
    applyTransition(parameter.id, { kind: 'clear' });
  };

  return (
    <section
      ref={cardLayout}
      className="program-properties dpanel"
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
                : first === '_identity'
                  ? document.getElementById('program-node-name')
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
      {stopWarnings.map((warning) => (
        <p key={warning} className="inline-message severity-warning" role="note">{warning}</p>
      ))}
      <section className="dcard" aria-label="Node">
          <h4>Node</h4>
          <label>
            <span>Name</span>
            <DraftTextInput
              id="program-node-name"
              value={selected.instance_id}
              disabled={busy}
              validate={(value) => {
                const normalized = value.trim();
                if (!/^[a-z][a-z0-9_.-]*$/.test(normalized)) {
                  return 'Node name must be a portable lower-case ID.';
                }
                if (new TextEncoder().encode(normalized).length > 128) {
                  return 'Node name must be at most 128 bytes.';
                }
                return flattenProgramForest(roots).some(
                  ({ node }) => node.instance_id === normalized
                    && node.instance_id !== selected.instance_id,
                ) ? 'Another node already uses this name.' : null;
              }}
              onInvalidChange={(message) => setFieldError('_identity', message)}
              onCommit={renameNode}
            />
          </label>
          <dl>
            <div><dt>Catalog name</dt><dd>{entry.name}</dd></div>
            <div><dt>Catalog ID</dt><dd><code>{selected.node_id}</code></dd></div>
            <div><dt>Type and profile</dt><dd>{entry.kind} · {entry.category} · {entry.minProgramProfile ?? 'portable'}</dd></div>
          </dl>
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
      </section>
      <ValueSourceContext.Provider value={sourceController}>
      <section className="dcard" aria-label="Parameters">
        <h4>Parameters</h4>
        {entry.parameters.length === 0 && (
          <p className="muted">This node has no authored parameters.</p>
        )}
        {entry.parameters.map((parameter) => {
          const hasMotionTcpEndpoint = entry.parameters.some((field) => field.id === 'tcp_endpoint');
          const source = selected.value_sources?.[parameter.id];
          const rule = keyValueParameterRule(entry, selected, blackboardKeys, parameter.id);
          const hasLiteral = Object.prototype.hasOwnProperty.call(
            selected.parameters,
            parameter.id,
          );
          if (rule.inapplicable !== undefined && !hasLiteral && source === undefined) return null;
          const literal = hasLiteral
            ? selected.parameters[parameter.id]
            : parameter.hasDefault ? parameter.default as JsonValue : undefined;
          const displayedSpeed = source === undefined ? literal
            : source.kind === 'program_parameter'
              ? programParameters.find((row) => row.parameter_id === source.reference)?.default
              : undefined;
          const literalValidation = hasLiteral
            ? firstCatalogParameterValidationMessage(parameter, literal)
            : null;
          const repairMode = literalValidation !== null && source === undefined;
          const schema = rule.choices === undefined
            ? parameter.schema
            : { ...parameter.schema, enum: [...rule.choices] };
          const backendDiagnostic = selectedDiagnostics.find((diagnostic) =>
            diagnosticTargetsParameter(diagnostic.path, parameter.id));
          const signalChoices = parameter.id === 'resource_role'
            ? ioSignalRoleChoices(
              selected.node_id,
              selected.parameters.target_role,
              deploymentWorkspace?.binding,
              { program, roleAssignment: programRoleAssignment,
                deploymentDocument: deploymentDocument as Record<string, JsonValue> | null },
              systemSignalChoices,
            )
            : null;
          const unsupportedChoices = ioUnsupportedChoices(
            selected.node_id,
            parameter.id,
            schema,
          );
          const emptyOmits = ioEmptyOmits(selected.node_id, parameter.id);
          const interruptField = INTERRUPT_NODE_IDS.has(selected.node_id) && source === undefined
            && !repairMode
            && (parameter.id === 'interrupt_condition' || parameter.id === 'stop_entry_stage_id'
              || parameter.id === 'success_through_stage_id')
            ? parameter.id : null;
          // Catalog schema type `mock_signal_simulation` (contract 133): literal rows only.
          const mockField = MOCK_SIMULATION_NODE_IDS.has(selected.node_id)
            && parameter.id === MOCK_SIMULATION_PARAMETER && !repairMode;
          const requiredInputsField = selected.node_id === 'gripper.command_position'
            && parameter.id === 'required_inputs' && source === undefined && !repairMode
            && !hasSourcesWithin(selected.value_sources, parameter.id)
            && (!Array.isArray(literal) || new Set(literal.map((item) => (
              item !== null && typeof item === 'object' && !Array.isArray(item) ? item.input : null
            ))).size === literal.length);
          const graspLostPolicyField = selected.node_id === 'scene.attach_carried_object'
            && parameter.id === 'on_grasp_lost' && source === undefined && !repairMode;
          const valueEditorContext: ProgramSchemaValueEditorContext = {
            entry,
            node: selected,
            parameterId: parameter.id,
            program,
            globalRuleIds: ruleIds,
            sourceJointInventory,
            onCreateReference,
            semanticChoices: signalChoices,
            unsupportedChoices,
            emptyOmits: emptyOmits
              ? { placeholder: 'Wait forever', onOmit: () => removeLiteral(parameter) }
              : undefined,
          };
          const storedText = (id: string) => (typeof selected.parameters[id] === 'string'
            ? selected.parameters[id] as string : undefined);
          return (
            <div
              className={`program-property${backendDiagnostic ? ' backend-invalid' : ''}`}
              title={backendDiagnostic?.message}
              key={`${parameter.id}:${discardEpoch}`}
            >
              <div className="program-property-heading">
                <label htmlFor={`property-${selected.instance_id}-${parameter.id}`}>
                  {hasMotionTcpEndpoint && parameter.id === 'tcp_endpoint'
                    ? 'Motion TCP endpoint'
                    : parameter.id === 'max_tcp_linear_speed_mps'
                    ? 'TCP speed ceiling (m/s)'
                    : parameter.id}
                </label>
                <span>{parameter.required ? 'required' : 'optional'}</span>
              </div>
              {parameter.id === 'max_tcp_linear_speed_mps' && (
                <p className="muted">
                  {selectedDeploymentSpeed === null
                    ? 'Optional speed ceiling; the backend uses the lower of this value and the applied Deployment target maximum.'
                    : `Currently applied Deployment maximum for ${selectedDeploymentSpeed.target}: ${selectedDeploymentSpeed.maximum} m/s. ${typeof displayedSpeed === 'number' && Number.isFinite(displayedSpeed) && displayedSpeed > 0 ? `With ${source === undefined ? 'request' : 'parameter default'} ${displayedSpeed} m/s, the effective ceiling is ${Math.min(displayedSpeed, selectedDeploymentSpeed.maximum)} m/s. ` : 'The backend resolves the effective ceiling when the request is known. '}The authored value is unchanged.`}
                </p>
              )}
              {rule.inapplicable !== undefined && (
                <p className="program-form-hint">{rule.inapplicable} Omit it below.</p>
              )}
              {signalChoices !== null && signalChoices.length === 0 && (
                <p className="program-form-hint">
                  The current binding has no {selected.node_id === 'io.set_output' ? 'output' : 'input'} signal for {String(selected.parameters.target_role)}.
                </p>
              )}
              {systemSignalsNeedApply && signalChoices !== null && selected.node_id !== 'io.set_output'
                && parameter.id === 'resource_role' && (
                <p className="program-form-hint">Apply to list system signals.</p>
              )}
              {signalChoices !== null && selected.node_id !== 'io.set_output'
                && conditionChoices.some((choice) => choice.liveness === 'change_only'
                  && operandId(choice.operand) === operandId({ signal: {
                    target_role: selected.parameters.target_role ?? null,
                    resource_role: selected.parameters.resource_role ?? null,
                  } })) && (
                <p className="inline-message severity-warning" role="note">{IO_LIVENESS_UNPROVEN_WARNING}</p>
              )}
              {unsupportedChoices !== undefined && (
                <p className="program-form-hint">{unsupportedChoices.hint}</p>
              )}
              {emptyOmits && <p className="program-form-hint">{ioTimeoutHint(parameter.schema)}</p>}
              {rule.locked !== undefined && (
                <p className="program-form-hint">{rule.locked}</p>
              )}
              {rule.choices !== undefined && typeof literal === 'string'
                && !rule.choices.includes(literal) && (
                <p className="program-form-error inline-message severity-error">
                  {literal} does not apply to this key; choose {rule.choices.join(' or ')}.
                </p>
              )}
              {graspLostPolicyField ? (
                <GraspLostPolicyEditor
                  key={`${selected.instance_id}:${parameter.id}:${discardEpoch}`}
                  value={literal}
                  nodeId={selected.instance_id}
                  context={valueEditorContext}
                  disabled={busy}
                  onCommit={(value) => commitLiteral(parameter, value)}
                />
              ) : requiredInputsField ? (() => {
                const target = draftTargetRoles.find((row) => row.target_role_id === selected.parameters.target_role);
                return <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <PneumaticRequiredInputsEditor value={literal}
                    declaredKeys={declaredPneumaticInputs(deploymentDocument as Record<string, JsonValue> | null,
                      target?.controllable_component_id, target?.command_target_id)}
                    disabled={busy || rule.locked !== undefined}
                    onCommit={(value) => commitLiteral(parameter, value)} />
                </div>;
              })() : mockField ? (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  {(() => {
                    const signals = nodeMockSignals(selected);
                    const problem = mockSimulationProblem(selected);
                    return (
                      <>
                        <MockSimulationEditor
                          value={hasLiteral ? literal : undefined}
                          signals={signals}
                          profiles={mockSignalProfiles(signals, deploymentWorkspace?.binding,
                            deploymentDocument as Record<string, JsonValue> | null,
                            program, programRoleAssignment)}
                          disabled={busy || rule.locked !== undefined}
                          onCommit={(value) => commitLiteral(parameter, value)}
                          onClear={() => removeLiteral(parameter)}
                          onInvalidChange={(message) => setFieldError(parameter.id, message)}
                        />
                        {problem?.startsWith(MOCK_SIMULATION_SOURCE_REFUSED) && (
                          <p className="program-form-error inline-message severity-error" role="alert">{problem}</p>
                        )}
                      </>
                    );
                  })()}
                </div>
              ) : interruptField === 'interrupt_condition' ? (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  {systemSignalsNeedApply && (
                    <p className="program-form-hint">Apply to list system signals.</p>
                  )}
                  <InterruptConditionEditor
                    label="Interrupt condition"
                    value={hasLiteral ? literal : undefined}
                    choices={conditionChoices}
                    disabled={busy || rule.locked !== undefined}
                    onCommit={(value) => commitLiteral(parameter, value)}
                    onInvalidChange={(message) => setFieldError(parameter.id, message)}
                  />
                  {(() => {
                    const problem = interruptKeyInitialProblem(hasLiteral ? literal : undefined, blackboardKeys);
                    return problem === null ? null : (
                      <p className="program-form-error inline-message severity-error" aria-label="Interrupt condition key initial">{problem}</p>
                    );
                  })()}
                </div>
              ) : interruptField !== null ? (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <StopStagePicker
                    label={interruptField === 'stop_entry_stage_id' ? 'Stop entry stage' : 'Success through stage'}
                    value={storedText(interruptField)}
                    stages={qualifiedStopStages === null ? null
                      : interruptField === 'stop_entry_stage_id' ? qualifiedStopStages
                        : successThroughStages(qualifiedStopStages, storedText('stop_entry_stage_id'))}
                    omittedLabel={interruptField === 'stop_entry_stage_id'
                      ? 'First stage (default)' : 'Only the entry stage counts (default)'}
                    disabled={busy}
                    onChange={(next) => (next === undefined
                      ? removeLiteral(parameter) : commitLiteral(parameter, next))}
                  />
                  {interruptField === 'success_through_stage_id' && (
                    <p className="program-form-hint">
                      A trip contained up to this stage follows on_signal; containment at a later stage fails the node through its normal failure handling.
                    </p>
                  )}
                </div>
              ) : repairMode ? (
                <>
                  <p className="program-form-error inline-message severity-error" role="alert">
                    Catalog-invalid value: {literalValidation}. Repair it as strict JSON; specialized controls are disabled to preserve every represented field.
                  </p>
                  <DraftTextarea
                    id={`property-${selected.instance_id}-${parameter.id}`}
                    rows={7}
                    value={JSON.stringify(literal, null, 2)}
                    disabled={busy || rule.locked !== undefined}
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
                    onInvalidChange={(message) => setFieldError(parameter.id, message)}
                    onCommit={(text) => {
                      try {
                        commitLiteral(parameter, JSON.parse(text) as JsonValue);
                      } catch (error) {
                        setFieldError(parameter.id, error instanceof Error
                          ? error.message : 'Value must be strict JSON.', 'commit');
                      }
                    }}
                  />
                </>
              ) : hasMotionTcpEndpoint && parameter.id === 'tcp_endpoint' ? (
                <div id={`property-${selected.instance_id}-tcp_endpoint`}>
                  {source !== undefined ? (
                    <>
                      <p className="program-form-error inline-message severity-error" role="alert">
                        The motion TCP endpoint must be a literal selection.
                      </p>
                      <button
                        type="button"
                        className="text-button"
                        disabled={busy || rule.locked !== undefined}
                        onClick={() => {
                          const request = buildMotionEndpointMutation(selected, null);
                          void onMutate(request.mutation, request.args);
                        }}
                      >Remove dynamic endpoint value</button>
                    </>
                  ) : (() => {
                    const currentKey = motionEndpointKeyOfNode(selected) ?? '';
                    const currentIsDeclared = motionEndpointChoices.some(
                      (choice) => choice.key === currentKey,
                    );
                    const currentLabel = 'Current endpoint is not declared';
                    return (
                      <>
                        <select
                          value={currentKey}
                          disabled={busy || rule.locked !== undefined}
                          aria-label="Motion TCP endpoint"
                          onChange={(event) => {
                            const choice = motionEndpointChoices.find(
                              (candidate) => candidate.key === event.currentTarget.value,
                            );
                            const request = buildMotionEndpointMutation(
                              selected,
                              choice === undefined ? null : {
                                target_role: choice.targetRole,
                                frame_role: choice.frameRole,
                              },
                            );
                            void onMutate(request.mutation, request.args);
                          }}
                        >
                          <option value="">Select a declared endpoint</option>
                          {currentKey !== '' && !currentIsDeclared && (
                            <option value={currentKey}>{currentLabel}</option>
                          )}
                          {motionEndpointChoices.map((choice) => (
                            <option key={choice.key} value={choice.key}>{choice.label}</option>
                          ))}
                        </select>
                        <p className="program-form-hint">
                          Choose a frame declared on a target role. The selected endpoint must be on the same physical component as the motion target; this is checked when the motion starts. Current-pose capture is available when the selected endpoint matches the robot’s current TCP.
                        </p>
                      </>
                    );
                  })()}
                </div>
              ) : (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <ProgramSchemaValueEditor
                    schema={schema}
                    value={literal}
                    disabled={busy || rule.locked !== undefined}
                    context={valueEditorContext}
                    path={parameter.id}
                    onCommit={(value, moves) => commitLiteral(parameter, value, moves)}
                    onInvalidChange={(message) => setFieldError(parameter.id, message)}
                  />
                </div>
              )}
              {parameter.id === 'goal'
                && (selected.node_id === 'motion.move_to_pose'
                  || selected.node_id === 'motion.move_to_joint_target') && (() => {
                const pose = selected.node_id === 'motion.move_to_pose';
                const targetRole = selected.parameters.target_role;
                const relative = pose && selected.parameters.target_mode === 'relative';
                const unavailable = busy
                  ? 'Editing is unavailable.'
                  : typeof targetRole !== 'string'
                    ? 'Choose the target role first.'
                    : null;
                return (
                  <button
                    type="button"
                    className="text-button"
                    disabled={unavailable !== null}
                    title={unavailable ?? (
                      `Write the ${pose ? 'current TCP pose' : 'current joint positions'} of `
                      + `${String(targetRole)} into this goal`
                      + (pose ? '. Replaces the goal frame with this robot’s base frame' : '')
                      + (pose && selected.parameters.tcp_endpoint !== undefined
                        ? '. Capture requires the selected target and frame roles to match the robot’s current TCP'
                        : '')
                      + (relative ? '. This switches the move to absolute.' : '.')
                    )}
                    onClick={() => {
                      void onMutate('program_draft_store_observed_move', {
                        instance_id: selected.instance_id,
                        observed_kind: pose ? 'tcp_pose' : 'joint',
                      });
                    }}
                  >
                    {pose ? 'Use current robot pose' : 'Use current joints'}
                  </button>
                );
              })()}
              {!source && (!parameter.required || parameter.hasDefault) && (
                <button
                  type="button"
                  className="text-button"
                  disabled={busy || rule.locked !== undefined || selected.parameters[parameter.id] === undefined}
                  onClick={() => removeLiteral(parameter)}
                >
                  {isProgramMotionField(parameter.id)
                    && motionBindings?.[parameter.id] !== undefined
                    ? 'Use Program default'
                    : 'Use catalog default / omit'}
                </button>
              )}
            </div>
          );
        })}
      </section>
      </ValueSourceContext.Provider>
      {sourceMenu !== null && menuModel !== null && (
        <CanvasContextMenu
          x={sourceMenu.x}
          y={sourceMenu.y}
          heading={sourceMenu.path}
          label={`Where ${sourceMenu.path} comes from`}
          entries={valueSourceMenuEntries(menuModel, {
            typeValues: () => typeValues(menuModel.path, sourceMenu.schema),
            use: (chosen) => {
              setFieldError(valuePathParameter(menuModel.path), null, 'commit');
              applyTransition(menuModel.path, { kind: 'source', source: chosen });
            },
            editFormula: () => sourceController.editFormula(menuModel.path, sourceMenu.schema),
          })}
          onClose={closeSourceMenu}
        />
      )}
      {formula !== null && (
        <FormulaEditor
          title={`Formula for ${formula.path}`}
          expression={formula.expression}
          destinationType={formula.type}
          excludeComputedKey={null}
          onSave={(expression) => applyTransition(formula.path, {
            kind: 'source',
            source: { kind: 'expression', expression },
          })}
          onClose={() => setFormula(null)}
          returnFocus={formula.returnFocus}
        />
      )}
      <ProgramPolicyEditor
        key={`${selected.instance_id}:${discardEpoch}`}
        node={selected}
        inheritedGroups={inheritedGroups}
        availableGroups={Object.keys(program.execution_groups ?? {}).sort()}
        onCreateGroup={onCreateGroup}
        busy={busy}
        onUpdate={onUpdateNodePolicies}
        onInvalidChange={(message) => setFieldError('_policy', message)}
      />
      {onUpdateNodePresentation && selected.children.length > 0 && (
        <label className="program-node-presentation">
          <input
            type="checkbox"
            checked={selected.metadata?.collapsed_by_default === true}
            disabled={busy}
            onChange={(event) => onUpdateNodePresentation(selected.instance_id, event.target.checked)}
          />
          Show collapsed by default
        </label>
      )}
    </section>
  );
}
