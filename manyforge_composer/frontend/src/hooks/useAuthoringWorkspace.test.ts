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
    submissionOutcome: vi.fn(),
  },
  manager: { applyStatus: vi.fn() },
  failedIdentity: vi.fn(),
}));

vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(),
  api: apiMock,
  failedAuthoringRequest: apiMock.failedIdentity,
}));

import { useAuthoringWorkspace } from './useAuthoringWorkspace';
import { managerApplyRecord } from '../test/managerApplyRecord';
import { ApiError, authoringRefusalMessage } from '../api/client';
import type { ControlTransportStatus } from './useHeadlessState';

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
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v2',
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
        formulaReferences: { computedKeys: {}, nodes: {} },
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
    runtime: { sections: { manager: {
      available: true, freshness: 'fresh', knownGaps: [], sourceOwnerId: 'manager',
      sourceOwnerEpoch: 'manager.1', sourceGeneration: 1, receiverAgeNs: 0,
      payload: { apply: { mutation_available: true, records: [], unavailable_reason_code: null } },
    } } } as unknown as ComposerHeadlessState['runtime'],
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
    alarms: { records: [], generation: 1 } as unknown as ComposerHeadlessState['alarms'],
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 4 },
    heldRunDraftEditAllowed: false,
    programCycle: { state: 'idle_after_reset' } as unknown as ComposerHeadlessState['programCycle'],
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision,
      programDraftId: 'draft.1',
      programDraftRevision,
      deploymentDraftId: 'deployment-draft.1',
      deploymentDraftRevision,
    },
    authoringWorkspaceContainment: null,
    receiverEpoch: 'receiver.epoch.1',
    snapshotSequence: workspaceRevision,
  };
}

let root: { render: (value: ReactNode) => void; unmount: () => void } | null = null;
let latest: ReturnType<typeof useAuthoringWorkspace> | null = null;

async function mountHarness(
  initialSnapshot: ComposerHeadlessState | null,
  initialTransport: ControlTransportStatus = 'connected',
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
  function Harness({
    currentSnapshot,
    transport,
  }: {
    currentSnapshot: ComposerHeadlessState | null;
    transport: ControlTransportStatus;
  }) {
    const controller = useAuthoringWorkspace(currentSnapshot, transport);
    useEffect(() => {
      latest = controller;
      return () => {
        latest = null;
      };
    }, [controller]);
    return null;
  }
  await act(async () => {
    root?.render(createElement(Harness, {
      currentSnapshot: initialSnapshot, transport: initialTransport,
    }));
  });
  return async (
    nextSnapshot: ComposerHeadlessState | null,
    transport: ControlTransportStatus = 'connected',
  ) => {
    await act(async () => {
      root?.render(createElement(Harness, { currentSnapshot: nextSnapshot, transport }));
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
    apiMock.failedIdentity.mockReset().mockReturnValue(null);
    apiMock.authoringWorkspace.submissionOutcome.mockReset();
    apiMock.manager.applyStatus.mockReset();
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

  it('keeps the installed publication through same-session delayed and stale recovery', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    const installed = latest?.published;
    for (const status of ['delayed', 'stale', 'connected', 'delayed', 'connected'] as const) {
      await rerender(headlessSnapshot(1), status);
    }
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(1);
    expect(latest?.published).toBe(installed);
  });

  it('blocks a second synchronous submission and hands an accepted Apply to matching owner evidence', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      expect(latest!.beginMutation()).toBe(false);
      latest!.finishMutation({ applyId: 'apply-3', applyGeneration: 3 });
    });
    expect(latest!.unavailableReason).toContain('submitted operation');
    await rerender(headlessSnapshot(1));
    expect(latest!.unavailableReason).toContain('submitted operation');
    const pending = headlessSnapshot(1);
    pending.runtime.sections.manager.payload = { apply: {
      records: [managerApplyRecord()], mutation_available: false, unavailable_reason_code: 'pending',
    } };
    await rerender(pending);
    expect(latest!.unavailableReason).toContain('Manager operation');
    const terminal = headlessSnapshot(1);
    terminal.runtime.sections.manager.payload = { apply: {
      records: [managerApplyRecord('terminal', 'succeeded')], mutation_available: true, unavailable_reason_code: null,
    } };
    terminal.authoringAdmission.authoringAllowed = false;
    await rerender(terminal);
    expect(latest!.unavailableReason).not.toBe(null);
    terminal.authoringAdmission.authoringAllowed = true;
    await rerender({ ...terminal });
    expect(latest!.unavailableReason).toBe(null);
  });

  it('reconstructs the gate from pending owner records on initial load', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const pending = headlessSnapshot(1);
    pending.runtime.sections.manager.payload = { apply: {
      records: [managerApplyRecord()], mutation_available: false, unavailable_reason_code: 'pending',
    } };
    await mountHarness(pending);
    await flush();
    expect(latest!.beginMutation()).toBe(false);
    expect(latest!.unavailableReason).toContain('Manager operation');
  });

  it('handles completion before the response, and exact terminal lookup after inventory eviction', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => { expect(latest!.beginMutation()).toBe(true); });
    const terminal = headlessSnapshot(1);
    terminal.runtime.sections.manager.payload = { apply: {
      records: [managerApplyRecord('terminal', 'succeeded')], mutation_available: true, unavailable_reason_code: null,
    } };
    await rerender(terminal);
    await act(async () => latest!.finishMutation({ applyId: 'apply-3', applyGeneration: 3 }));
    expect(latest!.unavailableReason).toBe(null);
    await rerender(headlessSnapshot(1));
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation({ applyId: 'apply-4', applyGeneration: 4 });
    });
    const result = { applyId: 'apply-4', applyGeneration: 4, state: 'succeeded' } as import('../types/api').ManagerApplyStatus;
    await act(async () => latest!.observeApply({ ...result, applyId: 'other' }));
    expect(latest!.unavailableReason).not.toBe(null);
    await act(async () => latest!.observeApply(result));
    expect(latest!.unavailableReason).toBe(null);
  });

  it('keeps a lost response unknown, but releases a marked pre-effect refusal', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError('not ready', 409, {
        detail: { code: 'authoring_not_ready', effect: 'none' },
      }));
    });
    expect(latest!.unavailableReason).toBe(null);
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError('Applied configuration changed', 409, {
        detail: { code: 'observed_move_configuration_changed', effect: 'none' },
      }));
    });
    expect(latest!.unavailableReason).toBe(null);
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new Error('reply lost'));
    });
    await rerender(headlessSnapshot(1));
    expect(latest!.unavailableReason).toContain('unconfirmed');
    expect(latest!.beginMutation()).toBe(false);
  });

  it('releases the guard for a marked observed-move refusal reported as 503', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    const message = 'current pose/joints could not be read: execution owner disconnected';
    const refusal = new ApiError(message, 503, {
      detail: { code: 'observed_move_unavailable', message, effect: 'none' },
    });
    expect(authoringRefusalMessage(refusal)).toBe(
      `Edit not applied: ${message}. Wait for the robot to report its current state, then try again.`,
    );
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, refusal);
    });
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.unavailableReason).toBe(null);
    await act(async () => { expect(latest!.beginMutation()).toBe(true); });
  });

  it('leaves the editor usable after the live Copy current position refusal', async () => {
    // The exact backend body: the observed-object resolver refuses a carried
    // object before any Draft change, so it carries the before-effect marker.
    const message = 'only one current cell-fixed object with one portable shape can be saved';
    const fetchMock = vi.fn(async () => new Response(
      JSON.stringify({ detail: { code: 'observed_scene_object_ineligible', message, effect: 'none' } }),
      { status: 409, statusText: 'Conflict', headers: { 'Content-Type': 'application/json' } },
    ));
    vi.stubGlobal('fetch', fetchMock);
    const { api: realApi } = await vi.importActual<typeof import('../api/client')>('../api/client');
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => { expect(latest!.beginMutation('edit')).toBe(true); });
    const fence = {
      draftId: 'draft.1', expectedDraftRevision: 3,
      expectedAdmissionGeneration: 4, requestOwnerEpoch: 'browser.1', requestId: 'copy.1',
      expectedWorkspaceEpoch: 'workspace.epoch.1', expectedWorkspaceRevision: 1,
    } as Parameters<typeof realApi.programWorkspace.mutate>[0];
    const failure = await realApi.programWorkspace.mutate(
      fence,
      'program_draft_store_observed_scene_resource',
      { scene_resource_id: 'part' },
    ).then(() => null, (error: unknown) => error);
    expect(failure).toBeInstanceOf(ApiError);
    expect(authoringRefusalMessage(failure)).toBe(`Edit not applied: ${message}`);
    await act(async () => latest!.finishMutation(null, failure));
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.editUnavailableReason).toBe(null);
    await act(async () => { expect(latest!.beginMutation('edit')).toBe(true); });
  });

  it('keeps the guard locked for the same conflict without the before-effect marker', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation('edit')).toBe(true);
      latest!.finishMutation(null, new ApiError('refused', 409, {
        detail: { code: 'observed_scene_object_ineligible' },
      }));
    });
    expect(latest!.submissionUnconfirmed).toBe(true);
    expect(latest!.beginMutation('edit')).toBe(false);
  });

  it('releases the guard when Load or Save refuses a project with a leftover overrides.yaml', async () => {
    // The exact backend refusal body (routes_scenario_documents._raise), read
    // through the real client so the classification sees what production sees.
    const message = '/workspace/cell: Scenario parameter overrides (overrides.yaml) were removed; '
      + 'Program parameters always take their Program defaults.';
    const fetchMock = vi.fn(async () => new Response(
      JSON.stringify({ detail: { code: 'scenario_artifact_version_unsupported', message, effect: 'none' } }),
      { status: 409, statusText: 'Conflict', headers: { 'Content-Type': 'application/json' } },
    ));
    vi.stubGlobal('fetch', fetchMock);
    const { api: realApi } = await vi.importActual<typeof import('../api/client')>('../api/client');
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    const fence = {
      draftId: 'draft.1', expectedDraftRevision: 3,
      deploymentDraftId: 'deployment-draft.1', expectedDeploymentDraftRevision: 4,
      expectedAdmissionGeneration: 4, requestOwnerEpoch: 'browser.1', requestId: 'load.1',
      expectedWorkspaceEpoch: 'workspace.epoch.1', expectedWorkspaceRevision: 1,
    } as Parameters<typeof realApi.scenarioDocuments.load>[0];
    for (const request of [
      () => realApi.scenarioDocuments.load(fence, '/workspace/cell', false),
      () => realApi.scenarioDocuments.save(fence, {
        path: '/workspace/cell', scenarioId: 'cell.1', name: 'Cell', description: '', confirmOverwrite: true,
      }),
    ]) {
      await act(async () => { expect(latest!.beginMutation()).toBe(true); });
      const failure = await request().then(() => null, (error: unknown) => error);
      expect(failure).toBeInstanceOf(ApiError);
      expect(authoringRefusalMessage(failure)).toBe(
        `Not applied: ${message.replace(/\.$/, '')}. Nothing was changed.`,
      );
      await act(async () => latest!.finishMutation(null, failure));
      expect(latest!.submissionUnconfirmed).toBe(false);
      expect(latest!.unavailableReason).toBe(null);
    }
    expect(fetchMock).toHaveBeenCalledTimes(2);
    // Authoring stays usable: the next change is admitted.
    await act(async () => { expect(latest!.beginMutation()).toBe(true); });
  });

  it('releases a refused-before-effect stale fence without locking', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError('stale', 409, {
        detail: { code: 'composer_draft_revision_stale', message: 'stale', effect: 'none' },
      }));
    });
    expect(latest!.submissionUnconfirmed).toBe(false);
    await act(async () => { expect(latest!.beginMutation()).toBe(true); });
  });

  it.each([
    'program_saved_baseline_not_adopted',
    'deployment_saved_baseline_not_adopted',
    'scenario_saved_baseline_not_adopted',
  ])('releases the guard for the definite written-but-not-adopted outcome %s', async (code) => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    // Written, then not read back or adopted: no before-effect marker, but a
    // definite outcome naming the written path, so the editor never locks.
    const message = 'The document was written to /workspace/cell, but it was not adopted as the Saved baseline';
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError(message, 409, {
        detail: { code, message, path: '/workspace/cell', causeCode: 'scenario_source_invalid' },
      }));
    });
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.unavailableReason).toBe(null);
    await act(async () => { expect(latest!.beginMutation()).toBe(true); });
  });

  it.each([
    ['scenario_artifact_write_failed', 'Scenario project /workspace/cell may be partially written: No space left on device'],
    ['scenario_artifact_target_invalid', 'Scenario project /workspace/cell may be partially written: Scenario project member program.yaml is not a regular file'],
  ])('releases a pre-write %s refusal but keeps a partial Save unconfirmed', async (code, partial) => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    // Before Save creates or writes anything the backend keeps the named 400.
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError('refused', 400, { detail: { code, message: 'refused' } }));
    });
    expect(latest!.submissionUnconfirmed).toBe(false);
    // The same cause after the first write is the one distinct partial outcome.
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError(partial, 500, {
        detail: { code: 'scenario_artifact_save_incomplete', message: partial },
      }));
    });
    expect(latest!.submissionUnconfirmed).toBe(true);
    expect(latest!.unavailableReason).toContain('unconfirmed');
    expect(latest!.beginMutation()).toBe(false);
  });

  it.each([
    ['502 without a body', new ApiError('Bad Gateway', 502, null)],
    ['503 without a body', new ApiError('Service Unavailable', 503, null)],
    ['503 with an unknown code', new ApiError('unavailable', 503, { detail: { code: 'headless_unavailable' } })],
    ['502 with an unstructured detail', new ApiError('bad gateway', 502, { detail: 'upstream failed' })],
  ])('keeps the guard locked for a bare %s', async (_label, failure) => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, failure);
    });
    expect(latest!.submissionUnconfirmed).toBe(true);
    expect(latest!.unavailableReason).toContain('unconfirmed');
    expect(latest!.beginMutation()).toBe(false);
  });

  it('does not release the submission guard for an unclassified conflict', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      expect(latest!.beginMutation()).toBe(true);
      latest!.finishMutation(null, new ApiError('conflict', 409, {
        detail: { code: 'component_fault' },
      }));
    });
    expect(latest!.unavailableReason).toContain('unconfirmed');
    expect(latest!.beginMutation()).toBe(false);
  });

  it('rechecks exact local settlement, not an unrelated revision or idle status', async () => {
    const identity = { requestOwnerEpoch: 'browser.1', requestId: 'edit.1', expectedAdmissionGeneration: 7 };
    apiMock.failedIdentity.mockReturnValue(identity);
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => {
      latest!.beginMutation(); latest!.finishMutation(null, new Error('reply lost'));
    });
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(2, 4, 4, '/workspace/cell'));
    await rerender(headlessSnapshot(2, 4, 4));
    await flush();
    expect(latest!.submissionUnconfirmed).toBe(true);
    apiMock.authoringWorkspace.submissionOutcome.mockResolvedValue(null);
    await act(async () => { await latest!.recheckSubmission(); });
    expect(latest!.submissionUnconfirmed).toBe(true);
    expect(apiMock.authoringWorkspace.submissionOutcome).toHaveBeenCalledWith(identity);
    apiMock.authoringWorkspace.submissionOutcome.mockResolvedValue({ kind: 'settled', apply: null });
    await act(async () => { await latest!.recheckSubmission(); });
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.unavailableReason).toBe(null);
  });

  it('looks up a recovered Manager identity and preserves terminal unknown-effect admission', async () => {
    apiMock.failedIdentity.mockReturnValue({ requestOwnerEpoch: 'browser.1', requestId: 'apply.1', expectedAdmissionGeneration: 7 });
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => { latest!.beginMutation(); latest!.finishMutation(null, new Error('lost')); });
    const terminal = headlessSnapshot(1);
    terminal.runtime.sections.manager.payload = { apply: {
      records: [managerApplyRecord('terminal', 'unknown_effect')], mutation_available: false, unavailable_reason_code: 'unknown_effect',
    } };
    await rerender(terminal);
    apiMock.authoringWorkspace.submissionOutcome.mockResolvedValue({ kind: 'manager_accepted', apply: { applyId: 'apply-3', applyGeneration: 3 } });
    apiMock.manager.applyStatus.mockRejectedValueOnce(new Error('status unavailable'));
    await act(async () => { await latest!.recheckSubmission(); });
    expect(latest!.submissionUnconfirmed).toBe(true);
    apiMock.manager.applyStatus.mockResolvedValue({ applyId: 'apply-3', applyGeneration: 3, state: 'unknown_effect' });
    await act(async () => { await latest!.recheckSubmission(); });
    expect(apiMock.manager.applyStatus).toHaveBeenCalledWith('apply-3', 3);
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.beginMutation()).toBe(false);
    expect(latest!.unavailableReason).toContain('Manager operation');
  });

  it('correlates a lost reply to its Apply by exact request id, never by the latest record', async () => {
    apiMock.failedIdentity.mockReturnValue({ requestOwnerEpoch: 'browser.1', requestId: 'composer-document.1', expectedAdmissionGeneration: 7 });
    apiMock.authoringWorkspace.get.mockResolvedValue(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    await act(async () => { latest!.beginMutation(); latest!.finishMutation(null, new Error('lost')); });
    const unrelated = managerApplyRecord('terminal', 'unknown_effect');
    unrelated.request = { ...unrelated.request, correlation_id: 'composer-document.other' as never };
    const other = headlessSnapshot(1);
    other.runtime.sections.manager.payload = { apply: {
      records: [unrelated], mutation_available: false, unavailable_reason_code: 'unknown_effect',
    } };
    await rerender(other);
    expect(latest!.submissionUnconfirmed).toBe(true);
    expect(latest!.correlatedApply).toBe(null);
    const own = managerApplyRecord('terminal', 'unknown_effect');
    own.apply_id = 'apply-7';
    own.request = { ...own.request, correlation_id: 'composer-document.1' as never };
    const matched = headlessSnapshot(1);
    matched.runtime.sections.manager.payload = { apply: {
      records: [unrelated, own], mutation_available: false, unavailable_reason_code: 'unknown_effect',
    } };
    await rerender(matched);
    expect(latest!.correlatedApply).toEqual({ applyId: 'apply-7', applyGeneration: 3 });
    expect(latest!.submissionUnconfirmed).toBe(false);
    expect(latest!.beginMutation()).toBe(false);
    expect(latest!.unavailableReason).toContain('Manager operation');
  });

  it('reads once after a real reconnect, even through delay, and retains identical objects', async () => {
    apiMock.authoringWorkspace.get.mockImplementation(async () => snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    const installed = latest?.published;
    for (const status of ['disconnected', 'connecting', 'delayed', 'stale', 'connected'] as const) {
      await rerender(headlessSnapshot(1), status);
    }
    await flush();
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);
    expect(latest?.published).toBe(installed);
    await rerender(headlessSnapshot(1));
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);
  });

  it('coalesces reconnect and identity reads while a newer publication is pending', async () => {
    apiMock.authoringWorkspace.get.mockResolvedValueOnce(snapshotFixture(1, 3, 4, '/workspace/cell'));
    const rerender = await mountHarness(headlessSnapshot(1));
    await flush();
    const installed = latest?.published;
    await rerender(headlessSnapshot(1), 'disconnected');
    const next = deferred<AuthoringWorkspaceSnapshot>();
    apiMock.authoringWorkspace.get.mockReturnValueOnce(next.promise);
    await rerender(headlessSnapshot(2), 'connected');
    expect(apiMock.authoringWorkspace.get).toHaveBeenCalledTimes(2);
    expect(latest?.current).toBe(false);
    next.resolve(snapshotFixture(2, 3, 4, '/workspace/cell'));
    await flush();
    expect(latest?.published).not.toBe(installed);
    expect(latest?.current).toBe(true);
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
