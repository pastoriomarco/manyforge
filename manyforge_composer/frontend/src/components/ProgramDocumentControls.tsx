import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type MouseEvent,
} from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ArtifactKind, FileBrowserEntry } from '../types/api';
import { ArtifactBrowser } from './ArtifactBrowser';
import { OverlayPanel } from './OverlayPanel';
import { ProgramSettingsEditor } from './ProgramSettingsEditor';

type ProgramAction =
  | 'new'
  | 'open'
  | 'save_as'
  | 'apply'
  | 'revert'
  | 'discard'
  | 'load';

interface ProgramDocumentControlsProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  localInvalidEdit?: boolean;
  settingsRequest?: number;
  onProgramRequirementCreated?: (
    kind: 'component_role' | 'target_role',
    id: string,
  ) => void;
}

function shortRevision(revision: string | null): string {
  if (!revision) return 'none';
  return revision.length > 18 ? `${revision.slice(0, 14)}…` : revision;
}

function actionTitle(action: ProgramAction): string {
  switch (action) {
    case 'new': return 'New Program';
    case 'open': return 'Open Program as Draft';
    case 'save_as': return 'Save Program As';
    case 'apply': return 'Apply Program Draft';
    case 'revert': return 'Revert Program to Saved';
    case 'discard': return 'Discard Draft Changes';
    case 'load': return 'Load Scenario';
  }
}

export function ProgramDocumentControls({
  controller,
  runtime,
  localInvalidEdit = false,
  settingsRequest = 0,
  onProgramRequirementCreated,
}: ProgramDocumentControlsProps) {
  const {
    state,
    mutationAvailable,
    mutationUnavailableReason,
    newDraft,
    openDraft,
    save,
    saveAs,
    apply: applyDraft,
    discard,
    revert,
    loadScenario,
  } = controller;
  const [action, setAction] = useState<ProgramAction | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [settingsInvalid, setSettingsInvalid] = useState(false);
  const [selectedProgramPath, setSelectedProgramPath] = useState<string | null>(null);
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [saveAsPath, setSaveAsPath] = useState('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const settingsButton = useRef<HTMLButtonElement>(null);
  const observedSettingsRequest = useRef(settingsRequest);
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
  const replaceNeedsConfirmation = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true;
  const scenarioCatalog = runtime.state.catalog;
  const selectedScenario = scenarioCatalog?.scenarios.find(
    (row) => row.scenarioId === runtime.state.selectedScenarioId,
  ) ?? null;
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
    if (workspace?.saved.path) setSaveAsPath(workspace.saved.path);
  }, [workspace?.saved.path]);

  useEffect(() => {
    if (settingsRequest === observedSettingsRequest.current) return;
    observedSettingsRequest.current = settingsRequest;
    if (authoring !== null) setSettingsOpen(true);
  }, [authoring, settingsRequest]);

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
    setSelectedProgramPath(null);
    setConfirmDiscard(false);
    setConfirmOverwrite(false);
  }, []);
  const beginAction = (
    next: ProgramAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    returnFocus.current = event.currentTarget;
    setSelectedProgramPath(null);
    setConfirmDiscard(false);
    setConfirmOverwrite(false);
    setAction(next);
  };
  const chooseArtifact = useCallback((
    kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => {
    setSelectedProgramPath(kind === 'program' ? entry?.path ?? null : null);
  }, []);
  const complete = (operation: () => void) => {
    operation();
    closeAction();
  };

  const programName = authoring?.name ?? 'No Program';
  const draftState = authoring !== null && program === null
    ? 'incomplete'
    : workspace?.hasUnsavedChanges ? 'modified' : 'draft';
  const savedState = workspace?.saved.available
    ? workspace.saved.matchesDraft ? 'saved' : 'different'
    : 'unsaved';
  const appliedState = workspace?.applied.available
    ? workspace.applied.matchesDraft ? 'applied' : 'different'
    : 'unavailable';

  return (
    <>
      <div className="program-document-controls" aria-label="Program document controls">
        <div className="program-document-identity">
          <strong title={workspace?.saved.path ?? authoring?.description ?? programName}>
            {programName}
          </strong>
          <span className="document-identity-divider" aria-hidden="true" />
          <span
            className={`document-state state-${draftState}`}
            title={program === null && authoring !== null
              ? `${workspace?.draft.diagnostics.length ?? 0} issue(s) · ${incompleteReason}`
              : `Draft ${shortRevision(workspace?.draft.programRevision ?? null)} · ID ${workspace?.draft.draftId ?? 'unavailable'} · edit ${workspace?.draft.draftRevision ?? 0}`}
          >
            {draftState}
          </span>
          <span
            className={`document-state state-${savedState}`}
            title={workspace?.saved.path ?? 'No Saved Program path'}
          >
            {savedState}
          </span>
          <span
            className={`document-state state-${appliedState}`}
            title={workspace?.applied.unavailableReason?.message ?? `Applied ${shortRevision(workspace?.applied.programRevision ?? null)}`}
          >
            {appliedState}
          </span>
          <span className="document-identity-divider document-settings-divider" aria-hidden="true" />
          <button
            ref={settingsButton}
            type="button"
            className="icon-button program-settings-button"
            aria-label="Program-wide settings"
            title="Program-wide settings"
            disabled={authoring === null}
            onClick={() => setSettingsOpen(true)}
          >
            ⚙
          </button>
          <span className="document-identity-divider document-settings-divider" aria-hidden="true" />
        </div>

        <div className="program-document-actions">
          <button type="button" disabled={authoringBusy || workspace === null} title={authoringBlockReason ?? 'Create a new local Program Draft.'} onClick={(event) => beginAction('new', event)}>New</button>
          <button type="button" disabled={authoringBusy || workspace === null} title={authoringBlockReason ?? 'Open a Program as the local Draft without changing the active Scenario.'} onClick={(event) => beginAction('open', event)}>Open</button>
          <button type="button" disabled={authoringBusy || !program || !workspace?.saved.available || !workspace.hasUnsavedChanges} title={authoringBlockReason ?? (!program ? incompleteReason : !workspace?.hasUnsavedChanges ? 'Draft already matches Saved.' : 'Save the current Draft.')} onClick={save}>Save</button>
          <button type="button" disabled={authoringBusy || !program} title={authoringBlockReason ?? (!program ? incompleteReason : 'Save the current Draft to another Program file.')} onClick={(event) => beginAction('save_as', event)}>Save As</button>
          <button type="button" disabled={authoringBusy || workspace === null || scenarioCatalog === null} title={authoringBlockReason ?? (scenarioCatalog === null ? 'Scenario catalog is still loading or unavailable.' : 'Load a complete Scenario: Deployment, Binding, and Program.')} onClick={(event) => beginAction('load', event)}>Load</button>
          <span className="header-action-divider" aria-hidden="true" />
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

      <OverlayPanel
        open={settingsOpen}
        title="Program settings"
        titleId="program-settings-title"
        kind="info"
        onClose={() => {
          if (!settingsInvalid) setSettingsOpen(false);
        }}
        returnFocusRef={settingsButton}
      >
        {authoring && (
          <ProgramSettingsEditor
            program={authoring}
            candidateRevision={workspace?.draft.programRevision ?? null}
            busy={authoringBusy}
            runtime={runtime}
            onMutate={controller.mutate}
            onInvalidChange={setSettingsInvalid}
            onRoleCreated={onProgramRequirementCreated}
            diagnostics={workspace?.draft.diagnostics ?? []}
          />
        )}
      </OverlayPanel>

      <OverlayPanel
        open={action !== null}
        title={action === null ? 'Program action' : actionTitle(action)}
        titleId="program-action-dialog-title"
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action === 'new' && (
          <div className="program-dialog-copy">
            <p>Create an empty local Program Draft. This has no runtime effect.</p>
            {replaceNeedsConfirmation && (
              <label className="program-confirmation">
                <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                Discard the current unsaved or unapplied Draft changes
              </label>
            )}
            <button type="button" className="primary-action" disabled={authoringBusy || (replaceNeedsConfirmation && !confirmDiscard)} onClick={() => complete(() => newDraft(confirmDiscard))}>
              Create New Draft
            </button>
          </div>
        )}

        {action === 'open' && (
          <div className="program-document-dialog">
            <p className="program-dialog-context">
              Open changes only the editable Draft. The current active Scenario
              {currentScenario ? ` (${currentScenario.scenarioId})` : ''} and its Deployment remain unchanged.
            </p>
            <ArtifactBrowser lockedKind="program" onSelectionChange={chooseArtifact} />
            <div className="program-dialog-actions">
              {replaceNeedsConfirmation && (
                <label className="program-confirmation">
                  <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                  Replace the current unsaved or unapplied Draft
                </label>
              )}
              <button type="button" className="primary-action" disabled={authoringBusy || selectedProgramPath === null || (replaceNeedsConfirmation && !confirmDiscard)} onClick={() => {
                if (selectedProgramPath) complete(() => openDraft(selectedProgramPath, confirmDiscard));
              }}>
                Open as Draft
              </button>
            </div>
          </div>
        )}

        {action === 'save_as' && (
          <div className="program-dialog-copy">
            <label className="program-save-as-path">
              Path inside an approved Program root
              <input value={saveAsPath} onChange={(event) => setSaveAsPath(event.target.value)} />
            </label>
            <label className="program-confirmation">
              <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
              Allow replacing an existing file
            </label>
            <button type="button" className="primary-action" disabled={authoringBusy || !program || !saveAsPath.trim()} onClick={() => complete(() => saveAs(saveAsPath.trim(), confirmOverwrite))}>
              Save As
            </button>
          </div>
        )}

        {action === 'apply' && (
          <div className="program-dialog-copy">
            {currentScenario ? (
              <div className="program-current-scenario" aria-label="Current Program Apply target">
                <strong>{currentScenario.scenarioId}</strong>
                <span>Deployment {currentScenario.deploymentArtifactId} · {shortRevision(currentScenario.deploymentRevision)}</span>
                <span>Binding {shortRevision(currentScenario.programBindingId)}</span>
              </div>
            ) : (
              <div className="program-apply-blocked" role="alert">
                <strong>No exact current Scenario.</strong>
                <span>{currentScenarioUnavailable}</span>
                <span>Use Load Scenario to activate a complete Program, Deployment, and Binding.</span>
              </div>
            )}
            <p>
              Apply replaces only the Program in this exact current Scenario.
              It never selects or changes Deployment.
            </p>
            {!workspace?.applicability.applyReady && (
              <div className="program-apply-blocked" role="alert">
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
              Apply Draft to {currentScenario?.scenarioId ?? 'current Scenario'}
            </button>
          </div>
        )}

        {action === 'load' && (
          <div className="program-document-dialog">
            <div className="program-dialog-copy">
              <label className="program-save-as-path">
                Qualified scenario
                <select
                  value={runtime.state.selectedScenarioId ?? ''}
                  disabled={busy || scenarioCatalog === null}
                  onChange={(event) => {
                    if (event.target.value) runtime.selectScenario(event.target.value);
                    else runtime.clearScenarioSelection();
                  }}
                >
                  <option value="">Select a scenario</option>
                  {scenarioCatalog?.scenarios.map((row) => (
                    <option key={row.scenarioId} value={row.scenarioId}>
                      {row.description ? `${row.scenarioId} — ${row.description}` : row.scenarioId}
                    </option>
                  ))}
                </select>
              </label>
              <p>Load activates this Scenario’s exact Deployment, waits for convergence, then Applies its bound Program. It completes as one backend-owned operation.</p>
              {replaceNeedsConfirmation && (
                <label className="program-confirmation">
                  <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                  Discard current unsaved or unapplied Draft changes
                </label>
              )}
              <button
                type="button"
                className="primary-action"
                disabled={
                  authoringBusy
                  || selectedScenario === null
                  || (replaceNeedsConfirmation && !confirmDiscard)
                }
                onClick={() => {
                  if (!selectedScenario || !scenarioCatalog) return;
                  complete(() => loadScenario(selectedScenario.scenarioId, scenarioCatalog.scenarioCatalogRevision, confirmDiscard));
                }}
              >
                Load Scenario
              </button>
            </div>
          </div>
        )}

        {action === 'discard' && (
          <div className="program-dialog-copy">
            <p>Replace the local Draft with the current backend-issued Applied Program.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || workspace?.hasUnappliedChanges !== true} onClick={() => complete(discard)}>
              Discard to Applied
            </button>
          </div>
        )}

        {action === 'revert' && (
          <div className="program-dialog-copy">
            <p>Restore Saved through the authoritative Apply path, then replace the Draft with that exact result.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || !hasAppliedUnsavedChanges} onClick={() => complete(revert)}>
              Revert to Saved
            </button>
          </div>
        )}
      </OverlayPanel>
    </>
  );
}
