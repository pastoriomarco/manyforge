import { afterEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import { cellRecoverMessage, readOpenComponentIncidents } from './useComponentIncidents';
import type { CCLCComponentIncident, CCLCComponentSnapshot } from '../types/cclc';
import type { CellRecoverResult, ExecutionIdentity } from '../types/robotControl';

const execution: ExecutionIdentity = {
  ownerEpoch: 'kernel-owner.1',
  cellId: 'cell.1',
  activePublicationGeneration: 1,
  controlManagementMode: 'component_lifecycle',
  graphRevision: 'graph.1',
  retainedBundleId: 'bundle.1',
};

const incident: CCLCComponentIncident = {
  incidentId: 'incident.arm.1',
  causeClass: 'collision',
  vendorCodes: ['C31'],
  detectedAtNs: 1_000,
  summary: 'Collision detected',
  acknowledged: false,
};

function snapshot(controllableComponentId: string, open: boolean): CCLCComponentSnapshot {
  return {
    controllableComponentId,
    lifecycleGeneration: 4,
    lifecycleState: 'active',
    publicationGeneration: 1, activePublicPointerDigest: 'pointer.1',
    retainedBundleId: 'bundle.1',
    graphRevision: 'graph.1',
    providers: [],
    domains: [],
    lifecycleTarget: { requestedState: 'active', origin: 'startup', targetGeneration: 1 },
    readiness: { requestOwnerEpoch: 'cclc.epoch.1', evaluatedAtNs: 100, nextIntent: null, requestedState: null,
      ready: !open, error: open ? { code: 'incident_open', message: 'Collision detected', details: {} } : null,
      providerOccurrences: [], measuredState: { required: false, available: null, error: null } },
    lifecycleProgress: {
      phase: open ? 'blocked' : 'satisfied',
      nextIntent: null,
      blockingCauseClass: open ? 'collision' : 'none',
      blockingReason: null,
      waitingSinceNs: null,
      delayed: false,
      incident: open ? incident : null,
    },
  };
}

function mockOwner(snapshots: Record<string, CCLCComponentSnapshot>, mode = execution) {
  vi.spyOn(api.execution, 'identity').mockResolvedValue(mode);
  vi.spyOn(api.cclc, 'owner').mockResolvedValue({ requestOwnerEpoch: 'cclc.epoch.1' });
  vi.spyOn(api.cclc, 'component').mockImplementation(
    async (controllableComponentId: string) => snapshots[controllableComponentId]!,
  );
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe('component incidents for Recover cell', () => {
  it('reads only the incidents CCLC currently reports', async () => {
    mockOwner({ arm: snapshot('arm', true), gripper: snapshot('gripper', false) });
    await expect(readOpenComponentIncidents(['arm', 'gripper'])).resolves.toEqual([
      { controllableComponentId: 'arm', incident },
    ]);
  });

  it('has nothing to read under direct control', async () => {
    mockOwner({ arm: snapshot('arm', true) }, { ...execution, controlManagementMode: 'direct' });
    await expect(readOpenComponentIncidents(['arm'])).resolves.toEqual([]);
  });
});

describe('Recover cell result wording', () => {
  const result = (reason: CellRecoverResult['reason'], acknowledged = 1): CellRecoverResult => ({
    outcome: reason === null ? 'done' : 'not_recovered',
    acknowledgedIncidents: Array.from({ length: acknowledged }, (_, index) => ({
      controllableComponentId: `component.${index}`, incidentId: `incident.${index}`, causeClass: 'collision',
    })),
    latchCleared: false,
    recoveredComponents: [],
    reason,
  });

  it('says nothing when the cell is recovered', () => {
    expect(cellRecoverMessage({ ...result(null), latchCleared: true })).toBeNull();
  });

  it('names an incident that is open again, such as an e-stop still pressed', () => {
    expect(cellRecoverMessage(result({
      code: 'incident_open', message: 'm',
      details: { controllable_component_id: 'arm', cause_class: 'emergency_stop' },
    }))).toBe('1 component incident(s) acknowledged. The arm incident is open again (emergency stop): '
      + 'its cause is still present, for example an e-stop still pressed. '
      + 'Clear the cause, then press Recover cell again.');
  });

  it('says what the latch still waits for', () => {
    expect(cellRecoverMessage(result({
      code: 'stop_fault_recovery_waiting_for_quiescence', message: 'm', details: {},
    }, 0))).toBe('Cell recovery is not ready yet: waiting for the arm to be proven at rest. '
      + 'Press Recover cell again when it is ready.');
    expect(cellRecoverMessage(result({
      code: 'stop_fault_recovery_waiting_for_observation', message: 'm', details: {},
    }, 0))).toContain('an uncertain operation to resolve');
  });

  it('names the components whose Recover started and a replaced latch', () => {
    expect(cellRecoverMessage({
      ...result({ code: 'stop_fault_latch_replaced', message: 'm', details: {} }, 0),
      recoveredComponents: ['arm'],
    })).toBe('Recover started for arm. A newer cell stop replaced the one this press saw. '
      + 'Press Recover cell again.');
  });

  it('does not call an unknown outcome a refusal', () => {
    expect(cellRecoverMessage(result({
      code: 'cell_recover_outcome_unknown', message: 'm', details: {},
    }, 1))).toBe('1 component incident(s) acknowledged. Recover cell outcome not confirmed — '
      + 'check the cell state. Steps not listed here may or may not have run.');
  });

  it('shows any other owner refusal with its code', () => {
    expect(cellRecoverMessage(result({
      code: 'stale_request_owner_epoch', message: 'Stop fault recovery names a stale owner epoch',
      details: {},
    }, 0))).toBe('Cell recovery was refused: Stop fault recovery names a stale owner epoch '
      + '(stale_request_owner_epoch).');
  });
});
