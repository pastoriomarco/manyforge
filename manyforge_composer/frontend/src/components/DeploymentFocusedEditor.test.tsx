import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue, SourceModelProjection } from '../types/api';
import type { ManualControlGripperDescriptor } from '../types/robotControl';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
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
              runtime_unit_id: 'robot.native_execution',
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
    loading: false,
    busy: false,
    applying: false,
    error: null,
    feedback: null,
    acceptedApply: null,
    acceptedApplyCorrelationId: null,
    activationScope: null,
    pendingApplyConfirmation: null,
    pendingConfirmationAction: null,
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
    confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  };
}

afterEach(() => vi.unstubAllGlobals());

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
  configuration.controller_profiles = [{
    controllable_component_id: 'robot', mock_profile: 'lite6_lite_gripper',
  }];
  const row = (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0];
  row.output_mode = outputMode;
  row.state_freshness_timeout_s = 0.75;
  return value;
}

// Use the existing mounted-test pattern: real React state and handlers on a
// minimal DOM host, without introducing another browser/test dependency.
type FakeNode = Record<string, unknown>;
type Props = Record<string, unknown> & {
  onChange?: (event: unknown) => void;
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
  const find = (match: (props: Props) => boolean) => {
    const found = elements.map((element) => element[
      Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
    ] as Props | undefined).find((props) => props !== undefined && match(props));
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find };
}

async function mountGripper(
  actuationKind: 'position_commanded' | 'pneumatic',
  outputMode: 'commanded' | 'none' = 'commanded',
  timeoutSource?: JsonValue,
  componentStreams?: JsonValue,
) {
  const value = rosGripperController(actuationKind, outputMode);
  if (componentStreams !== undefined) {
    (value.workspace!.draft.document!.policies as Record<string, JsonValue>).component_streams = componentStreams;
  }
  const graph = value.workspace!.draft.document!.graph as Record<string, JsonValue>;
  const runtime = (graph.runtime_units as Array<Record<string, JsonValue>>)[0];
  const configuration = runtime.configuration as Record<string, JsonValue>;
  const sourceRow = (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0];
  if (timeoutSource !== undefined) sourceRow.delivery_result_timeout_s = timeoutSource;
  vi.mocked(value.mutate).mockResolvedValue(value.workspace);
  const descriptor: ManualControlGripperDescriptor = {
    ...statusFixture().directControlGripper!.targetDescriptor,
    controllableComponentId: 'robot', actuationKind, outputMode,
    completionPolicy: actuationKind === 'pneumatic'
      ? outputMode === 'none' ? 'no_output' : 'output_delivery'
      : 'goal_fenced_target_reached',
    graspConfirmationPolicy: actuationKind === 'pneumatic' ? 'not_required' : 'binding_feedback_required',
    namedPresets: [
      { name: 'closed', masterPosition: 0, coordinates: [] },
      { name: 'open', masterPosition: 0.04, coordinates: [] },
    ],
  };
  const { host, find } = mountedHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<DeploymentFocusedEditor controller={value}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[descriptor]} />));
  const field = (label: string) => {
    const row = find((props) => props.className === 'deployment-focused-field' && Array.isArray(props.children)
      && (props.children[0] as { props?: Props })?.props?.children === label);
    return (row.children as Array<{ props: Props }>)[1].props;
  };
  const duration = () => (field(field('Actuation kind').value === 'pneumatic'
    ? 'Program dwell' : 'Motion time').children as Array<{ props: Props }>)[0].props;
  const setDuration = (durationS: number) => act(() => duration().onChange?.({
    target: { value: String(durationS) },
  }));
  const update = () => act(() => {
    find((props) => props.children === 'Update gripper Draft').onClick?.();
  });
  return { value, sourceRow, find, field, duration, setDuration, update,
    unmount: () => act(() => root.unmount()) };
}

describe('DeploymentFocusedEditor', () => {
  it('offers a retained gripper contact endpoint when arm motion endpoints are also present', async () => {
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
        profile: 'geometric_contact', ingress_depth: 64,
        endpoint: {
          controllable_component_id: 'robot', command_target_id: 'arm', frame_role_id: 'tcp',
        },
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
      const endpoint = find((props) => props['aria-label'] === 'contact Contact endpoint');
      const gripperValue = JSON.stringify(['robot', 'gripper', 'tcp']);
      expect(find((props) => props.value === gripperValue)).toBeDefined();
      await act(() => endpoint.onChange?.({ target: { value: gripperValue } }));
      await act(() => find((props) => props.children === 'Update I/O Draft').onClick?.());
      expect(value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [expect.objectContaining({ provider: expect.objectContaining({ endpoint: {
          controllable_component_id: 'robot', command_target_id: 'gripper', frame_role_id: 'tcp',
        } }) })],
      );
    } finally {
      await act(() => root.unmount());
    }
  });

  describe('gripper feedback timeout', () => {
    const feedbackInput = (mounted: Awaited<ReturnType<typeof mountGripper>>) => (
      mounted.field('Feedback timeout').children as Array<{ props: Props }>)[0].props;
    const commit = async (mounted: Awaited<ReturnType<typeof mountGripper>>, text: string) => {
      await act(() => feedbackInput(mounted).onChange?.({ target: { value: text } }));
      await act(() => (feedbackInput(mounted).onBlur as () => void)());
    };
    const policiesWith = (mounted: Awaited<ReturnType<typeof mountGripper>>, streams?: JsonValue) => {
      const policies = { ...(mounted.value.workspace!.draft.document!.policies as Record<string, JsonValue>) };
      if (streams === undefined) delete policies.component_streams;
      else policies.component_streams = streams;
      return policies;
    };
    const other = { controllable_component_id: 'camera', stream_id: 'frames', severity: 'warning' };

    it('adds a budget override in ns and shows the profile default as placeholder', async () => {
      const mounted = await mountGripper('position_commanded');
      try {
        expect(feedbackInput(mounted).value).toBe('');
        expect(feedbackInput(mounted).placeholder).toBe('750 ms (profile default)');
        await commit(mounted, '250');
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policiesWith(mounted, [
          { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 250_000_000 },
        ]));
      } finally { await mounted.unmount(); }
    });

    it('updates the override and keeps other rows and fields', async () => {
      const mounted = await mountGripper('position_commanded', 'commanded', undefined, [
        other,
        { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 1_500_000_000, severity: 'blocking' },
      ]);
      try {
        expect(feedbackInput(mounted).value).toBe('1500');
        await commit(mounted, '2000');
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policiesWith(mounted, [
          other,
          { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 2_000_000_000, severity: 'blocking' },
        ]));
        await commit(mounted, '');
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policiesWith(mounted, [
          other,
          { controllable_component_id: 'robot', stream_id: 'gripper_feedback', severity: 'blocking' },
        ]));
      } finally { await mounted.unmount(); }
    });

    it('clearing a budget-only override removes the row and the empty table', async () => {
      const mounted = await mountGripper('position_commanded', 'commanded', undefined, [
        { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 1_000_000_000 },
      ]);
      try {
        await commit(mounted, '');
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policiesWith(mounted));
      } finally { await mounted.unmount(); }
    });

    it.each(['0', '-5'])('rejects non-positive timeout %s before mutation', async (text) => {
      const mounted = await mountGripper('position_commanded');
      try {
        await commit(mounted, text);
        expect(mounted.value.mutate).not.toHaveBeenCalled();
      } finally { await mounted.unmount(); }
    });
  });

  it.each(['commanded', 'none'] as const)(
    'preserves explicit pneumatic timeout through edits and %s output changes', async (mode) => {
      const mounted = await mountGripper('pneumatic', mode, 2.5);
      try {
        expect(mounted.field('Program dwell')).toBeDefined();
        expect(() => mounted.field('Motion time')).toThrow('Mounted control not found');
        expect(() => mounted.field('Delivery result timeout')).toThrow('Mounted control not found');
        await mounted.setDuration(0.25);
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
          'set', { ...mounted.sourceRow, command_duration_s: 0.25 },
        );
        const otherMode = mode === 'none' ? 'commanded' : 'none';
        await act(() => mounted.field('Output').onChange?.({ target: { value: otherMode } }));
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenLastCalledWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
          'set', { ...mounted.sourceRow, output_mode: otherMode, command_duration_s: 0.25 },
        );
      } finally { await mounted.unmount(); }
    },
  );

  it.each(['position_commanded', 'pneumatic'] as const)(
    'retains timeout omission and the existing row fields for %s', async (kind) => {
      const mounted = await mountGripper(kind);
      try {
        expect(Object.keys(mounted.sourceRow)).toHaveLength(14);
        await mounted.setDuration(0.25);
        await mounted.update();
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
          'set', { ...mounted.sourceRow, command_duration_s: 0.25 },
        );
      } finally { await mounted.unmount(); }
    },
  );

  it('drops the pneumatic-only timeout on an intentional switch to position', async () => {
    const mounted = await mountGripper('pneumatic', 'commanded', 2.5);
    try {
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.field('Motion time')).toBeDefined();
      await mounted.update();
      const positionRow = { ...mounted.sourceRow };
      delete positionRow.delivery_result_timeout_s;
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', { ...positionRow, actuation_kind: 'position_commanded' },
      );
    } finally { await mounted.unmount(); }
  });

  it.each([0, -1, null, '2.5', true, NaN, Infinity, -Infinity])(
    'does not repair an invalid authored pneumatic timeout %s', async (timeout) => {
      const mounted = await mountGripper('pneumatic', 'commanded', timeout);
      try {
        expect(() => mounted.field('Program dwell')).toThrow('Mounted control not found');
        expect(mounted.value.mutate).not.toHaveBeenCalled();
        expect(mounted.sourceRow.delivery_result_timeout_s).toBe(timeout);
      } finally { await mounted.unmount(); }
    },
  );

  it('does not silently strip a foreign timeout from an authored position row', async () => {
    const mounted = await mountGripper('position_commanded', 'commanded', 2.5);
    try {
      expect(() => mounted.field('Motion time')).toThrow('Mounted control not found');
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(mounted.sourceRow.delivery_result_timeout_s).toBe(2.5);
    } finally { await mounted.unmount(); }
  });

  it.each([0, 0.8])('writes explicit pneumatic no-output with dwell %s', async (duration) => {
    const mounted = await mountGripper('pneumatic');
    try {
      expect(mounted.field('Output').value).toBe('commanded');
      await act(() => mounted.field('Output').onChange?.({ target: { value: 'none' } }));
      await mounted.setDuration(duration);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', expect.objectContaining({ output_mode: 'none', actuation_kind: 'pneumatic',
          command_duration_s: duration, open_state_ref: 'open', closed_state_ref: 'closed',
          mount_frame: 'tool0', tcp_frame: 'tool0' }),
      );
      await act(() => mounted.field('Output').onChange?.({ target: { value: 'commanded' } }));
      // Keep the duration changed from the owner's row so the Draft write is requested.
      await mounted.setDuration(0.1);
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenLastCalledWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', expect.objectContaining({ output_mode: 'commanded', command_duration_s: 0.1 }),
      );
    } finally { await mounted.unmount(); }
  });

  it('requires explicit commanded output after changing pneumatic-none to position', async () => {
    const mounted = await mountGripper('pneumatic', 'none');
    try {
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.field('Output').value).toBe('none');
      await mounted.update();
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      expect(mounted.find((props) => props.role === 'alert').children)
        .toBe('No output is available only for pneumatic grippers.');
      await act(() => mounted.field('Output').onChange?.({ target: { value: 'commanded' } }));
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', expect.objectContaining({ output_mode: 'commanded', actuation_kind: 'position_commanded' }),
      );
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
      expect(mounted.duration().min).toBe(kind === 'pneumatic' ? '0' : '0.000001');
      expect(mounted.duration().max).toBe('60');
      await mounted.setDuration(duration);
      await mounted.update();
      if (valid) {
        expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
          ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
          'set',
          expect.objectContaining({ actuation_kind: kind, command_duration_s: duration }),
        );
      } else {
        expect(mounted.value.mutate).not.toHaveBeenCalled();
        expect(mounted.find((props) => props.role === 'alert').children).toContain(
          kind === 'pneumatic' ? 'from 0 to 60 s' : 'above 0 and at most 60 s',
        );
      }
    } finally { await mounted.unmount(); }
  });

  it('revalidates zero dwell when the authored actuation kind changes', async () => {
    const mounted = await mountGripper('pneumatic');
    try {
      await mounted.setDuration(0);
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'position_commanded' } }));
      expect(mounted.duration().min).toBe('0.000001');
      await mounted.update();
      expect(mounted.value.mutate).not.toHaveBeenCalled();
      await act(() => mounted.field('Actuation kind').onChange?.({ target: { value: 'pneumatic' } }));
      expect(mounted.duration().min).toBe('0');
      await mounted.update();
      expect(mounted.value.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0],
        'set', expect.objectContaining({ actuation_kind: 'pneumatic', command_duration_s: 0 }),
      );
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

    expect(markup).toContain('Isaac world');
    expect(markup).toContain('lite6_grf_ros2');
    expect(markup).toContain('Maximum TCP linear speed');
    expect(markup).toContain('value="0.2"');
    expect(markup).toContain('robot · arm');
    expect(markup).toContain('joint1 maximum deceleration');
    expect(markup).toContain('Native runtime configuration');
    expect(markup).not.toContain('TF frame prefix');
    expect(markup).not.toContain('Applying a change restarts this robot holding.');
    expect(markup).toContain('Execution cadence');
    expect(markup).toContain('Controlled Stop jerk limits');
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
    { enabled: false, override: null, imported: null, maximum: '0' },
    { enabled: true, override: 10, imported: 12, maximum: '10' },
    { enabled: true, override: null, imported: 12, maximum: '12' },
    { enabled: true, override: null, imported: null, maximum: null },
  ])('bounds manual jerk from explicit Draft settings: %j', ({ enabled, override, imported, maximum }) => {
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
        ...(imported === null ? {} : { imported: { enabled: true, maximum_rad_s3: imported } }),
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
      sourceJointName: 'joint1', source: 'preset', maximumRadS3: 12, presetRadS3: 12, imported: null,
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
    // Q51: absent, the robot's own deadline is its deceleration time + 0.5 s.
    expect(markup).toContain('Explicit maximum Stop time');
    expect(markup).toContain('Automatic: 1.5 s (deceleration time + 0.5 s).');
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

  it('presents canonical target, native runtime, and normalized Stop inputs', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={controller()}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[{
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v5',
          ordinaryJerk: { enabled: false, presetReferenceTimeS: 1, jointLimits: [{
            sourceJointName: 'joint1', source: 'off', maximumRadS3: null,
            presetRadS3: 12, imported: null,
          }] },
          controllableComponentId: 'robot',
          commandTargetId: 'arm',
          componentRoleId: 'robot',
          targetRoleId: 'arm',
          frames: [],
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
    expect(markup).toContain('Native runtime configuration');
    expect(markup).toContain('Execution cadence');
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

  it('joins one authored gripper row to exact owner semantics', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={rosGripperController('position_commanded')}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[]}
        gripperDescriptors={[{
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v3',
          controllableComponentId: 'robot', commandTargetId: 'gripper',
          componentRoleId: 'robot', targetRoleId: 'gripper',
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
    expect(markup).toContain('Update gripper Draft');
    expect(markup).toContain('finger_link');
  });
});
