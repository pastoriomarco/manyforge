import { useReferenceValidation } from './hooks/useReferenceValidation';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { DirectControlMotionStatus } from './types/robotControl';
import type { CSSProperties } from 'react';

import { ApiError, api, apiErrorCode } from './api/client';
import { parseLogicalSceneProjection } from './api/parsers';
import {
  AssistantOverlay,
  type AssistantModelSummary,
  NO_ASSISTANT_COMPONENT,
  assistantAvailability,
} from './components/AssistantOverlay';
import { ComponentsWorkspace } from './components/ComponentsWorkspace';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { deploymentDocumentHeader } from './components/DeploymentDocumentControls';
import {
  HeadlessRuntimeSurfaces,
  errorAlarms,
  runtimeErrorItemCount,
  warningAlarms,
} from './components/HeadlessRuntimeSurfaces';
import {
  RuntimeAttentionAutoOpen,
  shouldCloseAutomaticRuntimeAttention,
} from './components/RuntimeAttentionAutoOpen';
import { DocumentControls } from './components/DocumentControls';
import { programDocumentHeader } from './components/ProgramDocumentControls';
import { ScenarioDocumentCommands } from './components/ScenarioDocumentCommands';
import { DraftInputResetProvider } from './components/common/DraftInputs';
import { PaneDivider } from './components/PaneDivider';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { componentsTabStatus, runtimeStatusPresentation } from './components/runtimeStatus';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import { WorkbenchNotificationStack } from './components/WorkbenchNotifications';
import { useExactAlarmAcknowledgement } from './hooks/useExactAlarmAcknowledgement';
import {
  useCellState,
  useHeadlessStateSource,
  useShellHeadlessState,
} from './hooks/useHeadlessState';
import { useAuthoringWorkspace } from './hooks/useAuthoringWorkspace';
import { useDeploymentWorkspace } from './hooks/useDeploymentWorkspace';
import { aiModelComponents } from './components/AiModelComponentForm';
import { useComponentCatalog } from './hooks/useComponentCatalog';
import { requestOpenSettings } from './state/settingsNavigation';
import type { ComponentManagerTab } from './utils/componentManager';
import { useManagerApplyStatus } from './hooks/useManagerApplyStatus';
import { activationFailureNotice } from './state/activationNotices';
import { isaacJointStreamNotices } from './state/isaacJointStreamNotices';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useProgramWorkspace } from './hooks/useProgramWorkspace';
import { useRobotControl } from './hooks/useRobotControl';
import {
  robotControlHasActiveOperation,
  robotControlHasTransientPreviewRefusal,
  type RobotControlState,
} from './state/robotControl';
import {
  AuthoringAdmissionTooltipProvider,
  authoringAdmissionTooltip,
  replacementAdmissionAllowed,
  manualOperationAdmissionCopy,
  type ManualOperationAdmissionCopy,
} from './components/authoringAdmission';
import { useHeldNodePreview } from './hooks/useHeldNodePreview';
import { useResetApply } from './hooks/useResetApply';
import { useAppliedSettlementRefresh } from './hooks/useAppliedSettlementRefresh';
import { appliedCartesianDefaults } from './state/robotControl';
import {
  managerActivationSettlementKey,
  nodeEnvironmentRefreshKey,
  refreshOnManagerActivationSettled,
  managerReadinessTransientRetryKey,
  refreshTransientResourcesOnManagerReadiness,
  refreshViewerModelsOnManagerChange,
  isTransientManagerReadinessErrorCode,
  type ReadinessTransientRefreshAttempts,
  viewerModelRefreshKey,
} from './state/nodeEnvironmentRefresh';
import { useStructuralResource } from './state/structuralResource';
import { deriveProgramMotionConfiguration, programBlackboardKeys } from './utils/programAuthoring';
import { globalRuleIds } from './utils/interruptAuthoring';
import { isRuleHoldsRefusal } from './state/commandRefusal';
import { CARRIED_OBJECT_LOST_SIGNAL_PREFIX } from './utils/systemSignalAuthoring';
import {
  useOperationalErrorMirror,
  useOperationalErrors,
} from './state/operationalErrors';
import {
  authoringContainmentNotices,
  authoringUnavailableCause,
  requiredComponentFaultNotices,
} from './state/authoringWorkspace';
import {
  managerApplySettlementKey,
  sameManagerApply,
  type ManagerApplyIdentity,
} from './state/managerApply';
import {
  deploymentDocumentStates,
  deploymentUnapplied,
  programDocumentStates,
} from './state/documentStates';
import { DocumentStateLetters } from './components/DocumentStateLetters';
import { ScenarioActionControls } from './components/ScenarioActionControls';
import { useComposerSettingsNotices } from './components/ComposerSettingsPanel';
import {
  previousSessionAlarmHandoff,
  activeViewerIncidentKinds,
  classifyWorkbenchNotices,
  useTransitionScope,
  useViewerConditionHold,
  noticesOfSeverity,
  useBriefNotice,
  usePublishedWorkbenchNotices,
  type ViewerIncidentKind,
  type WorkbenchNotice,
} from './state/workbenchNotices';
import { startupScenarioNotices } from './state/startupScenarioNotice';
import { initialSceneNotice } from './state/initialSceneNotice';
import {
  onOpenGlobalRulesRequest,
  takeOpenGlobalRulesRequest,
} from './state/globalRulesNavigation';
import {
  alarmWarningNotices,
  composerWarningNotices,
  locallyAcknowledgedWarnings,
  resetReachedIdle,
  viewerConditionWarning,
} from './state/warningNotices';
import type { ProgramWorkspaceActionName } from './state/programWorkspace';
import {
  INSPECTOR_WIDTH_MIN,
  WORKBENCH_DIVIDER_WIDTH,
  inspectorWidthMax as centeredInspectorWidthMax,
} from './state/workbenchLayout';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

function isAssistantProfileAbsent(error: unknown): boolean {
  return apiErrorCode(error) === 'assistant_mode_projection_unavailable';
}

function isTransientManagerReadinessError(error: unknown): boolean {
  return isTransientManagerReadinessErrorCode(apiErrorCode(error));
}

const INSPECTOR_SECTION_MIN_HEIGHT = 140;

const NO_MOTION_TARGETS: readonly DirectControlMotionStatus[] = [];

const PROGRAM_ACTION_LABELS: Partial<Record<ProgramWorkspaceActionName, string>> = {
  new_scenario_document: 'Opening the Untitled Scenario',
  open_draft: 'Opening Program as Draft',
  apply: 'Submitting Program Apply',
  discard: 'Discarding Program changes',
  discard_scenario: 'Discarding Scenario changes',
  revert: 'Submitting Program Revert',
  load: 'Loading Program',
  load_scenario_document: 'Submitting Scenario Load',
  export_scenario_document: 'Exporting Scenario',
  save: 'Saving Program',
  save_as: 'Saving Program as',
  save_scenario_document: 'Saving Scenario',
};

/** Report browser-only viewer errors once on activation and once on resolution. */
function useViewerIncidentReporting(
  errorConditions: readonly WorkbenchNotice[],
  report: ((
    kind: ViewerIncidentKind,
    active: boolean,
    reasonCode: string,
    observedDelayNs?: number | null,
    startedMs?: number,
  ) => void) | undefined,
) {
  const reported = useRef(new Set<ViewerIncidentKind>());
  const kinds = activeViewerIncidentKinds(errorConditions);
  const signature = kinds.join(',');
  // When each kind began (the held condition's onset), so the record's delay
  // covers the whole occurrence, not only the part after the hold.
  const onsets = JSON.stringify(kinds.map((kind) => errorConditions
    .find((notice) => notice.incidentKind === kind)?.firstObservedMs ?? null));
  useEffect(() => {
    if (report === undefined) return;
    const active = signature === '' ? [] : signature.split(',') as ViewerIncidentKind[];
    const startedAt = JSON.parse(onsets) as Array<number | null>;
    active.forEach((kind, index) => {
      if (!reported.current.has(kind)) {
        reported.current.add(kind);
        report(kind, true, kind, null, startedAt[index] ?? undefined);
      }
    });
    for (const kind of [...reported.current]) {
      if (!active.includes(kind)) {
        reported.current.delete(kind);
        report(kind, false, kind);
      }
    }
    // Onsets only matter when a kind first activates.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [report, signature]);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export function authoringAdmissionNotices(
  authoringAllowed: boolean | null | undefined,
  manualOperation: ManualOperationAdmissionCopy | null = null,
  backgroundStep = false,
): WorkbenchNotice[] {
  if (manualOperation !== null) return [{
    id: `authoring-admission.manual.${manualOperation.ownerEpoch}.${manualOperation.operationId}`,
    severity: 'info',
    domain: 'program_authoring',
    title: manualOperation.title,
    message: manualOperation.message,
    busy: true,
  }];
  return authoringAllowed === false && backgroundStep ? [{
    id: 'authoring-admission',
    severity: 'info',
    domain: 'program_authoring',
    title: 'The cell is finishing a background step.',
    message: '',
    busy: true,
  }] : [];
}

export function manualPreviewRefusalNotice(
  state: RobotControlState,
): WorkbenchNotice | null {
  if (!robotControlHasTransientPreviewRefusal(state)) return null;
  const ownerEpoch = state.status?.ownerEpoch ?? 'unknown-owner';
  return {
    id: `robot-control.preview.${ownerEpoch}.${state.generation}`,
    severity: 'info',
    domain: 'runtime',
    title: 'Manual preview refused',
    message: state.previewIssue ?? state.error ?? 'The manual motion preview was refused.',
  };
}

export default function App() {
  const [activeWorkspace, setActiveWorkspace] = useState<'program' | 'scene' | 'components' | 'deployment'>('program');
  const [selectedProgramNode, setSelectedProgramNode] = useState<{draftId: string; id: string | null} | null>(null);
  const [deploymentTabRequest, setDeploymentTabRequest] = useState<{
    tab: ComponentManagerTab;
    sequence: number;
  } | null>(null);
  const [inspectorWidth, setInspectorWidth] = useState(320);
  const [workbenchWidth, setWorkbenchWidth] = useState(() => (
    typeof window === 'undefined' ? 1440 : window.innerWidth
  ));
  const [sceneEditorTarget, setSceneEditorTarget] = useState<HTMLElement | null>(null);
  const [programInspectorTarget, setProgramInspectorTarget] = useState<HTMLElement | null>(null);
  const [viewerInspectorTarget, setViewerInspectorTarget] = useState<HTMLElement | null>(null);
  const [programInspectorMinimized, setProgramInspectorMinimized] = useState(false);
  const [viewerInspectorMinimized, setViewerInspectorMinimized] = useState(false);
  const [programInspectorState, setProgramInspectorState] = useState<{
    active: boolean;
    title: string;
    readOnlyReason: string | null;
  }>({ active: false, title: 'Program inspector', readOnlyReason: null });
  const [viewerInspectorPanel, setViewerInspectorPanel] = useState<
    'tools' | 'robot' | 'object' | 'io' | null
  >(null);
  const [inspectorHeight, setInspectorHeight] = useState(() => (
    typeof window === 'undefined' ? 720 : Math.max(320, window.innerHeight - 52)
  ));
  const [programInspectorHeight, setProgramInspectorHeight] = useState(() => (
    typeof window === 'undefined' ? 360 : Math.max(160, Math.floor((window.innerHeight - 52) / 2))
  ));
  const [globalPanel, setGlobalPanel] = useState<'runtime' | 'assistant' | null>(null);
  const [runtimePanelExpanded, setRuntimePanelExpanded] = useState(false);
  const [runtimeAttentionRequest, setRuntimeAttentionRequest] = useState<string | null>(null);
  const [runtimePanelAutomatic, setRuntimePanelAutomatic] = useState(false);
  const [programInvalidEdit, setProgramInvalidEdit] = useState(false);
  const [componentOverviewRequest, setComponentOverviewRequest] = useState(0);
  const workbench = useRef<HTMLDivElement>(null);
  const centralInspector = useRef<HTMLElement>(null);
  const bothInspectorSectionsExpanded = programInspectorState.active
    && !programInspectorMinimized
    && viewerInspectorPanel !== null
    && !viewerInspectorMinimized;
  const previousBothInspectorSectionsExpanded = useRef(false);
  const runtimeButton = useRef<HTMLButtonElement>(null);
  const assistantButton = useRef<HTMLButtonElement>(null);
  const [health, retryHealth] = useStructuralResource(api.health.get, {
    failureMessage: 'Could not read the Composer structural boundary.',
  });
  const [structuralMode, setStructuralMode] = useState<
    'headless_client' | 'structural_only' | null
  >(null);
  const structuralRetryAttempt = useRef(0);
  const headlessEnabled = structuralMode === 'headless_client';
  const [catalog, retryCatalog] = useStructuralResource(api.catalog.list, {
    failureMessage: 'Could not read the immutable v2 node catalog.',
    retryableWhen: isTransientManagerReadinessError,
  });
  const catalogLatestRequestId = useRef(0);
  const retryCatalogTracked = useCallback(() => {
    const requestId = retryCatalog();
    catalogLatestRequestId.current = requestId;
    return requestId;
  }, [retryCatalog]);
  const [manifest, retryManifest] = useStructuralResource(api.urdf.manifest, {
    absentWhen: isNotFound,
    absentMessage: 'No complete URDF is available for the active retained deployment.',
    failureMessage: 'Could not read the retained-neutral URDF manifest.',
  });
  const [nodeEnvironment, retryNodeEnvironment] = useStructuralResource(
    api.nodes.environment,
    {
      enabled: headlessEnabled,
      disabledMessage: 'Node-environment facts require a connected headless owner.',
      failureMessage: 'Could not read the active v2 node environment.',
      retryableWhen: isTransientManagerReadinessError,
    },
  );
  const nodeEnvironmentLatestRequestId = useRef(0);
  const retryNodeEnvironmentTracked = useCallback(() => {
    const requestId = retryNodeEnvironment();
    nodeEnvironmentLatestRequestId.current = requestId;
    return requestId;
  }, [retryNodeEnvironment]);
  const readinessTransientRefreshAttempts = useRef<ReadinessTransientRefreshAttempts>({
    catalog: new Set(),
    nodeEnvironment: new Set(),
  });
  const nodeEnvironmentWakeKey = useRef<string | null>(null);
  const viewerModelWakeKey = useRef<string | null>(null);
  const managerActivationWakeKey = useRef<string | null>(null);
  useEffect(() => {
    if (health.status === 'ready') {
      structuralRetryAttempt.current = 0;
      setStructuralMode(health.data.mode);
      return undefined;
    }
    if (health.status === 'loading' || structuralMode !== null) return undefined;
    const attempt = structuralRetryAttempt.current;
    structuralRetryAttempt.current += 1;
    const timer = window.setTimeout(
      retryHealth,
      Math.min(1_000 * 2 ** attempt, 8_000),
    );
    return () => window.clearTimeout(timer);
  }, [health, retryHealth, structuralMode]);
  const [sourceModel, retrySourceModel] = useStructuralResource(api.sourceModel.get, {
    enabled: headlessEnabled,
    disabledMessage: 'Source-model facts require a connected headless owner.',
    failureMessage: 'Could not read the verified active source-model projection.',
  });
  const [assistantModes, retryAssistantModes] = useStructuralResource(api.assistant.modes, {
    enabled: headlessEnabled,
    disabledMessage: 'The assistant bridge is unavailable.',
    absentWhen: isAssistantProfileAbsent,
    // Without an active configuration there is no assistant component either.
    absentMessage: NO_ASSISTANT_COMPONENT,
    failureMessage: 'Could not read the assistant modes.',
  });
  // This page's chat turn runs: Execute, Run and Resume wait for it.
  const [assistantTurnRunning, setAssistantTurnRunning] = useState(false);
  const closeAssistant = useCallback(() => setGlobalPanel(null), []);
  const headlessStateSource = useHeadlessStateSource(headlessEnabled);
  const headlessState = useShellHeadlessState(headlessStateSource);
  const headlessSnapshot = headlessState.snapshot;
  // One shared authoring publication for the whole workbench: Program,
  // Deployment and Scenario authoring all read this single snapshot, so no
  // consumer establishes an independent authoring identity.
  const authoringWorkspace = useAuthoringWorkspace(
    headlessSnapshot,
    headlessState.transportStatus,
  );
  const deploymentController = useDeploymentWorkspace(
    headlessEnabled,
    headlessSnapshot,
    authoringWorkspace,
  );
  useEffect(() => {
    if (!headlessEnabled) {
      nodeEnvironmentWakeKey.current = null;
      viewerModelWakeKey.current = null;
      return;
    }
    if (headlessSnapshot === null) return;
    const key = nodeEnvironmentRefreshKey(headlessSnapshot);
    if (nodeEnvironmentWakeKey.current === null) {
      nodeEnvironmentWakeKey.current = key;
      return;
    }
    if (nodeEnvironmentWakeKey.current === key) return;
    nodeEnvironmentWakeKey.current = key;
    retryNodeEnvironmentTracked();
    retryAssistantModes();
  }, [headlessEnabled, headlessSnapshot, retryAssistantModes, retryNodeEnvironmentTracked]);
  // The catalog and the Deployment workspace (its Applied/runAdmission half)
  // depend on the active applied selection, which settles after an accepted
  // Load/Apply from any client. Re-read both once when it settles.
  const refreshDeploymentWorkspace = deploymentController.refresh;
  useEffect(() => {
    if (!headlessEnabled) {
      managerActivationWakeKey.current = null;
      return;
    }
    if (headlessSnapshot === null) return;
    managerActivationWakeKey.current = refreshOnManagerActivationSettled(
      managerActivationWakeKey.current,
      managerActivationSettlementKey(headlessSnapshot),
      [retryCatalogTracked, refreshDeploymentWorkspace],
    );
  }, [headlessEnabled, headlessSnapshot, refreshDeploymentWorkspace, retryCatalogTracked]);
  // A settled read may fail after its transition. Retry only the latest transient
  // failure once for the verified Apply identity; an older error cannot consume it.
  useEffect(() => {
    if (!headlessEnabled || headlessSnapshot === null) return;
    readinessTransientRefreshAttempts.current = refreshTransientResourcesOnManagerReadiness(
      readinessTransientRefreshAttempts.current,
      managerReadinessTransientRetryKey(headlessSnapshot, {
        transportStatus: headlessState.transportStatus,
        semanticStatus: headlessState.semanticStatus,
      }),
      {
        failureRequestId: catalog.status === 'error' ? catalog.requestId ?? null : null,
        latestRequestId: catalogLatestRequestId.current,
        retryable: catalog.status === 'error' && catalog.retryable === true,
        refresh: retryCatalogTracked,
      },
      {
        failureRequestId: nodeEnvironment.status === 'error'
          ? nodeEnvironment.requestId ?? null
          : null,
        latestRequestId: nodeEnvironmentLatestRequestId.current,
        retryable: nodeEnvironment.status === 'error' && nodeEnvironment.retryable === true,
        refresh: retryNodeEnvironmentTracked,
      },
    );
  }, [
    catalog,
    headlessEnabled,
    headlessSnapshot,
    headlessState.semanticStatus,
    headlessState.transportStatus,
    nodeEnvironment,
    retryCatalogTracked,
    retryNodeEnvironmentTracked,
  ]);
  useEffect(() => {
    if (!headlessEnabled) {
      viewerModelWakeKey.current = null;
      return;
    }
    if (headlessSnapshot === null) return;
    viewerModelWakeKey.current = refreshViewerModelsOnManagerChange(
      viewerModelWakeKey.current,
      viewerModelRefreshKey(headlessSnapshot),
      retrySourceModel,
      retryManifest,
    );
  }, [
    headlessEnabled,
    headlessSnapshot,
    retryManifest,
    retrySourceModel,
  ]);
  // Composer settings problems appear as workbench notices.
  useComposerSettingsNotices();
  // Bumped by Reset reaching Idle and by Scenario New: clears every warning.
  const [warningClearEpoch, setWarningClearEpoch] = useState(0);
  // A successful Load/New clears a refused-startup notice in the backend;
  // health is read once per page, so read it again then.
  const onScenarioActivated = useCallback((kind: 'new' | 'load') => {
    retryHealth();
    if (kind === 'new') setWarningClearEpoch((epoch) => epoch + 1);
  }, [retryHealth]);
  const programController = useProgramWorkspace(
    headlessSnapshot,
    authoringWorkspace,
    onScenarioActivated,
  );
  // The node selection belongs to one Program Draft. The handler stays stable
  // (memoized canvas callbacks keep it) and reads the current Draft from a ref.
  const programDraftId = programController.state.workspace?.draft.draftId;
  const programDraftIdRef = useRef(programDraftId);
  useEffect(() => { programDraftIdRef.current = programDraftId; }, [programDraftId]);
  const selectProgramNode = useCallback((id: string | null) => {
    const draftId = programDraftIdRef.current;
    if (draftId) setSelectedProgramNode({ draftId, id });
  }, []);
  const referenceValidation = useReferenceValidation(programController.state.workspace);
  // Draft / Saved / Applied of both documents, shown in every workspace.
  const programStates = programDocumentStates(programController.state.workspace);
  // Keys a global interrupt rule may read: the Program Draft's declared and
  // system keys (resolved against the Applied Program).
  const programDraftAuthoring = programController.state.workspace?.draft.authoring ?? null;
  const deploymentDraftDocument = deploymentController.workspace?.draft.document ?? null;
  const assistantModelComponentId = assistantModes.status === 'ready'
    ? assistantModes.data.aiModelComponentId : null;
  const assistantModelUnapplied = deploymentUnapplied(deploymentController.workspace);
  const assistantModel = useMemo((): AssistantModelSummary | null => {
    if (assistantModelComponentId === null) return null;
    const component = aiModelComponents(deploymentDraftDocument)
      .find((candidate) => candidate.componentId === assistantModelComponentId);
    const configuration = component?.runtimeConfiguration ?? {};
    return {
      componentId: assistantModelComponentId,
      model: typeof configuration.model === 'string' ? configuration.model : null,
      baseUrl: typeof configuration.base_url === 'string' ? configuration.base_url : null,
      draftUnapplied: assistantModelUnapplied,
    };
  }, [assistantModelComponentId, assistantModelUnapplied, deploymentDraftDocument]);
  // Null without a Program Draft: a rule's user keys then wait for one.
  const globalRuleProgramKeys = useMemo(() => (programDraftAuthoring === null ? null
    : programBlackboardKeys(programDraftAuthoring, globalRuleIds(deploymentDraftDocument))),
  [deploymentDraftDocument, programDraftAuthoring]);
  const programSceneIds = useMemo(() => (programDraftAuthoring === null ? null
    : (programDraftAuthoring.scene ?? []).map((resource) => resource.scene_resource_id)),
  [programDraftAuthoring]);
  const sceneResourceIds = useMemo(() => {
    const section = headlessSnapshot?.runtime.sections.logicalScene;
    if (!section?.available || section.freshness !== 'fresh' || section.payload === null) return [];
    try { return parseLogicalSceneProjection(section.payload).snapshot?.objects.map((item) => item.scene_resource_id) ?? []; }
    catch { return []; }
  }, [headlessSnapshot]);
  const deploymentStates = deploymentDocumentStates(deploymentController.workspace);
  const cellState = useCellState(headlessStateSource);
  const changedOutside = useMemo(() => new Set((referenceValidation.selectorFacts?.signals ?? [])
    .filter((row) => !row.ambiguous && row.target !== null && row.value_type === 'bool'
      && row.direction === 'input' && row.reference.signal_name.endsWith('.changed_outside'))
    .map((row) => `${row.reference.component_id}/${row.reference.signal_name.slice(0, -'.changed_outside'.length)}`)),
  [referenceValidation.selectorFacts]);
  const programRuntime = useProgramRuntime(
    headlessEnabled,
    headlessSnapshot,
    programController.state.workspace,
    headlessStateSource,
    deploymentUnapplied(deploymentController.workspace),
    assistantTurnRunning,
  );
  const assistant = assistantAvailability(assistantModes, cellState.components, programRuntime.active);
  // Manual motion uses Applied defaults; Draft changes wait for Apply.
  const appliedProgram = programRuntime.state.applied;
  // A global rule's execution_groups scope names the Applied Program's groups.
  const appliedExecutionGroupIds = useMemo(() => (appliedProgram === null || appliedProgram === undefined ? null
    : Object.keys(appliedProgram.program.execution_groups ?? {}).sort()), [appliedProgram]);
  const programCartesianDefaults = useMemo(() => {
    if (catalog.status !== 'ready') return null;
    const defaults = (program: Parameters<typeof deriveProgramMotionConfiguration>[0]) => {
      const configuration = deriveProgramMotionConfiguration(program, catalog.data.entries);
      const translationToleranceM = configuration.available
        ? configuration.configuration.values.translation_tol_m
        : undefined;
      return translationToleranceM === undefined ? null : { translationToleranceM };
    };
    return appliedCartesianDefaults(appliedProgram === null
      ? null
      : defaults({ parameters: appliedProgram.program.parameters, forest: [appliedProgram.program.tree] }));
  }, [appliedProgram, catalog]);
  const componentCatalog = useComponentCatalog(
    headlessEnabled, deploymentController.workspace?.draft.id ?? null);
  const robotControl = useRobotControl(
    headlessEnabled,
    programRuntime.cycleState !== null
      && programRuntime.cycleState !== 'idle_after_reset',
    ['motion-stop', 'full-stop'].includes(programRuntime.state.pending ?? '')
      || ['stopping', 'full_stopping'].includes(
        programRuntime.state.run?.state ?? '',
      ),
    headlessSnapshot,
    headlessStateSource,
    programCartesianDefaults,
    programController.state.workspace?.hasUnappliedChanges === true
      || deploymentUnapplied(deploymentController.workspace),
  );
  const systemSignalChoices = useMemo(() => (referenceValidation.selectorFacts?.signals ?? [])
    .filter((row) => !row.ambiguous && row.target !== null && row.value_type === 'bool'
      && row.direction === 'input' && row.reference.signal_name.startsWith(CARRIED_OBJECT_LOST_SIGNAL_PREFIX))
    .map((row) => ({ controllableComponentId: row.target!.component_id,
      commandTargetId: row.target!.command_target_id, signalName: row.reference.signal_name,
      direction: 'input' as const, valueType: 'bool' as const })), [referenceValidation.selectorFacts]);
  const systemSignalsNeedApply = referenceValidation.selectorFacts === null;
  // Robots are moving: a Program run that is not positively held, a starting or
  // stopping cycle, or an active manual operation. Document actions wait.
  const documentCycle = headlessSnapshot?.programCycle;
  const documentMotionActive = robotControlHasActiveOperation(robotControl.state)
    || documentCycle?.state === 'starting' || documentCycle?.state === 'stopping'
    || (documentCycle?.state === 'running'
      && documentCycle.runtimeControls?.executionSelections.motionHeld !== true);
  const operationalErrors = useOperationalErrors();
  useOperationalErrorMirror({
    source: 'composer.deployment_workspace',
    title: 'Deployment command or response failed',
    message: deploymentController.error,
    onDismiss: deploymentController.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.program_workspace',
    title: 'Program authoring command or response failed',
    message: programController.state.error,
    onDismiss: programController.clearFeedback,
  });
  // A global rule holding its scope (`rule_holds`) refused a
  // past command: like any refusal it is a warning until acknowledged.
  useOperationalErrorMirror({
    source: 'composer.program_runtime',
    title: isRuleHoldsRefusal(programRuntime.state.errorRefusal)
      ? 'Held by a global rule' : 'Program Run, Stop, or Reset failed',
    message: programRuntime.state.error,
    onDismiss: programRuntime.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.runtime_scale',
    title: 'Runtime velocity update failed',
    message: robotControl.runtimeScaleError,
    onDismiss: robotControl.dismissRuntimeScaleError,
  });
  // Local validation stays in the panel; only typed preview planner/collision
  // refusals move to a brief Activity notice instead of the warning rail.
  const robotPreviewRefusal = manualPreviewRefusalNotice(robotControl.state);
  const robotOperationalError = robotControl.state.errorLocal || robotPreviewRefusal !== null
    ? null : robotControl.state.error;
  useOperationalErrorMirror({
    source: 'composer.robot_control',
    title: isRuleHoldsRefusal(robotControl.state.errorRefusal)
      ? 'Held by a global rule' : 'Robot control request failed',
    message: robotOperationalError,
    onDismiss: robotControl.dismissError,
  });
  const clientErrors = operationalErrors.errors;

  // --- One notification model: current conditions, activity and messages. ---
  // Viewer model/source-model read failures are reported once, as the red
  // viewer condition with its Retry action, not also as a dismissible message.
  const controlNotices = useMemo<WorkbenchNotice[]>(() => {
    if (!headlessEnabled) return [];
    if (headlessState.transportStatus === 'delayed') {
      return [{
        id: 'control.connection',
        severity: 'warning',
        domain: 'transport',
        title: 'Control connection delayed',
        message: headlessState.transportError ?? 'Commands are disabled until the connection recovers.',
      }];
    }
    if (headlessState.transportError !== null) {
      return [{
        id: 'control.connection',
        severity: 'error',
        domain: 'transport',
        title: 'Control connection unavailable',
        message: headlessState.transportError,
        actions: [{
          id: 'reconnect',
          label: 'Reconnect',
          title: 'Open a new control session. No earlier command is replayed.',
          run: headlessState.reconnect,
        }],
      }];
    }
    if (headlessSnapshot !== null && headlessState.semanticStatus === 'unavailable') {
      return [{
        id: 'control.state',
        severity: 'error',
        domain: 'transport',
        title: 'Control status unavailable',
        message: `${headlessState.semanticReason ?? 'The owner projection is unavailable.'} Commands are disabled.`,
      }];
    }
    if (headlessSnapshot !== null && headlessState.semanticStatus === 'synchronizing') {
      return [{
        id: 'control.state',
        severity: 'info',
        domain: 'transport',
        title: 'Control status synchronizing',
        message: headlessState.semanticReason ?? 'Waiting for a current owner projection.',
        busy: true,
      }];
    }
    return [];
  }, [
    headlessEnabled,
    headlessSnapshot,
    headlessState.reconnect,
    headlessState.semanticReason,
    headlessState.semanticStatus,
    headlessState.transportError,
    headlessState.transportStatus,
  ]);

  // Activity follows the exact accepted Manager operation: the advertised
  // maintenance operation (also after a browser reload), the identity this
  // browser's accepted Load/Apply/Revert returned, or the Apply recorded for
  // a lost reply's exact request id. A different or older operation's status
  // never clears it.
  const [trackedApply, setTrackedApply] = useState<ManagerApplyIdentity | null>(null);
  const [applyAcknowledgement, setApplyAcknowledgement] = useState<{
    key: string;
    busy: boolean;
    message: string | null;
  } | null>(null);
  // The failed-Apply notice hidden here on positive evidence that nothing is
  // held: a refusal before the fence, or a re-read showing no failed Apply.
  const [dismissedActivationNotice, setDismissedActivationNotice] = useState<string | null>(null);
  useEffect(() => {
    if (programController.acceptedApply !== null) setTrackedApply(programController.acceptedApply);
  }, [programController.acceptedApply]);
  useEffect(() => {
    if (authoringWorkspace.correlatedApply !== null) setTrackedApply(authoringWorkspace.correlatedApply);
  }, [authoringWorkspace.correlatedApply]);
  const maintenance = headlessSnapshot?.componentOverview.maintenanceOperation ?? null;
  const observedApply = useMemo<ManagerApplyIdentity | null>(() => (
    maintenance !== null
      ? { applyId: maintenance.applyId, applyGeneration: maintenance.applyGeneration }
      : trackedApply
  ), [maintenance, trackedApply]);
  const activation = useManagerApplyStatus(
    observedApply,
    headlessSnapshot?.runtime.sections.manager ?? null,
    headlessState.transportStatus,
  );
  const activationStatus = activation.status !== null
    && sameManagerApply(activation.status, observedApply)
    ? activation.status
    : null;
  const acknowledgementKey = observedApply === null
    ? null
    : `${observedApply.applyId}:${observedApply.applyGeneration}`;
  // Acknowledge sends the durable acknowledgement while it is offered, else
  // re-reads the status; only positive evidence that no failed Apply remains
  // hides the notice here.
  const applyAcknowledgementOutcomeUnknown = activation.acknowledgementOutcomeUnknown;
  const acknowledgeFailure = activation.acknowledgeFailure;
  const acknowledgeFailedApply = useCallback(async () => {
    if (applyAcknowledgement?.busy === true || acknowledgementKey === null
      || activationStatus?.state !== 'failed') return;
    setApplyAcknowledgement({ key: acknowledgementKey, busy: true, message: null });
    try {
      const result = await acknowledgeFailure();
      if (result.noFailedApply) {
        setDismissedActivationNotice(`activation.${acknowledgementKey}`);
      } else if (!result.confirmed && result.message !== null) {
        setApplyAcknowledgement((current) => current?.key === acknowledgementKey
          ? { ...current, message: result.message } : current);
      }
    } catch (error) {
      const reason = error instanceof Error ? ` (${error.message})` : '';
      setApplyAcknowledgement((current) => current?.key === acknowledgementKey
        ? {
          ...current,
          message: `Acknowledgement was not confirmed${reason}. The failed Apply remains recorded; no request was retried.`,
        } : current);
    } finally {
      setApplyAcknowledgement((current) => current?.key === acknowledgementKey
        ? { ...current, busy: false } : current);
    }
  }, [
    acknowledgementKey,
    applyAcknowledgement?.busy,
    acknowledgeFailure,
    activationStatus,
  ]);
  const acknowledgingApply = applyAcknowledgement?.busy === true;
  const currentApplyAcknowledgementError = applyAcknowledgement?.key === acknowledgementKey
    ? applyAcknowledgement.message
    : null;
  const activationUnobservable = activation.unavailable;
  // A settled Apply re-reads both Applied views (event-driven).
  useAppliedSettlementRefresh({
    settlement: managerApplySettlementKey(activation.status),
    runtime: programRuntime,
    program: programController,
  });
  // While a run is held on a failed node, preview its Draft edits (advisory).
  const previewCatalogEntries = useMemo(
    () => catalog.status === 'ready' ? catalog.data.entries : [],
    [catalog],
  );
  const [heldPreviewVisible, setHeldPreviewVisible] = useState(false);
  const heldNodePreview = useHeldNodePreview({
    visible: activeWorkspace === 'program' && !programInspectorMinimized && heldPreviewVisible,
    runtime: programRuntime,
    workspace: programController,
    snapshot: headlessSnapshot,
    catalogEntries: previewCatalogEntries,
    motionTargets: robotControl.currentStatus?.directControlTargets ?? NO_MOTION_TARGETS,
  });
  // Apply while a Program run exists: Reset, observed Idle, then Apply.
  const resetApply = useResetApply({
    runtime: programRuntime,
    program: programController,
    deployment: deploymentController,
    snapshot: headlessSnapshot,
    activation: activationStatus,
  });
  useEffect(() => {
    authoringWorkspace.observeApply(activation.status);
  }, [authoringWorkspace, activation.status]);
  const savedScenarioPath = headlessState.transportStatus === 'connected'
    && authoringWorkspace.current
    ? authoringWorkspace.project?.documentPath ?? null
    : null;
  const authoringAdmissionOpen = headlessSnapshot?.authoringAdmission.authoringAllowed ?? null;
  const authoringCause = headlessEnabled
    ? authoringUnavailableCause(authoringWorkspace.published, headlessSnapshot)
    : null;
  // Only owner evidence scopes a transition: the observed activation's status
  // read as pending. An advertised, submitted or unobservable (lost) operation
  // is not evidence, so it never downgrades an error.
  const transitionKey = observedApply !== null
    && !activationUnobservable
    && activationStatus?.state === 'pending'
    ? `${observedApply.applyId}:${observedApply.applyGeneration}`
    : null;
  const activationNotices = useMemo<WorkbenchNotice[]>(() => {
    if (observedApply === null) return [];
    const id = `activation.${observedApply.applyId}:${observedApply.applyGeneration}`;
    if (activationStatus?.state === 'failed' || activationStatus?.state === 'unknown_effect') {
      if (dismissedActivationNotice === id) return [];
      const notice = activationFailureNotice(
        id,
        observedApply.applyId,
        {
          state: activationStatus.state,
          failure: activationStatus.failure,
          failureAcknowledgement: activationStatus.failureAcknowledgement,
        },
        savedScenarioPath,
        authoringAdmissionOpen,
        {
          busy: acknowledgingApply,
          error: currentApplyAcknowledgementError,
          outcomeUnknown: applyAcknowledgementOutcomeUnknown,
          run: () => { void acknowledgeFailedApply(); },
          dismiss: () => setDismissedActivationNotice(id),
        },
      );
      return notice === null ? [] : [notice];
    }
    if (activationStatus?.state === 'succeeded') return [];
    if (activationUnobservable) {
      return [{
        id,
        severity: 'error',
        domain: 'activation',
        title: 'Activation status unavailable',
        message: `Completion of ${observedApply.applyId} cannot currently be observed; it is neither confirmed nor failed.`,
      }];
    }
    const phase = activationStatus?.phase ?? maintenance?.phase ?? 'accepted';
    return [{
      id,
      severity: 'info',
      domain: 'activation',
      title: 'Activation in progress',
      message: `Manager phase: ${String(phase).replaceAll('_', ' ')} (${observedApply.applyId}). Motion does not start automatically.`,
      busy: true,
    }];
  }, [acknowledgeFailedApply, acknowledgingApply, activationStatus, activationUnobservable,
    authoringAdmissionOpen, currentApplyAcknowledgementError, dismissedActivationNotice,
    maintenance, observedApply, applyAcknowledgementOutcomeUnknown, savedScenarioPath]);
  const activationCompleted = useBriefNotice(
    observedApply !== null && activationStatus?.state === 'succeeded'
      ? {
          id: `activation.completed.${observedApply.applyId}`,
          severity: 'info',
          domain: 'activation',
          title: 'Activation completed',
          message: `Manager confirmed ${observedApply.applyId}.`,
        }
      : null,
  );
  const robotPreviewNotice = useBriefNotice(robotPreviewRefusal);

  const pendingProgramAction = programController.state.pendingAction;
  const programMessage = useBriefNotice(
    programController.state.message === null
      ? null
      : {
          id: 'program.operation.result',
          severity: 'info',
          domain: 'program_authoring',
          title: 'Program',
          message: programController.state.message,
        },
  );
  const deploymentMessage = useBriefNotice(
    deploymentController.feedback === null
      ? null
      : {
          id: 'deployment.operation.result',
          severity: 'info',
          domain: 'program_authoring',
          title: 'Deployment',
          message: deploymentController.feedback,
        },
  );
  const manualOperationAdmission = useMemo(
    () => manualOperationAdmissionCopy(robotControl.state),
    [robotControl.state],
  );
  const authoringAdmissionBackgroundStep = authoringAdmissionOpen === false
    && (maintenance !== null || activationStatus?.state === 'pending'
      || deploymentController.applying || pendingProgramAction !== null);
  const authoringAdmissionTooltipText = authoringAdmissionTooltip(
    manualOperationAdmission,
    authoringAdmissionBackgroundStep,
    authoringCause,
  );
  const [resetReloadRequest, setResetReloadRequest] = useState(0);
  const initialSceneFailure = headlessSnapshot?.runtime.initialSceneFailure ?? null;
  const operationNotices = useMemo<WorkbenchNotice[]>(() => {
    const notices: WorkbenchNotice[] = [
      ...requiredComponentFaultNotices(headlessSnapshot),
      ...authoringContainmentNotices(headlessSnapshot),
      ...isaacJointStreamNotices(headlessSnapshot?.runtime.sections.sceneRuntime ?? null),
    ];
    const sceneNotice = initialSceneNotice(initialSceneFailure, () => {
      setActiveWorkspace('program');
      setResetReloadRequest((value) => value + 1);
    });
    if (sceneNotice !== null) notices.push(sceneNotice);
    const label = pendingProgramAction === null
      ? undefined
      : PROGRAM_ACTION_LABELS[pendingProgramAction];
    if (label !== undefined) {
      notices.push({
        id: 'program.operation',
        severity: 'info',
        domain: 'program_authoring',
        title: `${label}…`,
        message: 'Waiting for the backend response.',
        busy: true,
      });
    }
    if (deploymentController.busy) {
      notices.push({
        id: 'deployment.operation',
        severity: 'info',
        domain: 'program_authoring',
        title: deploymentController.applying
          ? 'Submitting Deployment Apply…'
          : 'Deployment request in progress…',
        message: 'Waiting for the backend response.',
        busy: true,
      });
    }
    const reason = authoringWorkspace.unavailableReason ?? '';
    // Execution that is not Idle is not reported here: it is the direct
    // consequence of a Run, shown as the editors' status and control tooltips.
    if (authoringCause === 'synchronizing' || authoringCause === 'not_read') {
      notices.push({
        id: 'authoring.unavailable',
        severity: 'info',
        domain: 'program_authoring',
        title: 'Authoring synchronizing',
        message: reason,
        busy: true,
      });
    } else if (authoringCause === 'not_admitted' && manualOperationAdmission === null
      && maintenance === null && !deploymentController.applying && pendingProgramAction === null) {
      notices.push({
        id: 'authoring.unavailable',
        severity: 'info',
        domain: 'program_authoring',
        title: 'Authoring locked',
        message: reason,
      });
    } else if (authoringCause === 'disconnected' && controlNotices.length === 0) {
      notices.push({
        id: 'authoring.unavailable',
        severity: 'error',
        domain: 'program_authoring',
        title: 'Authoring unavailable',
        message: reason,
      });
    }
    return notices;
  }, [
    authoringCause,
    initialSceneFailure,
    authoringWorkspace.unavailableReason,
    controlNotices.length,
    deploymentController.applying,
    deploymentController.busy,
    pendingProgramAction,
    headlessSnapshot,
    maintenance,
    manualOperationAdmission,
  ]);
  const publishedNotices = usePublishedWorkbenchNotices();
  const sessionNotice = health.status === 'ready'
    ? health.data.workbenchSessionNotice ?? null
    : null;
  const [registeredSessionNotice, setRegisteredSessionNotice] = useState<string | null>(null);
  const sessionRegistration = health.status === 'ready'
    ? health.data.headless?.workbench_previous_session_registration
    : null;
  const sessionNoticeHandoff = previousSessionAlarmHandoff(
    sessionNotice?.sessionId ?? null, registeredSessionNotice, sessionRegistration,
  );
  useEffect(() => {
    setRegisteredSessionNotice(sessionNoticeHandoff);
  }, [sessionNoticeHandoff]);
  // A completed native startup registers this exact predecessor with the
  // existing alarm owner (or proves its persisted acknowledgement). Keep the
  // handoff through transport loss and removal of the acknowledged alarm.
  const sessionNoticeOwnedByAlarms = sessionNotice !== null
    && sessionNoticeHandoff === sessionNotice.sessionId;
  const sessionNotices = useMemo<WorkbenchNotice[]>(() => sessionNotice === null
    || sessionNoticeOwnedByAlarms ? [] : [{
    id: `cold-session.${sessionNotice.sessionId}`,
    severity: 'warning',
    domain: 'recovery',
    title: 'ManyForge did not close correctly last time.',
    message: 'The cell started empty; nothing from the previous session was resumed. Its records are kept; details in History.',
    details: `Failure time unavailable. Session: ${sessionNotice.sessionId}. Archive: ${sessionNotice.archiveRelativePath}. Journal families: ${sessionNotice.journalFamilies.join(', ')}. History coverage: ${sessionNotice.historyCoverage}. ${sessionNotice.message}`,
  }], [sessionNotice, sessionNoticeOwnedByAlarms]);
  // A refused startup project (health is read once per page).
  const startupNotice = health.status === 'ready'
    ? health.data.startupScenarioNotice ?? null
    : null;
  const startupNotices = useMemo(() => startupScenarioNotices(startupNotice), [startupNotice]);
  // A lost reply's outcome and its recheck belong in the notice rail: the
  // header keeps its fixed slots so the document actions stay visible.
  const {
    submissionUnconfirmed, recheckMessage, rechecking, recheckSubmission,
  } = authoringWorkspace;
  // Unconfirmed stays until rechecked; a verified result is brief Activity.
  const submissionVerified = useBriefNotice(
    !submissionUnconfirmed && recheckMessage !== null
      ? {
          id: 'authoring.submission.verified',
          severity: 'info',
          domain: 'activation',
          title: 'Submission verified',
          message: recheckMessage,
        }
      : null,
  );
  const submissionNotices = useMemo<WorkbenchNotice[]>(() => submissionUnconfirmed ? [{
    id: 'authoring.submission',
    severity: 'warning',
    domain: 'activation',
    title: 'Submission unconfirmed',
    message: recheckMessage ?? 'The reply to the last request was lost; its outcome is not known yet. Nothing was retried.',
    busy: rechecking,
    actions: [{
      id: 'recheck',
      label: rechecking ? 'Checking…' : 'Recheck status',
      disabled: rechecking,
      run: () => { void recheckSubmission(); },
    }],
  }] : submissionVerified === null ? [] : [submissionVerified],
  [recheckMessage, recheckSubmission, rechecking, submissionUnconfirmed, submissionVerified]);
  const workbenchNotices = useMemo(() => [
    ...controlNotices,
    ...activationNotices,
    ...publishedNotices,
    ...operationNotices,
    ...sessionNotices,
    ...startupNotices,
    ...submissionNotices,
    ...(robotPreviewNotice === null ? [] : [robotPreviewNotice]),
    ...authoringAdmissionNotices(
      authoringAdmissionOpen,
      manualOperationAdmission,
      authoringAdmissionBackgroundStep,
    ),
    ...(activationCompleted === null ? [] : [activationCompleted]),
    ...(programMessage === null ? [] : [programMessage]),
    ...(deploymentMessage === null ? [] : [deploymentMessage]),
  ], [
    activationCompleted,
    activationNotices,
    authoringAdmissionBackgroundStep,
    controlNotices,
    deploymentMessage,
    manualOperationAdmission,
    operationNotices,
    programMessage,
    publishedNotices,
    sessionNotices,
    startupNotices,
    submissionNotices,
    authoringAdmissionOpen,
    robotPreviewNotice,
  ]);
  const transitionScope = useTransitionScope(transitionKey, workbenchNotices);
  const transitionClassified = useMemo(
    () => classifyWorkbenchNotices(workbenchNotices, transitionScope),
    [transitionScope, workbenchNotices],
  );
  // Viewer conditions become errors (and are reported) only after the hold;
  // re-sort because held ones are presented as information meanwhile.
  const heldNotices = useViewerConditionHold(transitionClassified);
  const classifiedNotices = useMemo(
    () => classifyWorkbenchNotices(heldNotices, null),
    [heldNotices],
  );
  // Red is only what blocks now. A viewer condition blocks no command, so it
  // is a warning; it is still reported as a viewer incident while it lasts.
  const viewerConditions = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'error')
      .filter((notice) => notice.incidentKind !== undefined),
    [classifiedNotices],
  );
  const errorConditions = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'error')
      .filter((notice) => notice.incidentKind === undefined),
    [classifiedNotices],
  );
  // Warnings that block nothing stay until acknowledged here (per browser
  // session), or until Reset or Scenario New.
  const [acknowledgedWarnings, setAcknowledgedWarnings] = useState<ReadonlySet<string>>(
    () => new Set(),
  );
  const localWarnings = useMemo(() => [
    ...noticesOfSeverity(classifiedNotices, 'warning'),
    ...viewerConditions.map(viewerConditionWarning),
  ], [classifiedNotices, viewerConditions]);
  const acknowledgeableKey = localWarnings
    .filter((notice) => notice.acknowledgeable === true)
    .map((notice) => notice.id)
    .join('\n');
  const presentAcknowledgeable = useRef<string[]>([]);
  useEffect(() => {
    const present = acknowledgeableKey === '' ? [] : acknowledgeableKey.split('\n');
    presentAcknowledgeable.current = present;
    // A warning that has gone is forgotten, so its next occurrence shows again.
    setAcknowledgedWarnings((current) => ([...current].every((id) => present.includes(id))
      ? current
      : new Set([...current].filter((id) => present.includes(id)))));
  }, [acknowledgeableKey]);
  const cycleStateNow = headlessSnapshot?.programCycle.state;
  const previousCycleState = useRef(cycleStateNow);
  useEffect(() => {
    const previous = previousCycleState.current;
    previousCycleState.current = cycleStateNow;
    if (resetReachedIdle(previous, cycleStateNow)) setWarningClearEpoch((epoch) => epoch + 1);
  }, [cycleStateNow]);
  const clearOperationalErrors = operationalErrors.clear;
  useEffect(() => {
    if (warningClearEpoch === 0) return;
    clearOperationalErrors();
    setAcknowledgedWarnings(new Set(presentAcknowledgeable.current));
  }, [clearOperationalErrors, warningClearEpoch]);
  const composerWarnings = useMemo(
    () => composerWarningNotices(clientErrors, operationalErrors.dismiss),
    [clientErrors, operationalErrors.dismiss],
  );
  const alarmAcknowledgement = useExactAlarmAcknowledgement();
  const { acknowledge: acknowledgeAlarm, setError: setAlarmAcknowledgementError } = alarmAcknowledgement;
  useOperationalErrorMirror({
    source: 'composer.warning_alarm_acknowledgement',
    title: 'Alarm acknowledgement failed',
    message: alarmAcknowledgement.error,
    onDismiss: () => setAlarmAcknowledgementError(null),
  });
  // Unacknowledged warning alarms (yellow) with the server's exact acknowledgement.
  const alarmWarnings = useMemo(() => {
    const alarms = headlessSnapshot?.alarms;
    return alarms === undefined ? [] : alarmWarningNotices(
      warningAlarms(alarms),
      alarmAcknowledgement.busyActivation,
      (alarm) => { void acknowledgeAlarm([alarm], alarms.ownerEpoch); },
    );
  }, [acknowledgeAlarm, alarmAcknowledgement.busyActivation, headlessSnapshot]);
  const acknowledgeWarning = useCallback((id: string) => {
    setAcknowledgedWarnings((current) => new Set(current).add(id));
  }, []);
  const warningNotices = useMemo<WorkbenchNotice[]>(() => [
    ...locallyAcknowledgedWarnings(localWarnings, acknowledgedWarnings, acknowledgeWarning),
    ...composerWarnings,
    ...alarmWarnings,
  ], [acknowledgeWarning, acknowledgedWarnings, alarmWarnings, composerWarnings, localWarnings]);
  const activityNotices = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'info'),
    [classifiedNotices],
  );
  useViewerIncidentReporting(
    viewerConditions,
    headlessEnabled ? headlessStateSource.reportClientIncident : undefined,
  );
  const errorItemCount = useMemo(() => runtimeErrorItemCount(
    headlessSnapshot?.alarms,
    errorConditions,
  ), [errorConditions, headlessSnapshot]);
  // Only red items open the panel automatically; warnings never do.
  const runtimeAttentionItems = useMemo(() => [
    ...errorConditions.map((condition) => ({
      identity: `condition:${condition.id}`,
    })),
    ...errorAlarms(headlessSnapshot?.alarms).map((alarm) => ({
      identity: `backend:${alarm.activationId}`,
    })),
  ], [errorConditions, headlessSnapshot]);
  const openRuntimeAttention = useCallback((identity: string) => {
    setRuntimeAttentionRequest(identity);
    // Automatic attention may choose the initial compact presentation, but it
    // must never override an already-open panel that the operator controls.
    if (globalPanel !== 'runtime') {
      setRuntimePanelExpanded(false);
      setRuntimePanelAutomatic(true);
    }
    setGlobalPanel('runtime');
  }, [globalPanel]);
  useEffect(() => {
    if (shouldCloseAutomaticRuntimeAttention(
      runtimePanelAutomatic,
      globalPanel,
      runtimeAttentionItems.length,
    )) {
      setGlobalPanel(null);
      setRuntimePanelExpanded(false);
      setRuntimeAttentionRequest(null);
      setRuntimePanelAutomatic(false);
    }
  }, [globalPanel, runtimeAttentionItems.length, runtimePanelAutomatic]);
  const runtimeView = headlessSnapshot?.componentOverview ?? null;
  const inspectorWidthMax = centeredInspectorWidthMax(workbenchWidth);
  const programInspectorHeightMax = Math.max(
    INSPECTOR_SECTION_MIN_HEIGHT,
    inspectorHeight - INSPECTOR_SECTION_MIN_HEIGHT - WORKBENCH_DIVIDER_WIDTH,
  );
  const layoutStyle = {
    '--inspector-width': `${inspectorWidth}px`,
    '--program-inspector-height': `${programInspectorHeight}px`,
  } as CSSProperties;
  useEffect(() => {
    const element = workbench.current;
    if (element === null) return undefined;
    const update = () => setWorkbenchWidth(element.clientWidth);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    setInspectorWidth((current) => clamp(
      current,
      INSPECTOR_WIDTH_MIN,
      inspectorWidthMax,
    ));
  }, [inspectorWidthMax]);
  useEffect(() => {
    const element = centralInspector.current;
    if (element === null) return undefined;
    const update = () => setInspectorHeight(element.clientHeight);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    setProgramInspectorHeight((current) => clamp(
      current,
      INSPECTOR_SECTION_MIN_HEIGHT,
      programInspectorHeightMax,
    ));
  }, [programInspectorHeightMax]);
  useEffect(() => {
    if (
      bothInspectorSectionsExpanded
      && !previousBothInspectorSectionsExpanded.current
    ) {
      setProgramInspectorHeight(Math.floor(inspectorHeight / 2));
    }
    previousBothInspectorSectionsExpanded.current = bothInspectorSectionsExpanded;
  }, [bothInspectorSectionsExpanded, inspectorHeight]);
  const handleProgramInspectorState = useCallback((next: {
    active: boolean;
    title: string;
    readOnlyReason: string | null;
  }) => {
    setProgramInspectorState((current) => (
      current.active === next.active
      && current.title === next.title
      && current.readOnlyReason === next.readOnlyReason
        ? current
        : next
    ));
  }, []);
  const maximizeProgramInspector = useCallback(() => {
    const alreadyMaximized = viewerInspectorPanel !== null
      && viewerInspectorMinimized
      && !programInspectorMinimized;
    setProgramInspectorMinimized(false);
    if (viewerInspectorPanel !== null) {
      setViewerInspectorMinimized(!alreadyMaximized);
    }
  }, [programInspectorMinimized, viewerInspectorMinimized, viewerInspectorPanel]);
  const maximizeViewerInspector = useCallback(() => {
    const alreadyMaximized = programInspectorState.active
      && programInspectorMinimized
      && !viewerInspectorMinimized;
    setViewerInspectorMinimized(false);
    if (programInspectorState.active) {
      setProgramInspectorMinimized(!alreadyMaximized);
    }
  }, [programInspectorMinimized, programInspectorState.active, viewerInspectorMinimized]);
  // Components page links: Robot Control (in the viewer) and a Deployment tab.
  const { selectRobot, selectMode, motionTargets: robotMotionTargets } = robotControl;
  const robotControlApproving = robotControl.state.phase === 'approving';
  const openRobotControl = useCallback((componentId: string) => {
    if (!robotControlApproving) {
      const motion = robotMotionTargets.find(
        (target) => target.targetDescriptor.controllableComponentId === componentId,
      );
      selectRobot(componentId, motion?.targetDescriptor.commandTargetId ?? '');
      // A tool has only its gripper target.
      if (motion === undefined) selectMode('gripper');
    }
    requestOpenSettings('robot');
  }, [robotControlApproving, robotMotionTargets, selectMode, selectRobot]);
  const openDeploymentTab = useCallback((tab: ComponentManagerTab) => {
    setActiveWorkspace('deployment');
    setDeploymentTabRequest((current) => ({ tab, sequence: (current?.sequence ?? 0) + 1 }));
  }, []);
  // "Open global rules" from the Program node editor: the rules are on Cell
  // settings. Refused like the Deployment button while a Program field edit is invalid.
  useEffect(() => onOpenGlobalRulesRequest(() => {
    if (programInvalidEdit) takeOpenGlobalRulesRequest();
    else openDeploymentTab('cell');
  }), [openDeploymentTab, programInvalidEdit]);
  const robotControlIds = useMemo(() => [...new Set([
    ...robotControl.motionTargets, ...robotControl.gripperTargets,
  ].map((target) => target.targetDescriptor.controllableComponentId))], [
    robotControl.gripperTargets, robotControl.motionTargets,
  ]);
  const openRuntimeDetails = useCallback(() => {
    setRuntimePanelExpanded(true);
    setRuntimePanelAutomatic(false);
    setRuntimeAttentionRequest(null);
    setGlobalPanel('runtime');
  }, []);
  const closeRuntimePanel = useCallback(() => {
    setGlobalPanel(null);
    setRuntimePanelExpanded(false);
    setRuntimeAttentionRequest(null);
    setRuntimePanelAutomatic(false);
  }, []);
  const setRuntimePanelExpansion = useCallback((expanded: boolean) => {
    setRuntimePanelExpanded(expanded);
    if (expanded) setRuntimePanelAutomatic(false);
  }, []);

  const connectedProjection = headlessEnabled
    && headlessSnapshot !== null
    && headlessState.transportStatus === 'connected'
    && headlessState.semanticStatus === 'current';
  const connectionLabel = connectedProjection
    ? 'Headless owner connected'
    : !headlessEnabled
      ? 'Runtime unavailable'
      : headlessState.transportStatus === 'delayed'
        ? 'Control connection delayed'
        : headlessState.transportStatus === 'stale'
          ? 'Control connection stale'
          : headlessState.transportStatus === 'connected'
            ? `Headless owner connected · control state ${headlessState.semanticStatus}`
            : headlessState.transportStatus === 'connecting'
              ? 'Connecting to Headless owner'
              : 'Headless owner disconnected';
  const connectionDetail = connectedProjection
    ? `Receiver ${headlessSnapshot.receiverEpoch} · snapshot ${headlessSnapshot.snapshotSequence}`
    : headlessState.transportError
      ?? (headlessState.semanticStatus === 'current'
        ? null
        : `Control state ${headlessState.semanticStatus}: ${headlessState.semanticReason
          ?? 'owner projection unavailable'}`)
      ?? headlessState.error
      ?? (health.status === 'error' || health.status === 'absent'
        ? health.message
        : health.status === 'ready'
          ? health.data.runtimeAvailability?.message ?? 'Structural Composer mode'
          : 'Reading backend status…');
  const deploymentStatus = runtimeStatusPresentation({
    status: runtimeView,
    backendConnected: connectedProjection,
    error: headlessState.error,
    connectionDetail,
    unacknowledgedAlarms: headlessSnapshot?.alarms.unacknowledgedCount ?? 0,
  });
  const deploymentTitle = programInvalidEdit
    ? `Review or discard the invalid Program field edit first. Component status: ${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`
    : `${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`;
  const componentsStatus = componentsTabStatus(cellState);
  // New/Save/Save as/Load act on the whole Scenario in every workspace.
  const scenarioCommands = (
    <ScenarioDocumentCommands
      controller={programController}
      authoringAdmissionAllowed={authoringAdmissionOpen}
      replacementAdmissionAllowed={replacementAdmissionAllowed(authoringAdmissionOpen, authoringCause)}
      deploymentDirty={deploymentController.workspace?.draft.differsFromSaved === true}
      localInvalidEdit={programInvalidEdit}
      motionActive={documentMotionActive}
      cellComponents={cellState.components?.map((component) => component.component_id) ?? null}
    />
  );
  // Discard/Revert/Apply act on the whole Scenario in every workspace too.
  const scenarioActions = (
    <ScenarioActionControls
      program={programController}
      deployment={deploymentController}
      runtime={programRuntime}
      authoringAdmissionAllowed={authoringAdmissionOpen}
      resetApply={resetApply}
      localInvalidEdit={programInvalidEdit}
    />
  );

  return (
    <AuthoringAdmissionTooltipProvider tooltip={authoringAdmissionTooltipText}>
    <DraftInputResetProvider scope="scenario" resetKey={`${programController.scenarioDraftInputResetEpoch}:${deploymentController.scenarioDraftInputResetEpoch}`}>
    <main className="app-shell">
      <header className="app-header">
        <div className="app-identity" aria-label="ManyForge Composer">
          <span className="brand-mark" aria-hidden="true">MF</span>
          <strong>ManyForge</strong>
        </div>
        <nav className="workspace-switcher" aria-label="Workbench sections">
          <DocumentStateLetters states={programStates} owner="Program" side="left" />
          <button
            type="button"
            data-workspace="program"
            className={activeWorkspace === 'program' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'program'}
            onClick={() => setActiveWorkspace('program')}
          >
            Program
          </button>
          <button
            type="button"
            data-workspace="scene"
            className={activeWorkspace === 'scene' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'scene'}
            disabled={programInvalidEdit}
            title={programInvalidEdit ? 'Review or discard the invalid Program field edit first.' : undefined}
            onClick={() => setActiveWorkspace('scene')}
          >
            Scene
          </button>
          <button
            type="button"
            data-workspace="components"
            className={`components-status-button readiness-${componentsStatus.readiness}${
              activeWorkspace === 'components' ? ' active' : ''
            }`}
            aria-pressed={activeWorkspace === 'components'}
            aria-label={`Components · ${componentsStatus.summary}`}
            disabled={programInvalidEdit}
            title={programInvalidEdit
              ? `Review or discard the invalid Program field edit first. ${componentsStatus.summary}`
              : componentsStatus.summary}
            onClick={() => setActiveWorkspace('components')}
          >
            Components
          </button>
          <button
            type="button"
            data-workspace="deployment"
            className={`deployment-status-button readiness-${deploymentStatus.readiness}${
              activeWorkspace === 'deployment' ? ' active' : ''
            }`}
            aria-pressed={activeWorkspace === 'deployment'}
            aria-label={`Deployment · ${deploymentStatus.accessibleLabel}. Live status feed ${
              deploymentStatus.backendConnected ? 'connected' : 'unavailable'
            }. ${deploymentStatus.attentionCount > 0
              ? `${deploymentStatus.attentionCount} attention ${
                  deploymentStatus.attentionCount === 1 ? 'item' : 'items'
                }. `
              : ''}${deploymentStatus.detail}`}
            data-backend-connected={deploymentStatus.backendConnected}
            disabled={programInvalidEdit}
            title={deploymentTitle}
            onClick={() => {
              setActiveWorkspace('deployment');
              setDeploymentTabRequest(null);
              setComponentOverviewRequest((current) => current + 1);
            }}
          >
            <span>Deployment</span>
            <span
              className={`deployment-status-indicator${
                deploymentStatus.attentionCount > 0 ? ' has-attention' : ''
              }`}
              aria-hidden="true"
              title={`${deploymentStatus.backendConnected
                ? 'Live status feed connected'
                : 'Live status feed unavailable'}${deploymentStatus.attentionCount > 0
                ? ` · ${deploymentStatus.attentionCount} attention ${
                    deploymentStatus.attentionCount === 1 ? 'item' : 'items'
                  }`
                : ''}`}
            >
              {deploymentStatus.attentionCount > 0
                ? deploymentStatus.attentionCount
                : ''}
            </span>
          </button>
          <DocumentStateLetters states={deploymentStates} owner="Deployment" side="right" />
        </nav>
        <DocumentControls
          {...(activeWorkspace === 'deployment' || activeWorkspace === 'components'
            ? deploymentDocumentHeader(deploymentController)
            : programDocumentHeader(programController))}
          scenarioCommands={scenarioCommands}
          actions={scenarioActions}
        />
        <div className="global-surface-launchers" aria-label="Application tools">
          <button
            ref={runtimeButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'runtime'}
            onClick={() => {
              setRuntimePanelExpanded(false);
              setRuntimePanelAutomatic(false);
              setRuntimeAttentionRequest(null);
              setGlobalPanel('runtime');
            }}
          >
            Runtime / history
            {errorItemCount > 0 && (
              <span className="global-attention-count" title="Errors">
                {errorItemCount}
              </span>
            )}
            {warningNotices.length > 0 && (
              <span className="global-attention-count warning" title="Warnings">
                {warningNotices.length}
              </span>
            )}
          </button>
          {/* An open panel can still be hidden when the assistant becomes unavailable. */}
          <button
            ref={assistantButton}
            type="button"
            aria-expanded={globalPanel === 'assistant'}
            aria-controls="assistant-dock"
            disabled={assistant.unavailableReason !== null && globalPanel !== 'assistant'}
            title={assistant.unavailableReason
              ?? (globalPanel === 'assistant' ? 'Hide the AI assistant.' : 'Open the AI assistant.')}
            onClick={() => setGlobalPanel((panel) => (panel === 'assistant' ? null : 'assistant'))}
          >AI</button>
        </div>
      </header>

      <div
        ref={workbench}
        className="workbench"
        style={layoutStyle}
      >
        <section className="workbench-workspace">
          {activeWorkspace === 'program' ? (
            <DraftInputResetProvider scope="program" resetKey={programController.programDraftInputResetEpoch}>
            <ProgramWorkspace
              selectedNodeId={selectedProgramNode !== null && selectedProgramNode.draftId === programDraftId ? selectedProgramNode.id : null}
              onSelectedNodeIdChange={selectProgramNode}
              onHeldPreviewVisibilityChange={setHeldPreviewVisible}
              referenceValidation={referenceValidation}
              controller={programController}
              runtime={programRuntime}
              manualPreviewController={robotControl}
              heldNodePreview={heldNodePreview}
              catalog={catalog}
              environment={nodeEnvironment}
              sourceModel={sourceModel}
              deploymentWorkspace={deploymentController.workspace}
              changedOutsideOutputs={changedOutside}
              systemSignalsNeedApply={systemSignalsNeedApply}
              motionDescriptors={robotControl.motionTargets.map(
                (target) => target.targetDescriptor,
              )}
              onRetryCatalog={retryCatalogTracked}
              onRetryEnvironment={retryNodeEnvironmentTracked}
              onInvalidEditChange={setProgramInvalidEdit}
              onOpenScene={() => setActiveWorkspace('scene')}
              inspectorTarget={programInspectorTarget}
              inspectorMinimized={programInspectorMinimized}
              onInspectorMinimizedChange={setProgramInspectorMinimized}
              onInspectorMaximize={maximizeProgramInspector}
              onInspectorStateChange={handleProgramInspectorState}
            />
            </DraftInputResetProvider>
          ) : activeWorkspace === 'scene' ? (
            <div
              ref={setSceneEditorTarget}
              className="scene-workspace-host"
              aria-label="Scene authoring workspace"
            />
          ) : activeWorkspace === 'components' ? (
            <div className="deployment-workspace">
              <DraftInputResetProvider scope="deployment" resetKey={deploymentController.deploymentDraftInputResetEpoch}>
              <ComponentsWorkspace
                enabled={headlessEnabled}
                controller={deploymentController}
                cellState={cellState}
                catalog={componentCatalog}
                sourceModel={sourceModel}
                motionDescriptors={robotControl.motionTargets.map(
                  (target) => target.targetDescriptor,
                )}
                gripperDescriptors={robotControl.gripperTargets.map(
                  (target) => target.targetDescriptor,
                )}
                programSceneIds={programSceneIds}
                sceneResourceIds={sceneResourceIds}
                robotControlIds={robotControlIds}
                onOpenRobotControl={openRobotControl}
                onOpenDeployment={openDeploymentTab}
              />
              </DraftInputResetProvider>
            </div>
          ) : (
            <div className="deployment-workspace">
              <DraftInputResetProvider scope="deployment" resetKey={deploymentController.deploymentDraftInputResetEpoch}>
              <DeploymentCompositionPanel
                controller={deploymentController}
                componentCatalog={componentCatalog}
                tabRequest={deploymentTabRequest}
                onOpenComponents={() => setActiveWorkspace('components')}
                enabled={headlessEnabled}
                snapshot={headlessSnapshot}
                transportStatus={headlessState.transportStatus}
                sourceModel={sourceModel}
                motionDescriptors={robotControl.motionTargets.map(
                  (target) => target.targetDescriptor,
                )}
                stopChains={robotControl.motionTargets}
                gripperDescriptors={robotControl.gripperTargets.map(
                  (target) => target.targetDescriptor,
                )}
                programKeys={globalRuleProgramKeys}
                programSceneIds={programSceneIds}
                executionGroupIds={appliedExecutionGroupIds}
                systemSignalChoices={systemSignalChoices}
                systemSignalsNeedApply={systemSignalsNeedApply}
                changedOutsideOutputs={changedOutside}
                maintenanceStatus={activation}
                componentOverview={{
                  request: componentOverviewRequest,
                  status: runtimeView,
                  backendConnected: connectedProjection,
                  connectionLabel,
                  connectionDetail,
                  contextFacts: programRuntime.state.currentScenario?.currentScenario === null
                    || programRuntime.state.currentScenario === null
                    ? []
                    : [
                        `Scenario ${programRuntime.state.currentScenario.currentScenario.scenarioId}`,
                        `Deployment ${programRuntime.state.currentScenario.currentScenario.deploymentRevision}`,
                        `Program ${programRuntime.state.currentScenario.currentScenario.programRevision}`,
                      ],
                  unacknowledgedAlarms:
                    headlessSnapshot?.alarms.unacknowledgedCount ?? 0,
                  activeAcknowledgedAlarms:
                    headlessSnapshot?.alarms.activeAcknowledgedCount ?? 0,
                  clientErrorCount: clientErrors.length,
                  error: headlessState.error,
                  cellState,
                  onReconnect: () => {
                    headlessState.reconnect();
                    if (structuralMode === null) retryHealth();
                  },
                  onOpenRuntimeHistory: openRuntimeDetails,
                }}
              />
              </DraftInputResetProvider>
            </div>
          )}
        </section>

        <PaneDivider
          label="central inspector"
          value={inspectorWidth}
          min={INSPECTOR_WIDTH_MIN}
          max={inspectorWidthMax}
          direction={-1}
          scale={2}
          onChange={setInspectorWidth}
        />

        <aside
          ref={centralInspector}
          className={`workbench-inspector${bothInspectorSectionsExpanded ? ' split' : ''}`}
          aria-label="Program and viewer inspectors"
        >
          <div
            ref={setProgramInspectorTarget}
            className={`central-inspector-slot program-slot${
              programInspectorState.active && !programInspectorMinimized ? ' expanded' : ' minimized'
            }${programInspectorState.active ? ' active' : ' inactive'}`}
          >
            {activeWorkspace !== 'program' && (
              <div className="central-inspector-placeholder">
                <strong>Program inspector</strong>
                <span>No Program node is selected.</span>
              </div>
            )}
          </div>
          {bothInspectorSectionsExpanded && (
            <PaneDivider
              axis="y"
              className="central-inspector-divider"
              label="Program and viewer inspector height"
              value={programInspectorHeight}
              min={INSPECTOR_SECTION_MIN_HEIGHT}
              max={programInspectorHeightMax}
              onChange={setProgramInspectorHeight}
            />
          )}
          <div
            ref={setViewerInspectorTarget}
            className={`central-inspector-slot viewer-inspector-slot${
              viewerInspectorPanel !== null && !viewerInspectorMinimized ? ' expanded' : ' minimized'
            }${viewerInspectorPanel !== null ? ' active' : ' inactive'}`}
          />
        </aside>

        <PaneDivider
          label="central inspector (viewer side)"
          value={inspectorWidth}
          min={INSPECTOR_WIDTH_MIN}
          max={inspectorWidthMax}
          scale={2}
          onChange={setInspectorWidth}
        />

        <aside className="workbench-commands" aria-label="3D viewer and live controls">
          <div className="workbench-pane">
            <div className="viewer-slot">
              <SceneAuthoringWorkspace
                selectorFacts={referenceValidation.selectorFacts}
                editorTarget={sceneEditorTarget}
                environment={nodeEnvironment}
                headlessStateSource={headlessStateSource}
                manifest={manifest}
                onRetryManifest={retryManifest}
                onRetrySourceModel={retrySourceModel}
                programController={programController}
                deploymentController={deploymentController}
                programRuntime={programRuntime}
                authoringAdmissionAllowed={authoringAdmissionOpen}
                robotControl={robotControl}
                correctionGhost={heldNodePreview.ghost}
                runtimeEnabled={connectedProjection}
                resetReloadRequest={resetReloadRequest}
                sourceModel={sourceModel}
                viewerActive
                viewerInspectorTarget={viewerInspectorTarget}
                viewerInspectorMinimized={viewerInspectorMinimized}
                onViewerInspectorMinimizedChange={setViewerInspectorMinimized}
                onViewerInspectorMaximize={maximizeViewerInspector}
                onViewerInspectorPanelChange={setViewerInspectorPanel}
                onOpenRuntimeHistory={openRuntimeDetails}
                runtimeOverlay={(
                  <WorkbenchNotificationStack
                    // Every row is visible whenever it holds active items.
                    // Runtime/history is the error row's expandable view:
                    // closing it collapses to the row and never hides an
                    // unresolved error, recovery condition or alarm.
                    errorRow={globalPanel === 'runtime' || errorItemCount > 0 ? (
                      <HeadlessRuntimeSurfaces
                        enabled={headlessEnabled || errorConditions.length > 0}
                        headlessStateSource={headlessStateSource}
                        conditions={errorConditions}
                        attentionRequest={runtimeAttentionRequest}
                        expanded={runtimePanelExpanded}
                        onExpandedChange={setRuntimePanelExpansion}
                        onClose={errorItemCount === 0 ? closeRuntimePanel : undefined}
                      />
                    ) : null}
                    warnings={warningNotices}
                    activity={activityNotices}
                  />
                )}
              />
            </div>
          </div>
        </aside>
      </div>
      <RuntimeAttentionAutoOpen
        items={runtimeAttentionItems}
        onOpen={openRuntimeAttention}
      />
      {(assistant.modeIndex !== null || assistant.modeError !== null) && (
        <AssistantOverlay
          open={globalPanel === 'assistant'}
          onClose={closeAssistant}
          returnFocusRef={assistantButton}
          unavailableReason={assistant.unavailableReason}
          modeIndex={assistant.modeIndex}
          modeError={assistant.modeError}
          modelNotice={assistant.modelNotice}
          onRetryModes={retryAssistantModes}
          onTurnRunningChange={setAssistantTurnRunning}
          programController={programController}
          model={assistantModel}
        />
      )}
    </main>
    </DraftInputResetProvider>
    </AuthoringAdmissionTooltipProvider>
  );
}
