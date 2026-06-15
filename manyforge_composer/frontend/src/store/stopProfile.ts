import type { RuntimeStatus } from '../types/api';

/**
 * Controlled-stop profile view + run-gate derivation.
 *
 * Pure, status-derived selectors that the transport controls consume to
 * render the stop-profile inspector, gate the Run button, and freeze the
 * velocity slider. They read ONLY the runtime-status capability fields the
 * backend already publishes (supportedStopModes, configuredStopModes,
 * stopProfileUnsupported(+Reasons), backendUnsupportedStopReasons,
 * stopCapabilityQueryFailed); nothing here mutates or extends that contract.
 *
 * Profile selection is deployment-config-driven: the dropdown can update the
 * loaded operator stop profile while idle/stopped, with backend-unsupported
 * modes disabled by this status-derived view.
 */

/**
 * Stop modes ManyForge knows about, in operator-priority order. Mirrors the
 * node-catalog stop_mode enum. The backend advertises which subset it can
 * actually honor via supportedStopModes; any configured mode outside that
 * set is surfaced disabled with the backend's reason.
 */
export const KNOWN_STOP_MODES = [
  'soft_on_path',
  'spring_back_hold',
  'hard_deceleration',
] as const;

const STOP_MODE_LABELS: Record<string, string> = {
  soft_on_path: 'Soft on-path',
  spring_back_hold: 'Spring-back hold',
  hard_deceleration: 'Hard deceleration',
};

/** The deployment slots that carry a configured stop profile. */
export const OPERATOR_STOP_SLOT = 'operator_stop_profile';
export const FAULT_STOP_SLOT = 'default_fault_stop_profile';

export interface StopModeOption {
  mode: string;
  label: string;
  /** Advertised by the backend as honorable. */
  supported: boolean;
  /** Option is non-selectable (the operator must not pick an unhonorable mode). */
  disabled: boolean;
  /** Backend explanation when unsupported, else null. */
  reason: string | null;
  /** Deployment slots configured to this mode (e.g. ['operator_stop_profile']). */
  configuredSlots: string[];
}

export interface StopProfileView {
  /** Whether there is enough capability info to render the inspector at all. */
  available: boolean;
  operatorMode: string | null;
  faultMode: string | null;
  options: StopModeOption[];
  /** Backend could not report its supported modes — fail-closed, all disabled. */
  queryFailed: boolean;
  /** A configured stop profile names a mode the backend cannot honor. */
  unsupported: boolean;
}

export type StopRunGateSeverity = 'fault' | 'degraded';

export interface StopRunGate {
  blocked: boolean;
  severity: StopRunGateSeverity | null;
  reason: string | null;
}

export function stopModeLabel(mode: string): string {
  return STOP_MODE_LABELS[mode] ?? mode;
}

function configuredModeForSlot(
  runtime: RuntimeStatus,
  slot: string,
): string | null {
  const configured = runtime.configuredStopModes;
  if (!configured) {
    return null;
  }
  const value = configured[slot];
  return typeof value === 'string' && value.length > 0 ? value : null;
}

/**
 * Backend-provided reason a stop mode is unsupported. Prefers the per-mode
 * `backendUnsupportedStopReasons` map, then the structured
 * `stopProfileUnsupportedReasons` entries (matched on stop_mode).
 */
function backendReasonForMode(
  runtime: RuntimeStatus,
  mode: string,
): string | null {
  const direct = runtime.backendUnsupportedStopReasons?.[mode];
  if (typeof direct === 'string' && direct.length > 0) {
    return direct;
  }
  const entries = runtime.stopProfileUnsupportedReasons ?? [];
  for (const entry of entries) {
    if (entry && typeof entry === 'object') {
      const record = entry as Record<string, unknown>;
      if (record.stop_mode === mode && typeof record.reason === 'string' && record.reason.length > 0) {
        return record.reason;
      }
    }
  }
  return null;
}

/**
 * Derive the read-only stop-profile inspector model from runtime status.
 * Unsupported (or, when the capability query failed, all) modes are disabled.
 */
export function buildStopProfileView(
  runtime: RuntimeStatus | null,
): StopProfileView {
  if (runtime == null) {
    return {
      available: false,
      operatorMode: null,
      faultMode: null,
      options: [],
      queryFailed: false,
      unsupported: false,
    };
  }

  const queryFailed = runtime.stopCapabilityQueryFailed === true;
  const operatorMode = configuredModeForSlot(runtime, OPERATOR_STOP_SLOT);
  const faultMode = configuredModeForSlot(runtime, FAULT_STOP_SLOT);
  const supportedList = runtime.supportedStopModes ?? null;
  const supported = new Set<string>(supportedList ?? []);

  // The inspector is meaningful only once the backend has reported its
  // capability (or failed to), or a deployment has configured a stop profile.
  const available =
    supportedList != null ||
    queryFailed ||
    (runtime.configuredStopModes != null &&
      Object.keys(runtime.configuredStopModes).length > 0);

  // Nothing to inspect (e.g. composer_only with no execution capability): do
  // not fabricate an all-disabled mode list the UI would never render.
  if (!available) {
    return {
      available: false,
      operatorMode,
      faultMode,
      options: [],
      queryFailed,
      unsupported: runtime.stopProfileUnsupported === true,
    };
  }

  // Union of known modes (stable order) plus any configured/advertised mode
  // not in the known set, so nothing the deployment relies on is ever hidden.
  const ordered: string[] = [...KNOWN_STOP_MODES];
  const extras = new Set<string>();
  for (const mode of supported) {
    extras.add(mode);
  }
  if (operatorMode) extras.add(operatorMode);
  if (faultMode) extras.add(faultMode);
  for (const mode of extras) {
    if (!ordered.includes(mode)) {
      ordered.push(mode);
    }
  }

  const options: StopModeOption[] = ordered.map((mode) => {
    // Fail-closed: if the backend never reported its modes, treat every mode
    // as unsupported rather than implying capability we cannot verify.
    const isSupported = !queryFailed && supported.has(mode);
    const configuredSlots: string[] = [];
    if (operatorMode === mode) configuredSlots.push(OPERATOR_STOP_SLOT);
    if (faultMode === mode) configuredSlots.push(FAULT_STOP_SLOT);
    let reason: string | null = null;
    if (!isSupported) {
      reason =
        backendReasonForMode(runtime, mode) ??
        (queryFailed
          ? 'stop capability could not be verified'
          : 'not supported by backend');
    }
    return {
      mode,
      label: stopModeLabel(mode),
      supported: isSupported,
      disabled: !isSupported,
      reason,
      configuredSlots,
    };
  });

  return {
    available,
    operatorMode,
    faultMode,
    options,
    queryFailed,
    unsupported: runtime.stopProfileUnsupported === true,
  };
}

/**
 * Whether Run should be blocked on stop-capability grounds, with a severity
 * and operator-facing reason. The backend remains authoritative (it will
 * reject arming with a 409); this mirrors the same fail-closed gate in the UI
 * so the operator sees why Run is unavailable instead of a late command error.
 *
 * Scope is intentionally limited to the controlled-stop capability — other
 * canExecute=false causes (e.g. external bring-up) are surfaced by the runtime
 * status overlay and are not duplicated here.
 */
export function evaluateStopRunGate(runtime: RuntimeStatus | null): StopRunGate {
  if (runtime == null) {
    return { blocked: false, severity: null, reason: null };
  }
  if (runtime.stopCapabilityQueryFailed === true) {
    return {
      blocked: true,
      severity: 'fault',
      reason:
        'Run blocked: the execution backend could not report its supported stop modes, ' +
        'so the configured stop profile cannot be verified.',
    };
  }
  if (runtime.stopProfileUnsupported === true) {
    const operatorMode = configuredModeForSlot(runtime, OPERATOR_STOP_SLOT);
    const faultMode = configuredModeForSlot(runtime, FAULT_STOP_SLOT);
    const offending = operatorMode ?? faultMode;
    if (offending == null) {
      // Defensive: backend flagged the profile unsupported but reported no
      // configured slot (a contract violation). Block anyway, without naming
      // a phantom mode.
      return {
        blocked: true,
        severity: 'degraded',
        reason: 'Run blocked: a configured stop profile is not supported by the backend.',
      };
    }
    const backendReason = backendReasonForMode(runtime, offending);
    return {
      blocked: true,
      severity: 'degraded',
      reason:
        `Run blocked: the configured stop profile (${stopModeLabel(offending)}) is not ` +
        `supported by the backend${backendReason ? ` — ${backendReason}` : ''}.`,
    };
  }
  return { blocked: false, severity: null, reason: null };
}

/**
 * Whether the velocity-scale slider must be frozen.
 *
 * The cycle FSM jumps running→stopped synchronously (there is no intermediate
 * 'stopping' state), and the controlled-stop deceleration ramp executes while
 * the cycle is already 'stopped'. So the slider is frozen both while a stop is
 * in flight (operator pressed Stop, state has not yet settled) and for the
 * whole 'stopped' hold — rescaling motion is meaningless/unsafe there. 'idle',
 * 'running' and 'direct_control' keep the slider live.
 */
export function shouldFreezeVelocitySlider(
  cycleState: string,
  stopInProgress: boolean,
): boolean {
  return stopInProgress || cycleState === 'stopped';
}

/**
 * Whether the transient stop-in-progress latch should clear. Once the cycle
 * leaves the running/jog states it has settled — to 'stopped' (which freezes
 * the slider on its own) or 'idle' — so the latch is no longer needed.
 */
export function shouldClearStopInProgress(cycleState: string): boolean {
  return cycleState !== 'running' && cycleState !== 'direct_control';
}
