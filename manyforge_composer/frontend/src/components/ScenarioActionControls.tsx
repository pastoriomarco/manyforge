import { useCallback, useEffect, useRef, useState, type MouseEvent } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ResetApplyController } from '../hooks/useResetApply';
import { programActionUnavailableReason, programApplyScenarioReason } from '../state/authoringWorkspace';
import { busyLabel, busyReason } from '../state/busyOperation';
import {
  deploymentActionUnavailableReason,
  deploymentSavedDiffersFromApplied,
  deploymentUnapplied,
  scenarioActionRoute,
  type ScenarioAction,
} from '../state/documentStates';
import { ApplyImpact } from './ApplyImpact';
import { useAuthoringAdmissionTooltip } from './authoringAdmission';
import { OverlayPanel } from './OverlayPanel';

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
 * never changes components or the Scene. Discard is Draft-only. A Deployment
 * Apply is the server's one Apply path: Preview, confirm the
 * per-component impact, Apply; the server disables, applies and prepares.
 */
export function ScenarioActionControls({
  program,
  deployment,
  runtime,
  resetApply = null,
  localInvalidEdit = false,
  authoringAdmissionAllowed = null,
}: {
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  runtime: ProgramRuntimeController;
  /** Apply while a Program run exists Resets it first (one confirmation). */
  resetApply?: ResetApplyController | null;
  localInvalidEdit?: boolean;
  /** Current Headless authoring admission; unknown leaves existing behavior unchanged. */
  authoringAdmissionAllowed?: boolean | null;
}) {
  const authoringAdmissionTooltip = useAuthoringAdmissionTooltip();
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [applyRefusal, setApplyRefusal] = useState<string | null>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  const programWorkspace = program.state.workspace;
  const deploymentWorkspace = deployment.workspace;
  const authoringAdmissionBlock = authoringAdmissionAllowed === false
    ? authoringAdmissionTooltip : null;
  const resetsRun = resetApply?.runExists === true;

  // Program reasons: the same order the Program controls always used.
  const programBusy = program.state.status === 'loading'
    || (program.state.status === 'ready' && program.state.pendingAction !== null);
  // The one operation in progress (any document's) names itself in the tooltips.
  const operation = program.busyOperation ?? deployment.busyOperation;
  const operationReason = busyReason(operation);
  const programBusyReason = programBusy ? operationReason ?? 'Another Program operation is in progress.' : null;
  const programApplyCommon = programBusyReason ?? (resetsRun && resetApply !== null
    ? resetApply.unavailableReason() : program.documentUnavailableReason);
  const currentScenario = runtime.state.currentScenario?.currentScenario ?? null;
  const currentScenarioUnavailable = programApplyScenarioReason(runtime.state.currentScenario);
  const invalidEditReason = localInvalidEdit ? 'Review or discard the invalid local field edit first.' : null;
  const programReasons = {
    apply: authoringAdmissionBlock
      ?? programActionUnavailableReason(programWorkspace, 'apply', programApplyCommon)
      ?? invalidEditReason ?? currentScenarioUnavailable,
    revert: authoringAdmissionBlock
      ?? programActionUnavailableReason(programWorkspace, 'revert', programBusyReason ?? program.documentUnavailableReason),
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
  const deploymentBusyReason = deployment.busy ? operationReason ?? 'Another Deployment operation is in progress.' : null;
  // While a run exists the Deployment Apply Resets a stopped or held run first
  // (one shared Reset and Apply); a finished run awaiting Reset is
  // reset by the server's Apply itself.
  const deploymentResetsRun = resetApply?.deploymentRunReset === true;
  const deploymentApplyCommon = deploymentBusyReason ?? (resetApply === null || !resetApply.runExists
    ? deployment.documentUnavailableReason
    : deploymentResetsRun ? resetApply.unavailableReason('deployment') : deployment.mutationUnavailableReason);
  // A Deployment Apply carries the Program Draft, which must be complete.
  const programIncomplete = programWorkspace?.draft.program === null
    ? programWorkspace.draft.diagnostics[0]?.message ?? 'Complete the Program Draft before Apply.'
    : null;
  const deploymentReasons = {
    apply: authoringAdmissionBlock
      ?? deploymentActionUnavailableReason(deploymentWorkspace, 'apply', deploymentApplyCommon)
      ?? invalidEditReason ?? programIncomplete,
    revert: authoringAdmissionBlock ?? deploymentActionUnavailableReason(
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
    // A Deployment Apply previews first; its impact is confirmed below. The
    // server refuses it while a run is in progress (Reset first).
    if (next === 'apply' && routes.apply.target === 'deployment') {
      void deployment.applyDraft();
      return;
    }
    // A Deployment Revert is an Apply of the Saved Scenario: previewed and
    // confirmed the same way.
    if (next === 'revert' && routes.revert.target === 'deployment') {
      void deployment.revert();
      return;
    }
    setAction(next);
  };
  const confirm = () => {
    if (action === null) return;
    const target = routes[action].target;
    if (target === null) return;
    if (action === 'apply' && resetsRun && resetApply !== null) {
      void resetApply.confirm().then((refusal) => {
        if (refusal === null) closeAction();
        else setApplyRefusal(refusal);
      });
      return;
    }
    closeAction();
    if (action === 'discard') program.discardScenario();
    else if (action === 'revert') program.revert();
    else program.apply();
  };

  // The Deployment Apply (or Revert) confirmation: its Preview.
  const preview = deployment.applyPreview;
  const reverting = deployment.applyPreviewKind === 'revert';
  const previewResetsRun = deploymentResetsRun && !reverting && resetApply !== null;
  const cancelPreview = () => {
    setApplyRefusal(null);
    deployment.cancelApplyConfirmation();
  };
  const confirmPreview = () => {
    if (preview === null) return;
    if (!previewResetsRun || resetApply === null) {
      void deployment.confirmApply();
      return;
    }
    void resetApply.confirm('deployment', preview.scope).then((refusal) => {
      if (refusal === null) {
        setApplyRefusal(null);
        deployment.cancelApplyConfirmation();
      } else setApplyRefusal(refusal);
    });
  };
  const confirmationReason = reverting ? deploymentReasons.revert : deploymentReasons.apply;

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
      >{busyLabel(operation, 'discard', 'Discard')}</button>
      <button
        type="button"
        disabled={routes.revert.target === null}
        title={routes.revert.reason ?? `Restore Draft and Applied to Saved: ${covers(routes.revert.target)}.`}
        onClick={(event) => beginAction('revert', event)}
      >{busyLabel(operation, ['revert_preview', 'revert'], 'Revert')}</button>
      <button
        type="button"
        className="primary-action"
        disabled={routes.apply.target === null}
        title={routes.apply.reason ?? `${resetsRun && routes.apply.target === 'program' ? 'Reset the current run, then apply' : 'Apply'} ${covers(routes.apply.target)}.`}
        onClick={(event) => beginAction('apply', event)}
      >{busyLabel(operation, ['apply_preview', 'apply'], 'Apply')}</button>

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
            <div className="dialog-actions">
              <button type="button" className="primary-action" disabled={routes.discard.target === null} onClick={confirm}>Discard changes</button>
            </div>
          </div>
        )}
        {action === 'revert' && (
          <div className="dialog-copy compact-confirmation">
            <p>Replace the Draft and Applied with the Saved Scenario: {covers(routes.revert.target)}.</p>
            <div className="dialog-actions">
              <button type="button" className="primary-action" disabled={routes.revert.target === null}
                title={authoringAdmissionBlock ?? undefined} onClick={confirm}>Revert to saved</button>
            </div>
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
            {resetsRun && <ResetsRunWarning />}
            {applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <div className="dialog-actions">
              <button type="button" className="primary-action" disabled={routes.apply.target === null}
                title={authoringAdmissionBlock ?? undefined} onClick={confirm}>
                {resetsRun ? 'Reset run and Apply' : 'Apply'}
              </button>
            </div>
          </div>
        )}
      </OverlayPanel>

      {deployment.applyConfirmationOpen && (
        <OverlayPanel
          open
          title={reverting ? 'Revert Deployment to saved?' : 'Apply Deployment configuration change?'}
          titleId="deployment-apply-confirmation-title"
          kind="dialog"
          onClose={cancelPreview}
          compact
        >
          <div className="dialog-copy transport-confirmation">
            {preview !== null && confirmationReason !== null && <p role="status">{confirmationReason}</p>}
            {reverting && <p>Both Drafts return to the Saved Scenario, which is applied.</p>}
            {preview === null
              ? <p className="inline-message severity-info" role="status">Previewing impact…</p>
              : <ApplyImpact preview={preview} />}
            {previewResetsRun && <ResetsRunWarning />}
            {previewResetsRun && applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <div className="dialog-actions">
              <button type="button" onClick={cancelPreview}>Cancel</button>
              <button
                type="button"
                className="primary-action"
                disabled={preview === null || confirmationReason !== null}
                title={authoringAdmissionBlock ?? (preview === null ? 'Previewing impact…' : deploymentApplyCommon
                  ?? 'Apply the Deployment: affected components are disabled, applied and prepared again.')}
                onClick={confirmPreview}
              >{deployment.applying ? 'Applying Deployment' : reverting ? 'Confirm revert'
                : previewResetsRun ? 'Reset run and Apply' : 'Confirm apply'}</button>
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
