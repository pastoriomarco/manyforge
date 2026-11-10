import { afterEach, expect, it, vi } from 'vitest';
import { api } from './client';
import { parseManualControlAvailability } from './parsers';
import { buildJointMotionIntent } from '../state/robotControl';
import { statusFixture } from '../testFixtures/robotControl';

const context = {
  program_runner_owner_epoch: 'runner.1', program_run_id: 'program.1',
  claim_generation: 1, selection_revision: 0,
};
const reply = {
  api_version: 'manyforge.execution_command_response.v4', command: 'read_manual_handover',
  ok: true, request_id: 'query.1', request_owner_epoch: 'execution-owner-1',
  intent_digest: `sha256:${'a'.repeat(64)}`,
  availability: { available: true, cleanup_required: true, unavailable_reason: null },
  manual_handover: context,
};

afterEach(() => vi.unstubAllGlobals());

it('keeps opaque held context, including revision zero, separate from availability', () => {
  expect(parseManualControlAvailability(reply)).toMatchObject({
    available: true, cleanupRequired: true, manualHandover: context,
  });
  expect(parseManualControlAvailability({ ...reply, manual_handover: null,
    availability: { available: false, cleanup_required: false, unavailable_reason: 'target_busy' },
  })).toMatchObject({ available: false, unavailableReason: 'target_busy' });
});

it.each([
  { ...reply, handover: { producer_node_indices: [0] } },
  { ...reply, manual_handover: null },
  { ...reply, availability: { ...reply.availability, available: false } },
  { ...reply, manual_handover: { ...context, selection_revision: -1 } },
  { ...reply, manual_handover: { ...context, claim_generation: 0 } },
  { ...reply, manual_handover: { ...context, selection_revision: true } },
])('rejects inconsistent or noncompact owner replies', (value) => {
  expect(() => parseManualControlAvailability(value)).toThrow();
});

it.each(['current', 'stale'])('makes one read and checks its owner/request identity (%s)', async (caseName) => {
  const intent = buildJointMotionIntent(statusFixture(), [0, 0], 'query.1');
  const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({
    ...reply, request_id: caseName === 'stale' ? 'another-query' : 'query.1',
  }), { status: 200, headers: { 'content-type': 'application/json' } }));
  vi.stubGlobal('fetch', fetcher);
  if (caseName === 'stale') {
    await expect(api.execution.manualAvailability(intent)).rejects.toThrow('does not match');
  } else {
    await expect(api.execution.manualAvailability(intent)).resolves.toMatchObject({ available: true });
  }
  expect(fetcher).toHaveBeenCalledTimes(1);
  expect(fetcher.mock.calls[0][0]).toBe('/api/execution/manual-handover');
  expect(JSON.parse(fetcher.mock.calls[0][1].body)).toEqual(intent);
});
