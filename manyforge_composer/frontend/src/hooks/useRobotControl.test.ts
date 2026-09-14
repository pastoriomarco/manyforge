import { describe, expect, it } from 'vitest';
import { ApiError } from '../api/client';

import type { RuntimeScaleControlStatus } from '../types/robotControl';
import {
  acceptedRuntimeScaleStatus,
  mergeRuntimeScaleStatus,
  previewTargetRejection,
  runtimeScaleControlScopeFence,
  runtimeScaleFlushDelay,
  manualAvailabilityScopeFence,
} from './useRobotControl';
import { statusFixture } from '../testFixtures/robotControl';
import type { ComposerHeadlessState } from '../types/headless';

it('manual availability follows owner and hold changes, not observed poses or node ticks', () => {
  const snapshot = {
    executionControlStatus: statusFixture(), authoringAdmission: { admissionGeneration: 1 },
    programCycle: { programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 1, state: 'running',
      executionControlRevision: 1, nodeStatusRevision: 1,
      runtimeControls: { executionSelections: { selectionRevision: 1,
        groups: [{ groupId: 'all', state: 'stopping' }] } },
    },
  } as unknown as ComposerHeadlessState;
  const initial = manualAvailabilityScopeFence(snapshot, 'joint');
  snapshot.executionControlStatus!.observedMotion.positionsRad = [0.5, 0.6];
  snapshot.executionControlStatus!.observedMotion.sequence += 1;
  snapshot.programCycle.nodeStatusRevision = 2;
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).toBe(initial);
  expect(manualAvailabilityScopeFence(snapshot, 'pose_goal')).toBe(initial);
  expect(manualAvailabilityScopeFence(snapshot, 'gripper')).not.toBe(initial);
  snapshot.programCycle.runtimeControls!.executionSelections.groups[0].state = 'held';
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).not.toBe(initial);
  const held = manualAvailabilityScopeFence(snapshot, 'joint');
  snapshot.executionControlStatus!.ownerEpoch = 'replacement';
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).not.toBe(held);
});

it('keeps known target rejection local without hiding transport or protocol failures', () => {
  expect(previewTargetRejection(new ApiError('IK failed', 500, {
    detail: { code: 'ik_failed', details: { reason: 'native_ik_no_solution' } },
  }))).toContain('may be out of reach, outside joint limits, or in collision');
  expect(previewTargetRejection(new ApiError('Collision', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_goal_state_collision' } },
  }))).toContain('would collide at the target');
  expect(previewTargetRejection(new ApiError('Collision', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_start_state_collision' } },
  }))).toContain('current robot pose is in collision');
  expect(previewTargetRejection(new ApiError('Wrong result', 500, {
    detail: { code: 'ik_failed', details: { reason: 'native_goal_state_collision' } },
  }))).toBeNull();
  expect(previewTargetRejection(new ApiError('No path', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_rrt_failed' } },
  })))
    .toContain('No valid path');
  for (const code of ['ik_failed', 'planning_failed']) {
    expect(previewTargetRejection(new ApiError('Unavailable', 500, {
      detail: { code, details: { reason: 'native_ik_transport_failure' } },
    }))).toBeNull();
  }
  expect(previewTargetRejection(new TypeError('Network error'))).toBeNull();
  expect(previewTargetRejection(new Error('Invalid preview receipt'))).toBeNull();
});

const status = {
  selector: {
    kind: 'singular',
    programBindingId: 'binding-1',
    componentRoleId: 'robot',
    targetRoleId: 'arm',
  },
  controlGeneration: 3,
  requestedScale: 0.4,
  supported: true,
  frozen: false,
  controlDomainId: 'domain-1',
  controlDomainRevision: 'domain-revision-1',
  controlBindingId: 'control-binding-1',
  controlBindingRevision: 'control-binding-revision-1',
  scalingDomainId: 'scaling-domain-1',
  activeControlConfigurationId: 'configuration-1',
  activeControlConfigurationGeneration: 7,
  activeOperationId: null,
  activeOperationRevision: null,
  activeOperationScaleGeneration: null,
  effectiveScale: null,
  applicationState: null,
  reason: null,
} satisfies RuntimeScaleControlStatus;

describe('persistent runtime-scale transport cadence', () => {
  it('keeps distinct synchronized target sets in distinct correlation fences', () => {
    const group = {
      ...status,
      selector: {
        kind: 'group' as const,
        programBindingId: 'binding-1',
        synchronizedTargets: [
          { componentRoleId: 'robot', targetRoleId: 'arm' },
          { componentRoleId: 'robot', targetRoleId: 'gripper' },
        ],
      },
      synchronizedMembers: [
        {
          componentRoleId: 'robot', targetRoleId: 'arm',
          controlDomainId: 'domain-arm', controlDomainRevision: 'rev-arm',
          controlBindingId: 'binding-arm', controlBindingRevision: 'binding-rev-arm',
          scalingDomainId: 'scale-arm', activeControlConfigurationId: 'config-arm',
          activeControlConfigurationGeneration: 1,
        },
        {
          componentRoleId: 'robot', targetRoleId: 'gripper',
          controlDomainId: 'domain-gripper', controlDomainRevision: 'rev-gripper',
          controlBindingId: 'binding-gripper', controlBindingRevision: 'binding-rev-gripper',
          scalingDomainId: 'scale-gripper', activeControlConfigurationId: 'config-gripper',
          activeControlConfigurationGeneration: 1,
        },
      ],
    };
    const differentGroup = {
      ...group,
      selector: { ...group.selector, synchronizedTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'camera' },
      ] },
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', differentGroup),
    );
    const delimiterCollisionGroup = {
      ...group,
      selector: {
        kind: 'group' as const,
        programBindingId: 'binding-1',
        synchronizedTargets: [
          { componentRoleId: 'a', targetRoleId: 'b\u0000c' },
          { componentRoleId: 'a\u0000b', targetRoleId: 'c' },
        ],
      },
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', delimiterCollisionGroup),
    );
    const reordered = { ...group, synchronizedMembers: [...group.synchronizedMembers].reverse() };
    expect(runtimeScaleControlScopeFence('owner-1', group)).toBe(
      runtimeScaleControlScopeFence('owner-1', reordered),
    );
    const changedMember = {
      ...group,
      synchronizedMembers: group.synchronizedMembers.map((member) => (
        member.targetRoleId === 'arm'
          ? { ...member, activeControlConfigurationGeneration: 2 }
          : member
      )),
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', changedMember),
    );
  });

  it('sends the first value immediately and coalesces later values to 250 ms', () => {
    expect(runtimeScaleFlushDelay(10, Number.NEGATIVE_INFINITY, true, false))
      .toBe(0);
    expect(runtimeScaleFlushDelay(100, 0, true, false)).toBe(150);
    expect(runtimeScaleFlushDelay(300, 0, true, false)).toBe(0);
  });

  it('does not schedule without a queued value or while one request is in flight', () => {
    expect(runtimeScaleFlushDelay(10, 0, false, false)).toBeNull();
    expect(runtimeScaleFlushDelay(10, 0, true, true)).toBeNull();
  });

  it('invalidates pending input only when its stable owner or scaling scope changes', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-2', status)).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      selector: { ...status.selector, programBindingId: 'binding-2' },
    })).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      scalingDomainId: 'scaling-domain-2',
    })).not.toBe(initial);
  });

  it('keeps queued input across ordinary scale acceptance revisions', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      controlGeneration: 4,
      requestedScale: 0.25,
      activeControlConfigurationId: 'configuration-2',
      activeControlConfigurationGeneration: 8,
      activeOperationScaleGeneration: 4,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toBe(initial);
  });

  it('uses an exact owner acceptance to fence the next scale update', () => {
    const accepted = acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: 'operation-1',
      activeOperationRevision: 8,
      activeOperationScaleGeneration: 2,
    }, 'scale-request-1', 0.25);
    expect(accepted).toMatchObject({
      controlGeneration: 4,
      requestedScale: 0.25,
      activeOperationId: 'operation-1',
    });
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-2',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.4,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
  });

  it('rejects a delayed projection older than the accepted owner generation', () => {
    const accepted = {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
    };
    expect(mergeRuntimeScaleStatus(accepted, status)).toBe(accepted);
    expect(mergeRuntimeScaleStatus(accepted, {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toMatchObject({
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    });
  });
});
