import { describe, expect, it } from 'vitest';
import { parseActionResult, parseCellLatch, parseComponentState, parseRecoveryPreflight } from './components';

const stop = { code: 'estop', message: 'Emergency stop pressed', since_ns: 5, vendor_code: 'C2' };

describe('OD-54 recover-and-enable wire', () => {
  it('reads an e-stop on a component and as a latch cause', () => {
    const state = parseComponentState({
      component_id: 'xarm', lifecycle: 'prepared', stop, activity: 'idle', streams: [], capabilities: [],
      observed_at_ns: 9,
    }, 'c');
    expect(state.stop).toEqual(stop);
    expect(parseCellLatch({ latched: true, causes: [{ component_id: 'xarm', kind: 'stop', stop }] }).causes[0])
      .toEqual({ component_id: 'xarm', kind: 'stop', stop });
  });
  it('reads the phases of a Recover-and-enable result', () => {
    const result = parseActionResult({
      action: 'enable', outcome: 'succeeded', started_at_ns: 1, finished_at_ns: 2,
      phases: [{ component_id: 'xarm', action: 'recover', outcome: 'succeeded' },
        { component_id: 'xarm.arm', action: 'enable', outcome: 'succeeded' }],
    }, 'r');
    expect(result.phases?.map((phase) => phase.action)).toEqual(['recover', 'enable']);
  });
  it('reads a preflight with an owner cause and an unknown member, and refuses malformed rows', () => {
    const preflight = parseRecoveryPreflight({ snapshot_id: 's1', components: [
      { component_id: 'xarm', owner_id: null, eligibility: 'recover', stop,
        cause: { code: 'estop', message: 'Emergency stop pressed', vendor_code: 'C2', vendor_text: null,
          clearable: true, operator_action: 'recover', concerns: ['xarm.arm'] } },
      { component_id: 'xarm.arm', owner_id: 'xarm', cause: null, eligibility: 'unknown' },
    ] });
    expect(preflight.components[0].cause?.vendor_code).toBe('C2');
    expect(preflight.components[1]).toEqual({ component_id: 'xarm.arm', owner_id: 'xarm', cause: null,
      stop: null, eligibility: 'unknown' });
    expect(() => parseRecoveryPreflight({ snapshot_id: 's1', components: [] })).toThrow();
    expect(() => parseRecoveryPreflight({ snapshot_id: 's1', components: [
      { component_id: 'x', owner_id: null, cause: null, eligibility: 'maybe' }] })).toThrow();
  });
});
