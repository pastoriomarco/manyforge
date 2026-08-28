import { useCallback, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ArtifactKind, FileBrowserEntry } from '../types/api';
import { ArtifactBrowser } from './ArtifactBrowser';
import { OverlayPanel } from './OverlayPanel';

type Action = 'open' | 'save_as' | 'discard';

export function DeploymentDocumentControls({
  controller,
}: {
  controller: DeploymentWorkspaceController;
}) {
  const [action, setAction] = useState<Action | null>(null);
  const [selectedPath, setSelectedPath] = useState<string | null>(null);
  const [saveAsPath, setSaveAsPath] = useState('');
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const workspace = controller.workspace;
  const chooseArtifact = useCallback((
    _kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => setSelectedPath(entry?.path ?? null), []);
  const close = () => {
    setAction(null);
    setSelectedPath(null);
    setConfirmDiscard(false);
    setConfirmOverwrite(false);
  };
  const complete = async (request: () => Promise<unknown>) => {
    const result = await request();
    if (result !== null) close();
  };
  return (
    <>
      <div className="deployment-document-bar">
        <div className="deployment-document-identity">
          <span>Draft</span>
          <strong>{workspace?.draft.deploymentArtifactId ?? 'No Deployment open'}</strong>
          <small>{workspace?.draft.deploymentRevision ?? 'Open a bounded artifact to begin.'}</small>
        </div>
        <div className="deployment-document-state" aria-label="Deployment document state">
          <span className={workspace?.draft.differsFromSaved ? 'warning' : 'ok'}>
            {workspace?.draft.differsFromSaved ? 'Unsaved' : 'Saved'}
          </span>
          <span className={workspace?.runAdmission.ready ? 'ok' : 'warning'}>
            {workspace?.runAdmission.ready ? 'Applied match' : 'Applied differs'}
          </span>
        </div>
        <div className="deployment-document-actions">
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy}
            onClick={() => setAction('open')}
          >Open</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !workspace?.draft.document || !workspace.saved.path || !workspace.draft.differsFromSaved}
            onClick={() => { void controller.save(); }}
          >Save</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !workspace?.draft.document}
            onClick={() => setAction('save_as')}
          >Save As</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !workspace?.draft.differsFromSaved}
            onClick={() => setAction('discard')}
          >Discard</button>
        </div>
      </div>
      {controller.mutationUnavailableReason && (
        <div className="authoring-unavailable-notice">{controller.mutationUnavailableReason}</div>
      )}
      {!workspace?.runAdmission.ready && workspace?.runAdmission.reason && (
        <div className="deployment-run-gate" role="status">{workspace.runAdmission.reason}. Run remains disabled until Manager Applied matches this Draft.</div>
      )}
      {controller.error && <div className="panel-state error-state" role="alert">{controller.error}</div>}
      {controller.feedback && (
        <div className="program-feedback" role="status">
          {controller.feedback}
          <button type="button" className="quiet-button" onClick={controller.clearFeedback}>Dismiss</button>
        </div>
      )}

      {action !== null && (
        <OverlayPanel
          open
          title={action === 'open' ? 'Open Deployment as Draft' : action === 'save_as' ? 'Save Deployment As' : 'Discard Deployment changes?'}
          titleId="deployment-document-dialog-title"
          onClose={close}
          compact={action === 'discard'}
        >
          {action === 'open' && (
            <div className="program-document-dialog">
              <ArtifactBrowser lockedKind="deployment" onSelectionChange={chooseArtifact} />
              {workspace?.draft.differsFromSaved && (
                <label className="confirmation-check">
                  <input type="checkbox" checked={confirmDiscard} onChange={(event) => setConfirmDiscard(event.target.checked)} />
                  Discard current unsaved Deployment changes
                </label>
              )}
              <button
                type="button"
                className="primary-action"
                disabled={!selectedPath || (workspace?.draft.differsFromSaved === true && !confirmDiscard)}
                onClick={() => {
                  if (selectedPath) void complete(() => controller.open(selectedPath, confirmDiscard));
                }}
              >Open as Draft</button>
            </div>
          )}
          {action === 'save_as' && (
            <div className="program-dialog-copy">
              <label>Deployment path<input value={saveAsPath} onChange={(event) => setSaveAsPath(event.target.value)} /></label>
              <label className="confirmation-check">
                <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
                Allow replacing an existing file
              </label>
              <button
                type="button"
                className="primary-action"
                disabled={!saveAsPath.trim()}
                onClick={() => { void complete(() => controller.saveAs(saveAsPath.trim(), confirmOverwrite)); }}
              >Save As</button>
            </div>
          )}
          {action === 'discard' && (
            <div className="program-dialog-copy program-compact-confirmation">
              <p>Discard the unsaved Deployment Draft changes?</p>
              <button type="button" className="primary-action" onClick={() => { void complete(controller.discard); }}>Discard changes</button>
            </div>
          )}
        </OverlayPanel>
      )}
    </>
  );
}
