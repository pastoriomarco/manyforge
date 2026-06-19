import type { RuntimeStatus } from '../types/api';

export const DEFAULT_CONTROLLED_STOP_DECELERATION_TIME_S = 1.0;
export const MAX_CONTROLLED_STOP_DECELERATION_TIME_S = 2.0;
export const DEFAULT_SOFT_ON_PATH_DECELERATION_LIMIT_RAD_S2 = 10.0;
export const DEFAULT_SOFT_ON_PATH_LEAD_IN_S = 0.1;
export const DEFAULT_SOFT_ON_PATH_SAMPLE_PERIOD_S = 0.01;

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
 * Profile selection is deployment-config-driven: the Deployment panel edits the
 * loaded stop profiles while idle/stopped, with backend-unsupported modes
 * disabled by this status-derived view.
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
  operatorDecelerationTimeS: number | null;
  faultDecelerationTimeS: number | null;
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

export function hardStopLevelLabel(level: string | null | undefined): string {
  switch (level) {
    case 'hard_deceleration':
      return 'Profiled hard deceleration (JTC decelerate-on-cancel)';
    case 'jtc_cancel_unprofiled_hold':
      return 'Immediate cancel/hold (unprofiled JTC floor)';
    case 'native_immediate_hold':
      return 'Immediate native hold';
    case 'immediate_hard_stop':
      return 'Immediate hard stop';
    case 'unavailable':
      return 'Unavailable';
    case null:
    case undefined:
    case '':
      return 'Not reported';
    default:
      return level;
  }
}

function stringDetail(
  details: Record<string, unknown> | null | undefined,
  key: string,
): string | null {
  if (details == null) {
    return null;
  }
  const value = details[key];
  return typeof value === 'string' && value.length > 0 ? value : null;
}

function boolDetail(
  details: Record<string, unknown> | null | undefined,
  key: string,
): boolean {
  if (details == null) {
    return false;
  }
  const value = details[key];
  return value === true || value === 'true';
}

export function buildStopExecutionWarning(
  details: Record<string, unknown> | null | undefined,
): string | null {
  const requested = stringDetail(details, 'requested_stop_mode');
  const executed = stringDetail(details, 'executed_stop_mode');
  const reason =
    stringDetail(details, 'fallback_reason') ??
    stringDetail(details, 'controlled_stop_error_reason') ??
    stringDetail(details, 'reason');
  const floor =
    stringDetail(details, 'hard_stop_level') ??
    stringDetail(details, 'hard_stop_floor');
  if (requested && executed && requested !== executed) {
    return (
      `Controlled stop used ${stopModeLabel(executed)} instead of ` +
      `${stopModeLabel(requested)}${reason ? ` — ${reason}` : ''}.`
    );
  }
  if (boolDetail(details, 'hard_stop_transitioned')) {
    const floorText = floor ? ` (${hardStopLevelLabel(floor)})` : '';
    return `Controlled stop escalated to hard stop${floorText}${reason ? ` — ${reason}` : ''}.`;
  }
  return null;
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

function configuredDecelerationTimeForSlot(
  runtime: RuntimeStatus,
  slot: string,
): number | null {
  const configured = runtime.configuredStopDecelerationTimes;
  if (!configured) {
    return null;
  }
  const value = configured[slot];
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : null;
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

function hasUnsupportedReason(runtime: RuntimeStatus, reason: string): boolean {
  const entries = runtime.stopProfileUnsupportedReasons ?? [];
  return entries.some((entry) => {
    if (entry == null || typeof entry !== 'object') {
      return false;
    }
    return (entry as Record<string, unknown>).reason === reason;
  });
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
      operatorDecelerationTimeS: null,
      faultDecelerationTimeS: null,
      options: [],
      queryFailed: false,
      unsupported: false,
    };
  }

  const queryFailed = runtime.stopCapabilityQueryFailed === true;
  const operatorMode = configuredModeForSlot(runtime, OPERATOR_STOP_SLOT);
  const faultMode = configuredModeForSlot(runtime, FAULT_STOP_SLOT);
  const operatorDecelerationTimeS = configuredDecelerationTimeForSlot(
    runtime,
    OPERATOR_STOP_SLOT,
  );
  const faultDecelerationTimeS = configuredDecelerationTimeForSlot(
    runtime,
    FAULT_STOP_SLOT,
  );
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
      operatorDecelerationTimeS,
      faultDecelerationTimeS,
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
    operatorDecelerationTimeS,
    faultDecelerationTimeS,
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
    if (hasUnsupportedReason(runtime, 'stop_profile_required')) {
      return {
        blocked: true,
        severity: 'degraded',
        reason:
          'Run blocked: executable deployments must configure both operator and fault stop profiles.',
      };
    }
    if (hasUnsupportedReason(runtime, 'hard_stop_floor_unavailable')) {
      return {
        blocked: true,
        severity: 'degraded',
        reason:
          'Run blocked: the execution backend did not report a valid hard-stop floor.',
      };
    }
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
 * The slider is frozen while the stop command is in flight, while the backend
 * explicitly reports 'stopping', and for the whole 'stopped' hold — rescaling
 * motion is meaningless/unsafe there. 'idle', 'running' and 'direct_control'
 * keep the slider live unless the local stop latch is active.
 */
export function shouldFreezeVelocitySlider(
  cycleState: string,
  stopInProgress: boolean,
): boolean {
  return stopInProgress || cycleState === 'stopping';
}

/**
 * Whether the transient stop-in-progress latch should clear. Once the cycle
 * leaves the running/stopping/jog states it has settled — to 'stopped' (which
 * freezes the slider on its own) or 'idle' — so the latch is no longer needed.
 */
export function shouldClearStopInProgress(cycleState: string): boolean {
  return cycleState !== 'running' &&
    cycleState !== 'stopping' &&
    cycleState !== 'direct_control';
}
