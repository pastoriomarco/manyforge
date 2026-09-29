import { act, createElement, useEffect } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import type { ManagerApplyStatus } from '../types/api';
import type { HeadlessSourceSection } from '../types/headless';
import type { ControlTransportStatus } from './useHeadlessState';
import { useManagerApplyStatus } from './useManagerApplyStatus';

vi.mock('../api/client', () => ({ api: { manager: { applyStatus: vi.fn() } } }));

const identity = { applyId: 'apply-3', applyGeneration: 3 };
const digest = `sha256:${'a'.repeat(64)}`;
function record(phase = 'fence_held', terminal: string | null = null) {
  return {
    apply_generation: 3, apply_id: 'apply-3', candidate_graph_revision: 'graph-2',
    candidate_retained_bundle_id: 'bundle-2', committed: terminal === 'succeeded',
    expected_active_projection_revision: {
      retained_bundle_id: 'bundle-1', graph_revision: 'graph-1',
      scene_revision: { scene_id: 'scene-1', revision_id: 1, content_hash: digest },
    },
    fence: {
      acceptance_digest: digest, configuration_fence_generation: 2,
      configuration_fence_id: 'fence-2', fence_attempt_digest: digest,
      scope: { configurations: ['configuration-1'], resources: ['resource-1'] },
      state: terminal === null ? 'held' : 'released',
    },
    incomplete: terminal === null, intent_digest: digest, phase,
    prepared_activation: {
      schema_version: 'manyforge.prepared_activation_input.v3',
      prepared_activation_id: digest, scenario_id: 'fixture',
      source_document_digest: digest, captured_input_digest: digest,
      deployment_artifact_id: 'deployment.fixture', deployment_revision: digest,
      retained_bundle_id: digest, graph_revision: digest,
      program_id: 'fixture-program', program_revision: digest,
      program_outcome: { kind: 'bound', program_binding_id: digest },
      node_environment_digest: digest, assistant_mode: 'forbidden', typed_parameter_overrides: {},
    },
    request: { correlation_id: null, request_id: 'request-3',
      request_owner_epoch: 'browser-epoch-3', source_label: 'composer_ui' },
    terminal_state: terminal,
  };
}
function section(records: unknown[] = [record()]): HeadlessSourceSection {
  return {
    available: true, freshness: 'fresh', knownGaps: [], sourceGeneration: 1,
    sourceOwnerEpoch: 'manager-epoch', sourceOwnerId: 'manager', receiverAgeNs: 0,
    payload: { apply: { records, mutation_available: false, unavailable_reason_code: null } },
  };
}
function status(state: ManagerApplyStatus['state'] = 'pending'): ManagerApplyStatus {
  const phase = state === 'pending' ? 'accepted' : 'settled';
  return {
    ...identity, apiVersion: 'manyforge.manager_command_response.v1', ok: true,
    state, phase, candidateProjectionRevision: null, error: null, failure: null,
    lastProgress: { phase, recordDigest: digest },
    preparedActivation: {
      capturedInputDigest: digest, deploymentArtifactId: 'deployment.fixture',
      deploymentRevision: digest, programBindingId: digest, preparedActivationId: digest,
    },
    requestId: 'request-3', requestOwnerEpoch: 'browser-epoch-3',
  };
}

let root: import('react-dom/client').Root;
let latest: ReturnType<typeof useManagerApplyStatus>;
let browser: EventTarget;
function Harness({ manager, transport }: {
  manager: HeadlessSourceSection; transport: ControlTransportStatus;
}) {
  const result = useManagerApplyStatus(identity, manager, transport);
  useEffect(() => { latest = result; }, [result]);
  return null;
}
async function render(manager = section(), transport: ControlTransportStatus = 'connected') {
  await act(async () => root.render(createElement(Harness, { manager, transport })));
}

beforeEach(async () => {
  // The hook renders no DOM. Supply only React's root/event plumbing, as in
  // the other owner-observation hook tests; no browser or network timing.
  const document = Object.assign(new EventTarget(), {
    nodeType: 9, activeElement: null, defaultView: {},
  });
  const makeNode = (tag: string) => Object.assign(new EventTarget(), {
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
    appendChild: vi.fn(), removeChild: vi.fn(), insertBefore: vi.fn(),
    setAttribute: vi.fn(), removeAttribute: vi.fn(),
  });
  Object.assign(document, { createElement: makeNode, documentElement: makeNode('html') });
  browser = Object.assign(new EventTarget(), { document, HTMLIFrameElement: class {} });
  document.defaultView = browser;
  vi.stubGlobal('window', browser);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const { createRoot } = await import('react-dom/client');
  root = createRoot(makeNode('div') as unknown as Element);
  vi.mocked(api.manager.applyStatus).mockReset().mockResolvedValue(status());
});
afterEach(async () => {
  await act(async () => root.unmount());
  vi.unstubAllGlobals();
});

describe('Manager Apply owner observation', () => {
  it('confirms completion from a stream record without a workspace sequence change', async () => {
    await render();
    expect(latest.unavailable).toBe(false);
    expect(latest.status?.state).toBe('pending');
    vi.mocked(api.manager.applyStatus).mockResolvedValue(status('succeeded'));
    await render(section([record('terminal', 'succeeded')]));
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
    expect(latest.status?.state).toBe('succeeded');
    await render(section()); // A delayed pending advertisement never regresses success.
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
    expect(latest.status?.state).toBe('succeeded');
  });

  it('ignores heartbeat counters, unrelated records and same-socket delay recovery', async () => {
    await render();
    const next = section([record(), { ...record(), apply_id: 'unrelated' }]);
    next.sourceGeneration = 100;
    next.receiverAgeNs = 99;
    next.payload = { ...next.payload, runtime: { observed_units: [{ process_observation_sequence: 100 }] } };
    await render(next, 'delayed');
    await render(next, 'stale');
    await render(next, 'connected');
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(1);
  });

  it('reconciles pending work on focus, real reconnect and Manager owner changes', async () => {
    await render();
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
    await render(section(), 'disconnected');
    await render(section(), 'connecting');
    await render(section(), 'delayed');
    await render(section(), 'connected');
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
    await render({ ...section(), sourceOwnerEpoch: 'new-owner' });
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(4);
    await render({ ...section(), available: false, freshness: 'unavailable' });
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(5);
  });

  it('uses HTTP when a record disappears, never interpreting absence as completion', async () => {
    await render();
    await render(section([]));
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
    expect(latest.status?.state).toBe('pending');
  });

  it('reports malformed and duplicate inventory instead of reading fields permissively', async () => {
    await render(section([{ ...record(), unexpected: true }]));
    expect(latest.unavailable).toBe(true);
    expect(latest.reason).toContain('notification is invalid');
    await render(section([record(), record()]));
    expect(latest.reason).toContain('Duplicate');
    await render();
    expect(latest.unavailable).toBe(false);
  });

  it('rejects mismatched HTTP identities and leaves lost reads explicitly unavailable', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue({ ...status(), applyId: 'wrong' });
    await render();
    expect(latest.status).toBe(null);
    expect(latest.unavailable).toBe(true);
    vi.mocked(api.manager.applyStatus).mockRejectedValue(new Error('lost read'));
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(latest.unavailable).toBe(true);
    vi.mocked(api.manager.applyStatus).mockResolvedValue(status('succeeded'));
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(latest.status?.state).toBe('succeeded');
    expect(latest.unavailable).toBe(false);
  });

  it('discards an older in-flight response after a newer notification', async () => {
    let resolveOld!: (value: ManagerApplyStatus) => void;
    vi.mocked(api.manager.applyStatus).mockReturnValueOnce(new Promise((resolve) => { resolveOld = resolve; }));
    await render();
    vi.mocked(api.manager.applyStatus).mockResolvedValue(status('succeeded'));
    await render(section([record('terminal', 'succeeded')]));
    await act(async () => resolveOld(status()));
    expect(latest.status?.state).toBe('succeeded');
    expect(vi.mocked(api.manager.applyStatus).mock.calls[0][2]?.aborted).toBe(true);
  });
});
