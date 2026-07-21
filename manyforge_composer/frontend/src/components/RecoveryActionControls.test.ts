/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import type { CycleRecoveryAction, CycleRecoveryOffer } from '../types/api';
import {
  RecoveryCommandNotice,
  recoveryActionConfirmationText,
  recoveryActionLabel,
  recoveryActionsForTransport,
  recoveryCommandFailureMessage,
  recoveryConfirmationAction,
  isMotionProducingRecoveryAction,
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

describe('recovery action presentation helpers', () => {
  it('separates motion-producing recovery intent from attention/reset gates', () => {
    expect(isMotionProducingRecoveryAction('retry_motion_from_current_state')).toBe(true);
    expect(isMotionProducingRecoveryAction('retry_after_scene_repair')).toBe(true);
    expect(isMotionProducingRecoveryAction('replan_from_stopped_state')).toBe(true);
    expect(isMotionProducingRecoveryAction('retry_runtime_step')).toBe(true);
    expect(isMotionProducingRecoveryAction('acknowledge_and_continue')).toBe(false);
    expect(isMotionProducingRecoveryAction('reset_cycle')).toBe(false);
  });

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

  it('labels intervention acknowledgement as a gate, not an implicit continue', () => {
    const action: CycleRecoveryAction = {
      actionId: 'acknowledge_and_continue',
      labelKey: 'cycle.recovery.acknowledge_and_continue',
      requiresConfirmation: true,
      performsBackendReset: false,
      enabled: true,
    };

    expect(recoveryActionLabel(action)).toBe('Acknowledge fault');
    expect(recoveryActionConfirmationText(action)).toContain('Motion remains stopped');
    expect(recoveryActionConfirmationText(action)).toContain('Resume motion separately');
  });

  it('projects one primary and one reset action into the transport surface', () => {
    const primary: CycleRecoveryAction = {
      actionId: 'retry_motion_from_current_state',
      labelKey: 'cycle.recovery.retry_motion',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: true,
    };
    const reset: CycleRecoveryAction = {
      actionId: 'reset_cycle',
      labelKey: 'cycle.recovery.reset_cycle',
      requiresConfirmation: true,
      performsBackendReset: true,
      enabled: true,
    };

    expect(
      recoveryActionsForTransport(
        offer({
          status: 'retry_ready',
          actions: [primary, reset],
          primaryAction: primary.actionId,
        }),
      ),
    ).toEqual({ primaryAction: primary, resetAction: reset });
    expect(
      recoveryActionsForTransport(
        offer({
          status: 'reset_required',
          actions: [reset],
          primaryAction: reset.actionId,
        }),
      ),
    ).toEqual({ primaryAction: undefined, resetAction: reset });
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

  it('keeps a confirmation bound to its exact rendered action', () => {
    const reset: CycleRecoveryAction = {
      actionId: 'reset_cycle',
      labelKey: 'cycle.recovery.reset_cycle',
      requiresConfirmation: true,
      performsBackendReset: true,
      enabled: true,
    };
    const first = offer({ actions: [reset] });
    const confirmation = {
      offerId: first.offerId,
      offerRevision: first.offerRevision,
      actionId: reset.actionId,
    };

    expect(recoveryConfirmationAction(first, confirmation)).toBe(reset);
    expect(
      recoveryConfirmationAction(
        offer({
          offerId: first.offerId,
          offerRevision: first.offerRevision,
          actions: [],
        }),
        confirmation,
      ),
    ).toBeUndefined();
  });

  it('keeps a backend recovery rejection readable at the transport surface', () => {
    expect(
      recoveryCommandFailureMessage(
        new Error('The recovery offer changed; review the current action'),
      ),
    ).toBe('The recovery offer changed; review the current action');
    expect(recoveryCommandFailureMessage({})).toBe(
      'The recovery action could not be applied.',
    );

    const html = renderToStaticMarkup(
      createElement(RecoveryCommandNotice, {
        message: 'The recovery offer changed; review the current action',
        onDismiss: () => undefined,
      }),
    );
    expect(html).toContain('role="alert"');
    expect(html).toContain('Recovery action did not start.');
    expect(html).toContain('The recovery offer changed; review the current action');
    expect(html).toContain('Dismiss recovery action message');
  });
});
