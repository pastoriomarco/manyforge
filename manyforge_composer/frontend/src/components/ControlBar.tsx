import { useCallback, useEffect, useRef, useState } from 'react';
import { api, ApiError } from '../api/client';
import { useCycleStore, useEditLocked } from '../store/cycleStore';
import { KernelErrorPanel } from './KernelErrorPanel';
import { useRobotCommandAuthority } from '../store/commandAuthority';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useComposerStore } from '../store/composerStore';
import { useProgramStore } from '../store/programStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { FileBrowserDialog } from './FileBrowserDialog';
import type { FullDeployment, TreeNode } from '../types/api';
import type { EdgeStyle } from '../store/composerStore';

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

export function ControlBar() {
  const {
    error,
    errorCode: cycleErrorCode,
    errorDetails: cycleErrorDetails,
  } = useCycleStore();
  const { edgeStyle, setEdgeStyle } = useComposerStore();
  const {
    nodes,
    flowToTree,
    appliedTree,
    savedTree,
    programParameters,
    appliedProgramParameters,
    savedProgramParameters,
    sceneResources,
    appliedSceneResources,
    savedSceneResources,
    blackboardNamespaces,
    appliedBlackboardNamespaces,
    savedBlackboardNamespaces,
    blackboardKeys,
    appliedBlackboardKeys,
    savedBlackboardKeys,
    layoutDirection,
    setLayoutDirection,
    autoLayout,
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
    && signature(appliedBlackboardNamespaces) === signature(savedBlackboardNamespaces)
    && signature(appliedBlackboardKeys) === signature(savedBlackboardKeys);
  const deploymentApplied = signature(deployment) === signature(appliedDeployment);
  const deploymentSaved = signature(deployment) === signature(savedDeployment);
  const deploymentAppliedMatchesSaved = signature(appliedDeployment) === signature(savedDeployment);
  const hasUnappliedChanges =
    !programApplied
    || !sceneApplied
    || !programParametersApplied
    || !blackboardNamespacesApplied
    || !blackboardKeysApplied
    || !deploymentApplied;
  const hasUnsavedChanges =
    !programSaved
    || !sceneSaved
    || !programParametersSaved
    || !blackboardNamespacesSaved
    || !blackboardKeysSaved
    || !deploymentSaved;
  const hasAppliedUnsavedChanges = !programAppliedMatchesSaved || !deploymentAppliedMatchesSaved;
  // Resolve the active error to a uniform { message, errorCode, errorDetails }
  // shape. Precedence:
  //   1. actionError — a fresh cycle-command 409 (highest priority because
  //      it represents the operator's most recent action).
  //   2. cycle store error — pushed via WebSocket from the cycle manager,
  //      carries the runtime-fault structured cause (collision_detected /
  //      interrupt_latched harvest, motion-soft-fail diagnostics, etc.).
  //   3. programError / deploymentError — frontend validation/transport
  //      errors that don't carry kernel diagnostics. Rendered with the
  //      panel's generic-banner fallback (no errorCode mapping).
  let activeError: ActionErrorState | null = null;
  if (actionError) {
    activeError = actionError;
  } else if (error) {
    activeError = {
      message: error,
      errorCode: cycleErrorCode ?? null,
      errorDetails: cycleErrorDetails ?? null,
    };
  } else if (programError || deploymentError) {
    activeError = {
      message: programError || deploymentError!,
      errorCode: null,
      errorDetails: null,
    };
  }
  const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);

  const summarizeValidationErrors = (errors: Array<{ path: string; message: string }>): string => {
    const summary = errors
      .slice(0, 3)
      .map((issue) => `${issue.path}: ${issue.message}`)
      .join(' | ');
    if (errors.length <= 3) {
      return summary;
    }
    return `${summary} | …and ${errors.length - 3} more`;
  };

  const modifiedBadge = hasUnappliedChanges
    ? { label: 'MODIFIED', color: 'bg-orange-600' }
    : { label: 'APPLIED', color: 'bg-emerald-700' };
  const savedBadge = hasUnsavedChanges
    ? { label: 'UNSAVED', color: 'bg-amber-700' }
    : { label: 'SAVED', color: 'bg-slate-700' };
  const edgeStyleOptions: { value: EdgeStyle; label: string }[] = [
    { value: 'smoothstep', label: 'Orthogonal' },
    { value: 'simplebezier', label: 'Smooth' },
    { value: 'default', label: 'Bezier' },
    { value: 'straight', label: 'Straight' },
    { value: 'step', label: 'Step' },
  ];

  const applyDrafts = useCallback(async (forceDiscardOverrides: boolean) => {
    const payload: {
      tree?: TreeNode;
      scene?: typeof sceneResources;
      deployment?: FullDeployment;
      programParameters?: typeof programParameters;
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
    if (!blackboardNamespacesApplied || !blackboardKeysApplied) {
      payload.blackboard = {
        namespaces: blackboardNamespaces,
        keys: blackboardKeys,
      };
    }
    if (!payload.tree && !payload.scene && !payload.deployment && !payload.programParameters && !payload.blackboard) {
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
    if (payload.tree || payload.scene || payload.programParameters || payload.blackboard) {
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
    blackboardNamespaces,
    blackboardNamespacesApplied,
    blackboardKeys,
    blackboardKeysApplied,
    validateDraftGraph,
    setAppliedDeploymentToDraft,
    setAppliedToDraft,
    summarizeValidationErrors,
  ]);

  const saveDrafts = useCallback(async () => {
    const operations: Promise<unknown>[] = [];
    const programArtifactSaved =
      programSaved
      && sceneSaved
      && programParametersSaved
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
    blackboardNamespacesSaved,
    blackboardKeysSaved,
    programInfo?.sourcePath,
    programSaved,
    saveDeployment,
    saveProgram,
  ]);

  const handleApply = useCallback(async () => {
    if (
      hasRuntimeOverrides
      && !window.confirm(
        'Applying draft changes will discard active runtime-only patches created by recovery/LLM workflows. Continue?',
      )
    ) {
      return;
    }
    try {
      setBusy(true);
      setActionError(null);
      await applyDrafts(hasRuntimeOverrides);
    } catch (e) {
      setActionError(e);
    } finally {
      setBusy(false);
    }
  }, [applyDrafts, hasRuntimeOverrides, setActionError]);

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
      !window.confirm(
        'Revert the applied session back to the last saved program/deployment and discard current draft changes?',
      )
    ) {
      return;
    }
    if (
      hasRuntimeOverrides &&
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
      blackboard?: {
        namespaces: typeof savedBlackboardNamespaces;
        keys: typeof savedBlackboardKeys;
      };
      forceDiscardOverrides?: boolean;
    } = {
      forceDiscardOverrides: hasRuntimeOverrides,
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
    if (!payload.tree && !payload.scene && !payload.deployment && !payload.programParameters && !payload.blackboard) {
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
    hasRuntimeOverrides,
    appliedProgramParameters,
    appliedSceneResources,
    appliedBlackboardKeys,
    appliedBlackboardNamespaces,
    programAppliedMatchesSaved,
    savedDeployment,
    savedSceneResources,
    savedBlackboardKeys,
    savedBlackboardNamespaces,
    savedProgramParameters,
    savedTree,
    setActionError,
  ]);

  const cycleEditLocked = useEditLocked();
  const editLocked = cycleEditLocked || busy;
  const canSaveAs = (!editLocked && !!programInfo) || (!editLocked && !!deployment);

  return (
      <div className="border-t border-gray-700 bg-gray-800 px-4 py-2">
        {activeError && (
          <KernelErrorPanel
            message={activeError.message}
            errorCode={activeError.errorCode}
            errorDetails={activeError.errorDetails}
          />
        )}
        <div className="flex min-h-12 items-center gap-4">
        <button
          onClick={() => setLayoutDirection(layoutDirection === 'vertical' ? 'horizontal' : 'vertical')}
          disabled={editLocked || nodes.length === 0}
          className="px-4 py-1.5 bg-violet-600 hover:bg-violet-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          {layoutDirection === 'vertical' ? 'Horizontal View' : 'Vertical View'}
        </button>
        <button
          onClick={autoLayout}
          disabled={editLocked || nodes.length === 0}
          className="px-4 py-1.5 bg-sky-600 hover:bg-sky-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Auto Align
        </button>
        <select
          value={edgeStyle}
          onChange={(e) => setEdgeStyle(e.target.value as EdgeStyle)}
          disabled={editLocked}
          className="px-3 py-1.5 rounded bg-gray-700 border border-gray-600 text-sm text-white disabled:bg-gray-600 disabled:cursor-not-allowed"
          title="Connection line style"
        >
          {edgeStyleOptions.map((option) => (
            <option key={option.value} value={option.value}>
              {option.label}
            </option>
          ))}
        </select>
        <button
          onClick={handleApply}
          disabled={!hasUnappliedChanges || editLocked}
          className="px-4 py-1.5 bg-indigo-600 hover:bg-indigo-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Apply
        </button>
        <button
          onClick={handleRevert}
          disabled={!hasAppliedUnsavedChanges || editLocked}
          className="px-4 py-1.5 bg-fuchsia-700 hover:bg-fuchsia-600 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
          title="Restore the applied runtime session to the last saved program/deployment."
        >
          Revert
        </button>
        <button
          onClick={handleDiscard}
          disabled={!hasUnappliedChanges || editLocked}
          className="px-4 py-1.5 bg-red-700 hover:bg-red-600 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Discard
        </button>
        <button
          onClick={handleSaveAll}
          disabled={!hasUnsavedChanges || editLocked}
          className="px-4 py-1.5 bg-amber-600 hover:bg-amber-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Save
        </button>
        <button
          onClick={handleSaveAs}
          disabled={!canSaveAs}
          className="px-4 py-1.5 bg-teal-600 hover:bg-teal-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Save As
        </button>
        <button
          onClick={handleLoad}
          disabled={editLocked}
          className="px-4 py-1.5 bg-yellow-600 hover:bg-yellow-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Load
        </button>
        <span className={`px-3 py-1 rounded text-xs font-mono ${modifiedBadge.color}`}>
          {modifiedBadge.label}
        </span>
        <span className={`px-3 py-1 rounded text-xs font-mono ${savedBadge.color}`}>
          {savedBadge.label}
        </span>

        {programLoading && (
          <span className="text-gray-400 text-sm">Loading program...</span>
        )}

        {/* Cycle controls — pushed right so they sit next to the
            floating "Ready" overlay. Run/Stop/Reset + velocity-scale
            slider were missing from the Composer UI (only existed as
            backend routes + cycleStore actions); landing them here
            gives the operator parity with the legacy Viser GUI's
            control panel without a separate overlay. */}
        <CycleControls />

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
      </div>
  );
}

/**
 * Cycle-control surface: Run / Stop / Reset + velocity-scale slider.
 *
 * Sits in the bottom bar, pushed right with `ml-auto` so it lands
 * next to the floating <RuntimeStatusOverlay /> "Ready" indicator
 * at bottom-right of the screen. Button enabled-state mirrors the
 * cycle-state machine: Run only fires from idle/stopped, Stop from
 * running / direct_control, Reset from any
 * state except where the kernel forbids it (the backend returns
 * 409 in that case; we let it surface via the cycle store's error).
 */
function CycleControls() {
  const { state, velocityScale, run, stop, reset, setVelocityScale } = useCycleStore();
  const runAuthority = useRobotCommandAuthority({
    requireFreshScene: true,
  });
  const [cycleBusy, setCycleBusy] = useState(false);
  const [velocityDraft, setVelocityDraft] = useState(velocityScale);
  const pendingVelocityScaleRef = useRef<number | null>(null);
  const velocityScaleInFlightRef = useRef(false);
  const velocityScaleTimerRef = useRef<number | null>(null);
  const lastVelocityScaleSentAtRef = useRef(0);
  const canRun =
    (state === 'idle' || state === 'stopped') &&
    runAuthority.canCommand;
  // Stop applies to in-flight motion the operator can see — BT
  // execution (state==='running') or an intervention / direct-jog
  // session (direct_control). Default Robot Control authoring no longer
  // streams, so there is no separate streaming-session stop path.
  const cycleStoppable =
    state === 'running' || state === 'direct_control';
  const canStop = cycleStoppable;

  const runCycleCommand = useCallback(
    (command: () => Promise<void>) => {
      void (async () => {
        setCycleBusy(true);
        try {
          await command();
        } finally {
          setCycleBusy(false);
        }
      })();
    },
    [],
  );

  const flushVelocityScale = useCallback(() => {
    if (velocityScaleTimerRef.current !== null) {
      window.clearTimeout(velocityScaleTimerRef.current);
      velocityScaleTimerRef.current = null;
    }
    if (velocityScaleInFlightRef.current || pendingVelocityScaleRef.current === null) {
      return;
    }
    const nextScale = pendingVelocityScaleRef.current;
    pendingVelocityScaleRef.current = null;
    velocityScaleInFlightRef.current = true;
    lastVelocityScaleSentAtRef.current = performance.now();
    void setVelocityScale(nextScale).finally(() => {
      velocityScaleInFlightRef.current = false;
      if (pendingVelocityScaleRef.current !== null) {
        const elapsed = performance.now() - lastVelocityScaleSentAtRef.current;
        const delay = Math.max(0, 250 - elapsed);
        velocityScaleTimerRef.current = window.setTimeout(flushVelocityScale, delay);
      }
    });
  }, [setVelocityScale]);

  const queueVelocityScale = useCallback((value: number) => {
    const clamped = Math.max(0, Math.min(1, value));
    setVelocityDraft(clamped);
    pendingVelocityScaleRef.current = clamped;
    if (velocityScaleInFlightRef.current) {
      return;
    }
    const elapsed = performance.now() - lastVelocityScaleSentAtRef.current;
    const delay = Math.max(0, 250 - elapsed);
    if (delay === 0) {
      flushVelocityScale();
    } else if (velocityScaleTimerRef.current === null) {
      velocityScaleTimerRef.current = window.setTimeout(flushVelocityScale, delay);
    }
  }, [flushVelocityScale]);

  useEffect(() => {
    if (pendingVelocityScaleRef.current === null && !velocityScaleInFlightRef.current) {
      setVelocityDraft(velocityScale);
    }
  }, [velocityScale]);

  useEffect(() => () => {
    if (velocityScaleTimerRef.current !== null) {
      window.clearTimeout(velocityScaleTimerRef.current);
      velocityScaleTimerRef.current = null;
    }
  }, []);

  return (
    <div className="ml-auto flex items-center gap-2">
      <button
        type="button"
        onClick={() => void run()}
        disabled={cycleBusy || !canRun}
        title={
          canRun
            ? 'Start the loaded program'
            : runAuthority.reason !== null
              ? runAuthority.reason
              : `Cycle is ${state}`
        }
        className="rounded bg-emerald-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-emerald-500 disabled:cursor-not-allowed disabled:bg-gray-600"
      >
        Run
      </button>
      <button
        type="button"
        onClick={() => {
          // Stop the active cycle, then force a scene refresh so the
          // viewer reflects the kStopped pose without waiting a full
          // poll interval.
          runCycleCommand(async () => {
            await stop();
            try {
              await useSceneStateStore.getState().refresh();
            } catch (err) {
              console.warn('post-stop scene refresh failed', err);
            }
          });
        }}
        disabled={cycleBusy || !canStop}
        title={canStop ? 'Stop the active cycle' : `Cycle is ${state}`}
        className="rounded bg-orange-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-orange-500 disabled:cursor-not-allowed disabled:bg-gray-600"
      >
        Stop
      </button>
      <button
        type="button"
        onClick={() => {
          runCycleCommand(reset);
        }}
        disabled={cycleBusy}
        title="Reset the cycle to its initial state"
        className="rounded bg-rose-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-rose-500 disabled:cursor-not-allowed disabled:bg-gray-600"
      >
        Reset
      </button>
      <label className="ml-2 flex items-center gap-1.5 text-xs text-gray-300" title="Runtime velocity scale (0.0 - 1.0; 0 holds motion)">
        <span className="font-mono text-[11px] text-gray-400">vel</span>
        <input
          type="range"
          min={0.0}
          max={1.0}
          step={0.01}
          value={velocityDraft}
          onChange={(e) => queueVelocityScale(Number(e.target.value))}
          className="w-24 accent-emerald-500"
        />
        <span className="w-8 text-right font-mono text-[11px]">
          {velocityDraft.toFixed(2)}
        </span>
      </label>
    </div>
  );
}
