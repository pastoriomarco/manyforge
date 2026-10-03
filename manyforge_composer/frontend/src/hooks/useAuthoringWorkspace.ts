import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, ApiError, apiErrorCode, authoringRefusalMessage, failedAuthoringRequest, isStaleWorkspaceError, type AuthoringRequestIdentity } from '../api/client';
import { parseManagerApplyAvailability } from '../api/parsers';
import { sameManagerApply, type ManagerApplyIdentity } from '../state/managerApply';
import { errorMessage } from '../state/structuralResource';
import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  draftEditUnavailableReason,
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import type { AuthoringWorkspaceProjectFacts, ManagerApplyStatus } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { sameAuthoringWorkspaceIdentity, type ControlTransportStatus } from './useHeadlessState';

/**
 * One shared authoring-workspace read for the whole workbench.
 *
 * Every authoring consumer — Program, Deployment, Scenario documents — takes its
 * identity, both Draft projections and the Scenario project facts from this one
 * controller. It reads the composite snapshot once per observed workspace
 * identity and publishes it atomically, so no consumer establishes an
 * independent authoring identity and no two consumers can disagree about which
 * workspace revision they are editing.
 *
 * A newer identity invalidates an older response instead of publishing it, so a
 * revision gap costs exactly one read of the newest identity and never becomes a
 * poll loop. Applied, Manager and runtime truth stay in their existing owner
 * projections; this controller deliberately publishes authoring state only.
 */
export function useAuthoringWorkspace(
  snapshot: ComposerHeadlessState | null,
  transportStatus: ControlTransportStatus = 'connected',
) {
  const [published, setPublished] = useState<PublishedAuthoringWorkspace | null>(
    null,
  );
  const [error, setError] = useState<string | null>(null);
  const [reading, setReading] = useState(false);
  const mounted = useRef(true);
  const sequence = useRef(0);
  const inFlight = useRef(new Map<string, Promise<void>>());
  const installedKey = useRef<string | null>(null);
  const reconnectPending = useRef(false);
  const submission = useRef<null | 'sending' | 'unknown' | ManagerApplyIdentity>(null);
  const [submissionState, setSubmissionState] = useState<typeof submission.current>(null);
  const unknownRequest = useRef<AuthoringRequestIdentity | null>(null);
  const recheckingRef = useRef(false);
  const [rechecking, setRechecking] = useState(false);
  const [recheckMessage, setRecheckMessage] = useState<string | null>(null);
  const [correlatedApply, setCorrelatedApply] = useState<ManagerApplyIdentity | null>(null);
  const manager = snapshot?.runtime.sections?.manager;
  const managerAdmission = useMemo(() => {
    if (!manager?.available || manager.freshness !== 'fresh') {
      return { records: [], reason: 'Current Manager status is unavailable.' };
    }
    try {
      const apply = parseManagerApplyAvailability(manager.payload?.apply, 'manager.payload.apply');
      return {
        records: apply.records,
        reason: !apply.mutationAvailable || apply.records.some((record) => record.incomplete)
          ? 'A Manager operation is pending or authoring is unavailable.' : null,
      };
    } catch {
      return { records: [], reason: 'Manager operation status is invalid.' };
    }
  }, [manager]);

  useEffect(() => {
    const pending = submission.current;
    if (pending !== null && typeof pending === 'object'
      && manager?.available && manager.freshness === 'fresh'
      && managerAdmission.records.some((record) => sameManagerApply(record, pending))) {
      // Matching owner evidence takes over. A detail-read failure cannot leave
      // this browser holding a second, independent admission lock.
      submission.current = null;
      setSubmissionState(null);
    }
  }, [manager, managerAdmission, submissionState]);

  useEffect(() => {
    // A lost reply is not a lost operation. Every Manager-submitting route
    // records this browser's own request id as the Apply's correlation id, so
    // the exact record (never merely the latest one) identifies its Apply.
    const request = unknownRequest.current;
    if (submission.current !== 'unknown' || request === null) return;
    // An Apply sent with a shared correlation (Apply after Reset) is
    // recorded under that correlation instead of its own request id.
    const record = managerAdmission.records.find(
      (candidate) => candidate.request.correlationId === (request.correlationId ?? request.requestId),
    );
    if (record === undefined) return;
    const identity = { applyId: record.applyId, applyGeneration: record.applyGeneration };
    submission.current = identity;
    unknownRequest.current = null;
    setSubmissionState(identity);
    setCorrelatedApply(identity);
    setRecheckMessage(`Request ${request.requestId}: Manager recorded Apply ${identity.applyId} (${identity.applyGeneration}).`);
  }, [managerAdmission, submissionState]);

  const observed = snapshot === null ? null : snapshot.authoringWorkspace;
  const observedKey = observed === null
    ? null
    : `${observed.workspaceEpoch}:${observed.workspaceRevision}`;

  const install = useCallback(async (identityKey: string) => {
    const current = ++sequence.current;
    setReading(true);
    try {
      const next = await api.authoringWorkspace.get();
      // A newer observed identity (or an unmounted consumer) supersedes this
      // response: publishing it would install content the control stream has
      // already replaced.
      if (!mounted.current || sequence.current !== current) return;
      const identity = authoringWorkspaceIdentity(next);
      const responseKey = `${identity.workspaceEpoch}:${identity.workspaceRevision}`;
      if (responseKey !== identityKey) {
        installedKey.current = null;
        setError(null);
        return;
      }
      installedKey.current = responseKey;
      // This owner publication is immutable at its full identity. A reconnect
      // may verify it again without replacing every downstream document object.
      setPublished((previous) => previous !== null
        && sameAuthoringWorkspaceIdentity(previous.identity, identity)
        ? previous
        : { identity, snapshot: next });
      setError(null);
    } catch (reason: unknown) {
      if (!mounted.current || sequence.current !== current) return;
      // A failed read publishes nothing: the previously installed publication
      // stays on screen, authoring stays gated, and only a *newer* observed
      // identity triggers another read. This identity is never polled, and the
      // request is never repeated automatically.
      installedKey.current = null;
      setError(errorMessage(reason, 'Authoring workspace read failed.'));
    } finally {
      if (mounted.current && sequence.current === current) setReading(false);
    }
  }, []);

  const read = useCallback((identityKey: string): Promise<void> => {
    const existing = inFlight.current.get(identityKey);
    if (existing !== undefined) return existing;
    const request = install(identityKey).finally(() => {
      inFlight.current.delete(identityKey);
    });
    inFlight.current.set(identityKey, request);
    return request;
  }, [install]);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  useEffect(() => {
    if (observedKey === null) return;
    if (installedKey.current === observedKey) return;
    void read(observedKey);
  }, [read, observedKey]);

  /**
   * Read the newest observed identity once, discarding any older response.
   *
   * Used by a typed stale rejection and by an explicit operator refresh; it can
   * never install a publication for an identity older than the newest one.
   */
  const refresh = useCallback(async (): Promise<void> => {
    if (observedKey === null) return;
    installedKey.current = null;
    await read(observedKey);
  }, [read, observedKey]);

  useEffect(() => {
    if (transportStatus === 'connecting' || transportStatus === 'disconnected') {
      reconnectPending.current = true;
    }
    // Delayed/stale recovery on the same connection changes admission, not
    // document identity. Remember a real reconnect through intermediate states.
    if (transportStatus !== 'connected' || observedKey === null
      || !reconnectPending.current) return;
    reconnectPending.current = false;
    void refresh();
  }, [observedKey, refresh, transportStatus]);

  const current = publicationIsCurrent(published, snapshot);
  const ownerReason = transportStatus !== 'connected'
    ? 'Current backend status is unavailable.'
    : authoringUnavailableReason(published, snapshot) ?? managerAdmission.reason;
  // A Draft edit is also admitted while a held run has all motion stopped.
  const editOwnerReason = transportStatus !== 'connected'
    ? 'Current backend status is unavailable.'
    : draftEditUnavailableReason(published, snapshot) ?? managerAdmission.reason;
  const submissionReason = submissionState === null ? null
    : submissionState === 'unknown' ? 'Submission outcome is unconfirmed; inspect backend status before further changes.'
      : 'Waiting for the submitted operation to be observed by the backend.';
  const unavailableReason = submissionReason ?? ownerReason;
  const editUnavailableReason = submissionReason ?? editOwnerReason;
  /** `edit`: a Draft-only edit (or Discard); `document`: every other action. */
  const beginMutation = useCallback((kind: 'edit' | 'document' = 'document') => {
    if (submission.current !== null
      || (kind === 'edit' ? editOwnerReason : ownerReason) !== null) return false;
    submission.current = 'sending';
    unknownRequest.current = null;
    setRecheckMessage(null);
    setSubmissionState('sending');
    return true;
  }, [editOwnerReason, ownerReason]);
  const finishMutation = useCallback((accepted: ManagerApplyIdentity | null = null, failure?: unknown) => {
    // Only a definite client refusal proves that the effect was not accepted.
    // A transport/parse/server failure is not permission to replay a mutation.
    submission.current = failure === undefined ? accepted
      : authoringRefusalMessage(failure) !== null || isStaleWorkspaceError(failure)
        || apiErrorCode(failure) === 'manager_apply_rejected'
        || (failure instanceof ApiError && [400, 403, 404, 422].includes(failure.status))
        ? null : 'unknown';
    unknownRequest.current = submission.current === 'unknown' ? failedAuthoringRequest(failure) : null;
    if (unknownRequest.current !== null) {
      setRecheckMessage(`Request ${unknownRequest.current.requestId}${unknownRequest.current.path ? ` (${unknownRequest.current.path})` : ''}: outcome unconfirmed.`);
    }
    setSubmissionState(submission.current);
  }, []);
  const recheckSubmission = useCallback(async () => {
    if (submission.current !== 'unknown' || recheckingRef.current) return;
    const request = unknownRequest.current;
    if (request === null) {
      setRecheckMessage('The failed request has no verifiable identity. Its outcome remains unknown.');
      return;
    }
    recheckingRef.current = true;
    setRechecking(true);
    try {
      const outcome = await api.authoringWorkspace.submissionOutcome(request);
      if (!mounted.current || submission.current !== 'unknown' || unknownRequest.current !== request) return;
      if (outcome === null) {
        setRecheckMessage('No confirmed outcome is available for this exact request. It may still be processing, or its evidence is unavailable. Nothing was retried.');
        return;
      }
      // Successful readback settles only this browser's uncertainty. Current
      // workspace, Manager and Kernel admission still govern all new actions.
      const status = outcome.apply === null ? null : await api.manager.applyStatus(
        outcome.apply.applyId, outcome.apply.applyGeneration,
      );
      if (!mounted.current || submission.current !== 'unknown' || unknownRequest.current !== request) return;
      submission.current = status?.state === 'pending' ? outcome.apply : null;
      unknownRequest.current = null;
      setSubmissionState(submission.current);
      setRecheckMessage(outcome.apply === null
        ? `Request ${request.requestId}: the authoring change is confirmed settled.`
        : `Request ${request.requestId}: Apply ${outcome.apply.applyId} (${outcome.apply.applyGeneration}) is ${status!.state}. Backend admission still applies.`);
      await refresh();
    } catch (error) {
      if (mounted.current) setRecheckMessage(errorMessage(error, 'Submission status could not be verified.'));
    } finally {
      recheckingRef.current = false;
      if (mounted.current) setRechecking(false);
    }
  }, [refresh]);
  const observeApply = useCallback((status: ManagerApplyStatus | null) => {
    const pending = submission.current;
    // Bounded stream inventories may age a completed record out. An exact
    // terminal read plus current admission is also sufficient; absence isn't.
    if (status !== null && status.state !== 'pending' && ownerReason === null
      && pending !== null && typeof pending === 'object' && sameManagerApply(status, pending)) {
      submission.current = null;
      setSubmissionState(null);
    }
  }, [ownerReason]);
  const facts: AuthoringWorkspaceProjectFacts | null = projectFacts(published);

  return useMemo(() => ({
    published,
    /** True while the installed publication is the newest observed identity. */
    current,
    /** Stable reason authoring is unavailable, or ``null`` when it is. */
    unavailableReason,
    /** Why a Draft-only edit (or Discard) is unavailable, or ``null``. */
    editUnavailableReason,
    /** The Scenario project facts of the installed publication. */
    project: facts,
    reading,
    error,
    refresh,
    beginMutation,
    finishMutation,
    observeApply,
    submissionUnconfirmed: submissionState === 'unknown',
    /** The Manager Apply found for a lost reply by its exact request id. */
    correlatedApply,
    rechecking,
    recheckMessage,
    recheckSubmission,
  }), [current, error, facts, published, reading, refresh, unavailableReason, editUnavailableReason, beginMutation, finishMutation, observeApply, submissionState, correlatedApply, rechecking, recheckMessage, recheckSubmission]);
}

export type AuthoringWorkspaceController = ReturnType<typeof useAuthoringWorkspace>;
