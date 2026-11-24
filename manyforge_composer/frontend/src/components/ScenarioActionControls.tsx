import { useCallback, useEffect, useRef, useState, type MouseEvent } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { InitialSceneReloadController } from '../hooks/useInitialSceneReload';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ResetApplyController } from '../hooks/useResetApply';
import {
  programActionUnavailableReason,
  programApplyScenarioReason,
  type AuthoringUnavailableCause,
} from '../state/authoringWorkspace';
import { busyLabel, busyReason } from '../state/busyOperation';
import {
  deploymentActionUnavailableReason,
  deploymentSavedDiffersFromApplied,
  deploymentUnapplied,
  scenarioActionRoute,
  type ScenarioAction,
} from '../state/documentStates';
import {
  PERSISTENT_KEYS_RESET_NOTE,
  RELOAD_INITIAL_SCENE_NOTE,
  declaresPersistentKeys,
  initialSceneReloadOffered,
  type InitialSceneExpectation,
} from '../state/initialSceneApplied';
import { ApplyImpact } from './ApplyImpact';
import { applyAdmissionBlock, useAuthoringAdmissionTooltip } from './authoringAdmission';
import { OverlayPanel } from './OverlayPanel';
import { PersistentKeyResetNotice } from './PersistentKeyValues';

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
  authoringCause = null,
  initialScene = null,
}: {
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  runtime: ProgramRuntimeController;
  /** Apply while a Program run exists Resets it first (one confirmation). */
  resetApply?: ResetApplyController | null;
  localInvalidEdit?: boolean;
  /** Current Headless authoring admission; unknown leaves existing behavior unchanged. */
  authoringAdmissionAllowed?: boolean | null;
  /** Why Headless authoring admission is closed, when it is. */
  authoringCause?: AuthoringUnavailableCause | null;
  /** Offers Reload initial scene with an Apply or Revert. */
  initialScene?: InitialSceneReloadController | null;
}) {
  const authoringAdmissionTooltip = useAuthoringAdmissionTooltip();
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [applyRefusal, setApplyRefusal] = useState<string | null>(null);
  const [reloadScene, setReloadScene] = useState(false);
  const returnFocus = useRef<HTMLElement | null>(null);
  const programWorkspace = program.state.workspace;
  const deploymentWorkspace = deployment.workspace;
  const authoringAdmissionBlock = authoringAdmissionAllowed === false
    ? authoringAdmissionTooltip : null;
  const resetsRun = resetApply?.runExists === true;
  const applyAuthoringBlock = applyAdmissionBlock(authoringAdmissionBlock, resetsRun, authoringCause);

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
    apply: applyAuthoringBlock
      ?? programActionUnavailableReason(programWorkspace, 'apply', programApplyCommon)
      ?? invalidEditReason ?? currentScenarioUnavailable,
    revert: applyAuthoringBlock
      ?? programActionUnavailableReason(programWorkspace, 'revert', programBusyReason ?? (resetsRun && resetApply !== null
        ? resetApply.unavailableReason('program', 'revert') : program.documentUnavailableReason)),
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
    apply: applyAuthoringBlock
      ?? deploymentActionUnavailableReason(deploymentWorkspace, 'apply', deploymentApplyCommon)
      ?? invalidEditReason ?? programIncomplete,
    revert: applyAuthoringBlock ?? deploymentActionUnavailableReason(
      deploymentWorkspace, 'revert', deploymentBusyReason ?? (resetApply === null || !resetApply.runExists
        ? deployment.documentUnavailableReason
        : deploymentResetsRun ? resetApply.unavailableReason('deployment', 'revert') : deployment.mutationUnavailableReason),
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
    // Reload initial scene starts off each time the confirmation opens.
    setReloadScene(false);
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
  const appliedProgram = runtime.state.applied?.program ?? null;
  const draftProgram = programWorkspace?.draft.program ?? null;
  const reloadOffered = initialScene !== null && initialSceneReloadOffered({
    action,
    target: action === null ? null : routes[action].target,
    draftProgram,
    appliedProgram,
    savedDiffersFromApplied: programSavedDiffers,
  });
  const reloadPersistentKeys = declaresPersistentKeys(action === 'apply' ? draftProgram : appliedProgram);
  const confirm = () => {
    if (action === null) return;
    const target = routes[action].target;
    if (target === null) return;
    let expectation: InitialSceneExpectation | null = null;
    if (action !== 'discard' && initialScene !== null) {
      const targetProgramRevision = action === 'apply'
        ? programWorkspace?.draft.programRevision : programWorkspace?.saved.programRevision;
      if (targetProgramRevision != null) {
        expectation = { target: 'program', targetProgramRevision, reload: reloadOffered && reloadScene };
        initialScene.expect(expectation);
      }
    }
    // A refused or failed Apply or Revert leaves nothing to reload.
    const dropUnless = (applied: boolean) => {
      if (!applied && expectation !== null) initialScene?.drop(expectation);
    };
    if (action !== 'discard' && resetsRun && resetApply !== null) {
      void resetApply.confirm('program', null, action).then((refusal) => {
        dropUnless(refusal === null);
        if (refusal === null) closeAction();
        else setApplyRefusal(refusal);
      });
      return;
    }
    closeAction();
    if (action === 'discard') program.discardScenario();
    else {
      (action === 'revert' ? program.revert() : program.apply())
        .then((result) => dropUnless(result !== null), () => dropUnless(false));
    }
  };

  // The Deployment Apply (or Revert) confirmation: its Preview.
  const preview = deployment.applyPreview;
  const reverting = deployment.applyPreviewKind === 'revert';
  const previewResetsRun = deploymentResetsRun && resetApply !== null;
  const cancelPreview = () => {
    setApplyRefusal(null);
    deployment.cancelApplyConfirmation();
  };
  const previewAction = reverting ? 'revert' : 'apply';
  const previewReloadOffered = initialScene !== null && initialSceneReloadOffered({
    action: previewAction,
    target: 'deployment',
    draftProgram,
    appliedProgram,
    savedDiffersFromApplied: programSavedDiffers,
  });
  const confirmPreview = () => {
    if (preview === null) return;
    const expectation: InitialSceneExpectation | null = initialScene === null
      ? null : { target: 'deployment', reload: previewReloadOffered && reloadScene };
    if (expectation !== null) initialScene?.expect(expectation);
    if (!previewResetsRun || resetApply === null) {
      // Not sent, or refused: nothing to reload (its end settles the rest).
      void deployment.confirmApply().then((result) => {
        if (result === null && expectation !== null) initialScene?.drop(expectation);
      });
      return;
    }
    void resetApply.confirm(
      'deployment', preview.scope, previewAction, preview.collision_rules.map((row) => row.rule_id),
    ).then((refusal) => {
      if (refusal !== null && expectation !== null) initialScene?.drop(expectation);
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
        title={routes.revert.reason ?? `${resetsRun && routes.revert.target === 'program' ? 'Reset the current run, then revert' : 'Restore Draft and Applied to Saved'}: ${covers(routes.revert.target)}.`}
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
            {reloadOffered && (
              <ReloadInitialSceneOption
                action="revert" resetsRun={resetsRun} checked={reloadScene} onChange={setReloadScene}
                persistentKeys={reloadPersistentKeys} persistentValues={runtime.persistentValues}
              />
            )}
            {resetsRun && <ResetsRunWarning action="revert" />}
            {applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <div className="dialog-actions">
              <button type="button" className="primary-action" disabled={routes.revert.target === null}
                title={applyAuthoringBlock ?? undefined} onClick={confirm}>
                {resetsRun ? 'Reset run and Revert' : 'Revert to saved'}
              </button>
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
            {reloadOffered && (
              <ReloadInitialSceneOption
                action="apply" resetsRun={resetsRun} checked={reloadScene} onChange={setReloadScene}
                persistentKeys={reloadPersistentKeys} persistentValues={runtime.persistentValues}
              />
            )}
            {resetsRun && <ResetsRunWarning />}
            {applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <div className="dialog-actions">
              <button type="button" className="primary-action" disabled={routes.apply.target === null}
                title={applyAuthoringBlock ?? undefined} onClick={confirm}>
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
            {previewReloadOffered && (
              <ReloadInitialSceneOption
                action={previewAction} resetsRun={previewResetsRun} checked={reloadScene} onChange={setReloadScene}
                persistentKeys={declaresPersistentKeys(reverting ? appliedProgram : draftProgram)}
                persistentValues={runtime.persistentValues}
              />
            )}
            {previewResetsRun && <ResetsRunWarning action={previewAction} />}
            {previewResetsRun && applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <div className="dialog-actions">
              <button type="button" onClick={cancelPreview}>Cancel</button>
              <button
                type="button"
                className="primary-action"
                disabled={preview === null || confirmationReason !== null}
                title={applyAuthoringBlock
                  ?? (preview === null ? 'Previewing impact…' : deploymentApplyCommon
                  ?? 'Apply the Deployment: affected components are disabled, applied and prepared again.')}
                onClick={confirmPreview}
              >{deployment.applying ? 'Applying Deployment'
                : previewResetsRun ? (reverting ? 'Reset run and Revert' : 'Reset run and Apply')
                  : reverting ? 'Confirm revert' : 'Confirm apply'}</button>
            </div>
          </div>
        </OverlayPanel>
      )}
    </>
  );
}

/**
 * The single, visually clear warning of an Apply (or Revert) while a Program
 * run exists. It is part of the ordinary confirmation: no second dialog.
 */
export function ResetsRunWarning({ action = 'apply' }: { action?: 'apply' | 'revert' }) {
  return (
    <div className="document-apply-blocked document-apply-resets-run" role="alert">
      <strong>{action === 'revert' ? 'Reverting' : 'Applying'} Resets the current run.</strong>
      <span>
        The whole run ends, including any repeating part, and does not resume
        where it stopped. When the cycle is Idle,
        {action === 'revert' ? ' the Saved Scenario is Applied.' : ' exactly this Draft is Applied.'}
        {' '}Nothing is saved, executed or run automatically.
      </span>
    </div>
  );
}

/**
 * Reload initial scene with an Apply or Revert: once it is Applied, one Reset
 * with Reload initial scene, as the Reset confirmation describes it. Off by
 * default.
 */
export function ReloadInitialSceneOption({
  action,
  resetsRun,
  checked,
  onChange,
  persistentKeys,
  persistentValues,
}: {
  action: 'apply' | 'revert';
  resetsRun: boolean;
  checked: boolean;
  onChange: (checked: boolean) => void;
  persistentKeys: boolean;
  persistentValues: ProgramRuntimeController['persistentValues'];
}) {
  const after = action === 'revert' ? 'Revert' : 'Apply';
  return (
    <>
      <fieldset className="transport-reset-options">
        <label className="toggle-control">
          <input
            type="checkbox"
            name="reloadInitialScene"
            checked={checked}
            onChange={(event) => onChange(event.target.checked)}
          />
          <span>Reload initial scene</span>
        </label>
        <small className="transport-reset-option-note">
          {resetsRun
            ? `After the ${after}, a second Reset reloads the initial scene. `
            : `After the ${after}, a Reset reloads the initial scene. `}
          {RELOAD_INITIAL_SCENE_NOTE}
          {persistentKeys && PERSISTENT_KEYS_RESET_NOTE}
          {' '}Otherwise the live cell keeps its current scene until a Reset with Reload initial scene.
        </small>
      </fieldset>
      {checked && persistentKeys && <PersistentKeyResetNotice client={persistentValues} />}
      {checked && <p>Robot positions stay as they are.</p>}
    </>
  );
}
