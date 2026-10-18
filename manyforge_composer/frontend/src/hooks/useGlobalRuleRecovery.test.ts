import { act, createElement, useEffect } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { reactPropsHost } from '../test/reactPropsHost';
import type { GlobalRuleRecoveryOffer } from '../types/robotControl';

const apiMock = vi.hoisted(() => ({
  execution: { globalRuleRecoveryOffers: vi.fn(), clearGlobalRuleLatch: vi.fn() },
}));
vi.mock('../api/client', async (original) => ({
  ...await original<typeof import('../api/client')>(), api: apiMock,
}));

import { useGlobalRuleRecovery } from './useGlobalRuleRecovery';

const offer = (patch: Partial<GlobalRuleRecoveryOffer> = {}): GlobalRuleRecoveryOffer => ({
  selector: {
    context: {
      owner_epoch: 'kernel.1', catalog_generation: '4', active_publication_generation: 2,
      bundle: 'bundle.1', graph: 'graph.1',
    },
    rule_id: 'door_open',
    expected_rule_registration_id: '9',
  },
  currentCondition: false,
  postLatchDisposition: 'await_operator_ack',
  latchCause: 'source_loss',
  ...patch,
});

let root: import('react-dom/client').Root;
let latest: ReturnType<typeof useGlobalRuleRecovery>;
function Harness({ ownerEpoch }: { ownerEpoch: string | null }) {
  const result = useGlobalRuleRecovery(true, ownerEpoch);
  useEffect(() => { latest = result; }, [result]);
  return null;
}
const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve(); });

beforeEach(async () => {
  const { host } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  root = createRoot(host as unknown as Element);
  apiMock.execution.globalRuleRecoveryOffers.mockReset().mockResolvedValue([offer()]);
  apiMock.execution.clearGlobalRuleLatch.mockReset();
});
afterEach(async () => {
  await act(async () => root.unmount());
  vi.unstubAllGlobals();
});

describe('global rule latch recovery', () => {
  it('sends the offer selector once and shows the owner result', async () => {
    await act(async () => root.render(createElement(Harness, { ownerEpoch: 'owner.1' })));
    await flush();
    expect(latest.latches).toEqual([expect.objectContaining({ ruleId: 'door_open', latchCause: 'source_loss' })]);
    apiMock.execution.clearGlobalRuleLatch.mockResolvedValue({
      accepted: false, replayStatus: 'accepted_new', ruleState: null, error: 'global_rule_condition_true',
    });
    await act(async () => { latest.clear!(latest.latches[0]); latest.clear!(latest.latches[0]); });
    await flush();
    expect(apiMock.execution.clearGlobalRuleLatch).toHaveBeenCalledTimes(1);
    expect(apiMock.execution.clearGlobalRuleLatch.mock.calls[0][0]).toBe('owner.1');
    expect(apiMock.execution.clearGlobalRuleLatch.mock.calls[0][2]).toEqual(offer());
    expect(latest.latches[0].clear).toMatchObject({
      pending: false, outcomeUnknown: false, message: 'Clear rejected: global_rule_condition_true.',
    });
  });

  it('never replays an unknown outcome and holds Clear until a later offer read', async () => {
    await act(async () => root.render(createElement(Harness, { ownerEpoch: 'owner.1' })));
    await flush();
    let fail!: (error: unknown) => void;
    apiMock.execution.clearGlobalRuleLatch.mockReturnValue(new Promise((_resolve, reject) => { fail = reject; }));
    await act(async () => { latest.clear!(latest.latches[0]); });
    expect(latest.latches[0].clear?.pending).toBe(true);
    let read!: (offers: GlobalRuleRecoveryOffer[]) => void;
    apiMock.execution.globalRuleRecoveryOffers.mockReturnValueOnce(new Promise((resolve) => { read = resolve; }));
    await act(async () => { fail(new TypeError('Failed to fetch')); });
    await flush();
    expect(latest.latches[0].clear).toMatchObject({ outcomeUnknown: true });
    expect(latest.latches[0].clear?.message).toContain('it was not resent');
    await act(async () => { latest.clear!(latest.latches[0]); });
    expect(apiMock.execution.clearGlobalRuleLatch).toHaveBeenCalledTimes(1);
    await act(async () => { read([offer()]); });
    await flush();
    expect(latest.latches[0].clear?.outcomeUnknown).toBe(false);
  });

  it('sends nothing while the condition is not false or no owner epoch is known', async () => {
    apiMock.execution.globalRuleRecoveryOffers.mockResolvedValue([offer({ currentCondition: null })]);
    await act(async () => root.render(createElement(Harness, { ownerEpoch: 'owner.1' })));
    await flush();
    await act(async () => { latest.clear!(latest.latches[0]); });
    expect(apiMock.execution.clearGlobalRuleLatch).not.toHaveBeenCalled();
    apiMock.execution.globalRuleRecoveryOffers.mockResolvedValue([offer()]);
    await act(async () => root.render(createElement(Harness, { ownerEpoch: null })));
    expect(latest.clear).toBeUndefined();
    await act(async () => root.render(createElement(Harness, { ownerEpoch: 'owner.2' })));
    expect(latest.clear).toBeDefined();
  });
});
