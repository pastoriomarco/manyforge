/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import type { CycleRecoveryAction, CycleRecoveryOffer } from '../types/api';
import {
  recoveryActionLabel,
  recoveryConfirmationAction,
  recoveryOfferHeadline,
} from './RecoveryActionControls';

function offer(overrides: Partial<CycleRecoveryOffer> = {}): CycleRecoveryOffer {
  return {
    schemaVersion: 'manyforge.cycle_recovery_offer.v1',
    offerId: 'offer-1',
    offerRevision: 1,
    generatedAtNs: 10,
    kernelAssessmentRevision: 2,
    controlCommandEpoch: 3,
    stopOwnerGeneration: 4,
    cycleGeneration: 5,
    programGeneration: 6,
    deploymentGeneration: 7,
    sceneAdmissionEpoch: 8,
    poseSourceApplyGeneration: 9,
    recoveryClass: 'runtime_collision',
    status: 'waiting',
    actions: [],
    reasonCode: 'recovery_evidence_expired',
    evidenceRefs: [],
    ...overrides,
  };
}

describe('RecoveryActionControls presentation', () => {
  it('uses only the server offer status/class for waiting copy', () => {
    expect(recoveryOfferHeadline(offer())).toBe('Stopping after collision');
    expect(
      recoveryOfferHeadline(
        offer({ recoveryClass: 'failed_validation' }),
      ),
    ).toBe('Waiting for fresh observed state');
  });

  it('maps stable label keys and does not inspect error details', () => {
    const action: CycleRecoveryAction = {
      actionId: 'retry_after_scene_repair',
      labelKey: 'cycle.recovery.retry_after_scene_repair',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: true,
    };
    expect(recoveryActionLabel(action)).toBe('Retry after scene repair');
  });

  it('cannot reuse a confirmation across an offer replacement', () => {
    const action: CycleRecoveryAction = {
      actionId: 'reset_cycle',
      labelKey: 'cycle.recovery.reset_cycle',
      requiresConfirmation: true,
      performsBackendReset: true,
      enabled: true,
    };
    const first = offer({ actions: [action] });
    const confirmation = {
      offerId: first.offerId,
      offerRevision: first.offerRevision,
      actionId: action.actionId,
    };
    expect(recoveryConfirmationAction(first, confirmation)).toBe(action);
    expect(
      recoveryConfirmationAction(
        offer({ offerId: 'offer-2', offerRevision: 2, actions: [action] }),
        confirmation,
      ),
    ).toBeUndefined();
  });
});
