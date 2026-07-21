/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import {
  errorPresentationKey,
  headlineForError,
  hintForError,
  incidentEventSummary,
  KernelErrorPanel,
  prettyBodyId,
  prettyPair,
} from './KernelErrorPanel';

describe('KernelErrorPanel mapping', () => {
  describe('errorPresentationKey', () => {
    it('keeps one runtime incident stable across incremental diagnostics', () => {
      const first = errorPresentationKey(
        'Motion execution ended in interrupted',
        'runtime_collision',
        {
          runtime_fault_id: 'runtime-fault-7',
          execution_id: 42,
          lookahead_checked_samples: 8,
        },
      );
      const updated = errorPresentationKey(
        'Motion execution ended in interrupted',
        'runtime_collision',
        {
          runtime_fault_id: 'runtime-fault-7',
          execution_id: 42,
          lookahead_checked_samples: 32,
        },
      );
      const next = errorPresentationKey(
        'Motion execution ended in interrupted',
        'runtime_collision',
        {
          runtime_fault_id: 'runtime-fault-8',
          execution_id: 43,
        },
      );

      expect(updated).toBe(first);
      expect(next).not.toBe(first);
    });

    it('renders a new alert collapsed to its title and alert controls', () => {
      const html = renderToStaticMarkup(
        createElement(KernelErrorPanel, {
          message: 'Test runtime alert',
          errorCode: 'test_runtime_alert',
          errorDetails: { hidden_detail: 'only visible after expansion' },
          presentationKey: 'incident-1',
          onAcknowledge: () => undefined,
          onMinimize: () => undefined,
        }),
      );

      expect(html).toContain('aria-expanded="false"');
      expect(html).toContain('Test runtime alert');
      expect(html).toContain('Acknowledge alert');
      expect(html).toContain('Minimize error to header icon');
      expect(html).not.toContain('only visible after expansion');
    });
  });

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

    it('retains support-contact diagnostics for contact_policy_violation', () => {
      const details = {
        collision_result_first_failure_pair:
          'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
        collision_result_first_failure_classification: 'support_contact_start',
        collision_result_waypoint_index: '0',
        collision_result_raw_contact_release_current_penetration_m: '0.021667',
        collision_result_raw_contact_release_max_start_penetration_m: '0.020000',
        policy_id: 'support-contact-policy',
      };

      expect(
        headlineForError(
          'contact_policy_violation',
          details,
          'Contact policy rejected trajectory',
        ),
      ).toBe(
        'Trajectory rejected: support contact ground ↔ attached graspable at waypoint 0 (current 0.022 m, max 0.020 m)',
      );
      expect(hintForError('contact_policy_violation', details)).toContain(
        '0.022 m, max 0.020 m',
      );
    });

    it('prefers authoritative Precise evidence over contradictory legacy fields', () => {
      const details = {
        collision_result_first_failure_pair:
          'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
        collision_result_waypoint_index: '4',
        policy_mode: 'contact_release',
        release_evidence_mode: 'signed_penetration',
        contact_present: 'true',
        initial_contact_pair_count: '2',
        current_contact_pair_count: '1',
        distance_request_mode: 'selected_colliding_pairs',
        evaluated_distance_pair_count: '1',
        penetration_depth_m: '0.021667',
        max_penetration_m: '0.020000',
        collision_result_raw_contact_release_current_penetration_m: '0.900000',
        collision_result_raw_contact_release_max_start_penetration_m: '0.800000',
      };

      const headline = headlineForError(
        'contact_policy_violation',
        details,
        'Contact policy rejected trajectory',
      );
      expect(headline).toContain('Precise - signed penetration');
      expect(headline).toContain('contacts 2 → 1');
      expect(headline).toContain('penetration 0.022 m');
      expect(headline).toContain('max 0.020 m');
      expect(headline).toContain('distance selected colliding pairs, 1 pair evaluated');
      expect(headline).not.toContain('0.900');
      expect(headline).not.toContain('0.800');
      expect(hintForError('contact_policy_violation', details)).toContain(
        'measures signed penetration only while the configured pair is colliding',
      );
    });

    it('does not present stray Precise metric evidence for an absent pair', () => {
      const details = {
        collision_result_first_failure_pair:
          'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
        policy_mode: 'contact_release',
        release_evidence_mode: 'signed_penetration',
        contact_present: 'false',
        initial_contact_pair_count: '1',
        current_contact_pair_count: '0',
        distance_request_mode: 'selected_colliding_pairs',
        evaluated_distance_pair_count: '0',
        penetration_depth_m: '0.777000',
      };

      const headline = headlineForError(
        'contact_policy_violation',
        details,
        'Contact policy rejected trajectory',
      );
      expect(headline).toContain('Precise - signed penetration');
      expect(headline).toContain('contact absent');
      expect(headline).toContain('0 pairs evaluated');
      expect(headline).not.toContain('penetration 0.777 m');
      expect(headline).not.toContain('clearance');
    });

    it('keeps a colliding Precise pair mode-aware when metric evidence is missing', () => {
      const details = {
        collision_result_engine_status: 'diagnostic_incomplete',
        collision_result_first_failure_pair:
          'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
        collision_result_raw_required_capability: 'penetration_depth',
        policy_mode: 'contact_release',
        release_evidence_mode: 'signed_penetration',
        contact_present: 'true',
        initial_contact_pair_count: '1',
        current_contact_pair_count: '1',
        distance_request_mode: 'selected_colliding_pairs',
        evaluated_distance_pair_count: '0',
      };

      const headline = headlineForError(
        'contact_policy_violation',
        details,
        'Contact policy diagnostics incomplete',
      );
      expect(headline).toContain('Contact-policy diagnostics incomplete for ground ↔ attached graspable');
      expect(headline).toContain('Precise - signed penetration');
      expect(headline).toContain('0 pairs evaluated');
      expect(headline).not.toMatch(/penetration [0-9]/);
      expect(hintForError('contact_policy_violation', details)).toContain(
        'could not produce the required contact-pair evidence',
      );
    });

    it('renders Lightweight set evidence without fabricating penetration', () => {
      const details = {
        collision_result_first_failure_pair:
          'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
        policy_mode: 'contact_release',
        release_evidence_mode: 'binary_contact',
        contact_present: 'false',
        initial_contact_pair_count: '2',
        current_contact_pair_count: '0',
        distance_request_mode: 'none',
        evaluated_distance_pair_count: '0',
        // Adversarial stale fields must never turn binary evidence into a
        // penetration claim.
        penetration_depth_m: '0.250000',
        max_penetration_m: '0.010000',
        collision_result_raw_contact_release_current_penetration_m: '0.500000',
      };

      const headline = headlineForError(
        'contact_policy_violation',
        details,
        'Contact policy rejected trajectory',
      );
      expect(headline).toContain('Lightweight - binary contact');
      expect(headline).toContain('contacts 2 → 0');
      expect(headline).toContain('no distance measurements');
      expect(headline).not.toContain('penetration 0.250 m');
      expect(headline).not.toContain('0.500');
      expect(headline).not.toContain('max 0.010 m');
      expect(hintForError('contact_policy_violation', details)).toContain(
        'does not measure penetration',
      );

      const inconsistent = headlineForError(
        'contact_policy_violation',
        {
          ...details,
          distance_request_mode: 'selected_colliding_pairs',
          evaluated_distance_pair_count: '3',
        },
        'Contact policy rejected trajectory',
      );
      expect(inconsistent).toContain(
        'unexpected distance evidence: selected colliding pairs, 3 pairs evaluated',
      );
      expect(inconsistent).not.toContain('no distance measurements');
    });

    it('renders bounded penetration only for a present configured pair', () => {
      const details = {
        collision_result_first_failure_pair:
          'object/sleeve/shape/0 <-> robot/robot0/attached/insert/shape/0',
        collision_result_waypoint_index: '9',
        policy_mode: 'bounded_penetration',
        contact_present: 'true',
        initial_contact_pair_count: '0',
        current_contact_pair_count: '1',
        distance_request_mode: 'selected_colliding_pairs',
        evaluated_distance_pair_count: '1',
        penetration_depth_m: '0.006000',
        max_contact_penetration_m: '0.005000',
        within_penetration_bound: 'false',
        // Release-only fields are invalid for bounded mode and must not win.
        release_evidence_mode: 'signed_penetration',
        max_penetration_m: '0.900000',
      };

      const headline = headlineForError(
        'contact_policy_violation',
        details,
        'Contact policy rejected trajectory',
      );
      expect(headline).toContain('Contact-tolerant - bounded penetration');
      expect(headline).toContain('contacts 0 → 1');
      expect(headline).toContain('penetration 0.006 m > max 0.005 m');
      expect(headline).not.toContain('0.900');
      expect(hintForError('contact_policy_violation', details)).toContain(
        'measured geometric penetration remains within the authored maximum',
      );

      const absentHeadline = headlineForError(
        'contact_policy_violation',
        {
          ...details,
          contact_present: 'false',
          current_contact_pair_count: '0',
          evaluated_distance_pair_count: '0',
          penetration_depth_m: '0.999000',
        },
        'Contact policy rejected trajectory',
      );
      expect(absentHeadline).toContain('contact absent');
      expect(absentHeadline).toContain('max 0.005 m');
      expect(absentHeadline).not.toContain('penetration 0.999 m');
      expect(absentHeadline).not.toContain('> max');
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

    it('renders a runtime missing-capability fault instead of obstacle guidance', () => {
      const details = {
        reason: 'runtime_collision_required_capability_unsupported',
        reason_code: 'runtime_collision_monitor',
        required_capability: 'penetration_depth',
        // Adversarial unrelated details must not make this look like a stale
        // dynamic obstacle or a geometric collision.
        dynamic_body_id: 'object/fixture/shape/0',
        dynamic_body_age_ns: '900000000',
        dynamic_body_effective_freshness_budget_ns: '100000000',
        collision_result_first_failure_pair:
          'object/fixture/shape/0 <-> robot/robot0/link/wrist_1_link/collision/0',
      };

      expect(
        headlineForError(
          'runtime_collision',
          details,
          'Motion execution ended in interrupted',
        ),
      ).toBe(
        'Runtime collision monitor missing required capability: penetration_depth',
      );
      const hint = hintForError('runtime_collision', details) ?? '';
      expect(hint).toContain('runtime engine does not provide (penetration_depth)');
      expect(hint).toContain('monitor failed closed');
      expect(hint).not.toContain('dynamic body');
      expect(hint).not.toContain('intersected the trajectory');
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

    it('renders validation timeouts as timeout faults rather than pair collisions', () => {
      const details = {
        collision_result_engine_status: 'timeout',
        collision_result_in_collision: 'false',
        collision_result_normalized_pair_count: '0',
        collision_result_raw_snapshot_sync_duration_ns: '72112711',
        collision_result_raw_timeout_budget_ns: '50000000',
        collision_result_raw_timeout_observed_duration_ns: '72165619',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation inconclusive',
        ),
      ).toBe('Pre-dispatch collision validation timed out (72.2 ms > 50.0 ms)');
      const hint = hintForError('validation_collision', details) ?? '';
      expect(hint).toContain('No collision pair was reported');
      expect(hint).toContain('Snapshot sync took 72.1 ms');
      expect(hint).toContain('validation_collision_query_timeout_ms');
    });

    it('renders validation wall-budget timeouts with the wall-budget knob', () => {
      const details = {
        reason: 'collision_validation_wall_timeout',
        collision_result_engine_status: 'timeout',
        collision_result_in_collision: 'false',
        collision_result_normalized_pair_count: '0',
        collision_result_raw_reason_code: 'collision_validation_wall_timeout',
        collision_result_raw_timeout_ns: '5000000000',
        collision_result_raw_elapsed_ns: '5100000000',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation inconclusive',
        ),
      ).toBe(
        'Pre-dispatch collision validation exceeded wall budget (5100 ms > 5000 ms)',
      );
      const hint = hintForError('validation_collision', details) ?? '';
      expect(hint).toContain('whole-validation wall budget');
      expect(hint).toContain('validation_collision_wall_timeout_ms');
      expect(hint).not.toContain('validation_collision_query_timeout_ms');
    });

    it('renders deferred validation distinctly from collision and timeout', () => {
      const details = {
        reason: 'validation_deferred_runtime_monitor_active',
        resource_conflict_reason: 'shared_mutable_context_id',
        operator_guidance:
          'retry validation after active motion/stop completes or configure an independent validation collision resource',
      };

      expect(
        headlineForError(
          'validation_deferred',
          details,
          'Pre-dispatch collision validation deferred',
        ),
      ).toBe(
        'Pre-dispatch validation deferred: runtime collision monitor owns the collision resource (shared_mutable_context_id)',
      );
      expect(hintForError('validation_deferred', details)).toContain(
        'independent validation collision resource',
      );
    });

    it('renders unsupported validation geometry distinctly', () => {
      const details = {
        collision_result_engine_status: 'unsupported_geometry',
        collision_result_raw_snapshot_sync_error:
          'unsupported scene geometry kind for manyforge_collision: object/mesh/shape/0',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation inconclusive',
        ),
      ).toBe(
        'Collision validation unsupported geometry: unsupported scene geometry kind for manyforge_collision: object/mesh/shape/0',
      );
      expect(hintForError('validation_collision', details)).toContain(
        'could not ingest',
      );
    });

    it('renders incomplete validation diagnostics distinctly', () => {
      const details = {
        collision_result_engine_status: 'diagnostic_incomplete',
        collision_result_raw_required_capability: 'penetration_depth',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation inconclusive',
        ),
      ).toBe('Collision validation diagnostics incomplete: penetration_depth');
      expect(hintForError('validation_collision', details)).toContain(
        'required to prove',
      );

      expect(
        headlineForError(
          'validation_collision',
          {
            collision_result_engine_status: 'diagnostic_incomplete',
            required_capability: 'penetration_depth',
            // The direct ErrorDTO capability is authoritative when both
            // shapes are present during a rolling upgrade.
            collision_result_raw_required_capability: 'contact_points',
          },
          'Collision validation inconclusive',
        ),
      ).toBe('Collision validation diagnostics incomplete: penetration_depth');
    });

    it('renders cancelled validation distinctly', () => {
      const details = {
        collision_result_engine_status: 'cancelled',
        cancellation_reason: 'operator_stop',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation cancelled by interrupt',
        ),
      ).toBe('Pre-dispatch collision validation cancelled: operator_stop');
      expect(hintForError('validation_collision', details)).toContain(
        'cancelled by a newer stop',
      );
    });

    it('renders validation engine exceptions distinctly', () => {
      const details = {
        collision_result_engine_status: 'engine_error',
        collision_result_raw_engine_exception: 'coal failure',
      };

      expect(
        headlineForError(
          'validation_collision',
          details,
          'Collision validation inconclusive',
        ),
      ).toBe('Collision validation engine error: coal failure');
      expect(hintForError('validation_collision', details)).toContain(
        'engine threw',
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

    it('uses scene-snapshot lease reason when the top-level code is generic', () => {
      const details = {
        reason_code: 'target_moved_after_validation',
        object_id: 'isaac_cheese_box',
        source_id: 'isaac_scene_objects',
        target_drift_m: '0.0342',
        target_angular_drift_rad: '0.1201',
      };

      expect(
        headlineForError(
          'scene_changed_during_validation',
          details,
          'Collision world changed after validation and before dispatch',
        ),
      ).toBe(
        'Observed target moved after validation: isaac_cheese_box from isaac_scene_objects (0.0342 m, 0.1201 rad)',
      );
      expect(hintForError('scene_changed_during_validation', details)).toContain(
        'safe-point tolerance',
      );
    });

    it('uses details.reason as lease reason when reason_code is absent', () => {
      const details = {
        reason: 'observed_scene_source_stale',
        object_id: 'box',
        source_id: 'foundationpose',
        observed_scene_source_age_ns: '750000000',
        source_freshness_budget_ns: '500000000',
      };

      expect(
        headlineForError(
          'scene_changed_during_validation',
          details,
          'Collision world changed after validation and before dispatch',
        ),
      ).toBe('Observed scene source stale: box from foundationpose (750 ms > 500 ms)');
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
