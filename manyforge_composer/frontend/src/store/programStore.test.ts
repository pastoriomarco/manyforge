import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import type {
  CatalogEntry,
  FullDeployment,
  ProgramInfo,
  SceneStateResponse,
} from '../types/api';
import { useCatalogStore } from './catalogStore';
import { useDeploymentStore } from './deploymentStore';
import { useProgramStore } from './programStore';
import { useSceneStateStore } from './sceneStateStore';
import { useViewerSelectionStore } from './viewerSelectionStore';

function catalogEntry(
  id: string,
  kind: string = 'leaf',
  childrenMax: number = 0,
  requiredCapabilities: string[] = [],
): CatalogEntry {
  return {
    id,
    name: id,
    category: 'test',
    description: '',
    stability: 'stable',
    kind,
    childrenMin: kind === 'composite' ? 1 : 0,
    childrenMax,
    parameters: [],
    requiredCapabilities,
    profilesMin: 'v0.0',
  };
}

const CATALOG = [
  catalogEntry('pick_plan_subtree', 'authoring_macro', 0, [
    'planning.motion.pose_goal',
    'planning.motion.cartesian_path',
    'planning.profile.first_feasible',
    'tool.gripper.command',
    'scene.edit.attach_detach',
  ]),
  catalogEntry('sequence', 'composite', -1),
  catalogEntry('plan_pick_sequence'),
  catalogEntry('move_manipulator_action'),
  catalogEntry('command_gripper'),
  catalogEntry('attach_object_to_link'),
];

const PROGRAM_INFO = {
  name: 'pick-test',
  description: '',
  programProfile: 'v0.1',
  robot: {
    robotId: 'robot',
    groupId: 'arm',
    baseFrame: 'base_link',
    tcpFrame: 'tool0',
  },
  sceneCount: 0,
  sceneResources: [],
  parameterCount: 0,
  parameters: [],
  blackboard: { namespaces: [], keys: [] },
  sourcePath: '/workspace/test.program.yaml',
  programSessionGeneration: 1,
  programTreeHash: 'hash',
} satisfies ProgramInfo;

const DEPLOYMENT = {
  programProfile: 'v0.1',
  robotConfig: { robotId: 'robot', groupId: 'arm' },
  grippers: {
    primary: {
      motionTimeS: 0.8,
      srdfPresets: { open: 0.8, closed: 0.1 },
      closedStateRef: 'closed',
      attachFrame: 'tool0',
    },
  },
  adapters: {
    planning: {
      capabilityKeys: [
        'planning.motion.pose_goal',
        'planning.motion.cartesian_path',
        'planning.profile.first_feasible',
        'planning.profile.balanced',
        'planning.profile.quality_bounded',
      ],
    },
    tooling: {
      capabilityKeys: [
        'tool.gripper.command',
        'scene.edit.attach_detach',
      ],
    },
  },
} as unknown as FullDeployment;

const SCENE = {
  objects: [
    {
      object_id: 'part',
      attached: false,
      source: 'sensed',
      persistable: false,
      viewer_frame_visible: true,
    },
  ],
  timestamp_ns: 1,
} satisfies SceneStateResponse;

function byLabel(label: string) {
  return useProgramStore.getState().nodes.find(
    (node) => (node.data as { label?: string }).label === label,
  );
}

describe('Pick Plan Subtree authoring macro', () => {
  beforeEach(() => {
    useCatalogStore.setState({ entries: CATALOG, version: '0.3.0' });
    useDeploymentStore.setState({ deployment: DEPLOYMENT });
    useSceneStateStore.setState({ scene: SCENE });
    useViewerSelectionStore.setState({ selectedId: 'part' });
    useProgramStore.setState({
      programInfo: PROGRAM_INFO,
      nodes: [],
      edges: [],
      blackboardNamespaces: [],
      selectedNodeId: null,
      isDirty: false,
      error: null,
      layoutDirection: 'horizontal',
    });
  });

  afterEach(() => {
    useProgramStore.setState({
      programInfo: null,
      nodes: [],
      edges: [],
      blackboardNamespaces: [],
      selectedNodeId: null,
      isDirty: false,
      error: null,
    });
    useDeploymentStore.setState({ deployment: null });
    useSceneStateStore.setState({ scene: null });
    useViewerSelectionStore.setState({ selectedId: null });
  });

  it('atomically expands to visible portable nodes and one declared namespace', () => {
    useProgramStore.getState().addNode('pick_plan_subtree', { x: 120, y: 80 });

    const state = useProgramStore.getState();
    expect(state.error).toBeNull();
    expect(state.nodes).toHaveLength(8);
    expect(state.edges).toHaveLength(7);
    expect(state.nodes.some(
      (node) => (node.data as { catalogId?: string }).catalogId === 'pick_plan_subtree',
    )).toBe(false);
    expect(state.blackboardNamespaces).toEqual([
      expect.objectContaining({
        id: 'pick_plan',
        contract: 'pick_plan_v1',
        prefix: 'planning/pick_plan',
      }),
    ]);

    const planner = byLabel('plan_pick_sequence');
    const approach = byLabel('move_to_approach');
    const pick = byLabel('move_to_pick');
    const close = byLabel('close_gripper');
    const attach = byLabel('attach_object');
    const retreat = byLabel('move_retreat');
    expect(planner?.data.params).toMatchObject({
      selected_object_id: 'part',
      output_key_prefix: { namespace_ref: 'pick_plan' },
      approach_move_node_name: 'move_to_approach',
      pick_move_node_name: 'move_to_pick',
      retreat_move_node_name: 'move_retreat',
    });
    expect(approach?.data.params).toMatchObject({
      motion_type: 'pose',
      target_mode: 'absolute',
      planning_profile: 'first_feasible',
      base_frame: 'base_link',
      tcp_frame: 'tool0',
    });
    expect(pick?.data.params).toMatchObject({
      motion_type: 'cartesian',
      target_mode: 'absolute',
      cartesian_path: {
        fallback_policy: 'fail_closed',
        waypoints: [{ position_m: [0, 0, 0] }, { position_m: [0, 0, 0] }],
      },
    });
    expect(retreat?.data.params).toMatchObject({ motion_type: 'cartesian' });
    expect(close?.data.params).toMatchObject({
      gripper_id: 'primary',
      position: 0.1,
      duration_s: 0.8,
    });
    expect(attach?.data.params).toMatchObject({
      object_id: 'part',
      link_frame: 'tool0',
      gripper_id: 'primary',
    });
  });

  it('uses one deterministic suffix across repeated insertions', () => {
    const store = useProgramStore.getState();
    store.addNode('pick_plan_subtree', { x: 100, y: 100 });
    useProgramStore.getState().addNode('pick_plan_subtree', { x: 500, y: 100 });

    expect(byLabel('pick_plan_master_sequence_2')).toBeDefined();
    expect(byLabel('move_to_pick_2')).toBeDefined();
    expect(useProgramStore.getState().blackboardNamespaces).toEqual([
      expect.objectContaining({ id: 'pick_plan' }),
      expect.objectContaining({ id: 'pick_plan_2', prefix: 'planning/pick_plan_2' }),
    ]);
    expect(byLabel('plan_pick_sequence_2')?.data.params).toMatchObject({
      output_key_prefix: { namespace_ref: 'pick_plan_2' },
      pick_move_node_name: 'move_to_pick_2',
    });
  });

  it('fails without partial graph changes when gripper context is ambiguous', () => {
    useDeploymentStore.setState({
      deployment: { ...DEPLOYMENT, grippers: {} },
    });

    useProgramStore.getState().addNode('pick_plan_subtree', { x: 100, y: 100 });

    const state = useProgramStore.getState();
    expect(state.error).toContain('exactly one active deployment gripper');
    expect(state.nodes).toEqual([]);
    expect(state.edges).toEqual([]);
    expect(state.blackboardNamespaces).toEqual([]);
    expect(state.isDirty).toBe(false);
  });

  it('fails atomically when the deployment lacks a required portable capability', () => {
    useDeploymentStore.setState({
      deployment: {
        ...DEPLOYMENT,
        adapters: {
          ...DEPLOYMENT.adapters,
          planning: {
            ...DEPLOYMENT.adapters.planning,
            capabilityKeys: [
              'planning.motion.pose_goal',
              'planning.profile.first_feasible',
            ],
          },
        },
      },
    });

    useProgramStore.getState().addNode('pick_plan_subtree', { x: 100, y: 100 });

    const state = useProgramStore.getState();
    expect(state.error).toContain('planning.motion.cartesian_path');
    expect(state.nodes).toEqual([]);
    expect(state.edges).toEqual([]);
    expect(state.blackboardNamespaces).toEqual([]);
    expect(state.isDirty).toBe(false);
  });

  it('does not author a catalog default whose capability is unavailable', () => {
    const profiledMotionEntry = {
      ...CATALOG.find((entry) => entry.id === 'move_manipulator_action')!,
      parameters: [
        {
          name: 'planning_profile',
          type: 'enum',
          required: false,
          default: 'first_feasible',
          enumValues: ['first_feasible', 'balanced'],
          enumOptions: [
            {
              value: 'first_feasible',
              description: '',
              profilesMin: 'v0.0',
              requiredCapabilities: ['planning.profile.first_feasible'],
            },
            {
              value: 'balanced',
              description: '',
              profilesMin: 'v0.0',
              requiredCapabilities: ['planning.profile.balanced'],
            },
          ],
          constraints: {},
          parameterizable: false,
          profilesMin: 'v0.0',
          description: '',
        },
      ],
    };
    useCatalogStore.setState({
      entries: CATALOG.map((entry) => (
        entry.id === 'move_manipulator_action' ? profiledMotionEntry : entry
      )),
      version: '0.3.0',
    });
    useDeploymentStore.setState({
      deployment: {
        ...DEPLOYMENT,
        adapters: {
          ...DEPLOYMENT.adapters,
          planning: {
            ...DEPLOYMENT.adapters.planning,
            capabilityKeys: [
              'planning.motion.pose_goal',
              'planning.motion.cartesian_path',
              'planning.profile.balanced',
            ],
          },
        },
      },
    });

    useProgramStore.getState().addNode(
      'move_manipulator_action',
      { x: 100, y: 100 },
    );

    const [node] = useProgramStore.getState().nodes;
    expect(node.data.params).not.toHaveProperty('planning_profile');
  });
});
