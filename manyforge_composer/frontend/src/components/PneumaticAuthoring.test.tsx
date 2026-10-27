import { act, useState } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { declaredPneumaticInputs, pneumaticGripperDrafts } from '../utils/pneumaticGripperAuthoring';
import { declaredIoInputs, declaredIoOutputs } from '../utils/ioSignalConfiguration';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { PneumaticRequiredInputsEditor } from './PneumaticRequiredInputsEditor';
import { pneumaticIoProblem } from './PneumaticIoEditor';

const inputSignal = {
  controllable_component_id: 'sensor_io', command_target_id: 'io', resource_role_id: 'part',
  signal_name: 'part', source_id: 'sensor_source', value_type: 'bool', direction: 'input',
  update_mode: 'change_only', transition_capability: 'sampled_only',
  freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' },
};
const outputSignal = {
  controllable_component_id: 'valve_io', command_target_id: 'io', resource_role_id: 'valve',
  signal_name: 'valve', source_id: 'valve_source', value_type: 'bool', direction: 'output',
  provider: { profile: 'mock_output', channel: 'valve' },
};

function document(pneumaticIo?: JsonValue): Record<string, JsonValue> {
  const gripper = {
    controllable_component_id: 'robot', command_target_id: 'gripper',
    actuation_kind: 'pneumatic', output_mode: 'commanded',
    ...(pneumaticIo === undefined ? {} : { pneumatic_io: pneumaticIo }),
  };
  return {
    api_version: 'manyforge.deployment_intent.v2',
    components: [
      { component_id: 'robot', target_settings: {}, configuration: {
        runtime_kind: 'ros2_control', runtime_configuration: { gripper_target_configurations: [gripper] },
      } },
      { component_id: 'valve_io', target_settings: {}, configuration: {
        runtime_kind: 'io_provider', runtime_configuration: { io_signals: [outputSignal] },
      } },
      { component_id: 'sensor_io', target_settings: {}, configuration: {
        runtime_kind: 'io_provider', runtime_configuration: { io_signals: [inputSignal] },
      } },
    ],
  };
}

const io = {
  outputs: [{ signal: { controllable_component_id: 'valve_io', signal_name: 'valve' }, open_value: false, close_value: true }],
  inputs: { workpiece_grasped: { signal: { controllable_component_id: 'sensor_io', signal_name: 'part' } } },
};

afterEach(() => vi.unstubAllGlobals());

describe('pneumatic gripper authoring', () => {
  it('rejects the same valve output twice before submitting the Draft', () => {
    expect(pneumaticIoProblem({ outputs: [io.outputs[0], io.outputs[0]] })).toContain('different signal');
  });
  it('reads v2 Draft targets and their logical inputs without an Applied binding', () => {
    const rows = pneumaticGripperDrafts(document(io));
    expect(rows[0].path).toEqual(['components', 0, 'configuration', 'runtime_configuration', 'gripper_target_configurations', 0, 'pneumatic_io']);
    expect(declaredPneumaticInputs(document(io), 'robot', 'gripper')).toEqual(['workpiece_grasped']);
    expect(declaredPneumaticInputs(document(io), 'robot', 'other')).toEqual([]);
    expect(declaredIoInputs(document()).map((row) => [row.componentId, row.signalName])).toEqual([['sensor_io', 'part']]);
    expect(declaredIoOutputs(document()).map((row) => [row.componentId, row.signalName])).toEqual([['valve_io', 'valve']]);
    expect(pneumaticGripperDrafts({ graph: { runtime_units: [{ configuration: {
      gripper_target_configurations: [{ controllable_component_id: 'robot', command_target_id: 'gripper', pneumatic_io: io }],
    } }] } })[0].path).toEqual(['graph', 'runtime_units', 0, 'configuration', 'gripper_target_configurations', 0, 'pneumatic_io']);
  });

  it('authors signals from different I/O components in a Draft without any Applied descriptors', async () => {
    const controller = {
      workspace: { draft: { document: document() } }, mutationAvailable: true, busy: false,
      mutate: vi.fn().mockResolvedValue({}), updateMotionTargetLimit: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const click = (label: string) => act(() => find((props) => props.type === 'button' && props.children === label).onClick?.());
    const select = (label: string, value: string) => act(() => find((props) => props['aria-label'] === label).onChange?.({ target: { value } }));
    try {
      await click('Configure pneumatic I/O');
      await click('Update pneumatic I/O Draft');
      expect(controller.mutate).not.toHaveBeenCalled();
      await click('Add valve output');
      await select('Valve output 1 signal', JSON.stringify(['valve_io', 'valve']));
      await select('Valve output 1 open value', 'false');
      await select('Valve output 1 close value', 'true');
      await select('Workpiece grasped input signal', JSON.stringify(['sensor_io', 'part']));
      await click('Update pneumatic I/O Draft');
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 0, 'configuration', 'runtime_configuration', 'gripper_target_configurations', 0, 'pneumatic_io'],
        'set', io,
      );
    } finally { await act(() => root.unmount()); }
  });

  it('preserves local edits across an identical re-fetch and blocks stale submission after a real mapping change', async () => {
    const controller = {
      workspace: { draft: { document: document(io) } }, mutationAvailable: true, busy: false,
      mutate: vi.fn().mockResolvedValue({}), updateMotionTargetLimit: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    const { host, find, all } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const render = () => act(() => root.render(<DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const control = (label: string) => find((props) => props['aria-label'] === label);
    const button = (label: string) => find((props) => props.type === 'button' && props.children === label);
    await render();
    try {
      await act(() => control('Valve output 1 open value').onChange?.({ target: { value: 'true' } }));
      const replacement = document(JSON.parse(JSON.stringify(io)) as JsonValue);
      replacement.name = 'An unrelated Draft change';
      controller.workspace!.draft.document = replacement;
      await render();
      expect(control('Valve output 1 open value').value).toBe('true');
      const external = { ...io, outputs: [{ ...io.outputs[0], close_value: false }] };
      controller.workspace!.draft.document = document(external);
      await render();
      expect(control('Valve output 1 open value').value).toBe('true');
      expect(button('Update pneumatic I/O Draft').disabled).toBe(true);
      expect(all((props) => props.role === 'alert').some((props) => String(props.children).includes('changed in the Deployment Draft'))).toBe(true);
      expect(controller.mutate).not.toHaveBeenCalled();
      await act(() => button('Discard pneumatic I/O edits').onClick?.());
      expect(control('Valve output 1 open value').value).toBe('false');
      expect(control('Valve output 1 close value').value).toBe('false');
    } finally { await act(() => root.unmount()); }
  });

  it('does not apply one gripper\'s local edits to a different gripper at the same row index', async () => {
    const controller = {
      workspace: { draft: { document: document(io) } }, mutationAvailable: true, busy: false,
      mutate: vi.fn().mockResolvedValue({}), updateMotionTargetLimit: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const render = () => act(() => root.render(<DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const control = (label: string) => find((props) => props['aria-label'] === label);
    await render();
    try {
      await act(() => control('Valve output 1 open value').onChange?.({ target: { value: 'true' } }));
      const replacement = document(io);
      const robot = (replacement.components as Array<Record<string, JsonValue>>)[0];
      const configuration = (robot.configuration as Record<string, JsonValue>).runtime_configuration as Record<string, JsonValue>;
      (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>)[0].command_target_id = 'other_gripper';
      controller.workspace!.draft.document = replacement;
      await render();
      expect(control('Valve output 1 open value').value).toBe('false');
      expect(find((props) => props.type === 'button' && props.children === 'Update pneumatic I/O Draft').disabled).toBe(true);
      expect(controller.mutate).not.toHaveBeenCalled();
    } finally { await act(() => root.unmount()); }
  });

  it('keeps one gripper\'s edits when its row moves and updates the new path', async () => {
    const controller = {
      workspace: { draft: { document: document(io) } }, mutationAvailable: true, busy: false,
      mutate: vi.fn().mockResolvedValue({}), updateMotionTargetLimit: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const render = () => act(() => root.render(<DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    const control = (label: string) => find((props) => props['aria-label'] === label);
    await render();
    try {
      await act(() => control('Valve output 1 open value').onChange?.({ target: { value: 'true' } }));
      const replacement = document(io);
      const robot = (replacement.components as Array<Record<string, JsonValue>>)[0];
      const configuration = (robot.configuration as Record<string, JsonValue>).runtime_configuration as Record<string, JsonValue>;
      (configuration.gripper_target_configurations as Array<Record<string, JsonValue>>).unshift({
        controllable_component_id: 'robot', command_target_id: 'other', actuation_kind: 'position_commanded', output_mode: 'commanded',
      });
      controller.workspace!.draft.document = replacement;
      await render();
      expect(control('Valve output 1 open value').value).toBe('true');
      await act(() => find((props) => props.type === 'button' && props.children === 'Update pneumatic I/O Draft').onClick?.());
      expect(controller.mutate).toHaveBeenCalledWith(
        ['components', 0, 'configuration', 'runtime_configuration', 'gripper_target_configurations', 1, 'pneumatic_io'],
        'set', { ...io, outputs: [{ ...io.outputs[0], open_value: true }] },
      );
    } finally { await act(() => root.unmount()); }
  });

  it('offers only configured logical inputs and keeps missing selections removable', () => {
    const html = renderToStaticMarkup(<PneumaticRequiredInputsEditor declaredKeys={['workpiece_grasped']}
      value={[{ input: 'pressure', value: false }]} disabled={false} onCommit={vi.fn()} />);
    expect(html).toContain('Workpiece grasped');
    expect(html).toContain('Pressure (not declared for this gripper)');
    expect(html).toContain('Pressure expected value');
    expect(html).toContain('Do not command the same pneumatic gripper');
    expect(html).toContain('command grippers that share valve outputs');
    expect(html).toContain('through ordinary I/O in parallel branches');
    expect(html).toContain('avoid overlapping manual commands to the same valves');
    expect(html).toContain('The last write wins for each output.');
    expect(html).toContain('read again to confirm');
    expect(html).toContain('waits without a time limit');
    expect(html).not.toContain('Closed expected value');
  });

  it('edits only the selected logical key and expected value', async () => {
    const commit = vi.fn();
    function Harness() {
      const [value, setValue] = useState<JsonValue>([{ input: 'pressure', value: false }]);
      return <PneumaticRequiredInputsEditor declaredKeys={['workpiece_grasped']} value={value}
        disabled={false} onCommit={(next) => { commit(next); setValue(next); }} />;
    }
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<Harness />));
    const change = (label: string, target: { checked?: boolean; value?: string }) => act(() => (
      find((props) => props['aria-label'] === label).onChange?.({ target })
    ));
    try {
      await change('Workpiece grasped required', { checked: true });
      expect(commit).toHaveBeenLastCalledWith([{ input: 'pressure', value: false }, { input: 'workpiece_grasped', value: true }]);
      await change('Workpiece grasped expected value', { value: 'false' });
      expect(commit).toHaveBeenLastCalledWith([{ input: 'pressure', value: false }, { input: 'workpiece_grasped', value: false }]);
      await change('Pressure required', { checked: false });
      expect(commit).toHaveBeenLastCalledWith([{ input: 'workpiece_grasped', value: false }]);
    } finally { await act(() => root.unmount()); }
  });
});
