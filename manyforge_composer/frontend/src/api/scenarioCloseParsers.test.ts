import { afterEach, expect, it, vi } from 'vitest';
import { api } from './client';
import { parseSceneClearStatus, parseCanonicalEmptySelection, parseForceCloseResult, parseLifecycleOwnerSettlement, parseLifecycleTargetRetirementOffer,
  parseLifecycleTargetRetirementResult } from './scenarioCloseParsers';
import type { LifecycleTargetRetirementIntent } from '../types/scenarioClose';

const intent: LifecycleTargetRetirementIntent = {
  request_context: { request_id: 'retire.1', request_owner_epoch: 'owner.1', source_label: 'composer', correlation_id: 'close.1' },
  scope: { request_owner_epoch: 'owner.1', publication_generation: 2, active_public_pointer_digest: `sha256:${'a'.repeat(64)}` },
  expected_targets: [{ controllable_component_id: 'arm', requested_state: 'active', origin: 'operator',
    target_generation: 3, reached: false, recovered_cause: 'fault' }],
};
const reply = () => ({ request_context: structuredClone(intent.request_context), replay_status: 'accepted_new', scope: structuredClone(intent.scope), retired_targets: structuredClone(intent.expected_targets) });
afterEach(() => vi.unstubAllGlobals());

it('keeps complete target facts and allows genuine empty offers', () => {
  expect(parseLifecycleTargetRetirementResult(reply(), intent).retired_targets[0].recovered_cause).toBe('fault');
  expect(parseLifecycleTargetRetirementOffer({ scope: intent.scope, targets: [] }).targets).toEqual([]);
});
it.each(['request_owner_epoch', 'publication_generation', 'active_public_pointer_digest'] as const)(
  'refuses foreign scope %s at retirement and settlement', (field) => {
    const value = reply(); Object.assign(value.scope, { [field]: field === 'publication_generation' ? 5 : `sha256:${'b'.repeat(64)}` });
    expect(() => parseLifecycleTargetRetirementResult(value, intent)).toThrow();
    expect(() => parseLifecycleOwnerSettlement({ scope: value.scope, settled: true }, intent.scope)).toThrow();
  },
);
it.each(['target_generation', 'reached', 'recovered_cause'] as const)('refuses changed complete target fact %s', (field) => {
  const value = reply(); Object.assign(value.retired_targets[0], { [field]: field === 'target_generation' ? 4 : field === 'reached' ? true : null });
  expect(() => parseLifecycleTargetRetirementResult(value, intent)).toThrow();
});
it('rejects missing facts, duplicate, unordered or overcapacity target vectors', () => {
  const value = { ...intent.expected_targets[0] } as Record<string, unknown>; delete value.reached;
  for (const targets of [[value], [...intent.expected_targets, ...intent.expected_targets],
    [...intent.expected_targets, { ...intent.expected_targets[0], controllable_component_id: 'a' }],
    Array.from({ length: 65 }, (_, index) => ({ ...intent.expected_targets[0], controllable_component_id: String(index).padStart(2, '0') }))]) {
    expect(() => parseLifecycleTargetRetirementOffer({ scope: intent.scope, targets })).toThrow();
  }
});
it('checks canonical UTF-8 ordering for non-ASCII component identities', () => {
  const targets = ['\uE000', '\u{10000}'].map((id) => ({ ...intent.expected_targets[0], controllable_component_id: id }));
  expect(parseLifecycleTargetRetirementOffer({ scope: intent.scope, targets }).targets).toEqual(targets);
  expect(() => parseLifecycleTargetRetirementOffer({ scope: intent.scope, targets: targets.toReversed() })).toThrow();
});
it('requires literal true settlement and never treats pending as settled', () => {
  expect(parseLifecycleOwnerSettlement({ scope: intent.scope, settled: true }, intent.scope).settled).toBe(true);
  for (const settled of [false, 1, 'true', null]) expect(() => parseLifecycleOwnerSettlement({ scope: intent.scope, settled }, intent.scope)).toThrow();
  expect(() => parseLifecycleTargetRetirementResult({ ...reply(), replay_status: 'request_result_expired' }, intent)).toThrow();
});
it('reads retirement result separately, preserving unknown null without resubmission', async () => {
  const calls: Array<[string, unknown]> = [];
  vi.stubGlobal('fetch', vi.fn(async (path: string, init?: RequestInit) => {
    const body = init?.body ? JSON.parse(init.body as string) : null; calls.push([path, body]);
    const result = path.endsWith('retirement-offer') ? { scope: intent.scope, targets: intent.expected_targets }
      : path.endsWith('/result') ? { result: null } : path.endsWith('settlement') ? { scope: intent.scope, settled: true } : reply();
    return new Response(JSON.stringify(result), { status: 200 });
  }));
  await api.cclc.targetRetirementOffer(); await api.cclc.retireTargets(intent);
  expect(await api.cclc.targetRetirementResult(intent)).toBeNull(); await api.cclc.ownerSettlement(intent.scope);
  expect(calls).toEqual([['/api/cclc/targets/retirement-offer', null], ['/api/cclc/targets/retire', intent],
    ['/api/cclc/targets/retire/result', intent], ['/api/cclc/targets/settlement', { scope: intent.scope }]]);
});
it('captures the complete retirement scope and vector before awaiting its acknowledgement', async () => {
  let finish!: (response: Response) => void;
  vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>((resolve) => { finish = resolve; })));
  const input = structuredClone(intent); const pending = api.cclc.retireTargets(input);
  input.expected_targets[0].target_generation = 99; input.scope.publication_generation = 99;
  finish(new Response(JSON.stringify(reply()), { status: 200 }));
  expect((await pending).retired_targets[0].target_generation).toBe(3);
});
it('accepts only passive seven-field selection, never prepared activation or completed Close', async () => {
  const selection = { request_owner_epoch: 'manager.1', cell_id: 'x'.repeat(128), deployment_artifact_id: 'deployment.empty',
    deployment_revision: `sha256:${'1'.repeat(64)}`, candidate_retained_bundle_id: `sha256:${'2'.repeat(64)}`,
    candidate_graph_revision: `sha256:${'3'.repeat(64)}`, admission_digest: `sha256:${'4'.repeat(64)}` };
  expect(parseCanonicalEmptySelection(selection)).toEqual(selection);
  expect(() => parseCanonicalEmptySelection({ ...selection, prepared_activation_id: 'fake' })).toThrow();
  expect(() => parseCanonicalEmptySelection({ ...selection, candidate_graph_revision: 'graph.foreign' })).toThrow();
  for (const cell_id of ['', 'x'.repeat(129), 'e\u0301', 'é'.repeat(65)]) {
    expect(() => parseCanonicalEmptySelection({ ...selection, cell_id })).toThrow();
  }
  const missing = { ...selection } as Record<string, unknown>; delete missing.cell_id;
  expect(() => parseCanonicalEmptySelection(missing)).toThrow();
  expect(() => parseCanonicalEmptySelection({ ...selection, cell_id: 'x'.repeat(16 * 1024) })).toThrow('16 KiB');
  vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify(selection), { status: 200 })));
  expect(await api.manager.canonicalEmptySelection()).toEqual(selection);
  expect(vi.mocked(fetch)).toHaveBeenCalledOnce();
  expect(vi.mocked(fetch).mock.calls[0][0]).toBe('/api/manager/canonical-empty-selection');
});


it.each(['request_id', 'request_owner_epoch', 'source_label', 'correlation_id'] as const)(
  'rejects foreign retirement request attribution %s', (field) => {
    const value = reply(); Object.assign(value.request_context, { [field]: 'foreign' });
    expect(() => parseLifecycleTargetRetirementResult(value, intent)).toThrow();
  },
);
it.each([false, true])('normalizes optional null correlation for retirement caller %s', (callerNull) => {
  const input = structuredClone(intent); delete input.request_context.correlation_id;
  const canonicalContext = structuredClone(input.request_context);
  if (callerNull) input.request_context.correlation_id = null;
  for (const echoNull of [false, true]) {
    const context = { ...canonicalContext, ...(echoNull ? { correlation_id: null } : {}) };
    const value = { ...reply(), request_context: context };
    expect(parseLifecycleTargetRetirementResult(value, input).request_context).toEqual(canonicalContext);
    expect(() => parseLifecycleTargetRetirementResult({ ...value,
      request_context: { ...context, correlation_id: 'foreign' } }, input)).toThrow();
  }
});
it.each([undefined, null])('omits absent/null correlation before retirement and result transport %s', async (correlation) => {
  const input = structuredClone(intent); input.request_context.correlation_id = correlation;
  const context = structuredClone(input.request_context); delete context.correlation_id;
  const calls: unknown[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_path: string, init?: RequestInit) => {
    const body = JSON.parse(init?.body as string); calls.push(body);
    const value = { ...reply(), request_context: context };
    return new Response(JSON.stringify(_path.endsWith('/result') ? { result: value } : value), { status: 200 });
  }));
  await api.cclc.retireTargets(input); await api.cclc.targetRetirementResult(input);
  expect(calls).toEqual([{ ...input, request_context: context }, { ...input, request_context: context }]);
});
it.each(['requested_state', 'origin', 'recovered_cause'])( 'rejects unknown retirement enum %s', (field) => {
  const targets = [{ ...intent.expected_targets[0], [field]: 'invented' }];
  expect(() => parseLifecycleTargetRetirementOffer({ scope: intent.scope, targets })).toThrow();
});
it.each(['retire', 'result', 'settlement', 'selection'])('preserves actual typed HTTP refusal for %s', async (surface) => {
  const code = surface === 'settlement' ? 'component_lifecycle_owner_settlement_unknown'
    : surface === 'selection' ? 'full_stack_restart_required' : 'request_id_conflict';
  const status = surface === 'settlement' ? 503 : 409;
  vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({ detail: { code, message: 'actual owner refusal' } }), { status })));
  const operation = surface === 'retire' ? api.cclc.retireTargets(intent)
    : surface === 'result' ? api.cclc.targetRetirementResult(intent)
    : surface === 'settlement' ? api.cclc.ownerSettlement(intent.scope) : api.manager.canonicalEmptySelection();
  await expect(operation).rejects.toMatchObject({ status, structuredError: { code, message: 'actual owner refusal' } });
});

// Item 37: a Force close reply must name its own step.
it('parses a Force close step reply and refuses another step or shape', () => {
  const reply = { api_version: 'manyforge.execution_command_response.v1', ok: true, command: 'force_close',
    history: { event_id: 'event-1', recorded: true }, result: { step: 'close_operations', stop_generation: 5,
      released_orphan_resources: [], operations: [{ operation_id: 'op.io', operation_revision: 2, state: 'failed',
        containment_uncertain: false, targets: [{ controllable_component_id: 'io', command_target_id: 'valve', output_unknown: true }] }] } };
  expect(parseForceCloseResult(reply, 'close_operations')).toEqual({ step: 'close_operations', stopGeneration: 5,
    motionBlockCleared: false, operations: [{ operationId: 'op.io', operationRevision: 2, state: 'failed', containmentUncertain: false,
      targets: [{ controllableComponentId: 'io', commandTargetId: 'valve', outputUnknown: true }] }] });
  expect(parseForceCloseResult({ ...reply, result: { ...reply.result, motion_block_cleared: true } }, 'close_operations')
    .motionBlockCleared).toBe(true);
  expect(() => parseForceCloseResult(reply, 'stop')).toThrow('does not match its step');
  // Night change 8: the server answers `stop` with the Kernel's `arm` reply.
  const armed = { ...reply, result: { step: 'arm', stop_generation: 5, released_orphan_resources: [], operations: [] } };
  expect(parseForceCloseResult(armed, 'stop')).toEqual({ step: 'stop', stopGeneration: 5, motionBlockCleared: false, operations: [] });
  expect(() => parseForceCloseResult(armed, 'close_operations')).toThrow('does not match its step');
  expect(() => parseForceCloseResult({ ...reply, ok: false }, 'close_operations')).toThrow();
  expect(() => parseForceCloseResult({ ...reply, result: { ...reply.result, operations: [{ operation_id: 'op.io' }] } },
    'close_operations')).toThrow('operation row is invalid');
});

it('parses transient Scene clear status and keeps unknown remaining distinct', () => {
  const row = { apply_id: 'apply.1', apply_generation: 3, state: 'unknown', remaining: null,
    reason: { code: 'scene_clear_unknown', message: 'Receipt unavailable', details: { reason: 'response_unavailable' } } };
  expect(parseSceneClearStatus({ scene_clear: row })).toEqual({ applyId: 'apply.1', applyGeneration: 3,
    state: 'unknown', remaining: null, reason: row.reason });
  expect(parseSceneClearStatus({ scene_clear: null })).toBeNull();
  expect(() => parseSceneClearStatus({ scene_clear: { ...row, remaining: [null] } })).toThrow();
});

it('reads the follower status carried by a typed cycle-status outage', async () => {
  const scene_clear = { apply_id: 'apply.1', apply_generation: 3, state: 'pending', remaining: null, reason: null };
  vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({ detail: {
    code: 'program_runner_unavailable', message: 'Runner unavailable', producer: 'headless', details: { scene_clear },
  } }), { status: 503 })));
  expect(await api.programs.sceneClearStatus()).toEqual({ applyId: 'apply.1', applyGeneration: 3,
    state: 'pending', remaining: null, reason: null });
});

it.each([
  { remaining: null, reason: null },
  { remaining: ['held'], reason: null },
  { remaining: [], reason: { code: 'fault', message: 'Fault', details: {} } },
])('rejects malformed Scene clear success: %j', (fields) => {
  expect(() => parseSceneClearStatus({ scene_clear: { apply_id: 'apply.1', apply_generation: 3,
    state: 'succeeded', ...fields } })).toThrow('success must have no remaining objects or failure');
});

it('accepts a Scene clear success only with an empty remainder and no reason', () => {
  expect(parseSceneClearStatus({ scene_clear: { apply_id: 'apply.1', apply_generation: 3,
    state: 'succeeded', remaining: [], reason: null } })).toMatchObject({ state: 'succeeded', remaining: [], reason: null });
});

it('requires a partial result to name remaining parts and typed refusals', () => {
  const partial = { apply_id: 'apply.1', apply_generation: 3, state: 'partial', remaining: ['held'],
    reason: { code: 'scene_clear_partial', message: 'Detach refused',
      details: { refused: [{ resource_id: 'held', code: 'detach_refused', message: 'No carrier pose' }] } } };
  expect(parseSceneClearStatus({ scene_clear: partial })).toMatchObject({ state: 'partial', remaining: ['held'] });
  expect(() => parseSceneClearStatus({ scene_clear: { ...partial, remaining: [] } })).toThrow();
  expect(() => parseSceneClearStatus({ scene_clear: { ...partial, reason: { ...partial.reason, details: { refused: [] } } } })).toThrow();
});

it('parses request-keyed direct Scene-clear status without inventing an Apply identity', () => {
  expect(parseSceneClearStatus({ scene_clear: { request_id: 'clear.1', state: 'succeeded', remaining: [], reason: null } }))
    .toEqual({ requestId: 'clear.1', state: 'succeeded', remaining: [], reason: null });
});

it.each(['failed', 'unknown'] as const)('requires the matching typed reason for %s', (state) => {
  const row = { apply_id: 'apply.1', apply_generation: 3, state, remaining: null };
  expect(() => parseSceneClearStatus({ scene_clear: { ...row, reason: null } })).toThrow('matching typed reason');
  expect(() => parseSceneClearStatus({ scene_clear: { ...row,
    reason: { code: state === 'failed' ? 'scene_clear_unknown' : 'scene_clear_failed', message: 'wrong', details: {} } } }))
    .toThrow('matching typed reason');
  const reason = { code: `scene_clear_${state}`, message: 'owner detail', details: { reason: 'owner_unavailable' } };
  expect(parseSceneClearStatus({ scene_clear: { ...row, reason } })).toMatchObject({ state, reason });
});

it.each(['pending', 'superseded'] as const)('rejects a failure reason for %s', (state) => {
  expect(() => parseSceneClearStatus({ scene_clear: { request_id: 'clear.1', state, remaining: null,
    reason: { code: 'scene_clear_failed', message: 'wrong', details: {} } } })).toThrow();
});

it.each(['pending', 'superseded'] as const)('accepts a %s record with no failure reason', (state) => {
  expect(parseSceneClearStatus({ scene_clear: { request_id: 'clear.1', state, remaining: null, reason: null } }))
    .toEqual({ requestId: 'clear.1', state, remaining: null, reason: null });
});
