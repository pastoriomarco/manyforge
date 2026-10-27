import { afterEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import {
  acknowledgeComponentIncidents,
  readOpenComponentIncidents,
  stopFaultRecoveryMissing,
} from './useComponentIncidents';
import type { CCLCComponentIncident, CCLCComponentSnapshot } from '../types/cclc';
import type { ExecutionIdentity, StopFaultRecoveryOffer } from '../types/robotControl';

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

  it('has nothing to read or acknowledge under direct control', async () => {
    mockOwner({ arm: snapshot('arm', true) }, { ...execution, controlManagementMode: 'direct' });
    const submit = vi.spyOn(api.cclc, 'submitLifecycle');
    await expect(readOpenComponentIncidents(['arm'])).resolves.toEqual([]);
    await expect(acknowledgeComponentIncidents(['arm'], 3)).resolves.toBe(0);
    expect(submit).not.toHaveBeenCalled();
  });

  it('acknowledges each open incident with one exact recover request', async () => {
    mockOwner({ arm: snapshot('arm', true), gripper: snapshot('gripper', false) });
    const submit = vi.spyOn(api.cclc, 'submitLifecycle').mockResolvedValue({
      replayStatus: 'accepted_new',
      requestId: 'request',
      transactionId: 'lifecycle.1',
      status: null,
      incidentRecovery: null,
    });
    await expect(acknowledgeComponentIncidents(['arm', 'gripper'], 3)).resolves.toBe(1);
    expect(submit).toHaveBeenCalledTimes(1);
    const [request] = submit.mock.calls[0]!;
    expect(request).toMatchObject({
      controllable_component_id: 'arm',
      intent: 'recover',
      expected_component_lifecycle_generation: 4,
      expected_admission_generation: 3,
      confirmed: true,
      request_context: { request_owner_epoch: 'cclc.epoch.1', source_label: 'manyforge_composer' },
    });
  });

  it('counts an authorization-only Recover (no transaction) as the acknowledgement', async () => {
    mockOwner({ arm: snapshot('arm', true) });
    vi.spyOn(api.cclc, 'submitLifecycle').mockResolvedValue({
      replayStatus: 'accepted_new',
      requestId: null,
      transactionId: null,
      status: null,
      incidentRecovery: {
        incidentId: 'incident.arm.1',
        controllableComponentId: 'arm',
        causeClass: 'collision',
        acknowledgedBy: 'manyforge_composer',
        acknowledgedAtNs: 2_000,
      },
    });
    await expect(acknowledgeComponentIncidents(['arm'], 3)).resolves.toBe(1);
  });

  it('refuses to acknowledge without admission and reports a refused request', async () => {
    mockOwner({ arm: snapshot('arm', true) });
    const submit = vi.spyOn(api.cclc, 'submitLifecycle').mockResolvedValue({
      replayStatus: 'stale_request_owner_epoch',
      requestId: null,
      transactionId: null,
      status: null,
      incidentRecovery: null,
    });
    await expect(acknowledgeComponentIncidents(['arm'], null)).rejects.toThrow('admission');
    expect(submit).not.toHaveBeenCalled();
    await expect(acknowledgeComponentIncidents(['arm'], 3)).rejects.toThrow(
      'Acknowledgement of the arm incident was stale_request_owner_epoch.',
    );
  });

  it('says what the cell recovery offer still waits for', () => {
    const offer = {
      recoveryStatus: 'waiting_for_configuration',
      recoveryUnavailableReason: null,
    } as StopFaultRecoveryOffer;
    expect(stopFaultRecoveryMissing(offer, 1)).toBe(
      '1 component incident(s) acknowledged. Cell recovery is not ready yet: waiting for the '
      + 'control configuration to be confirmed. Press Recover cell again when it is ready.',
    );
    expect(stopFaultRecoveryMissing({ ...offer, recoveryStatus: 'waiting_for_observation' }, 0))
      .toBe('Cell recovery is not ready yet: waiting for a fresh controller observation. '
        + 'Press Recover cell again when it is ready.');
  });
});
