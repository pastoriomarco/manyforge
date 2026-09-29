import { useState, type ReactNode } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ManagerDeploymentImpact } from '../types/api';
import { deploymentMatchesApplied, deploymentUnapplied, deploymentActionUnavailableReason } from '../state/documentStates';
import { OverlayPanel } from './OverlayPanel';

type Action = 'discard' | 'revert';

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
  scenarioCommands,
}: {
  controller: DeploymentWorkspaceController;
  /** The workspace-independent Scenario New/Save/Save as/Load commands. */
  scenarioCommands?: ReactNode;
}) {
  const [action, setAction] = useState<Action | null>(null);
  const workspace = controller.workspace;
  // Manager Applied is the publication identity.  Runtime convergence is a
  // separate readiness concern and must not make the document controls
  // oscillate while an accepted Apply is still settling.
  const draftMatchesManager = deploymentMatchesApplied(workspace);
  // Discard is a local Draft-to-Applied operation.  A Draft can remain
  // different from its saved baseline after Apply (which is what makes
  // Revert useful), but there is then nothing left for Discard to do.
  const hasUnappliedChanges = deploymentUnapplied(workspace);
  const pendingConfirmation = controller.pendingApplyConfirmation;
  // Why authoring is blocked comes first, so every disabled control explains
  // itself the same way the Program controls do.
  const blockReason = controller.busy
    ? 'Another Deployment operation is in progress.'
    : controller.mutationUnavailableReason;
  const close = () => setAction(null);
  const reasons = {
    apply: deploymentActionUnavailableReason(workspace, 'apply', blockReason),
    revert: deploymentActionUnavailableReason(workspace, 'revert', blockReason),
    discard: deploymentActionUnavailableReason(workspace, 'discard', blockReason),
  };
  const complete = async (request: () => Promise<unknown>) => {
    if (action === null || reasons[action] !== null) return;
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
        </div>
        <div className="document-actions">
          {scenarioCommands && (
            <>
              {scenarioCommands}
              <span className="header-action-divider" aria-hidden="true" />
            </>
          )}
          <button
            type="button"
            disabled={reasons.discard !== null}
            title={blockReason ?? (hasUnappliedChanges
              ? 'Discard Draft changes and return to Manager Applied.'
              : 'Draft already matches Manager Applied.')}
            onClick={() => setAction('discard')}
          >Discard</button>
          <button
            type="button"
            disabled={reasons.revert !== null}
            title={reasons.revert ?? 'Replace Draft and Manager Applied with the Saved Deployment.'}
            onClick={() => setAction('revert')}
          >Revert</button>
          <button
            type="button"
            className="primary-action"
            disabled={reasons.apply !== null}
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
          title={action === 'revert' ? 'Revert to saved Deployment?' : 'Discard Deployment changes?'}
          titleId="deployment-document-dialog-title"
          onClose={close}
          compact
        >
          {reasons[action] !== null && <p role="status">{reasons[action]}</p>}
          {action === 'discard' && (
            <div className="dialog-copy compact-confirmation">
              <p>Discard the unsaved Deployment Draft changes?</p>
              <button type="button" className="primary-action" disabled={reasons.discard !== null} onClick={() => { void complete(controller.discard); }}>Discard changes</button>
            </div>
          )}
          {action === 'revert' && (
            <div className="dialog-copy compact-confirmation">
              <p>Replace Draft and Manager Applied with the Saved Deployment?</p>
              <button type="button" className="primary-action" disabled={reasons.revert !== null} onClick={() => { void complete(controller.revert); }}>Revert to saved</button>
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
            {reasons.apply !== null && <p role="status">{reasons.apply}</p>}
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
              disabled={reasons.apply !== null}
              title={blockReason ?? 'Apply the validated Deployment to runtime.'}
              onClick={() => { void controller.confirmApply(); }}
            >{controller.applying ? 'Applying Deployment' : 'Confirm apply'}</button>
          </div>
        </OverlayPanel>
      )}
    </>
  );
}
