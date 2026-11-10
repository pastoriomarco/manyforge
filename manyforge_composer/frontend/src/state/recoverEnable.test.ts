import { describe, expect, it } from 'vitest';
import type { ComponentState, RecoveryPreflight } from '../types/components';
import {
  actionResultText,
  componentSummary,
  componentTone,
  latchCauseText,
  needsRecovery,
  recoveryCauseText,
  stateLabel,
} from './componentPresentation';
import { recoveryTargets } from './activateExecute';
import { recoveryBlockers, recoveryConfirmation, withinConfirmed } from './recoverEnable';

const stop = { code: 'estop' as const, message: 'E-stop pressed or not yet cleared', since_ns: 1, vendor_code: 'C2' };
function state(component_id: string, patch: Partial<ComponentState> = {}): ComponentState {
  return { component_id, lifecycle: 'prepared', activity: 'idle', streams: [], capabilities: [], observed_at_ns: 1, ...patch };
}
const owner = state('xarm', { stop });
const arm = state('xarm.arm', { owner_id: 'xarm', reason: { code: 'stopped_by_estop', message: '' } });
const io = state('xarm.io', { owner_id: 'xarm' });
const ownerRow = { component_id: 'xarm', owner_id: null, stop, eligibility: 'recover' as const,
  cause: { code: 'estop', message: 'E-stop', vendor_code: 'C2', vendor_text: null, clearable: true,
    operator_action: 'recover' as const, concerns: ['xarm.arm'] } };
const preflight: RecoveryPreflight = { snapshot_id: 's', components: [ownerRow,
  { component_id: 'xarm.arm', owner_id: 'xarm', cause: null, stop: null, eligibility: 'unknown' }] };

describe('e-stop presentation (OD-54)', () => {
  it('shows the machine as E-stop (a stop, not a fault) and its members with the reason', () => {
    expect(stateLabel(owner)).toBe('E-stop');
    expect(componentSummary(owner)).toBe('E-stop · E-stop pressed or not yet cleared (C2)');
    expect(componentTone(owner)).toBe('warning');
    expect(componentSummary(arm)).toBe('Prepared · Stopped by e-stop');
    expect(latchCauseText({ component_id: 'xarm', kind: 'stop', stop })).toBe('xarm: E-stop pressed or not yet cleared (C2)');
  });
  it('names the step that stopped a Recover and enable', () => {
    const base = { action: 'enable', started_at_ns: 1, finished_at_ns: 2 };
    expect(actionResultText({ ...base, outcome: 'succeeded', phases: [
      { component_id: 'xarm', action: 'recover', outcome: 'succeeded' },
      { component_id: 'xarm.arm', action: 'enable', outcome: 'succeeded' }] })).toBe('Recover and enable succeeded');
    expect(actionResultText({ ...base, outcome: 'failed', phases: [
      { component_id: 'xarm', action: 'recover', outcome: 'failed', error: { code: 'fault_estop', message: '' } }] }))
      .toBe('Recover and enable stopped: Recover xarm failed: Emergency stop active');
    expect(actionResultText({ ...base, outcome: 'unknown', phases: [
      { component_id: 'xarm', action: 'recover', outcome: 'succeeded' },
      { component_id: 'xarm.arm', action: 'enable', outcome: 'unknown' }] }))
      .toBe('Recover and enable stopped: Enable xarm.arm, outcome unknown (not resent)');
  });
});

describe('what needs recovery', () => {
  it('a component with its own fault or e-stop, or whose machine reports one that concerns it', () => {
    const states = [owner, arm, io];
    expect(needsRecovery(arm, states)).toBe(true);
    // The machine's stop applies to every member.
    expect(needsRecovery(io, states)).toBe(true);
    const faulted = state('xarm', { lifecycle: 'fault', fault: { code: 'fault_collision', message: '', clearable: true,
      operator_action: 'recover', concerns: ['xarm.arm'], since_ns: 1 } });
    expect(needsRecovery(arm, [faulted, arm, io])).toBe(true);
    expect(needsRecovery(io, [faulted, state('xarm.arm', { owner_id: 'xarm' }), io])).toBe(false);
    expect(needsRecovery(state('sim', { capabilities: ['sim_reset'], fault: faulted.fault }), [])).toBe(false);
    // Machines aren't enabled: only their members are targets.
    expect(recoveryTargets([owner, arm, io])).toEqual(['xarm.arm', 'xarm.io']);
    expect(recoveryTargets([owner, state('xarm.arm', { owner_id: 'xarm', lifecycle: 'enabled' })])).toEqual([]);
  });
});

describe('the confirmation', () => {
  it('sends back exactly the facts shown', () => {
    expect(recoveryConfirmation(preflight)).toEqual({ snapshot_id: 's', confirmed: [
      { component_id: 'xarm', cause_code: 'estop', vendor_code: 'C2', stop_code: 'estop', stop_vendor_code: 'C2', eligibility: 'recover' },
      { component_id: 'xarm.arm', cause_code: null, vendor_code: null, stop_code: null, stop_vendor_code: null, eligibility: 'unknown' },
    ] });
    expect(recoveryCauseText(preflight.components[0]!)).toBe('xarm: E-stop pressed or not yet cleared (C2)');
    expect(recoveryCauseText(preflight.components[1]!)).toBe('xarm.arm: state unknown until its machine recovers');
  });
  it('blocks what cannot be recovered here, and an unknown member without a recoverable machine', () => {
    expect(recoveryBlockers(preflight)).toEqual([]);
    expect(recoveryBlockers({ snapshot_id: 's', components: [{ ...ownerRow, eligibility: 'blocked' }] })[0])
      .toMatch(/^xarm: /);
    expect(recoveryBlockers({ snapshot_id: 's', components: [preflight.components[1]!] }))
      .toEqual(["xarm.arm: its state is unknown and its machine can't be recovered first."]);
  });
  it('lets a recovered machine drop out, and refuses a new or different cause', () => {
    const recovered: RecoveryPreflight = { snapshot_id: 't', components: [
      { ...ownerRow, stop: null, cause: null, eligibility: 'ready' },
      { component_id: 'xarm.io', owner_id: 'xarm', cause: null, stop: null, eligibility: 'ready' }] };
    expect(withinConfirmed(recovered, [preflight])).toBe(true);
    expect(withinConfirmed(preflight, [preflight])).toBe(true);
    const different: RecoveryPreflight = { snapshot_id: 'u', components: [
      { ...ownerRow, stop: null, cause: { ...ownerRow.cause, code: 'fault_collision' } }] };
    expect(withinConfirmed(different, [preflight])).toBe(false);
  });
});
