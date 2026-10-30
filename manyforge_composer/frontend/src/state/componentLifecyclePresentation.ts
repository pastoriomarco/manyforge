import type {
  CCLCBlockingCauseClass,
  CCLCComponentIncident,
  CCLCComponentReadiness,
  CCLCComponentSnapshot,
  CCLCIncidentCauseClass,
  CCLCLifecycleFactValue,
  CCLCLifecycleTargetOrigin,
  CCLCReadinessProviderOccurrence,
  CCLCTransactionStatus,
} from '../types/cclc';

/**
 * Presentation of one CCLC-issued component snapshot: requested target,
 * achieved lifecycle state, live readiness/blocking cause and the consequence
 * for motion. CCLC owns every fact; this module only words them. It holds no
 * state, starts no timer and never decides progression (the owner's `delayed`
 * flag is the only "slower than expected" signal).
 */
export type ComponentLifecycleTone = 'ready' | 'waiting' | 'held' | 'intervention';

export interface ComponentLifecyclePresentation {
  message: string;
  tone: ComponentLifecycleTone;
  target: string;
  achieved: string;
  readiness: string;
  consequence: string;
  /** Open incident facts (summary · cause · vendor codes), or null. */
  incident: string | null;
}

const INTERVENTION_MESSAGES: Partial<Record<CCLCBlockingCauseClass, string>> = {
  emergency_stop: 'Emergency stop engaged',
  protective_stop: 'Protective stop engaged',
  safeguard_stop: 'Safeguard stop engaged',
  fault: 'Fault · intervention required',
  configuration: 'Configuration mismatch',
  unknown: 'Controller status unknown · intervention required',
  collision: 'Collision detected · intervention required',
  external_control: 'Robot moved from its own interface · intervention required',
  provider_lost: 'Controller lost · motion held',
};

/** Open incidents take precedence over every other message. */
const INCIDENT_MESSAGES: Record<CCLCIncidentCauseClass, string> = {
  emergency_stop: 'Emergency stop · acknowledgement required',
  protective_stop: 'Protective stop · acknowledgement required',
  safeguard_stop: 'Safeguard stop · acknowledgement required',
  fault: 'Fault · acknowledgement required',
  configuration: 'Configuration changed · acknowledgement required',
  unknown: 'Controller status unknown · acknowledgement required',
  collision: 'Collision detected · acknowledgement required',
  external_control: 'Robot moved from its own interface · acknowledgement required',
  provider_lost: 'Controller lost · incident, motion held',
};

const CAUSE_LABELS: Record<CCLCBlockingCauseClass, string> = {
  none: 'no blocking cause reported',
  availability: 'controller unavailable or not yet ready',
  emergency_stop: 'emergency stop engaged',
  protective_stop: 'protective stop engaged',
  safeguard_stop: 'safeguard stop engaged',
  fault: 'controller fault',
  configuration: 'configuration mismatch',
  unknown: 'controller status cannot be established',
  collision: 'collision detected',
  external_control: 'robot moved or changed mode from its own interface',
  provider_lost: 'controller or provider lost after it was ready',
};

const ORIGIN_LABELS: Record<CCLCLifecycleTargetOrigin, string> = {
  startup: 'startup configuration',
  operator: 'operator',
  program: 'Program',
};

const INTERVENTION_CONSEQUENCE =
  'Motion is blocked. ManyForge keeps observing and continues only after the '
  + 'required reset is established; it never clears faults or enables drives on its own.';
const WAITING_CONSEQUENCE =
  'Motion is unavailable until the component reaches its requested target.';
const INCIDENT_CONSEQUENCE =
  'Motion is held and the cell is latched. Recover cell acknowledges the incident; '
  + 'ManyForge then performs only the recovery steps its policy permits. Motion never '
  + 'resumes by itself.';

/** Cause label of one incident cause class. */
export function incidentCauseLabel(cause: CCLCIncidentCauseClass): string {
  return CAUSE_LABELS[cause];
}

/** Summary · cause · vendor codes of one open incident, as owner-issued. */
export function incidentFacts(incident: CCLCComponentIncident): string {
  return [
    incident.summary,
    `cause: ${CAUSE_LABELS[incident.causeClass]}`,
    incident.vendorCodes.length === 0
      ? null
      : `vendor codes: ${incident.vendorCodes.join(', ')}`,
  ].filter((part): part is string => part !== null).join(' · ');
}

function targetFact(snapshot: CCLCComponentSnapshot): string {
  const target = snapshot.lifecycleTarget;
  if (target === null) return 'No requested target';
  return `${target.requestedState} (requested by ${ORIGIN_LABELS[target.origin]}, `
    + `target ${target.targetGeneration})`;
}

/** Preserve the actual owner ErrorDTO; this formatting adds no classification. */
export function readinessErrorPresentation(error: CCLCTransactionStatus['error']): string | null {
  if (error === null) return null;
  return [`[${error.code}] ${error.message}`, error.details.reason, error.details.decisive_fact]
    .filter((part): part is string => typeof part === 'string').join(' · ');
}

/**
 * Not-ready wording: the first provider-domain health diagnostic names the
 * actual blocker (e.g. a missing ROS service); the owner error follows unchanged.
 */
export function readinessBlockerPresentation(
  readiness: Pick<CCLCComponentReadiness, 'error' | 'providerOccurrences'>,
): string | null {
  const diagnostic = readiness.providerOccurrences
    .find((domain) => domain.health?.diagnostic?.message)?.health?.diagnostic?.message;
  const parts = [diagnostic, readinessErrorPresentation(readiness.error)]
    .filter((part): part is string => typeof part === 'string' && part !== '');
  return parts.length === 0 ? null : parts.join(' · ');
}

function targetWaitFact(snapshot: CCLCComponentSnapshot): string | null {
  if (snapshot.readiness.ready) return null;
  // Nothing waits while closed admission only holds an already Active component.
  if (snapshot.lifecycleState === 'active' && lifecycleRequestsClosed(snapshot)) return null;
  const progress = snapshot.lifecycleProgress;
  const start = progress?.waitingSinceNs;
  if (progress?.phase === 'satisfied' || progress?.phase === 'cancelled' || start === null || start === undefined) return 'Target wait duration unavailable';
  if (snapshot.readiness.evaluatedAtNs === undefined) return 'Target wait duration unavailable';
  const evaluated = BigInt(snapshot.readiness.evaluatedAtNs);
  const waiting = BigInt(start);
  if (evaluated < waiting) return 'Target wait duration unavailable';
  const milliseconds = (evaluated - waiting) / 1_000_000n;
  return `Target wait: ${milliseconds / 1000n}.${String(milliseconds % 1000n).padStart(3, '0')} s at last owner evaluation`;
}

/**
 * The owner refuses lifecycle requests because cell admission is closed (for
 * example while a Program runs). That says nothing about the component's own
 * health, so it never makes an Active component "Not ready".
 */
function lifecycleRequestsClosed(snapshot: CCLCComponentSnapshot): boolean {
  return !snapshot.readiness.ready && snapshot.readiness.error?.code === 'authoring_not_ready'
    && readinessSubBlocker(snapshot) === null;
}

/**
 * A real readiness failure the owner also returned. The owner's top-level
 * error keeps only its first refusal, so closed admission can sit in front of
 * a provider or measured-state failure; that failure still decides.
 */
function readinessSubBlocker(snapshot: CCLCComponentSnapshot): CCLCTransactionStatus['error'] {
  const readiness = snapshot.readiness;
  const provider = readiness.providerOccurrences.find((domain) => domain.error !== null)?.error;
  if (provider) return provider;
  if (readiness.measuredState.error !== null) return readiness.measuredState.error;
  if (readiness.measuredState.required === true && readiness.measuredState.available !== true) {
    return { code: 'measured_state_unavailable', message: 'Required measured state is unavailable', details: {} };
  }
  return null;
}

function readinessFact(snapshot: CCLCComponentSnapshot): string {
  const readiness = snapshot.readiness;
  const subBlocker = readiness.ready || readiness.error?.code !== 'authoring_not_ready'
    ? null : readinessSubBlocker(snapshot);
  return [
    readiness.ready ? 'Ready' : lifecycleRequestsClosed(snapshot) ? 'Lifecycle changes closed' : 'Not ready',
    // The real failure first; closed admission is only the owner's first refusal.
    readinessErrorPresentation(subBlocker),
    readinessErrorPresentation(readiness.error),
    readiness.nextIntent === null ? null : `next step: ${readiness.nextIntent}`,
    targetWaitFact(snapshot),
  ].filter((part): part is string => typeof part === 'string').join(' · ');
}

function verdict(
  snapshot: CCLCComponentSnapshot,
): Pick<ComponentLifecyclePresentation, 'message' | 'tone' | 'consequence'> {
  const target = snapshot.lifecycleTarget;
  const progress = snapshot.lifecycleProgress;
  const achieved = snapshot.lifecycleState;
  const cause = progress?.blockingCauseClass ?? 'none';
  const incident = progress?.incident ?? null;
  if (incident !== null) {
    return {
      message: INCIDENT_MESSAGES[incident.causeClass],
      tone: 'intervention',
      consequence: INCIDENT_CONSEQUENCE,
    };
  }
  const intervention = INTERVENTION_MESSAGES[cause];
  if (intervention !== undefined) {
    return { message: intervention, tone: 'intervention', consequence: INTERVENTION_CONSEQUENCE };
  }
  if (target?.requestedState === 'disabled' && target.origin === 'operator') {
    return {
      message: 'Disabled by operator',
      tone: 'waiting',
      consequence: 'Commands stay closed until an operator requests Prepare or Enable.',
    };
  }
  if (progress?.phase === 'outcome_uncertain') {
    return {
      message: 'Activation outcome uncertain · checking',
      tone: 'held',
      consequence:
        'Motion is unavailable while the lifecycle owner reconciles the outcome from actual observation.',
    };
  }
  if (achieved === 'faulted' || snapshot.readiness.providerOccurrences.some((domain) => domain.health?.observedState === 'faulted')) {
    return { message: 'Fault · intervention required', tone: 'intervention', consequence: INTERVENTION_CONSEQUENCE };
  }
  if (
    cause === 'availability'
    && target?.requestedState === 'active'
    && (achieved === 'active' || achieved === 'degraded')
  ) {
    return {
      message: 'Controller lost · motion held',
      tone: 'held',
      consequence:
        'Motion is held. Readiness returning re-admits the component, but interrupted '
        + 'work stays held until a new authorized request.',
    };
  }
  if (progress !== null && progress.phase === 'cancelled') {
    const retired = progress.blockingReason?.startsWith('target_retired:') === true;
    const reason = retired ? progress.blockingReason!.slice('target_retired:'.length).trim() : '';
    return {
      message: retired ? 'Target retired' : 'Lifecycle target cancelled',
      tone: 'waiting',
      consequence: `${reason ? `${reason}. ` : ''}No automatic progression; manual lifecycle requests remain available.`,
    };
  }
  if (progress !== null && progress.phase !== 'satisfied') {
    let message = 'Waiting for controller';
    if (progress.delayed) message = 'Startup delayed · still waiting';
    else if (progress.phase === 'transition_in_progress') {
      message = progress.nextIntent === null
        ? 'Lifecycle step in progress'
        : `Lifecycle step in progress: ${progress.nextIntent}`;
    }
    return { message, tone: 'waiting', consequence: WAITING_CONSEQUENCE };
  }
  if (!snapshot.readiness.ready && !(achieved === 'active' && lifecycleRequestsClosed(snapshot))) {
    return { message: 'Not ready', tone: 'waiting', consequence: WAITING_CONSEQUENCE };
  }
  if (achieved === 'active') {
    return {
      message: 'Ready',
      tone: 'ready',
      consequence: 'No lifecycle hold; motion still requires its own admission.',
    };
  }
  return {
    message: `Component ${achieved}`,
    tone: 'waiting',
    consequence: 'Motion is unavailable until the component is active.',
  };
}

export function componentLifecyclePresentation(
  snapshot: CCLCComponentSnapshot,
): ComponentLifecyclePresentation {
  return {
    ...verdict(snapshot),
    target: targetFact(snapshot),
    achieved: `${snapshot.lifecycleState} · generation ${snapshot.lifecycleGeneration}`,
    readiness: readinessFact(snapshot),
    incident: snapshot.lifecycleProgress?.incident
      ? incidentFacts(snapshot.lifecycleProgress.incident)
      : null,
  };
}

/** Read-only wording of the selected domain; raw health/classification is retained. */
export function componentReadinessOccurrencePresentation(domain: CCLCReadinessProviderOccurrence) {
  const facts = domain.health?.facts ?? [];
  if (domain.hasActivationStep !== false) {
    return { state: domain.health?.observedState ?? 'health unavailable', facts };
  }
  const state = domain.health === null ? 'No activation step · health unavailable'
    : domain.health.observedState === 'faulted' ? 'No activation step · faulted'
      : 'No activation step';
  return { state, facts: facts.filter((fact) => ![
    'external.controller_loaded', 'external.controller_configuration_ready',
    'external.controller_inactive_unrequested',
  ].includes(fact.lifecycleFactId)) };
}

/** Existing shared status-pill classes; no lifecycle-specific styling system. */
export function componentLifecycleToneClass(tone: ComponentLifecycleTone): string {
  switch (tone) {
    case 'ready': return 'status-pill status-pill-connected';
    case 'held': return 'status-pill status-pill-danger';
    case 'intervention': return 'status-pill phase-intervention_required';
    default: return 'status-pill';
  }
}

const TONE_DOT: Record<ComponentLifecycleTone, string> = {
  ready: 'status-ready',
  waiting: 'status-waiting',
  held: 'status-warning',
  intervention: 'status-fault',
};

/** Existing `component-dot` status class of a tone. */
export function componentLifecycleDotClass(tone: ComponentLifecycleTone): string {
  return TONE_DOT[tone];
}

/** The health badge of one component row: dot, lifecycle state and verdict. */
export function componentHealthBadge(
  snapshot: CCLCComponentSnapshot | null,
  readError: string | null,
): { dotClass: string; state: string; message: string } {
  if (snapshot === null) {
    return {
      dotClass: 'status-offline',
      state: 'unknown',
      message: readError ?? 'Current component health has not been read.',
    };
  }
  const presentation = componentLifecyclePresentation(snapshot);
  return {
    dotClass: TONE_DOT[presentation.tone],
    state: snapshot.lifecycleState,
    message: presentation.message,
  };
}

/**
 * The device facts a vendor driver reports through CCLC (any brand that uses
 * these fact ids), labelled for the component page. Vendor codes are copied
 * verbatim by the driver (for example error `C2`, `mode=0`).
 */
const VENDOR_FACT_LABELS: ReadonlyArray<readonly [string, string]> = [
  ['manyforge.vendor_code.error', 'Error code'],
  ['manyforge.vendor_code.warning', 'Warning code'],
  ['manyforge.vendor_code.mode', 'Mode'],
  ['manyforge.vendor_code.state', 'State'],
  ['manyforge.vendor_code.condition', 'Condition'],
  ['manyforge.vendor_code.power_cycle', 'Power cycle'],
  ['device.error_present', 'Error present'],
  ['device.warning_present', 'Warning present'],
  ['device.motion_enabled', 'Motors enabled'],
  ['device.accepts_commands', 'Accepts commands'],
  ['device.moved_externally', 'Moved externally'],
  ['device.in_motion', 'In motion'],
];

function vendorFactValue(value: CCLCLifecycleFactValue): string {
  switch (value.kind) {
    case 'unknown': return 'unknown';
    case 'boolean': return value.booleanValue ? 'yes' : 'no';
    case 'integer': return String(value.integerValue);
    default: return value.tokenValue.replaceAll('_', ' ');
  }
}

/**
 * Vendor status of one component, in a fixed order: the reported vendor and
 * device facts, then the owner's blocking reason (for example
 * `moved_externally:rearm_at_next_motion`). Empty when nothing is reported.
 */
export function vendorStatusFacts(snapshot: CCLCComponentSnapshot): Array<{ label: string; value: string }> {
  const facts = snapshot.readiness.providerOccurrences.flatMap((domain) => domain.health?.facts ?? []);
  const rows = VENDOR_FACT_LABELS.flatMap(([factId, label]) => {
    const fact = facts.find((candidate) => candidate.lifecycleFactId === factId);
    return fact === undefined ? [] : [{ label, value: vendorFactValue(fact.value) }];
  });
  const reason = snapshot.lifecycleProgress?.blockingReason ?? null;
  return reason === null || reason === '' ? rows
    : [...rows, { label: 'Blocking reason', value: reason.replace(':', ' · ').replaceAll('_', ' ') }];
}
