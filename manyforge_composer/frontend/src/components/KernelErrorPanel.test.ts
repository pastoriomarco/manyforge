/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import { headlineForError, prettyPair } from './KernelErrorPanel';

describe('KernelErrorPanel mapping', () => {
  describe('prettyPair', () => {
    it('shortens kernel object<->robot token to operator-friendly names', () => {
      expect(
        prettyPair('object/wall/shape/0 <-> robot/robot0/link/left_outer_finger/collision/0'),
      ).toBe('wall ↔ left_outer_finger');
    });

    it('keeps unfamiliar tokens intact rather than truncating data', () => {
      // No <-> separator — should round-trip the input untouched.
      expect(prettyPair('object/wall/shape/0')).toBe('object/wall/shape/0');
    });

    it('falls back to last path segment when neither side matches object/robot', () => {
      expect(prettyPair('frame/world <-> sensor/probe/tip')).toBe('world ↔ tip');
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

    it('returns the human message when no errorCode mapping exists', () => {
      const headline = headlineForError(
        'totally_made_up_code',
        { reason: 'something' },
        'Generic fallback message',
      );
      expect(headline).toBe('Generic fallback message');
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
});
