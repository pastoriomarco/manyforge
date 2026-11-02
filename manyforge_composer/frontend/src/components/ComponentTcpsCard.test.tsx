import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ManualControlMotionDescriptor } from '../types/robotControl';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import type { ComponentFrameTarget } from '../utils/componentManager';
import { ComponentTcpsCard } from './ComponentTcpsCard';

afterEach(() => vi.unstubAllGlobals());

const pen = {
  tcp_id: 'pen',
  parent_frame: 'flange',
  pose: { position_m: [0, 0, 0.12], orientation: { rpy_deg: [0, 0, 0] } },
};

function controller(customTcps: JsonValue[] = [pen]) {
  return {
    workspace: {
      draft: {
        id: 'draft', revision: 1,
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          components: [{ component_id: 'lite6_robot', component_profile_id: 'lite6_mock_commanded_v0',
            custom_tcps: customTcps }],
          attachments: [],
        },
      },
    },
    loading: false,
    busy: false,
    mutationAvailable: true,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
}

const frames: ComponentFrameTarget[] = [{
  targetId: 'arm',
  resolvedTcp: 'lite_gripper default TCP',
  frames: [
    { frameRoleId: 'base', sourceLinkName: 'link_base' },
    { frameRoleId: 'flange', sourceLinkName: 'link_eef' },
    { frameRoleId: 'tcp', sourceLinkName: 'link_tcp' },
    // An Applied custom TCP is listed once, as a custom TCP.
    { frameRoleId: 'pen', sourceLinkName: null },
  ],
}];

const descriptors = [{
  controllableComponentId: 'lite6_robot',
  endEffectorComponentId: null,
  tcps: [
    { entry: { componentId: 'lite6_robot', targetId: 'arm', frameRoleId: 'tcp' },
      targetRoleId: 'arm_target', controllableComponentId: 'lite6_robot', commandTargetId: 'arm',
      frameRoleId: 'tcp', qualifiedName: 'lite_gripper.tcp', compiledFrameName: 'link_tcp',
      modelInstanceId: 'robot_model', modelRevision: 'sha256:model', isDefault: true,
      mountedHere: true, mountedElsewhere: false, contactBodyRoleIds: [] },
    { entry: { componentId: 'lite6_robot', targetId: 'arm', frameRoleId: 'flange' },
      targetRoleId: 'arm_target', controllableComponentId: 'lite6_robot', commandTargetId: 'arm',
      frameRoleId: 'flange', qualifiedName: 'lite6_robot.flange', compiledFrameName: 'link_eef',
      modelInstanceId: 'robot_model', modelRevision: 'sha256:model', isDefault: false,
      mountedHere: true, mountedElsewhere: false, contactBodyRoleIds: [] },
  ],
}] as unknown as ManualControlMotionDescriptor[];

async function mounted(current: DeploymentWorkspaceController) {
  const host = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host.host as unknown as Element);
  await act(() => root.render(<ComponentTcpsCard componentId="lite6_robot" controller={current}
    frames={frames} gripperFrames={[]} motionDescriptors={descriptors} />));
  return { ...host, root };
}

const button = (label: string) => (props: MountedProps) => props.type === 'button'
  && (props.children === label || props['aria-label'] === label);
const field = (label: string) => (props: MountedProps) => props['aria-label'] === label;

describe('TCPs and frames card', () => {
  it('shows model frames and descriptor TCPs read-only, custom TCPs editable', () => {
    const html = renderToStaticMarkup(<ComponentTcpsCard componentId="lite6_robot" controller={controller()}
      frames={frames} gripperFrames={[]} motionDescriptors={descriptors} />);
    expect(html).toContain('Applied model · read-only');
    expect(html).toContain('link_tcp · TCP lite_gripper.tcp (default)');
    expect(html).toContain('lite6_robot.pen');
    // Only custom TCPs have Edit/Delete; model and descriptor rows have none.
    expect(html).toContain('aria-label="Edit custom TCP pen"');
    expect(html).not.toContain('Edit custom TCP flange');
    expect(html).not.toContain('Edit custom TCP tcp');
    expect(html).not.toMatch(/<dt>pen<\/dt>/);
    // No fine-tuning offset control anywhere.
    expect(html.toLowerCase()).not.toContain('fine-tun');
  });

  it('uses the mounted descriptor row when another robot lists the same TCP', () => {
    const mountedRow = descriptors[0].tcps[0];
    const elsewhere = { ...mountedRow, mountedHere: false, mountedElsewhere: true, isDefault: false };
    const html = renderToStaticMarkup(<ComponentTcpsCard componentId="lite6_robot" controller={controller()}
      frames={frames} gripperFrames={[]} motionDescriptors={[
        { ...descriptors[0], tcps: [elsewhere] }, descriptors[0],
      ]} />);
    expect(html).toContain('link_tcp · TCP lite_gripper.tcp (default)');
  });

  it('adds a custom TCP on a model frame in one Draft edit', async () => {
    const current = controller();
    const { find, all, root } = await mounted(current);
    try {
      await act(() => find(button('Add custom TCP')).onClick?.());
      // The parent picker lists the component's frames, never a custom TCP.
      const options = all((props) => props.value !== undefined && typeof props.children === 'string'
        && ['base', 'flange', 'tcp', 'pen'].includes(props.children as string))
        .map((props) => props.value);
      expect(options).toEqual(expect.arrayContaining(['base', 'flange', 'tcp']));
      expect(options).not.toContain('pen');
      await act(() => find(field('Custom TCP id')).onChange?.({ target: { value: 'probe' } }));
      await act(() => find(field('Custom TCP parent frame')).onChange?.({ target: { value: 'tcp' } }));
      await act(() => find(field('Custom TCP z (m)')).onChange?.({ target: { value: '0.05' } }));
      await act(() => find(field('Custom TCP pitch (deg)')).onChange?.({ target: { value: '90' } }));
      await act(() => find(button('Save to Draft')).onClick?.());
      expect(current.mutate).toHaveBeenCalledExactlyOnceWith(['components', 0, 'custom_tcps'], 'set', [
        pen,
        { tcp_id: 'probe', parent_frame: 'tcp',
          pose: { position_m: [0, 0, 0.05], orientation: { rpy_deg: [0, 90, 0] } } },
      ]);
    } finally { await act(() => root.unmount()); }
  });

  it('edits a custom TCP in place, keeping its id', async () => {
    const current = controller();
    const { find, root } = await mounted(current);
    try {
      await act(() => find(button('Edit custom TCP pen')).onClick?.());
      expect(find(field('Custom TCP id')).disabled).toBe(true);
      await act(() => find(field('Custom TCP z (m)')).onChange?.({ target: { value: '0.15' } }));
      await act(() => find(button('Save to Draft')).onClick?.());
      expect(current.mutate).toHaveBeenCalledExactlyOnceWith(['components', 0, 'custom_tcps'], 'set', [
        { ...pen, pose: { position_m: [0, 0, 0.15], orientation: { rpy_deg: [0, 0, 0] } } },
      ]);
    } finally { await act(() => root.unmount()); }
  });

  it('deletes the last custom TCP by removing the field', async () => {
    const current = controller();
    const { find, root } = await mounted(current);
    try {
      await act(() => find(button('Delete custom TCP pen')).onClick?.());
      expect(current.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 0, 'custom_tcps'], 'remove', undefined,
      );
    } finally { await act(() => root.unmount()); }
  });

  it('keeps a quaternion row verbatim when another custom TCP is edited', async () => {
    const quaternion = {
      tcp_id: 'probe', parent_frame: 'flange',
      pose: { position_m: [0, 0, 0.1], orientation: { xyzw: [0, 0, 0.7071068, 0.7071068] } },
    };
    const current = controller([pen, quaternion]);
    const { find, root } = await mounted(current);
    try {
      await act(() => find(button('Edit custom TCP pen')).onClick?.());
      await act(() => find(field('Custom TCP z (m)')).onChange?.({ target: { value: '0.15' } }));
      await act(() => find(button('Save to Draft')).onClick?.());
      expect(current.mutate).toHaveBeenCalledExactlyOnceWith(['components', 0, 'custom_tcps'], 'set', [
        { ...pen, pose: { position_m: [0, 0, 0.15], orientation: { rpy_deg: [0, 0, 0] } } },
        quaternion,
      ]);
    } finally { await act(() => root.unmount()); }
  });

  it('disables Add, Edit and Delete on a component without a Draft row (removed or compiled)', () => {
    const removed = controller();
    (removed.workspace!.draft.document as Record<string, JsonValue>).components = [];
    const compiled = controller();
    (compiled.workspace!.draft.document as Record<string, JsonValue>).api_version = 'manyforge.deployment.v1';
    for (const current of [removed, compiled]) {
      const html = renderToStaticMarkup(<ComponentTcpsCard componentId="lite6_robot" controller={current}
        frames={frames} gripperFrames={[]} motionDescriptors={descriptors} />);
      expect(html).toMatch(/<button type="button" disabled="" title="This component has no row in the Deployment Draft[^"]*">Add custom TCP<\/button>/);
    }
    // A compiled Deployment still lists its custom TCPs read-only.
    const compiledHtml = renderToStaticMarkup(<ComponentTcpsCard componentId="lite6_robot" controller={compiled}
      frames={frames} gripperFrames={[]} motionDescriptors={descriptors} />);
    expect(compiledHtml).toMatch(/disabled="" title="This component has no row[^"]*" aria-label="Edit custom TCP pen"/);
    expect(compiledHtml).toMatch(/disabled="" title="This component has no row[^"]*" aria-label="Delete custom TCP pen"/);
  });

  it('does not save a TCP on another custom TCP or with a clashing id', async () => {
    const current = controller();
    const { find, root } = await mounted(current);
    try {
      await act(() => find(button('Add custom TCP')).onClick?.());
      await act(() => find(field('Custom TCP id')).onChange?.({ target: { value: 'flange' } }));
      expect(find(button('Save to Draft')).disabled).toBe(true);
      await act(() => find(field('Custom TCP id')).onChange?.({ target: { value: 'probe' } }));
      await act(() => find(field('Custom TCP parent frame')).onChange?.({ target: { value: 'pen' } }));
      expect(find(button('Save to Draft')).disabled).toBe(true);
      expect(current.mutate).not.toHaveBeenCalled();
    } finally { await act(() => root.unmount()); }
  });
});
