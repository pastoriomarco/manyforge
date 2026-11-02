import type { ActivateExecuteController } from '../hooks/useActivateExecute';
import { componentSummary } from '../state/componentPresentation';
import { OverlayPanel } from './OverlayPanel';

const PHASE_TEXT: Partial<Record<string, string>> = {
  prepare: 'Preparing components…',
  enable: 'Enabling components…',
  waiting: 'Waiting for Execute readiness…',
};

/**
 * M7's "Activate components and execute?" (decision AUD-7): the components
 * that aren't enabled, each with its live state and any failure, while
 * Activate runs Prepare all and Enable all, then Execute once.
 */
export function ActivateExecuteDialog({ activation }: { activation: ActivateExecuteController }) {
  const { flow, states } = activation;
  const progress = flow === null ? undefined : PHASE_TEXT[flow.phase];
  const failed = flow?.phase === 'failed';
  const failureOf = (componentId: string | null) => flow?.failures
    .filter((failure) => failure.componentId === componentId).map((failure) => failure.text) ?? [];
  const listed = flow === null ? [] : [...flow.componentIds, ...flow.failures.flatMap((failure) => (
    failure.componentId !== null && !flow.componentIds.includes(failure.componentId) ? [failure.componentId] : []))];
  const general = failureOf(null);
  return (
    <OverlayPanel
      open={flow !== null}
      title="Activate components and execute?"
      titleId="activate-execute-title"
      kind="dialog"
      compact
      onClose={activation.cancel}
    >
      <div className="dialog-copy transport-confirmation">
        <p>{flow?.phase === 'offered' ? 'These components are not enabled yet:' : 'Component activation status:'}</p>
        <ul>{listed.map((componentId) => {
          const state = states.find((candidate) => candidate.component_id === componentId);
          const failures = failureOf(componentId);
          return (
            <li key={componentId}>
              {`${componentId} (${state === undefined ? 'not in the cell' : componentSummary(state, states)})`}
              {failures.length > 0 && <strong className="error-state">{` ${failures.join('; ')}`}</strong>}
            </li>
          );
        })}</ul>
        <p>Activate prepares and enables every active component of the Deployment, then executes the Program once.</p>
        {progress !== undefined && <p role="status">{progress}</p>}
        {failed && (
          <div className="inline-message severity-error" role="alert">
            <p>Activation stopped; the Program was not executed.</p>
            {general.map((text) => <p key={text}>{text}</p>)}
          </div>
        )}
        <div className="dialog-actions">
          <button type="button" onClick={activation.cancel}>{failed ? 'Close' : 'Cancel'}</button>
          <button type="button" className="transport-run" disabled={flow?.phase !== 'offered'}
            onClick={activation.confirm}>{progress !== undefined ? 'Activating…' : 'Activate and execute'}</button>
        </div>
      </div>
    </OverlayPanel>
  );
}
