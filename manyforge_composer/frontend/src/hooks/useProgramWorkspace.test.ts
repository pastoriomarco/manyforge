import { act, createElement, useEffect, useMemo, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
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
  },
  scenarioDocuments: {
    save: vi.fn(),
    load: vi.fn(),
    new: vi.fn(),
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
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { ProgramDraftChangedError, useProgramWorkspace } from './useProgramWorkspace';
import { ApiError } from '../api/client';

/**
 * A real shared-publication controller for this hook's tests.
 *
 * It uses the same pure gate and fence helpers the production controller uses, so
 * the hook is exercised against the real contract rather than a behavioural
 * stub; only the read loop itself belongs to `useAuthoringWorkspace`'s own test.
 */
function testAuthoringController(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): AuthoringWorkspaceController {
  return {
    published,
    current: publicationIsCurrent(published, snapshot),
    unavailableReason: authoringUnavailableReason(published, snapshot),
    editUnavailableReason: draftEditUnavailableReason(published, snapshot),
    project: projectFacts(published),
    reading: false,
    error: null,
    refresh: async () => undefined,
    beginMutation: () => true,
    finishMutation: () => undefined,
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
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v2' as const,
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
      programRoleAssignment: null,
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
    api_version: 'manyforge.program.v2' as const,
    program_id: 'program-1',
    program_revision: programRevision,
    program_profile: 'composable-cell-v1' as const,
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v2' as const,
      content_version: 'fixture',
    },
    component_roles: {},
    target_roles: {},
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
        schema_version: 'manyforge.composer_authoring_forest.v1',
        api_version: program.api_version,
        program_id: program.program_id,
        program_profile: program.program_profile,
        name: program.name,
        catalog: program.catalog,
        component_roles: program.component_roles,
        target_roles: program.target_roles,
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
  function Harness({ currentSnapshot }: { currentSnapshot: ComposerHeadlessState | null }) {
    const currentPublication = useMemo(() => {
      if (published === null || currentSnapshot === null) return published;
      const current = workspace(
        currentSnapshot.authoringWorkspace.programDraftRevision,
      );
      const nextSnapshot = {
        ...published.snapshot,
        workspaceEpoch: currentSnapshot.authoringWorkspace.workspaceEpoch,
        workspaceRevision: currentSnapshot.authoringWorkspace.workspaceRevision,
        program: { draft: current.draft, saved: current.saved },
        deployment: {
          ...published.snapshot.deployment,
          draftId: currentSnapshot.authoringWorkspace.deploymentDraftId,
          draftRevision: currentSnapshot.authoringWorkspace.deploymentDraftRevision,
        },
      };
      return {
        identity: authoringWorkspaceIdentity(nextSnapshot),
        snapshot: nextSnapshot,
      };
    }, [currentSnapshot]);
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
    return null;
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
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    executionControlStatus: null,
    componentOverview: {
      schemaVersion: 'manyforge.component_overview.v2',
      overviewRevision: `sha256:${'a'.repeat(64)}`,
      readiness: 'ready',
      summaryLabel: 'Ready',
      profileLabel: 'native',
      authorityLabel: 'owner-issued scoped facts',
      userMessage: 'Current.',
      capabilities: [],
      components: [],
      maintenanceOperation: null,
      basis: {
        runtimeProjectionOwnerEpoch: null,
        runtimeProjectionGeneration: null,
        managerSourceOwnerEpoch: null,
        logicalSceneSourceOwnerEpoch: null,
        logicalSceneSourceGeneration: null,
        sceneRuntimeSourceOwnerEpoch: null,
        sceneRuntimeSourceGeneration: null,
        alarmOwnerEpoch: null,
        alarmGeneration: null,
        alarmStateRevision: null,
        admissionGeneration: null,
        programRunnerOwnerEpoch: null,
        programCycleGeneration: null,
        appliedProgramOwnerEpoch: null,
        appliedProgramGeneration: null,
        executionOwnerEpoch: null,
        executionPublicationGeneration: null,
      },
    },
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
    heldRunDraftEditAllowed: false,
    programCycle: {
      programRunnerOwnerEpoch: 'program-runner.epoch.1',
      cycleGeneration: 3,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      terminalOrSettlementReference: null,
      runRevision: null,
      nodeStatusRevision: null,
      runtimeControlRevision: null,
      executionControlRevision: 1,
      causalCorrelationId: null,
      causalSessionId: null,
      causalClientSequence: null,
      currentCommandKind: null,
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
    receiverEpoch: 'headless-receiver.epoch.1',
    snapshotSequence: 11,
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      activeConfiguration: null,
      complete: true,
      projectionGeneration: 1,
      projectionOwnerEpoch: 'runtime.epoch.1',
      projectionOwnerId: 'runtime.owner',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1, sourceOwnerEpoch: 'manager.epoch.1',
          sourceOwnerId: 'manager.owner', receiverAgeNs: 1,
        },
        logicalScene: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1, sourceOwnerEpoch: 'scene.epoch.1',
          sourceOwnerId: 'scene.owner', receiverAgeNs: 1,
        },
        sceneRuntime: {
          available: true, freshness: 'fresh', knownGaps: [], payload: {},
          sourceGeneration: 1, sourceOwnerEpoch: 'runtime.epoch.1',
          sourceOwnerId: 'runtime.owner', receiverAgeNs: 1,
        },
      },
    },
    alarms: {
      schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
      ownerId: 'alarm.owner',
      ownerEpoch: 'alarm.epoch.1',
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
          requestOwnerEpoch: expect.stringMatching(/^composer-authoring-owner\./),
          requestId: expect.stringMatching(/^composer-authoring\./),
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
    apiMock.programWorkspace.apply.mockResolvedValueOnce({
      status: 'accepted',
      workspace: workspace(2),
      headless: { apply_id: 'apply-1', apply_generation: 2 },
    });
    await act(async () => {
      await latest?.apply({ correlationId: 'reset-apply.1', expectedDraft });
    });
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ draftId: 'draft-1', expectedDraftRevision: 2, expectedAdmissionGeneration: 12 }),
      undefined,
      'reset-apply.1',
    );
    // Another browser's edit is now installed: the confirmed content is gone,
    // so no Apply is sent, with a named refusal.
    apiMock.programWorkspace.get.mockResolvedValue(workspace(3));
    await rerender(authoringSnapshot(workspace(3)));
    await expect(latest!.apply({ correlationId: 'reset-apply.1', expectedDraft }))
      .rejects.toBeInstanceOf(ProgramDraftChangedError);
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
  });

  it('New loads Untitled with the shared fence and follows the exact accepted Apply without a second command', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)), testPublication(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    apiMock.scenarioDocuments.new.mockResolvedValue({
      status: 'accepted', workspace: workspace(1),
      activation: { status: 'accepted', apply_id: 'apply.new', apply_generation: 9 },
    });
    const confirmedAuthoring = {
      workspaceEpoch: 'confirmed.workspace', workspaceRevision: 5,
      programDraftId: 'confirmed.program', programDraftRevision: 8,
      deploymentDraftId: 'confirmed.deployment', deploymentDraftRevision: 13,
    };
    await act(async () => { await latest?.newScenarioDocument(true, confirmedAuthoring); });
    expect(apiMock.scenarioDocuments.new).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({
        expectedWorkspaceEpoch: 'confirmed.workspace', expectedWorkspaceRevision: 5,
        draftId: 'confirmed.program', expectedDraftRevision: 8,
        deploymentDraftId: 'confirmed.deployment', expectedDeploymentDraftRevision: 13,
      }), true,
    );
    expect(latest?.acceptedApply).toEqual({ applyId: 'apply.new', applyGeneration: 9 });
    expect(latest?.state.message).toContain('New Scenario accepted (apply.new)');
    expect(latest?.state.message).toContain('Motion does not start automatically');
    expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
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

  it('fences Scenario Save and Load with both current authored documents', async () => {
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
    apiMock.scenarioDocuments.load.mockResolvedValue({
      status: 'accepted',
      path: '/workspace/cell',
      scenarioId: 'cell',
      activation: {
        status: 'accepted', apply_id: 'apply.1', apply_generation: 1,
      },
      diagnostics: [],
      sourceDiagnostics: [],
      incomplete: false,
      workspace: workspace(1),
      authoringWorkspace: authoringSnapshot(workspace(1)),
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
    expect(latest?.scenarioDocumentPath).toBe('/workspace/cell');

    const confirmedAuthoring = {
      workspaceEpoch: 'load.confirmed', workspaceRevision: 6,
      programDraftId: 'program.confirmed', programDraftRevision: 7,
      deploymentDraftId: 'deployment.confirmed', deploymentDraftRevision: 10,
    };
    await act(async () => {
      latest?.loadScenarioDocument(
        '/workspace/cell',
        true,
        confirmedAuthoring,
      );
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(apiMock.scenarioDocuments.load).toHaveBeenCalledWith(
      expect.objectContaining({
        expectedWorkspaceEpoch: 'load.confirmed', expectedWorkspaceRevision: 6,
        draftId: 'program.confirmed', expectedDraftRevision: 7,
        deploymentDraftId: 'deployment.confirmed', expectedDeploymentDraftRevision: 10,
      }),
      '/workspace/cell',
      true,
    );
    expect(latest?.state.message).toContain('Scenario Load accepted');
    expect(latest?.state.message).toContain('Motion does not start automatically');
    expect(latest?.state.message).not.toContain('Check Runtime / history');
    // Activity follows the exact accepted Manager operation, not any newer one.
    expect(latest?.acceptedApply?.applyId).toBe('apply.1');
    // A successful Load reply asks the page to read health again.
    expect(scenarioOpened).toHaveBeenCalledTimes(1);

    apiMock.scenarioDocuments.load.mockResolvedValueOnce({
      status: 'applied',
      path: '/workspace/cell',
      scenarioId: 'cell',
      activation: { status: 'applied', apply_id: null },
      diagnostics: [],
      sourceDiagnostics: [],
      incomplete: false,
      workspace: workspace(1),
      authoringWorkspace: authoringSnapshot(workspace(1)),
    });
    await act(async () => {
      latest?.loadScenarioDocument('/workspace/cell', false);
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(latest?.state.message).toContain('already match the Applied configuration');
    expect(latest?.state.message).toContain('No configuration change was needed');
    expect(latest?.state.transitionStatus).toBe('applied');
    expect(scenarioOpened).toHaveBeenCalledTimes(2);

    // A refused or unconfirmed Load does not.
    apiMock.scenarioDocuments.load.mockRejectedValueOnce(new Error('activation not confirmed'));
    await act(async () => {
      latest?.loadScenarioDocument('/workspace/cell', false);
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(scenarioOpened).toHaveBeenCalledTimes(2);
  });
});
