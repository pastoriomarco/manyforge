import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { SceneRuntimePoseSource } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ScenePoseSourcePolicySection } from './ScenePoseSourcePolicySection';

afterEach(() => vi.unstubAllGlobals());

it('offers the six supported policies and edits the Deployment through its owner', async () => {
  const controller = {
    workspace: { draft: { document: { policies: { interrupt_rules: [] } } } },
    mutationAvailable: true, busy: false, loading: false,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
  const source = { eligible: true, source_id: 'camera', object_id: 'part', policy_id: 'continuous_unattached' } as SceneRuntimePoseSource;
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ScenePoseSourcePolicySection controller={controller} sources={[source]} />));
  const choices = all((props) => typeof props.value === 'string' && props.children !== undefined)
    .map((props) => props.value);
  for (const policy of ['observe_only', 'continuous_unattached', 'continuous',
    'continuous_relative_to_attached', 'before_planning', 'between_nodes'])
    expect(choices).toContain(policy);
  expect(choices).not.toContain('when_idle');
  await act(() => find((props) => props['aria-label'] === 'Pose policy for part from camera')
    .onChange?.({ target: { value: 'continuous' } }));
  expect(controller.mutate).toHaveBeenCalledWith(['policies'], 'set', {
    interrupt_rules: [], scene_pose_sources: [{ source_id: 'camera', scene_resource_id: 'part', apply_policy: 'continuous' }],
  });
  await act(() => root.unmount());
});
