import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ObjectPoseSourceCard } from './ObjectPoseSourceCard';

const isaac = {
  component_id: 'isaac_scene_objects',
  component_profile_id: 'pose_source_v0',
  target_settings: {},
  configuration: { runtime_kind: 'pose_source', runtime_configuration: {
    stream_budget_ns: 1000000000,
    simulation: { poll_period_ms: 250, source_coordinate_frame_id: 'universe', target_frame_id: 'universe' },
  } },
};
const camera = {
  component_id: 'camera',
  component_profile_id: 'pose_source_v0',
  target_settings: {},
  configuration: { runtime_kind: 'pose_source', runtime_configuration: {
    stream_budget_ns: 5000000000,
    detection3d: { topic: '/output', target_frame_id: 'link_base', fallback_frame_id: 'link_base' },
  } },
};
const link = {
  scene_resource_id: 'cheese', source_id: 'isaac_scene_objects',
  source_key: { simulation_entity: {
    entity: '/cheese', source_header_frame_id: 'cheese_frame', local_position_xyz: [0, 0, 0.1] } },
  apply_policy: 'continuous', deadband_translation_m: 0.002,
};

afterEach(() => vi.unstubAllGlobals());

async function mount(policies: Record<string, JsonValue>, sceneResourceId = 'cheese') {
  const controller = {
    workspace: { draft: { document: {
      api_version: 'manyforge.deployment_intent.v2', components: [isaac, camera], policies,
    } } },
    mutationAvailable: true, busy: false, loading: false,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ObjectPoseSourceCard controller={controller} sceneResourceId={sceneResourceId} />));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    controller, control, all,
    select: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    type: async (label: string, value: string) => {
      await act(() => (control(label).onFocus as (event: unknown) => void)({}));
      await act(() => control(label).onChange?.({ target: { value } }));
      await act(() => (control(label).onBlur as (event: unknown) => void)({}));
    },
    unmount: () => act(() => root.unmount()),
  };
}

describe('Object pose source card', () => {
  it('links a detection source at once and stages a simulation source until its entity and header are set', async () => {
    const mounted = await mount({ interrupt_rules: [] }, 'part');
    try {
      expect(mounted.control('Pose source').value).toBe('');
      await mounted.select('Pose source', 'camera');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        interrupt_rules: [],
        scene_pose_sources: [{
          scene_resource_id: 'part', source_id: 'camera', apply_policy: 'observe_only',
          source_key: { detection3d: {
            class_id: '', detection_id: '', confidence_min: 0, max_position_variance_m2: 0.0025, selection: 'first',
          } },
        }],
      });
      vi.mocked(mounted.controller.mutate).mockClear();
      await mounted.select('Pose source', 'isaac_scene_objects');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.type('Pose source entity', ' /World/part ');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', {
        interrupt_rules: [],
        scene_pose_sources: [{
          scene_resource_id: 'part', source_id: 'isaac_scene_objects', apply_policy: 'observe_only',
          source_key: { simulation_entity: { entity: '/World/part', source_header_frame_id: 'part' } },
        }],
      });
    } finally { await mounted.unmount(); }
  });

  it('edits policy and deadband on the one row, keeps the key offsets, and None removes the row', async () => {
    const other = { ...link, scene_resource_id: 'box', source_key: { simulation_entity: { entity: '/box' } } };
    const mounted = await mount({ scene_pose_sources: [other, link] });
    try {
      await mounted.select('Pose apply policy', 'continuous_relative_to_attached');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        scene_pose_sources: [other, { ...link, apply_policy: 'continuous_relative_to_attached' }],
      });
      expect(mounted.control('Pose deadband mm').value).toBe('2');
      await mounted.type('Pose deadband mm', '');
      const { deadband_translation_m: _removed, ...exact } = link;
      void _removed;
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        scene_pose_sources: [other, exact],
      });
      await mounted.type('Pose deadband °', '1');
      const rows = (vi.mocked(mounted.controller.mutate).mock.lastCall![2] as {
        scene_pose_sources: Record<string, number>[];
      }).scene_pose_sources;
      expect(rows[1].deadband_rotation_rad).toBeCloseTo(Math.PI / 180);
      await mounted.type('Pose source entity', '/cheese2');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        scene_pose_sources: [other, { ...link, source_key: {
          simulation_entity: { entity: '/cheese2', source_header_frame_id: 'cheese_frame',
            local_position_xyz: [0, 0, 0.1] } } }],
      });
      await mounted.type('Pose source header frame', 'other');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        scene_pose_sources: [other, { ...link, source_key: {
          simulation_entity: { entity: '/cheese', source_header_frame_id: 'other',
            local_position_xyz: [0, 0, 0.1] } } }],
      });
      await mounted.select('Pose source', '');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', {
        scene_pose_sources: [other],
      });
    } finally { await mounted.unmount(); }
  });

  it('shows the lifetime read-only: the stream override, else the source budget', async () => {
    const text = (all: ReturnType<typeof reactPropsHost>['all']) => all((props) => Array.isArray(props.children))
      .flatMap((props) => props.children as unknown[]).filter((child) => typeof child === 'string');
    let mounted = await mount({ scene_pose_sources: [link], component_streams: [{
      controllable_component_id: 'isaac_scene_objects', stream_id: 'poses/cheese', budget_ns: 2000000000,
    }] });
    try { expect(text(mounted.all)).toContain('2 s'); } finally { await mounted.unmount(); }
    mounted = await mount({ scene_pose_sources: [link] });
    try { expect(text(mounted.all)).toContain('1 s'); } finally { await mounted.unmount(); }
  });
});
