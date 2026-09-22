import { act, createElement, useEffect, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  AuthoringWorkspaceSnapshot,
  ProgramAuthoringDocument,
  ProgramDocument,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({
  authoringWorkspace: {
    get: vi.fn(),
  },
}));

vi.mock('../api/client', () => ({
  api: apiMock,
  isStaleWorkspaceError: (error: unknown) =>
    error !== null
    && typeof error === 'object'
    && (error as { status?: number }).status === 409,
}));

import { useAuthoringWorkspace } from './useAuthoringWorkspace';

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

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function snapshotFixture(
  workspaceRevision: number,
  programDraftRevision: number,
  deploymentDraftRevision: number,
  documentPath: string,
): AuthoringWorkspaceSnapshot {
  const programRevision = workspaceRevision % 2 === 0
    ? null
    : `sha256:${String(programDraftRevision).padStart(64, '0')}`;
  return {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v1',
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision,
    program: {
      draft: {
        draftId: 'draft.1',
        draftRevision: programDraftRevision,
        programLoaded: true,
        authoring: { forest: [] } as unknown as ProgramAuthoringDocument,
        candidateAvailable: programRevision !== null,
        programRevision,
        program: (
          programRevision === null
            ? null
            : { api_version: 'manyforge.program.v2' }
        ) as unknown as ProgramDocument | null,
        diagnostics: [],
      },
      saved: {
        available: false,
        matchesDraft: false,
        path: null,
        programRevision: null,
      },
    },
    deployment: {
      draftId: 'deployment-draft.1',
      draftRevision: deploymentDraftRevision,
      document: { deployment_revision: 'deployment.1' },
      savedPath: null,
      savedDocument: null,
      differsFromSaved: true,
    },
    project: {
      documentPath,
      documentId: 'cell.1',
      documentName: 'Cell',
      documentDescription: '',
      sourceDiagnostics: [],
      sourceDocumentDigest: null,
      programRoleAssignment: null,
      typedParameterOverrides: {},
      sourceProgramDigest: null,
      sourceDeploymentDigest: null,
    },
  };
}

function headlessSnapshot(
  workspaceRevision: number,
  programDraftRevision = 3,
  deploymentDraftRevision = 4,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    executionControlStatus: null,
    runtime: {} as ComposerHeadlessState['runtime'],
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
    alarms: { records: [], generation: 1 } as unknown as ComposerHeadlessState['alarms'],
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 4 },
    programCycle: { state: 'idle_after_reset' } as unknown as ComposerHeadlessState['programCycle'],
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision,
      programDraftId: 'draft.1',
      programDraftRevision,
      deploymentDraftId: 'deployment-draft.1',
      deploymentDraftRevision,
    },
    receiverEpoch: 'receiver.epoch.1',
    snapshotSequence: workspaceRevision,
  };
}

let root: { render: (value: ReactNode) => void; unmount: () => void } | null = null;
let latest: ReturnType<typeof useAuthoringWorkspace> | null = null;

async function mountHarness(initialSnapshot: ComposerHeadlessState | null) {
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
  function Harness({
    currentSnapshot,
  }: {
    currentSnapshot: ComposerHeadlessState | null;
  }) {
    const controller = useAuthoringWorkspace(currentSnapshot);
    useEffect(() => {
      latest = controller;
      return () => {
        latest = null;
      };
    }, [controller]);
    return null;
  }
  await act(async () => {
    root?.render(createElement(Harness, { currentSnapshot: initialSnapshot }));
  });
  return async (nextSnapshot: ComposerHeadlessState | null) => {
    await act(async () => {
      root?.render(createElement(Harness, { currentSnapshot: nextSnapshot }));
    });
  };
}

async function flush(): Promise<void> {
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
  });
}

describe('useAuthoringWorkspace', () => {
  beforeEach(() => {
    apiMock.authoringWorkspace.get.mockReset();
  });

  afterEach(async () => {
    if (root !== null) {
      await act(async () => root?.unmount());
    }
    await new Promise<void>((resolve) => setImmediate(resolve));
    root = null;
    latest = null;
    vi.unstubAllGlobals();
  });

  it('reads the composite snapshot once per observed identity', async () => {
    const first = deferred<AuthoringWorkspaceSnapshot>();
    const second = deferred<AuthoringWorkspaceSnapshot>();
    apiMock.authoringWorkspace.get
      .mockReturnValueOnce(first.promise)
      .mockReturnValueOnce(second.promise);

    const rerender = await mountHarness(headlessSnapshot(1));
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);

    first.resolve(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await flush();
    expect(latest?.published?.identity.workspaceRevision).toBe(1);
    expect(latest?.current).toBe(true);
    expect(latest?.project?.documentPath).toBe('/workspace/cell');

    // Re-announcing the same identity must not read again, however many Deltas
    // arrive: the controller is not a poll loop.
    await rerender(headlessSnapshot(1));
    await rerender(headlessSnapshot(1));
    await flush();
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);

    // A newer identity reads exactly once more.
    await rerender(headlessSnapshot(2));
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);
    second.resolve(snapshotFixture(2, 3, 4, '/workspace/cell'));
    await flush();
    expect(latest?.published?.identity.workspaceRevision).toBe(2);
  });

  it('discards a superseded read so a revision gap publishes only the newest identity', async () => {
    const stale = deferred<AuthoringWorkspaceSnapshot>();
    const newest = deferred<AuthoringWorkspaceSnapshot>();
    apiMock.authoringWorkspace.get
      .mockReturnValueOnce(stale.promise)
      .mockReturnValueOnce(newest.promise);

    const rerender = await mountHarness(headlessSnapshot(1));
    await rerender(headlessSnapshot(5, 9, 9));
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);

    newest.resolve(snapshotFixture(5, 9, 9, '/workspace/cell'));
    await flush();
    // The older response arrives last and must not replace the newest one.
    stale.resolve(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await flush();

    expect(latest?.published?.identity.workspaceRevision).toBe(5);
    expect(latest?.current).toBe(true);
  });

  it('keeps authoring gated with the same reason until the exact identity is installed', async () => {
    const pending = deferred<AuthoringWorkspaceSnapshot>();
    apiMock.authoringWorkspace.get.mockReturnValueOnce(pending.promise);

    const rerender = await mountHarness(headlessSnapshot(1));
    expect(latest?.published).toBeNull();
    expect(latest?.current).toBe(false);
    expect(latest?.unavailableReason).toBe(
      'The shared authoring workspace has not been read yet.',
    );

    pending.resolve(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await flush();
    expect(latest?.current).toBe(true);
    expect(latest?.unavailableReason).toBeNull();

    // The control stream moves on while the replacement read is still in
    // flight: the installed publication is superseded and authoring is gated
    // again with one stable reason.
    apiMock.authoringWorkspace.get.mockReturnValueOnce(
      deferred<AuthoringWorkspaceSnapshot>().promise,
    );
    await rerender(headlessSnapshot(2));
    await flush();
    expect(latest?.current).toBe(false);
    expect(latest?.unavailableReason).toBe(
      'The displayed workspace is still synchronizing with the backend.',
    );
  });

  it('never repeats a failed read for the same identity', async () => {
    apiMock.authoringWorkspace.get.mockRejectedValueOnce(
      Object.assign(new Error('read failed'), { status: 409 }),
    );

    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);
    expect(latest?.published).toBeNull();
    expect(latest?.error).toBe('read failed');
    // The same identity is not polled, and the request is not retried blindly.
    await rerender(headlessSnapshot(1));
    await flush();
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);

    // A newer advertised identity is the only thing that reads again, and the
    // explicit operator refresh reads the newest identity once.
    apiMock.authoringWorkspace.get.mockResolvedValueOnce(
      snapshotFixture(1, 3, 4, '/workspace/cell'),
    );
    await act(async () => {
      await latest?.refresh();
    });
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);
    expect(latest?.published?.identity.workspaceRevision).toBe(1);
    expect(latest?.error).toBeNull();
  });

  it('serves two mounted consumers from one publication', async () => {
    const read = deferred<AuthoringWorkspaceSnapshot>();
    apiMock.authoringWorkspace.get.mockReturnValueOnce(read.promise);

    const rerender = await mountHarness(headlessSnapshot(1));
    read.resolve(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await flush();
    const first = latest?.published;

    // A second consumer observing the same identity reuses the installation
    // instead of establishing its own authoring identity.
    await rerender(headlessSnapshot(1));
    await flush();
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);
    expect(latest?.published).toBe(first);
    expect(latest?.published?.snapshot.project.documentPath).toBe('/workspace/cell');
  });
});
