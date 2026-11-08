import type { DeploymentWorkspaceResponse } from '../types/api';

function shortIdentity(value: string): string {
  return value.length > 32 ? `${value.slice(0, 28)}…` : value;
}

export function DeploymentBindingProjection({
  workspace,
}: {
  workspace: DeploymentWorkspaceResponse;
}) {
  const { binding, scenarioCandidates } = workspace;
  return (
    <details className="dcard">
      <summary>
        <span>Program Binding</span>
        <span className="dcard-meta">{binding === null ? 'no exact match' : `${binding.component_bindings.length} components`}</span>
      </summary>
      <div className="deployment-binding-body">
        {binding === null ? (
          <p className="panel-intro">
            This Deployment Draft does not match the current retained Program Binding.
            Save and Manager Apply remain independent.
          </p>
        ) : (
          <>
            <dl className="deployment-binding-identities">
              <div><dt>Program</dt><dd>{binding.program_id}</dd></div>
              <div><dt>Program revision</dt><dd>{shortIdentity(binding.program_revision)}</dd></div>
              <div><dt>Binding</dt><dd>{shortIdentity(binding.program_binding_id)}</dd></div>
              <div><dt>Retained bundle</dt><dd>{shortIdentity(binding.retained_bundle_id)}</dd></div>
              <div><dt>Graph</dt><dd>{shortIdentity(binding.graph_revision)}</dd></div>
            </dl>
            <div className="deployment-binding-groups">
              <section>
                <h4>Components</h4>
                {binding.component_bindings.map((row) => (
                  <div className="deployment-binding-row" key={row.component_id}>
                    <strong>{row.component_id}</strong>
                    <small>{row.component_type_id}</small>
                  </div>
                ))}
              </section>
              <section>
                <h4>Targets</h4>
                {binding.target_bindings.map((row) => (
                  <div className="deployment-binding-row"
                    key={`${row.target.component_id}/${row.target.command_target_id}`}>
                    <strong>{row.target.component_id} · {row.target.command_target_id}</strong>
                    <small>
                      {row.frame_references.length} frames · {row.signal_references.length} signals · {row.capability_bindings.length} capabilities
                    </small>
                  </div>
                ))}
              </section>
            </div>
          </>
        )}
        <section className="deployment-scenario-candidates">
          <h4>Named scenario matches</h4>
          {scenarioCandidates.length === 0 ? (
            <p className="panel-intro">No named Scenario matches this exact Deployment Draft.</p>
          ) : scenarioCandidates.map((candidate) => (
            <div className="deployment-binding-row" key={candidate.scenarioId}>
              <strong>{candidate.scenarioId}</strong>
              <span>{candidate.description ?? 'No description'}</span>
              <small>
                Program {candidate.programId} · {shortIdentity(candidate.programRevision)}
                {' · '}Binding {shortIdentity(candidate.programBindingId)}
              </small>
            </div>
          ))}
        </section>
      </div>
    </details>
  );
}
