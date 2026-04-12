import { useCallback, useEffect, useMemo, useState } from 'react';
import { api } from '../api/client';
import { useCycleStore } from '../store/cycleStore';
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
  const { state, error } = useCycleStore();
  const { edgeStyle, setEdgeStyle } = useComposerStore();
  const {
    nodes,
    edges,
    flowToTree,
    appliedTree,
    savedTree,
    layoutDirection,
    setLayoutDirection,
    autoLayout,
    discardToApplied: discardProgramToApplied,
    setAppliedToDraft,
    saveProgram,
    loadProgram,
    fetchProgramInfo,
    fetchTree,
    fetchRuntimeOverrideStatus,
    programInfo,
    runtimeOverrideStatus,
    activateRuntimeOverrides,
    clearRuntimeOverrides,
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

  const [actionError, setActionError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [browserSession, setBrowserSession] = useState<BrowserSession | null>(null);

  const draftTree = useMemo(() => flowToTree(), [nodes, edges, flowToTree]);
  const programApplied = signature(draftTree) === signature(appliedTree);
  const programSaved = signature(draftTree) === signature(savedTree);
  const deploymentApplied = signature(deployment) === signature(appliedDeployment);
  const deploymentSaved = signature(deployment) === signature(savedDeployment);
  const hasUnappliedChanges = !programApplied || !deploymentApplied;
  const hasUnsavedChanges = !programSaved || !deploymentSaved;
  const activeError = actionError || error || programError || deploymentError;

  const modifiedBadge = hasUnappliedChanges
    ? { label: 'MODIFIED', color: 'bg-orange-600' }
    : { label: 'APPLIED', color: 'bg-emerald-700' };
  const savedBadge = hasUnsavedChanges
    ? { label: 'UNSAVED', color: 'bg-amber-700' }
    : { label: 'SAVED', color: 'bg-slate-700' };
  const runtimeOverrideLocked =
    state === 'running' ||
    state === 'direct_control' ||
    busy ||
    hasUnappliedChanges ||
    hasUnsavedChanges;
  const hasRuntimeOverrides =
    !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);
  const edgeStyleOptions: { value: EdgeStyle; label: string }[] = [
    { value: 'smoothstep', label: 'Orthogonal' },
    { value: 'simplebezier', label: 'Smooth' },
    { value: 'default', label: 'Bezier' },
    { value: 'straight', label: 'Straight' },
    { value: 'step', label: 'Step' },
  ];

  const applyDrafts = useCallback(async () => {
    const payload: { tree?: TreeNode; deployment?: FullDeployment } = {};
    if (!programApplied && draftTree) {
      payload.tree = draftTree;
    }
    if (!deploymentApplied && deployment) {
      payload.deployment = deployment;
    }
    if (!payload.tree && !payload.deployment) {
      return;
    }

    await api.session.apply(payload);
    if (payload.tree) {
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
    setAppliedDeploymentToDraft,
    setAppliedToDraft,
  ]);

  const saveDrafts = useCallback(async () => {
    const operations: Promise<unknown>[] = [];
    if (!programSaved && programInfo?.sourcePath) {
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
    programInfo?.sourcePath,
    programSaved,
    saveDeployment,
    saveProgram,
  ]);

  const handleApply = useCallback(async () => {
    try {
      setBusy(true);
      setActionError(null);
      await applyDrafts();
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [applyDrafts]);

  const handleSaveAll = useCallback(async () => {
    try {
      setBusy(true);
      setActionError(null);
      await saveDrafts();
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [saveDrafts]);

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
  }, [deployment, deploymentSourcePath, programInfo]);

  const handleLoad = useCallback(async () => {
    const hasDraftChanges = hasUnappliedChanges || hasUnsavedChanges;
    if (
      hasDraftChanges &&
      !window.confirm('Discard current draft changes and load another program?')
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
        setActionError((e as Error).message);
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
      saveDeployment,
      saveProgram,
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
  }, [discardDeploymentToApplied, discardProgramToApplied]);

  const handleDiscard = useCallback(() => {
    discardDrafts();
  }, [discardDrafts]);

  const handleActivateOverrides = useCallback(async () => {
    try {
      setBusy(true);
      setActionError(null);
      await activateRuntimeOverrides();
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [activateRuntimeOverrides]);

  const handleClearOverrides = useCallback(async () => {
    try {
      setBusy(true);
      setActionError(null);
      await clearRuntimeOverrides();
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [clearRuntimeOverrides]);

  useEffect(() => {
    if (!programInfo) {
      return undefined;
    }
    void fetchRuntimeOverrideStatus();
    const handle = window.setInterval(() => {
      void fetchRuntimeOverrideStatus();
    }, 2000);
    return () => {
      window.clearInterval(handle);
    };
  }, [fetchRuntimeOverrideStatus, programInfo?.sourcePath]);

  const editLocked = state === 'running' || state === 'direct_control' || busy;
  const canSaveAs = (!editLocked && !!programInfo) || (!editLocked && !!deployment);

  return (
      <div className="flex h-16 items-center gap-4 border-t border-gray-700 bg-gray-800 px-4">
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
        <button
          onClick={handleActivateOverrides}
          disabled={
            !runtimeOverrideStatus?.hasStaged ||
            runtimeOverrideLocked ||
            !runtimeOverrideStatus.canActivateNow
          }
          title={
            runtimeOverrideStatus?.requiresResetToActivate
              ? 'Reset the cycle in Viser before activating staged runtime overrides.'
              : 'Bind staged runtime overrides into the effective runtime tree.'
          }
          className="px-4 py-1.5 bg-cyan-700 hover:bg-cyan-600 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Use Staged
        </button>
        <button
          onClick={handleClearOverrides}
          disabled={!hasRuntimeOverrides || runtimeOverrideLocked}
          title="Drop all active and staged runtime overrides."
          className="px-4 py-1.5 bg-rose-800 hover:bg-rose-700 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
        >
          Clear Overrides
        </button>
        <span className={`px-3 py-1 rounded text-xs font-mono ${modifiedBadge.color}`}>
          {modifiedBadge.label}
        </span>
        <span className={`px-3 py-1 rounded text-xs font-mono ${savedBadge.color}`}>
          {savedBadge.label}
        </span>
        {runtimeOverrideStatus && !runtimeOverrideStatus.hasActive && !runtimeOverrideStatus.hasStaged && (
          <span className="px-3 py-1 rounded text-xs font-mono bg-slate-700">
            RUNTIME BASELINE
          </span>
        )}
        {runtimeOverrideStatus?.hasActive && (
          <span className="px-3 py-1 rounded text-xs font-mono bg-emerald-700">
            {runtimeOverrideStatus.activeCount} ACTIVE OVERRIDE{runtimeOverrideStatus.activeCount === 1 ? '' : 'S'}
          </span>
        )}
        {runtimeOverrideStatus?.hasStaged && (
          <span className="px-3 py-1 rounded text-xs font-mono bg-amber-700 text-black">
            {runtimeOverrideStatus.stagedCount} STAGED OVERRIDE{runtimeOverrideStatus.stagedCount === 1 ? '' : 'S'}
          </span>
        )}
        {runtimeOverrideStatus?.requiresResetToActivate && (
          <span className="px-3 py-1 rounded text-xs font-mono bg-red-700">
            RESET TO ACTIVATE
          </span>
        )}

        {programLoading && (
          <span className="text-gray-400 text-sm ml-auto">Loading program...</span>
        )}

        {activeError && (
          <span className="text-red-400 text-sm ml-auto">{activeError}</span>
        )}

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
