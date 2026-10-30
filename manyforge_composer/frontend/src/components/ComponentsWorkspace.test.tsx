import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { CCLCController } from '../hooks/useCCLC';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { CCLCComponentSnapshot } from '../types/cclc';
import type { ComponentProfileFacts } from '../types/componentCatalog';
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

function armSnapshot(): CCLCComponentSnapshot {
  return {
    controllableComponentId: 'arm',
    lifecycleGeneration: 4,
    lifecycleState: 'active',
    retainedBundleId: 'bundle.1',
    publicationGeneration: 1, activePublicPointerDigest: 'pointer.1',
    graphRevision: 'graph.1',
    domains: [],
    providers: [],
    lifecycleTarget: null,
    lifecycleProgress: null,
    readiness: { requestOwnerEpoch: 'owner.1', evaluatedAtNs: 100, nextIntent: null, requestedState: null,
      ready: true, error: null, measuredState: { required: false, available: null, error: null },
      providerOccurrences: [{
        providerOccurrence: { controlDomainId: 'domain.arm', controlDomainRevision: 'revision.1',
          lifecyclePlanCatalogDigest: 'catalog.1', providerInstanceId: 'provider.1',
          providerManifestId: 'manifest.1', providerManifestRevision: 'revision.1',
          providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
        health: { observedState: 'active', diagnostic: null, facts: [
          { lifecycleFactId: 'manyforge.vendor_code.error', value: { kind: 'token', tokenValue: 'C2' } },
        ] },
        decisionKind: null, error: null,
      }] },
  };
}

function cclcController(): CCLCController {
  const snapshot = armSnapshot();
  return {
    controllableComponentId: 'arm',
    snapshot,
    routeTemplates: null,
    transaction: null,
    pendingTransaction: null,
    pendingActivationRequest: null,
    busy: false,
    readUnavailableReason: null,
    error: null,
    setControllableComponentId: vi.fn(),
    refresh: vi.fn(async () => {}),
    reconcileTransaction: vi.fn(),
    dismissError: vi.fn(),
    activate: vi.fn(async () => {}),
    deactivateComponents: vi.fn(),
    activateSequence: vi.fn(async () => true),
    readActivationSequence: vi.fn(),
    cancelActivation: vi.fn(),
    activationProgress: null,
    activationRefusal: null, activationUnresolved: false,
    componentHealth: { arm: { snapshot, error: null } },
    submitLifecycle: vi.fn(),
    submitRoute: vi.fn(),
  };
}

function props(overrides: Partial<Parameters<typeof ComponentsWorkspace>[0]> = {}) {
  return {
    enabled: true,
    controller: deploymentController([arm, device], ['arm']),
    cclc: cclcController(),
    lifecycleIds: ['arm'],
    snapshot: null,
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
    // Left: Activate all, the list (Applied with health; Draft-only marked), Add component.
    expect(html).toContain('Activate all');
    expect(html).toContain('active · Ready');
    expect(html).toContain('Added in Draft · runs after Apply');
    expect(html).toContain('>Add component</button>');
    expect(html).toContain('Custom I/O device or AI model');
    // Right: the page of the CCLC-selected component.
    expect(html).toContain('id="component-page-title">arm</h3>');
    expect(html).toContain('aria-label="Component lifecycle status"');
    expect(html).toContain('Vendor status');
    expect(html).toContain('C2');
    expect(html).toContain('Open in Robot Control');
    // Its settings only: the robot's limits, not the I/O device or the cell settings.
    expect(html).toContain('arm settings');
    expect(html).toContain('Robot model and target limits');
    expect(html).not.toContain('station_io lifecycle command time limit');
    expect(html).not.toContain('Motion takeover timing');
    expect(html).not.toContain('Global interrupt rules');
  });

  it('opens a Draft-only component\'s page with its settings and I/O, leaving the lifecycle selection alone', async () => {
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
      // Not in Manager Applied: no lifecycle request, a link to Deployment Changes instead.
      expect(current.cclc.setControllableComponentId).not.toHaveBeenCalled();
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

  it('shows a mounted tool\'s model payload read-only, and 0 kg when the model defines none', () => {
    const hand = {
      component_id: 'hand',
      component_profile_id: 'hand_v0',
      model_source: { urdf: { project_path: 'sources/hand.urdf' }, package_roots: {} },
      target_settings: {},
      configuration: { runtime_kind: 'ros2_control' },
    };
    const controller = deploymentController([arm, hand], ['arm']);
    (controller.workspace!.draft.document as Record<string, JsonValue>).attachments = [{
      parent: { component_id: 'arm', interface: 'flange' },
      child: { component_id: 'hand', interface: 'mount' },
    }];
    const cclc = cclcController();
    cclc.controllableComponentId = 'hand';
    const page = (facts: Pick<ComponentProfileFacts, 'payload'>) => renderToStaticMarkup(<ComponentsWorkspace {...props({
      controller,
      cclc,
      catalog: {
        catalog: {
          schemaVersion: 'manyforge.composer_component_catalog.v1', projectRoot: null,
          entries: [], profilesWithoutEntry: [],
          componentProfiles: {
            robot_native_v1: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
            hand_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets: [], ...facts },
          },
        },
        error: null,
        reload: vi.fn(),
      },
    })} />);

    const defined = page({ payload: { massKg: 0.6, centerOfMassM: [0, 0.01, 0.05] } });
    expect(defined).toContain('id="component-page-title">hand → arm.flange</h3>');
    expect(defined).toContain('aria-label="hand payload"');
    expect(defined).toContain('<dt>Mass</dt><dd>0.6 kg</dd>');
    expect(defined).toContain('0, 0.01, 0.05 m');
    expect(defined).toContain('Defined by the component&#x27;s model');
    // Read-only: no input or button inside the payload card.
    const card = defined.slice(defined.indexOf('id="component-payload-title"'));
    expect(card.slice(0, card.indexOf('</section>'))).not.toMatch(/<input|<button/);

    const none = page({});
    expect(none).toContain('<dt>Mass</dt><dd>0 kg (the model defines none)</dd>');
    expect(none).not.toContain('Centre of mass');
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
          entries: [], profilesWithoutEntry: [],
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

  it('selects the page\'s component in CCLC when it has a lifecycle, but not during a request', async () => {
    const cclc = cclcController();
    cclc.controllableComponentId = 'other';
    const current = props({ cclc, lifecycleIds: ['arm', 'other'] });
    const { host } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      cclc.busy = true;
      await act(() => root.render(<ComponentsWorkspace {...current} />));
      expect(cclc.setControllableComponentId).not.toHaveBeenCalled();
      cclc.busy = false;
      await act(() => root.render(<ComponentsWorkspace {...current} cclc={{ ...cclc }} />));
      // 'other' is not in the Draft: the first row's page (arm) is shown and selected.
      expect(cclc.setControllableComponentId).toHaveBeenCalledWith('arm');
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
    expect(robotHtml).toContain('Native runtime configuration');
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
