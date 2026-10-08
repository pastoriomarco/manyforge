import type {
  CCLCBlockingCauseClass,
  CCLCComponentIncident,
  CCLCComponentSnapshot,
  CCLCIncidentCauseClass,
  CCLCLifecycleTargetOrigin,
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
  fault: 'Fault — intervention required',
  configuration: 'Configuration mismatch',
  unknown: 'Controller status unknown — intervention required',
  collision: 'Collision detected — intervention required',
  external_control: 'Robot moved from its own interface — intervention required',
  provider_lost: 'Controller lost — motion held',
};

/** Open incidents take precedence over every other message. */
const INCIDENT_MESSAGES: Record<CCLCIncidentCauseClass, string> = {
  emergency_stop: 'Emergency stop — acknowledgement required',
  protective_stop: 'Protective stop — acknowledgement required',
  safeguard_stop: 'Safeguard stop — acknowledgement required',
  fault: 'Fault — acknowledgement required',
  configuration: 'Configuration changed — acknowledgement required',
  unknown: 'Controller status unknown — acknowledgement required',
  collision: 'Collision detected — acknowledgement required',
  external_control: 'Robot moved from its own interface — acknowledgement required',
  provider_lost: 'Controller lost — incident, motion held',
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

function readinessFact(snapshot: CCLCComponentSnapshot): string {
  const progress = snapshot.lifecycleProgress;
  if (progress === null) return 'Not reported by the lifecycle owner';
  return [
    CAUSE_LABELS[progress.blockingCauseClass],
    progress.blockingReason,
    progress.nextIntent === null ? null : `next step: ${progress.nextIntent}`,
  ].filter((part): part is string => part !== null).join(' · ');
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
      message: 'Activation outcome uncertain — checking',
      tone: 'held',
      consequence:
        'Motion is unavailable while the lifecycle owner reconciles the outcome from actual observation.',
    };
  }
  if (
    cause === 'availability'
    && target?.requestedState === 'active'
    && (achieved === 'active' || achieved === 'degraded')
  ) {
    return {
      message: 'Controller lost — motion held',
      tone: 'held',
      consequence:
        'Motion is held. Readiness returning re-admits the component, but interrupted '
        + 'work stays held until a new authorized request.',
    };
  }
  if (progress !== null && progress.phase === 'cancelled') {
    return {
      message: 'Lifecycle target cancelled',
      tone: 'waiting',
      consequence: 'No automatic progression; manual lifecycle requests remain available.',
    };
  }
  if (progress !== null && progress.phase !== 'satisfied') {
    let message = 'Waiting for controller';
    if (progress.delayed) message = 'Startup delayed — still waiting';
    else if (progress.phase === 'transition_in_progress') {
      message = progress.nextIntent === null
        ? 'Lifecycle step in progress'
        : `Lifecycle step in progress: ${progress.nextIntent}`;
    }
    return { message, tone: 'waiting', consequence: WAITING_CONSEQUENCE };
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

/** Existing shared status-pill classes; no lifecycle-specific styling system. */
export function componentLifecycleToneClass(tone: ComponentLifecycleTone): string {
  switch (tone) {
    case 'ready': return 'status-pill status-pill-connected';
    case 'held': return 'status-pill status-pill-danger';
    case 'intervention': return 'status-pill phase-intervention_required';
    default: return 'status-pill';
  }
}
