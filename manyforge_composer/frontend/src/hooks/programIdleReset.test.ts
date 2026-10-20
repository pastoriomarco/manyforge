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

it('a failed Reset in recovery_required is retried with exactly its retained options', () => {
  const allowedCommands = [{ commandKind: 'program_reset', allowed: true }];
  const failed = { state: 'recovery_required', programRunId: null, cycleClaimGeneration: null,
    terminalOrSettlementReference: null, cycleGeneration: 7, programRunnerOwnerEpoch: 'runner.actual',
    currentCommandKind: 'program_reset', resetPhase: 'restoring_scene',
    failure: { failed_phase: 'restoring_scene' },
    resetOptions: { reloadInitialScene: true, resetRobotPosition: true }, allowedCommands };
  const build = (cycle: object, options?: object) => buildProgramResetRequest(
    { programCycle: cycle } as unknown as ComposerHeadlessState,
    'browser.owner.actual', 'reset.retry', 'reset.retry', options);
  // (a) Run-less: sent with null run and the retained options, whatever the caller passes.
  for (const options of [undefined, {}, { reloadInitialScene: false, resetRobotPosition: false }])
    expect(build(failed, options)).toMatchObject({
      program_run_id: null, expected_owner_epoch: 'runner.actual', expected_cycle_generation: 7,
      expected_claim_generation: null, terminal_reference: null, settlement_evidence_digest: null,
      reload_initial_scene: true, reset_robot_position: true });
  // (b) With a run: the cycle identity is kept and the retained options still win.
  expect(build({ ...failed, programRunId: 'run-1', cycleClaimGeneration: 12,
    resetOptions: { reloadInitialScene: false, resetRobotPosition: true } },
  { reloadInitialScene: true, resetRobotPosition: false })).toMatchObject({
    program_run_id: 'run-1', expected_claim_generation: 12,
    reload_initial_scene: false, reset_robot_position: true });
  // A recovery_required with a run but no retained Reset keeps using the caller's options.
  expect(build({ ...failed, programRunId: 'run-1', currentCommandKind: null, resetPhase: null,
    failure: null, resetOptions: null }, { reloadInitialScene: true })).toMatchObject({
    program_run_id: 'run-1', reload_initial_scene: true, reset_robot_position: false });
  // (d) Run-less without a retained Reset, or in another non-idle state, is still not sent.
  expect(build({ ...failed, currentCommandKind: null, resetOptions: null })).toBeNull();
  for (const state of ['resetting', 'terminal_awaiting_reset', 'running'])
    expect(build({ ...failed, state })).toBeNull();
  expect(build({ ...failed, allowedCommands: [{ commandKind: 'program_reset', allowed: false }] })).toBeNull();
});

it('idle Reset still uses the caller options, including the empty contract-141 Reset', () => {
  const cycle = { state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
    terminalOrSettlementReference: null, cycleGeneration: 4, programRunnerOwnerEpoch: 'runner.actual',
    currentCommandKind: null, resetOptions: null,
    allowedCommands: [{ commandKind: 'program_reset', allowed: true }] };
  const snapshot = { programCycle: cycle } as unknown as ComposerHeadlessState;
  expect(buildProgramResetRequest(snapshot, 'o', 'r')).toMatchObject({
    program_run_id: null, reload_initial_scene: false, reset_robot_position: false });
  expect(buildProgramResetRequest(snapshot, 'o', 'r', 'r', { resetRobotPosition: true })).toMatchObject({
    reload_initial_scene: false, reset_robot_position: true });
});
