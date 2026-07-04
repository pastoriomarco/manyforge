/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import {
  headlineForError,
  hintForError,
  incidentEventSummary,
  prettyBodyId,
  prettyPair,
} from './KernelErrorPanel';

describe('KernelErrorPanel mapping', () => {
  describe('prettyPair', () => {
    it('shortens kernel object<->robot token to operator-friendly names', () => {
      expect(
        prettyPair('object/wall/shape/0 <-> robot/robot0/link/left_outer_finger/collision/0'),
      ).toBe('wall ↔ left_outer_finger');
    });

    it('keeps attached object names instead of showing the shape index', () => {
      expect(
        prettyPair('object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0'),
      ).toBe('ground ↔ attached graspable');
    });

    it('keeps unfamiliar tokens intact rather than truncating data', () => {
      // No <-> separator — should round-trip the input untouched.
      expect(prettyPair('object/wall/shape/0')).toBe('object/wall/shape/0');
    });

    it('falls back to last path segment when neither side matches object/robot', () => {
      expect(prettyPair('frame/world <-> sensor/probe/tip')).toBe('world ↔ tip');
    });
  });

  describe('prettyBodyId', () => {
    it('shortens object and robot body ids without dropping unfamiliar ids', () => {
      expect(prettyBodyId('object/isaac_cheese_box/shape/0')).toBe('isaac_cheese_box');
      expect(prettyBodyId('robot/ur10e/link/wrist_3_link/collision/0')).toBe('wrist_3_link');
      expect(prettyBodyId('custom/body/token')).toBe('token');
    });
  });

  describe('headlineForError', () => {
    it('renders the collision pair plus waypoint for validation_collision', () => {
      const headline = headlineForError(
        'validation_collision',
        {
          collision_result_first_failure_pair:
            'object/wall/shape/0 <-> robot/robot0/link/left_outer_finger/collision/0',
          collision_result_waypoint_index: '12',
        },
        'Trajectory rejected: collision validation failed',
      );
      expect(headline).toBe(
        'Trajectory rejected: wall ↔ left_outer_finger at waypoint 12',
      );
    });

    it('renders support-contact policy numbers for validation_collision', () => {
      const headline = headlineForError(
        'validation_collision',
        {
          collision_result_first_failure_pair:
            'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
          collision_result_first_failure_classification: 'support_contact_start',
          collision_result_waypoint_index: '0',
          collision_result_raw_contact_release_current_penetration_m: '0.021667',
          collision_result_raw_contact_release_max_start_penetration_m: '0.020000',
        },
        'Collision validation rejected trajectory',
      );
      expect(headline).toBe(
        'Trajectory rejected: support contact ground ↔ attached graspable at waypoint 0 (current 0.022 m, max 0.020 m)',
      );
      expect(
        hintForError('validation_collision', {
          collision_result_raw_contact_release_current_penetration_m: '0.021667',
          collision_result_raw_contact_release_max_start_penetration_m: '0.020000',
        }),
      ).toContain('0.022 m, max 0.020 m');
    });

    it('renders runtime_collision distinct from validation_collision', () => {
      // The runtime monitor catches a collision DURING execution; pre-
      // execute validation catches it BEFORE dispatch. Both render with
      // the colliding pair but with different operator-facing copy.
      const headline = headlineForError(
        'runtime_collision',
        {
          collision_result_first_failure_pair:
            'object/wall/shape/0 <-> robot/robot0/link/wrist_1_link/collision/0',
          collision_result_waypoint_index: '0',
        },
        'Motion execution ended in interrupted',
      );
      expect(headline).toBe(
        'Runtime collision: wall ↔ wrist_1_link at waypoint 0',
      );
    });

    it('renders stale runtime dynamic bodies as freshness faults, not contact collisions', () => {
      const details = {
        dynamic_body_age_ns: '526186753',
        dynamic_body_effective_freshness_budget_ns: '500000000',
        dynamic_body_id: 'object/isaac_cheese_box/shape/0',
        reason: 'stale_dynamic_collision_body',
        reason_code: 'runtime_collision_monitor',
        surface: 'observed_runtime_monitor',
      };

      expect(
        headlineForError(
          'runtime_collision',
          details,
          'Motion execution ended in interrupted',
        ),
      ).toBe(
        'Dynamic collision-world pose stale: isaac_cheese_box (526 ms > 500 ms)',
      );
      expect(hintForError('runtime_collision', details)).toContain(
        'No geometric collision pair was reported',
      );
    });

    it('falls back gracefully when validation_collision details are missing', () => {
      const headline = headlineForError(
        'validation_collision',
        null,
        'Trajectory rejected: pre-dispatch collision validation failed',
      );
      expect(headline).toBe(
        'Trajectory rejected: pre-dispatch collision validation failed',
      );
    });

    it('uses the reason field for motion_interrupt_backend_stop_failed', () => {
      const headline = headlineForError(
        'motion_interrupt_backend_stop_failed',
        { reason: 'hard_deceleration_not_canceled' },
        'Backend stop failed',
      );
      expect(headline).toBe('Backend stop failed: hard_deceleration_not_canceled');
    });

    it('shows an explicit escalation message for soft_on_path -> hard_deceleration', () => {
      const details = { reason: 'soft_on_path_escalated_to_hard_deceleration' };
      // Not "Backend stop failed: ..." — the stop did not fail, it escalated
      // and the robot is at rest.
      expect(
        headlineForError('motion_interrupt_backend_stop_failed', details, 'Backend stop failed'),
      ).toBe('On-path soft stop infeasible — escalated to a hard stop (robot stopped)');
      const hint = hintForError('motion_interrupt_backend_stop_failed', details);
      expect(hint).toContain('hard deceleration');
      expect(hint).toContain('Run or Reset');
    });

    it('returns the human message when no errorCode mapping exists', () => {
      const headline = headlineForError(
        'totally_made_up_code',
        { reason: 'something' },
        'Generic fallback message',
      );
      expect(headline).toBe('Generic fallback message');
    });

    it('uses backend reason_code when the top-level code is generic', () => {
      const details = {
        reason_code: 'tracking_error_exceeded',
        joint_name: 'shoulder_pan_joint',
        peak_tracking_error_rad: '0.125',
        tracking_error_tolerance_rad: '0.05',
      };

      expect(
        headlineForError(
          'execution_aborted',
          details,
          'Controller tracking error exceeded tolerance: 0.125 rad',
        ),
      ).toBe(
        'Controller tracking error: shoulder_pan_joint 0.125 rad > 0.05 rad',
      );
      expect(hintForError('execution_aborted', details)).toContain(
        're-plan from the current robot state',
      );
    });

    it('renders structured start-state mismatch diagnostics', () => {
      const details = {
        reason_code: 'start_state_mismatch',
        joint_name: 'elbow_joint',
        max_joint_error_rad: '0.031',
        start_tolerance_rad: '0.02',
      };

      expect(
        headlineForError(
          'execution_rejected',
          details,
          'Start state mismatch',
        ),
      ).toBe('Start state mismatch: elbow_joint 0.031 rad > 0.02 rad');
      expect(hintForError('execution_rejected', details)).toContain(
        'current measured state',
      );
    });

    it('returns the human message when errorCode is null (legacy string-only path)', () => {
      const headline = headlineForError(null, null, 'No program loaded');
      expect(headline).toBe('No program loaded');
    });

    it('maps intervention_session_running to a clear restart-blocked headline', () => {
      const headline = headlineForError(
        'intervention_session_running',
        null,
        'Cannot start: intervention required',
      );
      expect(headline).toBe('Kernel is in InterventionRequired');
    });

    it('renders motion_interrupt_latched with the policy and action', () => {
      // The cycle manager synthesizes this code from the kernel event
      // stream when a fault interrupt fires during execution (the live-
      // testing case in Isaac sim that produces "Motion execution ended
      // in interrupted" otherwise).
      const headline = headlineForError(
        'motion_interrupt_latched',
        {
          policy_id: 'manyforge.kernel.runtime_collision_fault.preempt_trajectory',
          preemption_action: 'preempt_trajectory',
          latch_id: 'interrupt-1',
        },
        'Motion execution ended in interrupted',
      );
      expect(headline).toBe(
        'Motion interrupted by runtime_collision_fault (preempt_trajectory)',
      );
    });

    it('falls back to "Motion interrupted by an active fault policy" when policy id is missing', () => {
      const headline = headlineForError(
        'motion_interrupt_latched',
        { preemption_action: 'preempt_trajectory' },
        'Motion execution ended in interrupted',
      );
      expect(headline).toBe('Motion interrupted by an active fault policy');
    });
  });

  describe('incidentEventSummary', () => {
    it('includes node identity fields so duplicate display names stay disambiguated', () => {
      const summary = incidentEventSummary({
        type: 'plan_failed',
        message: 'Planning timed out',
        details: {
          display_name: 'Move',
          action_span_id: 'span-right-move',
          node_instance_id: 'root/parallel/right/move',
          node_attempt_id: 'attempt-right-1',
          reason_code: 'planning_timeout',
        },
      });

      expect(summary).toContain('plan_failed');
      expect(summary).toContain('node_instance_id=root/parallel/right/move');
      expect(summary).toContain('node_attempt_id=attempt-right-1');
      expect(summary).toContain('action_span_id=span-right-move');
    });

    it('includes pick-stage failure reports inside the span-local event line', () => {
      const summary = incidentEventSummary({
        type: 'plan_failed',
        message: 'pick plan failed',
        details: {
          candidate_stage_reports: [
            {
              candidate_index: 1,
              stages: [
                {
                  stage: 'approach',
                  status: 'failure',
                  failures: [{ cause: 'collision_validation_failure' }],
                },
                {
                  stage: 'pick',
                  status: 'pending',
                },
              ],
            },
          ],
        },
      });

      expect(summary).toContain('plan_failed');
      expect(summary).toContain('pick plan failed');
      expect(summary).toContain('candidate 1 approach:collision_validation_failure');
    });
  });
});
