import { useEffect, useMemo, useState } from 'react';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
} from '../types/api';
import { flattenProgramTree } from '../utils/programAuthoring';
import type { ProgramInsertionRequest } from '../utils/programCanvasInteraction';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramNodePalette } from './ProgramNodePalette';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

interface ProgramWorkspaceProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  catalog: StructuralResourceState<CatalogResponse>;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  selectedProgramPath: string | null;
  onRetryCatalog: () => void;
  onRetryEnvironment: () => void;
}

function shortRevision(revision: string | null): string {
  if (!revision) return 'none';
  return revision.length > 22 ? `${revision.slice(0, 18)}…` : revision;
}

function EmptyAuthoringSurface() {
  return (
    <div className="program-authoring-grid program-authoring-empty" aria-label="Empty Program authoring surface">
      <section
        className="program-palette"
        aria-labelledby="empty-palette-title"
        aria-disabled="true"
      >
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Node catalog</p>
            <h3 id="empty-palette-title">Authoring palette</h3>
          </div>
        </header>
        <div className="program-empty-guidance">
          <strong>Open a Draft to use the palette</strong>
          <span>Catalog entries remain available in the Library at left.</span>
        </div>
      </section>
      <section className="program-tree-surface" aria-labelledby="empty-tree-title">
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Program graph</p>
            <h3 id="empty-tree-title">Tree canvas</h3>
          </div>
        </header>
        <div className="program-tree-scroll program-tree-empty">
          <strong>No Program Draft is open</strong>
          <span>Select an artifact in the Library, then choose Open as Draft.</span>
        </div>
      </section>
      <section className="program-properties" aria-labelledby="empty-properties-title">
        <header className="program-subpanel-header">
          <div>
            <p className="eyebrow">Selection</p>
            <h3 id="empty-properties-title">Properties</h3>
          </div>
        </header>
        <div className="program-empty-guidance">
          <strong>Select a node to inspect properties</strong>
          <span>Properties become editable after a Draft and node are selected.</span>
        </div>
      </section>
    </div>
  );
}

export function ProgramWorkspace({
  controller,
  runtime,
  catalog,
  environment,
  selectedProgramPath,
  onRetryCatalog,
  onRetryEnvironment,
}: ProgramWorkspaceProps) {
  const {
    state,
    refresh,
    mutate,
    openDraft,
    save,
    saveAs,
    apply: applyDraft,
    load: loadProgram,
    clearFeedback,
  } = controller;
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [insertionRequest, setInsertionRequest] = useState<ProgramInsertionRequest | null>(null);
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [saveAsPath, setSaveAsPath] = useState('');
  const workspace = state.workspace;
  const program = workspace?.draft.program ?? null;
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const catalogEntries = catalog.status === 'ready' ? catalog.data.entries : [];
  const nodeEnvironment = environment.status === 'ready' ? environment.data : null;
  const environmentAligned = catalog.status === 'ready'
    && environment.status === 'ready'
    && catalog.data.environmentDigest === environment.data.environmentDigest;
  const authoringUnavailable = nodeEnvironment !== null
    && !nodeEnvironment.portableAuthoringAvailable;
  const authoringBusy = busy || authoringUnavailable;
  const scenarioCatalog = runtime.state.catalog;
  const selectedScenario = scenarioCatalog?.scenarios.find(
    (row) => row.scenarioId === runtime.state.selectedScenarioId,
  ) ?? null;
  const replaceNeedsConfirmation = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true;

  useEffect(() => {
    if (workspace?.saved.path) setSaveAsPath(workspace.saved.path);
  }, [workspace?.saved.path]);

  const validNodeIds = useMemo(
    () => new Set(program ? flattenProgramTree(program.tree)
      .map(({ node }) => node.instance_id) : []),
    [program],
  );
  useEffect(() => {
    if (!program) {
      setSelectedNodeId(null);
      return;
    }
    if (!selectedNodeId || !validNodeIds.has(selectedNodeId)) {
      setSelectedNodeId(program.tree.instance_id);
    }
  }, [program, selectedNodeId, validNodeIds]);

  return (
    <section className="panel program-workspace" aria-labelledby="program-workspace-title">
      <header className="panel-header program-workspace-header">
        <div>
          <p className="eyebrow">Composer-owned authoring</p>
          <h2 id="program-workspace-title">Program workspace</h2>
        </div>
        <div className="program-workspace-header-actions">
          <button type="button" className="quiet-button" disabled={busy} onClick={refresh}>
            Refresh Draft
          </button>
          <button
            type="button"
            disabled={busy || !program || !workspace?.saved.available}
            onClick={save}
          >
            Save
          </button>
        </div>
      </header>

      {state.status === 'loading' && workspace === null && (
        <div className="panel-state" role="status">Reading the Program workspace…</div>
      )}
      {state.status === 'error' && workspace === null && (
        <div className="panel-state error-state" role="alert">
          <strong>Program workspace unavailable</strong>
          <span>{state.error}</span>
          <button type="button" onClick={refresh}>Retry workspace</button>
        </div>
      )}

      {workspace && (
        <>
          <div className="program-identity-strip">
            <div className={workspace.hasUnsavedChanges ? 'identity-dirty' : 'identity-current'}>
              <span>Draft</span>
              <strong>{workspace.draft.programLoaded ? shortRevision(workspace.draft.programRevision) : 'empty'}</strong>
              <small>ID {workspace.draft.draftId} · edit {workspace.draft.draftRevision}</small>
            </div>
            <div className={workspace.saved.matchesDraft ? 'identity-current' : 'identity-dirty'}>
              <span>Saved</span>
              <strong>{workspace.saved.available ? shortRevision(workspace.saved.programRevision) : 'not saved'}</strong>
              <small>{workspace.saved.path ?? 'Use Save As to choose a path'}</small>
            </div>
            <div className={workspace.applied.available ? 'identity-current' : 'identity-unavailable'}>
              <span>Applied</span>
              <strong>
                {workspace.applied.available
                  ? shortRevision(workspace.applied.programRevision)
                  : 'Unavailable'}
              </strong>
              <small>
                {workspace.applied.available
                  ? workspace.applied.matchesDraft
                    ? 'Matches the current Draft.'
                    : 'Owner selection differs from Draft.'
                  : workspace.applied.unavailableReason?.message
                    ?? 'No owner-issued Applied identity.'}
              </small>
            </div>
          </div>

          <details className="program-actions-disclosure">
            <summary>Program actions · Open · Save As · Apply · Load</summary>
            <div className="program-actions-popover">
              <div className="program-artifact-actions program-apply-actions">
                <label className="program-save-as-path">
                  Qualified scenario
                  <select
                    aria-label="Qualified scenario"
                    value={runtime.state.selectedScenarioId ?? ''}
                    disabled={busy || scenarioCatalog === null}
                    onChange={(event) => {
                      const scenarioId = event.target.value;
                      if (scenarioId === '') {
                        runtime.clearScenarioSelection();
                      } else {
                        runtime.selectScenario(scenarioId);
                      }
                    }}
                  >
                    <option value="">Select a scenario</option>
                    {scenarioCatalog?.scenarios.map((row) => (
                      <option key={row.scenarioId} value={row.scenarioId}>
                        {row.description
                          ? `${row.scenarioId} — ${row.description}`
                          : row.scenarioId}
                      </option>
                    ))}
                  </select>
                </label>
                <button
                  type="button"
                  disabled={busy || !program || selectedScenario === null}
                  onClick={() => {
                    if (!selectedScenario || !scenarioCatalog) return;
                    applyDraft(
                      selectedScenario.scenarioId,
                      scenarioCatalog.scenarioCatalogRevision,
                    );
                  }}
                >
                  Apply Draft
                </button>
                <button
                  type="button"
                  disabled={
                    busy
                    || selectedProgramPath === null
                    || selectedScenario === null
                    || (replaceNeedsConfirmation && !confirmDiscard)
                  }
                  onClick={() => {
                    if (!selectedProgramPath || !selectedScenario || !scenarioCatalog) return;
                    loadProgram(
                      selectedProgramPath,
                      selectedScenario.scenarioId,
                      scenarioCatalog.scenarioCatalogRevision,
                      confirmDiscard,
                    );
                    setConfirmDiscard(false);
                  }}
                >
                  Load and Apply
                </button>
                <small>
                  Apply changes runtime only. Load validates and applies the selected
                  artifact before replacing Draft and Saved.
                </small>
              </div>

              <div className="program-artifact-actions">
                <div>
                  <span>Artifact-browser selection</span>
                  <code>{selectedProgramPath ?? 'Select a Program artifact below'}</code>
                </div>
                <label className="program-confirmation">
                  <input
                    type="checkbox"
                    checked={confirmDiscard}
                    onChange={(event) => setConfirmDiscard(event.target.checked)}
                  />
                  Allow discarding unsaved or unapplied Draft changes
                </label>
                <button
                  type="button"
                  disabled={
                    busy
                    || selectedProgramPath === null
                    || (workspace.hasUnsavedChanges && !confirmDiscard)
                  }
                  onClick={() => {
                    if (!selectedProgramPath) return;
                    openDraft(selectedProgramPath, confirmDiscard);
                    setConfirmDiscard(false);
                  }}
                >
                  Open as Draft
                </button>
                <label className="program-save-as-path">
                  Save As path
                  <input
                    value={saveAsPath}
                    onChange={(event) => setSaveAsPath(event.target.value)}
                    placeholder="Path inside an approved Program root"
                  />
                </label>
                <label className="program-confirmation">
                  <input
                    type="checkbox"
                    checked={confirmOverwrite}
                    onChange={(event) => setConfirmOverwrite(event.target.checked)}
                  />
                  Allow replacing an existing file
                </label>
                <button
                  type="button"
                  disabled={busy || !program || !saveAsPath.trim()}
                  onClick={() => {
                    saveAs(saveAsPath.trim(), confirmOverwrite);
                    setConfirmOverwrite(false);
                  }}
                >
                  Save As
                </button>
              </div>
            </div>
          </details>

          {(state.message || state.error) && (
            <div
              className={`program-feedback${state.error ? ' error-state' : ''}`}
              role={state.error ? 'alert' : 'status'}
            >
              <span>{state.error ?? state.message}</span>
              <button type="button" className="text-button" onClick={clearFeedback}>Dismiss</button>
            </div>
          )}

          {runtime.active && (
            <div className="panel-state" role="status">
              <strong>Run mode is active</strong>
              <span>
                This run keeps its captured Program and Binding. Draft, Save,
                Apply, and Load affect only later runs unless the Manager
                rejects an affected deployment change as busy.
              </span>
            </div>
          )}

          {catalog.status !== 'ready' && (
            <div className="panel-state error-state">
              <strong>Exact node catalog unavailable</strong>
              <span>{catalog.status === 'error' || catalog.status === 'absent' ? catalog.message : 'Loading catalog…'}</span>
              <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
            </div>
          )}
          {environment.status !== 'ready' && (
            <div className="panel-state error-state">
              <strong>Node authoring availability unavailable</strong>
              <span>{environment.status === 'error' || environment.status === 'absent' ? environment.message : 'Loading node environment…'}</span>
              <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
            </div>
          )}
          {catalog.status === 'ready'
            && environment.status === 'ready'
            && !environmentAligned && (
              <div className="panel-state error-state" role="alert">
                <strong>Catalog and node environment identities disagree</strong>
                <span>
                  Authoring stays disabled until both reads describe the same
                  immutable environment.
                </span>
                <div className="program-workspace-header-actions">
                  <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
                  <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
                </div>
              </div>
            )}
          {environmentAligned && nodeEnvironment && authoringUnavailable && (
            <div className="panel-state error-state" role="alert">
              <strong>Program mutations are unavailable</strong>
              <span>
                {nodeEnvironment.reasonCodes.length > 0
                  ? `Reason: ${nodeEnvironment.reasonCodes.join(', ')}.`
                  : 'The active node environment does not permit portable authoring.'}
                {nodeEnvironment.restorationGates.length > 0
                  ? ` Restore at ${nodeEnvironment.restorationGates.join(', ')}.`
                  : ''}
              </span>
              <span>Draft inspection, Open as Draft, and Save remain available.</span>
            </div>
          )}
          {program && (
            <>
              <div className="program-authoring-grid">
                <ProgramNodePalette
                  root={program.tree}
                  catalogEntries={catalogEntries}
                  environment={nodeEnvironment}
                  selectedId={selectedNodeId}
                  busy={authoringBusy}
                  onMutate={mutate}
                  dropRequest={insertionRequest}
                  onDropRequestHandled={() => setInsertionRequest(null)}
                />
                <ProgramTreeCanvas
                  root={program.tree}
                  catalogEntries={catalogEntries}
                  environment={nodeEnvironment}
                  selectedId={selectedNodeId}
                  busy={authoringBusy}
                  onSelect={setSelectedNodeId}
                  onMutate={mutate}
                  onStageInsert={(request) => {
                    setSelectedNodeId(request.targetId);
                    setInsertionRequest(request);
                  }}
                />
                <ProgramPropertyEditor
                  root={program.tree}
                  selectedId={selectedNodeId}
                  catalogEntries={catalogEntries}
                  programParameters={program.parameters ?? []}
                  blackboardKeys={program.blackboard?.keys ?? []}
                  busy={authoringBusy}
                  onMutate={mutate}
                />
              </div>
              <ProgramDeclarationsEditor
                root={program.tree}
                parameters={program.parameters ?? []}
                namespaces={program.blackboard?.namespaces ?? []}
                keys={program.blackboard?.keys ?? []}
                busy={authoringBusy}
                onMutate={mutate}
              />
            </>
          )}
        </>
      )}
      {(!workspace || !program) && <EmptyAuthoringSurface />}
    </section>
  );
}
