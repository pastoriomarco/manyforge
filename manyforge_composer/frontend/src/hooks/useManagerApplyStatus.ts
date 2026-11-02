import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import { parseManagerApplyAvailability } from '../api/parsers';
import { commandRefusalText } from '../state/commandRefusal';
import {
  acceptManagerApplyAcknowledgementStatus,
  acceptManagerApplyStatus,
  sameManagerApply,
  selectManagerApplyObservationIdentity,
  type ManagerApplyIdentity,
} from '../state/managerApply';
import type { ManagerApplyStatus } from '../types/api';
import type { HeadlessSourceSection } from '../types/headless';
import type { ControlTransportStatus } from './useHeadlessState';
import { browserRequestIdentity } from '../utils/requestIdentity';

interface ManagerApplyStatusResult {
  status: ManagerApplyStatus | null;
  synchronizing: boolean;
  /** The operation's status could not be read; it is neither confirmed nor failed. */
  unavailable: boolean;
  reason: string | null;
  /**
   * Send the durable acknowledgement while the Manager offers it and no
   * earlier attempt's outcome is unknown; otherwise only re-read the exact
   * status (never resend: the Manager permits one attempt). `noFailedApply: true`
   * is positive evidence that no failed Apply remains for this identity.
   */
  acknowledgeFailure: () => Promise<{
    confirmed: boolean; message: string | null; noFailedApply: boolean;
  }>;
  /** An acknowledgement was sent and its outcome is unknown (lost reply). */
  acknowledgementOutcomeUnknown: boolean;
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
  const [acknowledgementResolutionIdentity, setAcknowledgementResolutionIdentity] =
    useState<ManagerApplyIdentity | null>(null);
  const observedIdentity = useRef<ManagerApplyIdentity | null>(null);
  // An unavailable status after a proven refusal can be refreshed back to
  // available; after a lost reply it cannot prove the earlier POST finished.
  const resolutionAllowsAvailable = useRef(false);
  // The Apply whose acknowledgement attempt has an unknown outcome. The
  // Manager keeps that attempt's row incomplete and refuses any resend.
  const [outcomeUnknownIdentity, setOutcomeUnknownIdentity] =
    useState<ManagerApplyIdentity | null>(null);
  const target = selectManagerApplyObservationIdentity(advertised, status);
  const applyId = target?.applyId ?? null;
  const applyGeneration = target?.applyGeneration ?? null;
  const [refresh, setRefresh] = useState(0);
  const readStatus = useCallback((identity: ManagerApplyIdentity, signal?: AbortSignal) => (
    api.manager.applyStatus(identity.applyId, identity.applyGeneration, signal)
  ), []);
  const refreshStatus = useCallback(async (
    identity?: ManagerApplyIdentity,
    afterAcknowledgement = false,
  ): Promise<ManagerApplyStatus | null> => {
    const selected = identity ?? (status === null ? null : {
      applyId: status.applyId,
      applyGeneration: status.applyGeneration,
    });
    if (selected === null) return null;
    try {
      const candidate = await readStatus(selected);
      if (!sameManagerApply(candidate, selected)) {
        throw new Error('Manager returned status for a different Apply.');
      }
      setStatus((current) => (
        current === null || !sameManagerApply(current, selected)
          ? current
          : afterAcknowledgement
            ? acceptManagerApplyAcknowledgementStatus(current, candidate)
            : acceptManagerApplyStatus(current, candidate)
      ));
      return candidate;
    } catch {
      return null;
    }
  }, [readStatus, status]);
  const acknowledgeFailure = useCallback(async () => {
    const current = status;
    if (current?.state !== 'failed') {
      return { confirmed: false, noFailedApply: false, message: null };
    }
    const identity = { applyId: current.applyId, applyGeneration: current.applyGeneration };
    const outcomeUnknown = sameManagerApply(outcomeUnknownIdentity, identity);
    if (current.failureAcknowledgement !== 'available' || outcomeUnknown) {
      // Only re-read: hide only on positive evidence, never resend.
      let candidate: ManagerApplyStatus;
      try {
        candidate = await readStatus(identity);
        if (!sameManagerApply(candidate, identity)) {
          throw new Error('Manager returned status for a different Apply.');
        }
      } catch (error) {
        if (error instanceof ApiError && error.structuredError?.code === 'apply_status_not_found') {
          return { confirmed: false, noFailedApply: true, message: null };
        }
        return {
          confirmed: false,
          noFailedApply: false,
          message: 'The Apply status could not be re-read. Nothing was sent; the notice stays.',
        };
      }
      if (candidate.state !== 'failed') return { confirmed: false, noFailedApply: true, message: null };
      if (candidate.failureAcknowledgement === 'acknowledged') {
        setStatus((observed) => acceptManagerApplyAcknowledgementStatus(observed, candidate));
        setAcknowledgementResolutionIdentity((observed) => (
          sameManagerApply(observed, identity) ? null : observed
        ));
        return { confirmed: true, noFailedApply: false, message: null };
      }
      if (!outcomeUnknown && candidate.failureAcknowledgement === 'available') {
        // A fresh exact read offers it again (no attempt is pending): the next
        // Acknowledge sends it.
        setStatus((observed) => (
          observed !== null && sameManagerApply(observed, identity) && observed.state === 'failed'
            ? { ...observed, failureAcknowledgement: 'available' }
            : observed
        ));
        setAcknowledgementResolutionIdentity((observed) => (
          sameManagerApply(observed, identity) ? null : observed
        ));
        resolutionAllowsAvailable.current = false;
        return { confirmed: false, noFailedApply: false, message: 'Manager offers acknowledgement again. Nothing was sent yet.' };
      }
      return {
        confirmed: false,
        noFailedApply: false,
        message: outcomeUnknown
          ? 'The acknowledgement outcome is still unknown. The Manager permits no second attempt, so nothing was resent.'
          : 'Manager reports acknowledgement unavailable. That does not prove the Apply holds nothing, so the notice stays. Nothing was sent.',
      };
    }
    let confirmed = false;
    let message: string | null = null;
    let postOutcomeUnknown = false;
    let preAttemptRefusal = false;
    let postStatus: ManagerApplyStatus | null = null;
    try {
      const candidate = await api.manager.acknowledgeApply(
        current.applyId,
        current.applyGeneration,
        {
          request_id: browserRequestIdentity('composer-manager-apply-ack'),
          source_label: 'manyforge_composer',
        },
      );
      if (!sameManagerApply(candidate, current) || candidate.state !== 'failed') {
        throw new Error('Manager did not return the same failed Apply status.');
      }
      postStatus = candidate;
      setStatus((observed) => acceptManagerApplyAcknowledgementStatus(observed, candidate));
      if (candidate.failureAcknowledgement === 'available') {
        message = 'Manager still reports acknowledgement available. The failed Apply remains recorded; no request was retried.';
      } else if (candidate.failureAcknowledgement === 'acknowledged') {
        confirmed = true;
      } else {
        message = 'Manager reports acknowledgement unavailable. The failed Apply remains active; this status does not confirm acknowledgement.';
      }
    } catch (error) {
      preAttemptRefusal = error instanceof ApiError
        && error.structuredError?.details?.effect === 'none';
      postOutcomeUnknown = !preAttemptRefusal;
      const reason = error instanceof ApiError && error.structuredError !== null
        ? commandRefusalText(error.structuredError)
        : error instanceof Error ? error.message : 'Manager response was unavailable';
      message = preAttemptRefusal
        ? `Acknowledgement was refused (${reason}). The failed Apply remains recorded; no request was retried.`
        : `Acknowledgement was not confirmed (${reason}). The failed Apply remains recorded; no request was retried.`;
    }
    const acknowledgementIdentity = {
      applyId: current.applyId,
      applyGeneration: current.applyGeneration,
    };
    const refreshed = await refreshStatus(acknowledgementIdentity, true);
    if (refreshed?.failureAcknowledgement === 'acknowledged') {
      confirmed = true;
      message = null;
      setAcknowledgementResolutionIdentity((observed) => (
        sameManagerApply(observed, acknowledgementIdentity) ? null : observed
      ));
    } else if (
      postOutcomeUnknown
      || postStatus?.failureAcknowledgement === 'unavailable'
      || refreshed?.failureAcknowledgement === 'unavailable'
      || (preAttemptRefusal && refreshed === null)
    ) {
      setStatus((observed) => {
        if (observed === null
          || !sameManagerApply(observed, acknowledgementIdentity)
          || observed.state !== 'failed'
          || observed.failureAcknowledgement === 'acknowledged'
          || observed.failureAcknowledgement === 'unavailable') return observed;
        return { ...observed, failureAcknowledgement: 'unavailable' };
      });
      setAcknowledgementResolutionIdentity((observed) => (
        observed === null || sameManagerApply(observed, acknowledgementIdentity)
          ? acknowledgementIdentity
          : observed
      ));
      resolutionAllowsAvailable.current = preAttemptRefusal;
      if (postOutcomeUnknown) setOutcomeUnknownIdentity(acknowledgementIdentity);
      if (postOutcomeUnknown && refreshed?.failureAcknowledgement === 'available') {
        message += ' The exact status still reports available, but it does not establish that the earlier request has finished. The Manager permits no second attempt; Acknowledge only re-reads the status.';
      } else if (postOutcomeUnknown && refreshed?.failureAcknowledgement === 'unavailable') {
        message += ' Manager reports acknowledgement unavailable; the failed Apply remains active.';
      } else if (postOutcomeUnknown) {
        message += ' The failed Apply remains active. The Manager permits no second attempt; Acknowledge only re-reads the status.';
      } else if (refreshed?.failureAcknowledgement === 'unavailable') {
        message = 'Manager reports acknowledgement unavailable. The failed Apply remains active; this status does not confirm acknowledgement.';
      }
    }
    if (refreshed === null) {
      message = confirmed
        ? 'Manager returned acknowledgement status, but the follow-up Apply status refresh failed.'
        : `${message ?? 'Acknowledgement was not confirmed.'} Follow-up Apply status refresh failed.`;
    }
    return { confirmed, message, noFailedApply: false };
  }, [outcomeUnknownIdentity, readStatus, refreshStatus, status]);
  const reconnectPending = useRef(false);
  const inventory = manager?.payload?.apply;
  const observedApplyId = applyId ?? acknowledgementResolutionIdentity?.applyId ?? null;
  const observedApplyGeneration = applyId === null
    ? acknowledgementResolutionIdentity?.applyGeneration ?? null : applyGeneration;
  const observation = useMemo(() => {
    if (observedApplyId === null || inventory === undefined) return { record: null, error: null };
    try {
      const matches = parseManagerApplyAvailability(inventory, 'manager.payload.apply')
        .records.filter((record) => record.applyId === observedApplyId
          && record.applyGeneration === observedApplyGeneration);
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
  }, [observedApplyId, observedApplyGeneration, inventory]);
  // Owner generation and observation counters are heartbeats, not Apply progress.
  // Only this operation and its source availability wake the exact HTTP read.
  const wakeKey = JSON.stringify([
    manager?.sourceOwnerId, manager?.sourceOwnerEpoch, manager?.available,
    manager?.freshness, manager?.knownGaps, observation,
  ]);

  useEffect(() => {
    if (acknowledgementResolutionIdentity !== null && status !== null
      && !sameManagerApply(status, acknowledgementResolutionIdentity)) {
      setAcknowledgementResolutionIdentity(null);
      resolutionAllowsAvailable.current = false;
    }
  }, [acknowledgementResolutionIdentity, status]);

  const resolveAcknowledgement = useCallback((identity: ManagerApplyIdentity, signal: AbortSignal) => {
    void readStatus(identity, signal).then((candidate) => {
        if (signal.aborted || !sameManagerApply(candidate, identity)) return;
        const mayOffer = resolutionAllowsAvailable.current
          && candidate.state === 'failed' && candidate.failureAcknowledgement === 'available';
        setStatus((current) => {
          if (current === null || !sameManagerApply(current, identity)) return current;
          if (mayOffer && current.state === 'failed'
            && current.failureAcknowledgement !== 'acknowledged') {
            return { ...current, failureAcknowledgement: 'available' };
          }
          return acceptManagerApplyAcknowledgementStatus(current, candidate);
        });
        if (mayOffer || candidate.failureAcknowledgement === 'acknowledged') {
          setAcknowledgementResolutionIdentity((current) => (
            sameManagerApply(current, identity) ? null : current
          ));
          resolutionAllowsAvailable.current = false;
        }
    }).catch(() => {});
  }, [readStatus]);

  useEffect(() => {
    if (acknowledgementResolutionIdentity === null) return undefined;
    const identity = acknowledgementResolutionIdentity;
    const controller = new AbortController();
    const onFocus = () => resolveAcknowledgement(identity, controller.signal);
    window.addEventListener('focus', onFocus);
    return () => {
      window.removeEventListener('focus', onFocus);
      controller.abort();
    };
  }, [acknowledgementResolutionIdentity, resolveAcknowledgement]);

  // A source change or reconnect is also an existing status-refresh signal.
  // Do not add a timer or immediately repeat the status read after the POST.
  const previousResolutionWake = useRef<{
    identity: ManagerApplyIdentity | null; wakeKey: string; refresh: number;
  }>({ identity: null, wakeKey, refresh });
  useEffect(() => {
    const previous = previousResolutionWake.current;
    previousResolutionWake.current = { identity: acknowledgementResolutionIdentity, wakeKey, refresh };
    if (acknowledgementResolutionIdentity === null
      || !sameManagerApply(previous.identity, acknowledgementResolutionIdentity)
      || (previous.wakeKey === wakeKey && previous.refresh === refresh)) return undefined;
    const controller = new AbortController();
    resolveAcknowledgement(acknowledgementResolutionIdentity, controller.signal);
    return () => controller.abort();
  }, [acknowledgementResolutionIdentity, wakeKey, refresh, resolveAcknowledgement]);

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
    void readStatus(identity, controller.signal).then((candidate) => {
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
    readStatus,
    wakeKey,
    refresh,
  ]);

  return {
    status,
    synchronizing,
    unavailable: unavailable || (applyId !== null && observation.error !== null),
    reason: applyId === null || observation.error === null ? reason
      : `Deployment transition notification is invalid: ${observation.error}`,
    acknowledgeFailure,
    acknowledgementOutcomeUnknown: status?.state === 'failed'
      && status.failureAcknowledgement !== 'acknowledged'
      && sameManagerApply(status, outcomeUnknownIdentity),
  };
}
