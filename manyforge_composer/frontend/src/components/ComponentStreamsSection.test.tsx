import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ComponentStreamsSection } from './ComponentStreamsSection';

afterEach(() => vi.unstubAllGlobals());

const robotStreams = [
  { budget_ns: 1_000_000_000, controllable_component_id: 'robot', stream_id: 'gripper_feedback' },
  { budget_ns: 250_000_000, controllable_component_id: 'robot', stream_id: 'measured_joints' },
];

const partLink = [{ scene_resource_id: 'part', source_id: 'camera', apply_policy: 'before_planning',
  source_key: { detection3d: { class_id: 'part' } } }];

function controllerWith(overrides?: JsonValue[], result: unknown = {}) {
  const document = {
    api_version: 'manyforge.deployment_intent.v2',
    components: [
      { component_id: 'robot', configuration: { runtime_kind: 'ros2_control',
        runtime_configuration: { component_streams: robotStreams } } },
      { component_id: 'camera', configuration: { runtime_kind: 'pose_source', runtime_configuration: {
        stream_budget_ns: 5_000_000_000,
        detection3d: { topic: '/detections', target_frame_id: 'world', fallback_frame_id: 'camera' },
        robot_overlay: { robot_component_id: 'robot',
          joint_state: { topic: '/joint_states', required_joint_names: ['joint_1'] } } } } },
      { component_id: 'io', configuration: { runtime_kind: 'io_provider', runtime_configuration: {} } },
      { component_id: 'assistant_model', configuration: { runtime_kind: 'ai_model', runtime_configuration: {
        base_url: 'http://127.0.0.1:8000/v1', model: 'm', api_key_env: null } } },
    ],
    policies: {
      interrupt_rules: [],
      scene_pose_sources: partLink,
      ...(overrides === undefined ? {} : { component_streams: overrides }),
    },
  };
  return {
    workspace: { draft: { document } },
    mutationAvailable: true, busy: false, loading: false,
    mutate: vi.fn().mockResolvedValue(result),
  } as unknown as DeploymentWorkspaceController;
}

async function mount(controller: DeploymentWorkspaceController) {
  const { host, find } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ComponentStreamsSection controller={controller} />));
  const control = (label: string, component = 'robot') => find(
    (props) => props['aria-label'] === `${label} budget override for ${component}`);
  const commitBudget = async (label: string, text: string, component = 'robot') => {
    await act(() => control(label, component).onChange?.({ target: { value: text } }));
    await act(() => (control(label, component).onBlur as () => void)());
  };
  return { find, control, commitBudget, unmount: () => act(() => root.unmount()) };
}

const policies = (streams?: JsonValue[]) => ({
  interrupt_rules: [], scene_pose_sources: partLink,
  ...(streams === undefined ? {} : { component_streams: streams }),
});

describe('ComponentStreamsSection', () => {
  it('lists every component with its declared streams and budget defaults', () => {
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controllerWith()} />);
    expect(markup).toContain('<legend>Measured joints</legend>');
    expect(markup).toContain('Default: 250 ms');
    expect(markup).toContain('<legend>Gripper feedback</legend>');
    expect(markup).toContain('placeholder="1000 ms (default)"');
    expect(markup).toContain('<legend>Pose robot / viewer.measured_robot</legend>');
    expect(markup).toContain('<legend>Pose part</legend>');
    expect(markup).toContain('Default: 5000 ms');
    expect(markup).toMatch(/<strong>io<\/strong><\/header><p class="muted">No streams declared<\/p>/);
    expect(markup).toContain('<legend>Model endpoint</legend>');
    expect(markup).toContain('Default: 10000 ms');
    expect(markup).not.toContain('severity');
    expect(markup).not.toContain('Recovery');
    // CONTRACTS §7: what a stale stream means is the component kind's.
    expect(markup).toContain('own stream going stale is a fault');
    expect(markup).toContain('In a simulator lane');
  });

  it('sets and clears a budget override in ns', async () => {
    const own = { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 1_500_000_000 };
    const controller = controllerWith([own]);
    const mounted = await mount(controller);
    try {
      expect(mounted.control('Gripper feedback').value).toBe('1500');
      await mounted.commitBudget('Measured joints', '500');
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies([
        own, { controllable_component_id: 'robot', stream_id: 'measured_joints', budget_ns: 500_000_000 },
      ]));
      await mounted.commitBudget('Gripper feedback', '');
      // The emptied row and then the empty table go.
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies());
    } finally { await mounted.unmount(); }
  });

  const undeclaredStream = { controllable_component_id: 'robot', stream_id: 'old_stream', budget_ns: 1_500_000_000 };

  it('shows and removes override rows without a declared stream', async () => {
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controllerWith([undeclaredStream])} />);
    expect(markup).toContain('Overrides without a declared stream');
    expect(markup).toContain('robot · old_stream: budget 1500 ms');
    const controller = controllerWith([undeclaredStream]);
    const mounted = await mount(controller);
    try {
      await act(() => mounted.find(
        (props) => props['aria-label'] === 'Remove old_stream override for robot').onClick?.());
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policies());
    } finally { await mounted.unmount(); }
  });

  it.each(['0', '-5'])('rejects non-positive budget %s before mutation', async (text) => {
    const controller = controllerWith();
    const mounted = await mount(controller);
    try {
      await mounted.commitBudget('Measured joints', text);
      expect(controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('offers the Inputs stream of an I/O device with ROS inputs, off by default', async () => {
    const ioSignal = (component: string, profile: string) => ({
      controllable_component_id: component, direction: 'input', signal_name: 'ready',
      provider: { profile, topic: `/${component}/ready` } });
    const controller = {
      workspace: { draft: { document: { api_version: 'manyforge.deployment_intent.v2', components: [
        { component_id: 'plc', configuration: { runtime_kind: 'io_provider',
          runtime_configuration: { io_signals: [ioSignal('plc', 'ros_bool')] } } },
        { component_id: 'station', configuration: { runtime_kind: 'io_provider',
          runtime_configuration: { io_signals: [ioSignal('station', 'mock')] } } },
      ], policies: {} } } },
      mutationAvailable: true, busy: false, loading: false,
      mutate: vi.fn().mockResolvedValue({}),
    } as unknown as DeploymentWorkspaceController;
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controller} />);
    expect(markup).toContain('<legend>Inputs</legend>');
    expect(markup).toContain('Default: off');
    expect(markup).toContain('placeholder="off (default)"');
    // Mock inputs have no sender: no Inputs stream.
    expect(markup).toMatch(/<strong>station<\/strong><\/header><p class="muted">No streams declared<\/p>/);
    const mounted = await mount(controller);
    try {
      await mounted.commitBudget('Inputs', '2000', 'plc');
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', { component_streams: [
        { controllable_component_id: 'plc', stream_id: 'inputs', budget_ns: 2_000_000_000 }] });
    } finally { await mounted.unmount(); }
  });

  it('finds graph-form components in unit rows', () => {
    const controller = {
      workspace: { draft: { document: { graph: { runtime_units: [{ runtime_unit_id: 'unit',
        configuration: { gripper_target_configurations: [{ controllable_component_id: 'tool' }],
          component_streams: robotStreams } }] } } } },
      mutationAvailable: true, busy: false, loading: false, mutate: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controller} />);
    expect(markup).toContain('<strong>tool</strong></header><p class="muted">No streams declared</p>');
    expect(markup).toContain('<strong>robot</strong>');
    expect(markup).toContain('<legend>Measured joints</legend>');
  });
});
