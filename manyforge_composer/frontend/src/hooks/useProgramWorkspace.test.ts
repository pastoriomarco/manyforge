import { act, createElement, useEffect, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  DraftMutationResponse,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({
  programWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
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
    schemaVersion: 'manyforge.composer_program_workspace.v2',
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
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    componentOverview: {
      schemaVersion: 'manyforge.component_overview.v1',
      overviewRevision: `sha256:${'a'.repeat(64)}`,
      readiness: 'ready',
      summaryLabel: 'Ready',
      profileLabel: 'native',
      authorityLabel: 'owner-issued scoped facts',
      userMessage: 'Current.',
      capabilities: [],
      components: [],
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
      recoveryOffer: null,
      run: null,
      runtimeControls: null,
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

  it('uses the ordinary Draft mutation path for node policies', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial, authoringSnapshot(workspace(1)));
    await act(async () => initial.resolve(workspace(1)));
    const mutation = deferred<DraftMutationResponse>();
    apiMock.programWorkspace.mutate.mockReturnValueOnce(mutation.promise);

    await act(async () => {
      latest?.updateNodePolicies(
        'root',
        modificationPolicy,
        recoveryPolicy,
        operatorStopPolicy,
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
          modification_policy: modificationPolicy,
          recovery_policy: recoveryPolicy,
          operator_stop_policy: operatorStopPolicy,
        },
      );
    });
    await act(async () => mutation.resolve({
      mutation: 'program_draft_update_node_policies',
      details: { updated_instance_id: 'root' },
      workspace: workspace(2),
    }));
    expect(latest?.state.workspace?.draft.draftRevision).toBe(2);
    expect(latest?.state.pendingAction).toBeNull();
  });
});
