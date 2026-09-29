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
import { OverlayPanel } from './OverlayPanel';

type ProgramAction = 'apply' | 'revert' | 'discard';

interface ProgramDocumentControlsProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
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
  localInvalidEdit = false,
  scenarioCommands,
}: ProgramDocumentControlsProps) {
  const {
    state,
    mutationAvailable,
    mutationUnavailableReason,
    apply: applyDraft,
    discard,
    revert,
    scenarioSourceDiagnostics = [],
  } = controller;
  const [action, setAction] = useState<ProgramAction | null>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
  const program = workspace?.draft.program ?? null;
  const authoring = workspace?.draft.authoring ?? null;
  const incompleteReason = workspace?.draft.diagnostics[0]?.message
    ?? 'Complete the authoring Draft before Save or Apply.';
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const authoringBusy = busy || !mutationAvailable || localInvalidEdit;
  const authoringBlockReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy
    ? 'Another Program operation is in progress.'
    : mutationUnavailableReason;
  const currentScenarioProjection = runtime.state.currentScenario;
  const currentScenario = currentScenarioProjection?.currentScenario ?? null;
  const currentScenarioUnavailable = currentScenarioProjection === null
    ? 'Current Scenario is still being read from the backend.'
    : currentScenarioProjection.unavailableReasons.length > 0
      ? currentScenarioProjection.unavailableReasons.join(', ').replaceAll('_', ' ')
      : 'No exact current Scenario exists.';
  const affectedPolicyInstances = workspace?.applicability.affectedInstanceIds ?? [];
  const hasAppliedUnsavedChanges = workspace?.saved.available === true
    && workspace.applied.available
    && workspace.saved.programRevision !== workspace.applied.programRevision;
  const affectedPolicyInstanceSummary = affectedPolicyInstances.length <= 12
    ? affectedPolicyInstances.join(', ')
    : `${affectedPolicyInstances.slice(0, 12).join(', ')}, and ${affectedPolicyInstances.length - 12} more`;

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
  }, []);
  const beginAction = (
    next: ProgramAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    returnFocus.current = event.currentTarget;
    setAction(next);
  };
  const complete = (operation: () => void) => {
    operation();
    closeAction();
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
          <button type="button" disabled={authoringBusy || workspace?.hasUnappliedChanges !== true} title={authoringBlockReason ?? (workspace?.hasUnappliedChanges !== true ? 'Draft already matches Applied.' : 'Discard Draft edits and return to Applied.')} onClick={(event) => beginAction('discard', event)}>Discard</button>
          <button type="button" disabled={authoringBusy || !hasAppliedUnsavedChanges} title={authoringBlockReason ?? (!hasAppliedUnsavedChanges ? 'Applied already matches Saved.' : 'Restore Applied and Draft to Saved.')} onClick={(event) => beginAction('revert', event)}>Revert</button>
          <button
            className="primary-action"
            type="button"
            disabled={
              authoringBusy
              || !program
              || !workspace?.applicability.applyReady
              || workspace?.applied.matchesDraft === true
              || currentScenario === null
            }
            title={currentScenario === null
              ? `Load a Scenario before Program Apply. ${currentScenarioUnavailable}`
              : authoringBlockReason
                ?? (!program
                  ? incompleteReason
                  : workspace?.applied.matchesDraft === true
                  ? 'Draft already matches Applied.'
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
        {action === 'apply' && (
          <div className="dialog-copy compact-confirmation">
            {currentScenario ? (
              <p>
                Apply this Program Draft to <strong>{currentScenario.scenarioId}</strong>?
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
                {affectedPolicyInstanceSummary && <span>Affected nodes: {affectedPolicyInstanceSummary}.</span>}
                <span>{workspace?.applicability.unavailableReason?.message}</span>
              </div>
            )}
            <button
              type="button"
              className="primary-action"
              disabled={
                authoringBusy
                || currentScenario === null
                || !program
                || !workspace?.applicability.applyReady
                || workspace?.applied.matchesDraft === true
              }
              onClick={() => complete(applyDraft)}
            >
              Apply Draft
            </button>
          </div>
        )}

        {action === 'discard' && (
          <div className="dialog-copy compact-confirmation">
            <p>The editor will return to the Applied Program. Your Saved Program will not change.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || workspace?.hasUnappliedChanges !== true} title={authoringBlockReason ?? undefined} onClick={() => complete(discard)}>
              Discard changes
            </button>
          </div>
        )}

        {action === 'revert' && (
          <div className="dialog-copy compact-confirmation">
            <p>Replace the Applied Program and Draft with the Saved Program?</p>
            <button type="button" className="primary-action" disabled={authoringBusy || !hasAppliedUnsavedChanges} title={authoringBlockReason ?? undefined} onClick={() => complete(revert)}>
              Revert to saved
            </button>
          </div>
        )}
      </OverlayPanel>
    </>
  );
}
