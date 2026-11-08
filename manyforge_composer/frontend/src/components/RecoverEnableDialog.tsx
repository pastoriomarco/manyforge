import { useEffect, useState, type RefObject } from 'react';

import { api } from '../api/client';
import { recoveryCauseText } from '../state/componentPresentation';
import { recoveryBlockers, recoveryConfirmation } from '../state/recoverEnable';
import type { RecoveryConfirmation, RecoveryPreflight } from '../types/components';
import { requestErrorText } from './ComponentStatePanel';
import { OverlayPanel } from './OverlayPanel';

/**
 * "Recover and enable <id>?": what stands in the way (each cause with its
 * code), what will happen, and one confirmation. A cause that can't be
 * recovered here is listed with its required action and the button stays
 * disabled. The backend checks the same facts again before every step.
 */
export function RecoverEnableDialog({ componentId, open, onClose, onConfirm, returnFocusRef }: {
  componentId: string;
  open: boolean;
  onClose: () => void;
  onConfirm: (confirmation: RecoveryConfirmation) => void;
  returnFocusRef?: RefObject<HTMLElement | null>;
}) {
  const [preflight, setPreflight] = useState<RecoveryPreflight | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!open) return;
    const controller = new AbortController();
    setPreflight(null);
    setError(null);
    api.components.recoverEnablePreflight(componentId, controller.signal)
      .then(setPreflight)
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) setError(requestErrorText(reason));
      });
    return () => controller.abort();
  }, [componentId, open]);
  const blockers = preflight === null ? [] : recoveryBlockers(preflight);
  const causes = preflight?.components.filter((row) => row.eligibility !== 'ready') ?? [];
  return (
    <OverlayPanel
      open={open}
      title={`Recover and enable ${componentId}?`}
      titleId={`component-${componentId}-recover-enable-title`}
      kind="dialog"
      compact
      onClose={onClose}
      returnFocusRef={returnFocusRef}
    >
      <div className="dialog-copy transport-confirmation" data-testid="recover-enable-dialog">
        {preflight === null && error === null && <p role="status">Reading the causes…</p>}
        {error !== null && <p className="inline-message severity-error" role="alert">{error}</p>}
        {preflight !== null && (
          <>
            {causes.length > 0 ? (
              <>
                <p>Needs recovery:</p>
                <ul>{causes.map((row) => <li key={row.component_id}>{recoveryCauseText(row)}</li>)}</ul>
                <p>
                  Recover clears these causes, then Enable enables {componentId}.
                  Neither runs a Program motion. If a step fails, the rest isn&apos;t run.
                </p>
              </>
            ) : <p>Nothing to recover: Enable enables {componentId}.</p>}
            {blockers.length > 0 && (
              <div className="inline-message severity-error" role="alert">
                <p>This can&apos;t be recovered from here:</p>
                {blockers.map((text) => <p key={text}>{text}</p>)}
              </div>
            )}
          </>
        )}
        <div className="dialog-actions">
          <button type="button" onClick={onClose}>Cancel</button>
          <button
            type="button"
            className="transport-run"
            disabled={preflight === null || blockers.length > 0}
            onClick={() => { if (preflight !== null) onConfirm(recoveryConfirmation(preflight)); }}
          >Recover and enable</button>
        </div>
      </div>
    </OverlayPanel>
  );
}
