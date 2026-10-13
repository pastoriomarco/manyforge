import { expect, it } from 'vitest';
import type { ComposerHeadlessState } from '../types/headless';
import { buildProgramResetRequest } from './useProgramRuntime';
it('ordinary Idle Reset carries genuine null run/claim/terminal with exact cycle owner', () => {
  const cycle = { state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
    terminalOrSettlementReference: null, cycleGeneration: 4, programRunnerOwnerEpoch: 'runner.actual',
    allowedCommands: [{ commandKind: 'program_reset', allowed: true }] };
  const snapshot = { programCycle: cycle } as unknown as ComposerHeadlessState;
  expect(buildProgramResetRequest(snapshot, 'browser.owner.actual', 'reset.actual')).toMatchObject({
    program_run_id: null, expected_owner_epoch: 'runner.actual', expected_cycle_generation: 4,
    expected_claim_generation: null, terminal_reference: null, settlement_evidence_digest: null });
  for (const changed of [{ state: 'running' }, { cycleClaimGeneration: 3 }, { terminalOrSettlementReference: 'old' }])
    expect(buildProgramResetRequest({ programCycle: { ...cycle, ...changed } } as unknown as ComposerHeadlessState,
      'browser.owner.actual', 'reset.actual')).toBeNull();
});
