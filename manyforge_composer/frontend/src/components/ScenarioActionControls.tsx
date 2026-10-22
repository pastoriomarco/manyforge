import { useCallback, useEffect, useRef, useState, type MouseEvent } from 'react';

import { DEACTIVATION_REQUIRED_STATES, type CCLCController } from '../hooks/useCCLC';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ResetApplyController } from '../hooks/useResetApply';
import { programActionUnavailableReason, programApplyScenarioReason } from '../state/authoringWorkspace';
import {
  deploymentActionUnavailableReason,
  deploymentSavedDiffersFromApplied,
  deploymentUnapplied,
  scenarioActionRoute,
  type ScenarioAction,
} from '../state/documentStates';
import { globalRulesReadingComponents } from '../utils/interruptAuthoring';
import { DeploymentApplyImpactSummary } from './DeploymentDocumentControls';
import { OverlayPanel } from './OverlayPanel';

/** Changed components a confirmed Apply must deactivate first (Q107), read fresh. */
interface DeactivationOffer {
  token: string;
  phase: 'reading' | 'ready' | 'deactivating' | 'awaiting_admission' | 'applying' | 'stopped';
  /** The admission the last Disable used; Apply waits for a newer one. */
  consumedAdmissionGeneration?: number | null;
  identity: string | null;
  /** In reverse activation order: the order Deactivate runs. */
  components: Array<{ id: string; state: string; reasons: string[] }>;
  message: string | null;
}

function actionTitle(action: ScenarioAction): string {
  switch (action) {
    case 'apply': return 'Apply Scenario changes?';
    case 'revert': return 'Revert Scenario to saved?';
    case 'discard': return 'Discard Scenario changes?';
  }
}

/**
 * The one Discard / Revert / Apply set of the open Scenario.
 *
 * Each action covers every document with changes, whichever workspace is
 * shown: whenever the Deployment differs it carries the action, together with
 * the Program, as one activation; otherwise the Program-only path does, which
 * never changes components or the Scene. Discard is Draft-only.
 */
export function ScenarioActionControls({
  program,
  deployment,
  runtime,
  resetApply = null,
  localInvalidEdit = false,
  cclc = null,
  authoringAdmissionGeneration = null,
}: {
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  runtime: ProgramRuntimeController;
  /** Apply while a Program run exists Resets it first (one confirmation). */
  resetApply?: ResetApplyController | null;
  localInvalidEdit?: boolean;
  /** The ordinary component lifecycle owner: reads states and deactivates. */
  cclc?: CCLCController | null;
  /** The current authoring admission generation from the live status. */
  authoringAdmissionGeneration?: number | null;
}) {
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [applyRefusal, setApplyRefusal] = useState<string | null>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  const programWorkspace = program.state.workspace;
  const deploymentWorkspace = deployment.workspace;
  const resetsRun = resetApply?.runExists === true;

  // Program reasons: the same order the Program controls always used.
  const programBusy = program.state.status === 'loading'
    || (program.state.status === 'ready' && program.state.pendingAction !== null);
  const programBusyReason = programBusy ? 'Another Program operation is in progress.' : null;
  const programApplyCommon = programBusyReason ?? (resetsRun && resetApply !== null
    ? resetApply.unavailableReason('program') : program.documentUnavailableReason);
  const currentScenario = runtime.state.currentScenario?.currentScenario ?? null;
  const currentScenarioUnavailable = programApplyScenarioReason(runtime.state.currentScenario);
  const invalidEditReason = localInvalidEdit ? 'Review or discard the invalid local field edit first.' : null;
  const programReasons = {
    apply: programActionUnavailableReason(programWorkspace, 'apply', programApplyCommon)
      ?? invalidEditReason ?? currentScenarioUnavailable,
    revert: programActionUnavailableReason(programWorkspace, 'revert', programBusyReason ?? program.documentUnavailableReason),
    discard: programActionUnavailableReason(programWorkspace, 'discard', programBusyReason ?? program.mutationUnavailableReason),
  };
  const programSavedDiffers = programWorkspace?.saved.available === true
    && programWorkspace.saved.programRevision !== programWorkspace.applied.programRevision;
  const programPending = {
    apply: programWorkspace?.applied.matchesDraft === false,
    revert: programSavedDiffers,
    discard: programWorkspace?.hasUnappliedChanges === true,
  };

  // Deployment reasons: the same order the Deployment controls always used.
  const deploymentBusyReason = deployment.busy ? 'Another Deployment operation is in progress.' : null;
  const deploymentApplyCommon = deploymentBusyReason ?? (resetsRun && resetApply !== null
    ? resetApply.unavailableReason('deployment') : deployment.documentUnavailableReason);
  // A Deployment Apply carries the Program Draft, which must be complete.
  const programIncomplete = programWorkspace?.draft.program === null
    ? programWorkspace.draft.diagnostics[0]?.message ?? 'Complete the Program Draft before Apply.'
    : null;
  const deploymentReasons = {
    apply: deploymentActionUnavailableReason(deploymentWorkspace, 'apply', deploymentApplyCommon)
      ?? invalidEditReason ?? programIncomplete,
    revert: deploymentActionUnavailableReason(
      deploymentWorkspace, 'revert', deploymentBusyReason ?? deployment.documentUnavailableReason,
    ),
    discard: deploymentActionUnavailableReason(
      deploymentWorkspace, 'discard', deploymentBusyReason ?? deployment.mutationUnavailableReason,
    ),
  };
  const deploymentPending = {
    apply: deploymentUnapplied(deploymentWorkspace),
    revert: deploymentSavedDiffersFromApplied(deploymentWorkspace),
    discard: deploymentUnapplied(deploymentWorkspace),
  };

  const route = (next: ScenarioAction) => scenarioActionRoute(
    next,
    { reason: programReasons[next], pending: programPending[next] },
    { reason: deploymentReasons[next], pending: deploymentPending[next] },
    deployment.comparisonUnavailableReason,
  );
  const routes = { apply: route('apply'), revert: route('revert'), discard: route('discard') };

  useEffect(() => {
    if (!programWorkspace?.hasUnsavedChanges && !programWorkspace?.hasUnappliedChanges) return undefined;
    const protectDirtyDraft = (event: BeforeUnloadEvent) => {
      event.preventDefault();
      event.returnValue = '';
    };
    window.addEventListener('beforeunload', protectDirtyDraft);
    return () => window.removeEventListener('beforeunload', protectDirtyDraft);
  }, [programWorkspace?.hasUnappliedChanges, programWorkspace?.hasUnsavedChanges]);

  const closeAction = useCallback(() => {
    setAction(null);
    setApplyRefusal(null);
  }, []);
  const beginAction = (next: ScenarioAction, event: MouseEvent<HTMLButtonElement>) => {
    if (routes[next].target === null) return;
    returnFocus.current = event.currentTarget;
    // A Deployment Apply without a run goes straight to Manager, which asks
    // for the exact impact confirmation below.
    if (next === 'apply' && routes.apply.target === 'deployment' && !resetsRun) {
      void deployment.applyDraft();
      return;
    }
    setAction(next);
  };
  const confirm = () => {
    if (action === null) return;
    const target = routes[action].target;
    if (target === null) return;
    if (action === 'apply' && resetsRun && resetApply !== null) {
      void resetApply.confirm(target === 'deployment' ? 'deployment' : 'program').then((refusal) => {
        if (refusal === null) closeAction();
        else setApplyRefusal(refusal);
      });
      return;
    }
    closeAction();
    if (action === 'discard') program.discardScenario();
    else if (action === 'revert') {
      if (target === 'deployment') void deployment.revert();
      else program.revert();
    } else program.apply();
  };

  // The Deployment Apply confirmation: Manager's exact impact, and the changed
  // components that must be deactivated first.
  const pendingConfirmation = deployment.pendingApplyConfirmation;
  // A confirmed Revert restores the Saved documents; it never applies the Draft.
  const reverting = deployment.pendingConfirmationAction === 'revert';
  const [deactivation, setDeactivation] = useState<DeactivationOffer | null>(null);
  const confirmationToken = pendingConfirmation?.result.confirmationToken ?? null;
  const changedComponents = pendingConfirmation?.result.candidatePreview?.impact.components
    .filter((row) => row.disposition === 'replace' || row.disposition === 'remove') ?? [];
  const changedKey = JSON.stringify(changedComponents);
  useEffect(() => {
    const changed = JSON.parse(changedKey) as typeof changedComponents;
    if (confirmationToken === null || cclc === null || changed.length === 0) {
      setDeactivation(null);
      return undefined;
    }
    const token = confirmationToken;
    const abort = new AbortController();
    setDeactivation({ token, phase: 'reading', identity: null, components: [], message: null });
    cclc.readActivationSequence(abort.signal).then((observation) => {
      if (abort.signal.aborted) return;
      const components = observation.components
        .filter((component) => DEACTIVATION_REQUIRED_STATES.includes(component.lifecycleState))
        .flatMap((component) => {
          const row = changed.find((candidate) => candidate.controllable_component_id === component.controllableComponentId);
          return row === undefined ? [] : [{
            id: component.controllableComponentId, state: component.lifecycleState, reasons: row.replacement_reasons,
          }];
        })
        .reverse();
      setDeactivation({ token, phase: 'ready', identity: observation.identity, components, message: null });
    }, (reason: unknown) => {
      if (abort.signal.aborted) return;
      setDeactivation({ token, phase: 'stopped', identity: null, components: [], message:
        `Component states are unavailable (${reason instanceof Error ? reason.message : 'read failed'}); `
        + 'Apply is refused if a changed component is still active.' });
    });
    return () => abort.abort();
    // The offer belongs to one exact confirmation and impact.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [confirmationToken, changedKey, cclc === null]);
  const offer = deactivation !== null && deactivation.token === confirmationToken ? deactivation : null;
  const deactivationNeeded = offer !== null && offer.components.length > 0;
  // Planned deactivation leaves a rule waiting for its source, never latched.
  const waitingRules = deactivationNeeded
    ? globalRulesReadingComponents(deploymentWorkspace?.draft.document ?? null, offer.components.map((row) => row.id))
    : [];
  // Each Disable consumes authoring admission: the Apply must be built from the
  // latest controller once the live status shows a newer admission generation.
  const latestDeployment = useRef(deployment);
  latestDeployment.current = deployment;
  const deactivateAndApply = async () => {
    if (cclc === null || offer === null || offer.phase !== 'ready' || offer.identity === null) return;
    setDeactivation({ ...offer, phase: 'deactivating', message: null });
    const result = await cclc.deactivateComponents(offer.components.map((row) => row.id), offer.identity);
    if (!result.succeeded) {
      setDeactivation({ ...offer, phase: 'stopped',
        message: 'Deactivation stopped; Apply was not sent. See component lifecycle details.' });
      return;
    }
    setDeactivation({ ...offer, phase: 'awaiting_admission', message: null,
      consumedAdmissionGeneration: result.consumedAdmissionGeneration });
  };
  const awaitingGeneration = offer?.phase === 'awaiting_admission' ? offer.consumedAdmissionGeneration ?? null : undefined;
  useEffect(() => {
    if (awaitingGeneration === undefined || deactivation === null) return;
    if (awaitingGeneration !== null && (authoringAdmissionGeneration === null
      || authoringAdmissionGeneration <= awaitingGeneration)) return;
    if (latestDeployment.current.documentUnavailableReason !== null) return;
    setDeactivation({ ...deactivation, phase: 'applying' });
    void latestDeployment.current.confirmApply();
  }, [authoringAdmissionGeneration, awaitingGeneration, deployment.documentUnavailableReason, deactivation]);
  const cancelConfirmation = () => {
    if (offer?.phase === 'deactivating') cclc?.cancelActivation();
    setDeactivation(null);
    deployment.cancelApplyConfirmation();
  };
  // A confirmed Revert is gated as a Revert of the Saved input, never as an Apply of the Draft.
  const confirmationReason = reverting ? deploymentReasons.revert : deploymentReasons.apply;

  const applying = deployment.applying || program.state.pendingAction === 'apply';
  const selected = action === null ? null : routes[action];
  const covers = (target: 'deployment' | 'program' | 'scenario' | null) => target === 'deployment'
    ? 'the Deployment and the Program'
    : 'the Program; the Deployment and its components do not change';

  return (
    <>
      <button
        type="button"
        disabled={routes.discard.target === null}
        title={routes.discard.reason ?? 'Discard unapplied Draft changes and return to Applied.'}
        onClick={(event) => beginAction('discard', event)}
      >Discard</button>
      <button
        type="button"
        disabled={routes.revert.target === null}
        title={routes.revert.reason ?? `Restore Draft and Applied to Saved: ${covers(routes.revert.target)}.`}
        onClick={(event) => beginAction('revert', event)}
      >Revert</button>
      <button
        type="button"
        className="primary-action"
        disabled={routes.apply.target === null}
        title={routes.apply.reason ?? `${resetsRun ? 'Reset the current run, then apply' : 'Apply'} ${covers(routes.apply.target)}.`}
        onClick={(event) => beginAction('apply', event)}
      >{applying ? 'Applying' : 'Apply'}</button>

      <OverlayPanel
        open={action !== null}
        title={action === null ? 'Scenario action' : actionTitle(action)}
        titleId="scenario-action-dialog-title"
        compact
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {selected !== null && selected.reason !== null && <p role="status">{selected.reason}</p>}
        {action === 'discard' && (
          <div className="dialog-copy compact-confirmation">
            <p>
              Every Draft with unapplied changes returns to Applied
              {programPending.discard && deploymentPending.discard ? ' (Program and Deployment)'
                : deploymentPending.discard ? ' (Deployment)' : ' (Program)'}.
              {' '}Saved and the running cell do not change.
            </p>
            <button type="button" className="primary-action" disabled={routes.discard.target === null} onClick={confirm}>Discard changes</button>
          </div>
        )}
        {action === 'revert' && (
          <div className="dialog-copy compact-confirmation">
            <p>
              Replace the Draft and Applied with the Saved Scenario: {covers(routes.revert.target)}.
              {routes.revert.target === 'deployment'
                && ' Changed components must be inactive first; Manager refuses to change an active one.'}
            </p>
            <button type="button" className="primary-action" disabled={routes.revert.target === null} onClick={confirm}>Revert to saved</button>
          </div>
        )}
        {action === 'apply' && (
          <div className="dialog-copy compact-confirmation">
            {routes.apply.target === 'program' && (currentScenarioUnavailable === null ? (
              <p>
                {currentScenario
                  ? <>Apply this Program Draft to <strong>{currentScenario.scenarioId}</strong>?</>
                  : 'Apply this Program Draft as the current Scenario’s first Program?'}
                {' '}Its Deployment will not change.
              </p>
            ) : (
              <div className="document-apply-blocked" role="alert">
                <strong>No exact current Scenario.</strong>
                <span>{currentScenarioUnavailable}</span>
                <span>Use Load Scenario to activate a complete Program, Deployment, and Binding.</span>
              </div>
            ))}
            {routes.apply.target === 'program' && !programWorkspace?.applicability.applyReady && (
              <div className="document-apply-blocked" role="alert">
                <strong>Draft is not Apply-ready.</strong>
                <span>{programWorkspace?.applicability.unavailableReason?.message}</span>
              </div>
            )}
            {routes.apply.target === 'deployment' && (
              <p>
                Apply the Deployment and Program Drafts to runtime? Manager confirms any
                disruptive impact before it changes components.
              </p>
            )}
            {resetsRun && <ResetsRunWarning />}
            {applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <button type="button" className="primary-action" disabled={routes.apply.target === null} onClick={confirm}>
              {resetsRun ? 'Reset run and Apply' : 'Apply'}
            </button>
          </div>
        )}
      </OverlayPanel>

      {pendingConfirmation !== null && (
        <OverlayPanel
          open
          title={deactivationNeeded
            ? `Deactivate components and ${reverting ? 'revert' : 'apply'}?`
            : reverting ? 'Revert Deployment configuration?' : 'Apply Deployment configuration change?'}
          titleId="deployment-apply-confirmation-title"
          kind="dialog"
          onClose={cancelConfirmation}
          compact
        >
          <div className="dialog-copy transport-confirmation">
            {confirmationReason !== null && <p role="status">{confirmationReason}</p>}
            {deactivationNeeded ? <>
              <p>This {reverting ? 'Revert' : 'Apply'} changes these components, which must be inactive first:</p>
              <ul>{offer.components.map((row) => (
                <li key={row.id}>
                  {row.id} ({row.state}){row.reasons.length > 0 && ` · changed: ${row.reasons.join(', ')}`}
                </li>
              ))}</ul>
              <p>
                Deactivate disables them in reverse Deployment order, then {reverting
                  ? 'restores the Saved Deployment and Saved Program'
                  : 'applies this exact Deployment Draft'}.
                Changed and new components stay inactive; activate them separately.
                {' '}Running operations remain protected by Manager admission.
              </p>
              {waitingRules.map((rule) => (
                <p key={rule.ruleId}>
                  Global rule {rule.ruleId} will wait for its source ({rule.componentIds.join(', ')} inactive):
                  {' '}motion in its scope stays blocked until {rule.componentIds.length === 1 ? 'it is' : 'they are'} active again.
                </p>
              ))}
            </> : (
              <p>
                {reverting
                  ? 'Restoring the Saved Deployment and Saved Program changes the cell configuration.'
                  : 'Applying this exact Deployment Draft changes the cell configuration.'} Changed or removed components must be inactive before Apply: deactivate them in Components first. New components start inactive; activate them separately.
                {' '}Running operations remain protected by Manager admission.
              </p>
            )}
            {offer?.phase === 'reading' && <p role="status">Reading component states…</p>}
            {offer?.phase === 'deactivating' && <p role="status">{cclc?.activationProgress ?? 'Deactivating components…'}</p>}
            {offer?.phase === 'awaiting_admission' && <p role="status">Components deactivated; waiting for updated backend authoring permissions…</p>}
            {offer?.message != null && <p className="inline-message severity-error" role="alert">{offer.message}</p>}
            {pendingConfirmation.result.candidatePreview !== null && (
              <DeploymentApplyImpactSummary
                impact={pendingConfirmation.result.candidatePreview.impact}
              />
            )}
            <p>
              <strong>{pendingConfirmation.result.preparedActivation.deploymentArtifactId}</strong>
              {' · '}
              <code>{pendingConfirmation.result.preparedActivation.deploymentRevision}</code>
            </p>
            <div className="dialog-actions">
              <button type="button" onClick={cancelConfirmation}>Cancel</button>
              {deactivationNeeded ? (
                <button
                  type="button"
                  className="danger-button"
                  disabled={confirmationReason !== null || offer.phase !== 'ready'}
                  title={deploymentApplyCommon ?? 'Deactivate the listed components, then apply the validated Deployment.'}
                  onClick={() => { void deactivateAndApply(); }}
                >{offer.phase === 'deactivating' ? 'Deactivating'
                  : offer.phase === 'awaiting_admission' || offer.phase === 'applying' || deployment.applying
                    ? 'Applying Deployment' : reverting ? 'Deactivate and revert' : 'Deactivate and apply'}</button>
              ) : (
                <button
                  type="button"
                  className="primary-action"
                  disabled={confirmationReason !== null || offer?.phase === 'reading'}
                  title={deploymentApplyCommon ?? 'Apply the validated Deployment to runtime.'}
                  onClick={() => { void deployment.confirmApply(); }}
                >{deployment.applying ? 'Applying Deployment'
                  : reverting ? 'Confirm revert' : 'Confirm apply'}</button>
              )}
            </div>
          </div>
        </OverlayPanel>
      )}
    </>
  );
}

/**
 * The single, visually clear warning of an Apply while a Program run exists.
 * It is part of Apply's ordinary confirmation: no second dialog.
 */
export function ResetsRunWarning() {
  return (
    <div className="document-apply-blocked document-apply-resets-run" role="alert">
      <strong>Applying Resets the current run.</strong>
      <span>
        The whole run ends, including any repeating part, and does not resume
        where it stopped. When the cycle is Idle, exactly this Draft is
        Applied. Nothing is saved, executed or run automatically.
      </span>
    </div>
  );
}
