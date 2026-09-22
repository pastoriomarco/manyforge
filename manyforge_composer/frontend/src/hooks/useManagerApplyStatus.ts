import { useEffect, useRef, useState } from 'react';

import { api } from '../api/client';
import {
  acceptManagerApplyStatus,
  sameManagerApply,
  selectManagerApplyObservationIdentity,
  type ManagerApplyIdentity,
} from '../state/managerApply';
import type { ManagerApplyStatus } from '../types/api';

interface ManagerApplyStatusResult {
  status: ManagerApplyStatus | null;
  synchronizing: boolean;
  reason: string | null;
}

/**
 * Observe one Manager-owned operation when the existing control publication
 * announces a change. There is deliberately no timer or retry loop.
 */
export function useManagerApplyStatus(
  advertised: ManagerApplyIdentity | null,
  snapshotSequence: number | null,
): ManagerApplyStatusResult {
  const [status, setStatus] = useState<ManagerApplyStatus | null>(null);
  const [synchronizing, setSynchronizing] = useState(advertised !== null);
  const [reason, setReason] = useState<string | null>(null);
  const observedIdentity = useRef<ManagerApplyIdentity | null>(null);
  const target = selectManagerApplyObservationIdentity(advertised, status);
  const applyId = target?.applyId ?? null;
  const applyGeneration = target?.applyGeneration ?? null;

  useEffect(() => {
    if (applyId === null || applyGeneration === null
        || snapshotSequence === null) return undefined;
    const identity = { applyId, applyGeneration };
    const firstObservation = !sameManagerApply(observedIdentity.current, identity);
    const controller = new AbortController();
    if (firstObservation) {
      setSynchronizing(true);
      setReason('Deployment transition status is synchronizing.');
    }
    void api.manager.applyStatus(
      identity.applyId,
      identity.applyGeneration,
      controller.signal,
    ).then((candidate) => {
      if (controller.signal.aborted) return;
      observedIdentity.current = identity;
      setStatus((current) => (
        current === null || !sameManagerApply(current, identity)
          ? candidate
          : acceptManagerApplyStatus(current, candidate)
      ));
      setSynchronizing(false);
      setReason(null);
    }).catch(() => {
      if (controller.signal.aborted) return;
      setSynchronizing(false);
      setReason('Deployment transition status is unavailable.');
    });
    return () => controller.abort();
  }, [
    applyGeneration,
    applyId,
    snapshotSequence,
  ]);

  return { status, synchronizing, reason };
}
