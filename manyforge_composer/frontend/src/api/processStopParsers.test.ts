import { afterEach, expect, it, vi } from 'vitest';
import type { ComponentProcessStopRequest } from '../types/cclc';
import { parseComponentProcessStopResult, parseComponentProcessStopOffer } from './processStopParsers';
import { api } from './client';

const request: ComponentProcessStopRequest = {
  request_context: { request_id: 'stop.1', request_owner_epoch: 'owner.1', source_label: 'composer', correlation_id: 'close.1' },
  predecessor_projection_revision: {
    retained_bundle_id: `sha256:${'a'.repeat(64)}`, graph_revision: `sha256:${'b'.repeat(64)}`,
    scene_revision: { scene_id: `sha256:${'c'.repeat(64)}`, revision_id: 2, content_hash: `sha256:${'d'.repeat(64)}` },
  },
  expected_predecessor_publication_generation: 4, expected_predecessor_public_pointer_digest: `sha256:${'e'.repeat(64)}`,
  runtime_unit_id: 'host.1', runtime_epoch: 'host.epoch.1', manager_runtime_epoch: 'manager.epoch.1',
  controllable_component_id: 'arm', launch_identity: 'launch.1', leader_pid: 42,
};
const reply = () => ({ request: structuredClone(request), state: 'drained', error: null, receipt: {
  leader_pid: request.leader_pid, controllable_component_id: request.controllable_component_id,
  launch_identity: request.launch_identity, observed_utc: '2026-10-13T10:00:00Z',
} });
afterEach(() => vi.unstubAllGlobals());

it('retains pending and unknown as incomplete and requires actual drain evidence', () => {
  for (const state of ['not_requested', 'pending', 'unknown']) {
    const value = { ...reply(), state, receipt: null };
    expect(parseComponentProcessStopResult(value, request)).toMatchObject({ state, receipt: null });
  }
  expect(parseComponentProcessStopResult(reply(), request).receipt?.leader_pid).toBe(42);
});
it.each(['runtime_epoch', 'manager_runtime_epoch', 'launch_identity', 'leader_pid',
  'expected_predecessor_publication_generation', 'expected_predecessor_public_pointer_digest'] as const)(
  'rejects a foreign echoed %s', (field) => {
    const value = reply();
    Object.assign(value.request, { [field]: typeof value.request[field] === 'number' ? 99 : 'foreign' });
    expect(() => parseComponentProcessStopResult(value, request)).toThrow('exact request');
  },
);
it('checks nested Scene and request attribution without depending on key order', () => {
  const value = reply();
  value.request = Object.fromEntries(Object.entries(value.request).reverse()) as unknown as ComponentProcessStopRequest;
  expect(parseComponentProcessStopResult(value, request).state).toBe('drained');
  value.request.predecessor_projection_revision.scene_revision.revision_id += 1;
  expect(() => parseComponentProcessStopResult(value, request)).toThrow('exact request');
  const other = reply(); other.request.request_context.correlation_id = 'foreign';
  expect(() => parseComponentProcessStopResult(other, request)).toThrow('exact request');
});
it.each(['missing_receipt', 'foreign_pid', 'foreign_launch', 'pending_receipt', 'missing_error'])(
  'refuses invalid drain proof %s', (mutation) => {
    const value = reply() as Record<string, unknown>;
    if (mutation === 'missing_receipt') value.receipt = null;
    if (mutation === 'foreign_pid') (value.receipt as Record<string, unknown>).leader_pid = 99;
    if (mutation === 'foreign_launch') (value.receipt as Record<string, unknown>).launch_identity = 'foreign';
    if (mutation === 'pending_receipt') value.state = 'pending';
    if (mutation === 'missing_error') delete value.error;
    expect(() => parseComponentProcessStopResult(value, request)).toThrow();
  },
);
it('forwards the same exact request to Stop and its read-only result route once each', async () => {
  const calls: Array<[string, unknown]> = [];
  vi.stubGlobal('fetch', vi.fn(async (path: string, init: RequestInit) => {
    calls.push([path, JSON.parse(init.body as string)]);
    return new Response(JSON.stringify(reply()), { status: 200 });
  }));
  await api.cclc.stopComponentProcess(request);
  await api.cclc.componentProcessStopResult(request);
  expect(calls).toEqual([
    ['/api/cclc/components/arm/process/stop', { request }],
    ['/api/cclc/components/arm/process/stop/result', { request }],
  ]);
});


it('preserves actual unknown ErrorDTO without imposing readiness field caps', () => {
  const error = { code: 'cleanup_unknown', message: 'x'.repeat(1800),
    details: Object.fromEntries(Array.from({ length: 70 }, (_, i) => [String(i), 'y'.repeat(300)])) };
  const value = { ...reply(), state: 'unknown', receipt: null, error };
  expect(parseComponentProcessStopResult(value, request).error).toEqual(error);
  expect(parseComponentProcessStopResult(value, request).receipt).toBeNull();
});


it('keeps the sent Stop occurrence immutable while its response is pending', async () => {
  let finish!: (response: Response) => void;
  vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>((resolve) => { finish = resolve; })));
  const input = structuredClone(request);
  const pending = api.cclc.stopComponentProcess(input);
  input.leader_pid = 99;
  finish(new Response(JSON.stringify(reply()), { status: 200 }));
  expect((await pending).request.leader_pid).toBe(42);
});


it.each(['unknown_state', 'drained_error', 'extra_result', 'extra_receipt', 'extra_error'])(
  'rejects malformed or contradictory Stop result %s', (mutation) => {
    const value = reply() as Record<string, unknown>;
    const error = { code: 'cleanup_unknown', message: 'actual uncertainty', details: {} };
    if (mutation === 'unknown_state') value.state = 'stopped';
    if (mutation === 'drained_error') value.error = error;
    if (mutation === 'extra_result') value.confirmed = true;
    if (mutation === 'extra_receipt') (value.receipt as Record<string, unknown>).extra = true;
    if (mutation === 'extra_error') { value.state = 'unknown'; value.receipt = null; value.error = { ...error, effect: 'none' }; }
    expect(() => parseComponentProcessStopResult(value, request)).toThrow();
  },
);
it.each(['stopComponentProcess', 'componentProcessStopResult'] as const)('preserves Stop HTTP refusal %s', async (method) => {
  vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({ detail: {
    code: 'stale_generation', message: 'publication changed', details: { expected: '4' },
  } }), { status: 409 })));
  await expect(api.cclc[method](request)).rejects.toMatchObject({ status: 409,
    structuredError: { code: 'stale_generation', message: 'publication changed' } });
});

it.each([undefined, null])('normalizes omitted/null Stop correlation across submit and result %s', async (correlation) => {
  const input = structuredClone(request); input.request_context.correlation_id = correlation;
  const canonical = structuredClone(input); delete canonical.request_context.correlation_id;
  const calls: unknown[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_path: string, init?: RequestInit) => {
    calls.push(JSON.parse(init?.body as string));
    return new Response(JSON.stringify({ ...reply(), request: canonical }), { status: 200 });
  }));
  for (const read of [api.cclc.stopComponentProcess, api.cclc.componentProcessStopResult]) {
    expect((await read(input)).request).toEqual(canonical);
  }
  expect(calls).toEqual([{ request: canonical }, { request: canonical }]);
  expect(parseComponentProcessStopResult({ ...reply(), request: input }, canonical).request).toEqual(canonical);
  expect(() => parseComponentProcessStopResult({ ...reply(), request: { ...canonical,
    request_context: { ...canonical.request_context, correlation_id: 'foreign' } } }, input)).toThrow();
});


it('retains all actual Stop-offer occurrences without treating unknown as absence', () => {
  const { request_context, controllable_component_id, launch_identity, leader_pid, ...scope } = request;
  const value = { controllable_component_id, process_inventories: [{ request: scope, error: null,
    process_occurrences: [{ controllable_component_id, launch_identity, leader_pid,
      process_presence: 'unknown', cleanup_state: 'unknown', receipt: null, installed_runtime_identity: null }] }] };
  expect(parseComponentProcessStopOffer(value, controllable_component_id)).toEqual(value);
  expect(() => parseComponentProcessStopOffer(value, 'foreign')).toThrow();
  value.process_inventories[0].process_occurrences[0].cleanup_state = 'drained';
  expect(() => parseComponentProcessStopOffer(value, controllable_component_id)).toThrow();
});
