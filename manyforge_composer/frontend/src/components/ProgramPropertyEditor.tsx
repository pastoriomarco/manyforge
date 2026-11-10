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
} from '../types/api';
import type { ManualControlMotionDescriptor } from '../types/robotControl';
import type { ManualPreviewToNodeController } from '../hooks/useRobotControl';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import {
  IO_INPUT_NODE_IDS,
  ioEmptyOmits,
  ioSignalChoices,
  ioTimeoutHint,
  ioUnsupportedChoices,
} from '../utils/ioAuthoring';
import {
  globalRuleIds,
  interruptKeyInitialProblem,
  INTERRUPT_NODE_IDS,
  candidateStopStages,
  nodeConditionChoices,
  successThroughStages,
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
  programTcpSelectorChoices,
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
  ProgramManualFillPreviewProvider,
} from './property/ProgramSpecializedEditors';
import {
  SourceableValue,
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
import type { ReferenceIssue, ReferenceSelectorFacts } from '../utils/referenceValidation';
import { ProgramPolicyEditor } from './ProgramPolicyEditor';
import { StopStagePicker } from './StopStagePicker';
import { GraspLostPolicyEditor } from './GraspLostPolicyEditor';
import { TripPresetEditor } from './TripPresetEditor';
import { manualPreviewToNode, type ManualFillNotice } from '../utils/manualPreviewToNode';
import { manualFillPreviewValues as getManualFillPreviewValues } from '../utils/manualFillPreview';
import { observedMoveToNodePreview } from '../utils/observedMoveToNode';
import {
  directControlTcpDisabledReason,
  ROBOT_CONTROL_MODE_LABELS,
  robotControlPreviewApprovable,
  selectedTcpOf,
} from '../state/robotControl';

interface ProgramPropertyEditorProps {
  embedded?: boolean;
  roots: ProgramTreeNode[];
  selectedId: string | null;
  catalogEntries: CatalogEntry[];
  program: ProgramAuthoringDocument;
  appliedProgram?: AppliedProgramSelection | null;
  manualPreviewController?: ManualPreviewToNodeController | null;
  deploymentWorkspace?: DeploymentWorkspaceResponse | null;
  /** Outputs holding a core-held `changed outside` signal (`<component>/<signal>`). */
  changedOutsideOutputs?: ReadonlySet<string>;
  systemSignalsNeedApply?: boolean;
  motionDescriptors?: ManualControlMotionDescriptor[];
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
  referenceIssues?: ReferenceIssue[];
  selectorFacts?: ReferenceSelectorFacts | null;
}

function isProgramMotionField(field: string): field is ProgramMotionField {
  return PROGRAM_MOTION_FIELDS.includes(field as ProgramMotionField);
}

function isProgramMotionCeilingField(field: string): boolean {
  return PROGRAM_MOTION_CEILING_FIELDS.includes(
    field as typeof PROGRAM_MOTION_CEILING_FIELDS[number],
  );
}

type ManualFillSource = 'robot_pose' | 'move_preview';

function jsonObject(value: JsonValue | undefined): Record<string, JsonValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function programTargetReferenceValid(value: JsonValue | undefined): boolean {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    && typeof value.component_id === 'string' && typeof value.command_target_id === 'string';
}

function ResetArrowIcon() {
  return (
    <svg aria-hidden="true" width="14" height="14" viewBox="0 0 16 16" fill="none">
      <path d="M3.1 6.3A5.5 5.5 0 1 1 2.7 9" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      <path d="M2.8 2.8v3.8h3.8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
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
  if (node.node_id === 'motion.set_tcp' && path === 'tcp'
      && !Object.prototype.hasOwnProperty.call(node.parameters, 'tcp')) {
    return { transition: { kind: 'type_values', literal: { kind: 'current' } } };
  }
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
  manualPreviewController = null,
  deploymentWorkspace = null,
  systemSignalsNeedApply = false,
  motionDescriptors = [],
  busy,
  onMutate,
  onCreateReference,
  onCreateGroup,
  onRenamed,
  onUpdateNodePolicies,
  onUpdateNodePresentation,
  onInvalidEditChange,
  diagnostics = [],
  referenceIssues = [],
  selectorFacts = null,
}: ProgramPropertyEditorProps) {
  const programParameters = program.parameters ?? [];
  const deploymentDocument = deploymentWorkspace?.draft?.document ?? null;
  // The Deployment Draft's global rules add read-only `interrupt_state.<rule_id>` keys.
  const ruleIds = useMemo(
    () => globalRuleIds(deploymentDocument as Record<string, JsonValue> | null),
    [deploymentDocument],
  );
  const blackboardKeys = useMemo(() => programBlackboardKeys(program, ruleIds), [program, ruleIds]);
  const conditionChoices = useMemo(() => nodeConditionChoices(program, selectorFacts?.signals ?? null, ruleIds), [program, ruleIds, selectorFacts]);
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
  const [showOtherRobotTcps, setShowOtherRobotTcps] = useState(false);
  const [hoveredManualFill, setHoveredManualFill] = useState<ManualFillSource | null>(null);
  // A robot change waits for the operator's confirmation; what a copy changed stays shown.
  const [robotChangePending, setRobotChangePending] = useState<string | null>(null);
  const [fillNotices, setFillNotices] = useState<{ nodeId: string; notices: ManualFillNotice[] } | null>(null);
  const [focusedManualFill, setFocusedManualFill] = useState<ManualFillSource | null>(null);
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
  const manualPreviewCopy = useMemo(() => {
    if (manualPreviewController === null || selected === null) {
      return { ok: false as const, reason: 'Show a successful motion preview first.' };
    }
    const status = manualPreviewController.selectedStatus;
    const binding = appliedProgram !== null
      && appliedProgram.graphRevision === status?.graphRevision
      && appliedProgram.retainedBundleId === status?.retainedBundleId
      ? appliedProgram.programBinding : null;
    return manualPreviewToNode(
      manualPreviewController.state,
      status,
      selected,
      binding,
    );
  }, [appliedProgram, manualPreviewController, selected]);
  const manualPreviewCopyReason = (() => {
    if (!manualPreviewCopy.ok) return manualPreviewCopy.reason;
    if (busy) return 'Editing is unavailable.';
    if (manualPreviewController === null) return 'Show a successful motion preview first.';
    if (!manualPreviewController.manualControlAllowed) {
      return manualPreviewController.manualControlUnavailableReason ?? 'Manual control is unavailable.';
    }
    const state = manualPreviewController.state;
    const tcpBlocksCartesian = manualPreviewController.tcpApplying
      && (state.mode === 'pose_goal' || state.mode === 'cartesian_path'
        || (state.mode === 'move_to' && state.moveToKind === 'pose'));
    if (tcpBlocksCartesian) return 'The selected TCP is still applying.';
    if (manualPreviewController.authoringPending) return 'Finish the current manual control edit first.';
    const tcpRequired = state.mode !== 'joint' && state.mode !== 'gripper'
      && !(state.mode === 'move_to' && state.moveToKind === 'joint');
    const motion = manualPreviewController.selectedStatus?.directControl ?? null;
    const tcp = motion === null ? null : selectedTcpOf(motion);
    if (tcpRequired && tcp === null) return 'Select a TCP for this motion preview.';
    if (tcp !== null) {
      const reason = directControlTcpDisabledReason(tcp);
      if (reason !== null) return reason;
    }
    if (!robotControlPreviewApprovable(state) || state.poseDragging) {
      return 'Show a successful motion preview first.';
    }
    return null;
  })();
  const observedOwnerStatus = manualPreviewController?.currentStatus ?? null;
  const observedMovePreview = useMemo(() => observedMoveToNodePreview(
    observedOwnerStatus,
    appliedProgram,
    selected,
  ), [appliedProgram, observedOwnerStatus, selected]);
  const observedMovePreviewReason = busy
    ? 'Editing is unavailable.'
    : observedMovePreview.ok ? null : observedMovePreview.reason;
  const robotPoseActionUnavailable = busy
    ? 'Editing is unavailable.'
    : selected === null || !programTargetReferenceValid(selected.parameters.target)
      ? 'Choose the target first.'
      : null;
  const activeManualFill = hoveredManualFill ?? focusedManualFill;
  const summaryManualFill = activeManualFill ?? 'move_preview';
  const manualFillPreviewFor = useCallback((source: ManualFillSource) => {
    if (selected === null) return new Map<string, string>();
    if (source === 'robot_pose') {
      return observedMovePreview.ok && observedMovePreviewReason === null
        ? getManualFillPreviewValues(selected, observedMovePreview.parameters, observedMovePreview.valueSources)
        : new Map<string, string>();
    }
    if (!manualPreviewCopy.ok || manualPreviewCopyReason !== null) return new Map<string, string>();
    return getManualFillPreviewValues(
      selected,
      jsonObject(manualPreviewCopy.args.parameters),
      jsonObject(manualPreviewCopy.args.value_sources),
    );
  }, [manualPreviewCopy, manualPreviewCopyReason, observedMovePreview,
    observedMovePreviewReason, selected]);
  const manualFillPreviewValues = useMemo(() => activeManualFill === null
    ? new Map<string, string>() : manualFillPreviewFor(activeManualFill),
  [activeManualFill, manualFillPreviewFor]);
  const summaryFillPreviewValues = useMemo(
    () => manualFillPreviewFor(summaryManualFill),
    [manualFillPreviewFor, summaryManualFill],
  );
  const activeFillPreviewReason = summaryManualFill === 'robot_pose'
    ? observedMovePreviewReason : manualPreviewCopyReason;
  const activeFillPreviewSummary = (() => {
    if (activeFillPreviewReason !== null) return activeFillPreviewReason;
    const count = summaryFillPreviewValues.size;
    const fieldCount = `${count} field${count === 1 ? '' : 's'} ${count === 1 ? 'changes' : 'change'}`;
    if (summaryManualFill === 'robot_pose' && observedMovePreview.ok) {
      return `Robot ${observedMovePreview.kind === 'joint' ? 'joints' : 'pose'} preview · ${observedMovePreview.referenceSummary} · ${fieldCount}`;
    }
    const state = manualPreviewController?.state;
    const mode = state ? ROBOT_CONTROL_MODE_LABELS[state.mode] : 'Motion';
    const reference = state?.mode === 'move_to'
      ? state.moveToDraft?.source.id ?? (state.moveToKind === 'pose' ? 'pose target' : 'joint target')
      : state?.mode === 'joint' ? 'current joint positions' : 'current TCP pose';
    return `${mode} preview · ${reference} · ${fieldCount}`;
  })();
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
  const stopWarnings = useMemo(() => (selected === null ? [] : precisionContactStopWarnings(
    selected, deploymentDocument as Record<string, JsonValue> | null,
  )), [deploymentDocument, selected]);
  // The bound robot's qualified Stop chain; null keeps "available after Apply".
  const movingTargetValue = selected?.parameters.target;
  const movingTarget = movingTargetValue !== null && typeof movingTargetValue === 'object'
    && !Array.isArray(movingTargetValue) && typeof movingTargetValue.component_id === 'string'
    && typeof movingTargetValue.command_target_id === 'string'
    ? { component_id: movingTargetValue.component_id, command_target_id: movingTargetValue.command_target_id } : null;
  const stopCapability = entry?.requiredCapabilities.find((requirement) =>
    requirement.targetParameter === 'target' && requirement.capabilityId === 'execution.trajectory.execute')?.capabilityId ?? null;
  const stopResolution = candidateStopStages(movingTarget, stopCapability, selectorFacts?.targets ?? null);
  const qualifiedStopStages = stopResolution.stages;
  const selectedDeploymentSpeed = useMemo(() => {
    if (selected === null) return null;
    const target = selected.parameters.target;
    if (target === null || typeof target !== 'object' || Array.isArray(target)
      || typeof target.component_id !== 'string' || typeof target.command_target_id !== 'string') return null;
    const descriptor = motionDescriptors.find(
      (candidate) => candidate.controllableComponentId === target.component_id
        && candidate.commandTargetId === target.command_target_id,
    );
    if (descriptor?.operationalLimit === null || descriptor?.operationalLimit === undefined) {
      return null;
    }
    return {
      target: `${target.component_id}/${target.command_target_id}`,
      maximum: descriptor.operationalLimit.maximumTcpLinearSpeedMps,
    };
  }, [motionDescriptors, selected]);
  const sourceJointInventory = useMemo(() => selected === null
    ? null
    : programSourceJointInventory(selected, selectorFacts?.targets ?? null, 'The qualified Draft candidate is unavailable.'), [selected, selectorFacts]);
  const manualMotion = manualPreviewController?.selectedStatus?.directControl ?? null;
  const activeTcpNow = manualMotion !== null && movingTarget !== null
    && manualMotion.controllableComponentId === movingTarget.component_id
    && manualMotion.commandTargetId === movingTarget.command_target_id
    ? manualMotion.activeTcp?.display ?? null : null;
  const motionEndpointChoices = programTcpSelectorChoices(
    movingTarget, selectorFacts?.targets ?? null, showOtherRobotTcps, activeTcpNow);
  useEffect(() => {
    setErrors({});
    setShowOtherRobotTcps(false);
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

  // Candidate-qualified ActiveTCP keys are supplied when the Draft result is available.
  const tcpSourceKeys = [...blackboardKeys];
  const valueSourceModel = (path: string, schema: CatalogJsonSchema) => valueSourceMenuModel({
    entry,
    path,
    schema,
    sources: selected.value_sources,
    literals: { ...catalogDefaultParameters(entry), ...selected.parameters },
    parameters: programParameters,
    keys: entry.id === 'motion.set_tcp' && path === 'tcp' ? tcpSourceKeys : blackboardKeys,
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
      const row = (selected.node_id === 'motion.set_tcp' ? tcpSourceKeys : blackboardKeys)
        .find((candidate) => candidate.key === source.reference);
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
      <TripPresetEditor
        // Distinct from ProgramPolicyEditor's key: duplicate sibling keys leave stale cards mounted.
        key={`trip:${selected.instance_id}:${discardEpoch}`}
        node={selected}
        keyContext={{
          entry, node: selected, parameterId: 'on_grasp_lost', program, globalRuleIds: ruleIds, onCreateReference,
        }}
        disabled={busy}
        invalidEdit={hasInvalidEdit}
        onReplace={(replacement) => {
          // The preset's parameter and recovery policy change in one Draft edit.
          void onMutate('tree_draft_replace_subtree', {
            instance_id: selected.instance_id,
            subtree: replacement as unknown as JsonValue,
          });
        }}
      />
      <ValueSourceContext.Provider value={sourceController}>
      <section className="dcard" aria-label="Parameters">
        <h4>Parameters</h4>
        {entry.parameters.length === 0 && (
          <p className="muted">This node has no authored parameters.</p>
        )}
        {entry.parameters.map((parameter) => {
          const hasMotionTcpEndpoint = parameter.id === 'tcp_endpoint'
            || (selected.node_id === 'motion.set_tcp' && parameter.id === 'tcp');
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
          const parameterReferenceIssues = referenceIssues.filter((issue) => (
            issue.location.node_instance_id === selected.instance_id && issue.location.parameter === parameter.id
          ));
          const requiredCapabilities = entry.requiredCapabilities.filter((requirement) =>
            requirement.scope === 'target_binding' && requirement.targetParameter === parameter.id
            && (requirement.whenParameter === undefined
              || (selected.parameters[requirement.whenParameter]
                ?? entry.parameters.find((item) => item.id === requirement.whenParameter)?.default) === requirement.whenEquals));
          const selectedTarget = selected.parameters.target;
          const isMovingTarget = (candidate: {component_id: string; command_target_id: string}) => (
            selectedTarget !== null && typeof selectedTarget === 'object' && !Array.isArray(selectedTarget)
            && candidate.component_id === selectedTarget.component_id
            && candidate.command_target_id === selectedTarget.command_target_id
          );
          const targetChoices = (selectorFacts?.targets ?? []).map((row) => {
            const unavailable = requiredCapabilities.map((requirement) => {
              const capability = row.capabilities.find((item) => item.capability_id === requirement.capabilityId);
              return capability?.available ? null : capability?.reason ?? `Missing ${requirement.capabilityId}`;
            }).find((reason) => reason !== null);
            return { value: { ...row.target }, label: `${row.target.component_id} · ${row.target.command_target_id}`,
              ...(unavailable ? { reason: unavailable } : {}) };
          });
          const frameFacts = selectorFacts?.frames ?? [];
          const bodyFacts = selectorFacts?.bodies ?? [];
          const ioChoices = ioSignalChoices(selected.node_id, selectorFacts?.signals ?? null);
          const frameNeedsTarget = parameter.id === 'base_frame';
          // A carried object's touch bodies: those moving with the selected target (its own and its mounted tools').
          const bodyNeedsTarget = parameter.id === 'touch_bodies' && movingTarget !== null;
          const movingFrames = frameFacts.filter((row) => !row.ambiguous && row.moving_targets.some(isMovingTarget)).map((row) => row.reference);
          const directReferenceChoices = {
            target_reference: targetChoices,
            frame_reference: frameFacts.map((row) => ({ value: { ...row.reference },
              label: `${row.reference.component_id} · ${row.reference.frame_id}`,
              ...(row.ambiguous ? { reason: 'Ambiguous frame identity' }
                : frameNeedsTarget && !row.moving_targets.some(isMovingTarget) ? { reason: 'Frame is not qualified for the selected target' } : {}) })),
            collision_body_reference: bodyFacts.filter((row) => !bodyNeedsTarget || row.moving_targets.some(isMovingTarget))
              .map((row) => ({ value: { ...row.reference },
              label: `${row.reference.component_id} · ${row.reference.collision_body_id}`,
              ...(row.ambiguous ? { reason: 'Ambiguous collision body identity' } : {}) })),
            signal_reference: (selectorFacts?.signals ?? []).map((row) => {
              const input = ['io.get_input', 'io.check_input', 'io.wait_for_input'].includes(selected.node_id);
              const ioChoice = ioChoices?.find((choice) => choice.value.component_id === row.reference.component_id
                && choice.value.signal_name === row.reference.signal_name);
              const signalTarget = row.target && selectorFacts?.targets.find((candidate) => (
                candidate.target.component_id === row.target?.component_id
                && candidate.target.command_target_id === row.target?.command_target_id
              ));
              const capabilityIssue = (input && row.provider_profile === 'system_carried_object' ? [] : requiredCapabilities).map((requirement) => {
                const capability = signalTarget?.capabilities.find((item) => item.capability_id === requirement.capabilityId);
                return capability?.available ? null : capability?.reason ?? `Missing ${requirement.capabilityId}`;
              }).find((issue) => issue !== null);
              const reason = row.ambiguous ? 'Ambiguous signal identity'
                : ioChoice?.reason ?? capabilityIssue ?? undefined;
              return { value: { ...row.reference }, label: `${row.reference.component_id} · ${row.reference.signal_name}`,
                ...(reason ? { reason } : {}) };
            }),
          };
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
          // Catalog schema type `mock_signal_simulation`: literal rows only.
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
          const parameterLabel = hasMotionTcpEndpoint && parameter.id === 'tcp_endpoint'
            ? 'Motion TCP endpoint'
            : parameter.id === 'max_tcp_linear_speed_mps'
            ? 'TCP speed ceiling (m/s)'
            : parameter.id === 'clearance_m'
            ? 'Collision clearance (m)'
            : parameter.id === 'wait_for_duration'
            ? 'Wait for command duration'
            : parameter.id;
          const parameterPreviewValue = manualFillPreviewValues.get(parameter.id);
          const resetLabel = isProgramMotionField(parameter.id)
            && motionBindings?.[parameter.id] !== undefined
            ? 'Use Program default' : 'Use catalog default / omit';
          const showReset = !source && (!parameter.required || parameter.hasDefault);
          const valueEditorContext: ProgramSchemaValueEditorContext = {
            entry,
            node: selected,
            parameterId: parameter.id,
            program,
            directReferenceChoices,
            frameScope: {
              program,
              frames: frameFacts.filter((row) => !row.ambiguous && (!frameNeedsTarget || row.moving_targets.some(isMovingTarget))).map((row) => row.reference),
              movingFrames,
              unavailableFrames: frameFacts.filter((row) => row.ambiguous || (frameNeedsTarget && !row.moving_targets.some(isMovingTarget)))
                .map((row) => ({ reference: row.reference, reason: row.ambiguous ? 'Ambiguous frame identity' : 'Frame is not qualified for the selected target' })),
              bodies: bodyFacts.filter((row) => !row.ambiguous).map((row) => row.reference),
            },
            globalRuleIds: ruleIds,
            sourceJointInventory,
            onCreateReference,
            unsupportedChoices,
            emptyOmits: emptyOmits
              ? { placeholder: 'Wait forever', onOmit: () => removeLiteral(parameter) }
              : undefined,
          };
          const storedText = (id: string) => (typeof selected.parameters[id] === 'string'
            ? selected.parameters[id] as string : undefined);
          return (
            <div
              className={`program-property${backendDiagnostic || parameterReferenceIssues.length ? ' backend-invalid' : ''}${parameterPreviewValue === undefined ? '' : ' program-manual-fill-changed'}`}
              title={backendDiagnostic?.message}
              key={`${parameter.id}:${discardEpoch}`}
            >
              <div className="program-property-heading">
                <label
                  htmlFor={`property-${selected.instance_id}-${parameter.id}`}
                  className={parameterPreviewValue === undefined ? '' : 'program-manual-fill-changed'}
                >
                  {parameterLabel}
                  {parameterPreviewValue !== undefined && (
                    <span className="program-manual-fill-new-value" aria-hidden="true">
                      {' '}→ {parameterPreviewValue}
                    </span>
                  )}
                </label>
                <span>{parameter.required ? 'required' : 'optional'}</span>
                {showReset && (
                  <button
                    type="button"
                    className="icon-button"
                    aria-label={resetLabel}
                    title={resetLabel}
                    disabled={busy || rule.locked !== undefined || selected.parameters[parameter.id] === undefined}
                    onClick={() => removeLiteral(parameter)}
                  >
                    <ResetArrowIcon />
                  </button>
                )}
              </div>
              {parameterReferenceIssues.map((issue, index) => (
                <p className="program-form-error inline-message severity-error" key={`${issue.code}:${issue.location.path}:${index}`}>
                  {issue.code.replaceAll('_', ' ')}
                </p>
              ))}
              {parameter.id === 'max_tcp_linear_speed_mps' && (
                <p className="muted">
                  {selectedDeploymentSpeed === null
                    ? 'Optional speed ceiling; the backend uses the lower of this value and the applied Deployment target maximum.'
                    : `Currently applied Deployment maximum for ${selectedDeploymentSpeed.target}: ${selectedDeploymentSpeed.maximum} m/s. ${typeof displayedSpeed === 'number' && Number.isFinite(displayedSpeed) && displayedSpeed > 0 ? `With ${source === undefined ? 'request' : 'parameter default'} ${displayedSpeed} m/s, the effective ceiling is ${Math.min(displayedSpeed, selectedDeploymentSpeed.maximum)} m/s. ` : 'The backend resolves the effective ceiling when the request is known. '}The authored value is unchanged.`}
                </p>
              )}
              {parameter.id === 'clearance_m' && (
                <p className="muted">
                  Optional. Omitted, the move keeps the Deployment clearance (0.001 m unless the Deployment sets one); 0 turns the clearance off for this move.
                </p>
              )}
              {parameter.id === 'wait_for_duration' && (
                <p className="muted">
                  Off: the command completes when the gripper controller reports done. On: it also waits until command_duration_s has passed since the command started. Pneumatic grippers always wait for their dwell.
                </p>
              )}
              {parameter.id === 'blend_radius_m' && (
                <p className="muted">
                  Corner blend radius (m). 0 keeps exact corners; the tool stops at sharp corners. A waypoint may set its own radius.
                </p>
              )}
              {parameter.id === 'cartesian_timing' && (
                <p className="muted">
                  Optional. Omitted, the path uses the Deployment&apos;s Cartesian timing.
                </p>
              )}
              {rule.inapplicable !== undefined && (
                <p className="program-form-hint">{rule.inapplicable} Omit it below.</p>
              )}
              {systemSignalsNeedApply && parameter.id === 'signal' && IO_INPUT_NODE_IDS.has(selected.node_id) && (
                <p className="program-form-hint">Qualified Draft system signals are unavailable.</p>
              )}
              {parameter.id === 'signal' && selected.node_id !== 'io.set_output'
                && conditionChoices.some((choice) => choice.liveness === 'change_only'
                  && JSON.stringify(choice.operand) === JSON.stringify({ signal: selected.parameters.signal ?? null })) && (
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
                const declaredKeys = movingTarget === null ? [] : declaredPneumaticInputs(
                  deploymentDocument as Record<string, JsonValue> | null,
                  movingTarget.component_id, movingTarget.command_target_id,
                );
                return <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <PneumaticRequiredInputsEditor value={literal}
                    declaredKeys={declaredKeys}
                    disabled={busy || rule.locked !== undefined || declaredKeys.length === 0}
                    onCommit={(value) => commitLiteral(parameter, value)} />
                  {declaredKeys.length === 0 && (
                    <p className="program-form-hint">Pneumatic input choices require qualified Draft candidate routes.</p>
                  )}
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
                          profiles={mockSignalProfiles(signals, selectorFacts?.signals ?? null)}
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
                    <p className="program-form-hint">Qualified Draft system signals are unavailable.</p>
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
                  {stopResolution.reason && <p className="program-form-hint">{stopResolution.reason}</p>}
                  {stopResolution.routes.length > 1 || stopResolution.routes.some((route) => !route.available) ? <ul className="program-form-hint">
                    {stopResolution.routes.map((route) => <li key={`${route.control_binding_id}:${route.control_binding_revision}:${route.control_route_id}`}>
                      {route.control_binding_id} · {route.control_binding_revision} · {route.control_route_id}: {route.available ? 'Available' : route.reason?.message ?? 'Unavailable'}
                      {route.reason && <> ({route.reason.code}){Object.entries(route.reason.details).map(([key, value]) => <span key={key}> · {key}: {value}</span>)}</>}
                    </li>)}
                  </ul> : null}
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
              ) : hasMotionTcpEndpoint ? (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <SourceableValue path={parameter.id} schema={schema}>
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
                          const request = buildMotionEndpointMutation(selected, null, parameter.id);
                          void onMutate(request.mutation, request.args);
                        }}
                      >Remove dynamic endpoint value</button>
                    </>
                  ) : (() => {
                    const currentKey = motionEndpointKeyOfNode(selected, parameter.id) ?? '';
                    const currentIsDeclared = motionEndpointChoices.some(
                      (choice) => choice.key === currentKey,
                    );
                    const currentLabel = 'Current endpoint is not declared';
                    return (
                      <>
                        {parameter.id === 'tcp_endpoint' && (
                          <p className="program-form-hint">{`Moves: ${movingTarget === null ? '—' : `${movingTarget.component_id} · ${movingTarget.command_target_id}`}`}</p>
                        )}
                        <select
                          value={currentKey}
                          disabled={busy || rule.locked !== undefined}
                          aria-label="Motion TCP endpoint"
                          onChange={(event) => {
                            const choice = motionEndpointChoices.find(
                              (candidate) => candidate.key === event.currentTarget.value,
                            );
                            if (!choice || choice.disabled) return;
                            const request = buildMotionEndpointMutation(
                              selected, choice.selector, parameter.id,
                            );
                            void onMutate(request.mutation, request.args);
                          }}
                        >
                          {currentKey === '' && <option value="">Select a TCP</option>}
                          {currentKey !== '' && !currentIsDeclared && (
                            <option value={currentKey}>{currentLabel}</option>
                          )}
                          {motionEndpointChoices.map((choice) => (
                            <option key={choice.key} value={choice.key} disabled={choice.disabled}>
                              {choice.label}{choice.reason ? ` · ${choice.reason}` : ''}
                            </option>
                          ))}
                        </select>
                        {selected.node_id === 'motion.set_tcp' && (
                          <button type="button" disabled={busy} onClick={(event) => (
                            sourceController.openMenu(parameter.id, schema, event.currentTarget, null)
                          )}>Use a TCP key…</button>
                        )}
                        <label title="Useful only once tool change is implemented: today a TCP whose tool is on another robot is refused when used.">
                          <input type="checkbox" checked={showOtherRobotTcps}
                            onChange={(event) => setShowOtherRobotTcps(event.currentTarget.checked)} />
                          Show TCPs of tools mounted on other robots
                        </label>
                        {selectorFacts === null && (
                          <p className="program-form-hint">Validate the Deployment Draft to list qualified TCPs.</p>
                        )}
                        <p className="program-form-hint">
                          Current uses the robot's Active TCP. A target default or an entry sets the Active TCP to that TCP.
                        </p>
                      </>
                    );
                  })()}
                  </SourceableValue>
                </div>
              ) : (
                <div id={`property-${selected.instance_id}-${parameter.id}`}>
                  <ProgramManualFillPreviewProvider values={manualFillPreviewValues}>
                    <ProgramSchemaValueEditor
                      schema={schema}
                      value={literal}
                      disabled={busy || rule.locked !== undefined}
                      context={valueEditorContext}
                      path={parameter.id}
                      onCommit={(value, moves) => commitLiteral(parameter, value, moves)}
                      onInvalidChange={(message) => setFieldError(parameter.id, message)}
                    />
                  </ProgramManualFillPreviewProvider>
                </div>
              )}
              {parameter.id === 'goal'
                && (selected.node_id === 'motion.move_to_pose'
                  || selected.node_id === 'motion.move_to_joint_target') && (() => {
                const pose = selected.node_id === 'motion.move_to_pose';
                const target = selected.parameters.target;
                const targetValid = target !== null && typeof target === 'object' && !Array.isArray(target)
                  && typeof target.component_id === 'string' && typeof target.command_target_id === 'string';
                const relative = pose && selected.parameters.target_mode === 'relative';
                const robotPoseLabel = pose ? 'Robot pose' : 'Robot joints';
                const robotPoseMutation = observedMovePreview.ok
                  ? observedMovePreview.mutation
                  : { instance_id: selected.instance_id, observed_kind: pose ? 'tcp_pose' : 'joint' };
                const robotPoseTitle = robotPoseActionUnavailable ?? (
                  `Write the ${pose ? 'current TCP pose' : 'current joint positions'} of `
                  + `${targetValid ? `${target.component_id}/${target.command_target_id}` : 'the selected target'} into this goal`
                  + (pose ? '. Replaces the goal frame with this robot’s base frame' : '')
                  + (pose && selected.parameters.tcp_endpoint !== undefined
                    ? '. The pose is the selected TCP’s, which must be one of the robot’s Applied TCPs'
                    : '')
                  + (relative ? '. This switches the move to absolute.' : '.')
                );
                const movePreviewTitle = manualPreviewCopyReason
                  ?? `Fill ${selected.instance_id} from the current manual motion preview.`;
                return (
                  <div className="program-fill-from" data-testid="program-fill-from">
                    <div className="program-fill-from-topline">
                      <span className="program-fill-from-label">FILL FROM</span>
                      <div className="program-fill-from-actions">
                        <button
                          type="button"
                          className="program-fill-action-button"
                          data-testid="fill-from-robot-pose"
                          disabled={robotPoseActionUnavailable !== null}
                          title={robotPoseTitle}
                          onMouseEnter={() => setHoveredManualFill('robot_pose')}
                          onMouseLeave={() => setHoveredManualFill((current) => current === 'robot_pose' ? null : current)}
                          onFocus={() => setFocusedManualFill('robot_pose')}
                          onBlur={() => setFocusedManualFill((current) => current === 'robot_pose' ? null : current)}
                          onClick={() => {
                            void onMutate('program_draft_store_observed_move', robotPoseMutation);
                          }}
                        >
                          <svg aria-hidden="true" width="14" height="14" viewBox="0 0 16 16" fill="none">
                            <circle cx="8" cy="8" r="4.3" stroke="currentColor" strokeWidth="1.4" />
                            <path d="M8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
                          </svg>
                          {robotPoseLabel}
                        </button>
                        <button
                          type="button"
                          className="program-fill-action-button"
                          data-testid="fill-from-move-preview"
                          disabled={manualPreviewCopyReason !== null}
                          title={movePreviewTitle}
                          onMouseEnter={() => setHoveredManualFill('move_preview')}
                          onMouseLeave={() => setHoveredManualFill((current) => current === 'move_preview' ? null : current)}
                          onFocus={() => setFocusedManualFill('move_preview')}
                          onBlur={() => setFocusedManualFill((current) => current === 'move_preview' ? null : current)}
                          onClick={() => {
                            if (!manualPreviewCopy.ok || manualPreviewCopyReason !== null) return;
                            if (manualPreviewCopy.robotChange !== null) {
                              setRobotChangePending(selected.instance_id);
                              return;
                            }
                            setFillNotices({ nodeId: selected.instance_id, notices: manualPreviewCopy.notices });
                            void onMutate('tree_draft_update_node_params', manualPreviewCopy.args);
                          }}
                        >
                          <svg aria-hidden="true" width="14" height="14" viewBox="0 0 16 16" fill="none">
                            <path d="M2 11.8 5.5 8l2.2 1.8L12.9 4" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
                            <circle cx="2" cy="11.8" r="1" fill="currentColor" />
                            <circle cx="5.5" cy="8" r="1" fill="currentColor" />
                            <circle cx="7.7" cy="9.8" r="1" fill="currentColor" />
                            <circle cx="12.9" cy="4" r="1" fill="currentColor" />
                          </svg>
                          Move preview
                        </button>
                      </div>
                    </div>
                    <div className={`program-fill-from-preview${activeFillPreviewReason === null ? ' compatible' : ''}`}>
                      {activeFillPreviewSummary}
                    </div>
                    {robotChangePending === selected.instance_id && manualPreviewCopy.ok
                      && manualPreviewCopy.robotChange !== null && (
                      <div className="program-fill-from-confirm" role="alertdialog" aria-label="Confirm robot change">
                        <p>
                          This node moves <strong>{manualPreviewCopy.robotChange.from}</strong>; the preview
                          moves <strong>{manualPreviewCopy.robotChange.to}</strong>. Copy the move and switch the
                          node to {manualPreviewCopy.robotChange.to}?
                        </p>
                        <div className="program-fill-from-confirm-actions">
                          <button
                            type="button"
                            onClick={() => {
                              setRobotChangePending(null);
                              if (!manualPreviewCopy.ok || manualPreviewCopyReason !== null) return;
                              setFillNotices({ nodeId: selected.instance_id, notices: manualPreviewCopy.notices });
                              void onMutate('tree_draft_update_node_params', manualPreviewCopy.args);
                            }}
                          >
                            Switch robot and copy
                          </button>
                          <button type="button" onClick={() => setRobotChangePending(null)}>Cancel</button>
                        </div>
                      </div>
                    )}
                    {(() => {
                      const notices = fillNotices?.nodeId === selected.instance_id
                        ? fillNotices.notices
                        : manualPreviewCopy.ok && manualPreviewCopyReason === null ? manualPreviewCopy.notices : [];
                      return notices.length > 0 && (
                        <ul className="program-fill-from-notices" data-testid="fill-from-notices">
                          {notices.map((notice) => (
                            <li key={notice.text} className={notice.tone}>{notice.text}</li>
                          ))}
                        </ul>
                      );
                    })()}
                  </div>
                );
              })()}
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
