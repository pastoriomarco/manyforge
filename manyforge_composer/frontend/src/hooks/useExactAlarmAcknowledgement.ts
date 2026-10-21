import { useCallback, useState } from 'react';

import { api } from '../api/client';
import type { AlarmMembership } from '../types/headless';

export function alarmActionErrorMessage(reason: unknown): string {
  return reason instanceof Error && reason.message.trim()
    ? reason.message
    : 'Headless request failed.';
}

export function alarmRequestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

/**
 * Exact-record alarm acknowledgement, shared by the Errors overlay and the
 * Program recovery block: each request names the activation and record
 * revision it was shown, under the owner epoch it was read in. It never
 * clears a condition or resumes motion.
 */
export function useExactAlarmAcknowledgement() {
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const acknowledge = useCallback(async (
    members: readonly AlarmMembership[],
    ownerEpoch: string,
  ) => {
    if (members.length === 0) return;
    setBusyActivation(members[0].activationId);
    setError(null);
    try {
      for (const member of members) {
        await api.alarms.acknowledge(member, ownerEpoch, alarmRequestId());
      }
    } catch (reason) {
      setError(alarmActionErrorMessage(reason));
    } finally {
      setBusyActivation(null);
    }
  }, []);
  return { busyActivation, error, setError, acknowledge };
}
