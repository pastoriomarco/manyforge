import type { ActivateExecuteController } from '../hooks/useActivateExecute';
import { componentSummary, recoveryCauseText } from '../state/componentPresentation';
import { recoveryBlockers } from '../state/recoverEnable';
import { useAuthoringAdmissionTooltip } from './authoringAdmission';
import { OverlayPanel } from './OverlayPanel';

const PHASE_TEXT: Partial<Record<string, string>> = {
  prepare: 'Preparing components…',
  recover: 'Recovering and enabling…',
  enable: 'Enabling components…',
  waiting: 'Waiting for Execute readiness…',
};

/**
 * "Activate components and execute?": the components
 * that aren't enabled, each with its live state and any failure, while
 * Activate runs Prepare all and Enable all, then Execute once.
 */
export function ActivateExecuteDialog({
  activation,
  authoringAdmissionAllowed = null,
}: {
  activation: ActivateExecuteController;
  /** Current Headless authoring admission; unknown leaves existing behavior unchanged. */
  authoringAdmissionAllowed?: boolean | null;
}) {
  const authoringAdmissionTooltip = useAuthoringAdmissionTooltip();
  const { flow, states } = activation;
  const progress = flow === null ? undefined : PHASE_TEXT[flow.phase];
  const failed = flow?.phase === 'failed';
  const failureOf = (componentId: string | null) => flow?.failures
    .filter((failure) => failure.componentId === componentId).map((failure) => failure.text) ?? [];
  // The simulator is never activated here (maintainer, 2026-11-06): it is not listed.
  const simulator = (componentId: string) => states.find((candidate) => candidate.component_id === componentId)
    ?.capabilities.includes('sim_reset') === true;
  const listed = flow === null ? [] : [...flow.componentIds, ...flow.failures.flatMap((failure) => (
    failure.componentId !== null && !flow.componentIds.includes(failure.componentId) ? [failure.componentId] : []))]
    .filter((componentId) => !simulator(componentId));
  const general = failureOf(null);
  // Needs recovery: each cause once (a machine shared by two members is listed once).
  const preflights = Object.values(flow?.recovery ?? {});
  const reading = preflights.some((preflight) => preflight === null);
  const causes = [...new Map(preflights.flatMap((preflight) => preflight?.components ?? [])
    .filter((row) => row.eligibility !== 'ready')
    .map((row) => [row.component_id, recoveryCauseText(row)])).values()];
  const blockers = [...new Set(preflights.flatMap((preflight) => preflight === null ? [] : recoveryBlockers(preflight)))];
  const recovering = Object.keys(flow?.recovery ?? {}).length > 0;
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
        {recovering && (
          <div data-testid="activation-needs-recovery">
            <p>Needs recovery:</p>
            {reading ? <p role="status">Reading the causes…</p>
              : <ul>{causes.map((text) => <li key={text}>{text}</li>)}</ul>}
            {blockers.length > 0 && (
              <div className="inline-message severity-error" role="alert">
                <p>This can&apos;t be recovered from here:</p>
                {blockers.map((text) => <p key={text}>{text}</p>)}
              </div>
            )}
          </div>
        )}
        <p>
          {recovering
            ? 'Activate prepares these components, recovers and enables the ones above (neither runs a Program motion), enables the rest, then executes the Program once.'
            : 'Activate prepares and enables these components, then executes the Program once.'}
          {' '}It never starts the simulator: play it yourself, or start it from the Program with a simulation node.
        </p>
        {progress !== undefined && <p role="status">{progress}</p>}
        {failed && (
          <div className="inline-message severity-error" role="alert">
            <p>Activation stopped; the Program was not executed.</p>
            {general.map((text) => <p key={text}>{text}</p>)}
          </div>
        )}
        <div className="dialog-actions">
          <button type="button" onClick={activation.cancel}>{failed ? 'Close' : 'Cancel'}</button>
          <button type="button" className="transport-run"
            disabled={flow?.phase !== 'offered' || !activation.recoveryReady || authoringAdmissionAllowed === false}
            title={authoringAdmissionAllowed === false ? authoringAdmissionTooltip : undefined}
            onClick={activation.confirm}>{progress !== undefined ? 'Activating…'
              : recovering ? 'Recover, enable and execute' : 'Activate and execute'}</button>
        </div>
      </div>
    </OverlayPanel>
  );
}
