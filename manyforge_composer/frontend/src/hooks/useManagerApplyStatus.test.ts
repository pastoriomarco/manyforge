import { act, createElement, useEffect } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError, api } from '../api/client';
import type { ManagerApplyStatus } from '../types/api';
import type { HeadlessSourceSection } from '../types/headless';
import type { ControlTransportStatus } from './useHeadlessState';
import { useManagerApplyStatus } from './useManagerApplyStatus';
import { managerApplyRecord as record } from '../test/managerApplyRecord';

vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(),
  api: { manager: { applyStatus: vi.fn(), acknowledgeApply: vi.fn() } },
}));

const identity = { applyId: 'apply-3', applyGeneration: 3 };
const digest = `sha256:${'a'.repeat(64)}`;
function section(records: unknown[] = [record()]): HeadlessSourceSection {
  return {
    available: true, freshness: 'fresh', knownGaps: [], sourceGeneration: 1,
    sourceOwnerEpoch: 'manager-epoch', sourceOwnerId: 'manager', receiverAgeNs: 0,
    payload: { apply: { records, mutation_available: false, unavailable_reason_code: null } },
  };
}
function status(
  state: ManagerApplyStatus['state'] = 'pending',
  applyIdentity = identity,
): ManagerApplyStatus {
  const phase = state === 'pending' ? 'accepted' : 'settled';
  return {
    ...applyIdentity, apiVersion: 'manyforge.manager_command_response.v1', ok: true,
    state, phase, candidateProjectionRevision: null, error: null, failure: null,
    failureAcknowledgement: 'unavailable',
    lastProgress: { phase, recordDigest: digest },
    sceneInitialization: 'preserve',
    preparedActivation: {
      capturedInputDigest: digest, deploymentArtifactId: 'deployment.fixture',
      deploymentRevision: digest, programBindingId: digest, preparedActivationId: digest,
    },
    requestId: 'request-3', requestOwnerEpoch: 'browser-epoch-3',
  };
}
function failedStatus(
  failureAcknowledgement: ManagerApplyStatus['failureAcknowledgement'],
  applyIdentity = identity,
): ManagerApplyStatus {
  const phase = 'accepted';
  return {
    ...status('failed', applyIdentity),
    phase,
    lastProgress: { phase, recordDigest: digest },
    failure: {
      code: 'manager_apply_preflight_refused',
      message: 'Apply was refused before effect.',
      phase,
      effectDisposition: 'known',
      causeCode: 'manager_apply_preflight_refused',
      causeDetails: {},
      producer: 'component_manager',
      effectBoundary: 'before_effect',
    },
    failureAcknowledgement,
  };
}

let root: import('react-dom/client').Root;
let latest: ReturnType<typeof useManagerApplyStatus>;
let browser: EventTarget;
function Harness({ manager, transport, applyIdentity }: {
  manager: HeadlessSourceSection; transport: ControlTransportStatus;
  applyIdentity: typeof identity;
}) {
  const result = useManagerApplyStatus(applyIdentity, manager, transport);
  useEffect(() => { latest = result; }, [result]);
  return null;
}
async function render(
  manager = section(),
  transport: ControlTransportStatus = 'connected',
  applyIdentity = identity,
) {
  await act(async () => root.render(createElement(Harness, { manager, transport, applyIdentity })));
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
  vi.mocked(api.manager.acknowledgeApply).mockReset();
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

  it('confirms only an acknowledged enum and retains the failed terminal record', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockResolvedValue(failedStatus('acknowledged'));
    await render();

    const originalFailure = latest.status?.failure;
    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });

    expect(result).toEqual({ confirmed: true, message: null, noFailedApply: false });
    expect(latest.status?.state).toBe('failed');
    expect(latest.status?.failure).toEqual(originalFailure);
    expect(latest.status?.failureAcknowledgement).toBe('acknowledged');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    const acknowledgementArgs = vi.mocked(api.manager.acknowledgeApply).mock.calls[0];
    expect(acknowledgementArgs?.[0]).toBe('apply-3');
    expect(acknowledgementArgs?.[1]).toBe(3);
    expect(acknowledgementArgs?.[2]).toMatchObject({
      request_id: expect.stringMatching(/^composer-manager-apply-ack-/),
      source_label: 'manyforge_composer',
    });
    expect(acknowledgementArgs?.[2]).not.toHaveProperty('request_owner_epoch');
    expect(acknowledgementArgs?.[2]).not.toHaveProperty('correlation_id');
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
  });

  it('keeps an unavailable acknowledgement active without treating it as completed', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('unavailable'));
    vi.mocked(api.manager.acknowledgeApply).mockResolvedValue(failedStatus('unavailable'));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });

    expect(result?.confirmed).toBe(false);
    expect(result?.message).toContain('acknowledgement unavailable');
    expect(result?.message).toContain('does not confirm acknowledgement');
    expect(latest.status?.state).toBe('failed');
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);

    vi.mocked(api.manager.applyStatus).mockResolvedValueOnce(failedStatus('acknowledged'));
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(latest.status?.failureAcknowledgement).toBe('acknowledged');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('fails closed when the acknowledgement POST and exact status read both fail', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();

    const originalFailure = latest.status?.failure;
    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });

    expect(result?.confirmed).toBe(false);
    expect(result?.message).toContain('Acknowledgement was not confirmed');
    expect(result?.message).toContain('Follow-up Apply status refresh failed');
    expect(latest.status?.state).toBe('failed');
    expect(latest.status?.failure).toEqual(originalFailure);
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
  });

  it('resolves an ambiguous acknowledgement only from a later exact acknowledged read', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'))
      .mockResolvedValueOnce(failedStatus('acknowledged'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');

    await act(async () => browser.dispatchEvent(new Event('focus')));

    expect(latest.status?.state).toBe('failed');
    expect(latest.status?.failureAcknowledgement).toBe('acknowledged');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('does not re-enable acknowledgement from a later available read that may race the earlier POST', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'))
      .mockResolvedValueOnce(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');

    await act(async () => browser.dispatchEvent(new Event('focus')));

    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('resolves an uncertain acknowledgement when the Manager source changes', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'))
      .mockResolvedValueOnce(failedStatus('acknowledged'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();
    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    await render({ ...section(), sourceOwnerEpoch: 'new-manager-epoch' });
    expect(latest.status?.failureAcknowledgement).toBe('acknowledged');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('does not accept available after an uncertain POST when the connection refreshes', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'))
      .mockResolvedValueOnce(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();
    await act(async () => { await latest.acknowledgeFailure(); });
    await render(section(), 'disconnected');
    await render(section(), 'connected');
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('does not carry an ambiguous acknowledgement hold to a different Apply identity', async () => {
    const nextIdentity = { applyId: 'apply-4', applyGeneration: 4 };
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status reply lost'))
      .mockResolvedValueOnce(failedStatus('unavailable'))
      .mockResolvedValueOnce(failedStatus('available', nextIdentity));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('POST reply lost'));
    await render();

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');

    await render(section([]), 'connected', nextIdentity);

    expect(latest.status?.applyId).toBe(nextIdentity.applyId);
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(4);
  });

  it('keeps an uncertain acknowledgement visible and performs only a status refresh', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('connection lost'));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });

    expect(result?.confirmed).toBe(false);
    expect(result?.message).toContain('Acknowledgement was not confirmed');
    expect(result?.message).toContain('no request was retried');
    expect(latest.status?.state).toBe('failed');
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
  });

  it('shows a proven pre-attempt refusal and re-enables only after an available read', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new ApiError(
      'request refused', 409, { detail: {
        code: 'stale_generation', message: 'The predecessor changed.',
        details: { effect: 'none' }, producer: 'kernel',
      } },
    ));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result?.confirmed).toBe(false);
    expect(result?.message).toContain('Acknowledgement was refused');
    expect(result?.message).toContain('The predecessor changed.');
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
  });

  it('sends the durable acknowledgement again after a proven no-effect refusal while it is still offered', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new ApiError(
      'request refused', 409, { detail: {
        code: 'stale_generation', message: 'The predecessor changed.',
        details: { effect: 'none' }, producer: 'kernel',
      } },
    ));
    await render();

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(latest.acknowledgementOutcomeUnknown).toBe(false);

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toMatchObject({ confirmed: false, noFailedApply: false });
    expect(result?.message).toContain('The predecessor changed.');
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(2);
    // A proven refusal may use a fresh request.
    const ids = vi.mocked(api.manager.acknowledgeApply).mock.calls.map((call) => call[2].request_id);
    expect(ids[0]).not.toBe(ids[1]);
  });

  it('never resends after an unknown outcome: the next click only re-reads, even if it reads available', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('connection lost'));
    await render();

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.acknowledgementOutcomeUnknown).toBe(true);

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toMatchObject({ confirmed: false, noFailedApply: false });
    expect(result?.message).toContain('outcome is still unknown');
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(latest.acknowledgementOutcomeUnknown).toBe(true);
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(3);
  });

  it('settles an unknown outcome only from a re-read that reports acknowledged', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('unavailable'))
      .mockResolvedValueOnce(failedStatus('acknowledged'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new Error('connection lost'));
    await render();
    await act(async () => { await latest.acknowledgeFailure(); });

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toEqual({ confirmed: true, noFailedApply: false, message: null });
    expect(latest.status?.failureAcknowledgement).toBe('acknowledged');
    expect(latest.acknowledgementOutcomeUnknown).toBe(false);
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
  });

  it('keeps the notice on an unavailable re-read: unavailable is not evidence of release', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue(failedStatus('unavailable'));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toMatchObject({ confirmed: false, noFailedApply: false });
    expect(result?.message).toContain('does not prove the Apply holds nothing');
    expect(api.manager.acknowledgeApply).not.toHaveBeenCalled();
  });

  it('reports no failed Apply only when the re-read says the Apply is unknown', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('unavailable'))
      .mockRejectedValueOnce(new ApiError('not found', 404, { detail: {
        code: 'apply_status_not_found', message: 'Manager Apply identity is unknown',
        details: { effect: 'none' }, producer: 'component_manager',
      } }));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toEqual({ confirmed: false, noFailedApply: true, message: null });
    expect(api.manager.acknowledgeApply).not.toHaveBeenCalled();
  });

  it('keeps the notice and sends nothing when the re-read itself fails', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('unavailable'))
      .mockRejectedValueOnce(new Error('status unavailable'));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toMatchObject({ confirmed: false, noFailedApply: false });
    expect(result?.message).toContain('could not be re-read');
    expect(api.manager.acknowledgeApply).not.toHaveBeenCalled();
  });

  it('after a proven refusal whose refresh failed, a re-read offering it again lets the next click send', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status unavailable'))
      .mockResolvedValueOnce(failedStatus('available'))
      .mockResolvedValueOnce(failedStatus('acknowledged'));
    vi.mocked(api.manager.acknowledgeApply)
      .mockRejectedValueOnce(new ApiError(
      'request refused', 409, { detail: {
        code: 'stale_generation', message: 'The predecessor changed.',
        details: { effect: 'none' }, producer: 'kernel',
      } },
    ))
      .mockResolvedValueOnce(failedStatus('acknowledged'));
    await render();
    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(latest.acknowledgementOutcomeUnknown).toBe(false);

    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });
    expect(result).toMatchObject({ confirmed: true, noFailedApply: false });
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(2);
  });

  it('waits for a fresh available read if a proven refusal cannot refresh immediately', async () => {
    vi.mocked(api.manager.applyStatus)
      .mockResolvedValueOnce(failedStatus('available'))
      .mockRejectedValueOnce(new Error('status unavailable'))
      .mockResolvedValueOnce(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new ApiError(
      'request refused', 409, { detail: {
        code: 'stale_generation', message: 'The predecessor changed.',
        details: { effect: 'none' }, producer: 'kernel',
      } },
    ));
    await render();
    await act(async () => { await latest.acknowledgeFailure(); });
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    await act(async () => browser.dispatchEvent(new Event('focus')));
    expect(latest.status?.failureAcknowledgement).toBe('available');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
  });

  it('shows the typed uncertain-outcome catalog text, message and details without retrying', async () => {
    vi.mocked(api.manager.applyStatus).mockResolvedValue(failedStatus('available'));
    vi.mocked(api.manager.acknowledgeApply).mockRejectedValue(new ApiError(
      'request failed',
      503,
      { detail: {
        code: 'apply_acknowledgement_outcome_unknown',
        message: 'The acknowledgement reply was lost.',
        details: { reason: 'acknowledgement may have acted' },
        producer: 'component_manager',
      } },
    ));
    await render();

    let result: Awaited<ReturnType<typeof latest.acknowledgeFailure>> | undefined;
    await act(async () => { result = await latest.acknowledgeFailure(); });

    expect(result?.confirmed).toBe(false);
    expect(result?.message).toContain('Apply acknowledgement outcome unknown');
    expect(result?.message).toContain('Check the cell and Apply status before acting again.');
    expect(result?.message).toContain('The acknowledgement reply was lost.');
    expect(result?.message).toContain('Reason: acknowledgement may have acted');
    expect(result?.message).toContain('no request was retried');
    expect(latest.status?.failureAcknowledgement).toBe('unavailable');
    expect(api.manager.acknowledgeApply).toHaveBeenCalledTimes(1);
    expect(api.manager.applyStatus).toHaveBeenCalledTimes(2);
  });
});
