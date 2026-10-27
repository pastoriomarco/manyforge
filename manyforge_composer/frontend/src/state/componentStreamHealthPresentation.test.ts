import { describe, expect, it } from 'vitest';

import { componentStreamHealthPresentation, streamCodeLabel, streamLabel } from './componentStreamHealthPresentation';
import type { CCLCComponentSnapshot, CCLCStreamHealth } from '../types/cclc';

function stream(overrides: Partial<CCLCStreamHealth>): CCLCStreamHealth {
  return {
    streamId: 'measured_joints', state: 'live', ageNs: 12_000_000, budgetNs: 250_000_000,
    severity: 'fault', recovery: 'ack', timestampSource: 'message_stamp',
    acknowledgementRequired: false, error: null, ...overrides,
  };
}

function snapshot(streamHealth?: CCLCStreamHealth[], incident = false): CCLCComponentSnapshot {
  return {
    controllableComponentId: 'component.arm', lifecycleGeneration: 1, lifecycleState: 'active',
    retainedBundleId: 'bundle.1', graphRevision: 'graph.1', publicationGeneration: 1,
    activePublicPointerDigest: 'pointer.1', domains: [], providers: [], lifecycleTarget: null,
    lifecycleProgress: incident ? {
      phase: 'satisfied', nextIntent: null, blockingCauseClass: 'provider_lost', blockingReason: null,
      waitingSinceNs: null, delayed: false,
      incident: { incidentId: 'i.1', causeClass: 'provider_lost', vendorCodes: [], detectedAtNs: 1,
        summary: 'stream measured_joints is stale', acknowledged: false },
    } : null,
    readiness: { requestOwnerEpoch: 'owner.1', evaluatedAtNs: 100, nextIntent: null, requestedState: null,
      ready: true, error: null, providerOccurrences: [], measuredState: { required: false, available: null, error: null } },
    ...(streamHealth === undefined ? {} : { streamHealth }),
  };
}

const stale = { code: 'component_stream_stale', message: 'stale', details: {} };

describe('componentStreamHealthPresentation', () => {
  it('shows nothing for a component without streams', () => {
    expect(componentStreamHealthPresentation(snapshot())).toBeNull();
  });

  it('summarizes all-live streams with age and budget per stream', () => {
    const presented = componentStreamHealthPresentation(snapshot([stream({})]))!;
    expect(presented.summary).toBe('Streams live (1)');
    expect(presented.dotClass).toBe('status-ready');
    expect(presented.streams[0]!.line)
      .toBe('measured_joints: live · age 12 ms / budget 250 ms · fault · recovery ack');
    expect(presented.recoverOffered).toBe(false);
  });

  it('orders worst first: fault event, then not-live by severity, then live', () => {
    const presented = componentStreamHealthPresentation(snapshot([
      stream({ streamId: 'a_live' }),
      stream({ streamId: 'b_warn', state: 'stale', severity: 'warning', recovery: 'auto', error: stale }),
      stream({ streamId: 'c_block', state: 'absent', ageNs: null, severity: 'blocking', recovery: 'auto',
        error: { ...stale, code: 'component_stream_absent' } }),
      stream({ streamId: 'd_event', error: { code: 'xarm_joint_value_invalid', message: 'joint 3 reported exactly 0 rad', details: {} } }),
    ]))!;
    expect(presented.streams.map((row) => row.streamId)).toEqual(['d_event', 'c_block', 'b_warn', 'a_live']);
    expect(presented.summary).toBe('d_event: fault event: invalid joint values · fault (+2)');
    expect(presented.dotClass).toBe('status-fault');
    expect(presented.streams[0]!.line).toContain('joint 3 reported exactly 0 rad');
    expect(presented.streams[1]!.line).toContain('c_block: absent · age none / budget 250 ms · blocking');
  });

  it('offers the component Recover for a latch without an incident only', () => {
    const latched = stream({ streamId: 'sim_clock', severity: 'blocking', acknowledgementRequired: true,
      error: { ...stale, code: 'component_stream_acknowledgement_required' } });
    const presented = componentStreamHealthPresentation(snapshot([latched]))!;
    expect(presented.summary).toBe('sim_clock: needs acknowledgement · blocking');
    expect(presented.dotClass).toBe('status-warning');
    expect(presented.recoverOffered).toBe(true);
    expect(componentStreamHealthPresentation(snapshot([latched], true))!.recoverOffered).toBe(false);
  });

  it('labels typed codes in one place and keeps unknown adapter codes as issued', () => {
    expect(streamCodeLabel('component_stream_stale')).toBe('stale');
    expect(streamCodeLabel('isaac_simulation_stopped')).toBe('simulation stopped');
    expect(streamCodeLabel('vendor_x')).toBe('vendor_x');
  });

  it('labels declared stream ids and keeps unknown ids', () => {
    expect(streamLabel('measured_joints')).toBe('Measured joints');
    expect(streamLabel('gripper_feedback')).toBe('Gripper feedback');
    expect(streamLabel('poses/part/before_planning')).toBe('Pose part / before_planning');
    expect(streamLabel('sim_clock')).toBe('sim_clock');
  });
});
