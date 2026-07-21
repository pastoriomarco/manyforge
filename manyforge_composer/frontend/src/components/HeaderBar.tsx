import { useCallback, useEffect, useRef, useState } from 'react';
import { api, ApiError } from '../api/client';
import { useCycleStore, useEditLocked } from '../store/cycleStore';
import {
  errorPresentationKey,
  headlineForError,
  KernelErrorPanel,
} from './KernelErrorPanel';
import { useProgramStore } from '../store/programStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { FileBrowserDialog } from './FileBrowserDialog';
import { ExecutionHistoryPanel } from './ExecutionHistoryPanel';
import {
  useOperatorAlertPolling,
  useOperatorAlertStore,
} from '../store/operatorAlertStore';
import type { FullDeployment, TreeNode } from '../types/api';

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

type BrowserIntent = 'load' | 'save';
type BrowserStep = 'program' | 'deployment';

interface BrowserSession {
  intent: BrowserIntent;
  step: BrowserStep;
  programPath: string;
  deploymentPath: string;
}

// Quiet, neutral chrome for every document/runtime action except Apply:
// per industrial-HMI practice, color is reserved for state and for the
// one primary commit action.
const NEUTRAL_BUTTON =
  'rounded border border-gray-600 bg-gray-800 px-3 py-1 text-sm font-medium text-gray-200 ' +
  'transition-colors hover:bg-gray-700 hover:text-white ' +
  'disabled:cursor-not-allowed disabled:border-gray-700 disabled:bg-gray-800/60 disabled:text-gray-600';

function confirmRuntimeReload(actionLabel: string, cycleState: string): boolean {
  return window.confirm(
    `${actionLabel} reloads the runtime session and can reset the backend-authored ` +
      'simulation/robot state to the deployment initial state.\n\n' +
      `Current cycle state: ${cycleState}. Any held/stopped execution context may be discarded.\n\n` +
      'Continue?',
  );
}

function IconButton({
  onClick,
  disabled,
  label,
  children,
}: {
  onClick: () => void;
  disabled?: boolean;
  label: string;
  children: React.ReactNode;
}) {
  return (
    <span className="group relative inline-flex">
      <button
        type="button"
        onClick={onClick}
        disabled={disabled}
        aria-label={label}
        className="rounded border border-gray-600 bg-gray-800 p-1.5 text-gray-300 transition-colors hover:bg-gray-700 hover:text-white disabled:cursor-not-allowed disabled:border-gray-700 disabled:bg-gray-800/60 disabled:text-gray-600"
      >
        {children}
      </button>
      {/* Instant tooltip: native title= has a ~1s delay; this shows on hover
          immediately so icon-only buttons stay self-explanatory. */}
      <span className="pointer-events-none absolute left-1/2 top-full z-50 mt-1.5 -translate-x-1/2 whitespace-nowrap rounded border border-gray-600 bg-gray-900 px-2 py-0.5 text-xs text-gray-200 opacity-0 shadow-lg shadow-black/50 transition-opacity duration-75 group-hover:opacity-100">
        {label}
      </span>
    </span>
  );
}

function StatusDot({
  ok,
  okLabel,
  pendingLabel,
  title,
}: {
  ok: boolean;
  okLabel: string;
  pendingLabel: string;
  title: string;
}) {
  return (
    <span
      className="flex items-center gap-1.5 font-mono text-[11px] uppercase tracking-wider"
      title={title}
    >
      <span
        className={`h-2 w-2 rounded-full ${ok ? 'bg-emerald-500' : 'bg-amber-400'}`}
      />
      <span className={ok ? 'text-gray-400' : 'text-amber-300'}>
        {ok ? okLabel : pendingLabel}
      </span>
    </span>
  );
}

function summarizeValidationErrors(
  errors: Array<{ path: string; message: string }>,
): string {
  const summary = errors
    .slice(0, 3)
    .map((issue) => `${issue.path}: ${issue.message}`)
    .join(' | ');
  if (errors.length <= 3) {
    return summary;
  }
  return `${summary} | …and ${errors.length - 3} more`;
}

export function HeaderBar() {
  useOperatorAlertPolling();
  const {
    state: cycleState,
    error,
    errorCode: cycleErrorCode,
    errorDetails: cycleErrorDetails,
  } = useCycleStore();
  const {
    alerts: operatorAlerts,
    blockingCount: operatorAlertCount,
    loaded: operatorAlertsLoaded,
    busy: operatorAlertsBusy,
    error: operatorAlertsError,
    acknowledge: acknowledgeOperatorAlert,
    acknowledgeAll: acknowledgeAllOperatorAlerts,
  } = useOperatorAlertStore();
  const {
    flowToTree,
    appliedTree,
    savedTree,
    programParameters,
    appliedProgramParameters,
    savedProgramParameters,
    programCompatibility,
    appliedProgramCompatibility,
    savedProgramCompatibility,
    viewerSettings,
    appliedViewerSettings,
    savedViewerSettings,
    sceneResources,
    appliedSceneResources,
    savedSceneResources,
    blackboardNamespaces,
    appliedBlackboardNamespaces,
    savedBlackboardNamespaces,
    blackboardKeys,
    appliedBlackboardKeys,
    savedBlackboardKeys,
    discardToApplied: discardProgramToApplied,
    setAppliedToDraft,
    saveProgram,
    loadProgram,
    fetchProgramInfo,
    fetchTree,
    validateDraftGraph,
    programInfo,
    runtimeOverrideStatus,
    error: programError,
    loading: programLoading,
  } = useProgramStore();
  const {
    deployment,
    appliedDeployment,
    savedDeployment,
    sourcePath: deploymentSourcePath,
    saveDeployment,
    fetchDeployment,
    discardToApplied: discardDeploymentToApplied,
    setAppliedToDraft: setAppliedDeploymentToDraft,
    error: deploymentError,
  } = useDeploymentStore();

  // Structured error capture for the KernelErrorPanel: when a cycle command
  // (run/stop/reset) is rejected, the backend returns a 409 with a
  // `detail.{message, errorCode, errorDetails}` payload that the API client
  // surfaces via ApiError. We hold both the message and the structured
  // fields so the panel can render a cause headline plus the diagnostics
  // map. Frontend-only errors (validation, transport) still set message-
  // only and the panel falls back to a generic banner.
  interface ActionErrorState {
    message: string;
    errorCode: string | null;
    errorDetails: Record<string, unknown> | null;
  }
  const [actionError, setActionErrorState] = useState<ActionErrorState | null>(null);
  const setActionError = useCallback((value: string | null | unknown) => {
    if (value === null || value === undefined) {
      setActionErrorState(null);
      return;
    }
    if (value instanceof ApiError) {
      setActionErrorState({
        message: value.message,
        errorCode: value.errorCode,
        errorDetails: value.errorDetails,
      });
      return;
    }
    if (value instanceof Error) {
      setActionErrorState({
        message: value.message,
        errorCode: null,
        errorDetails: null,
      });
      return;
    }
    if (typeof value === 'string') {
      setActionErrorState({ message: value, errorCode: null, errorDetails: null });
      return;
    }
    setActionErrorState({
      message: String(value),
      errorCode: null,
      errorDetails: null,
    });
  }, []);
  const [busy, setBusy] = useState(false);
  const [browserSession, setBrowserSession] = useState<BrowserSession | null>(null);

  const draftTree = flowToTree();
  const programApplied = signature(draftTree) === signature(appliedTree);
  const programSaved = signature(draftTree) === signature(savedTree);
  const sceneApplied = signature(sceneResources) === signature(appliedSceneResources);
  const sceneSaved = signature(sceneResources) === signature(savedSceneResources);
  const programParametersApplied =
    signature(programParameters) === signature(appliedProgramParameters);
  const programParametersSaved =
    signature(programParameters) === signature(savedProgramParameters);
  const programCompatibilityApplied =
    signature(programCompatibility) === signature(appliedProgramCompatibility);
  const programCompatibilitySaved =
    signature(programCompatibility) === signature(savedProgramCompatibility);
  const viewerSettingsApplied =
    signature(viewerSettings) === signature(appliedViewerSettings);
  const viewerSettingsSaved =
    signature(viewerSettings) === signature(savedViewerSettings);
  const blackboardNamespacesApplied =
    signature(blackboardNamespaces) === signature(appliedBlackboardNamespaces);
  const blackboardNamespacesSaved =
    signature(blackboardNamespaces) === signature(savedBlackboardNamespaces);
  const blackboardKeysApplied =
    signature(blackboardKeys) === signature(appliedBlackboardKeys);
  const blackboardKeysSaved =
    signature(blackboardKeys) === signature(savedBlackboardKeys);
  const programAppliedMatchesSaved =
    signature(appliedTree) === signature(savedTree)
    && signature(appliedSceneResources) === signature(savedSceneResources)
    && signature(appliedProgramParameters) === signature(savedProgramParameters)
    && signature(appliedProgramCompatibility) === signature(savedProgramCompatibility)
    && signature(appliedViewerSettings) === signature(savedViewerSettings)
    && signature(appliedBlackboardNamespaces) === signature(savedBlackboardNamespaces)
    && signature(appliedBlackboardKeys) === signature(savedBlackboardKeys);
  const deploymentApplied = signature(deployment) === signature(appliedDeployment);
  const deploymentSaved = signature(deployment) === signature(savedDeployment);
  const deploymentAppliedMatchesSaved = signature(appliedDeployment) === signature(savedDeployment);
  const hasUnappliedChanges =
    !programApplied
    || !sceneApplied
    || !programParametersApplied
    || !programCompatibilityApplied
    || !viewerSettingsApplied
    || !blackboardNamespacesApplied
    || !blackboardKeysApplied
    || !deploymentApplied;
  const hasRuntimeModelUnappliedChanges =
    !programApplied
    || !sceneApplied
    || !programParametersApplied
    || !programCompatibilityApplied
    || !blackboardNamespacesApplied
    || !blackboardKeysApplied
    || !deploymentApplied;
  const hasUnsavedChanges =
    !programSaved
    || !sceneSaved
    || !programParametersSaved
    || !programCompatibilitySaved
    || !viewerSettingsSaved
    || !blackboardNamespacesSaved
    || !blackboardKeysSaved
    || !deploymentSaved;
  const hasAppliedUnsavedChanges = !programAppliedMatchesSaved || !deploymentAppliedMatchesSaved;
  const hasRuntimeModelAppliedUnsavedChanges =
    signature(appliedTree) !== signature(savedTree)
    || signature(appliedSceneResources) !== signature(savedSceneResources)
    || signature(appliedProgramParameters) !== signature(savedProgramParameters)
    || signature(appliedProgramCompatibility) !== signature(savedProgramCompatibility)
    || signature(appliedBlackboardNamespaces) !== signature(savedBlackboardNamespaces)
    || signature(appliedBlackboardKeys) !== signature(savedBlackboardKeys)
    || !deploymentAppliedMatchesSaved;
  // Runtime incidents come from the server inbox. Keep a single local fallback
  // only for browser-side program/deployment failures (and for the brief first
  // render before the inbox has loaded). Once the inbox is loaded, an
  // acknowledged runtime alert must not be recreated from the still-latched
  // cycle error.
  let localError: ActionErrorState | null = null;
  if (operatorAlertCount === 0) {
    if (actionError) {
      localError = actionError;
    } else if ((!operatorAlertsLoaded || operatorAlertsError) && error) {
      localError = {
        message: error,
        errorCode: cycleErrorCode ?? null,
        errorDetails: cycleErrorDetails ?? null,
      };
    } else if (programError || deploymentError) {
      localError = {
        message: programError || deploymentError!,
        errorCode: null,
        errorDetails: null,
      };
    }
  }
  const localErrorFingerprint = localError
    ? `${localError.message}|${localError.errorCode ?? ''}|${signature(localError.errorDetails)}`
    : '';
  const localErrorPresentationKey = localError
    ? errorPresentationKey(
        localError.message,
        localError.errorCode,
        localError.errorDetails,
      )
    : '';
  const localErrorHeadline = localError
    ? headlineForError(
        localError.errorCode,
        localError.errorDetails,
        localError.message,
      )
    : '';
  const [minimizedLocalErrorKey, setMinimizedLocalErrorKey] = useState<string | null>(null);
  const [acknowledgedLocalErrorKey, setAcknowledgedLocalErrorKey] = useState<string | null>(null);
  useEffect(() => {
    if (!localErrorPresentationKey) {
      setMinimizedLocalErrorKey(null);
      setAcknowledgedLocalErrorKey(null);
    }
  }, [localErrorPresentationKey]);
  const localErrorIsMinimized = Boolean(
    localErrorPresentationKey
    && minimizedLocalErrorKey === localErrorPresentationKey,
  );
  const localErrorIsAcknowledged = Boolean(
    localErrorPresentationKey
    && acknowledgedLocalErrorKey === localErrorPresentationKey,
  );
  const [operatorAlertStackMinimized, setOperatorAlertStackMinimized] = useState(false);
  const [showAcknowledgeAllConfirm, setShowAcknowledgeAllConfirm] = useState(false);
  const [showExecutionHistory, setShowExecutionHistory] = useState(false);
  const previousOperatorAlertIds = useRef<string[]>([]);
  const operatorAlertIdsKey = operatorAlerts.map((alert) => alert.alertId).join('|');
  useEffect(() => {
    const previous = new Set(previousOperatorAlertIds.current);
    const hasNewAlert = operatorAlerts.some((alert) => !previous.has(alert.alertId));
    if (hasNewAlert) setOperatorAlertStackMinimized(false);
    if (operatorAlertCount === 0) {
      setOperatorAlertStackMinimized(false);
      setShowAcknowledgeAllConfirm(false);
    }
    previousOperatorAlertIds.current = operatorAlerts.map((alert) => alert.alertId);
  }, [operatorAlertCount, operatorAlertIdsKey, operatorAlerts]);

  const [incidentSnapshotsById, setIncidentSnapshotsById] = useState<
    Record<string, Record<string, unknown>>
  >({});
  const alertIncidentKey = operatorAlerts
    .map((alert) => `${alert.alertId}:${alert.revision}:${alert.incidentId}`)
    .join('|');
  useEffect(() => {
    if (!alertIncidentKey && !localErrorFingerprint) {
      setIncidentSnapshotsById({});
      return;
    }
    let cancelled = false;
    void api.execution.channels(80)
      .then((payload) => {
        if (cancelled) {
          return;
        }
        const byId: Record<string, Record<string, unknown>> = {};
        payload.incidentSnapshots.forEach((snapshot) => {
          const incidentId = snapshot.incidentId;
          if (typeof incidentId === 'string') byId[incidentId] = snapshot;
        });
        setIncidentSnapshotsById(byId);
      })
      .catch(() => {
        if (!cancelled) setIncidentSnapshotsById({});
      });
    return () => {
      cancelled = true;
    };
  }, [alertIncidentKey, localErrorFingerprint]);
  const localIncidentSnapshot = Object.values(incidentSnapshotsById).at(-1) ?? null;

  const acknowledgeOneAlert = useCallback(async (alertId: string) => {
    try {
      await acknowledgeOperatorAlert(alertId);
      setActionError(null);
    } catch (reason) {
      setActionError(reason);
    }
  }, [acknowledgeOperatorAlert, setActionError]);

  const confirmAcknowledgeAll = useCallback(async () => {
    setShowAcknowledgeAllConfirm(false);
    try {
      await acknowledgeAllOperatorAlerts();
      setActionError(null);
    } catch (reason) {
      setActionError(reason);
    }
  }, [acknowledgeAllOperatorAlerts, setActionError]);
  const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);

  const applyDrafts = useCallback(async (forceDiscardOverrides: boolean) => {
    const payload: {
      tree?: TreeNode;
      scene?: typeof sceneResources;
      deployment?: FullDeployment;
      programParameters?: typeof programParameters;
      compatibility?: typeof programCompatibility;
      viewer?: typeof viewerSettings;
      blackboard?: { namespaces: typeof blackboardNamespaces; keys: typeof blackboardKeys };
    } = {};
    const draftGraphError = validateDraftGraph();
    if (!programApplied && draftGraphError) {
      throw new Error(draftGraphError);
    }
    if (!programApplied && draftTree) {
      payload.tree = draftTree;
    } else if (!programApplied) {
      throw new Error('Draft tree is empty or invalid');
    }
    if (!sceneApplied) {
      payload.scene = sceneResources;
    }
    if (!deploymentApplied && deployment) {
      payload.deployment = deployment;
    }
    if (!programParametersApplied) {
      payload.programParameters = programParameters;
    }
    if (!programCompatibilityApplied) {
      payload.compatibility = programCompatibility;
    }
    if (!viewerSettingsApplied) {
      payload.viewer = viewerSettings;
    }
    if (!blackboardNamespacesApplied || !blackboardKeysApplied) {
      payload.blackboard = {
        namespaces: blackboardNamespaces,
        keys: blackboardKeys,
      };
    }
    if (!payload.tree && !payload.scene && !payload.deployment && !payload.programParameters && !payload.blackboard && payload.compatibility === undefined && payload.viewer === undefined) {
      return;
    }

    if (payload.tree || payload.programParameters || payload.blackboard) {
      const validation = await api.program.validate(
        draftTree!,
        programParameters,
        {
          namespaces: blackboardNamespaces,
          keys: blackboardKeys,
        },
      );
      if (!validation.valid) {
        throw new Error(summarizeValidationErrors(validation.errors));
      }
    }

    await api.session.apply({
      ...payload,
      forceDiscardOverrides,
    });
    if (payload.tree || payload.scene || payload.programParameters || payload.blackboard || payload.compatibility !== undefined || payload.viewer !== undefined) {
      setAppliedToDraft();
    }
    if (payload.deployment) {
      setAppliedDeploymentToDraft();
    }
  }, [
    deployment,
    deploymentApplied,
    draftTree,
    programApplied,
    sceneApplied,
    sceneResources,
    programParameters,
    programParametersApplied,
    programCompatibility,
    programCompatibilityApplied,
    viewerSettings,
    viewerSettingsApplied,
    blackboardNamespaces,
    blackboardNamespacesApplied,
    blackboardKeys,
    blackboardKeysApplied,
    validateDraftGraph,
    setAppliedDeploymentToDraft,
    setAppliedToDraft,
  ]);

  const saveDrafts = useCallback(async () => {
    const operations: Promise<unknown>[] = [];
    const programArtifactSaved =
      programSaved
      && sceneSaved
      && programParametersSaved
      && programCompatibilitySaved
      && viewerSettingsSaved
      && blackboardNamespacesSaved
      && blackboardKeysSaved;
    if (!programArtifactSaved && programInfo?.sourcePath) {
      operations.push(saveProgram(programInfo.sourcePath));
    }
    if (!deploymentSaved && deployment && deploymentSourcePath) {
      operations.push(saveDeployment(deploymentSourcePath));
    }
    if (operations.length === 0) {
      return;
    }
    await Promise.all(operations);
  }, [
    deployment,
    deploymentSaved,
    deploymentSourcePath,
    sceneSaved,
    programParametersSaved,
    programCompatibilitySaved,
    viewerSettingsSaved,
    blackboardNamespacesSaved,
    blackboardKeysSaved,
    programInfo?.sourcePath,
    programSaved,
    saveDeployment,
    saveProgram,
  ]);

  const handleApply = useCallback(async () => {
    if (
      hasRuntimeModelUnappliedChanges
      && !confirmRuntimeReload('Applying draft changes', cycleState)
    ) {
      return;
    }
    if (
      hasRuntimeModelUnappliedChanges
      && hasRuntimeOverrides
      && !window.confirm(
        'Applying draft changes will discard active runtime-only patches created by recovery/LLM workflows. Continue?',
      )
    ) {
      return;
    }
    try {
      setBusy(true);
      setActionError(null);
      await applyDrafts(hasRuntimeOverrides && hasRuntimeModelUnappliedChanges);
    } catch (e) {
      setActionError(e);
    } finally {
      setBusy(false);
    }
  }, [
    applyDrafts,
    cycleState,
    hasRuntimeModelUnappliedChanges,
    hasRuntimeOverrides,
    setActionError,
  ]);

  const handleSaveAll = useCallback(async () => {
    try {
      setBusy(true);
      setActionError(null);
      await saveDrafts();
    } catch (e) {
      setActionError(e);
    } finally {
      setBusy(false);
    }
  }, [saveDrafts, setActionError]);

  const handleSaveAs = useCallback(async () => {
    if (!programInfo && !deployment) {
      return;
    }
    setActionError(null);
    setBrowserSession({
      intent: 'save',
      step: programInfo ? 'program' : 'deployment',
      programPath: programInfo?.sourcePath ?? '',
      deploymentPath: deploymentSourcePath || deployment?.sourcePath || '',
    });
  }, [deployment, deploymentSourcePath, programInfo, setActionError]);

  const handleLoad = useCallback(async () => {
    const hasDraftChanges = hasUnappliedChanges || hasUnsavedChanges;
    if (
      hasDraftChanges &&
      !window.confirm('Discard current draft changes and load another program?')
    ) {
      return;
    }
    if (
      hasRuntimeOverrides &&
      !window.confirm(
        'Loading a different program will discard active runtime-only patches created by recovery/LLM workflows. Continue?',
      )
    ) {
      return;
    }
    setActionError(null);
    setBrowserSession({
      intent: 'load',
      step: 'program',
      programPath: programInfo?.sourcePath ?? '',
      deploymentPath: deploymentSourcePath || '',
    });
  }, [
    deploymentSourcePath,
    hasUnappliedChanges,
    hasUnsavedChanges,
    programInfo?.sourcePath,
    hasRuntimeOverrides,
    setActionError,
  ]);

  const finalizeBrowserSession = useCallback(
    async (session: BrowserSession) => {
      try {
        setBusy(true);
        setActionError(null);

        if (session.intent === 'load') {
          await loadProgram(
            session.programPath,
            session.deploymentPath.trim() === '' ? undefined : session.deploymentPath,
            hasRuntimeOverrides,
          );
          await Promise.all([fetchProgramInfo(), fetchTree(), fetchDeployment()]);
        } else {
          const operations: Promise<unknown>[] = [];
          if (programInfo && session.programPath.trim()) {
            operations.push(saveProgram(session.programPath.trim()));
          }
          if (deployment && session.deploymentPath.trim()) {
            operations.push(saveDeployment(session.deploymentPath.trim()));
          }
          await Promise.all(operations);
        }
      } catch (e) {
        setActionError(e);
      } finally {
        setBusy(false);
        setBrowserSession(null);
      }
    },
    [
      deployment,
      fetchDeployment,
      fetchProgramInfo,
      fetchTree,
      loadProgram,
      programInfo,
      hasRuntimeOverrides,
      saveDeployment,
      saveProgram,
      setActionError,
    ],
  );

  const advanceBrowserSession = useCallback(
    (path: string) => {
      if (!browserSession) {
        return;
      }

      if (browserSession.step === 'program') {
        const nextSession: BrowserSession = {
          ...browserSession,
          programPath: path,
        };

        if (deployment) {
          setBrowserSession({
            ...nextSession,
            step: 'deployment',
          });
          return;
        }

        void finalizeBrowserSession(nextSession);
        return;
      }

      void finalizeBrowserSession({
        ...browserSession,
        deploymentPath: path,
      });
    },
    [browserSession, deployment, finalizeBrowserSession],
  );

  const skipBrowserDeployment = useCallback(() => {
    if (!browserSession) {
      return;
    }
    void finalizeBrowserSession({
      ...browserSession,
      deploymentPath: '',
    });
  }, [browserSession, finalizeBrowserSession]);

  const closeBrowserSession = useCallback(() => {
    setBrowserSession(null);
  }, []);

  const discardDrafts = useCallback(() => {
    discardProgramToApplied();
    discardDeploymentToApplied();
    setActionError(null);
  }, [discardDeploymentToApplied, discardProgramToApplied, setActionError]);

  const handleDiscard = useCallback(() => {
    discardDrafts();
  }, [discardDrafts]);

  const handleRevert = useCallback(async () => {
    if (!hasAppliedUnsavedChanges) {
      return;
    }
    if (
      hasRuntimeModelAppliedUnsavedChanges
      &&
      !window.confirm(
        'Reverting restores the applied session to the last saved program/deployment, ' +
          'reloads the runtime, and can reset the backend-authored simulation/robot ' +
          `state to the deployment initial state.\n\nCurrent cycle state: ${cycleState}. ` +
          'Current draft edits and any held/stopped execution context may be discarded.\n\nContinue?',
      )
    ) {
      return;
    }
    if (
      hasRuntimeModelAppliedUnsavedChanges
      && hasRuntimeOverrides &&
      !window.confirm(
        'Reverting to the saved baseline will also discard active runtime-only patches created by recovery/LLM workflows. Continue?',
      )
    ) {
      return;
    }
    const payload: {
      tree?: TreeNode;
      scene?: typeof sceneResources;
      deployment?: FullDeployment;
      programParameters?: typeof programParameters;
      compatibility?: typeof savedProgramCompatibility;
      viewer?: typeof savedViewerSettings;
      blackboard?: {
        namespaces: typeof savedBlackboardNamespaces;
        keys: typeof savedBlackboardKeys;
      };
      forceDiscardOverrides?: boolean;
    } = {
      forceDiscardOverrides:
        hasRuntimeOverrides && hasRuntimeModelAppliedUnsavedChanges,
    };
    if (!programAppliedMatchesSaved && savedTree) {
      payload.tree = savedTree;
    }
    if (signature(appliedSceneResources) !== signature(savedSceneResources)) {
      payload.scene = savedSceneResources;
    }
    if (signature(appliedProgramParameters) !== signature(savedProgramParameters)) {
      payload.programParameters = savedProgramParameters;
    }
    if (signature(appliedProgramCompatibility) !== signature(savedProgramCompatibility)) {
      payload.compatibility = savedProgramCompatibility;
    }
    if (signature(appliedViewerSettings) !== signature(savedViewerSettings)) {
      payload.viewer = savedViewerSettings;
    }
    if (
      signature(appliedBlackboardNamespaces) !== signature(savedBlackboardNamespaces)
      || signature(appliedBlackboardKeys) !== signature(savedBlackboardKeys)
    ) {
      payload.blackboard = {
        namespaces: savedBlackboardNamespaces,
        keys: savedBlackboardKeys,
      };
    }
    if (!deploymentAppliedMatchesSaved && savedDeployment) {
      payload.deployment = savedDeployment;
    }
    if (!payload.tree && !payload.scene && !payload.deployment && !payload.programParameters && !payload.blackboard && payload.compatibility === undefined && payload.viewer === undefined) {
      return;
    }
    try {
      setBusy(true);
      setActionError(null);
      await api.session.apply(payload);
      await Promise.all([
        payload.tree ? fetchTree() : Promise.resolve(true),
        payload.deployment ? fetchDeployment() : Promise.resolve(true),
        payload.tree || payload.programParameters || payload.blackboard
          || payload.compatibility !== undefined
          || payload.viewer !== undefined
          || payload.scene
          ? fetchProgramInfo()
          : Promise.resolve(true),
      ]);
    } catch (e) {
      setActionError(e);
    } finally {
      setBusy(false);
    }
  }, [
    deploymentAppliedMatchesSaved,
    fetchDeployment,
    fetchProgramInfo,
    fetchTree,
    hasAppliedUnsavedChanges,
    hasRuntimeModelAppliedUnsavedChanges,
    hasRuntimeOverrides,
    appliedProgramParameters,
    appliedProgramCompatibility,
    appliedViewerSettings,
    appliedSceneResources,
    appliedBlackboardKeys,
    appliedBlackboardNamespaces,
    programAppliedMatchesSaved,
    savedDeployment,
    savedSceneResources,
    savedBlackboardKeys,
    savedBlackboardNamespaces,
    savedProgramParameters,
    savedProgramCompatibility,
    savedViewerSettings,
    savedTree,
    setActionError,
    cycleState,
  ]);

  const cycleEditLocked = useEditLocked();
  const editLocked = cycleEditLocked || busy;
  const canSaveAs = (!editLocked && !!programInfo) || (!editLocked && !!deployment);

  return (
    <div className="relative z-40 shrink-0 border-b border-gray-700 bg-gray-900">
      <div className="flex h-11 items-center gap-3 px-3">
        {/* Program identity + passive state indicators */}
        <div className="flex min-w-0 items-center gap-3">
          <span
            className="truncate text-sm font-semibold text-gray-100"
            title={programInfo?.sourcePath ?? 'No program loaded'}
          >
            {programInfo?.name ?? 'No program'}
          </span>
          <StatusDot
            ok={!hasUnappliedChanges}
            okLabel="applied"
            pendingLabel="modified"
            title="Whether the draft matches the applied runtime session"
          />
          <StatusDot
            ok={!hasUnsavedChanges}
            okLabel="saved"
            pendingLabel="unsaved"
            title="Whether the draft matches the files on disk"
          />
          {programLoading && (
            <span className="text-xs text-gray-400">Loading program…</span>
          )}
          {operatorAlertCount > 0 && (
            <button
              type="button"
              onClick={() => setOperatorAlertStackMinimized((value) => !value)}
              className="relative rounded border border-red-500/80 bg-red-950/70 p-1 text-red-100 transition-colors hover:bg-red-900/80"
              aria-label={`${operatorAlertStackMinimized ? 'Show' : 'Minimize'} ${operatorAlertCount} unacknowledged error alerts`}
              title={`${operatorAlertCount} unacknowledged error alert${operatorAlertCount === 1 ? '' : 's'}`}
            >
              <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.6" aria-hidden>
                <path d="M8 2.2 14 13H2L8 2.2Z" strokeLinejoin="round" />
                <path d="M8 5.5v3.8M8 11.6v.2" strokeLinecap="round" />
              </svg>
              <span className="absolute -right-2 -top-2 min-w-4 rounded-full bg-red-500 px-1 text-center text-[9px] font-bold leading-4 text-white">
                {operatorAlertCount > 99 ? '99+' : operatorAlertCount}
              </span>
            </button>
          )}
          {localError && localErrorIsMinimized && (
            <button
              type="button"
              onClick={() => setMinimizedLocalErrorKey(null)}
              className={`relative rounded border p-1 transition-colors ${
                localErrorIsAcknowledged
                  ? 'border-red-900/70 bg-gray-800 text-red-300 hover:bg-gray-700'
                  : 'border-red-500/80 bg-red-950/70 text-red-100 hover:bg-red-900/80'
              }`}
              aria-label={`Show active error: ${localErrorHeadline}`}
              title={localErrorHeadline}
            >
              <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.6" aria-hidden>
                <path d="M8 2.2 14 13H2L8 2.2Z" strokeLinejoin="round" />
                <path d="M8 5.5v3.8M8 11.6v.2" strokeLinecap="round" />
              </svg>
            </button>
          )}
          <button
            type="button"
            onClick={() => setShowExecutionHistory((value) => !value)}
            className="relative rounded border border-gray-600 bg-gray-800 p-1 text-gray-300 transition-colors hover:bg-gray-700 hover:text-white"
            aria-label="Show messages and execution history"
            title="Messages and execution history"
          >
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.5" aria-hidden>
              <path d="M2.5 3.5h11v8h-7L3 14v-2.5h-.5z" strokeLinejoin="round" />
              <path d="M5 6h6M5 8.5h4" strokeLinecap="round" />
            </svg>
            {(operatorAlertCount > 0 || operatorAlertsError) && (
              <span className={`absolute -right-1 -top-1 h-2 w-2 rounded-full ${operatorAlertsError ? 'bg-amber-400' : 'bg-red-400'}`} />
            )}
          </button>
        </div>

        {/* Document group, then runtime group — Apply is the one accent */}
        <div className="ml-auto flex items-center gap-1.5">
          <IconButton
            onClick={() => void handleSaveAll()}
            disabled={!hasUnsavedChanges || editLocked}
            label="Save"
          >
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.4" aria-hidden>
              <path d="M3 2.5h8L13.5 5v8.5a.5.5 0 0 1-.5.5H3a.5.5 0 0 1-.5-.5v-10a.5.5 0 0 1 .5-.5z" strokeLinejoin="round" />
              <path d="M5 2.5V6h5.5V2.5M5 14v-4.5h6V14" strokeLinejoin="round" />
            </svg>
          </IconButton>
          <IconButton onClick={() => void handleSaveAs()} disabled={!canSaveAs} label="Save As…">
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.4" aria-hidden>
              <path d="M3 2.5h6.5L12 5v3" strokeLinejoin="round" />
              <path d="M3 2.5a.5.5 0 0 0-.5.5v10a.5.5 0 0 0 .5.5h4.5" strokeLinejoin="round" />
              <path d="M5 2.5V5.5h4V2.5" strokeLinejoin="round" />
              <path d="M13.7 9.6l-3.9 3.9-1.4.4.4-1.4 3.9-3.9a1 1 0 0 1 1 1z" strokeLinejoin="round" />
            </svg>
          </IconButton>
          <IconButton onClick={() => void handleLoad()} disabled={editLocked} label="Load…">
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.4" aria-hidden>
              <path d="M2 4.5a.5.5 0 0 1 .5-.5H6l1.5 1.5h6a.5.5 0 0 1 .5.5V7H2z" strokeLinejoin="round" />
              <path d="M2 7h12.5l-1.6 5.6a.5.5 0 0 1-.5.4H3.3a.5.5 0 0 1-.5-.4L2 7z" strokeLinejoin="round" />
            </svg>
          </IconButton>
          <div className="mx-1.5 h-6 w-px bg-gray-600/80" />
          <button
            onClick={handleDiscard}
            disabled={!hasUnappliedChanges || editLocked}
            title="Throw away draft edits and return to the applied session."
            className={NEUTRAL_BUTTON}
          >
            Discard
          </button>
          <button
            onClick={handleRevert}
            disabled={!hasAppliedUnsavedChanges || editLocked}
            title="Restore the applied runtime session to the last saved program/deployment."
            className={NEUTRAL_BUTTON}
          >
            Revert
          </button>
          <button
            onClick={handleApply}
            disabled={!hasUnappliedChanges || editLocked}
            title="Commit the draft to the runtime session."
            className="rounded border border-sky-500 bg-sky-700 px-4 py-1 text-sm font-semibold text-white transition-colors hover:bg-sky-600 disabled:cursor-not-allowed disabled:border-gray-700 disabled:bg-gray-800/60 disabled:text-gray-600"
          >
            Apply
          </button>
        </div>
      </div>

      {operatorAlertCount > 0 && !operatorAlertStackMinimized && (
        <div className="pointer-events-none absolute inset-x-3 top-full z-50 max-h-[calc(100vh-4rem)] overflow-y-auto pt-2">
          <div className="pointer-events-auto mb-2 flex flex-wrap items-center gap-2 rounded border border-red-800/70 bg-gray-950/90 px-3 py-2 text-xs text-gray-200 shadow-xl shadow-black/50 backdrop-blur-sm">
            <span className="font-semibold text-red-100">
              {operatorAlertCount} unacknowledged error alert{operatorAlertCount === 1 ? '' : 's'}
            </span>
            <span className="text-gray-400">
              Showing newest {Math.min(3, operatorAlertCount)}
            </span>
            {operatorAlertsError && (
              <span
                className="max-w-md truncate text-red-300"
                title={operatorAlertsError}
              >
                {operatorAlertsError}
              </span>
            )}
            <div className="ml-auto flex items-center gap-2">
              <button
                type="button"
                disabled={operatorAlertsBusy}
                onClick={() => setShowAcknowledgeAllConfirm(true)}
                className="rounded border border-red-500/60 bg-red-950/50 px-2 py-1 font-medium text-red-100 hover:bg-red-900/60 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Acknowledge all
              </button>
              <button
                type="button"
                onClick={() => setOperatorAlertStackMinimized(true)}
                className="rounded p-1 text-gray-400 hover:bg-gray-800 hover:text-white"
                aria-label="Minimize alerts to header icon"
                title="Minimize alerts to header icon"
              >
                <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.6" aria-hidden>
                  <path d="M3 8h10" strokeLinecap="round" />
                </svg>
              </button>
            </div>
          </div>
          {showAcknowledgeAllConfirm && (
            <div
              role="dialog"
              aria-label="Acknowledge all error alerts confirmation"
              className="pointer-events-auto mb-2 rounded border border-red-500/70 bg-gray-950 px-3 py-3 text-xs text-red-100 shadow-xl shadow-black/60"
            >
              <div className="font-semibold">Acknowledge all {operatorAlertCount} alerts?</div>
              <div className="mt-1 text-red-200/80">
                This marks every currently listed error as seen. It does not clear a fault,
                reset the cycle, resume execution, or move the robot. If a new alert arrives
                before confirmation, the backend will reject this request.
              </div>
              <div className="mt-2 flex gap-2">
                <button
                  type="button"
                  disabled={operatorAlertsBusy}
                  onClick={() => void confirmAcknowledgeAll()}
                  className="rounded border border-red-400/70 bg-red-800 px-2.5 py-1 font-semibold text-white hover:bg-red-700 disabled:opacity-50"
                >
                  Confirm acknowledge all
                </button>
                <button
                  type="button"
                  disabled={operatorAlertsBusy}
                  onClick={() => setShowAcknowledgeAllConfirm(false)}
                  className="rounded border border-gray-600 bg-gray-800 px-2.5 py-1 font-semibold text-gray-200 hover:bg-gray-700 disabled:opacity-50"
                >
                  Cancel
                </button>
              </div>
            </div>
          )}
          <div className="grid gap-2">
            {operatorAlerts.map((alert) => (
              <KernelErrorPanel
                key={alert.alertId}
                message={alert.message}
                errorCode={alert.errorCode}
                errorDetails={alert.errorDetails}
                incidentSnapshot={incidentSnapshotsById[alert.incidentId] ?? null}
                presentationKey={alert.alertId}
                onAcknowledge={() => void acknowledgeOneAlert(alert.alertId)}
                onMinimize={() => setOperatorAlertStackMinimized(true)}
              />
            ))}
          </div>
        </div>
      )}

      {localError && !localErrorIsMinimized && operatorAlertCount === 0 && (
        <div className="pointer-events-none absolute inset-x-3 top-full z-50 pt-2">
          <KernelErrorPanel
            message={localError.message}
            errorCode={localError.errorCode}
            errorDetails={localError.errorDetails}
            incidentSnapshot={localIncidentSnapshot}
            presentationKey={localErrorPresentationKey}
            onAcknowledge={() => {
              setAcknowledgedLocalErrorKey(localErrorPresentationKey);
              setMinimizedLocalErrorKey(localErrorPresentationKey);
            }}
            onMinimize={() => setMinimizedLocalErrorKey(localErrorPresentationKey)}
          />
        </div>
      )}

      <ExecutionHistoryPanel
        open={showExecutionHistory}
        onClose={() => setShowExecutionHistory(false)}
      />

      {browserSession && (
        <FileBrowserDialog
          open={true}
          title={
            browserSession.intent === 'load'
              ? browserSession.step === 'program'
                ? 'Load Program'
                : 'Load Deployment (Optional)'
              : browserSession.step === 'program'
                ? 'Save Program As'
                : 'Save Deployment As (Optional)'
          }
          kind={browserSession.step}
          mode={browserSession.intent === 'load' ? 'open' : 'save'}
          initialPath={
            browserSession.step === 'program'
              ? browserSession.programPath
              : browserSession.deploymentPath
          }
          allowSkip={browserSession.step === 'deployment'}
          skipLabel={
            browserSession.intent === 'load'
              ? 'Keep Current Deployment'
              : 'Skip Deployment Save'
          }
          onConfirm={advanceBrowserSession}
          onCancel={closeBrowserSession}
          onSkip={skipBrowserDeployment}
        />
      )}
    </div>
  );
}
