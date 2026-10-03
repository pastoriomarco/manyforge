import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type MouseEvent,
  type ReactNode,
} from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ResetApplyController } from '../hooks/useResetApply';
import { OverlayPanel } from './OverlayPanel';
import { programActionUnavailableReason, programApplyScenarioReason } from '../state/authoringWorkspace';

type ProgramAction = 'apply' | 'revert' | 'discard';

interface ProgramDocumentControlsProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  /** Apply while a Program run exists Resets it first (one confirmation). */
  resetApply?: ResetApplyController | null;
  localInvalidEdit?: boolean;
  /** The workspace-independent Scenario New/Save/Save as/Load commands. */
  scenarioCommands?: ReactNode;
}

function actionTitle(action: ProgramAction): string {
  switch (action) {
    case 'apply': return 'Apply Draft?';
    case 'revert': return 'Revert to saved?';
    case 'discard': return 'Discard Draft changes?';
  }
}

export function ProgramDocumentControls({
  controller,
  runtime,
  resetApply = null,
  localInvalidEdit = false,
  scenarioCommands,
}: ProgramDocumentControlsProps) {
  const {
    state,
    mutationUnavailableReason,
    documentUnavailableReason,
    apply: applyDraft,
    discard,
    revert,
    scenarioSourceDiagnostics = [],
  } = controller;
  const [action, setAction] = useState<ProgramAction | null>(null);
  const [applyRefusal, setApplyRefusal] = useState<string | null>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
  const program = workspace?.draft.program ?? null;
  const authoring = workspace?.draft.authoring ?? null;
  const incompleteReason = workspace?.draft.diagnostics[0]?.message
    ?? 'Complete the authoring Draft before Save or Apply.';
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  // With a run, Apply ends it first: available when the Draft is editable
  // (all Program motion stopped) and Reset is allowed.
  const resetsRun = resetApply?.runExists === true;
  const busyReason = busy ? 'Another Program operation is in progress.' : null;
  const applyCommonReason = busyReason ?? (resetsRun && resetApply !== null
    ? resetApply.unavailableReason('program') : documentUnavailableReason);
  const authoringBlockReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : applyCommonReason;
  const currentScenario = runtime.state.currentScenario?.currentScenario ?? null;
  const currentScenarioUnavailable = programApplyScenarioReason(runtime.state.currentScenario);
  const reasons = {
    apply: programActionUnavailableReason(workspace, 'apply', applyCommonReason)
      ?? (localInvalidEdit ? authoringBlockReason : null)
      ?? currentScenarioUnavailable,
    revert: programActionUnavailableReason(workspace, 'revert', busyReason ?? documentUnavailableReason),
    discard: programActionUnavailableReason(workspace, 'discard', busyReason ?? mutationUnavailableReason),
  };

  useEffect(() => {
    if (!workspace?.hasUnsavedChanges && !workspace?.hasUnappliedChanges) return undefined;
    const protectDirtyDraft = (event: BeforeUnloadEvent) => {
      event.preventDefault();
      event.returnValue = '';
    };
    window.addEventListener('beforeunload', protectDirtyDraft);
    return () => window.removeEventListener('beforeunload', protectDirtyDraft);
  }, [workspace?.hasUnappliedChanges, workspace?.hasUnsavedChanges]);

  const closeAction = useCallback(() => {
    setAction(null);
    setApplyRefusal(null);
  }, []);
  const beginAction = (
    next: ProgramAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    if (reasons[next] !== null) return;
    returnFocus.current = event.currentTarget;
    setAction(next);
  };
  const complete = (operation: () => void) => {
    if (action === null || reasons[action] !== null) return;
    operation();
    closeAction();
  };
  // The one acknowledgement: Reset the run, then the ordinary Apply.
  const resetAndApply = () => {
    if (resetApply === null || reasons.apply !== null) return;
    void resetApply.confirm('program').then((refusal) => {
      if (refusal === null) closeAction();
      else setApplyRefusal(refusal);
    });
  };

  const programName = authoring?.name ?? 'No Program';

  return (
    <>
      <div className="document-controls" aria-label="Program document controls">
        <div className="document-identity">
          <strong title={workspace?.saved.path ?? authoring?.description ?? programName}>
            {programName}
          </strong>
        </div>

        <div className="document-actions">
          {scenarioCommands && (
            <>
              {scenarioCommands}
              <span className="header-action-divider" aria-hidden="true" />
            </>
          )}
          <button type="button" disabled={reasons.discard !== null} title={reasons.discard ?? 'Discard Draft edits and return to Applied.'} onClick={(event) => beginAction('discard', event)}>Discard</button>
          <button type="button" disabled={reasons.revert !== null} title={reasons.revert ?? 'Restore Draft and Applied to Saved.'} onClick={(event) => beginAction('revert', event)}>Revert</button>
          <button
            className="primary-action"
            type="button"
            disabled={reasons.apply !== null}
            title={currentScenarioUnavailable !== null
              ? `Load a Scenario before Program Apply. ${currentScenarioUnavailable}`
              : authoringBlockReason
                ?? (!program
                  ? incompleteReason
                  : workspace?.applied.matchesDraft === true
                  ? 'Nothing to apply: the Program Draft matches Applied.'
                  : resetsRun
                    ? 'Reset the current run, then Apply this Draft. Deployment does not change.'
                    : 'Apply this Draft to the current active Scenario without changing Deployment.')}
            onClick={(event) => beginAction('apply', event)}
          >
            Apply
          </button>
        </div>
      </div>

      {scenarioSourceDiagnostics.length > 0 && (
        <div className="notice severity-warning" role="status">
          <strong>Referenced sources changed since the last Save.</strong>
          <ul>
            {scenarioSourceDiagnostics.map((diagnostic) => (
              <li key={diagnostic.sourceId}>{diagnostic.sourceId}: {diagnostic.message}</li>
            ))}
          </ul>
          <span>Apply will capture and validate the current source contents.</span>
        </div>
      )}

      <OverlayPanel
        open={action !== null}
        title={action === null ? 'Program action' : actionTitle(action)}
        titleId="program-action-dialog-title"
        compact={action === 'apply' || action === 'discard' || action === 'revert'}
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action !== null && reasons[action] !== null && (
          <p role="status">{reasons[action]}</p>
        )}
        {action === 'apply' && (
          <div className="dialog-copy compact-confirmation">
            {currentScenarioUnavailable === null ? (
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
            )}
            {!workspace?.applicability.applyReady && (
              <div className="document-apply-blocked" role="alert">
                <strong>Draft is not Apply-ready.</strong>
                <span>{workspace?.applicability.unavailableReason?.message}</span>
              </div>
            )}
            {resetsRun && <ResetsRunWarning />}
            {applyRefusal !== null && <p role="alert">{applyRefusal}</p>}
            <button
              type="button"
              className="primary-action"
              disabled={reasons.apply !== null}
              onClick={() => (resetsRun ? resetAndApply() : complete(applyDraft))}
            >
              {resetsRun ? 'Reset run and Apply Draft' : 'Apply Draft'}
            </button>
          </div>
        )}

        {action === 'discard' && (
          <div className="dialog-copy compact-confirmation">
            <p>The editor will return to the Applied Program. Your Saved Program will not change.</p>
            <button type="button" className="primary-action" disabled={reasons.discard !== null} title={reasons.discard ?? undefined} onClick={() => complete(discard)}>
              Discard changes
            </button>
          </div>
        )}

        {action === 'revert' && (
          <div className="dialog-copy compact-confirmation">
            <p>Replace the Applied Program and Draft with the Saved Program?</p>
            <button type="button" className="primary-action" disabled={reasons.revert !== null} title={reasons.revert ?? undefined} onClick={() => complete(revert)}>
              Revert to saved
            </button>
          </div>
        )}
      </OverlayPanel>
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
