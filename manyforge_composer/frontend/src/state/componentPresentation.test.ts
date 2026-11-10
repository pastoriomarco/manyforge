import { describe, expect, it } from 'vitest';

import { parseApplyResult, parseCellLatch, parseComponentState, parseRunReadiness, parseSignalSample } from '../api/parsers/components';
import { applyImpactSummary, applyResultText } from '../components/ApplyImpact';
import { applyPreviewFixture } from '../test/applyPreview';
import { COMPONENT_CAPABILITIES } from '../types/components';
import {
  ageText,
  actionResultText,
  blockedReasonText,
  ruleHoldMeeting,
  codedErrorText,
  componentStateDetail,
  componentStateTree,
  componentSummary,
  componentTone,
  enableAllTargets,
  faultPresentation,
  flattenComponentTree,
  prepareAllTargets,
  latchCauseText,
  latchSummary,
  offeredActions,
  ownerIds,
  reasonText,
  runEndPresentation,
  COMPONENT_STATES_UNAVAILABLE,
  missingComponentStateText,
  streamLabel,
} from './componentPresentation';

const robot = {
  component_id: 'lite6_robot',
  lifecycle: 'fault',
  fault: {
    code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'enable',
    vendor_code: 'C2', vendor_text: 'emergency IO', since_ns: 5,
  },
  activity: 'idle',
  streams: [{ stream_id: 'joints', freshness: 'fresh', age_ns: 1000 }],
  capabilities: ['reset_position', 'io_manual_write'],
  observed_at_ns: 10,
};

describe('component model v2 wire parsing', () => {
  it('parses a component state with its fault, keeping the contract field names', () => {
    const state = parseComponentState(robot, 'state');
    expect(state.fault?.operator_action).toBe('enable');
    expect(state.streams[0]).toEqual({ stream_id: 'joints', freshness: 'fresh', age_ns: 1000 });
    expect(() => parseComponentState({ ...robot, lifecycle: 'active' }, 'state')).toThrow();
  });

  it('parses the optional read-only details', () => {
    expect(parseComponentState(robot, 'state')).not.toHaveProperty('details');
    const details = [{ name: 'Mode', value: '1' }, { name: 'Error code', value: 'C2' }];
    expect(parseComponentState({ ...robot, details }, 'state').details).toEqual(details);
    expect(() => parseComponentState({ ...robot, details: [{ name: 'Mode', value: 1 }] }, 'state')).toThrow();
  });

  it('parses the optional endpoints', () => {
    expect(parseComponentState(robot, 'state')).not.toHaveProperty('endpoints');
    const endpoints = [{ role_id: 'assistant_request', url: 'http://127.0.0.1:41234' }];
    expect(parseComponentState({ ...robot, endpoints }, 'state').endpoints).toEqual(endpoints);
    expect(() => parseComponentState({ ...robot, endpoints: [{ role_id: 'assistant_request' }] }, 'state'))
      .toThrow();
  });

  it('parses latch, readiness, samples and Apply results', () => {
    expect(parseCellLatch({ latched: true, causes: [{ component_id: 'a', kind: 'offline' }] }).causes[0])
      .toEqual({ component_id: 'a', kind: 'offline' });
    expect(parseRunReadiness({ can_execute: false, can_resume: false, motion_clear: false, blocked_reasons: [
      { code: 'rule_holds', message: 'held', rule_id: 'door' },
    ] }).blocked_reasons[0]).toEqual({ code: 'rule_holds', message: 'held', rule_id: 'door' });
    expect(parseSignalSample({ signal_id: 'plc/DO1', value: true, source: 'outside', age_ns: 2, fresh: true,
      controller_owned: false }, 's').source).toBe('outside');
    expect(() => parseSignalSample({ signal_id: 'x', value: 'on', source: 'device', age_ns: 0, fresh: true }, 's'))
      .toThrow();
    expect(parseApplyResult({ outcome: 'succeeded', components: [
      { component_id: 'a', disposition: 'replace', lifecycle_after: 'prepared' },
      { component_id: 'b', disposition: 'remove' },
    ] }).components).toEqual([
      { component_id: 'a', disposition: 'replace', lifecycle_after: 'prepared' },
      { component_id: 'b', disposition: 'remove' },
    ]);
  });
});

describe('component model v2 presentation', () => {
  it('offers the lifecycle actions plus declared action capabilities only', () => {
    expect(offeredActions(parseComponentState(robot, 'state')))
      .toEqual(['prepare', 'enable', 'disable', 'recover', 'reset_position']);
  });

  it('shows a fault in red with its catalogue text, operator action and vendor detail', () => {
    const state = parseComponentState(robot, 'state');
    expect(componentTone(state)).toBe('error');
    const fault = faultPresentation(state.fault!);
    expect(fault.title).toBe('Emergency stop active');
    expect(fault.operatorAction).toBe('Enable');
    expect(fault.vendor).toBe('Vendor: C2 · emergency IO');
    expect(componentSummary(state)).toBe('Fault · Emergency stop active');
  });

  it('declares sim_reset as a component capability and offers it as an action', () => {
    expect(COMPONENT_CAPABILITIES).toContain('sim_reset');
    const simulator = parseComponentState({
      component_id: 'isaac_sim', lifecycle: 'enabled', activity: 'idle',
      streams: [], capabilities: ['sim_reset'], observed_at_ns: 10,
    }, 'simulator');
    expect(offeredActions(simulator)).toContain('sim_reset');
  });

  it('shows a simulator fault as a warning while preserving its reported state and reason', () => {
    const fault = parseComponentState({
      component_id: 'isaac_sim', lifecycle: 'fault', activity: 'idle',
      fault: {
        code: 'fault_process_exited', message: 'The simulator process exited.',
        clearable: true, operator_action: 'recover', since_ns: 5,
      },
      streams: [], capabilities: ['sim_reset'], observed_at_ns: 10,
    }, 'simulator');
    expect(componentTone(fault)).toBe('warning');
    expect(fault.lifecycle).toBe('fault');
    expect(fault.fault?.message).toBe('The simulator process exited.');
  });

  it('keeps an unreachable simulator offline and presents it as a warning', () => {
    const simulator = parseComponentState({
      component_id: 'isaac_sim', lifecycle: 'offline', activity: 'idle',
      reason: { code: 'simulator_unreachable', message: "The simulator doesn't answer." },
      streams: [], capabilities: ['sim_reset'], observed_at_ns: 10,
    }, 'simulator');
    expect(componentTone(simulator)).toBe('warning');
    expect(simulator.lifecycle).toBe('offline');
    expect(simulator.reason?.code).toBe('simulator_unreachable');
  });

  it('shows an unknown simulator action as a warning and preserves its action result', () => {
    const simulator = parseComponentState({
      component_id: 'isaac_sim', lifecycle: 'enabled', activity: 'idle',
      last_action: { action: 'sim_reset', outcome: 'unknown', started_at_ns: 1, finished_at_ns: 2 },
      streams: [], capabilities: ['sim_reset'], observed_at_ns: 10,
    }, 'simulator');
    expect(componentTone(simulator)).toBe('warning');
    expect(actionResultText(simulator.last_action!)).toBe(
      'Reset simulation: outcome unknown (not resent)',
    );
  });

  it('shows a simulated component that stays enabled while the simulation is not playing (SIM-1)', () => {
    const state = parseComponentState({
      component_id: 'isaac_ur10e_light_robot', lifecycle: 'enabled', activity: 'idle',
      reason: { code: 'simulation_not_playing', message: 'The simulation is not playing' },
      streams: [{ stream_id: 'measured_joints', freshness: 'stale', age_ns: 2_000_000_000 }],
      capabilities: [], observed_at_ns: 1,
    }, 'state');
    expect(componentTone(state)).toBe('ok');
    expect(componentSummary(state)).toBe('Enabled · Simulation not playing');
  });

  it('summarizes the latch as "N errors require attention · <leading cause>"', () => {
    expect(latchSummary({ latched: false, causes: [] })).toBeNull();
    expect(latchSummary({ latched: true, causes: [{ component_id: 'a', kind: 'offline' }] })).toBe('a: offline');
    expect(latchSummary({ latched: true, causes: [
      { component_id: 'a', kind: 'activity_unknown' }, { component_id: 'b', kind: 'offline' },
    ] })).toBe("2 errors require attention · a: can't report whether it is still moving");
  });

  it('names blocked reasons with their ids and run ends with their cause', () => {
    expect(blockedReasonText({ code: 'component_not_enabled', message: 'x', component_id: 'lite6_robot' }))
      .toBe('lite6_robot: Component not enabled');
    expect(runEndPresentation({ end_reason: 'completed', details: {} }).tone).toBe('neutral');
    expect(runEndPresentation({ end_reason: 'stopped', details: { stopped_by: 'rule', rule_id: 'door' } }).text)
      .toBe('Stopped by rule door');
    expect(runEndPresentation({ end_reason: 'cell_fault', details: { component_id: 'lite6_robot' } }))
      .toMatchObject({ label: 'Ended by a cell fault', tone: 'error' });
    expect(ageText(2_500_000)).toBe('3 ms');
  });

  it('states the Preview impact and the Apply result', () => {
    expect(applyImpactSummary(applyPreviewFixture([
      { component_id: 'lite6_robot', disposition: 'replace', reasons: ['mount'], restarts: true },
      { component_id: 'lite_gripper', disposition: 'replace', reasons: ['mount'], restarts: true },
      { component_id: 'camera', disposition: 'retain', reasons: [], restarts: false },
    ]))).toBe('Restarts lite6_robot and lite_gripper; the others keep running.');
    expect(applyResultText({ outcome: 'succeeded', components: [
      { component_id: 'old_arm', disposition: 'remove' },
      { component_id: 'arm', disposition: 'add', lifecycle_after: 'prepared' },
    ] })).toBe('Applied: old_arm removed, arm prepared.');
    // The Apply continues past a failed step; each failure is named.
    const gripperError = { code: 'component_action_outcome_unknown', message: 'no reply' };
    const memberError = { code: 'owner_not_prepared', message: 'owner' };
    expect(applyResultText(parseApplyResult({ outcome: 'failed', error: gripperError, components: [
      { component_id: 'lite6_robot', disposition: 'add', lifecycle_after: 'prepared' },
      { component_id: 'gripper', disposition: 'add', lifecycle_after: 'not_prepared', error: gripperError },
      { component_id: 'io_member', disposition: 'add', lifecycle_after: 'offline', error: memberError },
      { component_id: 'camera', disposition: 'retain', lifecycle_after: 'enabled' },
    ] }))).toBe(`Apply failed: lite6_robot prepared; gripper: ${codedErrorText(gripperError)}; `
      + `io_member: ${codedErrorText(memberError)}.`);
    expect(applyResultText({ outcome: 'failed', components: [],
      error: { code: 'cell_latched', message: 'x' } })).toContain('Cell latched');
    // The Apply's own failure (no component row carries it) leads the line.
    const fenceError = { code: 'apply_fence_evidence_failed', message: 'fence' };
    expect(applyResultText({ outcome: 'failed', error: fenceError, components: [
      { component_id: 'lite6_robot', disposition: 'replace', lifecycle_after: 'prepared' },
    ] })).toBe(`Apply failed: ${codedErrorText(fenceError)}; lite6_robot prepared.`);
    // Both: the Apply's own failure first, the component's on its named row.
    const duplicate = { code: 'duplicate_component_instance', message: 'conflicting endpoints' };
    expect(applyResultText({ outcome: 'failed', error: fenceError, components: [
      { component_id: 'lite6', disposition: 'replace', lifecycle_after: 'prepared' },
      { component_id: 'lite_gripper', disposition: 'replace', lifecycle_after: 'prepared', error: duplicate },
    ] })).toBe(`Apply failed: ${codedErrorText(fenceError)}; lite6 prepared; `
      + `lite_gripper: ${codedErrorText(duplicate)}.`);
    // A host refusal names its typed cause on the component it concerns; a
    // component the failed Apply never installed isn't called removed.
    const site = { code: 'site_settings_unavailable',
      message: 'component lite6_robot: no site settings file is configured for this machine' };
    expect(applyResultText({ outcome: 'failed', error: site, components: [
      { component_id: 'lite6', disposition: 'add' },
      { component_id: 'lite6_robot', disposition: 'add', error: site },
      { component_id: 'old_arm', disposition: 'remove' },
    ] })).toBe('Apply failed: lite6 not in the cell; lite6_robot: No site settings file; old_arm removed.');
  });
});

describe('machines: owners and members', () => {
  const owner = parseComponentState({
    component_id: 'lite6', lifecycle: 'fault',
    fault: { code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'recover',
      concerns: ['lite6_robot', 'lite6_robot_controller_io'], since_ns: 5 },
    activity: 'idle', streams: [], capabilities: ['power_off', 'reboot'], observed_at_ns: 10,
  }, 'owner');
  const arm = parseComponentState({
    component_id: 'lite6_robot', owner_id: 'lite6', role: 'arm', lifecycle: 'offline',
    reason: { code: 'owner_unavailable', message: 'through lite6: C2' },
    activity: 'idle', streams: [], capabilities: [], observed_at_ns: 10,
  }, 'arm');
  const io = parseComponentState({
    component_id: 'lite6_robot_controller_io', owner_id: 'lite6', role: 'controller_io', lifecycle: 'prepared',
    activity: 'idle', streams: [], capabilities: ['io_manual_write'], observed_at_ns: 10,
  }, 'io');
  const plc = parseComponentState({
    component_id: 'plc', lifecycle: 'enabled', activity: 'idle', streams: [], capabilities: [], observed_at_ns: 10,
  }, 'plc');

  it('parses owner_id, role and the fault\'s concerns', () => {
    expect(arm.owner_id).toBe('lite6');
    expect(arm.role).toBe('arm');
    expect(owner.fault?.concerns).toEqual(['lite6_robot', 'lite6_robot_controller_io']);
    expect(plc).not.toHaveProperty('owner_id');
    expect(() => parseComponentState({ ...plc, owner_id: 3 }, 'plc')).toThrow();
  });

  it('offers an owner Prepare, Recover and its capabilities, no Enable or Disable', () => {
    const states = [owner, arm, io, plc];
    expect([...ownerIds(states)]).toEqual(['lite6']);
    expect(offeredActions(owner, true)).toEqual(['prepare', 'recover', 'power_off', 'reboot']);
    expect(offeredActions(arm)).toEqual(['prepare', 'enable', 'disable', 'recover']);
  });

  it('offers no Prepare on an enabled component, which the host refuses (component_enabled)', () => {
    const enabled = parseComponentState({ ...robot, lifecycle: 'enabled', fault: undefined }, 'state');
    expect(offeredActions(enabled)).toEqual(['enable', 'disable', 'recover', 'reset_position']);
    expect(offeredActions({ ...enabled, lifecycle: 'prepared' })[0]).toBe('prepare');
  });

  it('shows a member offline through its owner with the owner\'s own fault', () => {
    expect(reasonText(arm, [owner, arm])).toBe('through lite6: Emergency stop active');
    expect(componentSummary(arm, [owner, arm])).toBe('Offline · through lite6: Emergency stop active');
    // Without the owner's state: the producer's text.
    expect(reasonText(arm)).toBe('through lite6: C2');
    expect(reasonText(plc)).toBeNull();
  });

  it('omits the simulator from Prepare and Enable targets while retaining ordinary targets', () => {
    const offlineSimulator = parseComponentState({
      component_id: 'isaac_sim', lifecycle: 'offline', activity: 'idle',
      streams: [], capabilities: ['sim_reset'], observed_at_ns: 10,
    }, 'simulator');
    const pausedSimulator = { ...offlineSimulator, lifecycle: 'prepared' as const };
    const unpreparedSimulator = { ...offlineSimulator, lifecycle: 'not_prepared' as const };
    const offlineRobot = { ...plc, component_id: 'offline_robot', lifecycle: 'offline' as const };
    const preparedRobot = { ...plc, component_id: 'prepared_robot', lifecycle: 'prepared' as const };

    expect(prepareAllTargets([offlineSimulator, unpreparedSimulator, offlineRobot])).toEqual(['offline_robot']);
    expect(enableAllTargets([pausedSimulator, preparedRobot])).toEqual(['prepared_robot']);
  });

  it('lists one tree (members under their machine, a mounted tool under its parent) and names the members an owner fault concerns', () => {
    const gripper = { ...plc, component_id: 'lite_gripper' };
    const tree = (states: Parameters<typeof componentStateTree>[0], mounts?: Record<string, string>) => (
      flattenComponentTree(componentStateTree(states, mounts)).map(({ item, depth }) => [item.component_id, depth]));
    expect(tree([gripper, arm, plc, owner, io], { lite_gripper: 'lite6_robot' })).toEqual([
      ['plc', 0], ['lite6', 0], ['lite6_robot', 1], ['lite_gripper', 2], ['lite6_robot_controller_io', 1],
    ]);
    // A member whose owner isn't listed, a tool whose parent isn't: on their own.
    expect(tree([gripper, arm], { lite_gripper: 'other' })).toEqual([['lite_gripper', 0], ['lite6_robot', 0]]);
    expect(componentStateDetail(arm, [owner, arm])).toBe('through lite6: Emergency stop active');
    expect(componentStateDetail(plc)).toBeNull();
    // Prepare all: not prepared or offline; Enable all: not yet enabled (the
    // backend prepares an unprepared one first), never an owner.
    expect(prepareAllTargets([owner, arm, io, plc])).toEqual(['lite6_robot']);
    expect(enableAllTargets([{ ...owner, lifecycle: 'prepared' }, arm, io, plc]))
      .toEqual(['lite6_robot', 'lite6_robot_controller_io']);
    expect(latchCauseText({ component_id: 'lite6', kind: 'fault', fault: owner.fault }))
      .toBe('lite6: Emergency stop active (concerns lite6_robot, lite6_robot_controller_io)');
  });
});

describe('stream labels and unavailable states', () => {
  it('labels declared streams, pose streams per object and policy, and passes others through', () => {
    expect(streamLabel('measured_joints')).toBe('Measured joints');
    expect(streamLabel('gripper_feedback')).toBe('Gripper feedback');
    expect(streamLabel('endpoint')).toBe('Model endpoint');
    expect(streamLabel('poses/part')).toBe('Pose part');
    expect(streamLabel('poses/robot/viewer.measured_robot')).toBe('Pose robot / viewer.measured_robot');
    expect(streamLabel('io_states')).toBe('io_states');
  });

  it('says unavailable, not "not in the cell", while the host state cannot be read', () => {
    expect(missingComponentStateText(false)).toBe(COMPONENT_STATES_UNAVAILABLE);
    expect(missingComponentStateText(true)).toBe('Not in the cell: the next Apply brings it up.');
  });
});

describe('ruleHoldMeeting (as the backend rule_hold_meets_program)', () => {
  const hold = (scope: string, component_ids?: string[]) => ({
    code: 'rule_holds', message: 'held', rule_id: 'door', scope,
    ...(component_ids === undefined ? {} : { component_ids }),
  });
  it('meets by scope', () => {
    expect(ruleHoldMeeting([hold('everything')], [])).not.toBeNull();
    expect(ruleHoldMeeting([hold('all_motion')], ['robot'])).not.toBeNull();
    expect(ruleHoldMeeting([hold('all_motion')], [])).toBeNull();
    expect(ruleHoldMeeting([hold('execution_groups', ['other', 'robot'])], ['robot'])).not.toBeNull();
    expect(ruleHoldMeeting([hold('execution_groups', ['other'])], ['robot'])).toBeNull();
    expect(ruleHoldMeeting([{ code: 'cell_latched', message: 'x' }], ['robot'])).toBeNull();
  });
  it('parses the scope fields of a rule_holds reason', () => {
    const parsed = parseRunReadiness({ can_execute: false, can_resume: false, motion_clear: true,
      blocked_reasons: [hold('execution_groups', ['robot'])] });
    expect(parsed.blocked_reasons[0]).toEqual(hold('execution_groups', ['robot']));
  });
});
