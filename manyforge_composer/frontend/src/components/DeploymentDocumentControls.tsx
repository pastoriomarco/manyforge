import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ManagerDeploymentImpact } from '../types/api';

const DISPOSITION_LABEL = {
  add: 'Added',
  remove: 'Removed',
  replace: 'Replaced',
} as const;

/** The Manager's authoritative impact, presented as-is; never re-derived. */
export function DeploymentApplyImpactSummary({
  impact,
}: {
  impact: ManagerDeploymentImpact;
}) {
  const changed = impact.components.filter((row) => row.disposition !== 'retain');
  const retained = impact.components.filter((row) => row.disposition === 'retain').length;
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
          <li>{retained} component{retained === 1 ? '' : 's'} keep their state unchanged</li>
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

/** The Deployment-tab header facts for the shared document controls. */
export function deploymentDocumentHeader(controller: DeploymentWorkspaceController) {
  const workspace = controller.workspace;
  return {
    label: 'Deployment document controls',
    name: workspace?.draft.deploymentArtifactId ?? 'No Deployment',
    title: workspace?.saved.path ?? workspace?.draft.deploymentRevision ?? undefined,
  };
}
