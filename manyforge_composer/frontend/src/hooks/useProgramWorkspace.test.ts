import { act, createElement, useEffect, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramNodePolicyMutationResponse,
  ProgramOperatorStopPolicy,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({
  programWorkspace: {
    get: vi.fn(),
    updateNodePolicies: vi.fn(),
  },
}));

vi.mock('../api/client', () => ({ api: apiMock }));

import { useProgramWorkspace } from './useProgramWorkspace';

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
    api_version: 'manyforge.program.v1' as const,
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
    schemaVersion: 'manyforge.composer_program_workspace.v1',
    draft: {
      draftId: 'draft-1',
      draftRevision,
      programLoaded: true,
      programRevision,
      program,
    },
    applicability: {
      applyReady: true,
      affectedInstanceIds: [],
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

const modificationPolicy: ProgramNodeModificationPolicy = {
  runtime_override: 'human_approval',
  persistent_edit: 'human_approval',
  intervention_delegate: 'human_only',
};
const recoveryPolicy: ProgramNodeRecoveryPolicy = {
  allow_intervention: true,
  allowed_recovery_actions: ['intervention'],
  max_retries: 2,
  allowed_skills: [],
  persistence_ceiling: 'draft_patch',
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

async function mountHarness(
  initial: Deferred<ProgramWorkspaceResponse>,
  snapshot: ComposerHeadlessState | null = null,
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
    const controller = useProgramWorkspace(currentSnapshot);
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
    schemaVersion: 'manyforge.composer_headless_state.v1',
    complete: true,
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
    programCycle: {
      programRunnerOwnerEpoch: 'program-runner.epoch.1',
      cycleGeneration: 3,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      terminalOrSettlementReference: null,
    },
    draftIdentity: {
      draftId: current.draft.draftId,
      draftRevision: current.draft.draftRevision,
    },
    receiverEpoch: 'headless-receiver.epoch.1',
    snapshotSequence: 11,
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
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
      unacknowledgedMembership: [],
      alarms: [],
      truncated: false,
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
    const rerender = await mountHarness(initial, authoringSnapshot(workspace(1)));
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

  it('shows a Draft-only result before one follow-up GET, then retains it on stale refresh', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));

    const mutation = deferred<ProgramNodePolicyMutationResponse>();
    const refresh = deferred<ProgramWorkspaceResponse>();
    apiMock.programWorkspace.updateNodePolicies.mockReturnValueOnce(mutation.promise);
    apiMock.programWorkspace.get.mockReturnValueOnce(refresh.promise);

    await act(async () => {
      latest?.updateNodePolicies(
        'root',
        modificationPolicy,
        recoveryPolicy,
        operatorStopPolicy,
      );
    });
    await act(async () => mutation.resolve({
      schemaVersion: 'manyforge.composer_draft_mutation.v1',
      mutation: 'program_draft_update_node_policies',
      details: { updatedInstanceId: 'root' },
      draft: workspace(2).draft,
    }));

    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.pendingAction).toBe('refresh');
    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(2);

    await act(async () => refresh.reject(new Error('owner unavailable')));
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.refreshWarning).toBe(
      'Draft updated; workspace status refresh failed. Your Draft change remains; retry refresh.',
    );
    expect(latest?.state.comparisonStatus).toBe('stale');
    expect(latest?.state.pendingAction).toBeNull();
  });

  it('sends exact fences and policy payload, including no-op and successful refresh', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    const mutation = deferred<ProgramNodePolicyMutationResponse>();
    const refresh = deferred<ProgramWorkspaceResponse>();
    apiMock.programWorkspace.updateNodePolicies.mockReturnValueOnce(mutation.promise);
    apiMock.programWorkspace.get.mockReturnValueOnce(refresh.promise);

    await act(async () => {
      latest?.updateNodePolicies(
        'root',
        modificationPolicy,
        recoveryPolicy,
        operatorStopPolicy,
      );
      expect(apiMock.programWorkspace.updateNodePolicies).toHaveBeenCalledWith(
        expect.objectContaining({
          draftId: 'draft-1',
          expectedDraftRevision: 1,
          expectedAdmissionGeneration: 7,
          requestOwnerEpoch: expect.stringMatching(/^composer-authoring-owner\./),
          requestId: expect.stringMatching(/^composer-authoring\./),
        }),
        workspace(1).draft.programRevision,
        'root',
        modificationPolicy,
        recoveryPolicy,
        operatorStopPolicy,
      );
    });
    await act(async () => mutation.resolve({
      schemaVersion: 'manyforge.composer_draft_mutation.v1',
      mutation: 'program_draft_update_node_policies',
      details: { updatedInstanceId: 'root' },
      draft: workspace(1).draft,
    }));
    expect(latest?.state.message).toContain('no-op');
    await act(async () => refresh.resolve(workspace(1)));
    expect(latest?.state.comparisonStatus).toBe('observed');
    expect(latest?.state.refreshWarning).toBeNull();
  });

  it('keeps the previous Draft workspace when the policy mutation is stale', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)));
    const original = workspace(1);
    await act(async () => initial.resolve(original));
    apiMock.programWorkspace.updateNodePolicies.mockRejectedValueOnce(
      new Error('composer_program_revision_stale'),
    );

    await act(async () => {
      latest?.updateNodePolicies(
        'root',
        modificationPolicy,
        recoveryPolicy,
        operatorStopPolicy,
      );
    });
    expect(latest?.state.workspace).toStrictEqual(original);
    expect(latest?.state.workspace?.draft.draftRevision).toBe(1);
    expect(latest?.state.error).toContain('composer_program_revision_stale');
    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(1);
  });

  it('uses the newest backend admission generation after a rerender', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    const first = authoringSnapshot(workspace(1));
    const rerender = await mountHarness(initial, first);
    await act(async () => initial.resolve(workspace(1)));
    const next = {
      ...first,
      snapshotSequence: 12,
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 8 },
    };
    await rerender(next);
    apiMock.programWorkspace.updateNodePolicies.mockReturnValueOnce(
      new Promise<ProgramNodePolicyMutationResponse>(() => undefined),
    );
    await act(async () => {
      latest?.updateNodePolicies(
        'root', modificationPolicy, recoveryPolicy, operatorStopPolicy,
      );
    });
    expect(apiMock.programWorkspace.updateNodePolicies).toHaveBeenCalledWith(
      expect.objectContaining({
        expectedAdmissionGeneration: 8,
      }),
      workspace(1).draft.programRevision,
      'root', modificationPolicy, recoveryPolicy, operatorStopPolicy,
    );
  });
});
