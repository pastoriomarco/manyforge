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
}: ProgramDocumentControlsProps) {
  const {
    state,
    mutationAvailable,
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
  const [selectedProgramPath, setSelectedProgramPath] = useState<string | null>(null);
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [saveAsPath, setSaveAsPath] = useState('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const settingsButton = useRef<HTMLButtonElement>(null);
  const workspace = state.workspace;
  const program = workspace?.draft.program ?? null;
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const authoringBusy = busy || !mutationAvailable;
  const replaceNeedsConfirmation = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true;
  const scenarioCatalog = runtime.state.catalog;
  const selectedScenario = scenarioCatalog?.scenarios.find(
    (row) => row.scenarioId === runtime.state.selectedScenarioId,
  ) ?? null;
  const affectedPolicyInstances = workspace?.applicability.affectedInstanceIds ?? [];
  const affectedPolicyInstanceSummary = affectedPolicyInstances.length <= 12
    ? affectedPolicyInstances.join(', ')
    : `${affectedPolicyInstances.slice(0, 12).join(', ')}, and ${affectedPolicyInstances.length - 12} more`;

  useEffect(() => {
    if (workspace?.saved.path) setSaveAsPath(workspace.saved.path);
  }, [workspace?.saved.path]);

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

  const programName = program?.name ?? 'No Program';
  const draftState = workspace?.hasUnsavedChanges ? 'modified' : 'draft';
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
          <strong title={workspace?.saved.path ?? program?.description ?? programName}>
            {programName}
          </strong>
          <span className="document-identity-divider" aria-hidden="true" />
          <span
            className={`document-state state-${draftState}`}
            title={`Draft ${shortRevision(workspace?.draft.programRevision ?? null)} · ID ${workspace?.draft.draftId ?? 'unavailable'} · edit ${workspace?.draft.draftRevision ?? 0}`}
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
            disabled={program === null}
            onClick={() => setSettingsOpen(true)}
          >
            ⚙
          </button>
          <span className="document-identity-divider document-settings-divider" aria-hidden="true" />
        </div>

        <div className="program-document-actions">
          <button type="button" disabled={authoringBusy || workspace === null} onClick={(event) => beginAction('new', event)}>New</button>
          <button type="button" disabled={authoringBusy || workspace === null} onClick={(event) => beginAction('open', event)}>Open</button>
          <button type="button" disabled={authoringBusy || !program || !workspace?.saved.available} onClick={save}>Save</button>
          <button type="button" disabled={authoringBusy || !program} onClick={(event) => beginAction('save_as', event)}>Save As</button>
          <button type="button" disabled={authoringBusy || workspace === null} onClick={(event) => beginAction('load', event)}>Load</button>
          <span className="header-action-divider" aria-hidden="true" />
          <button type="button" disabled={authoringBusy || !workspace?.applied.available} onClick={(event) => beginAction('discard', event)}>Discard</button>
          <button type="button" disabled={authoringBusy || !workspace?.saved.available || !workspace?.applied.available} onClick={(event) => beginAction('revert', event)}>Revert</button>
          <button className="primary-action" type="button" disabled={authoringBusy || !program || workspace?.applied.matchesDraft === true} onClick={(event) => beginAction('apply', event)}>Apply</button>
        </div>
      </div>

      <OverlayPanel
        open={settingsOpen}
        title="Program settings"
        titleId="program-settings-title"
        kind="info"
        onClose={() => setSettingsOpen(false)}
        returnFocusRef={settingsButton}
      >
        {program && (
          <div className="program-settings-summary">
            <p>Current Program-wide Draft facts. Editing remains unavailable in this surface.</p>
            <dl>
              <div><dt>Program ID</dt><dd><code>{program.program_id}</code></dd></div>
              <div><dt>Profile</dt><dd><code>{program.program_profile}</code></dd></div>
              <div><dt>Description</dt><dd>{program.description || 'Not provided'}</dd></div>
              <div><dt>Catalog version</dt><dd><code>{program.catalog.content_version}</code></dd></div>
              <div><dt>Component roles</dt><dd>{Object.keys(program.component_roles).length}</dd></div>
              <div><dt>Target roles</dt><dd>{Object.keys(program.target_roles).length}</dd></div>
              <div><dt>Compatibility fields</dt><dd>{Object.keys(program.compatibility ?? {}).length}</dd></div>
            </dl>
          </div>
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
            <ArtifactBrowser lockedKind="program" onSelectionChange={chooseArtifact} />
            <div className="program-dialog-actions">
              {workspace?.hasUnsavedChanges && (
                <label className="program-confirmation">
                  <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                  Discard unsaved Draft changes
                </label>
              )}
              <button type="button" className="primary-action" disabled={authoringBusy || selectedProgramPath === null || (workspace?.hasUnsavedChanges === true && !confirmDiscard)} onClick={() => {
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

        {(action === 'apply' || action === 'load') && (
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
              <p>{action === 'load'
                ? 'Load activates this Scenario’s exact Deployment, waits for convergence, then Applies its bound Program. It completes as one backend-owned operation.'
                : 'Program Apply requires this scenario’s deployment already active and converged. Program Apply never changes the deployment.'}</p>
              {action === 'apply' && !workspace?.applicability.applyReady && (
                <div className="program-apply-blocked" role="alert">
                  <strong>Draft is not Apply-ready.</strong>
                  {affectedPolicyInstanceSummary && <span>Affected nodes: {affectedPolicyInstanceSummary}.</span>}
                  <span>{workspace?.applicability.unavailableReason?.message}</span>
                </div>
              )}
              {action === 'load' && replaceNeedsConfirmation && (
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
                  || (action === 'apply' && (!program || !workspace?.applicability.applyReady))
                  || (action === 'load' && replaceNeedsConfirmation && !confirmDiscard)
                }
                onClick={() => {
                  if (!selectedScenario || !scenarioCatalog) return;
                  if (action === 'apply') {
                    complete(() => applyDraft(selectedScenario.scenarioId, scenarioCatalog.scenarioCatalogRevision));
                  } else {
                    complete(() => loadScenario(selectedScenario.scenarioId, scenarioCatalog.scenarioCatalogRevision, confirmDiscard));
                  }
                }}
              >
                {action === 'apply' ? 'Apply Draft' : 'Load Scenario'}
              </button>
            </div>
          </div>
        )}

        {action === 'discard' && (
          <div className="program-dialog-copy">
            <p>Replace the local Draft with the current backend-issued Applied Program.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || !workspace?.applied.available} onClick={() => complete(discard)}>
              Discard to Applied
            </button>
          </div>
        )}

        {action === 'revert' && (
          <div className="program-dialog-copy">
            <p>Restore Saved through the authoritative Apply path, then replace the Draft with that exact result.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || !workspace?.saved.available || !workspace?.applied.available} onClick={() => complete(revert)}>
              Revert to Saved
            </button>
          </div>
        )}
      </OverlayPanel>
    </>
  );
}
