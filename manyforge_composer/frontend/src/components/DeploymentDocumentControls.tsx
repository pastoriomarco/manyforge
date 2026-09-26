import { useCallback, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type {
  ArtifactKind,
  FileBrowserEntry,
  ManagerDeploymentImpact,
} from '../types/api';
import { ArtifactBrowser } from './ArtifactBrowser';
import { OverlayPanel } from './OverlayPanel';

type Action = 'open' | 'save_as' | 'discard' | 'revert';

const DISPOSITION_LABEL = {
  add: 'Added',
  remove: 'Removed',
  replace: 'Restarted',
} as const;

/** The Manager's authoritative impact, presented as-is; never re-derived. */
export function DeploymentApplyImpactSummary({
  impact,
}: {
  impact: ManagerDeploymentImpact;
}) {
  const changed = impact.components.filter((row) => row.disposition !== 'retain');
  const retained = impact.components.length - changed.length;
  return (
    <div className="deployment-apply-impact" aria-label="Deployment Apply impact">
      <ul>
        {changed.map((row) => (
          <li key={row.controllable_component_id}>
            <strong>{DISPOSITION_LABEL[row.disposition as keyof typeof DISPOSITION_LABEL]}</strong>
            {' '}
            <code>{row.controllable_component_id}</code>
            {row.replacement_reasons.length > 0
              && ` (changed: ${row.replacement_reasons.join(', ')})`}
          </li>
        ))}
        {retained > 0 && (
          <li>{retained} component{retained === 1 ? '' : 's'} keep running unchanged</li>
        )}
        {impact.detachedComponents.map((id) => (
          <li key={`detached-${id}`}>
            <code>{id}</code> stays in the cell, detached at its authored position
          </li>
        ))}
        {impact.attachmentChangedComponents.length > 0 && (
          <li>
            Attachments change for
            {' '}
            {impact.attachmentChangedComponents.map((id) => <code key={id}>{id} </code>)}
          </li>
        )}
      </ul>
      {impact.program.executable ? (
        <p>Program <code>{impact.program.programId}</code> remains executable.</p>
      ) : (
        <div role="alert">
          <p>
            Program <code>{impact.program.programId}</code> will not be executable
            in this Deployment. It is kept unchanged; edit it to run it again.
          </p>
          <ul>
            {impact.program.unresolvedReferences.map((reference) => (
              <li key={`${reference.component_role_id}/${reference.target_role_id ?? ''}`}>
                {reference.target_role_id !== null
                  ? <>Target role <code>{reference.target_role_id}</code></>
                  : <>Component role <code>{reference.component_role_id}</code></>}
                {' → '}
                <code>{reference.controllable_component_id}</code>
                {reference.command_target_id !== null && <code>/{reference.command_target_id}</code>}
                {': '}
                {reference.reason.replaceAll('_', ' ')}
              </li>
            ))}
          </ul>
          {impact.program.affectedNodes.length > 0 && (
            <p>
              Affected steps: {impact.program.affectedNodes.join(', ')}
              {impact.program.affectedExecutionGroups.length > 0
                && `; groups: ${impact.program.affectedExecutionGroups.join(', ')}`}
            </p>
          )}
        </div>
      )}
    </div>
  );
}

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
  // Manager Applied is the publication identity.  Runtime convergence is a
  // separate readiness concern and must not make the document controls
  // oscillate while an accepted Apply is still settling.
  const draftMatchesManager = workspace !== null
    && workspace.draft.document !== null
    && workspace.draft.deploymentRevision !== null
    && workspace.draft.deploymentRevision
      === workspace.managerApplied.deploymentRevision;
  // Discard is a local Draft-to-Applied operation.  A Draft can remain
  // different from its saved baseline after Apply (which is what makes
  // Revert useful), but there is then nothing left for Discard to do.
  const hasUnappliedChanges = workspace !== null
    && workspace.draft.document !== null
    && workspace.managerApplied.deploymentRevision !== null
    && !draftMatchesManager;
  const pendingConfirmation = controller.pendingApplyConfirmation;
  // Why authoring is blocked comes first, so every disabled control explains
  // itself the same way the Program controls do.
  const blockReason = controller.busy
    ? 'Another Deployment operation is in progress.'
    : controller.mutationUnavailableReason;
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
    // The confirmation is only the user's authorization boundary. Close it
    // as soon as the request is dispatched; busy/error state belongs to the
    // document controls and remains visible if the owner rejects the action.
    close();
    await request();
  };
  return (
    <>
      <div className="document-controls" aria-label="Deployment document controls">
        <div className="document-identity">
          <strong title={workspace?.saved.path ?? workspace?.draft.deploymentRevision ?? undefined}>
            {workspace?.draft.deploymentArtifactId ?? 'No Deployment'}
          </strong>
          <span className="document-identity-divider" aria-hidden="true" />
          <span className={`document-state state-${workspace?.draft.differsFromSaved ? 'modified' : 'draft'}`}>
            {workspace?.draft.differsFromSaved ? 'modified' : 'draft'}
          </span>
          <span className={`document-state state-${workspace?.draft.differsFromSaved ? 'different' : 'saved'}`}>
            {workspace?.draft.differsFromSaved ? 'different' : 'saved'}
          </span>
          <span className={`document-state state-${draftMatchesManager ? 'applied' : 'different'}`}>
            {draftMatchesManager ? 'applied' : 'different'}
          </span>
          <span className="document-identity-divider document-settings-divider" aria-hidden="true" />
        </div>
        <div className="document-actions">
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy}
            title={blockReason ?? 'Open a Deployment as the Draft.'}
            onClick={() => setAction('open')}
          >Open</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !workspace?.draft.document || !workspace.saved.path || !workspace.draft.differsFromSaved}
            title={blockReason ?? (!workspace?.draft.document
              ? 'No Deployment Draft is loaded.'
              : !workspace.saved.path
                ? 'The Draft has no saved file yet; use Save as.'
                : !workspace.draft.differsFromSaved
                  ? 'Draft already matches Saved.'
                  : 'Save the current Deployment Draft.')}
            onClick={() => { void controller.save(); }}
          >Save</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !workspace?.draft.document}
            title={blockReason ?? (!workspace?.draft.document
              ? 'No Deployment Draft is loaded.'
              : 'Save the current Deployment Draft to another file.')}
            onClick={() => setAction('save_as')}
          >Save as</button>
          <button
            type="button"
            disabled={!controller.mutationAvailable || controller.busy || !hasUnappliedChanges}
            title={blockReason ?? (hasUnappliedChanges
              ? 'Discard Draft changes and return to Manager Applied.'
              : 'Draft already matches Manager Applied.')}
            onClick={() => setAction('discard')}
          >Discard</button>
          <button
            type="button"
            disabled={
              !controller.mutationAvailable
              || controller.busy
              || workspace?.savedScenarioCandidates.length === 0
              || workspace?.saved.deploymentRevision
                === workspace?.managerApplied.deploymentRevision
            }
            title={blockReason ?? (workspace?.savedScenarioCandidates.length === 0
              ? 'No saved Deployment is available to revert to.'
              : workspace?.saved.deploymentRevision
                  === workspace?.managerApplied.deploymentRevision
                ? 'Applied already matches Saved.'
                : 'Replace Draft and Manager Applied with the Saved Deployment.')}
            onClick={() => setAction('revert')}
          >Revert</button>
          <span className="header-action-divider" aria-hidden="true" />
          <button
            type="button"
            className="primary-action"
            disabled={
              !controller.mutationAvailable
              || controller.busy
              || !workspace?.draft.document
              || draftMatchesManager
            }
            title={blockReason ?? (!workspace?.draft.document
              ? 'No Deployment Draft is loaded.'
              : draftMatchesManager
                ? 'Draft already matches Manager Applied.'
                : 'Validate and apply this Deployment Draft to runtime.')}
            onClick={() => { void controller.applyDraft(); }}
          >{controller.applying ? 'Applying Deployment' : 'Apply'}</button>
        </div>
      </div>

      {action !== null && (
        <OverlayPanel
          open
          title={action === 'open' ? 'Open Deployment as Draft' : action === 'save_as' ? 'Save Deployment as' : action === 'revert' ? 'Revert to saved Deployment?' : 'Discard Deployment changes?'}
          titleId="deployment-document-dialog-title"
          onClose={close}
          compact={action === 'discard' || action === 'revert'}
        >
          {action === 'open' && (
            <div className="document-dialog">
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
            <div className="dialog-copy">
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
              >Save as</button>
            </div>
          )}
          {action === 'discard' && (
            <div className="dialog-copy compact-confirmation">
              <p>Discard the unsaved Deployment Draft changes?</p>
              <button type="button" className="primary-action" onClick={() => { void complete(controller.discard); }}>Discard changes</button>
            </div>
          )}
          {action === 'revert' && (
            <div className="dialog-copy compact-confirmation">
              <p>Replace Draft and Manager Applied with the Saved Deployment?</p>
              <button type="button" className="primary-action" onClick={() => { void complete(controller.revert); }}>Revert to saved</button>
            </div>
          )}
        </OverlayPanel>
      )}
      {pendingConfirmation !== null && (
        <OverlayPanel
          open
          title="Apply disruptive Deployment change?"
          titleId="deployment-apply-confirmation-title"
          onClose={controller.cancelApplyConfirmation}
          compact
        >
          <div className="dialog-copy compact-confirmation">
            <p>
              Applying this exact Deployment Draft will reconfigure active runtime
              components. Running operations remain protected by Manager admission.
            </p>
            {pendingConfirmation.result.candidatePreview !== null && (
              <DeploymentApplyImpactSummary
                impact={pendingConfirmation.result.candidatePreview.impact}
              />
            )}
            <p>
              <strong>{pendingConfirmation.result.preparedActivation.deploymentArtifactId}</strong>
              {' · '}
              <code>{pendingConfirmation.result.preparedActivation.deploymentRevision}</code>
            </p>
            <button
              type="button"
              className="primary-action"
              disabled={!controller.mutationAvailable || controller.busy}
              title={blockReason ?? 'Apply the validated Deployment to runtime.'}
              onClick={() => { void controller.confirmApply(); }}
            >{controller.applying ? 'Applying Deployment' : 'Confirm apply'}</button>
          </div>
        </OverlayPanel>
      )}
    </>
  );
}
