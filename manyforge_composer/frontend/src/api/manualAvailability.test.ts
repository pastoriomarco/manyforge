import { afterEach, expect, it, vi } from 'vitest';
import { api } from './client';
import { parseManualControlAvailability } from './parsers';
import { buildJointMotionIntent } from '../state/robotControl';
import { statusFixture } from '../testFixtures/robotControl';

const START = '3f9a000000000001';
const SENT_KEY = '00112233445566778899aabbccddeeff';
// Headless sets byte 0 (class 3, part 0) and keeps bytes 1-15 (contract 1.4).
const FORMED_KEY = 'c0112233445566778899aabbccddeeff';
const context = { program_run_id: 1, claim_generation: 1, hold_revision: 0 };
const reply = {
  api_version: 'manyforge.execution_command_response.v6', command: 'read_manual_handover',
  ok: true, request_key: FORMED_KEY,
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
  { ...reply, manual_handover: { ...context, hold_revision: -1 } },
  { ...reply, manual_handover: { ...context, claim_generation: 0 } },
  { ...reply, manual_handover: { ...context, hold_revision: true } },
  { ...reply, manual_handover: { ...context, program_run_id: 'program.1' } },
  { ...reply, manual_handover: { ...context, program_runner_owner_epoch: 'runner.1' } },
  { ...reply, request_key: 'query.1' },
  { ...reply, request_owner_epoch: 'execution-owner-1' },
  { ...reply, api_version: 'manyforge.execution_command_response.v4' },
])('rejects inconsistent or noncompact owner replies', (value) => {
  expect(() => parseManualControlAvailability(value)).toThrow();
});

it.each(['current', 'stale'])('makes one read and checks its owner/request identity (%s)', async (caseName) => {
  const intent = buildJointMotionIntent(statusFixture(), [0, 0], SENT_KEY);
  const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({
    ...reply, request_key: caseName === 'stale' ? 'c0ffffffffffffffffffffffffffffff' : FORMED_KEY,
  }), { status: 200, headers: { 'content-type': 'application/json' } }));
  vi.stubGlobal('fetch', fetcher);
  if (caseName === 'stale') {
    await expect(api.execution.manualAvailability(intent, START)).rejects.toThrow('does not match');
  } else {
    await expect(api.execution.manualAvailability(intent, START)).resolves.toMatchObject({ available: true });
  }
  expect(fetcher).toHaveBeenCalledTimes(1);
  expect(fetcher.mock.calls[0][0]).toBe('/api/execution/manual-handover');
  const body = JSON.parse(fetcher.mock.calls[0][1].body);
  expect(body).toEqual({ ...intent, application_start_id: START });
  expect(body.request_context).toEqual({ request_key: SENT_KEY, source_label: 'composer_ui' });
});
