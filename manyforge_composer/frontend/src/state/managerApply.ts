import type {
  ManagerApplyRecord,
  ManagerApplyLifecyclePhase,
  ManagerApplyStatus,
} from '../types/api';

const PHASE_ORDER: Record<ManagerApplyLifecyclePhase, number> = {
  accepted: 0,
  fence_acquired: 1,
  fence_accepted: 2,
  runtime_converged: 3,
  scene_staged: 4,
  scene_activated: 5,
  composed: 6,
  published: 7,
  settled: 8,
};

/**
 * Manager Apply family: only `unknown_effect` leaves it open whether the
 * Apply changed the cell; `failed` and `succeeded` are settled outcomes.
 */
export function applyMayHaveActed(state: ManagerApplyStatus['state'] | null | undefined): boolean {
  return state === 'unknown_effect';
}

export interface ManagerApplyIdentity {
  applyId: string;
  applyGeneration: number;
}

export function selectManagerApplyInventoryIdentity(
  records: ManagerApplyRecord[],
): ManagerApplyIdentity | null {
  const attention = records.filter((record) => (
    record.incomplete
    || record.terminalState === 'failed'
    || record.terminalState === 'unknown_effect'
  ));
  attention.sort((left, right) => (
    Number(right.incomplete) - Number(left.incomplete)
    || right.applyGeneration - left.applyGeneration
    || right.applyId.localeCompare(left.applyId)
  ));
  const selected = attention[0];
  return selected === undefined ? null : {
    applyId: selected.applyId,
    applyGeneration: selected.applyGeneration,
  };
}

export function sameManagerApply(
  left: ManagerApplyIdentity | null,
  right: ManagerApplyIdentity | null,
): boolean {
  return left !== null
    && right !== null
    && left.applyId === right.applyId
    && left.applyGeneration === right.applyGeneration;
}

export function selectManagerApplyObservationIdentity(
  advertised: ManagerApplyIdentity | null,
  current: ManagerApplyStatus | null,
): ManagerApplyIdentity | null {
  if (advertised !== null) {
    return current !== null
      && current.state !== 'pending'
      && sameManagerApply(advertised, current)
      ? null
      : advertised;
  }
  return current?.state === 'pending'
    ? { applyId: current.applyId, applyGeneration: current.applyGeneration }
    : null;
}

/**
 * Retain the newest durable operation fact. Socket reconnects and delayed HTTP
 * responses may replay an older phase; they never move presentation backwards.
 */
export function acceptManagerApplyStatus(
  current: ManagerApplyStatus | null,
  candidate: ManagerApplyStatus,
): ManagerApplyStatus | null {
  if (current === null) return candidate;
  if (!sameManagerApply(current, candidate)) return current;
  if (current.state !== 'pending') return current;
  if (candidate.state !== 'pending') return candidate;
  return PHASE_ORDER[candidate.phase] >= PHASE_ORDER[current.phase]
    ? candidate
    : current;
}

/** Accept only the acknowledgement state from its exact command/readback. */
export function acceptManagerApplyAcknowledgementStatus(
  current: ManagerApplyStatus | null,
  candidate: ManagerApplyStatus,
): ManagerApplyStatus | null {
  const accepted = acceptManagerApplyStatus(current, candidate);
  if (
    current !== null
    && sameManagerApply(current, candidate)
    && current.state === 'failed'
    && candidate.state === 'failed'
    && current.failureAcknowledgement !== 'acknowledged'
    && candidate.failureAcknowledgement === 'acknowledged'
  ) {
    return { ...current, failureAcknowledgement: 'acknowledged' };
  }
  if (
    current !== null
    && sameManagerApply(current, candidate)
    && current.state === 'failed'
    && candidate.state === 'failed'
    && current.failureAcknowledgement === 'available'
    && candidate.failureAcknowledgement === 'unavailable'
  ) {
    return { ...current, failureAcknowledgement: 'unavailable' };
  }
  return accepted;
}

/**
 * The identity of one settled Manager Apply (succeeded, failed or with an
 * unknown effect), or null while none is settled. A settlement is the
 * authoritative event after which the Applied projections are re-read.
 */
export function managerApplySettlementKey(
  status: Pick<ManagerApplyStatus, 'applyId' | 'applyGeneration' | 'state'> | null,
): string | null {
  return status === null || status.state === 'pending'
    ? null
    : `${status.applyId}:${status.applyGeneration}:${status.state}`;
}
