import { useEffect, useState } from 'react';
import type {
  CycleRecoveryAction,
  CycleRecoveryOffer,
} from '../types/api';

const ACTION_LABELS: Record<string, string> = {
  'cycle.recovery.retry_motion': 'Retry motion',
  'cycle.recovery.acknowledge_and_continue': 'Acknowledge and continue',
  'cycle.recovery.retry_after_scene_repair': 'Retry after scene repair',
  'cycle.recovery.replan_from_stopped_state': 'Replan and continue',
  'cycle.recovery.retry_runtime_step': 'Retry step',
  'cycle.recovery.reset_cycle': 'Reset cycle',
};

const WAITING_LABELS: Record<string, string> = {
  recovery_evidence_expired: 'Waiting for fresh observed state',
  cycle_lifecycle_command_pending: 'Waiting for lifecycle command',
  stronger_cycle_authority_active: 'Waiting for active control authority',
  kernel_not_quiescent_for_runtime_step_retry: 'Waiting for motion to become quiescent',
};

function actionLabel(action: CycleRecoveryAction): string {
  return ACTION_LABELS[action.labelKey] ?? action.actionId.replaceAll('_', ' ');
}

export function recoveryOfferHeadline(offer: CycleRecoveryOffer): string {
  if (offer.status === 'waiting') {
    if (offer.recoveryClass === 'runtime_collision') {
      return 'Stopping after collision';
    }
    return WAITING_LABELS[offer.reasonCode] ?? 'Recovery is not ready';
  }
  if (offer.status === 'reset_required') {
    return 'Reset required';
  }
  if (offer.status === 'action_required') {
    return 'Recovery choice required';
  }
  return 'Recovery ready';
}

function confirmationText(action: CycleRecoveryAction): string {
  if (action.performsBackendReset) {
    return 'Reset clears the current cycle and rebuilds its runtime state. A commanded simulator deployment may also restore its configured simulator state.';
  }
  return 'This accepts the current certified recovery outcome without resetting the execution backend. The next motion is planned and validated from current state.';
}

interface RecoveryConfirmation {
  offerId: string;
  offerRevision: number;
  actionId: string;
}

export function recoveryConfirmationAction(
  offer: CycleRecoveryOffer,
  confirmation: RecoveryConfirmation | null,
): CycleRecoveryAction | undefined {
  if (
    confirmation === null ||
    confirmation.offerId !== offer.offerId ||
    confirmation.offerRevision !== offer.offerRevision
  ) {
    return undefined;
  }
  return offer.actions.find(
    (action) => action.actionId === confirmation.actionId,
  );
}

export interface RecoveryActionControlsProps {
  offer: CycleRecoveryOffer;
  busy?: boolean;
  onAction: (
    offer: CycleRecoveryOffer,
    actionId: string,
  ) => void | Promise<void>;
  onError?: (error: unknown) => void;
}

/** Presentation-only control surface for one immutable server-authored offer. */
export function RecoveryActionControls({
  offer,
  busy = false,
  onAction,
  onError,
}: RecoveryActionControlsProps) {
  const [confirmation, setConfirmation] = useState<RecoveryConfirmation | null>(null);

  useEffect(() => {
    setConfirmation(null);
  }, [offer.offerId, offer.offerRevision]);

  const confirmationAction = recoveryConfirmationAction(offer, confirmation);

  const invoke = (action: CycleRecoveryAction) => {
    void Promise.resolve(onAction(offer, action.actionId)).catch((error) => {
      onError?.(error);
    });
  };

  return (
    <div
      data-testid="cycle-recovery-offer"
      data-offer-id={offer.offerId}
      data-offer-revision={offer.offerRevision}
      className="text-xs text-amber-100"
    >
      <div className="font-semibold">{recoveryOfferHeadline(offer)}</div>
      {offer.status === 'waiting' && (
        <div className="mt-0.5 text-amber-200/80">{offer.reasonCode}</div>
      )}
      <div className="mt-2 flex flex-wrap gap-2">
        {offer.actions.map((action) => (
          <button
            key={action.actionId}
            type="button"
            disabled={busy || !action.enabled}
            title={action.disabledReasonCode ?? actionLabel(action)}
            onClick={() => {
              if (action.requiresConfirmation) {
                setConfirmation({
                  offerId: offer.offerId,
                  offerRevision: offer.offerRevision,
                  actionId: action.actionId,
                });
              } else {
                invoke(action);
              }
            }}
            className={`rounded border px-2.5 py-1 font-semibold transition-colors disabled:cursor-not-allowed disabled:border-gray-700 disabled:bg-gray-800 disabled:text-gray-500 ${
              action.performsBackendReset
                ? 'border-amber-500/70 bg-amber-900/60 text-amber-50 hover:bg-amber-800/70'
                : 'border-emerald-500/60 bg-emerald-800/70 text-white hover:bg-emerald-700/80'
            }`}
          >
            {actionLabel(action)}
          </button>
        ))}
      </div>
      {confirmationAction && (
        <div role="dialog" aria-label={`${actionLabel(confirmationAction)} confirmation`} className="mt-2 border-t border-amber-500/40 pt-2">
          <div className="max-w-lg leading-5 text-amber-100/85">
            {confirmationText(confirmationAction)}
          </div>
          <div className="mt-2 flex gap-2">
            <button
              type="button"
              disabled={busy || !confirmationAction.enabled}
              onClick={() => {
                setConfirmation(null);
                invoke(confirmationAction);
              }}
              className="rounded border border-amber-400/70 bg-amber-800 px-2.5 py-1 font-semibold text-white hover:bg-amber-700 disabled:cursor-not-allowed disabled:opacity-50"
            >
              Confirm {actionLabel(confirmationAction)}
            </button>
            <button
              type="button"
              disabled={busy}
              onClick={() => setConfirmation(null)}
              className="rounded border border-gray-600 bg-gray-800 px-2.5 py-1 font-semibold text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
            >
              Cancel
            </button>
          </div>
        </div>
      )}
    </div>
  );
}

export function recoveryActionLabel(action: CycleRecoveryAction): string {
  return actionLabel(action);
}
