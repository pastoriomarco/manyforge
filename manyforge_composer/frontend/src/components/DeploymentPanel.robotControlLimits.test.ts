import { describe, expect, it } from 'vitest';

import type { RobotControlJointLimit } from '../types/api';
import {
  CONTROLLED_STOP_FIELD_HELP,
  applyControlledStopNumberRecordEdit,
  applyControlledStopAutoDeadlineEdit,
  applyControlledStopProfileModeEdit,
  applyRobotControlJointLimitEdit,
  controlledStopAutoMaxStopTimeS,
  controlledStopCompletionToleranceS,
  controlledStopFieldHelp,
  normalizeControlledStopProfileForMode,
} from './DeploymentPanel';
import { OPERATOR_STOP_SLOT } from '../store/stopProfile';

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

describe('controlled stop deployment reducers', () => {
  it('documents every controlled-stop field with defaults, limits, and consequences', () => {
    expect(Object.keys(CONTROLLED_STOP_FIELD_HELP).sort()).toEqual([
      'decelerationTime',
      'leadIn',
      'maxDeceleration',
      'maxJerk',
      'maxStopTime',
      'mode',
      'naturalCompletion',
      'pathRamp',
      'samplePeriod',
      'settleVelocity',
      'stopCompletionTimeout',
    ]);
    for (const key of Object.keys(CONTROLLED_STOP_FIELD_HELP) as Array<keyof typeof CONTROLLED_STOP_FIELD_HELP>) {
      expect(controlledStopFieldHelp(key).length).toBeGreaterThan(40);
    }
    expect(controlledStopFieldHelp('decelerationTime')).toContain('<= 2.0 s');
    expect(controlledStopFieldHelp('decelerationTime')).toContain('left unset');
    expect(controlledStopFieldHelp('maxStopTime')).toContain('Leave blank');
    expect(controlledStopFieldHelp('maxStopTime')).toContain('placeholder');
    expect(controlledStopFieldHelp('maxStopTime')).toContain('stop failure');
    expect(controlledStopFieldHelp('stopCompletionTimeout')).toContain('stop_completion_timeout_s');
    expect(controlledStopFieldHelp('stopCompletionTimeout')).toContain('greater than');
    expect(controlledStopFieldHelp('maxDeceleration')).toContain('Leave blank');
    expect(controlledStopFieldHelp('maxDeceleration')).toContain('hard-stop');
    expect(controlledStopFieldHelp('maxJerk')).toContain('Leave blank');
    expect(controlledStopFieldHelp('maxJerk')).toContain('infeasible');
  });

  it('derives the visible auto max-stop deadline from the active execution lane', () => {
    expect(
      controlledStopCompletionToleranceS(
        {},
        'execution.ros2_control_follow_joint_trajectory',
      ),
    ).toBe(0.5);
    expect(
      controlledStopAutoMaxStopTimeS(
        { stop_completion_timeout_s: 1.5 },
        'execution.ros2_control_follow_joint_trajectory',
        2.0,
      ),
    ).toBeCloseTo(3.5);
    expect(
      controlledStopAutoMaxStopTimeS(
        { soft_on_path_sample_period_s: 0.02 },
        'execution.roboplan_native',
        2.0,
      ),
    ).toBeCloseTo(2.02);
  });

  it('edits the ros2_control auto deadline as a total time greater than the requested stop time', () => {
    const executionConfig = {
      operator_stop_profile: {
        stop_mode: 'soft_on_path',
        deceleration_time_s: 1.0,
      },
      default_fault_stop_profile: {
        stop_mode: 'spring_back_hold',
        deceleration_time_s: 1.5,
      },
    };
    const tooShort = applyControlledStopAutoDeadlineEdit(executionConfig, '1.5');
    expect(tooShort).toBe(executionConfig);
    const next = applyControlledStopAutoDeadlineEdit(executionConfig, '2.0');
    expect(next.stop_completion_timeout_s).toBeCloseTo(0.5);
  });

  it('creates a complete soft_on_path operator profile with per-joint decel defaults', () => {
    const next = applyControlledStopProfileModeEdit(
      {},
      OPERATOR_STOP_SLOT,
      'soft_on_path',
      ['shoulder_joint', 'elbow_joint'],
    );
    const profile = next.operator_stop_profile as Record<string, unknown>;
    expect(profile.profile_id).toBe('operator_soft_on_path');
    expect(profile.stop_mode).toBe('soft_on_path');
    expect(profile.deceleration_time_s).toBe(1.0);
    expect(profile.require_backend_confirmation).toBe(true);
    expect(profile.allow_natural_completion_on_short_path).toBe(true);
    expect(profile.allowed_fallback_modes).toBeUndefined();
    expect(profile.per_joint_max_deceleration_rad_s2).toEqual({
      shoulder_joint: 10,
      elbow_joint: 10,
    });
  });

  it('creates a complete spring_back_hold operator profile with per-joint decel defaults', () => {
    const next = applyControlledStopProfileModeEdit(
      {},
      OPERATOR_STOP_SLOT,
      'spring_back_hold',
      ['shoulder_joint', 'elbow_joint'],
    );
    const profile = next.operator_stop_profile as Record<string, unknown>;
    expect(profile.profile_id).toBe('operator_spring_back_hold');
    expect(profile.stop_mode).toBe('spring_back_hold');
    expect(profile.deceleration_time_s).toBe(1.0);
    expect(profile.require_backend_confirmation).toBe(true);
    expect(profile.allow_natural_completion_on_short_path).toBeUndefined();
    expect(profile.per_joint_max_deceleration_rad_s2).toEqual({
      shoulder_joint: 10,
      elbow_joint: 10,
    });
  });

  it('removes the profile slot when the mode is cleared', () => {
    const next = applyControlledStopProfileModeEdit(
      {
        operator_stop_profile: {
          profile_id: 'operator_soft_on_path',
          stop_mode: 'soft_on_path',
          deceleration_time_s: 1.0,
        },
      },
      OPERATOR_STOP_SLOT,
      '',
      ['joint_1'],
    );
    expect(next.operator_stop_profile).toBeUndefined();
  });

  it('keeps explicit max_stop_time_s at or after the requested decel window', () => {
    const next = normalizeControlledStopProfileForMode(
      {
        profile_id: 'operator_soft',
        stop_mode: 'soft_on_path',
        deceleration_time_s: 1.5,
        max_stop_time_s: 0.5,
        settle_velocity_threshold_rad_s: 0.02,
      },
      OPERATOR_STOP_SLOT,
      'soft_on_path',
      ['joint_1'],
    );
    expect(next.max_stop_time_s).toBe(1.5);
    expect(next.settle_velocity_threshold_rad_s).toBe(0.02);
  });

  it('removes soft-only natural-completion when switching to spring_back_hold but preserves path limits', () => {
    const next = applyControlledStopProfileModeEdit(
      {
        operator_stop_profile: {
          stop_mode: 'soft_on_path',
          deceleration_time_s: 1.0,
          max_stop_time_s: 1.2,
          per_joint_max_deceleration_rad_s2: { joint_1: 2 },
          allow_natural_completion_on_short_path: true,
          allowed_fallback_modes: ['spring_back_hold'],
        },
      },
      OPERATOR_STOP_SLOT,
      'spring_back_hold',
      ['joint_1'],
    );
    const profile = next.operator_stop_profile as Record<string, unknown>;
    expect(profile.profile_id).toBe('operator_spring_back_hold');
    expect(profile.stop_mode).toBe('spring_back_hold');
    expect(profile.max_stop_time_s).toBe(1.2);
    expect(profile.per_joint_max_deceleration_rad_s2).toEqual({ joint_1: 2 });
    expect(profile.allow_natural_completion_on_short_path).toBeUndefined();
    expect(profile.allowed_fallback_modes).toBeUndefined();
  });

  it('edits positive per-joint controlled-stop records and rejects invalid values', () => {
    const initial = { joint_1: 4 };
    expect(applyControlledStopNumberRecordEdit(initial, 'joint_2', '6')).toEqual({
      joint_1: 4,
      joint_2: 6,
    });
    expect(applyControlledStopNumberRecordEdit(initial, 'joint_1', '')).toEqual({});
    expect(applyControlledStopNumberRecordEdit(initial, 'joint_2', '0')).toBe(initial);
    expect(applyControlledStopNumberRecordEdit(initial, 'joint_2', 'abc')).toBe(initial);
  });
});
