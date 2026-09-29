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
  ManagerActiveProjection,
} from '../types/api';
import { FolderPicker } from './FolderPicker';
import { OverlayPanel } from './OverlayPanel';
import {
  newScenarioFolderError,
  newScenarioNameError,
  parentFolder,
  scenarioFolderName,
  SCENARIO_NAME_MAX,
} from '../state/newScenario';

type ScenarioAction = 'new' | 'save_as' | 'load';
type SaveAsMode = 'project' | 'portable';

interface ScenarioDocumentCommandsProps {
  controller: ProgramWorkspaceController;
  /** The Deployment Draft differs from its Saved baseline. */
  deploymentDirty?: boolean;
  localInvalidEdit?: boolean;
}

const SCENARIO_ID_PATTERN = /^[a-z][a-z0-9._-]{0,127}$/;
function scenarioEntryKey(entry: ScenarioDocumentEntry): string {
  return `${entry.sourceKind}:${entry.path ?? entry.scenarioId ?? entry.name}`;
}

/** One icon-only header command; the tooltip shows at once on hover/focus. */
function ScenarioIconButton({
  label,
  reason,
  disabled,
  onClick,
  children,
}: {
  label: string;
  reason?: string | null;
  disabled: boolean;
  onClick: (event: MouseEvent<HTMLButtonElement>) => void;
  children: ReactNode;
}) {
  return (
    <button
      type="button"
      className="scenario-command"
      aria-label={label}
      data-tooltip={disabled && reason ? `${label}: ${reason}` : label}
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
}: ScenarioDocumentCommandsProps) {
  const {
    state,
    mutationAvailable,
    mutationUnavailableReason,
    newScenarioDocument,
    saveScenarioDocument,
    loadScenarioDocument,
    exportScenarioDocument,
    scenarioDocumentPath,
    scenarioDocument,
  } = controller;
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [saveAsMode, setSaveAsMode] = useState<SaveAsMode>('project');
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [scenarioIndex, setScenarioIndex] = useState<ScenarioDocumentIndexResponse | null>(null);
  const [scenarioIndexError, setScenarioIndexError] = useState<string | null>(null);
  const [scenarioIndexLoading, setScenarioIndexLoading] = useState(false);
  const [scenarioDirectory, setScenarioDirectory] = useState('');
  const [selectedScenarioKey, setSelectedScenarioKey] = useState('');
  const [savePath, setSavePath] = useState('');
  const [scenarioId, setScenarioId] = useState('scenario');
  const [scenarioName, setScenarioName] = useState('Scenario');
  const [scenarioDescription, setScenarioDescription] = useState('');
  const [deploymentChoice, setDeploymentChoice] = useState<'keep_applied' | 'empty'>('keep_applied');
  const [newApplied, setNewApplied] = useState<ManagerActiveProjection | null>(null);
  const [newError, setNewError] = useState<string | null>(null);
  const [newWorkspaceKey, setNewWorkspaceKey] = useState<string | null>(null);
  const [newParent, setNewParent] = useState<string | null>(null);
  const [newFolderName, setNewFolderName] = useState('');
  const [newFolderEdited, setNewFolderEdited] = useState(false);
  const [newNameTouched, setNewNameTouched] = useState(false);
  const [newPickerOpen, setNewPickerOpen] = useState(false);
  const [newParentEntries, setNewParentEntries] = useState<ReadonlySet<string> | null>(null);
  const [exportDestinationPath, setExportDestinationPath] = useState('');
  const scenarioDirectoryRef = useRef('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
  const workspaceKey = workspace === null ? null : JSON.stringify(workspace.authoringWorkspace);
  const newConfirmationStale = action === 'new' && newWorkspaceKey !== workspaceKey;
  const managedComponents = newApplied?.cellGraph.controllable_components;
  const managedComponentNames = Array.isArray(managedComponents)
    ? managedComponents.flatMap((row) => row !== null && typeof row === 'object'
      && !Array.isArray(row) && typeof row.controllable_component_id === 'string'
      ? [row.controllable_component_id] : []) : [];
  const authoring = workspace?.draft.authoring ?? null;
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const blockReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy
      ? 'Another operation is in progress.'
      : mutationUnavailableReason;
  const blocked = busy || !mutationAvailable || localInvalidEdit || workspace === null;
  const hasUnsavedChanges = workspace?.hasUnsavedChanges === true || deploymentDirty;
  const replaceNeedsConfirmation = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true
    || deploymentDirty;
  const saveTarget = scenarioDocumentPath !== null
    && scenarioDocument.id !== null
    && scenarioDocument.name !== null
    ? {
        path: scenarioDocumentPath,
        scenarioId: scenarioDocument.id,
        name: scenarioDocument.name,
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
  const scenarioIdValid = SCENARIO_ID_PATTERN.test(scenarioId.trim());
  const newFolder = newFolderEdited ? newFolderName : scenarioFolderName(scenarioName);
  const newNameError = newScenarioNameError(scenarioName);
  const newFolderError = newParent === null
    ? 'Choose where to create the project.'
    : newScenarioFolderError(newFolder, newParentEntries);
  const newPath = newParent === null ? null : `${newParent.replace(/\/+$/, '')}/${newFolder.trim()}`;

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
        error instanceof Error ? error.message : 'Scenario directory could not be read.',
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

  useEffect(() => {
    if (action !== 'new') return;
    const abort = new AbortController();
    setNewApplied(null);
    setNewError(null);
    void api.manager.status(abort.signal).then((value) => {
      if (abort.signal.aborted) return;
      if (!value.source.available || value.source.freshness !== 'fresh' || !value.status.managerReady) {
        setNewError('Current Applied Deployment cannot be verified.');
      } else setNewApplied(value.status.active);
    }, (error: unknown) => {
      if (!abort.signal.aborted) setNewError(error instanceof Error ? error.message : 'Applied Deployment is unavailable.');
    });
    return () => abort.abort();
  }, [action]);

  // New Scenario: default location next to the current Scenario, otherwise
  // the Scenario folder the backend offers for Load.
  useEffect(() => {
    if (action !== 'new') return;
    if (scenarioDocumentPath !== null) {
      setNewParent(parentFolder(scenarioDocumentPath));
      return;
    }
    void refreshScenarioIndex(scenarioDirectoryRef.current || undefined).then((index) => {
      if (index) setNewParent((current) => current ?? index.directory);
    });
  }, [action, refreshScenarioIndex, scenarioDocumentPath]);

  // Names already present in the chosen location, so a clash shows at once.
  useEffect(() => {
    if (action !== 'new' || newParent === null) return undefined;
    const controller = new AbortController();
    setNewParentEntries(null);
    api.files.browse('program', newParent, controller.signal).then(
      (listing) => setNewParentEntries(new Set(listing.entries.map((entry) => entry.name))),
      () => { if (!controller.signal.aborted) setNewParentEntries(null); },
    );
    return () => controller.abort();
  }, [action, newParent]);

  const closeAction = useCallback(() => {
    setAction(null);
    setConfirmDiscard(false);
    setConfirmOverwrite(false);
  }, []);
  const beginAction = (
    next: ScenarioAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    returnFocus.current = event.currentTarget;
    setConfirmDiscard(false);
    setConfirmOverwrite(false);
    if (next === 'new') {
      setNewWorkspaceKey(workspaceKey);
      setScenarioName('');
      setNewParent(null);
      setNewFolderName('');
      setNewFolderEdited(false);
      setNewNameTouched(false);
      setNewPickerOpen(false);
      setDeploymentChoice('keep_applied');
    }
    if (next === 'save_as') {
      const id = scenarioDocument.id ?? authoring?.program_id ?? 'scenario';
      setSaveAsMode('project');
      setScenarioId(id);
      setScenarioName(scenarioDocument.name ?? authoring?.name ?? 'Scenario');
      setScenarioDescription(scenarioDocument.description ?? authoring?.description ?? '');
      setSavePath(scenarioDocumentPath ?? '');
      if (scenarioDocumentPath === null) {
        void refreshScenarioIndex(scenarioDirectoryRef.current || undefined)
          .then((index) => {
            if (index) setSavePath((current) => current || `${index.directory}/${id}`);
          });
      }
    }
    setAction(next);
  };
  const complete = (operation: () => void) => {
    operation();
    closeAction();
  };

  return (
    <>
      <div className="scenario-commands" role="group" aria-label="Scenario">
        <ScenarioIconButton
        label="New"
          reason={blockReason}
          disabled={blocked}
          onClick={(event) => beginAction('new', event)}
        >
          <path d="M3.5 2.5h5.75L12.5 5.75v7.75a.5.5 0 0 1-.5.5H3.5a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5z" />
          <path d="M9.25 2.5v3.25h3.25M7.75 7.5v4.5M5.5 9.75h4.5" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label={saveTarget ? 'Save Scenario' : 'Save Scenario (choose a project folder)'}
          reason={blockReason ?? 'Scenario already matches Saved.'}
          disabled={blocked || (saveTarget !== null && !hasUnsavedChanges)}
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
          reason={blockReason}
          disabled={blocked}
          onClick={(event) => beginAction('save_as', event)}
        >
          <path d="M3 2.5h6.5L12 5v3" />
          <path d="M3 2.5a.5.5 0 0 0-.5.5v10a.5.5 0 0 0 .5.5h4.5" />
          <path d="M5 2.5V5.5h4V2.5" />
          <path d="M13.7 9.6l-3.9 3.9-1.4.4.4-1.4 3.9-3.9a1 1 0 0 1 1 1z" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label="Load Scenario"
          reason={blockReason}
          disabled={blocked}
          onClick={(event) => beginAction('load', event)}
        >
          <path d="M2 4.5a.5.5 0 0 1 .5-.5H6l1.5 1.5h6a.5.5 0 0 1 .5.5V7H2z" />
          <path d="M2 7h12.5l-1.6 5.6a.5.5 0 0 1-.5.4H3.3a.5.5 0 0 1-.5-.4L2 7z" />
        </ScenarioIconButton>
      </div>

      <OverlayPanel
        open={action !== null}
        title={action === 'new' ? 'New Scenario' : action === 'save_as' ? 'Save Scenario as' : 'Load Scenario'}
        titleId="scenario-action-dialog-title"
        compact={false}
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action === 'new' && (
          <div className="dpanel new-scenario-dialog">
            <p className="new-scenario-intro">
              Close the current Scenario and start a new saved one with an empty
              Program and Scene. The current Scenario&apos;s saved files are not changed.
            </p>
            {replaceNeedsConfirmation && (
              <p className="inline-message severity-warning">
                Current unsaved and unapplied Draft changes will be discarded.
              </p>
            )}
            <section className="dcard" aria-label="Scenario">
              <h4>Scenario</h4>
              <label className="stacked">
                <span>Name <span className="required-mark" aria-hidden="true">*</span></span>
                <input
                  value={scenarioName}
                  required
                  aria-required="true"
                  aria-invalid={newNameTouched && newNameError !== null}
                  aria-describedby="new-scenario-name-help"
                  maxLength={SCENARIO_NAME_MAX}
                  placeholder="e.g. lite6_pick_and_place"
                  spellCheck={false}
                  autoCapitalize="off"
                  autoFocus
                  onChange={(event) => {
                    setScenarioName(event.target.value);
                    setNewNameTouched(true);
                  }}
                />
                <small
                  id="new-scenario-name-help"
                  className={newNameTouched && newNameError !== null ? 'field-error' : 'field-hint'}
                  role={newNameTouched && newNameError !== null ? 'alert' : undefined}
                >
                  {newNameTouched && newNameError !== null ? newNameError : 'Letters, digits, “_” and “-” only, no spaces. Also used as the project folder name.'}
                </small>
              </label>
              <div className="new-scenario-location">
                <span>Location</span>
                <code title={newParent ?? undefined}>{newParent ?? 'Reading default location…'}</code>
                <button type="button" onClick={() => setNewPickerOpen((open) => !open)} aria-expanded={newPickerOpen}>
                  Browse…
                </button>
              </div>
              {newPickerOpen && (
                <FolderPicker
                  initialPath={newParent}
                  onCancel={() => setNewPickerOpen(false)}
                  onChoose={(path) => {
                    setNewParent(path);
                    setNewPickerOpen(false);
                  }}
                />
              )}
              <label className="stacked">
                <span>Project folder</span>
                <input
                  value={newFolder}
                  aria-invalid={newFolderError !== null}
                  aria-describedby="new-scenario-folder-help"
                  onChange={(event) => {
                    setNewFolderEdited(true);
                    setNewFolderName(event.target.value);
                  }}
                />
                <small
                  id="new-scenario-folder-help"
                  className={newFolderError !== null && newParent !== null ? 'field-error' : 'field-hint'}
                >
                  {newFolderError !== null && newParent !== null
                    ? newFolderError
                    : newPath !== null ? <>Will be created as <code>{newPath}</code></> : 'A new folder for this project.'}
                </small>
              </label>
            </section>
            <section className="dcard" aria-label="Deployment">
              <h4>Deployment</h4>
              <label className="stacked">
                <span>Components</span>
                <select value={deploymentChoice} onChange={(event) => setDeploymentChoice(event.target.value as 'keep_applied' | 'empty')}>
                  <option value="keep_applied">Keep the Applied components</option>
                  <option value="empty">Start empty (unload components)</option>
                </select>
                <small className="field-hint">
                  {deploymentChoice === 'keep_applied'
                    ? 'Uses the currently Applied components, not unapplied Deployment Draft edits.'
                    : 'Stops and unloads all managed components. Hardware is not powered off.'}
                  {newApplied !== null && ` ${deploymentChoice === 'keep_applied' ? 'Kept' : 'Unloaded'}: ${managedComponentNames.join(', ') || 'none'}.`}
                </small>
              </label>
              {deploymentChoice === 'empty' && (
                <p className="inline-message severity-warning" role="note">
                  Known limitation: unloading all native components currently fails and
                  locks the runtime until the workbench is restarted. Component
                  bring-up and tear-down are planned with the ROS 2 provider lifecycle.
                </p>
              )}
            </section>
            <p className="new-scenario-note">
              The old Scenario&apos;s objects and attachments are cleared through the
              Scene owner; an unsafe attachment removal is refused.
            </p>
            {newConfirmationStale && <p className="inline-message severity-warning" role="status">The workspace changed. Close and reopen New to review the current state.</p>}
            {(newError ?? blockReason) && <p className="inline-message severity-error" role="status">{newError ?? blockReason}</p>}
            <div className="dactions new-scenario-actions">
              <button type="button" onClick={closeAction}>Cancel</button>
              <button type="button" className="primary-action"
                disabled={blocked || newConfirmationStale || newApplied === null
                  || newNameError !== null || newFolderError !== null || newPath === null}
                title={newNameError ?? newFolderError ?? undefined}
                onClick={() => {
                  if (blocked || newConfirmationStale || newApplied === null
                    || newNameError !== null || newFolderError !== null || newPath === null) return;
                  complete(() => newScenarioDocument({
                    path: newPath, name: scenarioName, deploymentChoice,
                    expectedRetainedBundleId: newApplied.retainedBundleId,
                    expectedGraphRevision: newApplied.graphRevision,
                    confirmDiscard: true,
                  }));
                }}>
                {deploymentChoice === 'keep_applied' ? 'Create and activate' : 'Create and unload components'}
              </button>
            </div>
          </div>
        )}

        {action === 'save_as' && (
          <div className="document-dialog">
            <div className="segmented-control scenario-save-mode" role="radiogroup" aria-label="Save format">
              <button type="button" role="radio" aria-checked={saveAsMode === 'project'} className={saveAsMode === 'project' ? 'active' : ''} onClick={() => setSaveAsMode('project')}>Project</button>
              <button type="button" role="radio" aria-checked={saveAsMode === 'portable'} className={saveAsMode === 'portable' ? 'active' : ''} onClick={() => setSaveAsMode('portable')}>Portable copy</button>
            </div>
            {saveAsMode === 'project' ? (
              <div className="dialog-copy">
                <p>
                  Save the Program, Scene, Deployment, assignment and overrides as one project folder, and continue working on it. Robot meshes and other referenced sources stay where they are.
                </p>
                <label className="document-save-as-path">
                  Project folder
                  <input value={savePath} onChange={(event) => setSavePath(event.target.value)} />
                </label>
                <div className="scenario-metadata-grid">
                  <label className="document-save-as-path">
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
                  <label className="document-save-as-path">
                    Name
                    <input value={scenarioName} onChange={(event) => setScenarioName(event.target.value)} />
                  </label>
                </div>
                <label className="document-save-as-path">
                  Description
                  <input value={scenarioDescription} onChange={(event) => setScenarioDescription(event.target.value)} />
                </label>
                <div className="dialog-actions">
                  <label className="confirmation-check">
                    <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
                    Allow replacing an existing project
                  </label>
                  <button
                    type="button"
                    className="primary-action"
                    disabled={blocked || !savePath.trim() || !scenarioIdValid || !scenarioName.trim()}
                    title={blockReason ?? undefined}
                    onClick={() => complete(() => saveScenarioDocument({
                      path: savePath.trim(),
                      scenarioId: scenarioId.trim(),
                      name: scenarioName.trim(),
                      description: scenarioDescription.trim(),
                      confirmOverwrite,
                    }))}
                  >
                    Save Scenario
                  </button>
                </div>
              </div>
            ) : (
              <div className="dialog-copy">
                <p>
                  Write a self-contained copy of the saved project, including the robot sources and Scene assets it references. The open project is not changed.
                </p>
                {scenarioDocumentPath === null && (
                  <p className="inline-message severity-warning">Save the Scenario as a project first.</p>
                )}
                <label className="document-save-as-path">
                  New destination folder
                  <input value={exportDestinationPath} onChange={(event) => setExportDestinationPath(event.target.value)} />
                </label>
                <div className="dialog-actions">
                  <button
                    type="button"
                    className="primary-action"
                    disabled={busy || scenarioDocumentPath === null || !exportDestinationPath.trim()}
                    onClick={() => complete(() => exportScenarioDocument(exportDestinationPath.trim()))}
                  >
                    Export copy
                  </button>
                </div>
              </div>
            )}
          </div>
        )}

        {action === 'load' && (
          <div className="dialog-copy">
            <div className="scenario-directory-row">
              <label className="document-save-as-path">
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
            {scenarioIndexError && <div className="document-apply-blocked" role="alert">{scenarioIndexError}</div>}
            <label className="document-save-as-path">
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
                    {entry.name}
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
              The list shows saved metadata, not current readiness. The selected Scenario is validated when you load it.
            </p>
            <p>
              Load replaces the current Scenario and applies its Deployment, Scene and Program. It does not start motion.
            </p>
            <div className="dialog-actions">
              {replaceNeedsConfirmation && (
                <label className="confirmation-check">
                  <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                  Replace current unsaved or unapplied Draft changes
                </label>
              )}
              <button
                type="button"
                className="primary-action"
                disabled={blocked || selectedScenarioPath === null || (replaceNeedsConfirmation && !confirmDiscard)}
                title={blockReason ?? undefined}
                onClick={() => {
                  if (selectedScenarioPath === null) return;
                  complete(() => loadScenarioDocument(selectedScenarioPath, confirmDiscard));
                }}
              >
                Load Scenario
              </button>
            </div>
          </div>
        )}
      </OverlayPanel>
    </>
  );
}
