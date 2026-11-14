import { useCallback, useState } from 'react';

import { api } from '../api/client';
import type { ApplicationStartId } from '../api/applicationIdentity';
import type { AlarmMembership } from '../types/headless';

export function alarmActionErrorMessage(reason: unknown): string {
  return reason instanceof Error && reason.message.trim()
    ? reason.message
    : 'Headless request failed.';
}

/**
 * Exact-record alarm acknowledgement, shared by the Errors overlay and the
 * Program recovery block: each request names the activation and record
 * revision it was shown, under the application start it was read in. It never
 * clears a condition or resumes motion.
 */
export function useExactAlarmAcknowledgement() {
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const acknowledge = useCallback(async (
    members: readonly AlarmMembership[],
    applicationStartId: ApplicationStartId,
  ) => {
    if (members.length === 0) return;
    setBusyActivation(members[0].activationId);
    setError(null);
    try {
      for (const member of members) {
        await api.alarms.acknowledge(member, applicationStartId);
      }
    } catch (reason) {
      setError(alarmActionErrorMessage(reason));
    } finally {
      setBusyActivation(null);
    }
  }, []);
  return { busyActivation, error, setError, acknowledge };
}
