import { useEffect, useMemo, useState } from 'react';

import { useProgramWorkspace } from '../hooks/useProgramWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
} from '../types/api';
import { flattenProgramTree } from '../utils/programAuthoring';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramNodePalette } from './ProgramNodePalette';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

interface ProgramWorkspaceProps {
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

export function ProgramWorkspace({
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
    clearFeedback,
  } = useProgramWorkspace();
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
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
            <div className="identity-unavailable">
              <span>Applied</span>
              <strong>Unavailable</strong>
              <small>{workspace.applied.unavailableReason?.message ?? 'No owner-issued Applied identity.'}</small>
            </div>
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
              Allow discarding unsaved Draft changes
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

          {(state.message || state.error) && (
            <div
              className={`program-feedback${state.error ? ' error-state' : ''}`}
              role={state.error ? 'alert' : 'status'}
            >
              <span>{state.error ?? state.message}</span>
              <button type="button" className="text-button" onClick={clearFeedback}>Dismiss</button>
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
          {!program && environmentAligned && (
            <div className="panel-state program-empty-state">
              <strong>No Program Draft is open</strong>
              <span>Select a Program artifact and use Open as Draft. This does not Apply or execute it.</span>
            </div>
          )}

          {program && environmentAligned && (
            <>
              <div className="program-authoring-grid">
                <ProgramNodePalette
                  root={program.tree}
                  catalogEntries={catalogEntries}
                  environment={nodeEnvironment}
                  selectedId={selectedNodeId}
                  busy={authoringBusy}
                  onMutate={mutate}
                />
                <ProgramTreeCanvas
                  root={program.tree}
                  catalogEntries={catalogEntries}
                  environment={nodeEnvironment}
                  selectedId={selectedNodeId}
                  busy={authoringBusy}
                  onSelect={setSelectedNodeId}
                  onMutate={mutate}
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
    </section>
  );
}
