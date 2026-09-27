export interface RecoveryActionView {
  actionId: string;
  label: string;
  requiresConfirmation: boolean;
  performsBackendReset: boolean;
  enabled: boolean;
}

export interface RecoveryOfferView {
  offerId: string;
  offerRevision: number;
  status: string;
  headline?: string;
  actions: RecoveryActionView[];
  primaryAction?: string | null;
}

export function recoveryOfferHeadline(offer: RecoveryOfferView): string {
  const supplied = offer.headline?.trim();
  return supplied || offer.status.replaceAll('_', ' ');
}

export function recoveryActionsForPresentation(offer: RecoveryOfferView | null): {
  primaryAction: RecoveryActionView | undefined;
  resetAction: RecoveryActionView | undefined;
} {
  if (offer === null) {
    return { primaryAction: undefined, resetAction: undefined };
  }
  const offeredPrimary = offer.actions.find(
    (action) => action.actionId === offer.primaryAction,
  );
  return {
    primaryAction: offeredPrimary?.performsBackendReset
      ? undefined
      : offeredPrimary,
    resetAction: offer.actions.find((action) => action.performsBackendReset),
  };
}

export interface RecoveryConfirmation {
  offerId: string;
  offerRevision: number;
  actionId: string;
}

export function recoveryConfirmationAction(
  offer: RecoveryOfferView,
  confirmation: RecoveryConfirmation | null,
): RecoveryActionView | undefined {
  if (
    confirmation === null
    || confirmation.offerId !== offer.offerId
    || confirmation.offerRevision !== offer.offerRevision
  ) {
    return undefined;
  }
  return offer.actions.find((action) => action.actionId === confirmation.actionId);
}

export function recoveryActionLabel(action: RecoveryActionView): string {
  return action.label.trim() || action.actionId.replaceAll('_', ' ');
}

export function recoveryCommandFailureMessage(error: unknown): string {
  if (error instanceof Error && error.message.trim()) return error.message;
  const message = String(error).trim();
  return message && message !== '[object Object]'
    ? message
    : 'The recovery action could not be applied.';
}

export function RecoveryCommandNotice({
  message,
  onDismiss,
}: {
  message: string;
  onDismiss: () => void;
}) {
  return (
    <div role="alert" className="recovery-command-notice notice severity-warning">
      <span>
        <strong>Recovery action did not start.</strong> {message}
      </span>
      <button
        type="button"
        onClick={onDismiss}
        aria-label="Dismiss recovery action message"
      >
        ×
      </button>
    </div>
  );
}
