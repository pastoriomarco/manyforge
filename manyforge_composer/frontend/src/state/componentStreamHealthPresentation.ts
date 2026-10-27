import type { CCLCComponentSnapshot, CCLCStreamHealth, CCLCStreamSeverity } from '../types/cclc';

/**
 * Wording of the CP9 stream health a CCLC component snapshot carries. CCLC
 * owns every fact (state, latch, fault event); this module only orders and
 * words them. The typed codes are mapped to short labels here and nowhere else.
 */
export type StreamCondition = 'fault_event' | 'acknowledgement_required' | 'stale' | 'absent' | 'live';

const STREAM_CODE_LABELS: Record<string, string> = {
  component_stream_stale: 'stale',
  component_stream_absent: 'absent',
  component_stream_acknowledgement_required: 'needs acknowledgement',
  // Adapter fault events (contract (b)); an unlisted code is shown as issued.
  xarm_joint_value_invalid: 'invalid joint values',
  isaac_simulation_stopped: 'simulation stopped',
};

const STREAM_CODES = new Set([
  'component_stream_stale', 'component_stream_absent', 'component_stream_acknowledgement_required',
]);

const CONDITION_RANK: Record<StreamCondition, number> = {
  fault_event: 0, acknowledgement_required: 1, stale: 2, absent: 3, live: 4,
};
const SEVERITY_RANK: Record<CCLCStreamSeverity, number> = { fault: 0, blocking: 1, warning: 2 };

/** Short label of one typed stream or adapter fault code. */
export function streamCodeLabel(code: string): string {
  return STREAM_CODE_LABELS[code] ?? code;
}

export function streamCondition(stream: CCLCStreamHealth): StreamCondition {
  if (stream.error !== null && !STREAM_CODES.has(stream.error.code)) return 'fault_event';
  if (stream.acknowledgementRequired) return 'acknowledgement_required';
  return stream.state;
}

function conditionLabel(stream: CCLCStreamHealth): string {
  const condition = streamCondition(stream);
  if (condition === 'fault_event') return `fault event: ${streamCodeLabel(stream.error!.code)}`;
  if (condition === 'acknowledgement_required') {
    return streamCodeLabel('component_stream_acknowledgement_required');
  }
  return condition;
}

function duration(ns: number): string {
  return ns < 1_000_000_000 ? `${Math.round(ns / 1_000_000)} ms` : `${(ns / 1_000_000_000).toFixed(1)} s`;
}

/** Worst first: fault events, then not-live streams by severity, then live ones. */
function worstFirst(streams: readonly CCLCStreamHealth[]): CCLCStreamHealth[] {
  const group = (condition: StreamCondition) => (
    condition === 'fault_event' ? 0 : condition === 'live' ? 2 : 1);
  return [...streams].sort((left, right) => {
    const a = streamCondition(left);
    const b = streamCondition(right);
    return group(a) - group(b)
      || SEVERITY_RANK[left.severity] - SEVERITY_RANK[right.severity]
      || CONDITION_RANK[a] - CONDITION_RANK[b];
  });
}

export interface ComponentStreamHealthPresentation {
  /** Compact tile text: the worst stream, or all live. */
  summary: string;
  /** Existing component status-dot class. */
  dotClass: 'status-ready' | 'status-waiting' | 'status-warning' | 'status-fault';
  /** One line per stream, worst first. */
  streams: Array<{ streamId: string; line: string }>;
  /** A latched stream without an open incident: the component Recover clears it. */
  recoverOffered: boolean;
}

/** Null when the component declares no stream. */
export function componentStreamHealthPresentation(
  snapshot: CCLCComponentSnapshot,
): ComponentStreamHealthPresentation | null {
  const health = snapshot.streamHealth;
  if (health === undefined || health.length === 0) return null;
  const ordered = worstFirst(health);
  const worst = ordered[0]!;
  const worstCondition = streamCondition(worst);
  const notLive = ordered.filter((stream) => streamCondition(stream) !== 'live').length;
  const summary = worstCondition === 'live'
    ? `Streams live (${health.length})`
    : `${worst.streamId}: ${conditionLabel(worst)} · ${worst.severity}${notLive > 1 ? ` (+${notLive - 1})` : ''}`;
  const dotClass = worstCondition === 'live' ? 'status-ready'
    : worstCondition === 'fault_event' || worst.severity === 'fault' ? 'status-fault'
      : worst.severity === 'blocking' ? 'status-warning' : 'status-waiting';
  return {
    summary,
    dotClass,
    streams: ordered.map((stream) => ({
      streamId: stream.streamId,
      line: [
        `${stream.streamId}: ${conditionLabel(stream)}`,
        `age ${stream.ageNs === null ? 'none' : duration(stream.ageNs)} / budget ${duration(stream.budgetNs)}`,
        stream.severity,
        `recovery ${stream.recovery}`,
        streamCondition(stream) === 'fault_event' ? stream.error!.message : null,
      ].filter((part): part is string => part !== null).join(' · '),
    })),
    recoverOffered: snapshot.lifecycleProgress?.incident == null
      && health.some((stream) => stream.acknowledgementRequired),
  };
}
