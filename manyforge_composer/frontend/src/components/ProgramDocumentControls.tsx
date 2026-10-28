import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';

/** The Program-tab header facts for the shared document controls. */
export function programDocumentHeader(controller: ProgramWorkspaceController) {
  const workspace = controller.state.workspace;
  const authoring = workspace?.draft.authoring ?? null;
  const name = authoring?.name ?? 'No Program';
  const diagnostics = controller.scenarioSourceDiagnostics ?? [];
  const edited = diagnostics.some((diagnostic) => diagnostic.code === 'edited_outside_manyforge');
  return {
    label: 'Program document controls',
    name,
    title: workspace?.saved.path ?? authoring?.description ?? name,
    notice: diagnostics.length === 0 ? undefined : (
      <div className="notice severity-warning" role="status">
        <strong>
          {edited
            ? 'Scenario files were edited outside ManyForge.'
            : 'Referenced sources changed since the last Save.'}
        </strong>
        <ul>
          {diagnostics.map((diagnostic) => (
            <li key={diagnostic.sourceId}>{diagnostic.sourceId}: {diagnostic.message}</li>
          ))}
        </ul>
        <span>Apply will capture and validate the current source contents.</span>
      </div>
    ),
  };
}
