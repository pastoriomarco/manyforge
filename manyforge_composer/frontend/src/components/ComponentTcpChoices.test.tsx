import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import { reactPropsHost } from '../test/reactPropsHost';
import { ComponentTcpChoices } from './ComponentTcpChoices';

afterEach(() => vi.unstubAllGlobals());

const profiles: Record<string, ComponentProfileFacts> = {
  arm_v0: {
    tags: ['articulated-arm'], attachmentInterfaces: ['flange'],
    tcpTargets: [{ commandTargetId: 'arm', tcpFrameRoleIds: ['flange'], defaultTcpFrameRoleId: 'flange' }],
  },
  gripper_v0: {
    tags: ['tool'], attachmentInterfaces: [],
    tcpTargets: [{ commandTargetId: 'gripper', tcpFrameRoleIds: ['tcp'], defaultTcpFrameRoleId: 'tcp' }],
  },
  // A two-target tool with a `tcp` on both targets.
  dual_v0: {
    tags: ['tool'], attachmentInterfaces: [],
    tcpTargets: [
      { commandTargetId: 'arm', tcpFrameRoleIds: ['tcp'], defaultTcpFrameRoleId: 'tcp' },
      { commandTargetId: 'gripper', tcpFrameRoleIds: ['tcp'], defaultTcpFrameRoleId: 'tcp' },
    ],
  },
};

function controller(attachments: JsonValue[], dual: Record<string, JsonValue> = {}) {
  return {
    workspace: {
      draft: {
        id: 'draft', revision: 1,
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          components: [
            { component_id: 'robot', component_profile_id: 'arm_v0' },
            { component_id: 'hand', component_profile_id: 'gripper_v0' },
            { component_id: 'dual', component_profile_id: 'dual_v0', ...dual },
          ],
          attachments,
        },
      },
    },
    loading: false,
    busy: false,
    mutationAvailable: true,
    error: null,
    errorCauseCode: null,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
}

const mount = (child: string, primary?: boolean) => ({
  parent: { component_id: 'robot', interface: 'flange' },
  child: { component_id: child, interface: 'mount' },
  ...(primary === undefined ? {} : { primary }),
});

const html = (componentId: string, current: DeploymentWorkspaceController) => renderToStaticMarkup(
  <ComponentTcpChoices componentId={componentId} controller={current} profiles={profiles} />);

describe('Default TCP card', () => {
  it('shows the Default TCP selector only for a component with TCPs on more than one target', () => {
    const current = controller([mount('hand')]);
    expect(html('robot', current)).toBe('');
    expect(html('hand', current)).toBe('');
    const dual = html('dual', current);
    expect(dual).toContain('aria-label="dual default TCP"');
    expect(dual).toContain('<option value="" disabled="" selected="">Choose a TCP</option>');
    expect(dual).toContain('>tcp (arm)</option>');
    expect(dual).toContain('>tcp (gripper)</option>');
    expect(dual).not.toContain('Primary tool on');
    // Chosen: the pair is selected and the placeholder goes.
    const chosen = html('dual', controller([], { default_tcp: { command_target_id: 'gripper', frame_role_id: 'tcp' } }));
    expect(chosen).toContain('selected="">tcp (gripper)</option>');
    expect(chosen).not.toContain('Choose a TCP');
  });

  it('shows the primary tool, read-only like every mount, only when two tools share one mount', () => {
    expect(html('robot', controller([mount('hand')]))).toBe('');
    const shared = html('robot', controller([mount('hand'), mount('dual')]));
    expect(shared).toContain('Default TCP and primary tool');
    expect(shared).toContain('<dt>Primary tool on flange</dt><dd>none marked (hand, dual)</dd>');
    // Static markup escapes the apostrophe.
    expect(shared).toContain('Mounts can&#x27;t be edited here.');
    // Read-only: no control, and no Default TCP select on the robot.
    expect(shared).not.toMatch(/<select|<input|<button/);
    expect(html('robot', controller([mount('hand'), mount('dual', true)])))
      .toContain('<dt>Primary tool on flange</dt><dd>dual</dd>');
  });

  it('explains a typed Apply refusal of a choice with the catalog text', () => {
    const current = controller([]);
    Object.assign(current, { error: 'Not applied', errorCauseCode: 'component_default_tcp_required' });
    expect(html('dual', current)).toContain(
      'role="alert">The component has TCPs on more than one target and its type sets no single default.');
    Object.assign(current, { errorCauseCode: 'pneumatic_io_presets_coincident' });
    expect(html('dual', current)).not.toContain('role="alert"');
  });

  it('writes the chosen default TCP to the Deployment Draft', async () => {
    const current = controller([mount('hand'), mount('dual')]);
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(() => root.render(<ComponentTcpChoices componentId="dual" controller={current} profiles={profiles} />));
      await act(() => find((props) => props['aria-label'] === 'dual default TCP')
        .onChange?.({ target: { value: JSON.stringify(['gripper', 'tcp']) } }));
      expect(current.mutate).toHaveBeenLastCalledWith(
        ['components', 2, 'default_tcp'], 'set', { command_target_id: 'gripper', frame_role_id: 'tcp' });

    } finally { await act(() => root.unmount()); }
  });
});
