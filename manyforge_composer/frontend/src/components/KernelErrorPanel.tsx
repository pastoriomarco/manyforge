import { useState } from 'react';

/**
 * KernelErrorPanel — renders kernel/cycle-manager errors with their full
 * structured cause instead of a single generic banner.
 *
 * Phase: issues #1 (runtime fault during execution) and #3 (restart blocked
 * because kernel is in InterventionRequired). Both surface a kernel ErrorDTO
 * via the cycle command 409 detail or via the cycle status payload. The
 * panel maps the stable `errorCode` to a human-readable headline plus an
 * optional "what to do" hint, and exposes the full `errorDetails` map in a
 * collapsible diagnostics block so the operator can see exactly what the
 * kernel saw (collision pair, waypoint index, snapshot revision, …).
 *
 * Fallback: when `errorCode` and `errorDetails` are missing (legacy
 * string-only errors, frontend-only validation errors), only the headline
 * message is rendered — the panel is still useful as a structured banner.
 */

export interface KernelErrorPanelProps {
  /** Top-line human-readable message (the cause string). */
  message: string;
  /** Stable machine token for viewer mapping, when known. */
  errorCode?: string | null;
  /** Kernel diagnostics map, when known. */
  errorDetails?: Record<string, unknown> | null;
  /** Captured event snapshot for the latest fault/stop incident, when known. */
  incidentSnapshot?: Record<string, unknown> | null;
  /** Explicit non-reset recovery for retryable stop/fault interventions. */
  onAcknowledgeAndContinue?: () => void | Promise<void>;
  /** Optional close handler for a dismissible banner. */
  onDismiss?: () => void;
}

interface HeadlineMapping {
  /** Operator-facing one-line cause. May reference errorDetails. */
  headline: (details: Record<string, unknown> | null) => string;
  /** Optional "what to do next" hint shown under the headline. */
  hint?: string | ((details: Record<string, unknown> | null) => string);
  /** Severity chip color (Tailwind class fragment). */
  severity?: 'fault' | 'config' | 'state' | 'unknown';
}

const ERROR_CODE_MAPPING: Record<string, HeadlineMapping> = {
  validation_deferred: {
    headline: (d) => {
      const reason = stringField(d, 'reason') || stringField(d, 'reason_code');
      const conflict = stringField(d, 'resource_conflict_reason');
      if (reason === 'validation_deferred_runtime_monitor_active') {
        return conflict
          ? `Pre-dispatch validation deferred: runtime collision monitor owns the collision resource (${conflict})`
          : 'Pre-dispatch validation deferred: runtime collision monitor owns the collision resource';
      }
      return 'Pre-dispatch validation deferred';
    },
    hint: (d) => {
      const guidance = stringField(d, 'operator_guidance');
      return guidance
        || 'Runtime collision monitoring is active, so this validation pass was not allowed to borrow its collision engine. Retry after motion/stop completes or configure an independent validation collision resource.';
    },
    severity: 'state',
  },
  scene_snapshot_lease_missing: {
    headline: () => 'Scene snapshot lease missing at dispatch',
    hint: 'The trajectory could not be tied to a retained validation scene snapshot. Re-plan and retry; if this repeats, inspect the collision snapshot retention diagnostics.',
    severity: 'state',
  },
  live_scene_snapshot_unavailable: {
    headline: () => 'Live collision scene unavailable at dispatch',
    hint: 'The kernel could not read a live collision-scene snapshot to verify the validation lease. Retry after the scene publisher is healthy.',
    severity: 'state',
  },
  scene_snapshot_lease_expired: {
    headline: (d) => {
      const ageMs = numberFieldAsMs(d, 'scene_snapshot_lease_age_ns');
      const budgetMs = numberFieldAsMs(d, 'scene_snapshot_lease_max_age_ns')
        ?? numberFieldAsMs(d, 'lease_max_age_ns');
      if (ageMs && budgetMs) {
        return `Collision validation lease expired before dispatch (${ageMs} ms > ${budgetMs} ms)`;
      }
      if (ageMs) {
        return `Collision validation lease expired before dispatch (${ageMs} ms old)`;
      }
      return 'Collision validation lease expired before dispatch';
    },
    hint: 'The scene changed or dispatch waited too long after validation. Re-plan from a fresh scene snapshot.',
    severity: 'state',
  },
  observed_scene_source_stale: {
    headline: (d) => {
      const source = sceneLeaseActorText(d);
      const ageMs = numberFieldAsMs(d, 'observed_scene_source_age_ns');
      const budgetMs = numberFieldAsMs(d, 'source_freshness_budget_ns')
        ?? numberFieldAsMs(d, 'observed_scene_source_freshness_budget_ns');
      const subject = source ? `: ${source}` : '';
      if (ageMs && budgetMs) {
        return `Observed scene source stale${subject} (${ageMs} ms > ${budgetMs} ms)`;
      }
      return `Observed scene source stale${subject}`;
    },
    hint: 'The pose source behind this collision-scene object was not fresh enough at dispatch. Restore the source stream or wait for a fresh observation, then re-plan.',
    severity: 'state',
  },
  scene_topology_changed_after_validation: {
    headline: (d) => {
      const object = sceneLeaseActorText(d);
      return object
        ? `Scene topology changed after validation: ${object}`
        : 'Scene topology changed after validation';
    },
    hint: 'A collision object was added, removed, attached, detached, or otherwise changed outside the validation lease. Re-plan against the current scene.',
    severity: 'state',
  },
  collision_policy_changed_after_validation: {
    headline: () => 'Collision policy changed after validation',
    hint: 'Allowed-contact or runtime-collision policy changed between validation and dispatch. Re-plan so the trajectory is checked under the active policy.',
    severity: 'state',
  },
  target_moved_after_validation: {
    headline: (d) => {
      const target = sceneLeaseActorText(d);
      const drift = sceneLeaseDriftText(d, 'target');
      return `Observed target moved after validation${target ? `: ${target}` : ''}${drift ? ` (${drift})` : ''}`;
    },
    hint: 'The selected target moved beyond its configured safe-point tolerance after validation. Re-plan from the latest observation.',
    severity: 'fault',
  },
  obstacle_moved_after_validation: {
    headline: (d) => {
      const obstacle = sceneLeaseActorText(d);
      const drift = sceneLeaseDriftText(d, 'obstacle');
      return `Observed obstacle moved after validation${obstacle ? `: ${obstacle}` : ''}${drift ? ` (${drift})` : ''}`;
    },
    hint: 'A live obstacle moved beyond its admitted tolerance before dispatch. Re-plan from the latest scene, or qualify runtime dynamic-obstacle coverage before allowing this drift.',
    severity: 'fault',
  },
  runtime_dynamic_scene_not_qualified: {
    headline: (d) => {
      const object = sceneLeaseActorText(d);
      return object
        ? `Runtime dynamic-obstacle coverage not qualified: ${object}`
        : 'Runtime dynamic-obstacle coverage not qualified';
    },
    hint: 'The object was allowed to move as a runtime dynamic obstacle only if qualified runtime coverage was armed and fresh. The kernel failed closed before dispatch.',
    severity: 'fault',
  },
  scene_snapshot_lease_missing_collision_coverage: {
    headline: () => 'Collision scene changed without lease-covered objects',
    hint: 'The world revision changed after validation, but the lease did not cover any collision objects. Re-plan; if this repeats, inspect the pose-source admission policy for the moving object.',
    severity: 'state',
  },
  validation_collision: {
    headline: (d) => {
      // Pre-execute validation rejection: the planned trajectory was
      // checked against the scene snapshot before dispatch and failed.
      // Engine-status failures are not necessarily geometric collisions:
      // a timeout, unsupported geometry, diagnostic gap, cancellation, or
      // engine exception means the validator failed closed before proving
      // the trajectory dispatchable.
      const engineStatus = stringField(d, 'collision_result_engine_status');
      const reason = stringField(d, 'reason');
      if (engineStatus === 'timeout') {
        const timeoutReason = collisionTimeoutReason(d);
        const observedMs = numberFieldAsMs(
          d,
          'collision_result_raw_timeout_observed_duration_ns',
        )
          ?? numberFieldAsMs(d, 'collision_result_raw_total_call_duration_ns')
          ?? numberFieldAsMs(d, 'collision_result_raw_elapsed_ns');
        const budgetMs = numberFieldAsMs(
          d,
          'collision_result_raw_timeout_budget_ns',
        ) ?? numberFieldAsMs(d, 'collision_result_raw_timeout_ns');
        if (observedMs !== null && budgetMs !== null) {
          if (timeoutReason === 'collision_validation_wall_timeout') {
            return `Pre-dispatch collision validation exceeded wall budget (${observedMs} ms > ${budgetMs} ms)`;
          }
          return `Pre-dispatch collision validation timed out (${observedMs} ms > ${budgetMs} ms)`;
        }
        if (timeoutReason === 'collision_validation_wall_timeout') {
          return 'Pre-dispatch collision validation exceeded wall budget';
        }
        return 'Pre-dispatch collision validation timed out';
      }
      if (engineStatus === 'unsupported_geometry') {
        const reason = stringField(d, 'collision_result_raw_snapshot_sync_error');
        return reason
          ? `Collision validation unsupported geometry: ${reason}`
          : 'Collision validation hit unsupported scene geometry';
      }
      if (engineStatus === 'diagnostic_incomplete') {
        const capability = stringField(d, 'collision_result_raw_required_capability');
        const syncError = stringField(d, 'collision_result_raw_snapshot_sync_error');
        const reason = capability || syncError;
        return reason
          ? `Collision validation diagnostics incomplete: ${reason}`
          : 'Collision validation diagnostics incomplete';
      }
      if (engineStatus === 'cancelled' || reason === 'collision_validation_cancelled') {
        const cancelReason = stringField(d, 'cancellation_reason')
          || stringField(d, 'collision_result_raw_cancel_reason');
        return cancelReason
          ? `Pre-dispatch collision validation cancelled: ${cancelReason}`
          : 'Pre-dispatch collision validation cancelled';
      }
      if (engineStatus === 'engine_error') {
        const exception = stringField(d, 'collision_result_raw_engine_exception');
        return exception
          ? `Collision validation engine error: ${exception}`
          : 'Collision validation engine error';
      }
      const pair = stringField(d, 'collision_result_first_failure_pair');
      const wp = stringField(d, 'collision_result_waypoint_index');
      const classification = stringField(d, 'collision_result_first_failure_classification');
      const contactPolicy = supportContactPolicyText(d);
      const suffix = contactPolicy ? ` (${contactPolicy})` : '';
      if (classification === 'support_contact_start' && pair && wp) {
        return `Trajectory rejected: support contact ${prettyPair(pair)} at waypoint ${wp}${suffix}`;
      }
      if (pair && wp) {
        return `Trajectory rejected: ${prettyPair(pair)} at waypoint ${wp}${suffix}`;
      }
      if (pair) {
        return `Trajectory rejected: ${prettyPair(pair)}${suffix}`;
      }
      if (engineStatus && engineStatus !== 'ok') {
        return `Pre-dispatch collision validation inconclusive: ${engineStatus}`;
      }
      return 'Trajectory rejected: pre-dispatch collision validation failed';
    },
    hint: (d) => {
      const engineStatus = stringField(d, 'collision_result_engine_status');
      const reason = stringField(d, 'reason');
      if (engineStatus === 'timeout') {
        const timeoutReason = collisionTimeoutReason(d);
        const syncMs = numberFieldAsMs(
          d,
          'collision_result_raw_snapshot_sync_duration_ns',
        );
        const syncText = syncMs !== null ? ` Snapshot sync took ${syncMs} ms.` : '';
        if (timeoutReason === 'collision_validation_wall_timeout') {
          return `No collision pair was reported. The validator exceeded its whole-validation wall budget before proving the trajectory safe.${syncText} Increase validation_collision_wall_timeout_ms for this deployment, reduce validation workload, or retry after the validator has a warm snapshot.`;
        }
        return `No collision pair was reported. The validator exceeded its pre-dispatch budget while synchronizing or querying the collision world.${syncText} Increase validation_collision_query_timeout_ms for this deployment, reduce collision-scene complexity, or retry after the validator has a warm snapshot.`;
      }
      if (engineStatus === 'unsupported_geometry') {
        return 'The active collision engine could not ingest one of the scene or robot geometries. Inspect the diagnostic details, simplify or replace the unsupported geometry, then re-plan.';
      }
      if (engineStatus === 'diagnostic_incomplete') {
        return 'The collision engine could not produce the diagnostics required to prove the trajectory safe. Inspect the missing capability or snapshot diagnostic below before retrying.';
      }
      if (engineStatus === 'cancelled' || reason === 'collision_validation_cancelled') {
        return 'Validation was cancelled by a newer stop, reset, or fault path. Retry only after the kernel returns to a stable state.';
      }
      if (engineStatus === 'engine_error') {
        return 'The collision engine threw while validating the trajectory. Inspect the exception and collision-world diagnostics before retrying.';
      }
      const policy = supportContactPolicyText(d);
      if (policy) {
        return `The trajectory failed support-contact validation with ${policy}. Verify the active Support contacts row in Scene, adjust the contact policy or re-plan from a less penetrating pose, then retry.`;
      }
      const pair = stringField(d, 'collision_result_first_failure_pair');
      if (pair) {
        return 'The trajectory failed pre-dispatch validation. Inspect the colliding pair below, adjust the scene or re-plan, then retry.';
      }
      return 'The trajectory failed pre-dispatch validation without a colliding pair. Inspect the engine-status diagnostics below, adjust the scene/configuration, then retry.';
    },
    severity: 'fault',
  },
  runtime_collision: {
    headline: (d) => {
      // Runtime monitor stopped motion DURING execution. This may be a
      // geometric collision, but it may also be a fail-closed monitor fault
      // such as a stale dynamic collision-world body. Keep those distinct in
      // the viewer so operators do not hunt for a non-existent contact pair.
      const reason = stringField(d, 'reason');
      const body = stringField(d, 'dynamic_body_id');
      const ageMs = numberFieldAsMs(d, 'dynamic_body_age_ns');
      const budgetMs = numberFieldAsMs(d, 'dynamic_body_effective_freshness_budget_ns');
      if (reason === 'stale_dynamic_collision_body') {
        const bodyLabel = body ? `: ${prettyBodyId(body)}` : '';
        if (ageMs !== null && budgetMs !== null) {
          return `Dynamic collision-world pose stale${bodyLabel} (${ageMs} ms > ${budgetMs} ms)`;
        }
        return `Dynamic collision-world pose stale${bodyLabel}`;
      }
      if (reason === 'dynamic_body_missing_source_timestamp') {
        return body
          ? `Dynamic collision-world pose missing timestamp: ${prettyBodyId(body)}`
          : 'Dynamic collision-world pose missing timestamp';
      }
      const pair = stringField(d, 'collision_result_first_failure_pair');
      const wp = stringField(d, 'collision_result_waypoint_index');
      if (pair && wp) {
        return `Runtime collision: ${prettyPair(pair)} at waypoint ${wp}`;
      }
      if (pair) {
        return `Runtime collision: ${prettyPair(pair)}`;
      }
      if (reason) {
        return `Runtime collision monitor stopped execution: ${reason}`;
      }
      return 'Runtime collision detected during execution';
    },
    hint: (d) => {
      const reason = stringField(d, 'reason');
      if (
        reason === 'stale_dynamic_collision_body' ||
        reason === 'dynamic_body_missing_source_timestamp'
      ) {
        return 'No geometric collision pair was reported. The runtime monitor stopped before collision query because a dynamic collision-world body was not fresh enough.';
      }
      return 'A dynamic body intersected the trajectory during execution. Inspect the colliding pair below, clear the runtime obstacle (Reset removes runtime-added scene objects), then retry.';
    },
    severity: 'fault',
  },
  tracking_error_exceeded: {
    headline: (d) => {
      const joint = stringField(d, 'joint_name');
      const error = stringField(d, 'peak_tracking_error_rad')
        || stringField(d, 'tracking_error_rad');
      const tolerance = stringField(d, 'tracking_error_tolerance_rad');
      if (joint && error && tolerance) {
        return `Controller tracking error: ${joint} ${error} rad > ${tolerance} rad`;
      }
      if (error && tolerance) {
        return `Controller tracking error exceeded tolerance (${error} rad > ${tolerance} rad)`;
      }
      return 'Controller tracking error exceeded tolerance';
    },
    hint: 'The controller deviated from the commanded trajectory. Inspect the measured joint error, clear the fault, and re-plan from the current robot state.',
    severity: 'fault',
  },
  stale_controller_state: {
    headline: () => 'Controller state was not fresh enough before dispatch',
    hint: 'Check the ROS joint/controller-state publishers and retry after fresh complete state is available.',
    severity: 'state',
  },
  start_state_mismatch: {
    headline: (d) => {
      const joint = stringField(d, 'joint_name');
      const error = stringField(d, 'max_joint_error_rad');
      const tolerance = stringField(d, 'start_tolerance_rad');
      if (joint && error && tolerance) {
        return `Start state mismatch: ${joint} ${error} rad > ${tolerance} rad`;
      }
      if (error && tolerance) {
        return `Start state mismatch exceeded tolerance (${error} rad > ${tolerance} rad)`;
      }
      return 'Start state does not match the trajectory';
    },
    hint: 'The measured robot state does not match the first trajectory point. Re-plan from the current measured state before retrying.',
    severity: 'fault',
  },
  action_server_unavailable: {
    headline: (d) => {
      const action = stringField(d, 'action_name');
      return action
        ? `FollowJointTrajectory action server unavailable: ${action}`
        : 'FollowJointTrajectory action server unavailable';
    },
    hint: 'Check that the ros2_control controller is active and the action name matches the deployment.',
    severity: 'state',
  },
  controller_cancel_timeout: {
    headline: () => 'Controller cancel did not complete before the timeout',
    hint: 'The backend entered operator intervention because software stop completion was not observed. If the robot is stopped and the controller is healthy, acknowledge and continue; otherwise reset when safe.',
    severity: 'fault',
  },
  controller_feedback_timeout: {
    headline: () => 'Controller feedback did not arrive after dispatch',
    hint: 'Check the FollowJointTrajectory feedback stream and controller health before retrying.',
    severity: 'state',
  },
  controller_goal_response_timeout: {
    headline: () => 'Controller did not respond to the trajectory goal',
    hint: 'Check the FollowJointTrajectory action server and controller executor.',
    severity: 'state',
  },
  controller_goal_dispatch_failed: {
    headline: (d) => stringField(d, 'exception') || 'Trajectory goal dispatch failed',
    hint: 'Inspect the dispatch exception and controller action server state.',
    severity: 'fault',
  },
  controller_goal_rejected: {
    headline: () => 'Controller rejected the trajectory goal',
    hint: 'Inspect controller constraints and active controller state before retrying.',
    severity: 'fault',
  },
  controller_result_failed: {
    headline: (d) => stringField(d, 'controller_error') || 'Controller reported trajectory failure',
    hint: 'Inspect the controller result and tracking diagnostics, then re-plan from current state.',
    severity: 'fault',
  },
  empty_trajectory: {
    headline: () => 'Execution rejected an empty trajectory',
    hint: 'This indicates an upstream planning/adapter bug; re-plan and inspect planner output.',
    severity: 'fault',
  },
  monitor_thread_still_shutting_down: {
    headline: () => 'Previous controller monitor thread is still shutting down',
    hint: 'Wait for the backend to settle or reset before dispatching another trajectory.',
    severity: 'state',
  },
  motion_interrupt_backend_stop_failed: {
    headline: (d) => {
      const reason = stringField(d, 'reason') || stringField(d, 'reason_code');
      if (reason === 'soft_on_path_escalated_to_hard_deceleration') {
        // Not a failure: the path-preserving soft stop was infeasible, so the
        // backend escalated to a hard deceleration. The robot is at rest.
        return 'On-path soft stop infeasible — escalated to a hard stop (robot stopped)';
      }
      return reason
        ? `Backend stop failed: ${reason}`
        : 'Backend stop failed during motion-scoped interrupt';
    },
    hint: (d) => {
      const reason = stringField(d, 'reason') || stringField(d, 'reason_code');
      if (reason === 'soft_on_path_escalated_to_hard_deceleration') {
        return 'The path-preserving soft stop could not be produced within the configured stop limits, so the robot was brought to rest on a hard deceleration. Review the stop diagnostics below, then press Run or Reset to clear the kernel intervention and continue.';
      }
      return 'Reset the cycle to clear the kernel intervention state.';
    },
    severity: 'fault',
  },
  motion_interrupt_latched: {
    headline: (d) => {
      // Fault-session interrupt latched (e.g. runtime collision fault).
      // The policy id usually tells the operator which fault category
      // fired, and `preemption_action` says how the kernel responded.
      const policy = stringField(d, 'policy_id');
      const action = stringField(d, 'preemption_action');
      if (policy && action) {
        return `Motion interrupted by ${prettyPolicyId(policy)} (${action})`;
      }
      if (policy) {
        return `Motion interrupted by ${prettyPolicyId(policy)}`;
      }
      return 'Motion interrupted by an active fault policy';
    },
    hint: 'Inspect the latch diagnostics for the cause; click Reset to clear the kernel intervention state.',
    severity: 'fault',
  },
  validation_limits_violation: {
    headline: () => 'Trajectory exceeds joint/TCP limits',
    hint: 'Adjust velocity/acceleration scale or limits and re-plan.',
    severity: 'config',
  },
  planning_failed: {
    headline: (d) => stringField(d, 'reason') || 'Planning failed',
    hint: 'Inspect the planner reason; common causes are unreachable poses or scene collisions.',
    severity: 'fault',
  },
  intervention_session_running: {
    headline: () => 'Kernel is in InterventionRequired',
    hint: 'Clear the intervention before starting again. Click Reset to return to Idle.',
    severity: 'state',
  },
  no_program_loaded: {
    headline: () => 'No program loaded',
    hint: 'Load or apply a program before running.',
    severity: 'state',
  },
  staged_overrides_blocked_resume: {
    headline: () => 'Staged runtime overrides cannot resume from STOPPED',
    hint: 'Reset the cycle first; the staged overrides will be activated on the next Run from IDLE.',
    severity: 'state',
  },
};

const GENERIC_BACKEND_ERROR_CODES = new Set([
  'execution_aborted',
  'execution_rejected',
  'invalid_request',
  'scene_changed_during_validation',
  'stop_failed',
]);

function stringField(
  details: Record<string, unknown> | null | undefined,
  key: string,
): string {
  if (!details) {
    return '';
  }
  const value = details[key];
  return typeof value === 'string' ? value : '';
}

function numberFieldAsMs(
  details: Record<string, unknown> | null | undefined,
  key: string,
): string | null {
  if (!details) {
    return null;
  }
  const value = details[key];
  const raw = typeof value === 'number' ? value : typeof value === 'string' ? Number(value) : NaN;
  if (!Number.isFinite(raw)) {
    return null;
  }
  const ms = raw / 1_000_000;
  if (ms >= 100) {
    return ms.toFixed(0);
  }
  if (ms >= 10) {
    return ms.toFixed(1);
  }
  return ms.toFixed(2);
}

function sceneLeaseActorText(
  details: Record<string, unknown> | null | undefined,
): string {
  const object = stringField(details, 'object_id');
  const source = stringField(details, 'source_id');
  if (object && source) {
    return `${object} from ${source}`;
  }
  return object || source;
}

function sceneLeaseDriftText(
  details: Record<string, unknown> | null | undefined,
  prefix: 'target' | 'obstacle',
): string {
  const linear = numberField(details, `${prefix}_drift_m`);
  const angular = numberField(details, `${prefix}_angular_drift_rad`);
  const parts: string[] = [];
  if (linear !== null) {
    parts.push(`${linear.toFixed(4)} m`);
  }
  if (angular !== null) {
    parts.push(`${angular.toFixed(4)} rad`);
  }
  return parts.join(', ');
}

function collisionTimeoutReason(
  details: Record<string, unknown> | null | undefined,
): string {
  return stringField(details, 'reason')
    || stringField(details, 'cancellation_reason')
    || stringField(details, 'collision_result_raw_reason_code');
}

/**
 * Convert the kernel's verbose policy id into an operator-friendly token.
 * Example: "manyforge.kernel.runtime_collision_fault.preempt_trajectory"
 * -> "runtime_collision_fault".
 */
function prettyPolicyId(policy: string): string {
  const segments = policy.split('.').filter(Boolean);
  if (segments.length === 0) {
    return policy;
  }
  // Drop the kernel/manyforge prefixes; pick the most informative segment.
  // Heuristic: pick the longest segment that doesn't look like an action verb.
  const actionWords = new Set([
    'preempt_trajectory',
    'preempt',
    'pause',
    'fail',
  ]);
  const candidates = segments.filter((s) => !actionWords.has(s));
  if (candidates.length === 0) {
    return segments[segments.length - 1];
  }
  return candidates[candidates.length - 1];
}

/**
 * Convert the kernel's verbose collision-pair token into an operator-friendly
 * short form. Exported so the mapping logic can be unit-tested without
 * rendering the React tree.
 */
export function prettyPair(pair: string): string {
  // Kernel format: "object/wall/shape/0 <-> robot/robot0/link/left_outer_finger/collision/0"
  // Operator-friendly: "wall ↔ left_outer_finger"
  const parts = pair.split(/\s*<->\s*/);
  if (parts.length !== 2) {
    return pair;
  }
  const short = (token: string) => {
    const segments = token.split('/').filter(Boolean);
    if (segments.length === 0) {
      return token;
    }
    if (segments[0] === 'object' && segments.length >= 2) {
      return segments[1];
    }
    if (segments[0] === 'robot' && segments.length >= 4 && segments[2] === 'link') {
      return segments[3];
    }
    if (segments[0] === 'robot' && segments.length >= 4 && segments[2] === 'attached') {
      return `attached ${segments[3]}`;
    }
    return segments[segments.length - 1];
  };
  return `${short(parts[0])} ↔ ${short(parts[1])}`;
}

export function prettyBodyId(bodyId: string): string {
  const segments = bodyId.split('/').filter(Boolean);
  if (segments[0] === 'object' && segments.length >= 2) {
    return segments[1];
  }
  if (segments[0] === 'robot' && segments.length >= 4 && segments[2] === 'link') {
    return segments[3];
  }
  if (segments[0] === 'robot' && segments.length >= 4 && segments[2] === 'attached') {
    return segments[3];
  }
  return segments[segments.length - 1] || bodyId;
}

function numberField(
  details: Record<string, unknown> | null | undefined,
  key: string,
): number | null {
  if (!details) {
    return null;
  }
  const value = details[key];
  const raw = typeof value === 'number' ? value : typeof value === 'string' ? Number(value) : NaN;
  return Number.isFinite(raw) ? raw : null;
}

function supportContactPolicyText(details: Record<string, unknown> | null | undefined): string {
  const current = numberField(details, 'collision_result_raw_contact_release_current_penetration_m');
  const initial = numberField(details, 'collision_result_raw_contact_release_initial_penetration_m');
  const maxStart = numberField(details, 'collision_result_raw_contact_release_max_start_penetration_m');
  const enforce = stringField(details, 'collision_result_raw_contact_release_enforce_max_start_penetration');
  const values: string[] = [];
  if (current !== null) {
    values.push(`current ${current.toFixed(3)} m`);
  } else if (initial !== null) {
    values.push(`initial ${initial.toFixed(3)} m`);
  }
  if (maxStart !== null) {
    values.push(`max ${maxStart.toFixed(3)} m`);
  }
  if (enforce === 'false') {
    values.push('start cap disabled');
  }
  return values.join(', ');
}

function mappingForError(
  errorCode: string | null | undefined,
  errorDetails: Record<string, unknown> | null | undefined,
): HeadlineMapping | undefined {
  const reasonCode =
    stringField(errorDetails, 'reason_code') || stringField(errorDetails, 'reason');
  if (
    reasonCode
    && ERROR_CODE_MAPPING[reasonCode]
    && (!errorCode || GENERIC_BACKEND_ERROR_CODES.has(errorCode))
  ) {
    return ERROR_CODE_MAPPING[reasonCode];
  }
  if (errorCode && ERROR_CODE_MAPPING[errorCode]) {
    return ERROR_CODE_MAPPING[errorCode];
  }
  return reasonCode ? ERROR_CODE_MAPPING[reasonCode] : undefined;
}

/**
 * Resolve an (errorCode, errorDetails, fallbackMessage) tuple to the
 * operator-facing headline. Exported so the mapping can be unit-tested.
 */
export function headlineForError(
  errorCode: string | null | undefined,
  errorDetails: Record<string, unknown> | null | undefined,
  fallbackMessage: string,
): string {
  const mapping = mappingForError(errorCode, errorDetails);
  if (!mapping) {
    return fallbackMessage;
  }
  return mapping.headline(errorDetails ?? null);
}

export function hintForError(
  errorCode: string | null | undefined,
  errorDetails: Record<string, unknown> | null | undefined,
): string | undefined {
  const mapping = mappingForError(errorCode, errorDetails);
  const hint = mapping?.hint;
  if (!hint) {
    return undefined;
  }
  return typeof hint === 'function' ? hint(errorDetails ?? null) : hint;
}

function severityClasses(severity: HeadlineMapping['severity']): {
  border: string;
  chipBg: string;
  chipText: string;
} {
  switch (severity) {
    case 'fault':
      return { border: 'border-red-700 bg-red-950/60', chipBg: 'bg-red-800', chipText: 'text-red-100' };
    case 'config':
      return { border: 'border-yellow-700 bg-yellow-950/60', chipBg: 'bg-yellow-800', chipText: 'text-yellow-100' };
    case 'state':
      return { border: 'border-amber-700 bg-amber-950/60', chipBg: 'bg-amber-800', chipText: 'text-amber-100' };
    default:
      return { border: 'border-red-700 bg-red-950/60', chipBg: 'bg-gray-700', chipText: 'text-gray-100' };
  }
}

const STOP_CHAIN_KEYS = [
  'requested_stop_mode',
  'executed_stop_mode',
  'configured_stop_mode',
  'fallback_pair',
  'fallback_reason',
  'hard_stop_floor',
  'hard_stop_level',
  'hard_stop_floor_command_source',
  'command_source',
  'stop_command_source',
  'phase',
  'reason_code',
  'reason',
  'controlled_stop_error_reason',
  'wait_s',
  'max_stop_time_s',
  'stop_completion_timeout_s',
  'deceleration_time_s',
];

const INCIDENT_DETAIL_KEYS = [
  'action_span_id',
  'node_instance_id',
  'node_attempt_id',
  'parent_node_attempt_id',
  'move_id',
  'contact_policy_id',
  'requested_stop_mode',
  'executed_stop_mode',
  'fallback_pair',
  'fallback_reason',
  'hard_stop_floor',
  'hard_stop_level',
  'hard_stop_floor_command_source',
  'controlled_stop_error_reason',
  'reason_code',
  'reason',
  'phase',
  'command_source',
  'stop_command_source',
  'emergency_fallback',
  'hard_stop_transitioned',
];

function hasStopChain(details: Record<string, unknown> | null | undefined): boolean {
  if (!details) {
    return false;
  }
  return STOP_CHAIN_KEYS.some((key) => details[key] !== undefined && details[key] !== null);
}

function isRetryableStopIntervention(details: Record<string, unknown> | null | undefined): boolean {
  if (!details) {
    return false;
  }
  const reason = stringField(details, 'reason') || stringField(details, 'reason_code');
  if (
    reason === 'stop_not_settled_measured_velocity' ||
    reason === 'operator_stop_total_failure' ||
    reason === 'in_stop_completion_timeout' ||
    reason === 'stop_path_collision' ||
    reason === 'hard_deceleration_not_settled' ||
    reason === 'controller_cancel_timeout'
  ) {
    return true;
  }
  const phase = stringField(details, 'phase');
  return (
    (phase === 'operator_stop' ||
      phase === 'motion_trip_stop' ||
      phase === 'motion_trip_stop_path_collision') &&
    (stringField(details, 'requested_stop_mode') !== null ||
      stringField(details, 'executed_stop_mode') !== null ||
      stringField(details, 'hard_stop_transitioned') !== null)
  );
}

function stopChainRows(details: Record<string, unknown>): Array<[string, string]> {
  const requested =
    stringField(details, 'requested_stop_mode') ||
    stringField(details, 'configured_stop_mode');
  const executed = stringField(details, 'executed_stop_mode');
  const fallbackPair = stringField(details, 'fallback_pair');
  const fallbackReason = stringField(details, 'fallback_reason');
  const command =
    stringField(details, 'stop_command_source') ||
    stringField(details, 'command_source') ||
    stringField(details, 'surface');
  const hardStopFloor = stringField(details, 'hard_stop_floor');
  const hardStopLevel = stringField(details, 'hard_stop_level');
  const hardStopCommand =
    stringField(details, 'hard_stop_floor_command_source');
  const phase = stringField(details, 'phase');
  const reason =
    stringField(details, 'controlled_stop_error_reason') ||
    stringField(details, 'reason_code') ||
    stringField(details, 'reason');
  const timing = [
    stringField(details, 'deceleration_time_s')
      ? `decel ${stringField(details, 'deceleration_time_s')}s`
      : '',
    stringField(details, 'wait_s') ? `wait ${stringField(details, 'wait_s')}s` : '',
    stringField(details, 'max_stop_time_s')
      ? `max ${stringField(details, 'max_stop_time_s')}s`
      : '',
    stringField(details, 'stop_completion_timeout_s')
      ? `completion/cancel tolerance ${stringField(details, 'stop_completion_timeout_s')}s`
      : '',
  ].filter(Boolean).join(', ');

  const rows: Array<[string, string]> = [];
  if (requested) {
    rows.push(['Requested', requested]);
  }
  if (executed) {
    rows.push(['Executed', executed]);
  }
  if (fallbackPair || fallbackReason) {
    rows.push(['Fallback', [fallbackPair, fallbackReason].filter(Boolean).join(' — ')]);
  }
  if (hardStopFloor || hardStopLevel || hardStopCommand) {
    rows.push([
      'Hard stop floor',
      [hardStopFloor, hardStopLevel, hardStopCommand].filter(Boolean).join(' — '),
    ]);
  }
  if (command) {
    rows.push(['Backend action', command]);
  }
  if (phase) {
    rows.push(['Phase', phase]);
  }
  if (reason) {
    rows.push(['Reason', reason]);
  }
  if (timing) {
    rows.push(['Timing', timing]);
  }
  return rows;
}

function StopChainSummary({
  details,
}: {
  details: Record<string, unknown>;
}) {
  const rows = stopChainRows(details);
  if (rows.length === 0) {
    return null;
  }
  return (
    <div className="mt-2 rounded border border-red-800/70 bg-black/25 p-2 text-xs text-red-100/90">
      <div className="mb-1 font-semibold text-red-100">Stop chain</div>
      <div className="grid gap-1 sm:grid-cols-[7rem_1fr]">
        {rows.map(([label, value]) => (
          <div key={label} className="contents">
            <span className="text-red-300/80">{label}</span>
            <span className="break-words font-mono">{value}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

function objectField(
  details: Record<string, unknown> | null | undefined,
  key: string,
): Record<string, unknown> | null {
  if (!details) {
    return null;
  }
  const value = details[key];
  return value && typeof value === 'object' && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : null;
}

function objectArrayField(
  details: Record<string, unknown> | null | undefined,
  key: string,
): Array<Record<string, unknown>> {
  if (!details) {
    return [];
  }
  const value = details[key];
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter(
    (item): item is Record<string, unknown> =>
      item !== null && typeof item === 'object' && !Array.isArray(item),
  );
}

function incidentEventDetails(event: Record<string, unknown>): string {
  const details = objectField(event, 'details');
  if (!details) {
    return '';
  }
  return INCIDENT_DETAIL_KEYS
    .map((key) => {
      const value = details[key];
      if (value === undefined || value === null || value === '') {
        return '';
      }
      return `${key}=${String(value)}`;
    })
    .filter(Boolean)
    .join('  ');
}

function stageReportSummaries(event: Record<string, unknown>): string[] {
  const details = objectField(event, 'details');
  if (!details) {
    return [];
  }
  const reports = objectArrayField(details, 'candidate_stage_reports');
  const single = objectField(details, 'candidate_stage_report');
  const selectedReports = reports.length > 0 ? reports : single ? [single] : [];
  return selectedReports.slice(-3).map((report) => {
    const candidateValue =
      report.candidate_index ??
      report.candidate_id ??
      report.candidate;
    const candidate =
      candidateValue !== undefined && candidateValue !== null
        ? String(candidateValue)
        : '';
    const fallbackCandidate =
      stringField(report, 'candidate_index') ||
      stringField(report, 'candidate_id') ||
      stringField(report, 'candidate');
    const stages = objectArrayField(report, 'stages');
    const failures = stages
      .filter((stage) => stringField(stage, 'status') === 'failure')
      .map((stage) => {
        const stageName = stringField(stage, 'stage') || 'stage';
        const failuresList = objectArrayField(stage, 'failures');
        const firstFailure = failuresList[0] ?? null;
        const cause =
          stringField(firstFailure, 'cause') ||
          stringField(stage, 'cause') ||
          stringField(stage, 'collision_check') ||
          'failed';
        return `${stageName}:${cause}`;
      });
    const prefix = candidate || fallbackCandidate
      ? `candidate ${candidate || fallbackCandidate}`
      : 'candidate';
    return failures.length > 0
      ? `${prefix} ${failures.join(', ')}`
      : `${prefix} stage report`;
  });
}

export function incidentEventSummary(event: Record<string, unknown>): string {
  const type = stringField(event, 'type') || stringField(event, 'event') || 'event';
  const message = stringField(event, 'message');
  const details = incidentEventDetails(event);
  const stageReports = stageReportSummaries(event);
  const stageText = stageReports.length > 0 ? `stages: ${stageReports.join(' ; ')}` : '';
  return [type, message, details, stageText].filter(Boolean).join(' | ');
}

function IncidentSnapshotSummary({
  snapshot,
}: {
  snapshot: Record<string, unknown> | null | undefined;
}) {
  if (!snapshot) {
    return null;
  }
  const allEvents = objectArrayField(snapshot, 'allEvents');
  const spanEvents = objectArrayField(snapshot, 'spanEvents');
  const eventsByChannel = objectField(snapshot, 'eventsByChannel');
  const faultEvents = objectArrayField(eventsByChannel, 'fault_safety_stop');
  const spanScope = objectField(snapshot, 'spanScope');
  const headlineCause = objectField(snapshot, 'headlineCause');
  const headlineSummary = stringField(headlineCause, 'summary');
  const scopeLabel =
    stringField(spanScope, 'label') ||
    'Span-local chain scoped by strongest available identity';
  const timeline = allEvents.slice(-16);
  const triggerType = stringField(snapshot, 'triggerType');
  const triggerChannel = stringField(snapshot, 'triggerChannel');
  const triggerSeverity = stringField(snapshot, 'triggerSeverity');
  if (timeline.length === 0 && faultEvents.length === 0 && spanEvents.length === 0) {
    return null;
  }
  return (
    <details className="mt-2 rounded border border-red-800/70 bg-black/25 p-2 text-xs text-red-100/90">
      <summary className="cursor-pointer font-semibold text-red-100">
        Captured incident chain
        {triggerType ? `: ${triggerType}` : ''}
        {triggerChannel ? ` (${triggerChannel}${triggerSeverity ? `, ${triggerSeverity}` : ''})` : ''}
      </summary>
      {headlineSummary && (
        <div className="mt-2 rounded border border-red-900/70 bg-red-950/30 p-2">
          <div className="text-red-300/80">Headline cause</div>
          <div className="break-words font-mono">{headlineSummary}</div>
        </div>
      )}
      {spanEvents.length > 0 && (
        <div className="mt-2">
          <div className="mb-1 text-red-300/80">{scopeLabel}</div>
          <ol className="space-y-1">
            {spanEvents.slice(-14).map((event, index) => (
              <li
                key={`${stringField(event, 'event_index') || stringField(event, 'sequence') || index}`}
                className="break-words font-mono"
              >
                {incidentEventSummary(event)}
              </li>
            ))}
          </ol>
        </div>
      )}
      {faultEvents.length > 0 && (
        <div className="mt-2">
          <div className="mb-1 text-red-300/80">Stop/fault context channel</div>
          <ol className="space-y-1">
            {faultEvents.slice(-12).map((event, index) => (
              <li
                key={`${stringField(event, 'event_index') || stringField(event, 'sequence') || index}`}
                className="break-words font-mono"
              >
                {incidentEventSummary(event)}
              </li>
            ))}
          </ol>
        </div>
      )}
      {timeline.length > 0 && (
        <div className="mt-2">
          <div className="mb-1 text-red-300/80">All-events context window</div>
          <ol className="space-y-1">
            {timeline.map((event, index) => (
              <li
                key={`${stringField(event, 'event_index') || stringField(event, 'sequence') || index}`}
                className="break-words font-mono"
              >
                {incidentEventSummary(event)}
              </li>
            ))}
          </ol>
        </div>
      )}
    </details>
  );
}

export function KernelErrorPanel({
  message,
  errorCode,
  errorDetails,
  incidentSnapshot,
  onAcknowledgeAndContinue,
  onDismiss,
}: KernelErrorPanelProps) {
  const [showDetails, setShowDetails] = useState(false);
  const mapping = mappingForError(errorCode, errorDetails);
  const severity = mapping?.severity ?? 'unknown';
  const classes = severityClasses(severity);
  const headline = mapping
    ? mapping.headline(errorDetails ?? null)
    : message;
  const hint = hintForError(errorCode, errorDetails);
  const subline =
    mapping && headline !== message ? message : undefined;
  const detailEntries = errorDetails
    ? Object.entries(errorDetails).filter(([, v]) => v !== null && v !== undefined)
    : [];
  const showStopChain = hasStopChain(errorDetails);
  const canAcknowledgeAndContinue =
    Boolean(onAcknowledgeAndContinue) && isRetryableStopIntervention(errorDetails);
  return (
    <div className={`mb-2 rounded border px-3 py-2 text-sm text-red-200 ${classes.border}`}>
      <div className="flex items-start justify-between gap-2">
        <div className="flex-1">
          <div className="font-semibold text-red-100">{headline}</div>
          {subline && (
            <div className="mt-0.5 break-words text-xs text-red-300/90">{subline}</div>
          )}
          <div className="mt-1 flex flex-wrap items-center gap-1.5">
            {errorCode && (
              <span
                className={`rounded px-1.5 py-0.5 text-xs font-mono ${classes.chipBg} ${classes.chipText}`}
                title="kernel error code"
              >
                {errorCode}
              </span>
            )}
            {hint && (
              <span className="text-xs text-red-200/90">{hint}</span>
            )}
          </div>
          {showStopChain && errorDetails && (
            <StopChainSummary details={errorDetails} />
          )}
          <IncidentSnapshotSummary snapshot={incidentSnapshot} />
          {canAcknowledgeAndContinue && (
            <div className="mt-2 flex flex-wrap gap-2">
              <button
                type="button"
                onClick={() => void onAcknowledgeAndContinue?.()}
                className="rounded border border-red-300/50 bg-red-900/40 px-2.5 py-1 text-xs font-semibold text-red-100 hover:bg-red-800/60"
                title="Clear this classified stop/fault intervention without resetting the backend. The kernel will refuse if fresh backend state is unreadable or still moving."
              >
                Acknowledge error
              </button>
            </div>
          )}
          {detailEntries.length > 0 && (
            <div className="mt-2">
              <button
                type="button"
                onClick={() => setShowDetails((v) => !v)}
                className="text-xs underline text-red-100/80 hover:text-red-100"
              >
                {showDetails ? 'Hide diagnostic details' : `Show diagnostic details (${detailEntries.length})`}
              </button>
              {showDetails && (
                <div className="mt-1 max-h-48 overflow-auto rounded bg-black/40 p-2 font-mono text-[11px] text-red-100/90">
                  {detailEntries.map(([k, v]) => (
                    <div key={k} className="flex gap-2">
                      <span className="text-red-300/80">{k}:</span>
                      <span className="break-all">{String(v)}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}
        </div>
        {onDismiss && (
          <button
            type="button"
            onClick={onDismiss}
            className="text-red-200/80 hover:text-red-100"
            aria-label="Dismiss error"
          >
            ✕
          </button>
        )}
      </div>
    </div>
  );
}
