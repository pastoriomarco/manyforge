import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type MouseEvent,
} from 'react';

import { api } from '../api/client';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  ArtifactKind,
  CatalogEntry,
  FileBrowserEntry,
  ScenarioDocumentEntry,
  ScenarioDocumentIndexResponse,
} from '../types/api';
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
  | 'scenario';

interface ProgramDocumentControlsProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  catalogEntries?: CatalogEntry[];
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
    case 'apply': return 'Apply Draft?';
    case 'revert': return 'Revert to Saved?';
    case 'discard': return 'Discard Draft changes?';
    case 'scenario': return 'Scenario Save / Load';
  }
}

function scenarioEntryKey(entry: ScenarioDocumentEntry): string {
  return `${entry.sourceKind}:${entry.path ?? entry.scenarioId ?? entry.name}`;
}

function scenarioStatusLabel(entry: ScenarioDocumentEntry): string {
  return entry.displayStatus.replaceAll('_', ' ');
}

export function ProgramDocumentControls({
  controller,
  runtime,
  catalogEntries = [],
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
    saveScenarioDocument,
    loadScenarioDocument,
    convertLegacyScenarioDocument,
    exportScenarioDocument,
    scenarioDocumentPath,
    scenarioSourceDiagnostics = [],
  } = controller;
  const [action, setAction] = useState<ProgramAction | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [settingsInvalid, setSettingsInvalid] = useState(false);
  const [selectedProgramPath, setSelectedProgramPath] = useState<string | null>(null);
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [saveAsPath, setSaveAsPath] = useState('');
  const [scenarioMode, setScenarioMode] = useState<'load' | 'save' | 'export' | 'convert'>('load');
  const [scenarioIndex, setScenarioIndex] = useState<ScenarioDocumentIndexResponse | null>(null);
  const [scenarioIndexError, setScenarioIndexError] = useState<string | null>(null);
  const [scenarioIndexLoading, setScenarioIndexLoading] = useState(false);
  const [scenarioDirectory, setScenarioDirectory] = useState('');
  const [selectedScenarioKey, setSelectedScenarioKey] = useState('');
  const [scenarioSavePath, setScenarioSavePath] = useState('');
  const [scenarioId, setScenarioId] = useState('scenario');
  const [scenarioName, setScenarioName] = useState('Scenario');
  const [scenarioDescription, setScenarioDescription] = useState('');
  const [legacyScenarioPath, setLegacyScenarioPath] = useState('');
  const [legacyDestinationPath, setLegacyDestinationPath] = useState('');
  const [exportDestinationPath, setExportDestinationPath] = useState('');
  const [confirmLegacyOverwrite, setConfirmLegacyOverwrite] = useState(false);
  const scenarioDirectoryRef = useRef('');
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
  const currentScenarioProjection = runtime.state.currentScenario;
  const currentScenario = currentScenarioProjection?.currentScenario ?? null;
  const currentScenarioUnavailable = currentScenarioProjection === null
    ? 'Current Scenario is still being read from the backend.'
    : currentScenarioProjection.unavailableReasons.length > 0
      ? currentScenarioProjection.unavailableReasons.join(', ').replaceAll('_', ' ')
      : 'No exact current Scenario exists.';
  const affectedPolicyInstances = workspace?.applicability.affectedInstanceIds ?? [];
  const selectedScenarioDocument = scenarioIndex?.entries.find(
    (entry) => scenarioEntryKey(entry) === selectedScenarioKey,
  ) ?? null;
  const selectedScenarioLoadable = selectedScenarioDocument !== null
    && selectedScenarioDocument.displayStatus !== 'unsupported'
    && selectedScenarioDocument.path !== null;
  const scenarioIdValid = /^[a-z][a-z0-9._-]{0,127}$/.test(scenarioId.trim());
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
    if (scenarioDocumentPath) setScenarioSavePath(scenarioDocumentPath);
  }, [scenarioDocumentPath]);

  useEffect(() => {
    if (action !== 'scenario') return;
    const nextId = authoring?.program_id ?? 'scenario';
    setScenarioId(nextId);
    setScenarioName(authoring?.name ?? 'Scenario');
    setScenarioDescription(authoring?.description ?? '');
  }, [action, authoring?.description, authoring?.name, authoring?.program_id]);

  const refreshScenarioIndex = useCallback(async (directory?: string) => {
    setScenarioIndexLoading(true);
    setScenarioIndexError(null);
    try {
      const index = await api.scenarioDocuments.list(directory?.trim() || undefined);
      setScenarioIndex(index);
      setScenarioDirectory(index.directory);
      scenarioDirectoryRef.current = index.directory;
      setSelectedScenarioKey((current) => (
        index.entries.some((entry) => scenarioEntryKey(entry) === current)
          ? current
          : ''
      ));
      setScenarioSavePath((current) => current || (
        `${index.directory}/${authoring?.program_id ?? 'scenario'}`
      ));
    } catch (error: unknown) {
      setScenarioIndexError(
        error instanceof Error ? error.message : 'Scenario directory could not be read.',
      );
    } finally {
      setScenarioIndexLoading(false);
    }
  }, [authoring?.program_id]);

  useEffect(() => {
    if (action === 'scenario') {
      void refreshScenarioIndex(scenarioDirectoryRef.current || undefined);
    }
  }, [
    action,
    refreshScenarioIndex,
    runtime.state.catalog?.scenarioCatalogRevision,
  ]);

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
    if (next === 'scenario') setScenarioMode('load');
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
          <button type="button" disabled={busy || workspace === null} title={busy ? 'Another Program operation is in progress.' : 'Save or load the complete Program and Deployment setup.'} onClick={(event) => beginAction('scenario', event)}>Scenario</button>
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

      {scenarioSourceDiagnostics.length > 0 && (
        <div className="program-document-source-notice" role="status">
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
            catalogEntries={catalogEntries}
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
        compact={action === 'apply' || action === 'discard' || action === 'revert'}
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
          <div className="program-dialog-copy program-compact-confirmation">
            {currentScenario ? (
              <p>
                Apply this Program Draft to <strong>{currentScenario.scenarioId}</strong>?
                {' '}Its Deployment will not change.
              </p>
            ) : (
              <div className="program-apply-blocked" role="alert">
                <strong>No exact current Scenario.</strong>
                <span>{currentScenarioUnavailable}</span>
                <span>Use Load Scenario to activate a complete Program, Deployment, and Binding.</span>
              </div>
            )}
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
              Apply Draft
            </button>
          </div>
        )}

        {action === 'scenario' && (
          <div className="program-document-dialog">
            <div className="scenario-document-tabs" role="tablist" aria-label="Scenario operation">
              <button type="button" className={scenarioMode === 'load' ? 'active' : ''} onClick={() => setScenarioMode('load')}>Load</button>
              <button type="button" className={scenarioMode === 'save' ? 'active' : ''} onClick={() => setScenarioMode('save')}>Save current</button>
              <button type="button" className={scenarioMode === 'export' ? 'active' : ''} onClick={() => setScenarioMode('export')}>Export portable</button>
              <button type="button" className={scenarioMode === 'convert' ? 'active' : ''} onClick={() => setScenarioMode('convert')}>Convert legacy</button>
            </div>

            {scenarioMode === 'load' ? (
              <div className="program-dialog-copy">
                <div className="scenario-directory-row">
                  <label className="program-save-as-path">
                    Scenario directory
                    <input value={scenarioDirectory} onChange={(event) => {
                      setScenarioDirectory(event.target.value);
                      scenarioDirectoryRef.current = event.target.value;
                    }} />
                  </label>
                  <button type="button" disabled={scenarioIndexLoading} onClick={() => void refreshScenarioIndex(scenarioDirectory)}>
                    {scenarioIndexLoading ? 'Reading…' : 'Refresh'}
                  </button>
                </div>
                {scenarioIndexError && <div className="program-apply-blocked" role="alert">{scenarioIndexError}</div>}
                <label className="program-save-as-path">
                  Available Scenario
                  <select
                    value={selectedScenarioKey}
                    disabled={scenarioIndexLoading || scenarioIndex === null}
                    onChange={(event) => setSelectedScenarioKey(event.target.value)}
                  >
                    <option value="">Select a Scenario</option>
                    {scenarioIndex?.entries.map((entry) => (
                      <option
                        key={scenarioEntryKey(entry)}
                        value={scenarioEntryKey(entry)}
                        disabled={entry.displayStatus === 'unsupported'}
                        title={entry.statusDetail}
                      >
                        {entry.name} — {scenarioStatusLabel(entry)}
                      </option>
                    ))}
                  </select>
                </label>
                {selectedScenarioDocument && (
                  <div className={`scenario-status scenario-status-${selectedScenarioDocument.displayStatus}`}>
                    <strong>{selectedScenarioDocument.name}</strong>
                    <span>{selectedScenarioDocument.statusDetail}</span>
                    {selectedScenarioDocument.description && (
                      <span>{selectedScenarioDocument.description}</span>
                    )}
                  </div>
                )}
                <p>
                  Loading a saved Scenario project opens its current authored files for editing. Runtime state is not changed.
                </p>
                {replaceNeedsConfirmation && (
                  <label className="program-confirmation">
                    <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                    Replace current unsaved or unapplied Draft changes
                  </label>
                )}
                <button
                  type="button"
                  className="primary-action"
                  disabled={
                    authoringBusy
                    || !selectedScenarioLoadable
                    || (replaceNeedsConfirmation && !confirmDiscard)
                  }
                  onClick={() => {
                    const selected = selectedScenarioDocument;
                    if (!selected || !selectedScenarioLoadable) return;
                    const selectedPath = selected.path;
                    if (!selectedPath) return;
                    complete(() => loadScenarioDocument(
                      selectedPath,
                      confirmDiscard,
                    ));
                  }}
                >
                  Load Scenario
                </button>
              </div>
            ) : scenarioMode === 'save' ? (
              <div className="program-dialog-copy">
                <p>
                  Save the editable Program, Scene, Deployment, assignment, and overrides in one structured project folder. Referenced robot meshes and other source assets are not copied.
                </p>
                <label className="program-save-as-path">
                  Scenario project folder
                  <input value={scenarioSavePath} onChange={(event) => setScenarioSavePath(event.target.value)} />
                </label>
                <div className="scenario-metadata-grid">
                  <label className="program-save-as-path">
                    Scenario ID
                    <input
                      value={scenarioId}
                      aria-invalid={!scenarioIdValid}
                      onChange={(event) => setScenarioId(event.target.value)}
                    />
                    {!scenarioIdValid && (
                      <small>Use lowercase letters, digits, dots, underscores, or hyphens; start with a letter.</small>
                    )}
                  </label>
                  <label className="program-save-as-path">
                    Name
                    <input value={scenarioName} onChange={(event) => setScenarioName(event.target.value)} />
                  </label>
                </div>
                <label className="program-save-as-path">
                  Description
                  <input value={scenarioDescription} onChange={(event) => setScenarioDescription(event.target.value)} />
                </label>
                <label className="program-confirmation">
                  <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
                  Allow replacing an existing Scenario project
                </label>
                <button
                  type="button"
                  className="primary-action"
                  disabled={
                    authoringBusy
                    || !scenarioSavePath.trim()
                    || !scenarioIdValid
                    || !scenarioName.trim()
                  }
                  onClick={() => complete(() => saveScenarioDocument({
                    path: scenarioSavePath.trim(),
                    scenarioId: scenarioId.trim(),
                    name: scenarioName.trim(),
                    description: scenarioDescription.trim(),
                    confirmOverwrite,
                  }))}
                >
                  Save Scenario
                </button>
              </div>
            ) : scenarioMode === 'export' ? (
              <div className="program-dialog-copy">
                <p>
                  Export a portable copy with the complete currently supported robot-source and Scene-asset closure. The open project is not changed.
                </p>
                <label className="program-save-as-path">
                  New destination project folder
                  <input value={exportDestinationPath} onChange={(event) => setExportDestinationPath(event.target.value)} />
                </label>
                <button
                  type="button"
                  className="primary-action"
                  disabled={busy || !scenarioDocumentPath || !exportDestinationPath.trim()}
                  onClick={() => complete(() => exportScenarioDocument(exportDestinationPath.trim()))}
                >
                  Export Scenario
                </button>
              </div>
            ) : (
              <div className="program-dialog-copy">
                <p>
                  Convert one legacy <code>.scenario.yaml</code> file into the structured project format. The source file and runtime state are not changed.
                </p>
                <label className="program-save-as-path">
                  Legacy Scenario file
                  <input value={legacyScenarioPath} onChange={(event) => setLegacyScenarioPath(event.target.value)} />
                </label>
                <label className="program-save-as-path">
                  Destination project folder
                  <input value={legacyDestinationPath} onChange={(event) => setLegacyDestinationPath(event.target.value)} />
                </label>
                <label className="program-confirmation">
                  <input type="checkbox" checked={confirmLegacyOverwrite} onChange={(event) => setConfirmLegacyOverwrite(event.target.checked)} />
                  Allow replacing an existing Scenario project
                </label>
                <button
                  type="button"
                  className="primary-action"
                  disabled={busy || !legacyScenarioPath.trim() || !legacyDestinationPath.trim()}
                  onClick={() => complete(() => convertLegacyScenarioDocument({
                    sourcePath: legacyScenarioPath.trim(),
                    destinationPath: legacyDestinationPath.trim(),
                    confirmOverwrite: confirmLegacyOverwrite,
                  }))}
                >
                  Convert Scenario
                </button>
              </div>
            )}
          </div>
        )}

        {action === 'discard' && (
          <div className="program-dialog-copy program-compact-confirmation">
            <p>The editor will return to the Applied Program. Your Saved Program will not change.</p>
            <button type="button" className="primary-action" disabled={authoringBusy || workspace?.hasUnappliedChanges !== true} onClick={() => complete(discard)}>
              Discard changes
            </button>
          </div>
        )}

        {action === 'revert' && (
          <div className="program-dialog-copy program-compact-confirmation">
            <p>Replace the Applied Program and Draft with the Saved Program?</p>
            <button type="button" className="primary-action" disabled={authoringBusy || !hasAppliedUnsavedChanges} onClick={() => complete(revert)}>
              Revert to Saved
            </button>
          </div>
        )}
      </OverlayPanel>
    </>
  );
}
