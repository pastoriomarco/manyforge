import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';

/** The Deployment-tab header facts for the shared document controls. */
export function deploymentDocumentHeader(controller: DeploymentWorkspaceController) {
  const workspace = controller.workspace;
  return {
    label: 'Deployment document controls',
    name: workspace?.draft.deploymentArtifactId ?? 'No Deployment',
    title: workspace?.saved.path ?? workspace?.draft.deploymentRevision ?? undefined,
  };
}
