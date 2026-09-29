import { useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { parseManagerApplyAvailability } from '../api/parsers';
import {
  acceptManagerApplyStatus,
  sameManagerApply,
  selectManagerApplyObservationIdentity,
  type ManagerApplyIdentity,
} from '../state/managerApply';
import type { ManagerApplyStatus } from '../types/api';
import type { HeadlessSourceSection } from '../types/headless';
import type { ControlTransportStatus } from './useHeadlessState';

interface ManagerApplyStatusResult {
  status: ManagerApplyStatus | null;
  synchronizing: boolean;
  /** The operation's status could not be read; it is neither confirmed nor failed. */
  unavailable: boolean;
  reason: string | null;
}

/**
 * Observe one Manager-owned operation when the existing control publication
 * announces a change. There is deliberately no timer or retry loop.
 */
export function useManagerApplyStatus(
  advertised: ManagerApplyIdentity | null,
  manager: HeadlessSourceSection | null,
  transportStatus: ControlTransportStatus,
): ManagerApplyStatusResult {
  const [status, setStatus] = useState<ManagerApplyStatus | null>(null);
  const [synchronizing, setSynchronizing] = useState(advertised !== null);
  const [reason, setReason] = useState<string | null>(null);
  const [unavailable, setUnavailable] = useState(false);
  const observedIdentity = useRef<ManagerApplyIdentity | null>(null);
  const target = selectManagerApplyObservationIdentity(advertised, status);
  const applyId = target?.applyId ?? null;
  const applyGeneration = target?.applyGeneration ?? null;
  const [refresh, setRefresh] = useState(0);
  const reconnectPending = useRef(false);
  const inventory = manager?.payload?.apply;
  const observation = useMemo(() => {
    if (applyId === null || inventory === undefined) return { record: null, error: null };
    try {
      const matches = parseManagerApplyAvailability(inventory, 'manager.payload.apply')
        .records.filter((record) => record.applyId === applyId
          && record.applyGeneration === applyGeneration);
      if (matches.length > 1) throw new Error('Duplicate tracked Manager Apply record.');
      const record = matches[0];
      return {
        record: record === undefined ? null : [
          record.phase, record.terminalState, record.committed, record.incomplete,
        ],
        error: null,
      };
    } catch (error) {
      return {
        record: null,
        error: error instanceof Error ? error.message : 'Invalid Manager Apply inventory.',
      };
    }
  }, [applyId, applyGeneration, inventory]);
  // Owner generation and observation counters are heartbeats, not Apply progress.
  // Only this operation and its source availability wake the exact HTTP read.
  const wakeKey = JSON.stringify([
    manager?.sourceOwnerId, manager?.sourceOwnerEpoch, manager?.available,
    manager?.freshness, manager?.knownGaps, observation,
  ]);

  useEffect(() => {
    if (transportStatus === 'disconnected' || transportStatus === 'connecting') {
      reconnectPending.current = true;
    } else if (transportStatus === 'connected' && reconnectPending.current) {
      reconnectPending.current = false;
      setRefresh((value) => value + 1);
    }
  }, [transportStatus]);

  useEffect(() => {
    if (applyId === null) return undefined;
    const onFocus = () => setRefresh((value) => value + 1);
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, [applyId]);

  useEffect(() => {
    if (applyId === null || applyGeneration === null) return undefined;
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
      if (!sameManagerApply(candidate, identity)) {
        throw new Error('Manager returned status for a different Apply.');
      }
      observedIdentity.current = identity;
      setStatus((current) => (
        current === null || !sameManagerApply(current, identity)
          ? candidate
          : acceptManagerApplyStatus(current, candidate)
      ));
      setSynchronizing(false);
      setUnavailable(false);
      setReason(null);
    }).catch(() => {
      if (controller.signal.aborted) return;
      setSynchronizing(false);
      setUnavailable(true);
      setReason('Deployment transition status is unavailable.');
    });
    return () => controller.abort();
  }, [
    applyGeneration,
    applyId,
    wakeKey,
    refresh,
  ]);

  return {
    status,
    synchronizing,
    unavailable: unavailable || observation.error !== null,
    reason: observation.error === null ? reason
      : `Deployment transition notification is invalid: ${observation.error}`,
  };
}
