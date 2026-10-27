import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ComponentStreamsSection } from './ComponentStreamsSection';

afterEach(() => vi.unstubAllGlobals());

const robotStreams = [
  { budget_ns: 1_000_000_000, controllable_component_id: 'robot', recovery: 'ack', severity: 'fault', stream_id: 'gripper_feedback' },
  { budget_ns: 250_000_000, controllable_component_id: 'robot', recovery: 'auto', severity: 'warning', stream_id: 'measured_joints' },
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
  const control = (label: string, kind: 'budget' | 'severity' | 'recovery', component = 'robot') => find(
    (props) => props['aria-label'] === `${label} ${kind} override for ${component}`);
  const commitBudget = async (label: string, text: string) => {
    await act(() => control(label, 'budget').onChange?.({ target: { value: text } }));
    await act(() => (control(label, 'budget').onBlur as () => void)());
  };
  const select = (label: string, kind: 'severity' | 'recovery', value: string, component = 'robot') => act(
    () => control(label, kind, component).onChange?.({ target: { value } }));
  return { find, control, commitBudget, select, unmount: () => act(() => root.unmount()) };
}

const policies = (streams?: JsonValue[]) => ({
  interrupt_rules: [], scene_pose_sources: partLink,
  ...(streams === undefined ? {} : { component_streams: streams }),
});

describe('ComponentStreamsSection', () => {
  it('lists every component with its declared streams and defaults', () => {
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controllerWith()} />);
    expect(markup).toContain('Measured joints');
    expect(markup).toContain('Default: 250 ms · warning · auto');
    expect(markup).toContain('Gripper feedback');
    expect(markup).toContain('Default: 1000 ms · fault · ack');
    expect(markup).toContain('placeholder="1000 ms (default)"');
    expect(markup).toContain('Default (warning)');
    expect(markup).toContain('Pose robot / viewer.measured_robot');
    expect(markup).toContain('Pose part');
    expect(markup).toContain('Default: 5000 ms · warning · auto');
    expect(markup).toMatch(/<strong>io<\/strong><\/header><p class="muted">No streams declared<\/p>/);
    expect(markup).toContain('Model endpoint');
    expect(markup).toContain('Default: 10000 ms · warning · auto');
  });

  it('sets and clears a budget override in ns', async () => {
    const controller = controllerWith();
    const mounted = await mount(controller);
    try {
      expect(mounted.control('Measured joints', 'budget').value).toBe('');
      await mounted.commitBudget('Measured joints', '500');
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policies([
        { controllable_component_id: 'robot', stream_id: 'measured_joints', budget_ns: 500_000_000 },
      ]));
    } finally { await mounted.unmount(); }
  });

  it('keeps other rows and fields, removes an emptied row and the empty table', async () => {
    const other = { controllable_component_id: 'camera', stream_id: 'poses/part', severity: 'blocking' };
    const own = { controllable_component_id: 'robot', stream_id: 'gripper_feedback', budget_ns: 1_500_000_000, severity: 'blocking' };
    const controller = controllerWith([other, own]);
    const mounted = await mount(controller);
    try {
      expect(mounted.control('Gripper feedback', 'budget').value).toBe('1500');
      expect(mounted.control('Gripper feedback', 'severity').value).toBe('blocking');
      await mounted.commitBudget('Gripper feedback', '');
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies([
        other, { controllable_component_id: 'robot', stream_id: 'gripper_feedback', severity: 'blocking' },
      ]));
      await mounted.select('Gripper feedback', 'recovery', 'ack');
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies([
        other, { ...own, recovery: 'ack' },
      ]));
      await mounted.select('Pose part', 'severity', '', 'camera');
      // The camera override is identity-only after clearing: removed.
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies([own]));
    } finally { await mounted.unmount(); }
  });

  it('deletes the table when the last override is cleared', async () => {
    const controller = controllerWith([
      { controllable_component_id: 'robot', stream_id: 'measured_joints', severity: 'fault' },
    ]);
    const mounted = await mount(controller);
    try {
      await mounted.select('Measured joints', 'severity', '');
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policies());
    } finally { await mounted.unmount(); }
  });

  it('writes fault with auto recovery like any other override', async () => {
    const controller = controllerWith([
      { controllable_component_id: 'robot', stream_id: 'measured_joints', recovery: 'auto' },
    ]);
    const mounted = await mount(controller);
    try {
      await mounted.select('Measured joints', 'severity', 'fault');
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policies([
        { controllable_component_id: 'robot', stream_id: 'measured_joints', recovery: 'auto', severity: 'fault' },
      ]));
    } finally { await mounted.unmount(); }
  });

  const absentComponent = { controllable_component_id: 'gone', stream_id: 'measured_joints', severity: 'blocking' };
  const undeclaredStream = { controllable_component_id: 'robot', stream_id: 'old_stream', budget_ns: 1_500_000_000, recovery: 'ack' };
  const declared = { controllable_component_id: 'robot', stream_id: 'measured_joints', severity: 'fault' };

  it('shows override rows without a declared stream', () => {
    const markup = renderToStaticMarkup(<ComponentStreamsSection controller={controllerWith([
      absentComponent, declared, undeclaredStream,
    ])} />);
    expect(markup).toContain('Overrides without a declared stream');
    expect(markup).toContain('gone · measured_joints: severity blocking');
    expect(markup).toContain('robot · old_stream: budget 1500 ms · recovery ack');
    expect(markup).not.toContain('robot · measured_joints');
  });

  it('removes an orphan override row, and the empty table with the last one', async () => {
    const controller = controllerWith([absentComponent, declared, undeclaredStream]);
    const mounted = await mount(controller);
    try {
      await act(() => mounted.find(
        (props) => props['aria-label'] === 'Remove old_stream override for robot').onClick?.());
      expect(controller.mutate).toHaveBeenLastCalledWith(['policies'], 'set', policies([
        absentComponent, declared,
      ]));
    } finally { await mounted.unmount(); }
    const last = controllerWith([absentComponent]);
    const lastMounted = await mount(last);
    try {
      await act(() => lastMounted.find(
        (props) => props['aria-label'] === 'Remove measured_joints override for gone').onClick?.());
      expect(last.mutate).toHaveBeenCalledExactlyOnceWith(['policies'], 'set', policies());
    } finally { await lastMounted.unmount(); }
  });

  it.each(['0', '-5'])('rejects non-positive budget %s before mutation', async (text) => {
    const controller = controllerWith();
    const mounted = await mount(controller);
    try {
      await mounted.commitBudget('Measured joints', text);
      expect(controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('reports an owner rejection', async () => {
    const controller = controllerWith(undefined, null);
    const mounted = await mount(controller);
    try {
      await mounted.select('Measured joints', 'recovery', 'ack');
      // Let the owner's refusal settle.
      await act(() => new Promise((resolve) => { setTimeout(resolve, 0); }));
      expect(controller.mutate).toHaveBeenCalledOnce();
      expect(mounted.find((props) => props.role === 'alert').children).toBe('Value was rejected.');
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
    expect(markup).toContain('Measured joints');
  });
});
