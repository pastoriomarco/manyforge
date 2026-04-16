import { useCallback, useState } from 'react';
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
    flowToTree,
    appliedTree,
    savedTree,
    programParameters,
    appliedProgramParameters,
    savedProgramParameters,
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

  const [actionError, setActionError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [browserSession, setBrowserSession] = useState<BrowserSession | null>(null);

  const draftTree = flowToTree();
  const programApplied = signature(draftTree) === signature(appliedTree);
  const programSaved = signature(draftTree) === signature(savedTree);
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
    && signature(appliedProgramParameters) === signature(savedProgramParameters)
    && signature(appliedBlackboardNamespaces) === signature(savedBlackboardNamespaces)
    && signature(appliedBlackboardKeys) === signature(savedBlackboardKeys);
  const deploymentApplied = signature(deployment) === signature(appliedDeployment);
  const deploymentSaved = signature(deployment) === signature(savedDeployment);
  const deploymentAppliedMatchesSaved = signature(appliedDeployment) === signature(savedDeployment);
  const hasUnappliedChanges =
    !programApplied
    || !programParametersApplied
    || !blackboardNamespacesApplied
    || !blackboardKeysApplied
    || !deploymentApplied;
  const hasUnsavedChanges =
    !programSaved
    || !programParametersSaved
    || !blackboardNamespacesSaved
    || !blackboardKeysSaved
    || !deploymentSaved;
  const hasAppliedUnsavedChanges = !programAppliedMatchesSaved || !deploymentAppliedMatchesSaved;
  const activeError = actionError || error || programError || deploymentError;

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

  const applyDrafts = useCallback(async () => {
    const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);
    const payload: {
      tree?: TreeNode;
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
    if (!payload.tree && !payload.deployment && !payload.programParameters && !payload.blackboard) {
      return;
    }

    await api.session.apply({
      ...payload,
      forceDiscardOverrides: hasRuntimeOverrides,
    });
    if (payload.tree) {
      setAppliedToDraft();
    } else if (payload.programParameters) {
      setAppliedToDraft();
    } else if (payload.blackboard) {
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
    programParameters,
    programParametersApplied,
    blackboardNamespaces,
    blackboardNamespacesApplied,
    blackboardKeys,
    blackboardKeysApplied,
    validateDraftGraph,
    setAppliedDeploymentToDraft,
    setAppliedToDraft,
    runtimeOverrideStatus,
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
    const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);
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
    runtimeOverrideStatus,
  ]);

  const finalizeBrowserSession = useCallback(
    async (session: BrowserSession) => {
      try {
        setBusy(true);
        setActionError(null);

        if (session.intent === 'load') {
          const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);
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
      runtimeOverrideStatus,
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

  const handleRevert = useCallback(async () => {
    const hasRuntimeOverrides = !!runtimeOverrideStatus && (runtimeOverrideStatus.hasActive || runtimeOverrideStatus.hasStaged);
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
    if (!payload.tree && !payload.deployment && !payload.programParameters && !payload.blackboard) {
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
          ? fetchProgramInfo()
          : Promise.resolve(true),
      ]);
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }, [
    deploymentAppliedMatchesSaved,
    fetchDeployment,
    fetchProgramInfo,
    fetchTree,
    hasAppliedUnsavedChanges,
    appliedProgramParameters,
    appliedBlackboardKeys,
    appliedBlackboardNamespaces,
    programAppliedMatchesSaved,
    runtimeOverrideStatus,
    savedDeployment,
    savedBlackboardKeys,
    savedBlackboardNamespaces,
    savedProgramParameters,
    savedTree,
  ]);

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
