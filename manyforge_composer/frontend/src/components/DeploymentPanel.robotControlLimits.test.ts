import { describe, expect, it } from 'vitest';

import type { RobotControlJointLimit } from '../types/api';
import { applyRobotControlJointLimitEdit } from './DeploymentPanel';

describe('applyRobotControlJointLimitEdit (Phase 4-P5.1K per-joint caps)', () => {
  it('sets a jerk cap and preserves it alongside other dimensions', () => {
    const next = applyRobotControlJointLimitEdit({}, 'elbow_joint', 'maxJerkRadS3', '8');
    expect(next.elbow_joint.maxJerkRadS3).toBe(8);
    expect(next.elbow_joint.maxVelocityRadS ?? null).toBeNull();
  });

  it('preserves an existing jerk cap when editing velocity', () => {
    const limits: Record<string, RobotControlJointLimit> = {
      elbow_joint: { maxJerkRadS3: 8 },
    };
    const next = applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxVelocityRadS', '0.4');
    expect(next.elbow_joint.maxVelocityRadS).toBe(0.4);
    expect(next.elbow_joint.maxJerkRadS3).toBe(8); // not dropped
  });

  it('does NOT drop a joint that still has a jerk cap after clearing vel/accel', () => {
    const limits: Record<string, RobotControlJointLimit> = {
      elbow_joint: { maxVelocityRadS: 0.4, maxJerkRadS3: 8 },
    };
    const next = applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxVelocityRadS', '');
    expect(next.elbow_joint).toBeDefined();
    expect(next.elbow_joint.maxVelocityRadS ?? null).toBeNull();
    expect(next.elbow_joint.maxJerkRadS3).toBe(8);
  });

  it('drops a joint only when velocity, acceleration AND jerk are all empty', () => {
    const limits: Record<string, RobotControlJointLimit> = {
      elbow_joint: { maxJerkRadS3: 8 },
    };
    const next = applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxJerkRadS3', '');
    expect(next.elbow_joint).toBeUndefined();
  });

  it('rejects non-positive / non-finite input by returning the same reference unchanged', () => {
    const limits: Record<string, RobotControlJointLimit> = {
      elbow_joint: { maxVelocityRadS: 0.4 },
    };
    expect(applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxJerkRadS3', '0')).toBe(limits);
    expect(applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxJerkRadS3', '-1')).toBe(limits);
    expect(applyRobotControlJointLimitEdit(limits, 'elbow_joint', 'maxJerkRadS3', 'abc')).toBe(limits);
  });
});
