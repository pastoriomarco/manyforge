import { act, createElement, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramNodePolicyMutationResponse,
  ProgramOperatorStopPolicy,
  ProgramWorkspaceResponse,
} from '../types/api';

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
  function Harness() {
    latest = useProgramWorkspace();
    return null;
  }
  apiMock.programWorkspace.get.mockReturnValueOnce(initial.promise);
  await act(async () => {
    root?.render(createElement(Harness));
  });
  return initial;
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

  it('shows a Draft-only result before one follow-up GET, then retains it on stale refresh', async () => {
    const initial = deferred<ProgramWorkspaceResponse>();
    await mountHarness(initial);
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
    await mountHarness(initial);
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
        { draftId: 'draft-1', expectedDraftRevision: 1 },
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
    await mountHarness(initial);
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
    expect(latest?.state.workspace).toBe(original);
    expect(latest?.state.workspace?.draft.draftRevision).toBe(1);
    expect(latest?.state.error).toContain('composer_program_revision_stale');
    expect(apiMock.programWorkspace.get).toHaveBeenCalledTimes(1);
  });
});
