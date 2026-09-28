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
import { OverlayPanel } from './OverlayPanel';

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
    newDraft,
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
  const [exportDestinationPath, setExportDestinationPath] = useState('');
  const scenarioDirectoryRef = useRef('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
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
          label="New Scenario"
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
        compact={action === 'new'}
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action === 'new' && (
          <div className="dialog-copy compact-confirmation">
            <p>Start an empty Program Draft. The Deployment and the running configuration do not change until you Apply.</p>
            {replaceNeedsConfirmation && (
              <label className="confirmation-check">
                <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                Discard the current unsaved or unapplied Draft changes
              </label>
            )}
            <button type="button" className="primary-action" disabled={blocked || (replaceNeedsConfirmation && !confirmDiscard)} onClick={() => complete(() => newDraft(confirmDiscard))}>
              Create New Draft
            </button>
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
