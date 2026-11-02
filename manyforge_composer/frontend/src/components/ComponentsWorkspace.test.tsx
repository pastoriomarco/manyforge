import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentState } from '../types/components';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import { applyPreviewFixture } from '../test/applyPreview';
import { reactPropsHost } from '../test/reactPropsHost';
import { ComponentsWorkspace } from './ComponentsWorkspace';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';

afterEach(() => vi.unstubAllGlobals());

const signal = {
  controllable_component_id: 'station_io',
  command_target_id: 'io',
  resource_role_id: 'part_present',
  signal_name: 'part_present',
  source_id: 'station_mock',
  value_type: 'bool',
  direction: 'input',
  update_mode: 'change_only',
  transition_capability: 'sampled_only',
  freshness: { mode: 'provider_validity' },
  provider: { profile: 'mock' },
};

const arm = {
  component_id: 'arm',
  component_profile_id: 'robot_native_v1',
  model_source: { urdf: { project_path: 'sources/robot.urdf' }, package_roots: {} },
  root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
  target_settings: { arm: { maximum_tcp_linear_speed_mps: 0.2 } },
  configuration: { mode: 'simulated', execution_tick_hz: 100 },
};

const device = {
  component_id: 'station_io',
  component_profile_id: 'io_device_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'io_provider',
    runtime_configuration: {
      io_provider_devices: [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 12 }],
      io_signals: [signal],
    },
  },
};

function deploymentController(components: JsonValue[], appliedIds: string[]) {
  return {
    workspace: {
      draft: {
        id: 'draft', revision: 1,
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          deployment_artifact_id: 'cell-a',
          name: 'Cell A',
          components,
          attachments: [],
          policies: {},
        },
      },
      managerApplied: { activationOrder: { componentIds: appliedIds } },
    },
    loading: false,
    feedback: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
    busy: false,
    refresh: vi.fn(),
    clearFeedback: vi.fn(),
    mutate: vi.fn().mockResolvedValue({}),
    updateMotionTargetLimit: vi.fn(),
    addComponents: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

const armState: ComponentState = {
  component_id: 'arm',
  lifecycle: 'fault',
  fault: { code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'enable',
    vendor_code: 'C2', vendor_text: 'Emergency IO', since_ns: 1 },
  activity: 'idle',
  streams: [{ stream_id: 'joint_states', freshness: 'fresh', age_ns: 4_000_000 }],
  capabilities: ['reset_position', 'io_manual_write'],
  observed_at_ns: 2,
};

function props(overrides: Partial<Parameters<typeof ComponentsWorkspace>[0]> = {}) {
  return {
    enabled: true,
    controller: deploymentController([arm, device], ['arm']),
    cellState: { components: [armState], latch: null, io: null, readiness: null },
    catalog: { catalog: null, error: null, reload: vi.fn() },
    sourceModel: { status: 'loading' } as const,
    motionDescriptors: [],
    gripperDescriptors: [],
    robotControlIds: ['arm'],
    onOpenRobotControl: vi.fn(),
    onOpenDeployment: vi.fn(),
    ...overrides,
  };
}

describe('Components section', () => {
  it('lists every component with its health and opens the selected component\'s page', () => {
    const html = renderToStaticMarkup(<ComponentsWorkspace {...props()} />);
    // Left: Enable all, the list (Applied with state; Draft-only marked), Add component.
    expect(html).toContain('Enable all');
    expect(html).toContain('Fault · Emergency stop active');
    expect(html).toContain('Added in Draft · runs after Apply');
    expect(html).toContain('>Add component</button>');
    expect(html).toContain('Custom I/O device or AI model');
    // Right: the first component's page with its state and actions.
    expect(html).toContain('id="component-page-title">arm</h3>');
    expect(html).toContain('aria-label="arm status"');
    expect(html).toContain('Vendor: C2 · Emergency IO');
    expect(html).toContain('Operator action: Enable');
    expect(html).toContain('>Reset position</button>');
    expect(html).not.toContain('>io manual write</button>');
    expect(html).toContain('Open in Robot Control');
    // Its settings only: the robot's limits, not the I/O device or the cell settings.
    expect(html).toContain('arm settings');
    expect(html).toContain('Robot model and target limits');
    expect(html).not.toContain('station_io lifecycle command time limit');
    expect(html).not.toContain('Motion takeover timing');
    expect(html).not.toContain('Global interrupt rules');
  });

  it('opens a Draft-only component\'s page with its settings and I/O', async () => {
    const current = props();
    const { host, find, all } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentsWorkspace {...current} />));
    try {
      const row = find((candidate) => candidate.type === 'button'
        && String(candidate.title).includes('Added in Draft'));
      await act(() => row.onClick?.());
      const labels = all(() => true).map((candidate) => candidate['aria-label']);
      expect(labels).toContain('station_io lifecycle command time limit');
      expect(labels).toContain('Declared by this component');
      // Not in the cell yet: a link to Deployment Changes.
      await act(() => find((candidate) => candidate.type === 'button'
        && candidate.children === 'Review in Deployment Changes').onClick?.());
      expect(current.onOpenDeployment).toHaveBeenCalledExactlyOnceWith('changes');
    } finally { await act(() => root.unmount()); }
  });

  it('links a robot to Robot Control instead of duplicating jogging', async () => {
    const current = props();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentsWorkspace {...current} />));
    try {
      await act(() => find((candidate) => candidate.type === 'button'
        && candidate.children === 'Open in Robot Control').onClick?.());
      expect(current.onOpenRobotControl).toHaveBeenCalledExactlyOnceWith('arm');
    } finally { await act(() => root.unmount()); }
  });

  it('shows a mounted tool\'s model payload read-only, and 0 kg when the model defines none', async () => {
    const hand = {
      component_id: 'hand',
      component_profile_id: 'hand_v0',
      model_source: { urdf: { project_path: 'sources/hand.urdf' }, package_roots: {} },
      target_settings: {},
      configuration: { runtime_kind: 'ros2_control' },
    };
    const controller = deploymentController([{ ...arm, component_id: 'robot' }, hand], ['robot']);
    (controller.workspace!.draft.document as Record<string, JsonValue>).attachments = [{
      parent: { component_id: 'robot', interface: 'flange' },
      child: { component_id: 'hand', interface: 'mount' },
    }];
    // The list is one tree (robot, then its tool): open the tool's page.
    const page = async (facts: Pick<ComponentProfileFacts, 'payload'>) => {
      const { host, find, all } = reactPropsHost();
      const { createRoot } = await import('react-dom/client');
      const root = createRoot(host as unknown as Element);
      await act(() => root.render(<ComponentsWorkspace {...props({
        controller,
        catalog: {
          catalog: {
            schemaVersion: 'manyforge.composer_component_catalog.v1', projectRoot: null,
            entries: [], machines: [], profilesWithoutEntry: [],
            componentProfiles: {
              robot_native_v1: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
              hand_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets: [], ...facts },
            },
          },
          error: null,
          reload: vi.fn(),
        },
      })} />));
      try {
        await act(() => find((candidate) => candidate.type === 'button'
          && String(candidate.title).includes('Added in Draft')).onClick?.());
        const text = (value: unknown): string => Array.isArray(value)
          ? value.map(text).join('') : typeof value === 'string' || typeof value === 'number' ? String(value) : '';
        return {
          title: text(find((candidate) => candidate.id === 'component-page-title').children),
          texts: all((candidate) => candidate.children !== undefined).map((candidate) => text(candidate.children)),
          payload: all((candidate) => candidate['aria-label'] === 'hand payload'),
          controls: all((candidate) => candidate.onClick !== undefined || candidate.onChange !== undefined)
            .map((candidate) => String(candidate['aria-label'] ?? candidate.title ?? '')),
        };
      } finally { await act(() => root.unmount()); }
    };

    const defined = await page({ payload: { massKg: 0.6, centerOfMassM: [0, 0.01, 0.05] } });
    expect(defined.title).toBe('hand → robot.flange');
    expect(defined.payload).toHaveLength(1);
    expect(defined.texts).toContain('0.6 kg');
    expect(defined.texts).toContain('0, 0.01, 0.05 m (in the tool\'s mount)');
    expect(defined.texts.some((value) => value.startsWith('Defined by the component\'s model'))).toBe(true);
    // Read-only: no control edits the payload.
    expect(defined.controls.filter((label) => /payload|mass/i.test(label))).toEqual([]);

    const none = await page({});
    expect(none.texts).toContain('0 kg (the model defines none)');
    expect(none.texts.some((value) => value.startsWith('Centre of mass'))).toBe(false);
  });

  it('warns on the robot\'s page while its flange tool has no TCP (B-run-1)', () => {
    const bracket = {
      component_id: 'bracket',
      component_profile_id: 'bracket_v0',
      model_source: { urdf: { project_path: 'sources/bracket.urdf' }, package_roots: {} },
      target_settings: {},
      configuration: { runtime_kind: 'ros2_control' },
    };
    const controller = deploymentController([arm, bracket], ['arm']);
    (controller.workspace!.draft.document as Record<string, JsonValue>).attachments = [{
      parent: { component_id: 'arm', interface: 'flange' },
      child: { component_id: 'bracket', interface: 'mount' },
    }];
    const page = (tcpTargets: ComponentProfileFacts['tcpTargets']) => renderToStaticMarkup(<ComponentsWorkspace {...props({
      controller,
      catalog: {
        catalog: {
          schemaVersion: 'manyforge.composer_component_catalog.v1', projectRoot: null,
          entries: [], machines: [], profilesWithoutEntry: [],
          componentProfiles: {
            robot_native_v1: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
            bracket_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets },
          },
        },
        error: null,
        reload: vi.fn(),
      },
    })} />);
    const warning = 'bracket on arm has no TCP; Programs can&#x27;t run until one of its frames is marked as a TCP.';
    expect(page([])).toContain(`<p class="inline-message severity-warning" role="status">${warning}</p>`);
    expect(page([{ commandTargetId: 'mount', tcpFrameRoleIds: ['tip'], defaultTcpFrameRoleId: 'tip' }]))
      .not.toContain('has no TCP');
  });

  it('shows no payload card for a robot', () => {
    expect(renderToStaticMarkup(<ComponentsWorkspace {...props()} />)).not.toContain('component-payload-title');
  });

  it('edits the base pose, lane and mount as Draft edits and previews before Apply', async () => {
    const hand = {
      component_id: 'hand', component_profile_id: 'hand_v0', lane: 'mock',
      model_source: { urdf: { project_path: 'sources/hand.urdf' }, package_roots: {} },
      target_settings: {}, configuration: { runtime_kind: 'ros2_control' },
    };
    const robot = { ...arm, component_id: 'robot', lane: 'mock' };
    const second = { ...arm, component_id: 'second' };
    const controller = deploymentController([robot, second, hand], ['robot']);
    (controller.workspace!.draft.document as Record<string, JsonValue>).attachments = [{
      parent: { component_id: 'robot', interface: 'flange' },
      child: { component_id: 'hand', interface: 'mount' },
      collision_exclusions: [{ child_link: 'finger', parent_link: 'link5' }],
    }];
    const preview = applyPreviewFixture([{ component_id: 'hand', disposition: 'replace' as const,
      reasons: ['mount changed'], restarts: true }, { component_id: 'robot', disposition: 'retain' as const,
      reasons: [], restarts: false }]);
    Object.assign(controller, {
      previewApply: vi.fn().mockResolvedValue(preview),
      applyDraft: vi.fn().mockResolvedValue(preview),
      documentUnavailableReason: null,
    });
    (controller.workspace as unknown as Record<string, unknown>).runAdmission = { ready: false, reason: 'differs' };
    (controller.workspace!.draft as unknown as Record<string, unknown>).deploymentRevision = null;
    (controller.workspace as unknown as Record<string, unknown>).saved = { differsFromApplied: true };
    const current = props({
      controller,
      cellState: { components: [], latch: null, io: null, readiness: null },
      catalog: {
        catalog: {
          schemaVersion: 'manyforge.composer_component_catalog.v1', projectRoot: null,
          entries: [], machines: [], profilesWithoutEntry: [],
          componentProfiles: {
            robot_native_v1: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [],
              lanes: ['isaac', 'mock', 'real'] },
            hand_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets: [], lanes: ['mock'] },
          },
        },
        error: null,
        reload: vi.fn(),
      },
    });
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentsWorkspace {...current} />));
    try {
      // The tool's page: mount, primary tool and collision exclusions.
      await act(() => find((candidate) => String(candidate.className ?? '').includes('component-row')
        && Array.isArray(candidate.children)
        && candidate.children.some((child) => (child as { props?: { children?: unknown } })?.props?.children === 'hand'))
        .onClick?.());
      await act(() => find((candidate) => candidate['aria-label'] === 'hand mounted on')
        .onChange?.({ target: { value: 'second' } }));
      expect(controller.mutate).toHaveBeenLastCalledWith(['attachments'], 'set', [{
        parent: { component_id: 'second', interface: 'flange' },
        child: { component_id: 'hand', interface: 'mount' },
      }]);
      await act(() => find((candidate) => candidate['aria-label'] === 'hand primary tool')
        .onChange?.({ target: { checked: true } }));
      // A move drops the pairs (they name the old parent's links); a primary toggle keeps them.
      expect(controller.mutate).toHaveBeenLastCalledWith(['attachments'], 'set', [{
        parent: { component_id: 'robot', interface: 'flange' },
        child: { component_id: 'hand', interface: 'mount' },
        primary: true,
        collision_exclusions: [{ child_link: 'finger', parent_link: 'link5' }],
      }]);
      const exclusions = () => find((candidate) => candidate.type === 'button'
        && candidate.children === 'Update collision exclusions');
      expect(exclusions().disabled).toBe(true);
      await act(() => find((candidate) => candidate['aria-label'] === 'hand collision exclusions with parent')
        .onChange?.({ target: { value: 'finger link5\npalm,link4\n\n finger  link6 ' } }));
      await act(() => exclusions().onClick?.());
      expect(controller.mutate).toHaveBeenLastCalledWith(['attachments'], 'set', [{
        parent: { component_id: 'robot', interface: 'flange' },
        child: { component_id: 'hand', interface: 'mount' },
        collision_exclusions: [
          { child_link: 'finger', parent_link: 'link5' },
          { child_link: 'finger', parent_link: 'link6' },
          { child_link: 'palm', parent_link: 'link4' },
        ],
      }]);
      const calls = vi.mocked(controller.mutate).mock.calls.length;
      await act(() => find((candidate) => candidate['aria-label'] === 'hand collision exclusions with parent')
        .onChange?.({ target: { value: 'finger' } }));
      await act(() => exclusions().onClick?.());
      expect(vi.mocked(controller.mutate).mock.calls.length).toBe(calls);
      expect(find((candidate) => candidate.role === 'alert').children)
        .toBe('Each line must name two links: the tool link, then the parent link.');
      // Preview shows the per-component impact; Apply goes through the confirmed Apply.
      await act(() => find((candidate) => candidate.type === 'button' && candidate.children === 'Preview').onClick?.());
      expect(controller.previewApply).toHaveBeenCalledOnce();
      await act(() => find((candidate) => candidate.type === 'button' && candidate.children === 'Apply…').onClick?.());
      expect(controller.applyDraft).toHaveBeenCalledOnce();
      // The robot's page: lane among the model's lanes, base pose with orientation, Remove.
      await act(() => find((candidate) => candidate.type === 'button' && candidate.title === 'Applied').onClick?.());
      await act(() => find((candidate) => candidate['aria-label'] === 'robot lane')
        .onChange?.({ target: { value: 'real' } }));
      expect(controller.mutate).toHaveBeenLastCalledWith(['components', 0, 'lane'], 'set', 'real');
      await act(() => find((candidate) => candidate['aria-label'] === 'robot base yaw')
        .onChange?.({ target: { value: '90' } }));
      await act(() => find((candidate) => candidate.type === 'button'
        && candidate.children === 'Update base pose').onClick?.());
      const [path, operation, pose] = vi.mocked(controller.mutate).mock.lastCall!;
      expect(path).toEqual(['components', 0, 'root_pose_in_cell']);
      expect(operation).toBe('set');
      const { orientation_xyzw: xyzw } = pose as { orientation_xyzw: number[] };
      expect(xyzw[2]).toBeCloseTo(Math.SQRT1_2);
      expect(xyzw[3]).toBeCloseTo(Math.SQRT1_2);
      await act(() => find((candidate) => candidate.type === 'button'
        && candidate.children === 'Remove from Draft').onClick?.());
      // Its attachment row goes first, then the component.
      expect(vi.mocked(controller.mutate).mock.calls.slice(-2)).toEqual([
        [['attachments'], 'set', []],
        [['components', 0], 'remove'],
      ]);
    } finally { await act(() => root.unmount()); }
  });
});

describe('Focused editor scopes', () => {
  const render = (scope?: Parameters<typeof DeploymentFocusedEditor>[0]['scope']) => renderToStaticMarkup(
    <DeploymentFocusedEditor controller={deploymentController([arm, device], [])} scope={scope}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);

  it('keeps every section by default', () => {
    const html = render();
    for (const text of ['Robot model and target limits', 'I/O devices', 'Add I/O device',
      'Motion takeover timing', 'Global interrupt rules']) expect(html).toContain(text);
  });

  it('shows one component\'s own sections on its page, without add forms or cell settings', () => {
    const deviceHtml = render({ componentId: 'station_io' });
    expect(deviceHtml).toContain('station_io lifecycle command time limit');
    expect(deviceHtml).not.toContain('Add I/O device');
    expect(deviceHtml).not.toContain('Robot model and target limits');
    expect(deviceHtml).not.toContain('Motion takeover timing');
    const robotHtml = render({ componentId: 'arm' });
    expect(robotHtml).toContain('Robot model and target limits');
    expect(robotHtml).toContain('Execution host configuration');
    expect(robotHtml).not.toContain('<summary>I/O devices</summary>');
    expect(robotHtml).not.toContain('Global interrupt rules');
  });

  it('shows only the cell-level settings under Deployment', () => {
    const html = render('cell');
    expect(html).toContain('Cell settings');
    expect(html).toContain('Motion takeover timing');
    expect(html).toContain('Global interrupt rules');
    expect(html).not.toContain('<summary>I/O devices</summary>');
    expect(html).not.toContain('Robot model and target limits');
    expect(html).not.toContain('Stop settings');
  });
});
