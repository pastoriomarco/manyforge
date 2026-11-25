import { useCallback, useState } from 'react';

import { api, apiErrorCode } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type { ApplicationStartId } from '../api/applicationIdentity';
import type { AlarmMembership } from '../types/headless';

export function alarmActionErrorMessage(reason: unknown): string {
  return errorMessage(reason, 'Headless request failed.');
}

/**
 * Exact-record alarm acknowledgement, shared by the Errors overlay and the
 * Program recovery block: each request names the activation and record
 * revision it was shown, under the application start it was read in. It never
 * clears a condition or resumes motion.
 */
export function useExactAlarmAcknowledgement() {
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  // The failure's text and typed code (a local message has none).
  const [failure, setFailure] = useState<{ text: string; code: string | null } | null>(null);
  const setError = useCallback((text: string | null) => {
    setFailure(text === null ? null : { text, code: null });
  }, []);
  const acknowledge = useCallback(async (
    members: readonly AlarmMembership[],
    applicationStartId: ApplicationStartId,
  ) => {
    if (members.length === 0) return;
    setBusyActivation(members[0].activationId);
    setFailure(null);
    try {
      for (const member of members) {
        await api.alarms.acknowledge(member, applicationStartId);
      }
    } catch (reason) {
      setFailure({ text: alarmActionErrorMessage(reason), code: apiErrorCode(reason) });
    } finally {
      setBusyActivation(null);
    }
  }, []);
  return {
    busyActivation, error: failure?.text ?? null, errorCode: failure?.code ?? null, setError, acknowledge,
  };
}
