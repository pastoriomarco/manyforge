import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { api } from '../api/client';
import { reactPropsHost } from '../test/reactPropsHost';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';

const arm = {
  component_id: 'arm',
  component_profile_id: 'robot_native_v1',
  model_source: { urdf: { project_path: 'sources/robot.urdf' }, package_roots: {} },
  root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
  target_settings: { arm: { maximum_tcp_linear_speed_mps: 0.2 } },
  configuration: { mode: 'simulated', execution_tick_hz: 100 },
};

const armWithGripperTargets = (targetIds: string[]) => ({
  ...arm,
  configuration: {
    ...arm.configuration,
    gripper_target_configurations: targetIds.map((command_target_id) => ({
      controllable_component_id: 'arm',
      command_target_id,
      description: `Test ${command_target_id}`,
      actuation_kind: 'position_commanded',
      open_state_ref: 'open',
      closed_state_ref: 'closed',
      command_duration_s: 0.8,
      touch_links: ['tool_base_link'],
      mount_frame: 'tool0',
      attach_frame: 'tool0',
      tcp_frame: 'tool0',
      auxiliary_frames: [],
    })),
  },
});

const device = {
  component_id: 'station_io',
  component_profile_id: 'io_device_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'io_provider',
    runtime_configuration: {
      io_provider_devices: [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 12 }],
      io_signals: [{
        controllable_component_id: 'station_io', command_target_id: 'io', resource_role_id: 'part_present',
        signal_name: 'part_present', source_id: 'station_mock', value_type: 'bool', direction: 'input',
        update_mode: 'change_only', transition_capability: 'sampled_only',
        freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' },
      }],
    },
  },
};

const poseSource = {
  component_id: 'isaac_scene_objects',
  component_profile_id: 'pose_source_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'pose_source',
    runtime_configuration: {
      stream_budget_ns: 1000000000,
      simulation: { poll_period_ms: 250, source_coordinate_frame_id: 'universe', target_frame_id: 'universe' },
      robot_overlay: {
        robot_component_id: 'arm',
        joint_state: { topic: '/isaac_joint_states', required_joint_names: ['joint_1'] },
      },
    },
  },
};

const aiModel = {
  component_id: 'assistant_model',
  component_profile_id: 'ai_model_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'ai_model',
    runtime_configuration: { base_url: 'http://127.0.0.1:8000/v1', model: 'gemma4-12b-it-gguf', api_key_env: null },
  },
};

const links = [
  { scene_resource_id: 'cheese', source_id: 'isaac_scene_objects',
    source_key: { simulation_entity: { entity: '/cheese' } }, apply_policy: 'continuous_unattached' },
  { scene_resource_id: 'gone', source_id: 'isaac_scene_objects',
    source_key: { simulation_entity: { entity: '/gone' } }, apply_policy: 'observe_only' },
];

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

async function mount(
  components: JsonValue[],
  programSceneIds: string[] | null = null,
  extra: Record<string, JsonValue> = {},
) {
  const controller = {
    workspace: {
      draft: {
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          deployment_artifact_id: 'cell-a',
          name: 'Cell A',
          components,
          attachments: [],
          policies: { scene_pose_sources: links },
          ...extra,
        },
      },
    },
    mutationAvailable: true,
    busy: false,
    mutate: vi.fn().mockResolvedValue({}),
    updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<DeploymentFocusedEditor controller={controller}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]}
    programSceneIds={programSceneIds} />));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    controller, control, all,
    type: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    commit: async (label: string, value: string) => {
      await act(() => (control(label).onFocus as (event: unknown) => void)({}));
      await act(() => control(label).onChange?.({ target: { value } }));
      await act(() => (control(label).onBlur as (event: unknown) => void)({}));
    },
    unmount: () => act(() => root.unmount()),
  };
}

describe('Deployment pose sources', () => {
  it('keeps a pose source out of the robot sections and edits its own configuration', async () => {
    const mounted = await mount([arm, poseSource]);
    try {
      // The execution host section lists the arm only.
      const units = mounted.all((props) => props.className === 'deployment-runtime-unit')
        .map((props) => ((props.children as Array<{ props?: { children?: Array<{ props?: { children?: unknown } }> } }>)[0]
          ?.props?.children?.[0]?.props?.children));
      expect(units).toEqual(['arm']);
      expect(mounted.control('isaac_scene_objects pose source')).toBeDefined();
      const path = ['components', 1, 'configuration', 'runtime_configuration'];
      const configuration = poseSource.configuration.runtime_configuration;
      await mounted.commit('isaac_scene_objects Stream budget (s)', '2.5');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set',
        { ...configuration, stream_budget_ns: 2500000000 });
      await mounted.commit('isaac_scene_objects Poll period (ms)', '100');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set',
        { ...configuration, simulation: { ...configuration.simulation, poll_period_ms: 100 } });
      await mounted.commit('isaac_scene_objects Joint names', 'a, b');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set', { ...configuration, robot_overlay: {
        robot_component_id: 'arm', joint_state: { topic: '/isaac_joint_states', required_joint_names: ['a', 'b'] },
      } });
      // A new block is written only once its required fields are set.
      vi.mocked(mounted.controller.mutate).mockClear();
      await act(() => mounted.control('isaac_scene_objects 3D detections').onChange?.({ target: { checked: true } }));
      await mounted.commit('isaac_scene_objects Detection topic', '/output');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.commit('isaac_scene_objects Detection target frame', 'link_base');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(path, 'set', { ...configuration,
        detection3d: { topic: '/output', target_frame_id: 'link_base', fallback_frame_id: '' } });
      // The linked objects are listed read-only.
      const items = mounted.all((props) => Array.isArray(props.children)).flatMap((props) => props.children as unknown[]);
      expect(items).toContain('cheese');
      expect(items).toContain('/cheese');
    } finally { await mounted.unmount(); }
  });

  it('stores a per-robot reference only after every explicit field is complete', async () => {
    const legacyPoseSource = {
      ...poseSource,
      configuration: {
        ...poseSource.configuration,
        runtime_configuration: {
          ...poseSource.configuration.runtime_configuration,
          simulation: {
            ...poseSource.configuration.runtime_configuration.simulation,
            relative_reference: {
              entity: '/World/legacy-tool',
              link: 'legacy_base_link',
              source_header_frame_id: 'legacy_base_link',
            },
          },
        },
      },
    };
    const mounted = await mount([armWithGripperTargets(['gripper']), legacyPoseSource]);
    try {
      const notice = mounted.all((props) => props.role === 'status')
        .map((props) => props.children)
        .find((children) => String(children).includes('legacy single reference'));
      expect(notice).toContain(
        'legacy single reference: migrated at Apply only if the cell has exactly one gripper target; otherwise set per-target references',
      );
      const add = mounted.control('isaac_scene_objects add gripper target reference');
      expect(add).toBeDefined();
      await act(() => add.onClick?.());
      expect(mounted.controller.mutate).not.toHaveBeenCalled();

      await mounted.type('isaac_scene_objects relative reference 1 robot component', 'arm');
      await mounted.type('isaac_scene_objects relative reference 1 gripper command target', 'gripper');
      await mounted.commit('isaac_scene_objects relative reference 1 Measured entity', '/World/robot/tool');
      await mounted.commit('isaac_scene_objects relative reference 1 Measured body link', 'tool_base_link');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();

      const expectedSimulation: Record<string, JsonValue> = {
        ...legacyPoseSource.configuration.runtime_configuration.simulation,
        relative_references: [{
          controllable_component_id: 'arm',
          command_target_id: 'gripper',
          entity: '/World/robot/tool',
          link: 'tool_base_link',
          source_header_frame_id: 'tool_base_link',
        }],
      };
      delete expectedSimulation.relative_reference;
      await mounted.commit('isaac_scene_objects relative reference 1 Source header frame', 'tool_base_link');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration'], 'set',
        {
          ...legacyPoseSource.configuration.runtime_configuration,
          simulation: expectedSimulation,
        },
      );
      const saved = vi.mocked(mounted.controller.mutate).mock.calls[0]?.[2] as {
        simulation: Record<string, unknown>;
      };
      expect(saved.simulation).not.toHaveProperty('relative_reference');
    } finally { await mounted.unmount(); }
  });

  it('allows distinct gripper targets for the same robot component', async () => {
    const firstReference = {
      controllable_component_id: 'arm',
      command_target_id: 'gripper',
      entity: '/World/robot/tool',
      link: 'tool_base_link',
      source_header_frame_id: 'tool_base_link',
    };
    const configured = {
      ...poseSource,
      configuration: {
        ...poseSource.configuration,
        runtime_configuration: {
          ...poseSource.configuration.runtime_configuration,
          simulation: {
            ...poseSource.configuration.runtime_configuration.simulation,
            relative_references: [firstReference],
          },
        },
      },
    };
    const mounted = await mount([armWithGripperTargets(['gripper', 'parallel_jaw']), configured]);
    try {
      await act(() => mounted.control('isaac_scene_objects add gripper target reference').onClick?.());
      await mounted.type('isaac_scene_objects relative reference 2 robot component', 'arm');
      await mounted.type('isaac_scene_objects relative reference 2 gripper command target', 'parallel_jaw');
      await mounted.commit('isaac_scene_objects relative reference 2 Measured entity', '/World/robot/parallel_jaw');
      await mounted.commit('isaac_scene_objects relative reference 2 Measured body link', 'parallel_jaw_base');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.commit('isaac_scene_objects relative reference 2 Source header frame', 'parallel_jaw_base');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration'], 'set',
        {
          ...configured.configuration.runtime_configuration,
          simulation: {
            ...configured.configuration.runtime_configuration.simulation,
            relative_references: [firstReference, {
              controllable_component_id: 'arm',
              command_target_id: 'parallel_jaw',
              entity: '/World/robot/parallel_jaw',
              link: 'parallel_jaw_base',
              source_header_frame_id: 'parallel_jaw_base',
            }],
          },
        },
      );
    } finally { await mounted.unmount(); }
  });

  it('removes the relative_references property when the last robot row is removed', async () => {
    const configured = {
      ...poseSource,
      configuration: {
        ...poseSource.configuration,
        runtime_configuration: {
          ...poseSource.configuration.runtime_configuration,
          simulation: {
            ...poseSource.configuration.runtime_configuration.simulation,
            relative_references: [{
              controllable_component_id: 'arm',
              command_target_id: 'gripper',
              entity: '/World/robot/tool',
              link: 'tool_base_link',
              source_header_frame_id: 'tool_base_link',
            }],
          },
        },
      },
    };
    const mounted = await mount([armWithGripperTargets(['gripper']), configured]);
    try {
      const remove = mounted.control('Remove isaac_scene_objects relative reference 1');
      await act(() => remove.onClick?.());
      const expectedSimulation = {
        ...poseSource.configuration.runtime_configuration.simulation,
      };
      expect(expectedSimulation).not.toHaveProperty('relative_references');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration'], 'set',
        { ...poseSource.configuration.runtime_configuration, simulation: expectedSimulation },
      );
    } finally { await mounted.unmount(); }
  });

  it('lists links to objects missing from the Program and removes one', async () => {
    let mounted = await mount([arm, poseSource]);
    try {
      expect(() => mounted.control('Remove pose link of gone')).toThrow();
    } finally { await mounted.unmount(); }
    mounted = await mount([arm, poseSource], ['cheese']);
    try {
      expect(() => mounted.control('Remove pose link of cheese')).toThrow();
      await act(() => mounted.control('Remove pose link of gone').onClick?.());
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set',
        { scene_pose_sources: [links[0]] });
    } finally { await mounted.unmount(); }
  });

  it('preserves the pose source unchanged when other components are edited', async () => {
    const original = structuredClone(poseSource);
    const mounted = await mount([arm, device, poseSource]);
    try {
      await mounted.type('station_io lifecycle command time limit', '45');
      await act(() => (mounted.control('station_io lifecycle command time limit').onBlur as () => void)());
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration', 'io_provider_devices'], 'set',
        [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 45 }],
      );
      const document = mounted.controller.workspace!.draft.document as unknown as { components: JsonValue[] };
      expect(document.components[2]).toEqual(original);
    } finally { await mounted.unmount(); }
  });

  it('keeps an AI model out of the robot sections and edits its endpoint fields', async () => {
    const mounted = await mount([arm, aiModel]);
    try {
      const units = mounted.all((props) => props.className === 'deployment-runtime-unit')
        .map((props) => ((props.children as Array<{ props?: { children?: Array<{ props?: { children?: unknown } }> } }>)[0]
          ?.props?.children?.[0]?.props?.children));
      expect(units).toEqual(['arm']);
      expect(mounted.control('assistant_model AI model')).toBeDefined();
      const path = ['components', 1, 'configuration', 'runtime_configuration'];
      const configuration = aiModel.configuration.runtime_configuration;
      await mounted.commit('assistant_model Token environment variable', 'MANYFORGE_AI_TOKEN');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set',
        { ...configuration, api_key_env: 'MANYFORGE_AI_TOKEN' });
      vi.mocked(mounted.controller.mutate).mockClear();
      await mounted.commit('assistant_model Token environment variable', 'MY_TOKEN');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.commit('assistant_model Base URL', 'not a url');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.commit('assistant_model Model', 'qwen');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set', { ...configuration, model: 'qwen' });
    } finally { await mounted.unmount(); }
  });

  it('adds a second AI model component with a unique id', async () => {
    const mounted = await mount([arm, aiModel]);
    try {
      const add = mounted.all((props) => props.children === 'Add AI model')[0];
      await act(() => (add.onClick as () => void)());
      await mounted.type('New AI model Model', 'qwen');
      await mounted.type('New AI model Token environment variable', 'MANYFORGE_AI_QWEN');
      const create = mounted.all((props) => props.children === 'Create AI model')[0];
      await act(() => (create.onClick as () => void)());
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['components'], 'append', {
        component_id: 'ai_model_3',
        component_profile_id: 'ai_model_v0',
        target_settings: {},
        configuration: {
          runtime_kind: 'ai_model',
          runtime_configuration: {
            base_url: 'http://127.0.0.1:8000/v1', model: 'qwen', api_key_env: 'MANYFORGE_AI_QWEN',
          },
        },
      });
    } finally { await mounted.unmount(); }
  });

  it('maps each runtime model alias of the assistant profile to an AI model component', async () => {
    // The route lists runtime aliases only: the chat alias is bound by the assistant component.
    vi.spyOn(api.deploymentWorkspace, 'assistantProfiles').mockResolvedValue({
      semantic_shape_selection: { chat: true, modelEndpointAliases: ['pose_model', 'selection_model'] },
    });
    const selection = { ...aiModel, component_id: 'selection_model' };
    const mounted = await mount([arm, aiModel, selection], null, {
      assistant_profile_id: 'semantic_shape_selection',
      assistant_model_components: { selection_model: 'selection_model' },
    });
    try {
      await act(async () => { await Promise.resolve(); });
      expect(mounted.control('Model alias selection_model').value).toBe('selection_model');
      expect(mounted.control('Model alias pose_model').value).toBe('');
      expect(mounted.all((props) => props['aria-label'] === 'Model alias assistant_model')).toHaveLength(0);
      expect(mounted.all((props) => props.children === 'Choose the AI model for this alias.')).toHaveLength(1);
      await act(() => mounted.control('Model alias pose_model').onChange?.({
        target: { value: 'assistant_model' } }));
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['assistant_model_components'], 'set', {
        selection_model: 'selection_model', pose_model: 'assistant_model',
      });
    } finally { await mounted.unmount(); }
  });
});
