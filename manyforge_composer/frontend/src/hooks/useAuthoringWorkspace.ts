import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import type { AuthoringWorkspaceProjectFacts } from '../types/api';
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
  const unavailableReason = authoringUnavailableReason(published, snapshot);
  const facts: AuthoringWorkspaceProjectFacts | null = projectFacts(published);

  return useMemo(() => ({
    published,
    /** True while the installed publication is the newest observed identity. */
    current,
    /** Stable reason authoring is unavailable, or ``null`` when it is. */
    unavailableReason,
    /** The Scenario project facts of the installed publication. */
    project: facts,
    reading,
    error,
    refresh,
  }), [current, error, facts, published, reading, refresh, unavailableReason]);
}

export type AuthoringWorkspaceController = ReturnType<typeof useAuthoringWorkspace>;
