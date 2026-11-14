import { act, createElement, memo, useEffect, useMemo, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  ApplyCommandRow,
  DraftMutationResponse,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({
  programWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
    apply: vi.fn(),
    discard: vi.fn(),
    revert: vi.fn(),
    openDraft: vi.fn(),
  },
  scenarioDocuments: {
    save: vi.fn(),
    discard: vi.fn(),
  },
  cellApply: {
    load: vi.fn(),
    newScenario: vi.fn(),
  },
}));

vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(),
  api: apiMock,
  // The hook reads the shared publication through the controller, not through an
  // independent identity read, so the only client surface it needs is the
  // Program workspace projection that still owns Applied truth.
}));

import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  draftEditUnavailableReason,
  scenarioReplacementUnavailableReason,
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { ProgramDraftChangedError, useProgramWorkspace } from './useProgramWorkspace';
import { ApiError } from '../api/client';
import { endedApplyRow, endedProgramApplyRow } from '../test/applyCommandRow';
import { codedErrorText } from '../state/componentPresentation';

/**
 * A real shared-publication controller for this hook's tests.
 *
 * It uses the same pure gate and fence helpers the production controller uses, so
 * the hook is exercised against the real contract rather than a behavioural
 * stub; only the read loop itself belongs to `useAuthoringWorkspace`'s own test.
 */
const testRefresh = async () => undefined;
const testBeginMutation = () => true;
const testFinishMutation = () => undefined;

function testAuthoringController(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): AuthoringWorkspaceController {
  return {
    published,
    current: publicationIsCurrent(published, snapshot),
    unavailableReason: authoringUnavailableReason(published, snapshot),
    editUnavailableReason: draftEditUnavailableReason(published, snapshot),
    replaceUnavailableReason: scenarioReplacementUnavailableReason(published, snapshot),
    failedApplyHoldsCell: false,
    project: projectFacts(published),
    reading: false,
    error: null,
    refresh: testRefresh,
    operation: null,
    beginMutation: testBeginMutation,
    finishMutation: testFinishMutation,
    observeApply: () => undefined,
    submissionUnconfirmed: false,
    correlatedApply: null,
    rechecking: false,
    recheckMessage: null,
    recheckSubmission: async () => undefined,
  };
}

function testPublication(
  workspace: ProgramWorkspaceResponse,
): PublishedAuthoringWorkspace {
  const snapshot = {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v3' as const,
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: 1,
    program: { draft: workspace.draft, saved: workspace.saved },
    deployment: {
      draftId: 'deployment-draft-1',
      draftRevision: 4,
      document: null,
      savedPath: null,
      savedDocument: null,
      differsFromSaved: false,
    },
    project: {
      documentPath: '/workspace/cell',
      readOnly: false,
      documentId: null,
      documentName: null,
      documentDescription: null,
      sourceDiagnostics: [],
      sourceDocumentDigest: null,
      sourceProgramDigest: null,
      sourceDeploymentDigest: null,
    },
  };
  return { identity: authoringWorkspaceIdentity(snapshot), snapshot };
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}
type Deferred<T> = ReturnType<typeof deferred<T>>;

function workspace(
  draftRevision: number,
  programRevision = `sha256:${String(draftRevision).padStart(64, '0')}`,
): ProgramWorkspaceResponse {
  const program = {
    api_version: 'manyforge.program.v3' as const,
    program_id: 'program-1',
    program_revision: programRevision,
    program_profile: 'composable-cell-v1' as const,
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v3' as const,
      content_version: 'fixture',
    },
    tree: {
      node_id: 'timer',
      instance_id: 'root',
      parameters: {},
      children: [],
    },
  };
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
      programDraftId: 'draft-1', programDraftRevision: draftRevision,
      deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
    },
    draft: {
      draftId: 'draft-1',
      draftRevision,
      programLoaded: true,
      authoring: {
        schema_version: 'manyforge.composer_authoring_forest.v2',
        api_version: program.api_version,
        program_id: program.program_id,
        program_profile: program.program_profile,
        name: program.name,
        catalog: program.catalog,
        forest: [program.tree],
        positions: {},
      },
      candidateAvailable: true,
      programRevision,
      program,
      diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} },
    },
    applicability: {
      applyReady: true,
      unavailableReason: null,
    },
    saved: {
      available: true,
      matchesDraft: draftRevision === 1,
      path: '/workspace/program.program.yaml',
      programRevision: draftRevision === 1
        ? programRevision
        : `sha256:${'1'.repeat(64)}`,
    },
    applied: {
      available: true,
      matchesDraft: draftRevision === 1,
      programRevision: draftRevision === 1
        ? programRevision
        : `sha256:${'1'.repeat(64)}`,
      unavailableReason: null,
    },
    hasUnsavedChanges: draftRevision !== 1,
    hasUnappliedChanges: draftRevision !== 1,
  };
}

const recoveryPolicy: ProgramNodeRecoveryPolicy = {
  allow_intervention: true,
  max_retries: 2,
};
const operatorStopPolicy: ProgramOperatorStopPolicy = 'hold';

type FakeNode = {
  nodeType: number;
  nodeName: string;
  tagName: string;
  namespaceURI: string;
  ownerDocument: FakeDocument;
  style: Record<string, string>;
  addEventListener: () => void;
  removeEventListener: () => void;
  appendChild: () => void;
  insertBefore: () => void;
  removeChild: () => void;
  setAttribute: () => void;
  removeAttribute: () => void;
};

type FakeDocument = {
  nodeType: number;
  documentElement: FakeNode | null;
  activeElement: null;
  defaultView: Record<string, unknown>;
  addEventListener: () => void;
  removeEventListener: () => void;
  createElement: (tagName: string) => FakeNode;
};

let root: { render: (value: ReactNode) => void; unmount: () => void } | null = null;
let latest: ReturnType<typeof useProgramWorkspace> | null = null;
const scenarioOpened = vi.fn();

async function mountHarness(
  initial: Deferred<ProgramWorkspaceResponse>,
  snapshot: ComposerHeadlessState | null = null,
  published: PublishedAuthoringWorkspace | null = null,
  onConsumerRender?: () => void,
) {
  const document = {
    nodeType: 9,
    documentElement: null,
    activeElement: null,
    defaultView: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  } as FakeDocument;
  const makeNode = (tagName: string): FakeNode => ({
    nodeType: 1,
    nodeName: tagName.toUpperCase(),
    tagName: tagName.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document,
    style: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    appendChild: () => undefined,
    insertBefore: () => undefined,
    removeChild: () => undefined,
    setAttribute: () => undefined,
    removeAttribute: () => undefined,
  });
  document.createElement = makeNode;
  document.documentElement = makeNode('html');
  const window = {
    document,
    location: { href: 'http://composer.test/' },
    HTMLIFrameElement: class {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);

  const { createRoot } = await import('react-dom/client');
  root = createRoot(makeNode('div') as unknown as Element);
  const Consumer = memo((_props: { mutate: ReturnType<typeof useProgramWorkspace>['mutate'] }) => {
    void _props;
    onConsumerRender?.();
    return null;
  });
  function Harness({ currentSnapshot }: { currentSnapshot: ComposerHeadlessState | null }) {
    const observed = currentSnapshot?.authoringWorkspace;
    const workspaceEpoch = observed?.workspaceEpoch;
    const workspaceRevision = observed?.workspaceRevision;
    const programDraftRevision = observed?.programDraftRevision;
    const deploymentDraftId = observed?.deploymentDraftId;
    const deploymentDraftRevision = observed?.deploymentDraftRevision;
    const currentPublication = useMemo(() => {
      if (published === null || workspaceEpoch === undefined || workspaceRevision === undefined
        || programDraftRevision === undefined || deploymentDraftId === undefined
        || deploymentDraftRevision === undefined) return published;
      const current = workspace(programDraftRevision);
      const nextSnapshot = {
        ...published.snapshot,
        workspaceEpoch,
        workspaceRevision,
        program: { draft: current.draft, saved: current.saved },
        deployment: {
          ...published.snapshot.deployment,
          draftId: deploymentDraftId,
          draftRevision: deploymentDraftRevision,
        },
      };
      return {
        identity: authoringWorkspaceIdentity(nextSnapshot),
        snapshot: nextSnapshot,
      };
    }, [workspaceEpoch, workspaceRevision, programDraftRevision, deploymentDraftId, deploymentDraftRevision]);
    const authoring = useMemo(
      () => testAuthoringController(currentPublication, currentSnapshot),
      [currentPublication, currentSnapshot],
    );
    const controller = useProgramWorkspace(currentSnapshot, authoring, scenarioOpened);
    useEffect(() => {
      latest = controller;
      return () => {
        latest = null;
      };
    }, [controller]);
    return onConsumerRender ? createElement(Consumer, { mutate: controller.mutate }) : null;
  }
  apiMock.programWorkspace.get.mockReturnValueOnce(initial.promise);
  await act(async () => {
    root?.render(createElement(Harness, { currentSnapshot: snapshot }));
  });
  return async (nextSnapshot: ComposerHeadlessState | null) => {
    await act(async () => {
      root?.render(createElement(Harness, { currentSnapshot: nextSnapshot }));
    });
  };
}

function authoringSnapshot(
  current: ProgramWorkspaceResponse,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v4',
    applyCommands: { inProgress: null, ended: [] },
    complete: true,
    executionControlStatus: null,
    componentOverview: {
      schemaVersion: 'manyforge.component_overview.v3',
      overviewRevision: `sha256:${'a'.repeat(64)}`,
      readiness: 'ready',
      summaryLabel: 'Ready',
      profileLabel: 'standard',
      authorityLabel: 'owner-issued scoped facts',
      userMessage: 'Current.',
      capabilities: [],
      components: [],
      maintenanceOperation: null,
      basis: {
        runtimeProjectionGeneration: null,
        logicalSceneSourceGeneration: null,
        sceneRuntimeSourceGeneration: null,
        alarmGeneration: null,
        alarmStateRevision: null,
        admissionGeneration: null,
        programCycleGeneration: null,
        appliedProgramGeneration: null,
        executionPublicationGeneration: null,
      },
    },
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
    heldRunDraftEditAllowed: false,
    programCycle: {
      cycleGeneration: 3,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      terminalOrSettlementReference: null,
      runRevision: null,
      nodeStatusRevision: null,
      runtimeControlRevision: null,
      executionControlRevision: 1,
      causalRequestKey: null,
      causalDisplay: null,
      currentCommandKind: null, runnerCommands: { inProgress: null, ended: [] },
      failure: null,
      resetPhase: null, resetOptions: null,
      resetRobotPosition: { available: false, reason: 'unsupported' },
      allowedCommands: [],
      run: null,
      runtimeControls: null,
    },
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: current.draft.draftRevision,
      programDraftId: current.draft.draftId,
      programDraftRevision: current.draft.draftRevision,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 4,
    },
    authoringWorkspaceContainment: null,
    applicationStartId: '3f9a0000000000a1',
    snapshotSequence: 11,
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v3',
      activeConfiguration: null,
      complete: true,
      projectionGeneration: 1,
      projectionOwnerId: 'runtime.owner',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1,
          sourceOwnerId: 'manager.owner', receiverAgeNs: 1,
        },
        logicalScene: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1,
          sourceOwnerId: 'scene.owner', receiverAgeNs: 1,
        },
        sceneRuntime: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1,
          sourceOwnerId: 'runtime.owner', receiverAgeNs: 1,
        },
      },
    },
    alarms: {
      schemaVersion: 'manyforge.operator_alarm_snapshot.v3',
      ownerId: 'alarm.owner',
      generation: 1,
      stateRevision: 1,
      reconciled: true,
      healthState: 'healthy',
      liveCount: 0,
      unacknowledgedCount: 0,
      activeCount: 0,
      activeAcknowledgedCount: 0,
      motionAttentionCount: 0,
      motionAttentionMembership: [],
      unacknowledgedMembership: [],
      alarms: [],
      truncated: false,
      kernelAttention: null,
    },
  };
}

afterEach(async () => {
  if (root !== null) {
    await act(async () => root?.unmount());
  }
  root = null;
  latest = null;
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('useProgramWorkspace Draft policy interaction', () => {
  beforeEach(() => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  });

  it('keeps an unrelated status update out of a memoized mutation consumer and retains captured admission', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const snapshot = authoringSnapshot(workspace(1));
    const onConsumerRender = vi.fn();
    const rerender = await mountHarness(initial, snapshot, testPublication(workspace(1)), onConsumerRender);
    await act(async () => initial.resolve(workspace(1)));
    const oldMutate = latest!.mutate;
    const count = onConsumerRender.mock.calls.length;
    await rerender({ ...snapshot, componentOverview: { ...snapshot.componentOverview, userMessage: 'Updated status.' } });
    expect(latest!.mutate).toBe(oldMutate);
    expect(onConsumerRender).toHaveBeenCalledTimes(count);

    const next = { ...snapshot, authoringAdmission: { ...snapshot.authoringAdmission, admissionGeneration: 8 } };
    await rerender(next);
    const currentMutate = latest!.mutate;
    expect(currentMutate).not.toBe(oldMutate);
    apiMock.programWorkspace.mutate.mockRejectedValueOnce(new ApiError('refused', 409, {
      detail: { code: 'stale_generation', effect: 'none' },
    }));
    await act(async () => { await oldMutate('test', {}); });
    expect(apiMock.programWorkspace.mutate).toHaveBeenLastCalledWith(
      expect.objectContaining({ expectedAdmissionGeneration: 7 }), 'test', {},
    );
    expect(latest!.state.error).toContain('Edit not applied');
    apiMock.programWorkspace.mutate.mockResolvedValueOnce({
      mutation: 'test', details: {}, workspace: workspace(1),
      authoringWorkspace: testPublication(workspace(1)).snapshot,
    });
    await act(async () => { await currentMutate('test', {}); });
    expect(apiMock.programWorkspace.mutate).toHaveBeenLastCalledWith(
      expect.objectContaining({ expectedAdmissionGeneration: 8 }), 'test', {},
    );
    await rerender(null);
    expect(latest!.mutationAvailable).toBe(false);
    await act(async () => { await latest!.mutate('test', {}); });
    expect(apiMock.programWorkspace.mutate).toHaveBeenCalledTimes(2);
  });

  it('coalesces one workspace read for a newer backend Draft identity', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const rerender = await mountHarness(
      initial,
      authoringSnapshot(workspace(1)),
      testPublication(workspace(1)),
    );
    await act(async () => initial.resolve(workspace(1)));
    const resynchronized = deferred<ProgramWorkspaceResponse>();
    apiMock.programWorkspace.get.mockReturnValueOnce(resynchronized.promise);

    await rerender(authoringSnapshot(workspace(2)));
    await rerender(authoringSnapshot(workspace(2)));
    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(2);

    await act(async () => resynchronized.resolve(workspace(2)));
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.pendingAction).toBeNull();
  });

  it('re-reads Applied once when an accepted activation settles without a new Draft', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const snapshot = authoringSnapshot(workspace(1));
    const rerender = await mountHarness(initial, snapshot, testPublication(workspace(1)));
    const loading = {
      ...workspace(1),
      applied: { ...workspace(1).applied, available: false, matchesDraft: null },
      hasUnappliedChanges: null,
    };
    await act(async () => initial.resolve(loading));
    expect(latest?.state.workspace?.applied.available).toBe(false);
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(1));

    // Same Draft publication; only the Manager's active bundle moved.
    const settled = {
      ...snapshot,
      runtime: {
        ...snapshot.runtime,
        sections: {
          ...snapshot.runtime.sections,
          manager: {
            ...snapshot.runtime.sections.manager,
            payload: { active: { retained_bundle_id: 'bundle-2', graph_revision: 'graph-2' } },
          },
        },
      },
    };
    await rerender(settled);
    await rerender(settled);

    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(2);
    expect(latest?.state.workspace?.applied.available).toBe(true);
    expect(latest?.state.workspace?.applied.matchesDraft).toBe(true);
  });

  it.each(['authoring_not_ready', 'stale_generation'])('does not retry and keeps a refused edit dismissible for %s', async (code) => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    apiMock.programWorkspace.mutate.mockRejectedValueOnce(new ApiError('refused', 409, {
      detail: { code, effect: 'none' },
    }));
    await act(async () => {
      await latest?.mutate('program_draft_update_node_policies', {});
    });
    expect(apiMock.programWorkspace.mutate).toHaveBeenCalledTimes(1);
    expect(latest?.state.error).toContain('Edit not applied');
    expect(latest?.state.message).toBeNull();
    expect(latest?.state.workspace?.draft.draftRevision).toBe(1);
  });

  it('keeps an Apply preparation refusal and its backend cause until dismissed', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(2)), testPublication(workspace(2)));
    await act(async () => initial.resolve(workspace(2)));
    const cause = 'Program is missing compatibility with target role arm';
    apiMock.programWorkspace.apply.mockRejectedValueOnce(new ApiError(
      'candidate could not be prepared',
      409,
      { detail: {
        code: 'activation_preparation_failed',
        message: 'candidate could not be prepared',
        effect: 'none',
        details: { preparer_detail: `manyforge_prepare_activation: ${cause}` },
      } },
    ));

    const retained = latest?.state.workspace;
    await act(async () => { await latest?.apply(); });

    expect(apiMock.programWorkspace.apply).toHaveBeenCalledTimes(1);
    expect(latest?.state.workspace).toBe(retained);
    expect(latest?.state.message).toBeNull();
    expect(latest?.state.error).toBe(
      `Not applied: the candidate could not be prepared (${cause}). Correct it and try again.`,
    );
    act(() => latest?.clearFeedback());
    expect(latest?.state.error).toBeNull();
  });

  it.each(['composer_draft_revision_stale', 'composer_workspace_stale'])(
    'refreshes the workspace after a refused-before-effect stale fence (%s)',
    async (code) => {
      const initial = deferred<ProgramWorkspaceResponse>();
      await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
      await act(async () => initial.resolve(workspace(1)));
      apiMock.programWorkspace.mutate.mockRejectedValueOnce(new ApiError('stale', 409, {
        detail: { code, effect: 'none' },
      }));
      apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(2));
      await act(async () => {
        await latest?.mutate('program_draft_update_node_policies', {});
      });
      // The stale fence is handled before the refusal message: the newer
      // workspace is read once and the edit is never repeated.
      expect(apiMock.programWorkspace.mutate).toHaveBeenCalledTimes(1);
      expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(2);
      expect(latest?.state.message ?? '').not.toContain('not applied');
      expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    },
  );

  it('uses the ordinary Draft mutation path for node policies', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(
      initial,
      authoringSnapshot(workspace(1)),
      testPublication(workspace(1)),
    );
    await act(async () => initial.resolve(workspace(1)));
    const mutation = deferred<DraftMutationResponse>();
    apiMock.programWorkspace.mutate.mockReturnValueOnce(mutation.promise);

    await act(async () => {
      latest?.updateNodePolicies(
        'root',
        recoveryPolicy,
        operatorStopPolicy,
        ['all_grippers', 'robot_a'],
      );
      expect(apiMock.programWorkspace.mutate).toHaveBeenCalledWith(
        expect.objectContaining({
          draftId: 'draft-1',
          expectedDraftRevision: 1,
          expectedAdmissionGeneration: 7,
          // The snapshot's `A` and a fresh 32-hex key; no browser-stored epoch.
          applicationStartId: '3f9a0000000000a1',
          requestKey: expect.stringMatching(/^[0-9a-f]{32}$/),
        }),
        'program_draft_update_node_policies',
        {
          instance_id: 'root',
          recovery_policy: recoveryPolicy,
          operator_stop_policy: operatorStopPolicy,
          execution_groups: ['all_grippers', 'robot_a'],
        },
      );
    });
    await act(async () => mutation.resolve({
      mutation: 'program_draft_update_node_policies',
      details: { updated_instance_id: 'root' },
      workspace: workspace(2),
      authoringWorkspace: testPublication(workspace(2)).snapshot,
    }));
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.pendingAction).toBeNull();
  });

  it('fences an Apply after Reset to its exact Draft content, refreshing only admission', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const rerender = await mountHarness(
      initial,
      authoringSnapshot(workspace(2)),
      testPublication(workspace(2)),
    );
    await act(async () => initial.resolve(workspace(2)));
    const expectedDraft = {
      draftId: 'draft-1', draftRevision: 2, programRevision: workspace(2).draft.programRevision!,
    };
    // Reset legitimately advanced the admission generation: the fence follows it.
    const afterReset = authoringSnapshot(workspace(2));
    afterReset.authoringAdmission = { ...afterReset.authoringAdmission, admissionGeneration: 12 };
    await rerender(afterReset);
    apiMock.programWorkspace.apply.mockResolvedValueOnce(
      endedProgramApplyRow('program_apply', { applyId: 1, applyGeneration: 2 }),
    );
    // The workspace is read once the Apply's row ended (OD-214).
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(2));
    await act(async () => {
      await latest?.apply({ expectedDraft });
    });
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ draftId: 'draft-1', expectedDraftRevision: 2, expectedAdmissionGeneration: 12 }),
    );
    // Another browser's edit is now installed: the confirmed content is gone,
    // so no Apply is sent, with a named refusal.
    apiMock.programWorkspace.get.mockResolvedValue(workspace(3));
    await rerender(authoringSnapshot(workspace(3)));
    await expect(latest!.apply({ expectedDraft }))
      .rejects.toBeInstanceOf(ProgramDraftChangedError);
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
  });

  it('New is the server Apply of the empty cell: one call, then the workspace is read', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    apiMock.cellApply.newScenario.mockResolvedValue(endedApplyRow({ outcome: 'succeeded',
      components: [{ component_id: 'arm', disposition: 'remove', lifecycle_after: 'offline' }] }, 'scenario_new'));
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(2));
    await act(async () => { await latest?.newScenarioDocument(); });
    expect(apiMock.cellApply.newScenario).toHaveBeenCalledOnce();
    expect(apiMock.cellApply.load).not.toHaveBeenCalled();
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.message).toContain('New Scenario');
    expect(scenarioOpened).toHaveBeenCalledOnce();
    expect(latest?.scenarioDraftInputResetEpoch).toBe(1);
  });

  it('a failed New shows its typed refusal and is not retried', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    apiMock.cellApply.newScenario.mockRejectedValueOnce(new ApiError('A run is in progress.', 409, {
      detail: { code: 'apply_refused_run_in_progress', message: 'A run is in progress.' },
    }));
    let result: unknown;
    await act(async () => { result = await latest?.newScenarioDocument(); });
    expect(result).toBeNull();
    expect(latest?.scenarioDraftInputResetEpoch).toBe(0);
    expect(apiMock.cellApply.newScenario).toHaveBeenCalledOnce();
    expect(latest?.state.error).toContain('New Scenario refused');
    expect(scenarioOpened).not.toHaveBeenCalled();
  });

  it('a dropped Load connection is no refusal: it shows the current Scenario and is not retried', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    const digest = `sha256:${'b'.repeat(64)}`;
    const fetchMock = vi.fn(async (_url: string) => (void _url, { ok: true, status: 200, json: async () => ({
      schema_version: 'manyforge.current_scenario.v1', ready: true, unavailable_reasons: [],
      current_scenario: {
        scenario_id: 'mock_lite6_motion_preemption', source_document_digest: digest,
        deployment_artifact_id: 'mock_lite6_motion_preemption', deployment_revision: digest,
        retained_bundle_id: digest, graph_revision: digest, program_id: 'program',
        program_revision: digest, program_binding_id: digest,
      },
    }) }));
    vi.stubGlobal('fetch', fetchMock);
    apiMock.cellApply.load.mockRejectedValueOnce(
      new TypeError('NetworkError when attempting to fetch resource.'));
    let refusal: string | null = null;
    await act(async () => {
      await latest?.loadScenarioDocument('/workspace/cell', (message) => { refusal = message; });
    });
    expect(apiMock.cellApply.load).toHaveBeenCalledOnce();
    expect(fetchMock).toHaveBeenCalledExactlyOnceWith('/api/scenarios/current', expect.anything());
    expect(refusal).toBe('Scenario Load: the connection dropped before the reply arrived. '
      + 'Current Scenario: mock_lite6_motion_preemption, applied. Nothing was retried.');
    expect(refusal).not.toContain('refused');
    expect(scenarioOpened).not.toHaveBeenCalled();

    // The current Scenario cannot be read either: said so, still no refusal.
    fetchMock.mockRejectedValueOnce(new TypeError('NetworkError when attempting to fetch resource.'));
    apiMock.cellApply.newScenario.mockRejectedValueOnce(new TypeError('Failed to fetch'));
    await act(async () => { await latest?.newScenarioDocument((message) => { refusal = message; }); });
    expect(refusal).toBe('New Scenario: the connection dropped before the reply arrived. '
      + 'The current Scenario could not be read: NetworkError when attempting to fetch resource. Nothing was retried.');
  });

  it('shows a written-but-not-adopted Scenario Save with its path and refreshes once', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(
      initial,
      authoringSnapshot(workspace(1)),
      testPublication(workspace(1)),
    );
    await act(async () => initial.resolve(workspace(1)));
    const message = 'The Scenario project was written to /workspace/cell, but it was not '
      + 'adopted as the Saved baseline: model source vanished';
    apiMock.scenarioDocuments.save.mockRejectedValueOnce(new ApiError(message, 409, {
      detail: {
        code: 'scenario_saved_baseline_not_adopted',
        message,
        path: '/workspace/cell',
        causeCode: 'scenario_source_invalid',
      },
    }));
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(2));

    await act(async () => {
      latest?.saveScenarioDocument({
        directory: '/workspace',
        name: 'cell',
        description: '',
        confirmOverwrite: false,
      });
      await Promise.resolve();
      await Promise.resolve();
    });

    // Never retried; the current truth is read once and the outcome stays visible.
    expect(apiMock.scenarioDocuments.save).toHaveBeenCalledTimes(1);
    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(2);
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.message).toBe(message);
    expect(latest?.state.message).toContain('/workspace/cell');
  });

  it('fences Scenario Save with both current authored documents; Load is one server call', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(
      initial,
      authoringSnapshot(workspace(1)),
      testPublication(workspace(1)),
    );
    await act(async () => initial.resolve(workspace(1)));
    apiMock.scenarioDocuments.save.mockResolvedValue({
      status: 'saved',
      path: '/workspace/cell',
      scenarioId: 'cell',
      lastKnownStatus: 'unknown',
      workspace: workspace(1),
    });
    await act(async () => {
      latest?.saveScenarioDocument({
        directory: '/workspace',
        name: 'cell',
        description: '',
        confirmOverwrite: false,
      });
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(apiMock.scenarioDocuments.save).toHaveBeenCalledWith(
      expect.objectContaining({
        draftId: 'draft-1',
        expectedDraftRevision: 1,
        deploymentDraftId: 'deployment-draft-1',
        expectedDeploymentDraftRevision: 4,
      }),
      expect.objectContaining({ directory: '/workspace', name: 'cell' }),
    );

    apiMock.cellApply.load.mockResolvedValueOnce(endedApplyRow({ outcome: 'succeeded',
      components: [{ component_id: 'arm', disposition: 'add', lifecycle_after: 'prepared' }] }, 'scenario_load'));
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(1));
    await act(async () => { await latest?.loadScenarioDocument('/workspace/cell'); });
    expect(apiMock.cellApply.load).toHaveBeenCalledExactlyOnceWith('/workspace/cell');
    expect(latest?.state.message).toContain('arm prepared');
    expect(latest?.state.message).toContain('Enable the components to run');
    expect(scenarioOpened).toHaveBeenCalledTimes(1);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(1);

    // A failed Load (ApplyResult outcome failed) is shown and does not count as opened.
    apiMock.cellApply.load.mockResolvedValueOnce(endedApplyRow({ outcome: 'failed', components: [],
      error: { code: 'scenario_source_invalid', message: 'bad file' } }, 'scenario_load'));
    await act(async () => { await latest?.loadScenarioDocument('/workspace/cell'); });
    expect(apiMock.cellApply.load).toHaveBeenCalledTimes(2);
    expect(latest?.state.error).toContain('Apply failed');
    expect(scenarioOpened).toHaveBeenCalledTimes(1);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(1);
    // A Load whose preparation refused it ends its row refused (OD-214): it
    // is shown as a refusal, as its HTTP refusal was.
    const refusals: string[] = [];
    apiMock.cellApply.load.mockResolvedValueOnce({
      kind: 'scenario_load', requestKey: 'e'.repeat(32), step: 'previewing', stepSource: null,
      acceptedAtNs: '1', end: { outcome: 'refused', code: 'activation_preparation_failed',
        details: null, finishedAtNs: '2' },
    } satisfies ApplyCommandRow);
    await act(async () => {
      await latest?.loadScenarioDocument('/workspace/cell', (message) => { refusals.push(message); });
    });
    expect(refusals).toEqual([
      `Scenario Load refused: ${codedErrorText({ code: 'activation_preparation_failed', message: '' })}`,
    ]);
    expect(scenarioOpened).toHaveBeenCalledTimes(1);
  });

  it('rebases text only after successful Program, Scenario, and open-Draft resets', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const snapshot = authoringSnapshot(workspace(2));
    const rerender = await mountHarness(initial, snapshot, testPublication(workspace(2)));
    await act(async () => initial.resolve(workspace(2)));
    expect(latest?.programDraftInputResetEpoch).toBe(0);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.programWorkspace.discard.mockResolvedValueOnce(workspace(1));
    await act(async () => {
      latest?.discard();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.programDraftInputResetEpoch).toBe(1);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.scenarioDocuments.discard.mockRejectedValueOnce(new Error('discard failed'));
    await act(async () => {
      latest?.discardScenario();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.programDraftInputResetEpoch).toBe(1);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(0);
    apiMock.scenarioDocuments.discard.mockResolvedValueOnce(workspace(1));
    await act(async () => {
      latest?.discardScenario();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.programDraftInputResetEpoch).toBe(1);
    expect(latest?.scenarioDraftInputResetEpoch).toBe(1);

    apiMock.programWorkspace.revert.mockRejectedValueOnce(new Error('revert failed'));
    await act(async () => {
      latest?.revert();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.scenarioDraftInputResetEpoch).toBe(1);
    apiMock.programWorkspace.revert.mockResolvedValueOnce(
      endedProgramApplyRow('program_revert', { applyId: 5, applyGeneration: 2 }),
    );
    apiMock.programWorkspace.get.mockResolvedValueOnce(workspace(1));
    await act(async () => {
      latest?.revert();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.scenarioDraftInputResetEpoch).toBe(2);
    expect(latest?.state.transitionStatus).toBe('accepted');

    // OD-214: a Revert answered at acceptance is followed to its row's end;
    // one that ended refused shows its catalogue text as its refusal.
    const accepted: ApplyCommandRow = {
      kind: 'program_revert', requestKey: 'd'.repeat(32), step: 'previewing',
      stepSource: null, acceptedAtNs: '1', end: null,
    };
    apiMock.programWorkspace.revert.mockResolvedValueOnce(accepted);
    await act(async () => {
      latest?.revert();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(apiMock.programWorkspace.revert).toHaveBeenCalledTimes(3);
    expect(latest?.state.error).toBeNull();
    await rerender({ ...snapshot, applyCommands: { inProgress: null, ended: [{ ...accepted,
      end: { outcome: 'refused', code: 'scenario_revert_requires_deployment', details: null, finishedAtNs: '2' },
    }] } });
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(latest?.state.error).toBe(
      'Program Revert: Revert the whole Scenario. The Saved Deployment differs from the Applied one. '
      + 'Use the Deployment Revert to revert the whole Scenario.',
    );
    expect(latest?.scenarioDraftInputResetEpoch).toBe(2);

    apiMock.programWorkspace.openDraft.mockRejectedValueOnce(new Error('open failed'));
    await act(async () => {
      latest?.openDraft('/workspace/other.program.yaml', true);
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.programDraftInputResetEpoch).toBe(1);
    apiMock.programWorkspace.openDraft.mockResolvedValueOnce(workspace(2));
    await act(async () => {
      latest?.openDraft('/workspace/other.program.yaml', true);
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.programDraftInputResetEpoch).toBe(2);
  });
});
