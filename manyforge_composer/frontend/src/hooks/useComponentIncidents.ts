import { useCallback, useEffect, useState } from 'react';

import { api } from '../api/client';
import type { CCLCComponentIncident, CCLCComponentSnapshot } from '../types/cclc';
import type { CellRecoverResult } from '../types/robotControl';
import {
  cclcControlModeUnavailableReason,
  cclcRequestOwnerEpoch,
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

const WAITING_FOR: Record<string, string> = {
  stop_fault_recovery_waiting_for_observation:
    'a fresh controller observation or an uncertain operation to resolve',
  stop_fault_recovery_waiting_for_quiescence: 'the arm to be proven at rest',
  stop_fault_recovery_waiting_for_configuration: 'the control configuration to be confirmed',
  stop_fault_recovery_waiting_for_external_reconciliation: 'external reconciliation',
  stop_fault_recovery_waiting_for_durable_record: 'the Stop record to be saved',
  stop_fault_recovery_pending: 'the recovery already in progress',
};

/**
 * Word Recover cell's typed result for the operator; null when it is done.
 * The reason code is the owner's; only the wording is decided here.
 */
export function cellRecoverMessage(result: CellRecoverResult): string | null {
  const { reason } = result;
  if (result.outcome === 'done' || reason === null) return null;
  const acknowledged = result.acknowledgedIncidents.length;
  const waitingFor = WAITING_FOR[reason.code];
  let text: string;
  if (programCycleBlocksRecovery(reason.code)) {
    text = 'The cell latch stays while a Program run is active. '
      + 'Reset the Program first, then press Recover cell again.';
  } else if (reason.code === 'incident_open') {
    const component = reason.details.controllable_component_id ?? 'a component';
    const cause = reason.details.cause_class?.replaceAll('_', ' ') ?? 'unknown cause';
    text = `The ${component} incident is open again (${cause}): its cause is still present, `
      + 'for example an e-stop still pressed. Clear the cause, then press Recover cell again.';
  } else if (reason.code === 'cclc_incident_changed') {
    // M6 (item 36): nothing was acknowledged; the incident list refreshes.
    const component = reason.details.controllable_component_id ?? 'a component';
    const cause = reason.details.cause_class?.replaceAll('_', ' ');
    text = `An incident on ${component}${cause === undefined ? '' : ` (${cause})`} appeared that `
      + 'this press had not shown. Nothing was acknowledged: review it, then press Recover cell again.';
  } else if (waitingFor !== undefined) {
    text = `Cell recovery is not ready yet: waiting for ${waitingFor}. `
      + 'Press Recover cell again when it is ready.';
  } else if (reason.code === 'cell_recover_outcome_unknown'
    && reason.details.replay === 'request_id_unknown') {
    // RECTIME: an automatic resend found its press gone (Headless restarted).
    text = 'Recovery outcome unknown after a restart: check the cell state '
      + 'and press Recover cell again.';
  } else if (reason.code === 'cell_recover_outcome_unknown') {
    text = 'Recover cell outcome not confirmed — check the cell state. '
      + 'Steps not listed here may or may not have run.';
  } else if (reason.code === 'cell_recover_aborted') {
    // SX-679: the operator's abort; the steps listed here are all that ran.
    const steps = reason.details.reenable_steps;
    const notEnabled = reason.details.not_reenabled;
    text = [
      `Recover cell was aborted before its ${
        (reason.details.before_step ?? 'next').replaceAll('_', ' ')} step; no later step started.`,
      steps ? `Re-enable steps done: ${steps.replaceAll(';', '; ').replaceAll('+', ', ')}.` : null,
      notEnabled ? `Not enabled again: ${notEnabled.replaceAll(',', ', ')}.` : null,
      'Press Recover cell again to continue.',
    ].filter((part): part is string => part !== null).join(' ');
  } else if (reason.code === 'stop_fault_latch_replaced') {
    text = 'A newer cell stop replaced the one this press saw. Press Recover cell again.';
  } else if (reason.code === 'stop_fault_recovery_unavailable') {
    text = 'Cell recovery is unavailable: '
      + `${reason.details.recovery_unavailable_reason ?? reason.message}.`;
  } else {
    text = `Cell recovery was refused: ${reason.message} (${reason.code}).`;
  }
  return [
    acknowledged === 0 ? null : `${acknowledged} component incident(s) acknowledged.`,
    result.latchCleared ? 'The cell latch was cleared.' : null,
    result.recoveredComponents.length === 0
      ? null : `Recover started for ${result.recoveredComponents.join(', ')}.`,
    text,
    // SX-689: a step that failed after the abort keeps its own reason.
    reason.code !== 'cell_recover_aborted' && reason.details.aborted === 'true'
      ? 'Recover cell was aborted: no later step started.' : null,
  ].filter((part): part is string => part !== null).join(' ');
}

/** Reasons for the latch clear refused because a Program cycle is still active. */
export function programCycleBlocksRecovery(code: string): boolean {
  return code.startsWith('program_cycle_state_');
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
