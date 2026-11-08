import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, ApiError, apiErrorCode, currentScenarioText, failedAuthoringRequest, isStaleWorkspaceError, KnownOutcomeError, refusedBeforeEffect, type AuthoringRequestIdentity } from '../api/client';
import { parseManagerApplyAvailability } from '../api/parsers';
import { authoringOperation, busyReason, type AuthoringCommand, type BusyOperation } from '../state/busyOperation';
import { sameManagerApply, type ManagerApplyIdentity } from '../state/managerApply';
import { errorMessage } from '../state/structuralResource';
import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  draftEditUnavailableReason,
  projectFacts,
  publicationIsCurrent,
  scenarioReplacementUnavailableReason,
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
  // Gates only while this browser's own request is in flight, or until the
  // owner observes the Apply its reply accepted.
  const submission = useRef<null | 'sending' | ManagerApplyIdentity>(null);
  const [submissionState, setSubmissionState] = useState<typeof submission.current>(null);
  // The operation this browser submitted, named until its submission settles.
  const [submittedOperation, setSubmittedOperation] = useState<BusyOperation | null>(null);
  // A lost reply is the browser's own uncertainty, not backend state: it gates
  // nothing (backend admission decides) and is only shown, until Recheck reads
  // the outcome or a later successful action settles it. Nothing is replayed.
  // `request` is the exact identity Recheck can look up, when there is one.
  const unconfirmed = useRef<{ request: AuthoringRequestIdentity | null } | null>(null);
  const [unconfirmedState, setUnconfirmedState] = useState<typeof unconfirmed.current>(null);
  const recheckingRef = useRef(false);
  const [rechecking, setRechecking] = useState(false);
  const [recheckMessage, setRecheckMessage] = useState<string | null>(null);
  const [correlatedApply, setCorrelatedApply] = useState<ManagerApplyIdentity | null>(null);
  const manager = snapshot?.runtime.sections?.manager;
  const managerAdmission = useMemo(() => {
    if (!manager?.available || manager.freshness !== 'fresh') {
      return { records: [], reason: 'Current Manager status is unavailable.', replaceReason: null, applying: null };
    }
    try {
      const apply = parseManagerApplyAvailability(manager.payload?.apply, 'manager.payload.apply');
      const inFlight = apply.records.some((record) => record.incomplete);
      return {
        records: apply.records,
        reason: !apply.mutationAvailable || inFlight
          ? 'A Manager operation is pending or authoring is unavailable.' : null,
        // New and Load wait only for an Apply actually in flight.
        replaceReason: inFlight ? 'A Manager Apply is in progress.' : null,
        applying: inFlight,
      };
    } catch {
      return { records: [], reason: 'Manager operation status is invalid.', replaceReason: null, applying: null };
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
    const request = unconfirmed.current?.request ?? null;
    if (request === null) return;
    // An Apply sent with a shared correlation (Apply after Reset) is
    // recorded under that correlation instead of its own request id.
    const record = managerAdmission.records.find(
      (candidate) => candidate.request.correlationId === (request.correlationId ?? request.requestId),
    );
    if (record === undefined) return;
    const identity = { applyId: record.applyId, applyGeneration: record.applyGeneration };
    unconfirmed.current = null;
    setUnconfirmedState(null);
    setCorrelatedApply(identity);
    setRecheckMessage(`Request ${request.requestId}: Manager recorded Apply ${identity.applyId} (${identity.applyGeneration}).`);
  }, [managerAdmission, unconfirmedState]);

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
  // A Draft edit takes no claim: it never waits for Kernel or Manager
  // admission, and is also admitted while a held run has all motion stopped.
  const editOwnerReason = transportStatus !== 'connected'
    ? 'Current backend status is unavailable.'
    : draftEditUnavailableReason(published, snapshot);
  // New and Load: the server decides a run; the rest still applies.
  const replaceOwnerReason = transportStatus !== 'connected'
    ? 'Current backend status is unavailable.'
    : scenarioReplacementUnavailableReason(published, snapshot) ?? managerAdmission.replaceReason;
  const operation = submissionState === null ? null : submittedOperation;
  const submissionReason = submissionState === null ? null
    : busyReason(operation) ?? 'Waiting for the submitted operation to be observed by the backend.';
  const unavailableReason = submissionReason ?? ownerReason;
  const editUnavailableReason = submissionReason ?? editOwnerReason;
  const replaceUnavailableReason = submissionReason ?? replaceOwnerReason;
  /**
   * `edit`: a Draft-only edit (or Discard); `replace`: New, Load or Close;
   * `document`: every other action. `command` names the operation to the
   * GUI (its button's busy label, the other commands' tooltips) until it settles.
   */
  const beginMutation = useCallback((
    kind: 'edit' | 'replace' | 'document',
    command: AuthoringCommand,
  ) => {
    if (submission.current !== null
      || (kind === 'edit' ? editOwnerReason : kind === 'replace' ? replaceOwnerReason : ownerReason) !== null) return false;
    submission.current = 'sending';
    setRecheckMessage(null);
    setSubmittedOperation(authoringOperation(command));
    setSubmissionState('sending');
    return true;
  }, [editOwnerReason, ownerReason, replaceOwnerReason]);
  const finishMutation = useCallback((accepted: ManagerApplyIdentity | null = null, failure?: unknown) => {
    // Only a definite refusal proves that the effect was not accepted: the
    // backend's before-effect marker, a typed stale fence, a Manager
    // rejection or a client-error status. A transport/parse/server failure is
    // not permission to replay a mutation.
    // A request error re-worded for the operator is classified by its cause.
    const settled = failure instanceof Error && failure.cause !== undefined ? failure.cause : failure;
    const definite = failure === undefined
      || settled instanceof KnownOutcomeError
      || refusedBeforeEffect(settled) || isStaleWorkspaceError(settled)
      || apiErrorCode(settled) === 'manager_apply_rejected'
      || (settled instanceof ApiError && [400, 403, 404, 422].includes(settled.status));
    submission.current = failure === undefined ? accepted : null;
    setSubmissionState(submission.current);
    if (failure === undefined) {
      // A later successful action settles an earlier lost reply.
      unconfirmed.current = null;
      setUnconfirmedState(null);
    } else if (!definite) {
      const request = failedAuthoringRequest(failure);
      unconfirmed.current = { request };
      setUnconfirmedState(unconfirmed.current);
      setRecheckMessage(request === null ? null
        : `Request ${request.requestId}${request.path ? ` (${request.path})` : ''}: outcome unconfirmed.`);
    }
  }, []);
  const recheckSubmission = useCallback(async () => {
    const pending = unconfirmed.current;
    if (pending === null || recheckingRef.current) return;
    const request = pending.request;
    if (request === null) {
      // No exact record to look up: settled from current state. With no Apply
      // in flight the lost request has ended; the current Scenario says how.
      if (managerAdmission.applying !== false) {
        setRecheckMessage(managerAdmission.applying === null
          ? 'Current Manager status is unavailable; recheck once it is back. Nothing was retried.'
          : 'A Manager Apply is still in progress; recheck once it ends. Nothing was retried.');
        return;
      }
      recheckingRef.current = true;
      setRechecking(true);
      try {
        const text = await currentScenarioText();
        if (!mounted.current || unconfirmed.current !== pending) return;
        unconfirmed.current = null;
        setUnconfirmedState(null);
        setRecheckMessage(`${text} Nothing was retried.`);
        await refresh();
      } finally {
        recheckingRef.current = false;
        if (mounted.current) setRechecking(false);
      }
      return;
    }
    recheckingRef.current = true;
    setRechecking(true);
    try {
      const outcome = await api.authoringWorkspace.submissionOutcome(request);
      if (!mounted.current || unconfirmed.current !== pending) return;
      if (outcome === null) {
        setRecheckMessage('No confirmed outcome is available for this exact request. It may still be processing, or its evidence is unavailable. Nothing was retried.');
        return;
      }
      // Successful readback settles only this browser's uncertainty. Current
      // workspace, Manager and Kernel admission still govern all new actions.
      const status = outcome.apply === null ? null : await api.manager.applyStatus(
        outcome.apply.applyId, outcome.apply.applyGeneration,
      );
      if (!mounted.current || unconfirmed.current !== pending) return;
      unconfirmed.current = null;
      setUnconfirmedState(null);
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
  }, [managerAdmission.applying, refresh]);
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
    /** Why New, Load or Close is unavailable, or ``null``. */
    replaceUnavailableReason,
    /** The Scenario project facts of the installed publication. */
    project: facts,
    reading,
    error,
    refresh,
    /** The submitted operation in progress (busy label and tooltip), or null. */
    operation,
    beginMutation,
    finishMutation,
    observeApply,
    /** A lost reply's outcome is not known yet; informational, gates nothing. */
    submissionUnconfirmed: unconfirmedState !== null,
    /** The Manager Apply found for a lost reply by its exact request id. */
    correlatedApply,
    rechecking,
    recheckMessage,
    recheckSubmission,
  }), [current, error, facts, published, reading, refresh, operation, unavailableReason, editUnavailableReason, replaceUnavailableReason, beginMutation, finishMutation, observeApply, unconfirmedState, correlatedApply, rechecking, recheckMessage, recheckSubmission]);
}

export type AuthoringWorkspaceController = ReturnType<typeof useAuthoringWorkspace>;
