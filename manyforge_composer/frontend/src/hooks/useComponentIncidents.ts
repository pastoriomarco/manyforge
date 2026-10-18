import { useCallback, useEffect, useState } from 'react';

import { api } from '../api/client';
import type { CCLCComponentIncident, CCLCComponentSnapshot } from '../types/cclc';
import type { StopFaultRecoveryOffer } from '../types/robotControl';
import {
  cclcControlModeUnavailableReason,
  cclcRequestOwnerEpoch,
  lifecycleRequestFromSnapshot,
  requestContext,
} from './useCCLC';

const OBSERVATION_INTERVAL_MS = 2000;

/** One open component incident, exactly as the CCLC snapshot issued it. */
export interface OpenComponentIncident {
  controllableComponentId: string;
  incident: CCLCComponentIncident;
}

interface IncidentRead {
  ownerEpoch: string | null;
  snapshots: CCLCComponentSnapshot[];
}

/**
 * Read every listed component's current CCLC snapshot and keep those with an
 * open incident. A Deployment without component lifecycle control has no
 * incidents to acknowledge.
 */
async function readIncidentSnapshots(
  componentIds: readonly string[],
  signal?: AbortSignal,
): Promise<IncidentRead> {
  if (componentIds.length === 0) return { ownerEpoch: null, snapshots: [] };
  const execution = await api.execution.identity(signal);
  if (cclcControlModeUnavailableReason(execution.controlManagementMode) !== null) {
    return { ownerEpoch: null, snapshots: [] };
  }
  const [owner, ...snapshots] = await Promise.all([
    api.cclc.owner(signal),
    ...componentIds.map((controllableComponentId) => api.cclc.component(controllableComponentId, signal)),
  ]);
  const open = snapshots.filter((snapshot) => snapshot.lifecycleProgress?.incident != null);
  let ownerEpoch: string | null = null;
  for (const snapshot of open) {
    ownerEpoch = cclcRequestOwnerEpoch(owner, snapshot, execution);
  }
  return { ownerEpoch, snapshots: open };
}

export async function readOpenComponentIncidents(
  componentIds: readonly string[],
  signal?: AbortSignal,
): Promise<OpenComponentIncident[]> {
  const { snapshots } = await readIncidentSnapshots(componentIds, signal);
  return snapshots.map((snapshot) => ({
    controllableComponentId: snapshot.controllableComponentId,
    incident: snapshot.lifecycleProgress!.incident!,
  }));
}

/**
 * The operator's acknowledgement (addendum 2, contract item 3): one CCLC
 * lifecycle `recover` request per component with an open incident, built
 * from that component's fresh snapshot like the lifecycle panel's requests.
 * CCLC accepts it either authorization-only (`incidentRecovery`, no
 * transaction: the incident is closed and the cell recovery follows) or, when
 * the incident already closed, as an ordinary lifecycle transaction.
 * Returns how many were accepted; throws on the first refusal.
 */
export async function acknowledgeComponentIncidents(
  componentIds: readonly string[],
  admissionGeneration: number | null,
  signal?: AbortSignal,
): Promise<number> {
  const { ownerEpoch, snapshots } = await readIncidentSnapshots(componentIds, signal);
  if (snapshots.length === 0 || ownerEpoch === null) return 0;
  if (admissionGeneration === null) {
    throw new Error(
      'Current authoring admission is unavailable; component incidents cannot be acknowledged.',
    );
  }
  for (const snapshot of snapshots) {
    const result = await api.cclc.submitLifecycle(
      lifecycleRequestFromSnapshot(
        snapshot,
        'recover',
        requestContext(ownerEpoch, 'lifecycle:recover'),
        admissionGeneration,
      ),
      signal,
    );
    if (result.incidentRecovery === null && result.transactionId === null) {
      throw new Error(
        `Acknowledgement of the ${snapshot.controllableComponentId} incident was ${result.replayStatus}.`,
      );
    }
  }
  return snapshots.length;
}

const MISSING_RECOVERY_FACTS: Record<StopFaultRecoveryOffer['recoveryStatus'], string> = {
  waiting_for_observation: 'a fresh controller observation',
  waiting_for_quiescence: 'all motion to settle',
  waiting_for_configuration: 'the control configuration to be confirmed',
  waiting_for_external_reconciliation: 'external reconciliation',
  waiting_for_durable_record: 'the Stop record to be saved',
  recovery_pending: 'the recovery already in progress',
  ready: 'nothing',
};

/** What the cell recovery offer still waits for, worded for the operator. */
export function stopFaultRecoveryMissing(
  offer: StopFaultRecoveryOffer,
  acknowledged: number,
): string {
  return [
    acknowledged === 0 ? null : `${acknowledged} component incident(s) acknowledged.`,
    `Cell recovery is not ready yet: waiting for ${MISSING_RECOVERY_FACTS[offer.recoveryStatus]}.`,
    offer.recoveryUnavailableReason,
    'Press Recover cell again when it is ready.',
  ].filter((part): part is string => part !== null).join(' ');
}

export interface ComponentIncidentsController {
  incidents: OpenComponentIncident[];
  unavailableReason: string | null;
  refresh: () => void;
}

/**
 * Re-read the listed components' open incidents while `enabled`. It only
 * presents CCLC-issued facts and decides nothing.
 */
export function useComponentIncidents(
  enabled: boolean,
  componentIds: readonly string[],
): ComponentIncidentsController {
  const [incidents, setIncidents] = useState<OpenComponentIncident[]>([]);
  const [unavailableReason, setUnavailableReason] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const key = componentIds.join('\n');
  const refresh = useCallback(() => setReloadToken((value) => value + 1), []);

  useEffect(() => {
    if (!enabled || key === '') {
      setIncidents([]);
      setUnavailableReason(null);
      return undefined;
    }
    const ids = key.split('\n');
    let active = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;
    const controller = new AbortController();
    const observe = async () => {
      try {
        const next = await readOpenComponentIncidents(ids, controller.signal);
        if (!active) return;
        setIncidents(next);
        setUnavailableReason(null);
      } catch (reason) {
        if (!active) return;
        setIncidents([]);
        setUnavailableReason(
          reason instanceof Error ? reason.message : 'Component incidents are unavailable',
        );
      }
      if (active) timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    };
    void observe();
    return () => {
      active = false;
      controller.abort();
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [enabled, key, reloadToken]);

  return { incidents, unavailableReason, refresh };
}
