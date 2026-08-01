/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import {
  RecoveryCommandNotice,
  recoveryActionLabel,
  recoveryActionsForPresentation,
  recoveryCommandFailureMessage,
  recoveryConfirmationAction,
  recoveryOfferHeadline,
  type RecoveryActionView,
  type RecoveryOfferView,
} from './RecoveryActionControls';

const primary: RecoveryActionView = {
  actionId: 'retry',
  label: 'Retry from current state',
  requiresConfirmation: false,
  performsBackendReset: false,
  enabled: true,
};
const reset: RecoveryActionView = {
  actionId: 'reset',
  label: 'Reset',
  requiresConfirmation: true,
  performsBackendReset: true,
  enabled: true,
};

function offer(overrides: Partial<RecoveryOfferView> = {}): RecoveryOfferView {
  return {
    offerId: 'offer-1',
    offerRevision: 1,
    status: 'action_required',
    headline: 'Recovery choice required',
    actions: [primary, reset],
    primaryAction: primary.actionId,
    ...overrides,
  };
}

describe('recovery presentation helpers', () => {
  it('uses server-provided presentation fields without interpreting action IDs', () => {
    expect(recoveryOfferHeadline(offer())).toBe('Recovery choice required');
    expect(recoveryActionLabel(primary)).toBe('Retry from current state');
    expect(recoveryActionsForPresentation(offer())).toEqual({
      primaryAction: primary,
      resetAction: reset,
    });
  });

  it('never promotes a reset-marked action into the primary slot', () => {
    expect(
      recoveryActionsForPresentation(offer({ primaryAction: reset.actionId })),
    ).toEqual({ primaryAction: undefined, resetAction: reset });
  });

  it('binds confirmation to the exact offer revision and current action', () => {
    const confirmation = {
      offerId: 'offer-1',
      offerRevision: 1,
      actionId: 'reset',
    };
    expect(recoveryConfirmationAction(offer(), confirmation)).toBe(reset);
    expect(
      recoveryConfirmationAction(offer({ offerRevision: 2 }), confirmation),
    ).toBeUndefined();
    expect(
      recoveryConfirmationAction(offer({ offerId: 'offer-2' }), confirmation),
    ).toBeUndefined();
    expect(
      recoveryConfirmationAction(offer({ actions: [] }), confirmation),
    ).toBeUndefined();
  });

  it('keeps recovery rejection readable and dismissible', () => {
    expect(recoveryCommandFailureMessage(new Error('Offer changed'))).toBe(
      'Offer changed',
    );
    expect(recoveryCommandFailureMessage({})).toBe(
      'The recovery action could not be applied.',
    );
    const html = renderToStaticMarkup(createElement(RecoveryCommandNotice, {
      message: 'Offer changed',
      onDismiss: () => undefined,
    }));
    expect(html).toContain('role="alert"');
    expect(html).toContain('Offer changed');
    expect(html).toContain('Dismiss recovery action message');
  });
});
