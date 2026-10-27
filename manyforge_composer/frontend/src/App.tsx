import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { DirectControlMotionStatus } from './types/robotControl';
import type { CSSProperties } from 'react';

import { ApiError, api } from './api/client';
import { AssistantOverlay } from './components/AssistantOverlay';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { deploymentDocumentHeader } from './components/DeploymentDocumentControls';
import {
  HeadlessRuntimeSurfaces,
  attentionAlarmsNewestFirst,
  partitionRepresentedAlarms,
} from './components/HeadlessRuntimeSurfaces';
import {
  RuntimeAttentionAutoOpen,
  shouldCloseAutomaticRuntimeAttention,
} from './components/RuntimeAttentionAutoOpen';
import { DocumentControls } from './components/DocumentControls';
import { programDocumentHeader } from './components/ProgramDocumentControls';
import { ScenarioDocumentCommands } from './components/ScenarioDocumentCommands';
import { OverlayPanel } from './components/OverlayPanel';
import { PaneDivider } from './components/PaneDivider';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { runtimeStatusPresentation } from './components/runtimeStatus';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import { WorkbenchNotificationStack } from './components/WorkbenchNotifications';
import {
  useHeadlessStateSource,
  useShellHeadlessState,
} from './hooks/useHeadlessState';
import { useAuthoringWorkspace } from './hooks/useAuthoringWorkspace';
import { useDeploymentWorkspace } from './hooks/useDeploymentWorkspace';
import { useManagerApplyStatus } from './hooks/useManagerApplyStatus';
import { activationFailureNotice } from './state/activationNotices';
import { isaacJointStreamNotices } from './state/isaacJointStreamNotices';
import { isMotionWaitRefusal } from './state/motionWaitRefusal';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useProgramWorkspace, type ScenarioReplacementIdentity } from './hooks/useProgramWorkspace';
import { useRobotControl } from './hooks/useRobotControl';
import { robotControlHasActiveOperation } from './state/robotControl';
import { useHeldNodePreview } from './hooks/useHeldNodePreview';
import { useResetApply } from './hooks/useResetApply';
import { useAppliedSettlementRefresh } from './hooks/useAppliedSettlementRefresh';
import { appliedCartesianDefaults } from './state/robotControl';
import { closeScenario as closeCurrentScenario, resetCloseSnapshot, type ClosePreparationInputs } from './state/scenarioClose';
import { useCCLC, waitForObservation } from './hooks/useCCLC';
import type { ManagerActiveProjection } from './types/api';
import {
  managerActivationSettlementKey,
  nodeEnvironmentRefreshKey,
  refreshOnManagerActivationSettled,
  refreshViewerModelsOnManagerChange,
  viewerModelRefreshKey,
} from './state/nodeEnvironmentRefresh';
import { useStructuralResource } from './state/structuralResource';
import { deriveProgramMotionConfiguration, programBlackboardKeys } from './utils/programAuthoring';
import { globalRuleIds } from './utils/interruptAuthoring';
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
import type { ProgramWorkspaceActionName } from './state/programWorkspace';
import {
  INSPECTOR_WIDTH_MIN,
  WORKBENCH_DIVIDER_WIDTH,
  inspectorWidthMax as centeredInspectorWidthMax,
} from './state/workbenchLayout';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

const INSPECTOR_SECTION_MIN_HEIGHT = 140;

const NO_MOTION_TARGETS: readonly DirectControlMotionStatus[] = [];

const PROGRAM_ACTION_LABELS: Partial<Record<ProgramWorkspaceActionName, string>> = {
  new_scenario_document: 'Creating and activating Scenario',
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

export default function App() {
  const [activeWorkspace, setActiveWorkspace] = useState<'program' | 'scene' | 'deployment'>('program');
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
  const [programRequirementCreated, setProgramRequirementCreated] = useState<{
    sequence: number;
    id: string;
    kind: 'component_role' | 'target_role';
  } | null>(null);
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
  const [assistantConfigured, setAssistantConfigured] = useState(false);
  const structuralRetryAttempt = useRef(0);
  const headlessEnabled = structuralMode === 'headless_client';
  const [catalog, retryCatalog] = useStructuralResource(api.catalog.list, {
    failureMessage: 'Could not read the immutable v2 node catalog.',
  });
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
    },
  );
  const nodeEnvironmentWakeKey = useRef<string | null>(null);
  const viewerModelWakeKey = useRef<string | null>(null);
  const managerActivationWakeKey = useRef<string | null>(null);
  useEffect(() => {
    if (health.status === 'ready') {
      structuralRetryAttempt.current = 0;
      setStructuralMode(health.data.mode);
      setAssistantConfigured(
        health.data.components.assistant_bridge === 'configured',
      );
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
  const assistantEnabled = headlessEnabled && assistantConfigured;
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
    retryNodeEnvironment();
  }, [headlessEnabled, headlessSnapshot, retryNodeEnvironment]);
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
      [retryCatalog, refreshDeploymentWorkspace],
    );
  }, [headlessEnabled, headlessSnapshot, refreshDeploymentWorkspace, retryCatalog]);
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
  // A successful Load/New clears a refused-startup notice in the backend;
  // health is read once per page, so read it again then.
  const programController = useProgramWorkspace(
    headlessSnapshot,
    authoringWorkspace,
    retryHealth,
  );
  // Draft / Saved / Applied of both documents, shown in every workspace.
  const programStates = programDocumentStates(programController.state.workspace);
  // Keys a global interrupt rule may read: the Program Draft's declared and
  // system keys (schema04 §3; T2 resolves them against the Applied Program).
  const programDraftAuthoring = programController.state.workspace?.draft.authoring ?? null;
  const deploymentDraftDocument = deploymentController.workspace?.draft.document ?? null;
  // Null without a Program Draft: a rule's user keys then wait for one (contract 106).
  const globalRuleProgramKeys = useMemo(() => (programDraftAuthoring === null ? null
    : programBlackboardKeys(programDraftAuthoring, globalRuleIds(deploymentDraftDocument))),
  [deploymentDraftDocument, programDraftAuthoring]);
  const deploymentStates = deploymentDocumentStates(deploymentController.workspace);
  const configuredComponentIds = useMemo(() => (
    deploymentController.workspace?.managerApplied.activationOrder?.componentIds ?? []
  ), [deploymentController.workspace?.managerApplied.activationOrder]);
  const cclc = useCCLC(headlessEnabled,
    headlessSnapshot?.authoringAdmission.admissionGeneration ?? null, configuredComponentIds);
  const { controllableComponentId: cclcComponentId, setControllableComponentId: selectCCLCComponent, refresh: refreshCCLC } = cclc;
  useOperationalErrorMirror({
    source: 'composer.cclc',
    title: 'Component lifecycle or route command failed',
    message: cclc.error,
    onDismiss: cclc.dismissError,
  });
  useEffect(() => {
    const next = configuredComponentIds.includes(cclcComponentId) ? cclcComponentId : configuredComponentIds[0] ?? '';
    if (next !== cclcComponentId) selectCCLCComponent(next);
  }, [cclcComponentId, configuredComponentIds, selectCCLCComponent]);
  useEffect(() => {
    if (headlessEnabled && configuredComponentIds.includes(cclcComponentId)) void refreshCCLC();
  }, [cclcComponentId, configuredComponentIds, headlessEnabled, refreshCCLC]);
  const programRuntime = useProgramRuntime(
    headlessEnabled,
    headlessSnapshot,
    programController.state.workspace,
    headlessStateSource,
    deploymentUnapplied(deploymentController.workspace),
    cclc,
  );
  // Manual motion uses Applied defaults; Draft changes wait for Apply.
  const appliedProgram = programRuntime.state.applied;
  // A global rule's execution_groups scope names the Applied Program's groups (contract 73).
  const appliedExecutionGroupIds = useMemo(() => (appliedProgram === null || appliedProgram === undefined ? null
    : Object.keys(appliedProgram.program.execution_groups ?? {}).sort()), [appliedProgram]);
  const programCartesianDefaults = useMemo(() => {
    if (catalog.status !== 'ready') return null;
    const defaults = (program: Parameters<typeof deriveProgramMotionConfiguration>[0]) => {
      const configuration = deriveProgramMotionConfiguration(program, catalog.data.entries);
      return configuration.available ? {
        translationToleranceM: configuration.configuration.values.translation_tol_m,
        maxBlendDeviationRad: configuration.configuration.values.max_blend_deviation_rad,
      } : null;
    };
    return appliedCartesianDefaults(appliedProgram === null
      ? null
      : defaults({ parameters: appliedProgram.program.parameters, forest: [appliedProgram.program.tree] }));
  }, [appliedProgram, catalog]);
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
  useOperationalErrorMirror({
    source: 'composer.program_runtime',
    title: 'Program Run, Stop, or Reset failed',
    // A motion-admission wait (contract 75) is an amber notice, not an error.
    message: isMotionWaitRefusal(programRuntime.state.error) ? null : programRuntime.state.error,
    onDismiss: programRuntime.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.runtime_scale',
    title: 'Runtime velocity update failed',
    message: robotControl.runtimeScaleError,
    onDismiss: robotControl.dismissRuntimeScaleError,
  });
  const robotOperationalError = robotControl.state.error !== null
    && !isMotionWaitRefusal(robotControl.state.error)
    && !robotControl.state.error.startsWith('The authored ')
    && !robotControl.state.error.startsWith('The stored ')
    ? robotControl.state.error
    : null;
  useOperationalErrorMirror({
    source: 'composer.robot_control',
    title: 'Robot control request failed',
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
  const closeAbort = useRef<AbortController | null>(null);
  const [scenarioCloseProgress, setScenarioCloseProgress] = useState<string | null>(null);
  useEffect(() => () => closeAbort.current?.abort(), []);
  const latestProgramRuntime = useRef(programRuntime);
  latestProgramRuntime.current = programRuntime;
  const closeCurrent = useCallback(async (expected: ManagerActiveProjection, confirmed?: ScenarioReplacementIdentity, callerSignal?: AbortSignal) => {
    if (closeAbort.current !== null) throw new Error('Scenario Close is already in progress.');
    const abort = new AbortController();
    closeAbort.current = abort;
    const abortFromCaller = () => abort.abort(callerSignal?.reason);
    callerSignal?.addEventListener('abort', abortFromCaller, { once: true });
    if (callerSignal?.aborted) abortFromCaller();
    try {
      const preparation: ClosePreparationInputs | undefined = confirmed === undefined ? undefined : { confirmed,
        reset: (options) => latestProgramRuntime.current.reset(options),
        // Close detaches with the latest attachment and Scene revisions; the
        // shell snapshot does not follow pose-only Scene commits.
        readResetSnapshot: () => resetCloseSnapshot({
          ...headlessStateSource.getShellSnapshot(), snapshot: headlessStateSource.getLatestSnapshot(),
        }),
        readSnapshot: () => {
          const current = headlessStateSource.getShellSnapshot();
          const latest = headlessStateSource.getLatestSnapshot();
          if (latest === null || current.transportStatus !== 'connected' || current.semanticStatus !== 'current')
            throw new Error('Control source is unavailable; Close is unresolved.');
          return latest;
        },
      };
      return await closeCurrentScenario({ expected, signal: abort.signal,
        preparation,
        admissionGeneration: () => {
          const state = headlessStateSource.getShellSnapshot();
          if (state.snapshot === null || state.transportStatus !== 'connected'
            || state.semanticStatus !== 'current') throw new Error('Control source is unavailable; Close is unresolved.');
          return state.snapshot.authoringAdmission.admissionGeneration;
        },
        waitForObservation: () => waitForObservation(abort.signal),
        trackApply: setTrackedApply, progress: setScenarioCloseProgress,
      });
    } finally {
      callerSignal?.removeEventListener('abort', abortFromCaller);
      closeAbort.current = null; setScenarioCloseProgress(null);
    }
  }, [headlessStateSource]);
  useEffect(() => {
    if (programController.acceptedApply !== null) setTrackedApply(programController.acceptedApply);
  }, [programController.acceptedApply]);
  useEffect(() => {
    if (deploymentController.acceptedApply !== null) setTrackedApply(deploymentController.acceptedApply);
  }, [deploymentController.acceptedApply]);
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
  const heldNodePreview = useHeldNodePreview({
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
      return [activationFailureNotice(
        id,
        observedApply.applyId,
        { state: activationStatus.state, failure: activationStatus.failure },
        savedScenarioPath,
        authoringAdmissionOpen,
      )];
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
  }, [activationStatus, activationUnobservable, authoringAdmissionOpen, maintenance, observedApply, savedScenarioPath]);
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
  const authoringCause = headlessEnabled
    ? authoringUnavailableCause(authoringWorkspace.published, headlessSnapshot)
    : null;
  const operationNotices = useMemo<WorkbenchNotice[]>(() => {
    const notices: WorkbenchNotice[] = [
      ...requiredComponentFaultNotices(headlessSnapshot),
      ...authoringContainmentNotices(headlessSnapshot),
      ...isaacJointStreamNotices(headlessSnapshot?.runtime.sections.sceneRuntime ?? null),
    ];
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
    } else if (authoringCause === 'not_admitted' && maintenance === null && !deploymentController.applying && pendingProgramAction === null) {
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
    for (const [id, message, dismiss] of [
      ['program', programRuntime.state.error, programRuntime.dismissError],
      ['robot', robotControl.state.error, robotControl.dismissError],
    ] as const) {
      if (message === null || !isMotionWaitRefusal(message)) continue;
      notices.push({
        id: `motion.wait.${id}`,
        severity: 'warning',
        domain: 'runtime',
        title: 'Motion waiting',
        message,
        actions: [{ id: 'dismiss', label: 'Dismiss', run: dismiss }],
      });
    }
    return notices;
  }, [
    programRuntime.state.error,
    programRuntime.dismissError,
    robotControl.state.error,
    robotControl.dismissError,
    authoringCause,
    authoringWorkspace.unavailableReason,
    controlNotices.length,
    deploymentController.applying,
    deploymentController.busy,
    pendingProgramAction,
    headlessSnapshot,
    maintenance,
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
    ...(activationCompleted === null ? [] : [activationCompleted]),
    ...(programMessage === null ? [] : [programMessage]),
    ...(deploymentMessage === null ? [] : [deploymentMessage]),
  ], [
    activationCompleted,
    activationNotices,
    controlNotices,
    deploymentMessage,
    operationNotices,
    programMessage,
    publishedNotices,
    sessionNotices,
    startupNotices,
    submissionNotices,
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
  const errorConditions = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'error'),
    [classifiedNotices],
  );
  const warningNotices = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'warning'),
    [classifiedNotices],
  );
  const activityNotices = useMemo(
    () => noticesOfSeverity(classifiedNotices, 'info'),
    [classifiedNotices],
  );
  useViewerIncidentReporting(
    errorConditions,
    headlessEnabled ? headlessStateSource.reportClientIncident : undefined,
  );
  const unacknowledgedAlarmCount = headlessSnapshot?.alarms.unacknowledgedCount ?? 0;
  // An alarm a current notice represents is counted once, with the notice.
  const representedAlarmCount = useMemo(() => [...partitionRepresentedAlarms(
    attentionAlarmsNewestFirst(headlessSnapshot?.alarms.alarms ?? []),
    errorConditions,
  ).represented.values()].reduce((count, alarms) => count + alarms.length, 0),
  [errorConditions, headlessSnapshot]);
  const errorItemCount = clientErrors.length
    + Math.max(0, unacknowledgedAlarmCount - representedAlarmCount)
    + errorConditions.length;
  const runtimeAttentionItems = useMemo(() => [
    ...errorConditions.map((condition) => ({
      identity: `condition:${condition.id}`,
    })),
    ...attentionAlarmsNewestFirst(headlessSnapshot?.alarms.alarms ?? []).map((alarm) => ({
      identity: `backend:${alarm.activationId}`,
    })),
    ...clientErrors.map((error) => ({
      identity: `composer:${error.id}`,
    })),
  ], [clientErrors, errorConditions, headlessSnapshot]);
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
    clientErrorCount: clientErrors.length,
  });
  const deploymentTitle = programInvalidEdit
    ? `Review or discard the invalid Program field edit first. Component status: ${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`
    : `${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`;
  // New/Save/Save as/Load act on the whole Scenario in every workspace.
  const scenarioCommands = (
    <ScenarioDocumentCommands
      controller={programController}
      deploymentDirty={deploymentController.workspace?.draft.differsFromSaved === true}
      localInvalidEdit={programInvalidEdit}
      closeScenario={closeCurrent}
      closeProgress={scenarioCloseProgress}
      motionActive={documentMotionActive}
    />
  );
  // Discard/Revert/Apply act on the whole Scenario in every workspace too.
  const scenarioActions = (
    <ScenarioActionControls
      program={programController}
      deployment={deploymentController}
      runtime={programRuntime}
      resetApply={resetApply}
      localInvalidEdit={programInvalidEdit}
      cclc={cclc}
      authoringAdmissionGeneration={headlessSnapshot?.authoringAdmission.admissionGeneration ?? null}
    />
  );

  return (
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
          {...(activeWorkspace === 'deployment'
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
              <span className="global-attention-count">
                {errorItemCount}
              </span>
            )}
          </button>
          <button
            ref={assistantButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'assistant'}
            disabled={!assistantEnabled}
            title={assistantEnabled ? 'Open the AI assistant.' : 'The assistant bridge is unavailable.'}
            onClick={() => setGlobalPanel('assistant')}
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
            <ProgramWorkspace
              controller={programController}
              runtime={programRuntime}
              heldNodePreview={heldNodePreview}
              catalog={catalog}
              environment={nodeEnvironment}
              sourceModel={sourceModel}
              deploymentWorkspace={deploymentController.workspace}
              programRoleAssignment={authoringWorkspace.project?.programRoleAssignment ?? null}
              motionDescriptors={robotControl.motionTargets.map(
                (target) => target.targetDescriptor,
              )}
              stopChains={robotControl.motionTargets}
              onRetryCatalog={retryCatalog}
              onRetryEnvironment={retryNodeEnvironment}
              onInvalidEditChange={setProgramInvalidEdit}
              onOpenScene={() => setActiveWorkspace('scene')}
              inspectorTarget={programInspectorTarget}
              inspectorMinimized={programInspectorMinimized}
              onInspectorMinimizedChange={setProgramInspectorMinimized}
              onInspectorMaximize={maximizeProgramInspector}
              onInspectorStateChange={handleProgramInspectorState}
              programRequirementCreated={programRequirementCreated}
              onProgramRequirementCreated={(kind, id) => setProgramRequirementCreated((current) => ({
                sequence: (current?.sequence ?? 0) + 1,
                kind,
                id,
              }))}
            />
          ) : activeWorkspace === 'scene' ? (
            <div
              ref={setSceneEditorTarget}
              className="scene-workspace-host"
              aria-label="Scene authoring workspace"
            />
          ) : (
            <div className="deployment-workspace">
              <DeploymentCompositionPanel
                cclc={cclc}
                controller={deploymentController}
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
                executionGroupIds={appliedExecutionGroupIds}
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
                  onReconnect: () => {
                    headlessState.reconnect();
                    if (structuralMode === null) retryHealth();
                  },
                  onOpenRuntimeHistory: openRuntimeDetails,
                }}
              />
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
                editorTarget={sceneEditorTarget}
                environment={nodeEnvironment}
                headlessStateSource={headlessStateSource}
                manifest={manifest}
                onRetryManifest={retryManifest}
                onRetrySourceModel={retrySourceModel}
                programController={programController}
                programRuntime={programRuntime}
                robotControl={robotControl}
                correctionGhost={heldNodePreview.ghost}
                runtimeEnabled={connectedProjection}
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
                        enabled={headlessEnabled
                          || errorConditions.length > 0
                          || clientErrors.length > 0}
                        headlessStateSource={headlessStateSource}
                        clientErrors={clientErrors}
                        conditions={errorConditions}
                        onAcknowledgeClientError={operationalErrors.dismiss}
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
      <OverlayPanel
        open={globalPanel === 'assistant'}
        title="AI assistant"
        titleId="assistant-overlay-title"
        onClose={() => setGlobalPanel(null)}
        returnFocusRef={assistantButton}
      >
        <AssistantOverlay
          enabled={assistantEnabled}
          headlessConnected={headlessEnabled}
          programController={programController}
        />
      </OverlayPanel>
    </main>
  );
}
