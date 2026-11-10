import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue, SourceModelProjection } from '../types/api';
import type { ManualControlGripperDescriptor } from '../types/robotControl';
import { api } from '../api/client';
import { DeploymentFocusedEditor, geometricContactRobotOptions } from './DeploymentFocusedEditor';
import { DraftInputResetProvider } from './common/DraftInputs';
import { statusFixture } from '../testFixtures/robotControl';

const digest = `sha256:${'1'.repeat(64)}`;

function controller(
  actuationKind: 'position_commanded' | 'pneumatic' = 'position_commanded',
): DeploymentWorkspaceController {
  return {
    workspace: {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft-1', programDraftRevision: 1,
        deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
      },
      draft: {
        id: 'deployment-draft-1',
        revision: 1,
        document: {
          api_version: 'manyforge.deployment.v1',
          deployment_artifact_id: 'cell-a',
          deployment_revision: digest,
          cell_id: 'cell-a',
          name: 'Cell A',
          graph: {
            control_bindings: [{ control_binding_id: 'arm_binding', control_binding_revision: digest,
              controllable_component_id: 'robot', command_target_id: 'arm', capability_id: 'execution.trajectory.execute' }],
            runtime_units: [{
              runtime_unit_id: 'robot.ros2_control_execution',
              configuration: {
                mode: 'simulated',
                execution_tick_hz: 100,
                collision_monitor_period_ms: 50,
                planner_rng_seed: 12345,
                initial_joint_positions_rad: [0, 1.57],
                direct_control_joint_limits: [],
                gripper_target_configurations: [{
                  controllable_component_id: 'robot',
                  command_target_id: 'gripper',
                  description: 'Native test gripper',
                  actuation_kind: actuationKind,
                  open_state_ref: 'open',
                  closed_state_ref: 'closed',
                  command_duration_s: 0.8,
                  touch_links: ['finger_link'],
                  mount_frame: 'tool0',
                  attach_frame: 'tool0',
                  tcp_frame: 'tool0',
                  auxiliary_frames: [],
                }],
                motion_target_limits: [{
                  controllable_component_id: 'robot',
                  command_target_id: 'arm',
                  maximum_tcp_linear_speed_mps: 0.15,
                }],
              },
            }],
          },
          policies: {
            stop_stage_parameters: [{
              control_binding_id: 'arm_binding', control_binding_revision: digest,
              stop_escalation_stage_id: 'motion_controlled_stop',
              provider_contract_ref: { provider_contract_role_id: 'stop_contract' },
              default_contract_parameters: {
                stop_mode: 'soft_on_path',
                deceleration_time_s: 1,
                allow_natural_completion_on_short_path: true,
                jerk_limits_enabled: true,
                per_resource_limits: [{
                  resource_id: 'robot.arm.coordinate.000000',
                  max_deceleration: {
                    coordinate_kind: 'angular_rad_s2',
                    value_rad_s2: 4,
                  },
                  settle_velocity_threshold: {
                    coordinate_kind: 'angular_rad_s',
                    value_rad_s: 0.01,
                  },
                }],
              },
            }],
          },
        },
        deploymentArtifactId: 'cell-a',
        deploymentRevision: digest,
        graphRevision: digest,
        differsFromSaved: false,
      },
      saved: { path: '/workspace/cell-a.deployment.yaml', deploymentRevision: digest, differsFromApplied: null },
      managerApplied: { activationOrder: null, deploymentRevision: digest, scenarioId: 'cell-a' },
      runAdmission: { ready: true, reason: null },
      binding: null,
      scenarioCandidates: [],
      savedScenarioCandidates: [],
      unavailableReasons: [],
    },
    deploymentDraftInputResetEpoch: 0,
    scenarioDraftInputResetEpoch: 0,
    loading: false,
    busy: false,
    applying: false,
    error: null,
    errorCauseCode: null,
    feedback: null,
    busyOperation: null,
    applyConfirmationOpen: false,
    applyPreview: null,
    applyPreviewKind: 'apply',
    comparisonUnavailableReason: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
    documentUnavailableReason: null,
    refresh: vi.fn(),
    clearFeedback: vi.fn(),
    dismissError: vi.fn(),
    open: vi.fn(),
    mutate: vi.fn(),
    updateControlledStop: vi.fn(),
    updateMotionTargetLimit: vi.fn(),
    revert: vi.fn(),
    save: vi.fn(),
    saveAs: vi.fn(),
    discard: vi.fn(),
    applyDraft: vi.fn(),
    applyConfirmedScope: vi.fn(),
    addComponents: vi.fn(),
    previewApply: vi.fn(),
    confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  };
}

afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });

function rosGripperController(
  actuationKind: 'position_commanded' | 'pneumatic',
  outputMode: 'commanded' | 'none' = 'commanded',
) {
  const value = controller(actuationKind);
  const graph = value.workspace!.draft.document!.graph as Record<string, JsonValue>;
  const runtime = (graph.runtime_units as Array<Record<string, JsonValue>>)[0];
  const configuration = runtime.configuration as Record<string, JsonValue>;
  runtime.runtime_unit_id = 'robot.ros2_control_execution';
  configuration.mode = 'commanded_sim';
  const row = (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0];
  row.output_mode = outputMode;
  configuration.component_streams = [
    { controllable_component_id: 'robot', stream_id: 'gripper_feedback',
      budget_ns: 1_000_000_000 },
    { controllable_component_id: 'robot', stream_id: 'measured_joints',
      budget_ns: 250_000_000 },
  ];
  return value;
}

// Use the existing mounted-test pattern: real React state and handlers on a
// minimal DOM host, without introducing another browser/test dependency.
type FakeNode = Record<string, unknown>;
type Props = Record<string, unknown> & {
  onChange?: (event: unknown) => void;
  onBlur?: (event: unknown) => void;
  onClick?: () => void;
};
function mountedHost() {
  const elements: FakeNode[] = [];
  const document: FakeNode = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): FakeNode => {
    const element = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
      focus: () => { document.activeElement = element; },
      blur: () => { document.activeElement = null; },
    };
    elements.push(element);
    return element;
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const propsFor = (element: FakeNode): Props | undefined => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const findElement = (match: (props: Props) => boolean) => {
    const found = elements.find((element) => {
      const props = propsFor(element);
      return props !== undefined && match(props);
    });
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  const find = (match: (props: Props) => boolean) => {
    const found = propsFor(findElement(match));
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find, findElement };
}

async function mountGripper(
  actuationKind: 'position_commanded' | 'pneumatic',
  outputMode: 'commanded' | 'none' = 'commanded',
  pneumaticIo?: JsonValue,
  { scenarioTool = false, graspConfirmation, stopFinishTolerance }: {
    scenarioTool?: boolean;
    graspConfirmation?: JsonValue;
    stopFinishTolerance?: number;
  } = {},
) {
  const value = rosGripperController(actuationKind, outputMode);
  const graph = value.workspace!.draft.document!.graph as Record<string, JsonValue>;
  const runtime = (graph.runtime_units as Array<Record<string, JsonValue>>)[0];
  const configuration = runtime.configuration as Record<string, JsonValue>;
  const sourceRow = (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0];
  if (pneumaticIo !== undefined) sourceRow.pneumatic_io = pneumaticIo;
  if (graspConfirmation !== undefined) sourceRow.grasp_confirmation = graspConfirmation;
  if (stopFinishTolerance !== undefined) sourceRow.stop_finish_tolerance = stopFinishTolerance;
  if (scenarioTool) {
    // A Scenario Draft: the gripper row sits on a tool component mounted on
    // the robot flange, nested under runtime_configuration.
    sourceRow.controllable_component_id = 'lite_gripper';
    const model = { urdf: { project_path: 'model.urdf' }, package_roots: {} };
    value.workspace!.draft.document = {
      api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell-a', name: 'Cell A',
      components: [
        { component_id: 'lite6_robot', component_profile_id: 'lite6_v0', model_source: model, target_settings: {},
          root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
          configuration: { runtime_kind: 'ros2_control', runtime_configuration: {} } },
        { component_id: 'lite_gripper', component_profile_id: 'lite_gripper_v0', model_source: model, target_settings: {},
          configuration: { runtime_kind: 'ros2_control', runtime_configuration: { gripper_target_configurations: [sourceRow] } } },
      ],
      attachments: [{
        parent: { component_id: 'lite6_robot', interface: 'flange' },
        child: { component_id: 'lite_gripper', interface: 'mount' },
      }],
      policies: {},
    };
  }
  vi.mocked(value.mutate).mockResolvedValue(value.workspace);
  const descriptor: ManualControlGripperDescriptor = {
    ...statusFixture().directControlGripper!.targetDescriptor,
    controllableComponentId: scenarioTool ? 'lite_gripper' : 'robot', actuationKind, outputMode,
    completionPolicy: actuationKind === 'pneumatic'
      ? outputMode === 'none' ? 'no_output' : 'output_delivery'
      : 'goal_fenced_target_reached',
    graspConfirmationPolicy: actuationKind === 'pneumatic' ? 'not_required' : 'binding_feedback_required',
    namedPresets: [
      { name: 'closed', masterPosition: 0, coordinates: [] },
      { name: 'open', masterPosition: 0.04, coordinates: [] },
    ],
  };
  const { host, find, findElement } = mountedHost();
  const resetKeys = { scenario: 0, program: 0, deployment: 0 };
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = () => root.render(
    <DraftInputResetProvider scope="scenario" resetKey={resetKeys.scenario}>
      <DraftInputResetProvider scope="program" resetKey={resetKeys.program}><span /></DraftInputResetProvider>
      <DraftInputResetProvider scope="deployment" resetKey={resetKeys.deployment}>
        <DeploymentFocusedEditor controller={value}
          sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[descriptor]} />
      </DraftInputResetProvider>
    </DraftInputResetProvider>,
  );
  await act(() => render());
  const field = (label: string) => {
    const row = find((props) => props.className === 'deployment-focused-field' && Array.isArray(props.children)
      && (props.children[0] as { props?: Props })?.props?.children === label);
    return (row.children as Array<{ props: Props }>)[1].props;
  };
  const duration = () => find((props) => props['aria-label'] === (
    field('Actuation kind').value === 'pneumatic' ? 'Program dwell' : 'Motion time'
  ));
  const setDuration = (durationS: number) => act(() => {
    const input = duration();
    (input.onFocus as ((event: unknown) => void) | undefined)?.({ target: { value: String(durationS) } });
    input.onChange?.({ target: { value: String(durationS) } });
  });
  const update = () => act(() => (
    (duration().onBlur as (() => void) | undefined)?.()
  ));
  const reset = (scope: keyof typeof resetKeys) => act(() => {
    resetKeys[scope] += 1;
    render();
  });
  return { value, sourceRow, find, findElement, field, duration, setDuration, update, reset,
    rerender: () => act(() => render()),
    unmount: () => act(() => root.unmount()) };
}

describe('DeploymentFocusedEditor', () => {
  it('offers each motion robot once for geometric contact', () => {
    const descriptor = statusFixture().directControl!.targetDescriptor;
    expect(geometricContactRobotOptions([
      descriptor,
      { ...descriptor, commandTargetId: 'other_target' },
    ])).toEqual([{ componentId: descriptor.controllableComponentId }]);
  });

  it('offers a motion robot for dynamic geometric contact', async () => {
    const value = controller();
    vi.mocked(value.mutate).mockResolvedValue(value.workspace);
    const document = value.workspace!.draft.document as Record<string, JsonValue>;
    const graph = document.graph as Record<string, JsonValue>;
    const runtime = (graph.runtime_units as Array<Record<string, JsonValue>>)[0];
    const configuration = runtime.configuration as Record<string, JsonValue>;
    configuration.io_signals = [{
      controllable_component_id: 'robot', command_target_id: 'io',
      resource_role_id: 'contact', signal_name: 'contact', source_id: 'contact_geometry',
      value_type: 'bool', direction: 'input', update_mode: 'sampled',
      transition_capability: 'sampled_only',
      freshness: { mode: 'sample_age', maximum_age_ns: 75_000_000 },
      provider: {
        profile: 'geometric_contact', ingress_depth: 64, robot_component_id: 'old_robot',
        target_scene_resource_id: 'workpiece',
      },
    }];
    const descriptor = statusFixture().directControl!.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.commandTargetId = 'arm';
    const target = (targetId: string): SourceModelProjection['targets'][number] => ({
      collision_bodies: [], component_id: 'robot', model_groups: [], target_id: targetId,
      frames: [{ frame_kind: 'component_model_frame', frame_role_id: 'tcp',
        model_instance_id: 'lite6', source_link_name: targetId === 'gripper' ? 'link_eef' : 'link6' }],
    });
    const sourceModel = {
      targets: [target('arm'), target('gripper')], occurrences: [],
    } as unknown as SourceModelProjection;
    const { host, find } = mountedHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(() => root.render(<DeploymentFocusedEditor controller={value}
        sourceModel={{ status: 'ready', data: sourceModel }} motionDescriptors={[descriptor]}
        gripperDescriptors={[]} />));
      const robot = find((props) => props['aria-label'] === 'contact Robot');
      expect(find((props) => [props.children].flat().join('') === 'old_robot (not currently listed)')).toBeDefined();
      expect(find((props) => props.value === 'robot')).toBeDefined();
      await act(() => robot.onChange?.({ target: { value: 'robot' } }));
      expect(value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [expect.objectContaining({ provider: expect.objectContaining({ robot_component_id: 'robot' }) })],
      );
    } finally {
      await act(() => root.unmount());
    }
  });

  it('edits the gripper row of a mounted tool in a Scenario Draft (nested runtime_configuration)', async () => {
    const mounted = await mountGripper('pneumatic', 'commanded', undefined, { scenarioTool: true });
    try {
      expect(mounted.field('Program dwell')).toBeDefined();
      await mounted.setDuration(0.25);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenLastCalledWith(
        ['components', 1, 'configuration', 'runtime_configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
        'set', 0.25,
      );
      expect(mounted.sourceRow.controllable_component_id).toBe('lite_gripper');
    } finally { await mounted.unmount(); }
  });

  it.each(['commanded', 'none'] as const)(
    'preserves the pneumatic row through edits and %s output changes', async (mode) => {
      const mounted = await mountGripper('pneumatic', mode);
      try {
        expect(mounted.field('Program dwell')).toBeDefined();
        expect(() => mounted.field('Motion time')).toThrow('Mounted control not found');
        await mounted.setDuration(0.25);
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
          'set', 0.25,
        );
        const otherMode = mode === 'none' ? 'commanded' : 'none';
        await act(() => mounted.field('Output').onChange?.({ target: { value: otherMode } }));
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'output_mode'],
          'set', otherMode,
        );
      } finally { await mounted.unmount(); }
    },
  );

  it('preserves pneumatic signal mappings when an unrelated gripper field changes', async () => {
    const io: JsonValue = {
      outputs: [{
        signal: { controllable_component_id: 'valves', signal_name: 'open' },
        open_value: true, close_value: false,
      }],
      inputs: {
        workpiece_grasped: { signal: { controllable_component_id: 'sensors', signal_name: 'part' } },
      },
    };
    const mounted = await mountGripper('pneumatic', 'commanded', io);
    try {
      await mounted.setDuration(0.25);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenLastCalledWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
        'set', 0.25,
      );
    } finally { await mounted.unmount(); }
  });

  it.each(['position_commanded', 'pneumatic'] as const)(
    'retains the existing row fields for %s', async (kind) => {
      const mounted = await mountGripper(kind);
      try {
        expect(Object.keys(mounted.sourceRow)).toHaveLength(13);
        await mounted.setDuration(0.25);
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
          'set', 0.25,
        );
      } finally { await mounted.unmount(); }
    },
  );

  it('writes an intentional pneumatic switch to position as one row', async () => {
    const mounted = await mountGripper('pneumatic');
    try {
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', { ...mounted.sourceRow, actuation_kind: 'position_commanded' },
      );
    } finally { await mounted.unmount(); }
  });

  it.each([0, 0.7])('writes pneumatic no-output and dwell %s as separate valid fields', async (duration) => {
    const mounted = await mountGripper('pneumatic');
    try {
      expect(mounted.field('Output').value).toBe('commanded');
      await act(() => mounted.field('Output').onChange?.({ target: { value: 'none' } }));
      await mounted.setDuration(duration);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenNthCalledWith(1,
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'output_mode'], 'set', 'none');
      expect(mounted.value.mutate).toHaveBeenNthCalledWith(2,
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'], 'set', duration);
    } finally { await mounted.unmount(); }
  });

  it('rejects a position switch while pneumatic output is still none', async () => {
    const mounted = await mountGripper('pneumatic', 'none');
    try {
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.field('Output').value).toBe('none');
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(mounted.find((props) => props.role === 'alert').children)
        .toBe('No output is available only for pneumatic grippers.');
    } finally { await mounted.unmount(); }
  });

  it('does not submit an unsupported output choice', async () => {
    const mounted = await mountGripper('pneumatic');
    try {
      await act(() => mounted.field('Output').onChange?.({ target: { value: 'default' } }));
      await mounted.update();
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(mounted.find((props) => props.role === 'alert').children)
        .toBe('Choose Send command or No output.');
    } finally { await mounted.unmount(); }
  });

  it.each([
    ['pneumatic', 0, true],
    ['pneumatic', 60, true],
    ['position_commanded', 0, false],
    ['position_commanded', 0.1, true],
    ['position_commanded', 60, true],
    ...(['pneumatic', 'position_commanded'] as const).flatMap((kind) => (
      [-0.1, 60.1, NaN, Infinity, -Infinity].map((duration) => [kind, duration, false] as const)
    )),
  ] as const)('validates %s duration %s before Draft mutation', async (kind, duration, valid) => {
    const mounted = await mountGripper(kind);
    try {
      expect(mounted.duration().min).toBe(kind === 'pneumatic' ? 0 : 0.000001);
      expect(mounted.duration().max).toBe(60);
      await mounted.setDuration(duration);
      await mounted.update();
      if (valid) {
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
          'set', duration,
        );
      } else {
        expect(mounted.value.mutate).not.toHaveBeenCalled();
        expect(mounted.find((props) => props.role === 'alert').children).toContain(
          Number.isFinite(duration)
            ? kind === 'pneumatic' ? 'from 0 to 60 s' : 'above 0 and at most 60 s'
            : 'Enter a valid motion time.',
        );
      }
    } finally { await mounted.unmount(); }
  });

  it('keeps zero pneumatic dwell when a position switch would make it invalid', async () => {
    const mounted = await mountGripper('pneumatic');
    try {
      await mounted.setDuration(0);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
        'set', 0,
      );
      const nextDocument = JSON.parse(JSON.stringify(mounted.value.workspace!.draft.document)) as JsonValue;
      const graph = (nextDocument as Record<string, JsonValue>).graph as Record<string, JsonValue>;
      const runtime = (graph.runtime_units as Array<Record<string, JsonValue>>)[0];
      const configuration = runtime.configuration as Record<string, JsonValue>;
      const row = (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0];
      row.command_duration_s = 0;
      mounted.value.workspace!.draft.document = nextDocument as Record<string, JsonValue>;
      await mounted.rerender();
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'command_duration_s'],
        'set', 0,
      );
      expect(mounted.duration().min).toBe(0);
      expect(mounted.find((props) => props.role === 'alert').children)
        .toContain('motion time above 0');
    } finally { await mounted.unmount(); }
  });

  it('clears Stop finish tolerance text and validation on its scoped reset without blurring', async () => {
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { stopFinishTolerance: 0.018 });
    const stopInput = () => (
      (mounted.field('Stop finish tolerance').children as Array<{ props: Props }>)[0].props
    );
    try {
      const input = stopInput();
      const element = mounted.findElement((props) => props === input);
      (element.focus as () => void)();
      await act(() => stopInput().onChange?.({ target: { value: '-1' } }));
      await act(() => stopInput().onBlur?.({}));
      expect(stopInput().value).toBe('-1');
      expect(stopInput()['aria-invalid']).toBe(true);
      expect(mounted.find((props) => props.role === 'alert').children)
        .toBe('Stop finish tolerance must be a number of 0 or more.');
      expect(mounted.value.mutate).not.toHaveBeenCalled();

      // A no-op/failed reset and a Program-scoped reset do not clear this Deployment edit.
      await mounted.rerender();
      await mounted.reset('program');
      expect(stopInput().value).toBe('-1');
      expect(stopInput()['aria-invalid']).toBe(true);
      expect(mounted.find((props) => props.role === 'alert').children)
        .toBe('Stop finish tolerance must be a number of 0 or more.');
      expect(mounted.findElement((props) => props === stopInput())).toBe(element);
      expect((element.ownerDocument as FakeNode).activeElement).toBe(element);

      // The Deployment reset epoch clears the invalid draft even though its
      // Stop finish tolerance backing value remains 0.018.
      await mounted.reset('deployment');
      expect(stopInput().value).toBe('0.018');
      expect(stopInput()['aria-invalid']).not.toBe(true);
      expect(() => mounted.find((props) => props.role === 'alert')).toThrow('Mounted control not found');
      expect(mounted.findElement((props) => props === stopInput())).toBe(element);
      expect((element.ownerDocument as FakeNode).activeElement).toBe(element);
      expect(mounted.value.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('commits Controlled Stop selects and checkboxes directly with the full profile', async () => {
    const value = controller();
    const { host, find } = mountedHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    try {
      const profile = {
        stop_mode: 'soft_on_path', deceleration_time_s: 1,
        allow_natural_completion_on_short_path: true, jerk_limits_enabled: true,
        per_resource_limits: [expect.objectContaining({
          resource_id: 'robot.arm.coordinate.000000',
          max_deceleration: expect.objectContaining({ value: 4 }),
          settle_velocity_threshold: expect.objectContaining({ value: 0.01 }),
        })],
      };
      const update = (changes: Record<string, unknown>) => ({
        controllable_component_id: 'robot', command_target_id: 'arm',
        profile: expect.objectContaining({ ...profile, ...changes }),
      });
      await act(() => find((props) => props.value === 'soft_on_path' && props.onChange !== undefined)
        .onChange?.({ target: { value: 'spring_back_hold' } }));
      expect(value.updateControlledStop).toHaveBeenLastCalledWith(update({
        stop_mode: 'spring_back_hold', allow_natural_completion_on_short_path: false,
      }));
      const natural = find((props) => props.className === 'toggle-control' && Array.isArray(props.children)
        && props.children.includes('Allow natural completion on a short remaining path'));
      await act(() => (natural.children as Array<{ props: Props }>)[0].props
        .onChange?.({ target: { checked: false } }));
      expect(value.updateControlledStop).toHaveBeenLastCalledWith(update({
        allow_natural_completion_on_short_path: false,
      }));
      await act(() => find((props) => props['aria-label'] === 'Controlled Stop jerk limits')
        .onChange?.({ target: { value: 'off' } }));
      expect(value.updateControlledStop).toHaveBeenLastCalledWith(update({ jerk_limits_enabled: false }));
      expect(value.updateControlledStop).toHaveBeenCalledTimes(3);
      expect(() => find((props) => props.children === 'Update Controlled Stop Draft'))
        .toThrow('Mounted control not found');
    } finally { await act(() => root.unmount()); }
  });

  it('commits a Controlled Stop number only on blur or Enter with a valid value', async () => {
    const value = controller();
    const { host, find } = mountedHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const deceleration = () => find((props) =>
      props['aria-label'] === 'robot.arm.coordinate.000000 maximum deceleration');
    const type = (text: string) => act(() => deceleration().onChange?.({ target: { value: text } }));
    const blur = () => act(() => (deceleration().onBlur as () => void)());
    try {
      await type('');
      await type('-');
      await type('7');
      expect(value.updateControlledStop).not.toHaveBeenCalled();
      await type('-1');
      await blur();
      expect(value.updateControlledStop).not.toHaveBeenCalled();
      expect(deceleration()['aria-invalid']).toBe(true);
      await type('7');
      await blur();
      expect(value.updateControlledStop).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
        controllable_component_id: 'robot', command_target_id: 'arm',
        profile: expect.objectContaining({ per_resource_limits: [expect.objectContaining({
          max_deceleration: expect.objectContaining({ value: 7 }),
          settle_velocity_threshold: expect.objectContaining({ value: 0.01 }),
        })] }),
      }));
      await type('8');
      await act(() => (deceleration().onKeyDown as (event: unknown) => void)({
        key: 'Enter', currentTarget: { blur: () => (deceleration().onBlur as () => void)() },
      }));
      expect(value.updateControlledStop).toHaveBeenCalledTimes(2);
      expect(value.updateControlledStop).toHaveBeenLastCalledWith(expect.objectContaining({
        profile: expect.objectContaining({ per_resource_limits: [expect.objectContaining({
          max_deceleration: expect.objectContaining({ value: 8 }),
        })] }),
      }));
    } finally { await act(() => root.unmount()); }
  });

  // Fields show the backend's effective value and default;
  // writing adds the field, Reset to default removes it. The GUI holds no default.
  function robotDocument(configuration: Record<string, JsonValue>): Record<string, JsonValue> {
    return {
      api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell-a', name: 'Cell A',
      components: [{
        component_id: 'robot', component_profile_id: 'ufactory.lite6', lane: 'real', frame_prefix: 'robot/',
        model_source: { urdf: { project_path: 'sources/robot.urdf' }, package_roots: {} },
        root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
        target_settings: { arm: { maximum_tcp_linear_speed_mps: 0.2 } },
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: configuration },
      }],
      attachments: [],
      policies: {},
    };
  }
  const settingsPath = ['components', 0, 'configuration', 'runtime_configuration'];
  const key = (field: string) => [...settingsPath, field].join('.');

  async function mountEffective(
    configuration: Record<string, JsonValue>,
    fields: Record<string, { value: unknown; default: unknown; set: boolean }>,
    document: Record<string, JsonValue> = robotDocument(configuration),
  ) {
    const effective = vi.spyOn(api.cellApply, 'effective').mockResolvedValue({ fields });
    const value = controller();
    value.workspace!.draft.document = document;
    value.workspace!.draft.deploymentRevision = null;
    value.workspace!.draft.graphRevision = null;
    vi.mocked(value.mutate).mockResolvedValue(value.workspace);
    const { host, find } = mountedHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    await act(async () => { await Promise.resolve(); });
    return { value, find, effective, unmount: () => act(() => root.unmount()) };
  }

  it('shows the effective value of an unset field and writes an edit into the Draft', async () => {
    const mounted = await mountEffective({}, {
      [key('execution_tick_hz')]: { value: 250, default: 250, set: false },
      [key('collision_check_step_rad')]: { value: 0.05, default: 0.05, set: false },
    });
    try {
      expect(mounted.effective).toHaveBeenCalledOnce();
      const cadence = () => mounted.find((props) => props.type === 'number' && props.max === 10000);
      expect(cadence().value).toBe('250');
      expect(() => mounted.find((props) => props['aria-label'] === 'Reset Execution cadence to default')).toThrow();
      await act(() => cadence().onChange?.({ target: { value: '125' } }));
      await act(() => (cadence().onBlur as () => void)());
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith([...settingsPath, 'execution_tick_hz'], 'set', 125);
      const step = () => mounted.find((props) => props.type === 'number' && props.max === 0.2);
      expect(step().value).toBe('0.05');
      for (const refused of ['0', '0.2001']) {
        await act(() => step().onChange?.({ target: { value: refused } }));
        await act(() => (step().onBlur as () => void)());
      }
      expect(mounted.value.mutate).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('resets a set field to its default by removing it from the Draft', async () => {
    const mounted = await mountEffective({ collision_clearance_m: 0.004 }, {
      [key('collision_clearance_m')]: { value: 0.004, default: 0.0001, set: true },
    });
    try {
      expect(mounted.find((props) => props.type === 'number' && props.max === 0.05).value).toBe('0.004');
      await act(() => mounted.find((props) => props['aria-label'] === 'Reset Collision clearance to default').onClick?.());
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith([...settingsPath, 'collision_clearance_m'], 'remove');
    } finally { await mounted.unmount(); }
  });

  it('checks fresh plans before dispatch by default; unchecking sets it, Reset removes it', async () => {
    const label = 'Check fresh plans before dispatch';
    const box = (find: (match: (props: Props) => boolean) => Props) => find((props) => props['aria-label'] === label);
    const mounted = await mountEffective({}, {
      [key('collision_check_fresh_plans')]: { value: true, default: true, set: false },
    });
    try {
      expect(box(mounted.find).checked).toBe(true);
      expect(() => mounted.find((props) => props['aria-label'] === `Reset ${label} to default`)).toThrow();
      await act(() => box(mounted.find).onChange?.({ target: { checked: false } }));
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        [...settingsPath, 'collision_check_fresh_plans'], 'set', false);
    } finally { await mounted.unmount(); }

    const off = await mountEffective({ collision_check_fresh_plans: false }, {
      [key('collision_check_fresh_plans')]: { value: false, default: true, set: true },
    });
    try {
      expect(box(off.find).checked).toBe(false);
      expect(off.find((props) => props.children === 'Set · default on')).toBeDefined();
      await act(() => off.find((props) => props['aria-label'] === `Reset ${label} to default`).onClick?.());
      expect(off.value.mutate).toHaveBeenCalledExactlyOnceWith(
        [...settingsPath, 'collision_check_fresh_plans'], 'remove');
    } finally { await off.unmount(); }
  });

  it('hides a field the backend does not report', async () => {
    const mounted = await mountEffective({ planner_rng_seed: 7 }, {});
    try {
      expect(() => mounted.find((props) => props.type === 'number' && props.value === '7')).toThrow();
    } finally { await mounted.unmount(); }
  });

  it('edits a point-cloud source: required topic and source frame, limits with effective values', async () => {
    const sensorDocument = {
      api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell-a', name: 'Cell A',
      components: [{
        component_id: 'front_points', component_profile_id: 'manyforge.pointcloud_source',
        target_settings: {},
        configuration: { runtime_kind: 'sensor',
          runtime_configuration: { topic: '/front/points', source_frame_id: 'camera_link', max_points: 5000 } },
      }],
      attachments: [],
      policies: {},
    };
    const mounted = await mountEffective({}, {
      [key('max_points')]: { value: 5000, default: 200000, set: true },
      [key('minimum_period_s')]: { value: 0.1, default: 0.1, set: false },
    }, sensorDocument);
    try {
      expect(mounted.find((props) => props['aria-label'] === 'front_points topic').value).toBe('/front/points');
      expect(mounted.find((props) => props['aria-label'] === 'front_points source frame').value).toBe('camera_link');
      // Required, no default: nothing to reset to.
      expect(() => mounted.find((props) => props['aria-label'] === 'Reset Source frame to default')).toThrow();
      // No robot sections for a sensor.
      expect(() => mounted.find((props) => props.type === 'number' && props.max === 10000)).toThrow();
      const maxPoints = () => mounted.find((props) => props.type === 'number' && props.max === 200000);
      expect(maxPoints().value).toBe('5000');
      await act(() => mounted.find((props) => props['aria-label'] === 'Reset Max points to default').onClick?.());
      expect(mounted.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'max_points'], 'remove');
      const period = () => mounted.find((props) => props.type === 'number' && props.max === 10);
      await act(() => period().onChange?.({ target: { value: '0.5' } }));
      await act(() => (period().onBlur as () => void)());
      expect(mounted.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'minimum_period_s'], 'set', 0.5);
    } finally { await mounted.unmount(); }
  });

  it('shows Cartesian timing at its default and writes the chosen timing, with Reset to default', async () => {
    const unset = await mountEffective({}, {
      [key('cartesian_timing')]: { value: 'constant_speed', default: 'constant_speed', set: false },
    });
    try {
      const select = () => unset.find((props) => props['aria-label'] === 'Cartesian timing');
      expect(select().value).toBe('constant_speed');
      expect(() => unset.find((props) => props['aria-label'] === 'Reset Cartesian timing to default')).toThrow();
      await act(() => select().onChange?.({ target: { value: 'time_optimal' } }));
      expect(unset.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'cartesian_timing'], 'set', 'time_optimal');
    } finally { await unset.unmount(); }
    const set = await mountEffective({ cartesian_timing: 'time_optimal' }, {
      [key('cartesian_timing')]: { value: 'time_optimal', default: 'constant_speed', set: true },
    });
    try {
      expect(set.find((props) => props['aria-label'] === 'Cartesian timing').value).toBe('time_optimal');
      await act(() => set.find((props) => props['aria-label'] === 'Reset Cartesian timing to default').onClick?.());
      expect(set.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'cartesian_timing'], 'remove');
    } finally { await set.unmount(); }
  });

  it('shows Motion timing at its default Smooth and writes Fast, with Reset to default', async () => {
    const unset = await mountEffective({}, {
      [key('motion_timing')]: { value: 'smooth', default: 'smooth', set: false },
    });
    try {
      const select = () => unset.find((props) => props['aria-label'] === 'Motion timing');
      expect(select().value).toBe('smooth');
      expect(unset.find((props) => props.children === 'Smooth (slower)').value).toBe('smooth');
      expect(unset.find((props) => props.children === 'Fast (near the TCP speed limit)').value).toBe('fast');
      expect(() => unset.find((props) => props['aria-label'] === 'Reset Motion timing to default')).toThrow();
      await act(() => select().onChange?.({ target: { value: 'fast' } }));
      expect(unset.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'motion_timing'], 'set', 'fast');
    } finally { await unset.unmount(); }
    const set = await mountEffective({ motion_timing: 'fast' }, {
      [key('motion_timing')]: { value: 'fast', default: 'smooth', set: true },
    });
    try {
      expect(set.find((props) => props['aria-label'] === 'Motion timing').value).toBe('fast');
      await act(() => set.find((props) => props['aria-label'] === 'Reset Motion timing to default').onClick?.());
      expect(set.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'motion_timing'], 'remove');
    } finally { await set.unmount(); }
  });

  it('shows the planner block defaults, writes a choice and shows N only for Best of N', async () => {
    const defaults = {
      [key('planner_search')]: { value: 'rrt_connect', default: 'rrt_connect', set: false },
      [key('planner_optimize')]: { value: 'first', default: 'first', set: false },
      [key('planner_n')]: { value: 4, default: 4, set: false },
      [key('planner_max_nodes')]: { value: 5000, default: 5000, set: false },
    };
    const unset = await mountEffective({}, defaults);
    try {
      expect(unset.find((props) => props['aria-label'] === 'Planner search').value).toBe('rrt_connect');
      const optimize = () => unset.find((props) => props['aria-label'] === 'Planner optimize');
      expect(optimize().value).toBe('first');
      expect(() => unset.find((props) => props.type === 'number' && props.max === 32)).toThrow();
      expect(unset.find((props) => props.type === 'number' && props.max === 1000000).value).toBe('5000');
      await act(() => optimize().onChange?.({ target: { value: 'best_of_n' } }));
      expect(unset.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'planner_optimize'], 'set', 'best_of_n');
    } finally { await unset.unmount(); }
    const best = await mountEffective({ planner_optimize: 'best_of_n' }, {
      ...defaults,
      [key('planner_optimize')]: { value: 'best_of_n', default: 'first', set: true },
    });
    try {
      expect(best.find((props) => props.type === 'number' && props.max === 32).value).toBe('4');
      await act(() => best.find((props) => props['aria-label'] === 'Reset Planner optimize to default').onClick?.());
      expect(best.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'planner_optimize'], 'remove');
    } finally { await best.unmount(); }
  });

  it('writes the chosen Stop lead-in start and TCP offset sync, with Reset to default', async () => {
    const mounted = await mountEffective({ stop_lead_in_anchor: 'measured' }, {
      [key('stop_lead_in_anchor')]: { value: 'measured', default: 'commanded', set: true },
      [key('controller_tcp_offset_sync')]: { value: true, default: true, set: false },
    });
    try {
      const select = () => mounted.find((props) => props['aria-label'] === 'Stop lead-in start');
      expect(select().value).toBe('measured');
      await act(() => select().onChange?.({ target: { value: 'commanded' } }));
      expect(mounted.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'stop_lead_in_anchor'], 'set', 'commanded');
      await act(() => mounted.find((props) => props['aria-label'] === 'Reset Stop lead-in start to default').onClick?.());
      expect(mounted.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'stop_lead_in_anchor'], 'remove');
      const checkbox = () => mounted.find((props) => props['aria-label'] === 'Send TCP offset to controller');
      expect(checkbox().checked).toBe(true);
      await act(() => checkbox().onChange?.({ target: { checked: false } }));
      expect(mounted.value.mutate).toHaveBeenLastCalledWith([...settingsPath, 'controller_tcp_offset_sync'], 'set', false);
    } finally { await mounted.unmount(); }
  });

  it('edits a component\'s lifecycle time budget in the Draft, blank keeping the default', async () => {
    // Per-component time budget: robots, tools, pose sources and AI models
    // author `configuration.lifecycle_command_timeout_s`; blank removes it.
    const value = controller();
    value.workspace!.draft.document = {
      api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell-a', name: 'Cell A',
      components: [{
        component_id: 'llm', component_profile_id: 'ai_model_v0', target_settings: {},
        configuration: {
          runtime_kind: 'ai_model',
          runtime_configuration: { base_url: 'http://127.0.0.1:8000/v1', model: 'm' },
        },
      }, {
        component_id: 'robot', component_profile_id: 'ufactory.lite6', lane: 'isaac',
        model_source: { urdf: { project_path: 'model.urdf' }, package_roots: {} },
        root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
        target_settings: { arm: { maximum_tcp_linear_speed_mps: 0.15 } },
        configuration: {
          runtime_kind: 'ros2_control',
          lifecycle_command_timeout_s: 120,
          runtime_configuration: {},
        },
      }],
      attachments: [],
      policies: {},
    };
    value.workspace!.draft.deploymentRevision = null;
    value.workspace!.draft.graphRevision = null;
    vi.mocked(value.mutate).mockResolvedValue(value.workspace);
    const { host, find } = mountedHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const budget = (id: string) => () => find((props) => props['aria-label'] === `${id} lifecycle command time limit`);
    const commit = async (input: () => Props, text: string) => {
      await act(() => (input().onFocus as ((event: unknown) => void) | undefined)?.({}));
      await act(() => input().onChange?.({ target: { value: text } }));
      await act(() => (input().onBlur as () => void)());
    };
    const robot = budget('robot');
    const model = budget('llm');
    try {
      expect(robot().value).toBe('120');
      expect(model().value).toBe('');
      expect(model().placeholder).toBe('default');
      await commit(robot, '600');
      expect(value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'lifecycle_command_timeout_s'], 'set', 600);
      await commit(robot, '');
      expect(value.mutate).toHaveBeenLastCalledWith(
        ['components', 1, 'configuration', 'lifecycle_command_timeout_s'], 'remove');
      for (const refused of ['0', '3601', 'x']) await commit(robot, refused);
      expect(value.mutate).toHaveBeenCalledTimes(2);
      await commit(model, '90');
      expect(value.mutate).toHaveBeenLastCalledWith(
        ['components', 0, 'configuration', 'lifecycle_command_timeout_s'], 'set', 90);
      // Blank on a component without a budget writes nothing.
      await commit(model, '');
      expect(value.mutate).toHaveBeenCalledTimes(3);
      // A set budget offers Reset to default; an unset one doesn't.
      expect(() => find((props) => props['aria-label'] === 'Reset llm lifecycle command time limit to default')).toThrow();
      await act(() => find((props) => props['aria-label'] === 'Reset robot lifecycle command time limit to default').onClick?.());
      expect(value.mutate).toHaveBeenLastCalledWith(
        ['components', 1, 'configuration', 'lifecycle_command_timeout_s'], 'remove');
    } finally { await act(() => root.unmount()); }
  });

  it('shows the lifecycle time budget\'s effective value and resets a set one to default', async () => {
    const budgetPath = ['components', 0, 'configuration', 'lifecycle_command_timeout_s'];
    const unset = await mountEffective({}, {
      [budgetPath.join('.')]: { value: 10, default: 10, set: false },
    });
    const budget = (mounted: typeof unset) => mounted.find((props) => props['aria-label'] === 'robot lifecycle command time limit');
    try {
      expect(budget(unset).value).toBe('10');
      expect(() => unset.find((props) => props['aria-label'] === 'Reset robot lifecycle command time limit to default')).toThrow();
      // Re-entering the effective value writes nothing; another value is written.
      await act(() => (budget(unset).onFocus as ((event: unknown) => void) | undefined)?.({}));
      await act(() => budget(unset).onChange?.({ target: { value: '10' } }));
      await act(() => (budget(unset).onBlur as () => void)());
      expect(unset.value.mutate).not.toHaveBeenCalled();
      await act(() => (budget(unset).onFocus as ((event: unknown) => void) | undefined)?.({}));
      await act(() => budget(unset).onChange?.({ target: { value: '25' } }));
      await act(() => (budget(unset).onBlur as () => void)());
      expect(unset.value.mutate).toHaveBeenCalledExactlyOnceWith(budgetPath, 'set', 25);
    } finally { await unset.unmount(); }
  });

  it('renders authored intent without requiring a compiled graph projection', () => {
    const value = controller();
    value.workspace!.draft.document = {
      api_version: 'manyforge.deployment_intent.v2',
      deployment_artifact_id: 'cell-a',
      name: 'Cell A',
      components: [{
        component_id: 'robot',
        component_profile_id: 'robot_native_v1',
        model_source: {
          urdf: { project_path: 'sources/robot.urdf' },
          package_roots: {},
        },
        root_pose_in_cell: {
          position_m: [0, 0, 0],
          orientation_xyzw: [0, 0, 0, 1],
        },
        target_settings: {
          arm: {
            maximum_tcp_linear_speed_mps: 0.2,
            controlled_stop: {
              stop_mode: 'soft_on_path',
              allow_natural_completion_on_short_path: true,
              jerk_limits_enabled: true,
              deceleration_time_s: 1,
              per_joint_limits: [{
                source_joint_name: 'joint1',
                max_deceleration_rad_s2: 4,
                settle_velocity_threshold_rad_s: 0.01,
              }],
            },
          },
        },
        configuration: {
          mode: 'simulated',
          execution_tick_hz: 100,
          collision_monitor_period_ms: 50,
          planner_rng_seed: 12345,
          initial_joint_positions_rad: [0, 1.57],
          direct_control_joint_limits: [],
          gripper_target_configurations: [],
        },
      }],
      attachments: [],
      policies: {},
    };
    value.workspace!.draft.deploymentRevision = null;
    value.workspace!.draft.graphRevision = null;
    const descriptor = statusFixture().directControl!.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.commandTargetId = 'arm';
    descriptor.operationalLimit = { maximumTcpLinearSpeedMps: 0.15 };

    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]}
      gripperDescriptors={[]} />);

    expect(markup).toContain('Maximum TCP linear speed');
    expect(markup).toContain('value="0.2"');
    expect(markup).toContain('robot · arm');
    expect(markup).toContain('joint1 maximum deceleration');
    expect(markup).toContain('Execution host configuration');
    expect(markup).not.toContain('TF frame prefix');
    expect(markup).not.toContain('Applying a change restarts this robot holding.');
    // Effective-value fields wait for the backend's values.
    expect(markup).not.toContain('Execution cadence');
    expect(markup).toContain('Controlled Stop jerk limits');
    // The target is described and has a limit, but a Scenario component's
    // runtime has no direct-control caps: no editor whose values Apply ignores.
    // (A compiled runtime unit still shows it; see the canonical-inputs test.)
    expect(markup).toContain('Ordinary motion jerk');
    expect(markup).not.toContain('Robot Control joint caps');
    const authored = value.workspace!.draft.document as unknown as {
      components: Array<{ frame_prefix: string; configuration: Record<string, unknown> }>;
    };
    authored.components[0].configuration.runtime_kind = 'ros2_control';
    authored.components[0].frame_prefix = 'robot/';
    const rosMarkup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]}
      gripperDescriptors={[]} />);
    expect(rosMarkup).toContain('TF frame prefix');
    expect(rosMarkup).toContain('value="robot/"');
    expect(rosMarkup).toContain('Applying a change restarts this robot holding.');
    for (const prefix of ['', 'second/', 'Robot_2/nested/', '0/', '_/', 'robot//']) {
      authored.components[0].frame_prefix = prefix;
      const validMarkup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
        sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]}
        gripperDescriptors={[]} />);
      expect(validMarkup).toContain(`value="${prefix}"`);
      expect(validMarkup).not.toContain('Frame prefix must be empty');
      expect(validMarkup).toContain('aria-invalid="false"');
    }
    for (const prefix of ['literal prefix/', '/robot/', '/', 'robot', 'robot-b/', 'robot.b/',
      'röbot/', 'robot/\n', 'robot\\/']) {
      authored.components[0].frame_prefix = prefix;
      const invalidMarkup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
        sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]}
        gripperDescriptors={[]} />);
      expect(invalidMarkup).toContain('Frame prefix must be empty or use only ASCII letters');
      expect(invalidMarkup).toContain('without a leading');
      expect(invalidMarkup).toContain('with a trailing');
      expect(invalidMarkup).toContain('aria-invalid="true"');
    }
  });

  it.each([
    { enabled: false, override: null, maximum: '0' },
    { enabled: true, override: 10, maximum: '10' },
    { enabled: true, override: null, maximum: null },
  ])('bounds manual jerk from explicit Draft settings: %j', ({ enabled, override, maximum }) => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: Record<string, unknown> }> };
    };
    const configuration = document.graph.runtime_units[0].configuration;
    const targets = configuration.motion_target_limits as Array<Record<string, unknown>>;
    targets[0].ordinary_motion_jerk = {
      enabled, preset_reference_time_s: 2,
      joint_limits: [{ source_joint_name: 'joint1',
        ...(override === null ? {} : { override_rad_s3: override }),
      }],
    };
    const descriptor = statusFixture().directControl!.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.operationalLimit = {
      maximumTcpLinearSpeedMps: 0.15,
    };
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]} gripperDescriptors={[]} />);
    const input = markup.match(/Maximum jerk<\/span>[\s\S]*?(<input[^>]*>)/)?.[1];
    expect(input).toBeDefined();
    if (maximum === null) expect(input).not.toContain('max=');
    else expect(input).toContain(`max="${maximum}"`);
    // An applied preset is never used as an invented Draft ceiling.
    expect(input).not.toContain('disabled='); // clearing remains possible
  });

  it.each(['robot', 'other-robot'])('keeps manual jerk guards on the exact target: %s', (component) => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: Record<string, unknown> }> };
    };
    const configuration = document.graph.runtime_units[0].configuration;
    configuration.direct_control_joint_limits = [{ controllable_component_id: component,
      command_target_id: 'arm', source_joint_name: 'joint1', max_jerk_rad_s3: 8 }];
    const targets = configuration.motion_target_limits as Array<Record<string, unknown>>;
    targets[0].ordinary_motion_jerk = { enabled: true, preset_reference_time_s: 1, joint_limits: [] };
    const descriptor = statusFixture().directControl!.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.ordinaryJerk = { enabled: true, presetReferenceTimeS: 1, jointLimits: [{
      sourceJointName: 'joint1', source: 'preset', maximumRadS3: 12, presetRadS3: 12,
    }] };
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]} gripperDescriptors={[]} />);
    const selector = markup.match(/aria-label="robot \/ arm ordinary motion jerk"[\s\S]*?<\/select>/)?.[0];
    if (component === 'robot') {
      expect(selector).toContain('<option value="off" disabled="">');
      expect(markup).toContain('min="8"');
      expect(markup).toContain("Clear this target&#x27;s Robot Control jerk caps");
    } else {
      expect(selector).toContain('<option value="off">');
      expect(markup).not.toContain('min="8"');
      expect(markup).not.toContain('Clear this target');
    }
  });

  it('selects the exact described target instead of borrowing the first target limits', () => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: { motion_target_limits: unknown[] } }> };
    };
    document.graph.runtime_units[0].configuration.motion_target_limits.push({
      controllable_component_id: 'robot-b', command_target_id: 'arm',
      maximum_tcp_linear_speed_mps: 0.25,
    });
    const descriptor = statusFixture().directControl!.targetDescriptor;
    descriptor.controllableComponentId = 'robot-b';
    const unrelatedDescriptor = { ...descriptor, controllableComponentId: 'robot-c' };
    const sourceModel = {
      occurrences: [
        { component_id: 'robot-b', model_instance_id: 'unrelated-tool',
          source_robot_name: 'Unrelated tool', source_model_name: 'Wrong model', model_revision: digest },
        { component_id: 'robot-b', model_instance_id: 'selected-arm',
          source_robot_name: 'Selected robot', source_model_name: 'Selected arm', model_revision: digest },
      ],
      targets: [{ component_id: 'robot-b', target_id: 'arm', frames: [],
        model_groups: [{ model_instance_id: 'selected-arm', source_group_name: 'Selected group' }] }],
    } as unknown as SourceModelProjection;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'ready', data: sourceModel }}
      motionDescriptors={[unrelatedDescriptor, descriptor]} gripperDescriptors={[]} />);
    expect(markup).toContain('aria-label="Motion limits target"');
    expect(markup).toMatch(/<option[^>]*selected=""[^>]*>robot-b \/ arm<\/option>/);
    expect(markup).toContain('value="0.25"');
    expect(markup).not.toContain('value="0.15"');
    expect(markup).toContain('aria-label="Source model joint limits"');
    expect(markup).not.toContain('No current motion descriptor for this target.');
    expect(markup).toContain('Selected robot');
    expect(markup).toContain('Selected arm');
    expect(markup).toContain('Selected group');
    expect(markup).not.toContain('Unrelated tool');
    expect(markup).not.toContain('Wrong model');
  });

  it('does not show another component model or joint caps for an undescribed target', () => {
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={controller()}
      sourceModel={{ status: 'loading' }}
      motionDescriptors={[statusFixture().directControl!.targetDescriptor]} gripperDescriptors={[]} />);
    expect(markup).toContain('No current motion descriptor for this target.');
    expect(markup).not.toContain('aria-label="Source model joint limits"');
    expect(markup).not.toContain('Robot Control joint caps');
    expect(markup).not.toContain('aria-label="Motion limits target"');
    expect(markup).toContain('value="0.15"');
  });

  it('collapses derived capability bindings into one authored target Stop profile', () => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      graph: { control_bindings: Array<Record<string, unknown>> };
      policies: { stop_stage_parameters: Array<Record<string, unknown>> };
    };
    document.graph.control_bindings.push({ ...document.graph.control_bindings[0],
      control_binding_id: 'arm_contact', capability_id: 'execution.contact.geometric' });
    document.policies.stop_stage_parameters.push({ ...document.policies.stop_stage_parameters[0], control_binding_id: 'arm_contact' });
    const render = () => renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    let markup = render();
    expect(markup).toContain('robot · arm');
    expect(markup.match(/>robot · arm<\/option>/g)).toHaveLength(1);
    // Absent, the robot's own deadline to start braking is its
    // deceleration time + 0.5 s; the Stop then finishes like a motion.
    expect(markup).toContain('Explicit maximum Stop time');
    expect(markup).toContain(
      'Automatic: 1.5 s (deceleration time + 0.5 s) to start braking; the Stop then finishes like a normal motion, when the controller reports it.',
    );
    expect(markup).not.toContain('Max Stop time');
    for (const row of document.policies.stop_stage_parameters) {
      (row.default_contract_parameters as Record<string, unknown>).max_stop_time_s = 3;
    }
    markup = render();
    expect(markup).toContain('Max Stop time');
    expect(markup).toContain('value="3"');
    expect(markup).not.toContain('Automatic: 1.5 s');
    expect(markup).not.toContain('Stop timeout');
    expect(markup).not.toContain('Command latency');
  });

  it.each([false, true])('shows Spring brake-and-return timing and the saved natural-completion choice (%s)', (natural) => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      policies: { stop_stage_parameters: Array<{ default_contract_parameters: Record<string, unknown> }> };
    };
    const profile = document.policies.stop_stage_parameters[0].default_contract_parameters;
    profile.stop_mode = 'spring_back_hold';
    profile.allow_natural_completion_on_short_path = natural;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    expect(markup).toContain('Brake + return time');
    expect(markup).toContain('Allow natural completion');
    expect(markup).toContain('Off by default: brake and return');
    expect(markup).toMatch(natural ? /type="checkbox"[^>]*checked/ : /type="checkbox"(?![^>]*checked)[^>]*>/);
  });

  it('offers Spring-back fast and states plainly that its move is unchecked', () => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      policies: { stop_stage_parameters: Array<{ default_contract_parameters: Record<string, unknown> }> };
    };
    const profile = document.policies.stop_stage_parameters[0].default_contract_parameters;
    const render = () => renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    const soft = render();
    expect(soft).toContain('<option value="spring_back_hold">Spring-back on path</option>');
    expect(soft).toContain('<option value="spring_back_fast">Spring-back fast</option>');
    expect(soft).not.toContain('Sent at once, with no planning or checks');
    profile.stop_mode = 'spring_back_fast';
    const fast = render();
    expect(fast).toContain('Brake + return time');
    expect(fast).toContain('Sent at once, with no planning or checks');
    expect(fast).toContain('safety controller may trip');
  });

  it('keeps an older unreviewed Stop contract read-only', () => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      policies: { stop_stage_parameters: Array<{ default_contract_parameters: Record<string, unknown> }> };
    };
    delete document.policies.stop_stage_parameters[0].default_contract_parameters.jerk_limits_enabled;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    expect(markup).toContain('older Stop contract needs explicit migration');
    expect(markup).toMatch(/<select[^>]+aria-label="Controlled Stop jerk limits"[^>]+disabled/);
    expect(markup).toMatch(/<select[^>]+disabled[^>]*><option value="soft_on_path"/);
    expect(markup).toMatch(/<input[^>]+disabled[^>]+aria-label="robot\.arm\.coordinate\.000000 maximum deceleration"/);
    expect(markup).not.toContain('Update Controlled Stop Draft');
  });

  it('presents canonical target, execution host, and normalized Stop inputs', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={controller()}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[{
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v8',
          ordinaryJerk: { enabled: false, presetReferenceTimeS: 1, jointLimits: [{
            sourceJointName: 'joint1', source: 'off', maximumRadS3: null,
            presetRadS3: 12,
          }] },
          controllableComponentId: 'robot',
          endEffectorComponentId: null,
          commandTargetId: 'arm',
          frames: [],
          tcps: [],
          joints: [{
            sourceJointName: 'joint1', compiledJointName: 'robot__joint1', unit: 'rad',
            minPositionRad: -1, maxPositionRad: 1, maxVelocityRadS: 2,
            maxAccelerationRadS2: 3,
          }],
          operationalJointLimits: [{
            sourceJointName: 'joint1', modelMaxVelocityRadS: 2,
            modelMaxAccelerationRadS2: 3, deploymentMaxVelocityRadS: null,
            deploymentMaxAccelerationRadS2: null, deploymentMaxJerkRadS3: null,
            effectiveMaxVelocityRadS: 2, effectiveMaxAccelerationRadS2: 3,
            effectiveMaxJerkRadS3: null,
          }],
          operationalLimit: {
            maximumTcpLinearSpeedMps: 0.15,
          },
          contracts: {
            operationCapabilityId: 'execution.trajectory.execute',
            operationCapabilityDefinitionRevision: digest,
            commandSchema: null, controlPath: null, supportingProviders: [],
          },
        }]}
        gripperDescriptors={[]}
      />,
    );

    expect(markup).toContain('Robot model and target limits');
    expect(markup).toContain('Maximum TCP linear speed');
    expect(markup).toContain('value="0.15"');
    expect(markup).toContain('Execution host configuration');
    expect(markup).toContain('Initial joint state');
    expect(markup).toContain('Robot Control joint caps');
    expect(markup).toMatch(/Maximum velocity<\/span>[\s\S]*?<input[^>]*max="2"/);
    expect(markup).toMatch(/Maximum acceleration<\/span>[\s\S]*?<input[^>]*max="3"/);
    expect(markup).toContain('Ordinary motion jerk');
    expect(markup).toContain('Preset reference time');
    expect(markup).toContain('Applied: off');
    expect(markup).toContain('Preset: 12');
    expect(markup).toContain('Maximum acceleration');
    expect(markup).toContain('Controlled Stop');
    expect(markup).toContain('Controlled Stop jerk limits');
    expect(markup).toContain('Joint jerk limits');
    expect(markup).toContain('ManyForge preset');
    expect(markup).toContain('Soft on path');
    expect(markup).not.toContain('Update Controlled Stop Draft');
    expect(markup).not.toContain('Discard Stop edits');
    expect(markup).toContain('No current addressed gripper descriptor is available.');
  });

  it('explains a pneumatic I/O preparation refusal by its typed cause, not its text', () => {
    const render = (error: string | null, errorCauseCode: string | null) => renderToStaticMarkup(
      <DeploymentFocusedEditor controller={{ ...controller('pneumatic'), error, errorCauseCode }}
        sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />,
    );
    const coincident = 'Open and Close presets have the same position';
    const isaac = 'not supported by Isaac';
    expect(render('Not applied: the candidate could not be prepared.', 'pneumatic_io_presets_coincident'))
      .toContain(coincident);
    expect(render('Not applied: the candidate could not be prepared.', 'pneumatic_io_unsupported_on_isaac'))
      .toContain(isaac);
    // The reason named only in the text explains nothing.
    const textOnly = render('Not applied (pneumatic_io_presets_coincident: robot; '
      + 'pneumatic_io_unsupported_on_isaac).', null);
    expect(textOnly).not.toContain(coincident);
    expect(textOnly).not.toContain(isaac);
    // A cleared error hides the explanation.
    expect(render(null, 'pneumatic_io_presets_coincident')).not.toContain(coincident);
  });

  it('joins one authored gripper row to exact owner semantics', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={rosGripperController('position_commanded')}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[]}
        gripperDescriptors={[{
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v5',
          controllableComponentId: 'robot', commandTargetId: 'gripper',
          description: 'Native test gripper', commandKind: 'gripper_position',
          actuationKind: 'position_commanded', transportKind: 'resolved_binding',
          outputMode: 'commanded',
          controlInterfaceContractId: 'native.gripper',
          stateAuthorityId: 'native.predicted', stateAuthorityClass: 'predicted',
          completionPolicy: 'goal_fenced_target_reached',
          graspConfirmationPolicy: 'binding_feedback_required',
          sourceGroupName: 'gripper', sourceCoordinateName: 'finger_joint',
          sourceKind: 'model_role', coordinateKind: 'linear_m', unit: 'm',
          minPosition: 0, maxPosition: 0.04,
          openStateRef: 'open', closedStateRef: 'closed', commandDurationS: 0.8,
          touchLinks: ['finger_link'], mountFrame: 'tool0', attachFrame: 'tool0',
          tcpFrame: 'tool0', auxiliaryFrames: [], mimicFollowers: [],
          namedPresets: [
            { name: 'closed', masterPosition: 0, coordinates: [] },
            { name: 'open', masterPosition: 0.04, coordinates: [] },
          ],
          contracts: {
            operationCapabilityId: 'tool.gripper.command',
            operationCapabilityDefinitionRevision: digest,
            commandSchema: { commandSchemaId: 'gripper.command_position', commandSchemaRevision: digest },
            controlPath: null, supportingProviders: [],
          },
        }]}
      />,
    );

    expect(markup).toContain('Deployment-owned tool inputs');
    expect(markup).toContain('Native test gripper');
    expect(markup).toContain('goal fenced target reached');
    expect(markup).toContain('binding feedback required');
    expect(markup).not.toContain('Update gripper Draft');
    expect(markup).toContain('finger_link');
    // Streams show their freshness budget only (no severity or recovery).
    expect(markup).toContain('<legend>Gripper feedback</legend><p class="muted">Default: 1000 ms</p>');
  });
});

describe('Grasp confirmation on the gripper settings', () => {
  const rowPath = ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0];
  const graspPath = [...rowPath, 'grasp_confirmation'];
  type InputProps = Props & { onFocus?: (event: unknown) => void; onBlur?: () => void; onKeyDown?: (event: unknown) => void };
  const windowInput = (mounted: Awaited<ReturnType<typeof mountGripper>>, label: string) => (
    (mounted.field(label).children as Array<{ props: InputProps }>)[0].props
  );
  // Types one limit, then leaves the field (blur commits, as in NumberDraft).
  const typeLimit = async (mounted: Awaited<ReturnType<typeof mountGripper>>, label: string, value: string) => {
    await act(() => {
      const input = windowInput(mounted, label);
      input.onFocus?.({ target: { value } });
      input.onChange?.({ target: { value } });
    });
    await act(() => windowInput(mounted, label).onBlur?.());
  };
  const setWindow = async (mounted: Awaited<ReturnType<typeof mountGripper>>, min: string, max: string) => {
    await typeLimit(mounted, 'Stall window min', min);
    await typeLimit(mounted, 'Stall window max', max);
  };
  const setPolicy = (mounted: Awaited<ReturnType<typeof mountGripper>>, policy: string) => act(() => (
    mounted.field('Grasp confirmation').onChange?.({ target: { value: policy } })
  ));
  const alert = (mounted: Awaited<ReturnType<typeof mountGripper>>) => (
    mounted.find((props) => props.role === 'alert').children
  );
  const noAlert = (mounted: Awaited<ReturnType<typeof mountGripper>>) => (
    expect(() => mounted.find((props) => props.role === 'alert')).toThrow('Mounted control not found')
  );

  it('has no Update or Discard button', async () => {
    const mounted = await mountGripper('position_commanded');
    try {
      expect(() => mounted.find((props) => props.children === 'Update grasp confirmation'))
        .toThrow('Mounted control not found');
      expect(() => mounted.find((props) => props.children === 'Discard grasp confirmation edits'))
        .toThrow('Mounted control not found');
    } finally { await mounted.unmount(); }
  });

  it('clears an invalid stall window on its scoped reset without blurring', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      const input = windowInput(mounted, 'Stall window min');
      const element = mounted.findElement((props) => props === input);
      (element.focus as () => void)();
      await act(() => windowInput(mounted, 'Stall window min').onChange?.({ target: { value: '0.04' } }));
      await act(() => windowInput(mounted, 'Stall window min').onBlur?.({}));
      expect(windowInput(mounted, 'Stall window min').value).toBe('0.04');
      expect(windowInput(mounted, 'Stall window min')['aria-invalid']).toBe(true);
      expect(alert(mounted)).toBe('The stall window minimum must not exceed its maximum.');
      expect(mounted.value.mutate).not.toHaveBeenCalled();

      // A no-op/failed reset and a Program-scoped reset leave the Deployment
      // form and its validation alone while the backing value is unchanged.
      await mounted.rerender();
      await mounted.reset('program');
      expect(windowInput(mounted, 'Stall window min').value).toBe('0.04');
      expect(alert(mounted)).toBe('The stall window minimum must not exceed its maximum.');
      expect(mounted.findElement((props) => props === windowInput(mounted, 'Stall window min'))).toBe(element);
      expect((element.ownerDocument as FakeNode).activeElement).toBe(element);

      await mounted.reset('deployment');
      expect(windowInput(mounted, 'Stall window min').value).toBe('0.005');
      expect(windowInput(mounted, 'Stall window min')['aria-invalid']).not.toBe(true);
      noAlert(mounted);
      expect(mounted.findElement((props) => props === windowInput(mounted, 'Stall window min'))).toBe(element);
      expect((element.ownerDocument as FakeNode).activeElement).toBe(element);
      expect(mounted.value.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('writes Required on a controlled gripper once its window is complete, and not before', async () => {
    const mounted = await mountGripper('position_commanded');
    try {
      expect(mounted.field('Grasp confirmation').value).toBe('not_required');
      await setPolicy(mounted, 'required');
      // Incomplete: local validation, no Draft edit.
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(alert(mounted)).toBe('Required needs a stall window: the finished finger positions that count as a grasp.');
      await typeLimit(mounted, 'Stall window min', '0.005');
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(alert(mounted)).toBe('Enter both stall window limits as numbers.');
      await typeLimit(mounted, 'Stall window max', '0.03');
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(graspPath, 'set', {
        policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 },
      });
      noAlert(mounted);
    } finally { await mounted.unmount(); }
  });

  it('commits a window limit on Enter like the other number fields', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      await act(() => {
        const input = windowInput(mounted, 'Stall window max');
        input.onFocus?.({ target: { value: '0.02' } });
        input.onChange?.({ target: { value: '0.02' } });
      });
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      const blur = vi.fn(() => windowInput(mounted, 'Stall window max').onBlur?.());
      await act(() => windowInput(mounted, 'Stall window max').onKeyDown?.({ key: 'Enter', currentTarget: { blur } }));
      expect(blur).toHaveBeenCalledOnce();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(graspPath, 'set', {
        policy: 'required', stall_window: { min_position: 0.005, max_position: 0.02 },
      });
    } finally { await mounted.unmount(); }
  });

  it('Escape restores the Draft window without a Draft edit', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      await act(() => {
        const input = windowInput(mounted, 'Stall window min');
        input.onFocus?.({ target: { value: '' } });
        input.onChange?.({ target: { value: '' } });
      });
      await act(() => windowInput(mounted, 'Stall window min').onKeyDown?.({ key: 'Escape', currentTarget: { blur: vi.fn() } }));
      expect(windowInput(mounted, 'Stall window min').value).toBe('0.005');
      await act(() => windowInput(mounted, 'Stall window min').onBlur?.());
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      noAlert(mounted);
    } finally { await mounted.unmount(); }
  });

  it('writes nothing when the value equals the Draft', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      await act(() => windowInput(mounted, 'Stall window min').onBlur?.());
      await setPolicy(mounted, 'required');
      expect(mounted.value.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('turns Required off at once: keeps a window as not_required, and removes the field without one', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      expect(mounted.field('Grasp confirmation').value).toBe('required');
      expect(windowInput(mounted, 'Stall window min').value).toBe('0.005');
      await setPolicy(mounted, 'not_required');
      expect(mounted.value.mutate).toHaveBeenLastCalledWith(graspPath, 'set', {
        policy: 'not_required', stall_window: { min_position: 0.005, max_position: 0.03 },
      });
      // Clearing one limit is incomplete; clearing both removes the field.
      await typeLimit(mounted, 'Stall window min', '');
      expect(mounted.value.mutate).toHaveBeenCalledTimes(1);
      expect(alert(mounted)).toBe('Enter both stall window limits as numbers.');
      await typeLimit(mounted, 'Stall window max', '');
      expect(mounted.value.mutate).toHaveBeenLastCalledWith(graspPath, 'remove');
      expect(mounted.value.mutate).toHaveBeenCalledTimes(2);
    } finally { await mounted.unmount(); }
  });

  it('selecting Off always writes Off, dropping an incomplete or invalid window', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    for (const [min, max] of [['', '0.03'], ['0.04', '0.005']]) {
      const mounted = await mountGripper('position_commanded', 'commanded', undefined,
        { graspConfirmation: authored });
      try {
        await setWindow(mounted, min, max);
        expect(mounted.value.mutate).not.toHaveBeenCalled();
        await setPolicy(mounted, 'not_required');
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(graspPath, 'remove');
        expect(mounted.field('Grasp confirmation').value).toBe('not_required');
        expect(windowInput(mounted, 'Stall window min').value).toBe('');
        expect(windowInput(mounted, 'Stall window max').value).toBe('');
        noAlert(mounted);
      } finally { await mounted.unmount(); }
    }
  });

  it.each([
    ['', '', 'Required needs a stall window: the finished finger positions that count as a grasp.'],
    ['0.01', '', 'Enter both stall window limits as numbers.'],
    ['0.03', '0.005', 'The stall window minimum must not exceed its maximum.'],
  ])('writes nothing for the stall window min %j max %j and shows the local validation', async (min, max, message) => {
    const mounted = await mountGripper('position_commanded');
    try {
      await setPolicy(mounted, 'required');
      await setWindow(mounted, min, max);
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(alert(mounted)).toBe(message);
      expect(windowInput(mounted, 'Stall window min')['aria-invalid']).toBe(true);
    } finally { await mounted.unmount(); }
  });

  it('leaves an owner refusal to the yellow Deployment notice, with no red inline repeat', async () => {
    const mounted = await mountGripper('position_commanded');
    try {
      (mounted.value.mutate as ReturnType<typeof vi.fn>).mockResolvedValueOnce(null);
      await setPolicy(mounted, 'required');
      await setWindow(mounted, '0.005', '0.03');
      expect(mounted.value.mutate).toHaveBeenCalledTimes(1);
      noAlert(mounted);
    } finally { await mounted.unmount(); }
  });

  it('writes Required on a pneumatic gripper on the select, with the warning when no input is mapped', async () => {
    const io: JsonValue = {
      outputs: [{
        signal: { controllable_component_id: 'valves', signal_name: 'open' },
        open_value: true, close_value: false,
      }],
    };
    const mounted = await mountGripper('pneumatic', 'commanded', io);
    const warning = () => mounted.find((props) => props.className === 'inline-message severity-warning');
    try {
      expect(() => mounted.field('Stall window min')).toThrow('Mounted control not found');
      expect(warning).toThrow('Mounted control not found');
      await setPolicy(mounted, 'required');
      expect(String(warning().children)).toContain('Required needs a mapped Workpiece grasped input');
      // A warning, not a block: the owner names the missing source.
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(graspPath, 'set', { policy: 'required' });
    } finally { await mounted.unmount(); }
  });

  it('writes Off on a pneumatic gripper on the select, removing the field', async () => {
    const mounted = await mountGripper('pneumatic', 'commanded', undefined, {
      graspConfirmation: { policy: 'required' },
    });
    try {
      await setPolicy(mounted, 'not_required');
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(graspPath, 'remove');
    } finally { await mounted.unmount(); }
  });

  it('names the mapped workpiece_grasped input and shows no warning', async () => {
    const io: JsonValue = {
      outputs: [{
        signal: { controllable_component_id: 'valves', signal_name: 'open' },
        open_value: true, close_value: false,
      }],
      inputs: { workpiece_grasped: { signal: { controllable_component_id: 'sensors', signal_name: 'part' } } },
    };
    const mounted = await mountGripper('pneumatic', 'commanded', io, {
      graspConfirmation: { policy: 'required' },
    });
    try {
      expect(mounted.field('Grasp confirmation').value).toBe('required');
      expect(() => mounted.find((props) => props.className === 'inline-message severity-warning'))
        .toThrow('Mounted control not found');
      const help = mounted.find((props) => props.className === 'field-help' && Array.isArray(props.children)
        && String(props.children[0]).startsWith('Pneumatic gripper:'));
      expect(help.children).toContain('Pneumatic gripper: the evidence is the Workpiece grasped input, sensors · part. ');
    } finally { await mounted.unmount(); }
  });

  it('writes a valid gripper field directly without resubmitting the row', async () => {
    const authored = { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } };
    const mounted = await mountGripper('position_commanded', 'commanded', undefined,
      { graspConfirmation: authored });
    try {
      await mounted.setDuration(0.25);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        [...rowPath, 'command_duration_s'], 'set', 0.25,
      );
    } finally { await mounted.unmount(); }
  });

  it('writes the nested row of a mounted tool in a Scenario Draft', async () => {
    const mounted = await mountGripper('pneumatic', 'commanded', undefined, { scenarioTool: true });
    try {
      await setPolicy(mounted, 'required');
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration', 'gripper_target_configurations', 0,
          'grasp_confirmation'],
        'set', { policy: 'required' },
      );
    } finally { await mounted.unmount(); }
  });

  it('offers the setting for an authored row before Robot Control addresses it', () => {
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={rosGripperController('position_commanded')}
      scope={{ componentId: 'robot' }} sourceModel={{ status: 'loading' }} motionDescriptors={[]}
      gripperDescriptors={[]} />);
    expect(markup).toContain('No current addressed gripper descriptor is available.');
    expect(markup).toContain('Grasp confirmation · robot / gripper');
    expect(markup).toContain('Stall window min');
  });
});
