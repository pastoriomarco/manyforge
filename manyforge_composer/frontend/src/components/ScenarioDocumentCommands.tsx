import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type MouseEvent,
  type ReactNode,
} from 'react';

import { api } from '../api/client';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  ScenarioDocumentEntry,
  ScenarioDocumentIndexResponse,
} from '../types/api';
import { FolderPicker } from './FolderPicker';
import { OverlayPanel } from './OverlayPanel';
import {
  parentFolder,
  scenarioEntryLabel,
  scenarioNameError,
  scenarioNameTaken,
  scenarioProjectPath,
  scenarioProjectStem,
  SCENARIO_NAME_MAX,
} from '../state/newScenario';
import { busyLabel, busyReason, type BusyOperation } from '../state/busyOperation';
import { errorMessage } from '../state/structuralResource';

type ScenarioAction = 'new' | 'save_as' | 'load';
type SaveAsMode = 'project' | 'portable';
interface ReplacementConfirmation {
  /** New ends on the empty cell, so it has no path. */
  kind: 'new' | 'load';
  path: string | null;
}

interface ScenarioDocumentCommandsProps {
  controller: ProgramWorkspaceController;
  /** The Deployment Draft differs from its Saved baseline. */
  deploymentDirty?: boolean;
  localInvalidEdit?: boolean;
  /** Robots are moving (Program motion not held, or a manual operation). */
  motionActive?: boolean;
  /** The components now in the cell (CONTRACTS §1); null while not known. */
  cellComponents?: readonly string[] | null;
}

function scenarioEntryKey(entry: ScenarioDocumentEntry): string {
  return `${entry.sourceKind}:${entry.path ?? entry.scenarioId ?? entry.name}`;
}

/**
 * One icon-only header command; the tooltip shows at once on hover/focus.
 * While one of its `commands` runs, the tooltip is its busy label.
 */
function ScenarioIconButton({
  label,
  reason,
  disabled,
  operation,
  commands,
  onClick,
  children,
}: {
  label: string;
  reason?: string | null;
  disabled: boolean;
  operation: BusyOperation | null;
  commands: string | readonly string[];
  onClick: (event: MouseEvent<HTMLButtonElement>) => void;
  children: ReactNode;
}) {
  const busy = busyLabel(operation, commands, label);
  return (
    <button
      type="button"
      className="scenario-command"
      aria-label={label}
      aria-busy={busy !== label || undefined}
      data-tooltip={busy !== label ? busy : disabled && reason ? `${label}: ${reason}` : label}
      disabled={disabled}
      onClick={onClick}
    >
      <svg viewBox="0 0 16 16" aria-hidden="true">{children}</svg>
    </button>
  );
}

/**
 * The Scenario-level document commands shared by every workspace.
 *
 * Program and Deployment are separately editable Drafts, but the operator
 * saves and loads them together as one Scenario project.
 */
export function ScenarioDocumentCommands({
  controller,
  deploymentDirty = false,
  localInvalidEdit = false,
  motionActive = false,
  cellComponents = [],
}: ScenarioDocumentCommandsProps) {
  // Document actions wait while robots move (maintainer, 2026-10-16).
  const motionReason = motionActive ? 'Not available while robots are moving. Stop motion first.' : null;
  const {
    state,
    busyOperation: operation,
    documentUnavailableReason,
    scenarioReplacementUnavailableReason,
    saveScenarioDocument,
    exportScenarioDocument,
    scenarioDocumentPath,
    scenarioDocument,
    newScenarioDocument,
    loadScenarioDocument,
  } = controller;
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [saveAsMode, setSaveAsMode] = useState<SaveAsMode>('project');
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [scenarioIndex, setScenarioIndex] = useState<ScenarioDocumentIndexResponse | null>(null);
  const [scenarioIndexError, setScenarioIndexError] = useState<string | null>(null);
  const [scenarioIndexLoading, setScenarioIndexLoading] = useState(false);
  const [scenarioDirectory, setScenarioDirectory] = useState('');
  const [selectedScenarioKey, setSelectedScenarioKey] = useState('');
  const [saveDirectory, setSaveDirectory] = useState<string | null>(null);
  const [savePickerOpen, setSavePickerOpen] = useState(false);
  const [scenarioName, setScenarioName] = useState('');
  const [scenarioDescription, setScenarioDescription] = useState('');
  const [newError, setNewError] = useState<string | null>(null);
  const [replacementPending, setReplacementPending] = useState(false);
  const replacementInFlight = useRef(false);
  const [confirmation, setConfirmation] = useState<ReplacementConfirmation | null>(null);
  const replacementMounted = useRef(true);
  useEffect(() => {
    replacementMounted.current = true;
    return () => { replacementMounted.current = false; };
  }, []);
  // The refusal the workspace shows for a refused New or Load.
  const latestError = useRef<string | null>(null);
  latestError.current = state.error;
  // Entry names of the folder Save As writes into, so a clash shows at once.
  const [locationEntries, setLocationEntries] = useState<ReadonlySet<string> | null>(null);
  // Export writes `<exportParent>/<open project folder name>`, keeping the
  // project's one name; only the parent folder is chosen.
  const [exportParent, setExportParent] = useState<string | null>(null);
  const [exportPickerOpen, setExportPickerOpen] = useState(false);
  const [loadPickerOpen, setLoadPickerOpen] = useState(false);
  const scenarioDirectoryRef = useRef('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
  const authoring = workspace?.draft.authoring ?? null;
  const busy = replacementPending || state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const busyText = busyReason(operation) ?? 'Another operation is in progress.';
  const blockReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy
      ? busyText
      : documentUnavailableReason;
  // Ordinary document actions require Idle. New and Load are the server's
  // one Apply path (CONTRACTS §11); the server refuses them during a run.
  const blocked = busy || documentUnavailableReason !== null || localInvalidEdit || workspace === null;
  const replacementEntryReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy ? busyText
      : workspace === null ? 'Program workspace is not loaded.'
        // A finished or stopped run is the server's to decide (R3).
        : scenarioReplacementUnavailableReason ?? null;
  const hasUnsavedChanges = workspace?.hasUnsavedChanges === true || deploymentDirty;
  const draftDiscarded = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true
    || deploymentDirty;
  // New and Load stop and replace whatever is in the cell: they ask
  // once when it isn't empty (or isn't known), or when Drafts would be lost.
  const cellOccupied = cellComponents === null || cellComponents.length > 0;
  const replaceNeedsConfirmation = cellOccupied || draftDiscarded;
  // The built-in Untitled Scenario is never written: only Save As places it.
  const readOnly = scenarioDocument.readOnly;
  const ownProject = readOnly ? null : scenarioDocumentPath;
  // Plain Save writes back to the open project folder under its one name.
  const openProject = ownProject !== null && scenarioDocument.id !== null
    ? ownProject : null;
  const saveTarget = openProject !== null
    ? {
        directory: parentFolder(openProject),
        name: scenarioProjectStem(openProject),
        description: scenarioDocument.description ?? '',
      }
    : null;
  const selectedScenarioDocument = scenarioIndex?.entries.find(
    (entry) => scenarioEntryKey(entry) === selectedScenarioKey,
  ) ?? null;
  const selectedScenarioPath = selectedScenarioDocument !== null
    && selectedScenarioDocument.displayStatus !== 'unsupported'
    ? selectedScenarioDocument.path
    : null;
  // The index lists one directory, so two projects never share a name in it.
  const selectedScenarioLabel = selectedScenarioDocument === null ? null : scenarioEntryLabel(selectedScenarioDocument);
  const nameError = scenarioNameError(scenarioName);
  const nameTaken = nameError === null && scenarioNameTaken(scenarioName, locationEntries);
  const savePath = saveDirectory === null || nameError !== null ? null : scenarioProjectPath(saveDirectory, scenarioName);
  // Replacing another existing project needs the explicit overwrite choice;
  // saving the open project back to its own folder does not.
  const saveReplacesOther = nameTaken && savePath !== null
    && (openProject === null || savePath !== openProject.replace(/\/+$/, ''));
  const exportName = ownProject === null ? null : scenarioProjectStem(ownProject);
  const exportPath = exportParent === null || exportName === null ? null : scenarioProjectPath(exportParent, exportName);
  const exportTaken = exportName !== null && scenarioNameTaken(exportName, locationEntries);

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
      return index;
    } catch (error: unknown) {
      setScenarioIndexError(
        errorMessage(error, 'Scenario directory could not be read.'),
      );
      return null;
    } finally {
      setScenarioIndexLoading(false);
    }
  }, []);

  useEffect(() => {
    if (action === 'load') {
      void refreshScenarioIndex(scenarioDirectoryRef.current || undefined);
    }
  }, [action, refreshScenarioIndex]);

  // Browse… for Load picks one project: list its folder and select it there,
  // so Load uses the same listed entry as choosing it from the list.
  const chooseLoadProject = async (projectPath: string) => {
    const index = await refreshScenarioIndex(parentFolder(projectPath));
    if (index === null) return;
    const chosen = projectPath.replace(/\/+$/, '');
    const entry = index.entries.find((candidate) => candidate.path?.replace(/\/+$/, '') === chosen);
    if (entry === undefined || entry.displayStatus === 'unsupported') {
      setSelectedScenarioKey('');
      setScenarioIndexError(entry === undefined
        ? 'The chosen project is not a loadable Scenario.' : entry.statusDetail);
      return;
    }
    setSelectedScenarioKey(scenarioEntryKey(entry));
  };

  // Names already present in the chosen location, so a clash shows at once.
  const locationDirectory = action === 'save_as'
    ? (saveAsMode === 'portable' ? exportParent : saveDirectory) : null;
  useEffect(() => {
    if (locationDirectory === null) return undefined;
    const controller = new AbortController();
    setLocationEntries(null);
    api.files.browse('scenario', locationDirectory, controller.signal).then(
      (listing) => setLocationEntries(new Set(listing.entries.map((entry) => entry.name))),
      () => { if (!controller.signal.aborted) setLocationEntries(null); },
    );
    return () => controller.abort();
  }, [locationDirectory]);

  const closeAction = useCallback(() => {
    if (replacementInFlight.current) return;
    setConfirmation(null);
    setNewError(null);
    setAction(null);
    setConfirmOverwrite(false);
  }, []);
  const beginAction = (
    next: ScenarioAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    returnFocus.current = event.currentTarget;
    setNewError(null);
    setConfirmOverwrite(false);
    setLoadPickerOpen(false);
    if (next === 'save_as') {
      // The open project's folder name is its one name; a legacy folder name
      // outside the rule is shown so the operator corrects it.
      setSaveAsMode('project');
      setSavePickerOpen(false);
      setExportParent(null);
      setExportPickerOpen(false);
      setScenarioName(scenarioDocumentPath !== null
        ? scenarioProjectStem(scenarioDocumentPath) : authoring?.program_id ?? '');
      setScenarioDescription(scenarioDocument.description ?? authoring?.description ?? '');
      setSaveDirectory(ownProject !== null ? parentFolder(ownProject) : null);
      if (ownProject === null) {
        void refreshScenarioIndex(scenarioDirectoryRef.current || undefined)
          .then((index) => {
            if (index) setSaveDirectory((current) => current ?? index.directory);
          });
      }
    }
    setAction(next);
  };
  const complete = (operation: () => void) => {
    operation();
    closeAction();
  };

  // New and Load replace the Scenario through the server's one Apply path:
  // New is the Apply of the empty cell; Load is New, then Apply of the file.
  const replacesScenario = action === 'new' || action === 'load';
  const replacementReason = newError ?? (replacementPending ? null
    : replacesScenario ? replacementEntryReason : blockReason);
  const completeReplacement = async (kind: 'new' | 'load', acknowledged = false) => {
    if (replacementInFlight.current) return;
    const path = kind === 'load' ? selectedScenarioPath : null;
    if (replacementEntryReason !== null || (kind === 'load' && path === null)) return;
    // Stopping the cell's components or discarding Draft changes is the
    // operator's choice: one confirmation, the same for New and Load.
    if (replaceNeedsConfirmation && !acknowledged) {
      setConfirmation({ kind, path });
      return;
    }
    setConfirmation(null);
    replacementInFlight.current = true;
    setReplacementPending(true);
    setNewError(null);
    let refusal: string | null = null;
    const onRefusal = (message: string) => { refusal = message; };
    try {
      const result = kind === 'new' ? await newScenarioDocument(onRefusal)
        : await loadScenarioDocument(path as string, onRefusal);
      if (!replacementMounted.current) return;
      if (result !== null) setAction(null);
      else setNewError(refusal ?? latestError.current
        ?? `${kind === 'new' ? 'New' : 'Load'} was refused; nothing was retried.`);
    } finally {
      replacementInFlight.current = false;
      if (replacementMounted.current) setReplacementPending(false);
    }
  };
  // New has no form: it asks only when Draft changes would be discarded.
  useEffect(() => {
    if (action === 'new' && confirmation === null && !replacementPending && newError === null
      && replacementEntryReason === null) void completeReplacement('new');
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [action]);
  // Same look as the Reset/Execute confirmations: a short lead, the
  // consequences as a list, then what stays physically true.
  const replaceWarning = !cellOccupied
    ? <>
      <p>Discard the current Draft and continue?</p>
      <p>Current unsaved and unapplied Draft changes will be discarded.</p>
    </>
    : <>
      <p>
        The current Scenario is closed first
        {cellComponents === null ? '; its components could not be read.'
          : `: ${cellComponents.join(', ')}.`}
      </p>
      <ul>
        <li>a finished or stopped Program run is Reset first, ending the run;</li>
        <li>every component of the current cell is disabled and its processes are stopped;</li>
        {confirmation?.kind === 'load'
          && <li>the loaded Scenario&apos;s components are started and come back prepared, not enabled;</li>}
        {draftDiscarded && <li>Current unsaved and unapplied Draft changes will be discarded.</li>}
      </ul>
      <p>Robots and physically held parts may remain; account for them before using the next cell.</p>
    </>;
  const confirmationTitle = confirmation?.kind === 'new' ? 'New Scenario?' : 'Load Scenario?';

  return (
    <>
      <div className="scenario-commands" role="group" aria-label="Scenario">
        <ScenarioIconButton
          label="New"
          reason={motionReason ?? replacementEntryReason}
          disabled={motionReason !== null || replacementEntryReason !== null}
          operation={operation}
          commands="new"
          onClick={(event) => beginAction('new', event)}
        >
          <path d="M3.5 2.5h5.75L12.5 5.75v7.75a.5.5 0 0 1-.5.5H3.5a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5z" />
          <path d="M9.25 2.5v3.25h3.25M7.75 7.5v4.5M5.5 9.75h4.5" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label={saveTarget || readOnly ? 'Save Scenario' : 'Save Scenario (choose a project folder)'}
          reason={motionReason ?? blockReason
            ?? (readOnly ? 'The Untitled Scenario is read-only. Use Save Scenario as.' : 'Scenario already matches Saved.')}
          disabled={motionReason !== null || blocked || readOnly || (saveTarget !== null && !hasUnsavedChanges)}
          operation={operation}
          commands="save"
          onClick={(event) => {
            if (saveTarget === null) {
              beginAction('save_as', event);
              return;
            }
            saveScenarioDocument({ ...saveTarget, confirmOverwrite: false });
          }}
        >
          <path d="M3 2.5h8L13.5 5v8.5a.5.5 0 0 1-.5.5H3a.5.5 0 0 1-.5-.5v-10a.5.5 0 0 1 .5-.5z" />
          <path d="M5 2.5V6h5.5V2.5M5 14v-4.5h6V14" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label="Save Scenario as"
          reason={motionReason ?? blockReason}
          disabled={motionReason !== null || blocked}
          operation={operation}
          commands={['save_as', 'export']}
          onClick={(event) => beginAction('save_as', event)}
        >
          <path d="M3 2.5h6.5L12 5v3" />
          <path d="M3 2.5a.5.5 0 0 0-.5.5v10a.5.5 0 0 0 .5.5h4.5" />
          <path d="M5 2.5V5.5h4V2.5" />
          <path d="M13.7 9.6l-3.9 3.9-1.4.4.4-1.4 3.9-3.9a1 1 0 0 1 1 1z" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label="Load Scenario"
          reason={motionReason ?? replacementEntryReason}
          disabled={motionReason !== null || replacementEntryReason !== null}
          operation={operation}
          commands="load"
          onClick={(event) => beginAction('load', event)}
        >
          <path d="M2 4.5a.5.5 0 0 1 .5-.5H6l1.5 1.5h6a.5.5 0 0 1 .5.5V7H2z" />
          <path d="M2 7h12.5l-1.6 5.6a.5.5 0 0 1-.5.4H3.3a.5.5 0 0 1-.5-.4L2 7z" />
        </ScenarioIconButton>
        {newError !== null && action === null
          && <p className="inline-message severity-error" role="status">{newError}</p>}
      </div>

      <OverlayPanel
        open={action !== null && confirmation === null}
        title={action === 'new' ? 'New Scenario' : action === 'save_as' ? 'Save Scenario as' : 'Load Scenario'}
        titleId="scenario-action-dialog-title"
        compact={false}
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action === 'new' && <div className="document-dialog">
          <p>Empty the cell and open the empty Untitled Scenario. Use Save Scenario as to name and place it.</p>
          {replacementReason && <p className="inline-message severity-error" role="status">{replacementReason}</p>}
          <div className="dialog-actions">
            <button type="button" onClick={closeAction} disabled={replacementPending}>Cancel</button>
          </div>
        </div>}

        {action === 'save_as' && (
          <div className="dpanel new-scenario-dialog scenario-document-dialog">
            <div className="segmented-control scenario-save-mode" role="radiogroup" aria-label="Save format">
              <button type="button" role="radio" aria-checked={saveAsMode === 'project'} className={saveAsMode === 'project' ? 'active' : ''} onClick={() => setSaveAsMode('project')}>Project</button>
              <button type="button" role="radio" aria-checked={saveAsMode === 'portable'} className={saveAsMode === 'portable' ? 'active' : ''} onClick={() => setSaveAsMode('portable')}>Portable copy</button>
            </div>
            {saveAsMode === 'project' ? (
              <>
                <p className="new-scenario-intro">
                  Save the Program, Scene, Deployment, assignment and overrides as one project folder, and continue working on it.
                </p>
                <section className="dcard" aria-label="Scenario">
                  <h4>Scenario</h4>
                  <label className="stacked">
                    <span>Name <span className="required-mark" aria-hidden="true">*</span></span>
                    <input
                      value={scenarioName}
                      required
                      aria-required="true"
                      aria-invalid={nameError !== null}
                      aria-describedby="save-scenario-name-help"
                      maxLength={SCENARIO_NAME_MAX}
                      spellCheck={false}
                      autoCapitalize="off"
                      onChange={(event) => setScenarioName(event.target.value)}
                    />
                    <small
                      id="save-scenario-name-help"
                      className={scenarioName !== '' && nameError !== null ? 'field-error' : 'field-hint'}
                      role={scenarioName !== '' && nameError !== null ? 'alert' : undefined}
                    >
                      {scenarioName !== '' && nameError !== null ? nameError : 'Letters, digits, “_” and “-” only, no spaces or dots. Also the project folder name.'}
                    </small>
                  </label>
                  <div className="new-scenario-location">
                    <span>Location</span>
                    <code title={saveDirectory ?? undefined}>{saveDirectory ?? 'Reading default location…'}</code>
                    <button type="button" onClick={() => setSavePickerOpen((open) => !open)} aria-expanded={savePickerOpen}>
                      Browse…
                    </button>
                  </div>
                  {savePickerOpen && (
                    <FolderPicker
                      onCancel={() => setSavePickerOpen(false)}
                      onChoose={(path) => {
                        setSaveDirectory(path);
                        setSavePickerOpen(false);
                      }}
                    />
                  )}
                  <small className={saveReplacesOther && !confirmOverwrite ? 'field-error' : 'field-hint'}>
                    {savePath === null ? 'Saved as a project folder named after the Scenario.'
                      : saveReplacesOther
                        ? <>A project <code>{savePath}</code> already exists. Allow replacing it to overwrite it.</>
                        : <>Saved as <code>{savePath}</code></>}
                  </small>
                  <label className="confirmation-check">
                    <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
                    Allow replacing an existing project
                  </label>
                  <label className="stacked">
                    <span>Description</span>
                    <input value={scenarioDescription} onChange={(event) => setScenarioDescription(event.target.value)} />
                    <small className="field-hint">Free text, e.g. a readable title. Robot meshes and other referenced sources stay where they are.</small>
                  </label>
                </section>
                <div className="dialog-actions new-scenario-actions">
                  <button type="button" onClick={closeAction}>Cancel</button>
                  <button
                    type="button"
                    className="primary-action"
                    disabled={blocked || saveDirectory === null || nameError !== null
                      || (saveReplacesOther && !confirmOverwrite)}
                    title={blockReason ?? nameError ?? undefined}
                    onClick={() => {
                      if (saveDirectory === null || nameError !== null) return;
                      complete(() => saveScenarioDocument({
                        directory: saveDirectory,
                        name: scenarioName,
                        description: scenarioDescription.trim(),
                        confirmOverwrite,
                      }));
                    }}
                  >
                    Save Scenario
                  </button>
                </div>
              </>
            ) : (
              <>
                <p className="new-scenario-intro">
                  Write a self-contained copy of the saved project, including the robot sources and Scene assets it references. The open project is not changed.
                </p>
                {ownProject === null && (
                  <p className="inline-message severity-warning">Save the Scenario as a project first.</p>
                )}
                <section className="dcard" aria-label="Destination">
                  <h4>Destination</h4>
                  <div className="new-scenario-location">
                    <span>Location</span>
                    <code title={exportParent ?? undefined}>{exportParent ?? 'Choose where to write the copy.'}</code>
                    <button type="button" disabled={ownProject === null} onClick={() => setExportPickerOpen((open) => !open)} aria-expanded={exportPickerOpen}>
                      Browse…
                    </button>
                  </div>
                  {exportPickerOpen && (
                    <FolderPicker
                      onCancel={() => setExportPickerOpen(false)}
                      onChoose={(path) => {
                        setExportParent(path);
                        setExportPickerOpen(false);
                      }}
                    />
                  )}
                  <small className={exportTaken ? 'field-error' : 'field-hint'} role={exportTaken ? 'alert' : undefined}>
                    {exportPath === null ? 'The copy is a new project folder with the same name.'
                      : exportTaken
                        ? <>A project <code>{exportPath}</code> already exists. Choose another location.</>
                        : <>Written as <code>{exportPath}</code></>}
                  </small>
                </section>
                <div className="dialog-actions new-scenario-actions">
                  <button type="button" onClick={closeAction}>Cancel</button>
                  <button
                    type="button"
                    className="primary-action"
                    disabled={busy || exportPath === null || exportTaken}
                    onClick={() => {
                      if (exportPath === null) return;
                      complete(() => exportScenarioDocument(exportPath));
                    }}
                  >
                    Export copy
                  </button>
                </div>
              </>
            )}
          </div>
        )}

        {action === 'load' && (
          <div className="dpanel new-scenario-dialog scenario-document-dialog">
            <p className="new-scenario-intro">
              Load replaces the current Scenario and applies its Deployment, Scene and Program. It does not start motion.
            </p>
            <section className="dcard" aria-label="Location">
              <h4>Location</h4>
              <div className="scenario-directory-row">
                <label className="stacked">
                  <span>Scenario directory</span>
                  <input value={scenarioDirectory} disabled={replacementPending} onChange={(event) => {
                    setScenarioDirectory(event.target.value);
                    scenarioDirectoryRef.current = event.target.value;
                  }} />
                </label>
                <button type="button" disabled={replacementPending || scenarioIndexLoading} onClick={() => void refreshScenarioIndex(scenarioDirectory)}>
                  {scenarioIndexLoading ? 'Reading…' : 'Refresh'}
                </button>
                <button type="button" disabled={replacementPending} onClick={() => setLoadPickerOpen((open) => !open)} aria-expanded={loadPickerOpen}>
                  Browse…
                </button>
              </div>
              {loadPickerOpen && (
                <FolderPicker
                  chooseProject
                  onCancel={() => setLoadPickerOpen(false)}
                  onChoose={(path) => {
                    setLoadPickerOpen(false);
                    void chooseLoadProject(path);
                  }}
                />
              )}
              {scenarioIndexError && <div className="document-apply-blocked" role="alert">{scenarioIndexError}</div>}
            </section>
            <section className="dcard" aria-label="Scenario">
              <h4>Scenario</h4>
              <label className="stacked">
                <span>Available Scenario</span>
                <select
                  value={selectedScenarioKey}
                  disabled={replacementPending || scenarioIndexLoading || scenarioIndex === null}
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
                      {scenarioEntryLabel(entry)}
                    </option>
                  ))}
                </select>
                <small className="field-hint">The list shows saved metadata, not current readiness. The selected Scenario is validated when you load it.</small>
              </label>
              {selectedScenarioDocument && (
                <div className={`scenario-status scenario-status-${selectedScenarioDocument.displayStatus}`}>
                  <strong>{selectedScenarioLabel}</strong>
                  <span>{selectedScenarioDocument.statusDetail}</span>
                  {selectedScenarioDocument.description && (
                    <span>{selectedScenarioDocument.description}</span>
                  )}
                </div>
              )}
            </section>
            {replacementReason && <p className="inline-message severity-error" role="status">{replacementReason}</p>}
            <div className="dialog-actions new-scenario-actions">
              <button type="button" onClick={closeAction} disabled={replacementPending}>Cancel</button>
              <button
                type="button"
                className="primary-action"
                disabled={busy || (replacementReason !== null && newError === null) || selectedScenarioPath === null}
                title={replacementReason ?? undefined}
                onClick={() => {
                  if (selectedScenarioPath === null) return;
                  void completeReplacement('load');
                }}
              >
                {busyLabel(operation, 'load', 'Load Scenario')}
              </button>
            </div>
          </div>
        )}
        {replacementPending && <p className="scenario-replacement-status" role="status">Scenario replacement is in progress.</p>}

      </OverlayPanel>
      <OverlayPanel open={confirmation !== null}
        title={confirmationTitle}
        titleId="scenario-replacement-confirmation-title" kind="dialog" compact
        onClose={() => { if (confirmation?.kind === 'new') closeAction(); else setConfirmation(null); }}>
        <div className="dialog-copy transport-confirmation">
          {replaceWarning}
          <div className="dialog-actions">
            <button type="button" onClick={() => { if (confirmation?.kind === 'new') closeAction(); else setConfirmation(null); }}>Cancel</button>
            <button type="button" className="danger-button" onClick={() => {
              if (confirmation !== null) void completeReplacement(confirmation.kind, true);
            }}>{confirmation?.kind === 'new' ? 'Empty the cell and continue' : 'Load Scenario'}</button>
          </div>
        </div>
      </OverlayPanel>
    </>
  );
}
