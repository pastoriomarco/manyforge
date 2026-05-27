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
  /** Stable machine token for HMI mapping, when known. */
  errorCode?: string | null;
  /** Kernel diagnostics map, when known. */
  errorDetails?: Record<string, unknown> | null;
  /** Optional close handler for a dismissible banner. */
  onDismiss?: () => void;
}

interface HeadlineMapping {
  /** Operator-facing one-line cause. May reference errorDetails. */
  headline: (details: Record<string, unknown> | null) => string;
  /** Optional "what to do next" hint shown under the headline. */
  hint?: string;
  /** Severity chip color (Tailwind class fragment). */
  severity?: 'fault' | 'config' | 'state' | 'unknown';
}

const ERROR_CODE_MAPPING: Record<string, HeadlineMapping> = {
  validation_collision: {
    headline: (d) => {
      // Pre-execute validation rejection: the planned trajectory was
      // checked against the scene snapshot before dispatch and failed.
      // Operator action is to fix the scene or re-plan from a different
      // start state.
      const pair = stringField(d, 'collision_result_first_failure_pair');
      const wp = stringField(d, 'collision_result_waypoint_index');
      if (pair && wp) {
        return `Trajectory rejected: ${prettyPair(pair)} at waypoint ${wp}`;
      }
      if (pair) {
        return `Trajectory rejected: ${prettyPair(pair)}`;
      }
      return 'Trajectory rejected: pre-dispatch collision validation failed';
    },
    hint: 'The trajectory failed pre-dispatch validation. Inspect the colliding pair below, adjust the scene or re-plan, then retry.',
    severity: 'fault',
  },
  runtime_collision: {
    headline: (d) => {
      // Runtime monitor caught a collision DURING execution — the
      // trajectory passed pre-validation but a dynamic body appeared
      // (or moved) mid-flight. Operator action is to clear the dynamic
      // body or accept the stop.
      const pair = stringField(d, 'collision_result_first_failure_pair');
      const wp = stringField(d, 'collision_result_waypoint_index');
      if (pair && wp) {
        return `Runtime collision: ${prettyPair(pair)} at waypoint ${wp}`;
      }
      if (pair) {
        return `Runtime collision: ${prettyPair(pair)}`;
      }
      return 'Runtime collision detected during execution';
    },
    hint: 'A dynamic body intersected the trajectory during execution. Inspect the colliding pair below, clear the runtime obstacle (Reset removes runtime-added scene objects), then retry.',
    severity: 'fault',
  },
  motion_interrupt_backend_stop_failed: {
    headline: (d) => {
      const reason = stringField(d, 'reason') || stringField(d, 'reason_code');
      return reason
        ? `Backend stop failed: ${reason}`
        : 'Backend stop failed during motion-scoped interrupt';
    },
    hint: 'Reset the cycle to clear the kernel intervention state.',
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
  start_state_mismatch: {
    headline: () => 'Start-state mismatch: robot is not at the trajectory start',
    hint: 'Move the robot to the expected start state or re-plan from current state.',
    severity: 'state',
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
    'preempt_streaming',
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
    return segments[segments.length - 1];
  };
  return `${short(parts[0])} ↔ ${short(parts[1])}`;
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
  if (!errorCode) {
    return fallbackMessage;
  }
  const mapping = ERROR_CODE_MAPPING[errorCode];
  if (!mapping) {
    return fallbackMessage;
  }
  return mapping.headline(errorDetails ?? null);
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

export function KernelErrorPanel({
  message,
  errorCode,
  errorDetails,
  onDismiss,
}: KernelErrorPanelProps) {
  const [showDetails, setShowDetails] = useState(false);
  const mapping = errorCode ? ERROR_CODE_MAPPING[errorCode] : undefined;
  const severity = mapping?.severity ?? 'unknown';
  const classes = severityClasses(severity);
  const headline = mapping
    ? mapping.headline(errorDetails ?? null)
    : message;
  const subline =
    mapping && headline !== message ? message : undefined;
  const detailEntries = errorDetails
    ? Object.entries(errorDetails).filter(([, v]) => v !== null && v !== undefined)
    : [];
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
            {mapping?.hint && (
              <span className="text-xs text-red-200/90">{mapping.hint}</span>
            )}
          </div>
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
