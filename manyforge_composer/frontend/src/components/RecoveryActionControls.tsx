import type {
  CycleRecoveryAction,
  CycleRecoveryOffer,
} from '../types/api';

const MOTION_PRODUCING_RECOVERY_ACTION_IDS = new Set([
  'retry_motion_from_current_state',
  'retry_after_scene_repair',
  'replan_from_stopped_state',
  'retry_runtime_step',
]);

export function isMotionProducingRecoveryAction(actionId: string): boolean {
  return MOTION_PRODUCING_RECOVERY_ACTION_IDS.has(actionId);
}

const ACTION_LABELS: Record<string, string> = {
  'cycle.recovery.retry_motion': 'Retry motion',
  'cycle.recovery.acknowledge_and_continue': 'Acknowledge fault',
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

export function recoveryActionConfirmationText(action: CycleRecoveryAction): string {
  if (action.performsBackendReset) {
    return 'Reset clears the current cycle and rebuilds its runtime state. A commanded simulator deployment may also restore its configured simulator state.';
  }
  if (action.actionId === 'acknowledge_and_continue') {
    return 'This acknowledges the certified fault without resetting the execution backend. Motion remains stopped; press Resume motion separately to start a fresh plan and validation.';
  }
  return 'This accepts the current certified recovery outcome without resetting the execution backend. The next motion is planned and validated from current state.';
}

export function recoveryActionsForTransport(offer: CycleRecoveryOffer | null): {
  primaryAction: CycleRecoveryAction | undefined;
  resetAction: CycleRecoveryAction | undefined;
} {
  if (offer === null) {
    return { primaryAction: undefined, resetAction: undefined };
  }
  const offeredPrimary = offer.actions.find(
    (action) => action.actionId === offer.primaryAction,
  );
  return {
    primaryAction:
      offeredPrimary?.actionId === 'reset_cycle' ? undefined : offeredPrimary,
    resetAction: offer.actions.find(
      (action) => action.actionId === 'reset_cycle',
    ),
  };
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

export function recoveryActionLabel(action: CycleRecoveryAction): string {
  return actionLabel(action);
}
