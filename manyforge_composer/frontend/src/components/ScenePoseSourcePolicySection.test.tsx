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

it('keeps the deadband on the binding, in mm and degrees, and omits zero', async () => {
  const binding = { source_id: 'camera', scene_resource_id: 'part', apply_policy: 'continuous' };
  const mutate = vi.fn().mockResolvedValue({});
  const controller = {
    workspace: { draft: { document: { policies: { scene_pose_sources: [binding] } } } },
    mutationAvailable: true, busy: false, loading: false, mutate,
  } as unknown as DeploymentWorkspaceController;
  const { host, find } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ScenePoseSourcePolicySection controller={controller} sources={[]} />));
  const field = (unit: string) => find((props) => props['aria-label'] === `Pose deadband ${unit} for part from camera`);
  const blur = (unit: string, value: string) => act(() => (
    field(unit).onBlur as (event: { target: { value: string } }) => void)({ target: { value } }));
  expect(field('mm').defaultValue).toBe(0);
  await blur('mm', '0.1');
  const rows = (mutate.mock.calls[0][2] as { scene_pose_sources: Record<string, unknown>[] }).scene_pose_sources;
  expect(rows).toHaveLength(1);
  expect(rows[0].deadband_translation_m as number).toBeCloseTo(0.0001);
  expect(rows[0]).not.toHaveProperty('deadband_rotation_rad');
  await blur('°', '0');
  expect(mutate).toHaveBeenLastCalledWith(['policies'], 'set', { scene_pose_sources: [binding] });
  await act(() => root.unmount());
});
