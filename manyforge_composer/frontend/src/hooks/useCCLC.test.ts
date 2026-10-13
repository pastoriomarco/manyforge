import { act, createElement, useEffect, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const apiMock = vi.hoisted(() => ({
  execution: { identity: vi.fn() }, deploymentWorkspace: { get: vi.fn() },
  cclc: { owner: vi.fn(), component: vi.fn(), routeTemplates: vi.fn(),
    submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(), lifecycleRequestResult: vi.fn() },
}));
vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(), api: apiMock,
}));

import {
  activateThroughOrdinaryLifecycle,
  activateOrderedComponents,
  awaitActivationCompletion,
  browserRequestIdentity,
  cclcControlModeUnavailableReason,
  cclcRequestOwnerEpoch,
  lifecycleSupportsRouteSelection,
  lifecycleRequestFromSnapshot,
  routeRequestFromTemplate,
  useCCLC,
} from './useCCLC';
import type {
  CCLCComponentSnapshot,
  CCLCRequestContext,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';
import type { ExecutionIdentity } from '../types/robotControl';

const context: CCLCRequestContext = {
  request_id: 'request.1',
  request_owner_epoch: 'composer.epoch.1',
  source_label: 'manyforge_composer',
  correlation_id: null,
};

const snapshot: CCLCComponentSnapshot = {
  componentId: 'component.arm',
  lifecycleGeneration: 4,
  lifecycleState: 'prepared',
  retainedBundleId: 'bundle.1',
  graphRevision: 'graph.1',
  providers: [],
  lifecycleTarget: null,
  lifecycleProgress: null,
  domains: [{
    controlDomainId: 'domain.arm',
    controlDomainRevision: 'domain.revision.1',
    controllerTopologyId: 'topology.arm',
    topologyRevision: 'topology.revision.1',
    activeConfiguration: { id: 'active.1', generation: 7 },
    currentControlBindings: [{
      controlBindingId: 'binding.arm',
      controlBindingRevision: 'binding.revision.1',
      controlRouteId: 'route.normal',
    }],
    currentResources: ['joint.1'],
  }],
};

const templates: CCLCRouteTemplateSet = {
  componentId: 'component.arm',
  retainedBundleId: 'bundle.1',
  graphRevision: 'graph.1',
  templates: [{
    desired_configuration: {
      control_domain_id: 'domain.arm',
      control_domain_revision: 'domain.revision.1',
      controller_topology_id: 'topology.arm',
      topology_revision: 'topology.revision.1',
      activation_set_id: 'activation.arm',
      binding_routes: [{
        control_binding_id: 'binding.arm',
        control_binding_revision: 'binding.revision.1',
        control_route_id: 'route.fast',
      }],
    },
    expected_active_control_configuration: {
      presence: 'present',
      active_control_configuration_id: 'active.1',
      active_control_configuration_generation: 7,
    },
    component_expectations: [{
      controllable_component_id: 'component.arm',
      expected_component_lifecycle_generation: 4,
      expected_component_lifecycle_state: 'prepared',
    }],
  }],
};

const execution = {
  ownerEpoch: 'kernel-owner.1',
  cellId: 'cell.1',
  activePublicationGeneration: 1,
  controlManagementMode: 'component_lifecycle',
  graphRevision: snapshot.graphRevision,
  retainedBundleId: snapshot.retainedBundleId,
  sceneRevision: {
    scene_id: 'scene.1',
    revision_id: 1,
    content_hash: 'sha256:scene',
  },
} satisfies ExecutionIdentity;

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('CCLC request projection', () => {
  it('targets the CCLC owner epoch and rejects torn product projections', () => {
    const owner = { requestOwnerEpoch: 'cclc-owner.1' };
    expect(cclcRequestOwnerEpoch(owner, snapshot, execution)).toBe('cclc-owner.1');
    expect(() => cclcRequestOwnerEpoch(owner, snapshot, {
      ...execution,
      graphRevision: 'graph.2',
    })).toThrow('projections are torn');
  });

  it('reads retained routes only in route-capable lifecycle states', () => {
    expect(lifecycleSupportsRouteSelection('declared')).toBe(false);
    expect(lifecycleSupportsRouteSelection('prepared')).toBe(true);
    expect(lifecycleSupportsRouteSelection('active')).toBe(true);
    expect(lifecycleSupportsRouteSelection('disabled')).toBe(false);
  });

  it('queries CCLC only for component-lifecycle managed deployments', () => {
    expect(cclcControlModeUnavailableReason('component_lifecycle')).toBeNull();
    expect(cclcControlModeUnavailableReason('direct')).toContain('direct native control');
    expect(cclcControlModeUnavailableReason('pending')).toContain('still attaching');
  });

  it('creates bounded identities when randomUUID is unavailable on trusted-LAN HTTP', () => {
    vi.stubGlobal('crypto', {
      getRandomValues: (words: Uint32Array) => {
        words.set([1, 2, 3, 4]);
        return words;
      },
    });

    expect(browserRequestIdentity('composer-test')).toBe(
      'composer-test-00000001000000020000000300000004',
    );
  });

  it('keeps the last-resort identity fallback unique without Web Crypto', () => {
    vi.stubGlobal('crypto', undefined);
    vi.spyOn(Date, 'now').mockReturnValue(1_787_247_600_000);

    const first = browserRequestIdentity('composer-test');
    const second = browserRequestIdentity('composer-test');
    expect(first).not.toBe(second);
    expect(first).toMatch(/^composer-test-[0-9a-z]+-[0-9a-z]+$/);
    expect(second).toMatch(/^composer-test-[0-9a-z]+-[0-9a-z]+$/);
  });

  it('copies lifecycle compare-and-set truth from the owner snapshot', () => {
    expect(lifecycleRequestFromSnapshot(snapshot, 'enable', context, 7)).toEqual({
      request_context: context,
      controllable_component_id: 'component.arm',
      intent: 'enable',
      expected_graph_revision: 'graph.1',
      expected_component_lifecycle_generation: 4,
      expected_admission_generation: 7,
      expected_control_domains: [{
        control_domain_id: 'domain.arm',
        control_domain_revision: 'domain.revision.1',
        controller_topology_id: 'topology.arm',
        topology_revision: 'topology.revision.1',
        expected_active_control_configuration: {
          presence: 'present',
          active_control_configuration_id: 'active.1',
          active_control_configuration_generation: 7,
        },
        expected_control_bindings: [{
          control_binding_id: 'binding.arm',
          control_binding_revision: 'binding.revision.1',
          control_route_id: 'route.normal',
        }],
      }],
      reason: 'operator-confirmed Composer request',
      confirmed: true,
    });
  });

  it('copies the exact selected retained route template', () => {
    const request = routeRequestFromTemplate(
      templates, templates.templates[0]!, context, 7,
    );
    expect(request.desired_configuration).toBe(
      templates.templates[0]!.desired_configuration,
    );
    expect(request.component_expectations).toBe(
      templates.templates[0]!.component_expectations,
    );
    expect(request.expected_admission_generation).toBe(7);
    expect(request.confirmed).toBe(true);
    expect(() => routeRequestFromTemplate(
      templates,
      { ...templates.templates[0]! },
      context,
      7,
    )).toThrow('current projection');
  });
});

describe('explicit ordinary activation', () => {
  const accepted: CCLCTransactionStatus = {
    kind: 'lifecycle', transactionId: 'prepare.1', phase: 'accepted',
    terminalResult: null, reconciliationRequired: false, error: null,
  };

  it('waits through accepted and pending owner status until terminal completion', async () => {
    const read = vi.fn()
      .mockResolvedValueOnce({ ...accepted, phase: 'pending' })
      .mockResolvedValueOnce({ ...accepted, terminalResult: 'succeeded' });
    const wait = vi.fn().mockResolvedValue(undefined);
    await awaitActivationCompletion(accepted, read, wait);
    expect(read).toHaveBeenCalledTimes(2);
    expect(wait).toHaveBeenCalledTimes(2);
  });

  it('stops on lost status, reconciliation, or owner failure without replaying an effect', async () => {
    const wait = vi.fn().mockResolvedValue(undefined);
    await expect(awaitActivationCompletion(accepted, async () => null, wait))
      .rejects.toThrow('no retained status');
    const read = vi.fn();
    await expect(awaitActivationCompletion(
      { ...accepted, reconciliationRequired: true }, read, wait,
    )).rejects.toThrow('requires reconciliation');
    expect(read).not.toHaveBeenCalled();
    await expect(awaitActivationCompletion(
      { ...accepted, terminalResult: 'failed_partial_effect' }, read, wait,
    )).rejects.toThrow('failed_partial_effect');
  });

  it('waits for Prepare to finish before Enable and positive Active', async () => {
    let state: CCLCComponentSnapshot['lifecycleState'] = 'declared';
    const commands: string[] = [];
    await activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: state }, ownerEpoch: 'owner.1' }),
      async (intent) => {
        commands.push(intent);
        state = intent === 'prepare' ? 'prepared' : 'active';
      },
    );
    expect(commands).toEqual(['prepare', 'enable']);
  });

  it('observes already Active without cycling it', async () => {
    const submit = vi.fn();
    await activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: 'active' }, ownerEpoch: 'owner.1' }),
      submit,
    );
    expect(submit).not.toHaveBeenCalled();
  });

  it('never enables after an unknown Prepare outcome or changed owner', async () => {
    const commands: string[] = [];
    await expect(activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: 'declared' }, ownerEpoch: 'owner.1' }),
      async (intent) => { commands.push(intent); throw new Error('outcome unknown'); },
    )).rejects.toThrow('outcome unknown');
    expect(commands).toEqual(['prepare']);
    let reads = 0;
    await expect(activateThroughOrdinaryLifecycle(
      async () => ({ snapshot, ownerEpoch: ++reads === 1 ? 'owner.1' : 'owner.2' }),
      vi.fn(),
    )).rejects.toThrow('owner or applied configuration changed');
  });

  it('uses authored order and stops the remaining sequence at the first failure', async () => {
    const commands: string[] = [];
    await expect(activateOrderedComponents(['z', 'a', 'later'], async (id) => {
      commands.push(id);
      if (id === 'a') throw new Error('missing dependency');
    })).rejects.toThrow('missing dependency');
    expect(commands).toEqual(['z', 'a']);
  });
});

it('does not submit Enable while the accepted Prepare is still unfinished', async () => {
  let state: CCLCComponentSnapshot['lifecycleState'] = 'declared';
  let finishPrepare: (() => void) | null = null;
  const commands: string[] = [];
  const accepted = new Promise<void>((resolve) => { finishPrepare = resolve; });
  const activation = activateThroughOrdinaryLifecycle(
    async () => ({ snapshot: { ...snapshot, lifecycleState: state }, ownerEpoch: 'owner.1' }),
    async (intent) => {
      commands.push(intent);
      if (intent === 'prepare') await accepted;
      state = intent === 'prepare' ? 'prepared' : 'active';
    },
  );
  // Drain the read-only microtasks; Prepare's owner completion remains unresolved.
  for (let i = 0; i < 5; ++i) await Promise.resolve();
  expect(commands).toEqual(['prepare']);
  finishPrepare!();
  await activation;
  expect(commands).toEqual(['prepare', 'enable']);
});

type FakeNode = {
  nodeType: number;
  nodeName: string;
  tagName: string;
  namespaceURI: string;
  ownerDocument: FakeDocument;
  style: Record<string, string>;
  addEventListener: () => void;
  removeEventListener: () => void;
  appendChild: () => void;
  insertBefore: () => void;
  removeChild: () => void;
  setAttribute: () => void;
  removeAttribute: () => void;
};

type FakeDocument = {
  nodeType: number;
  documentElement: FakeNode | null;
  activeElement: null;
  defaultView: Record<string, unknown>;
  addEventListener: () => void;
  removeEventListener: () => void;
  createElement: (tagName: string) => FakeNode;
};

let hookRoot: { render: (value: ReactNode) => void; unmount: () => void } | null = null;
let hook: ReturnType<typeof useCCLC> | null = null;
async function mountCCLC() {
  const document = {
    nodeType: 9,
    documentElement: null,
    activeElement: null,
    defaultView: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  } as FakeDocument;
  const makeNode = (tagName: string): FakeNode => ({
    nodeType: 1,
    nodeName: tagName.toUpperCase(),
    tagName: tagName.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document,
    style: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    appendChild: () => undefined,
    insertBefore: () => undefined,
    removeChild: () => undefined,
    setAttribute: () => undefined,
    removeAttribute: () => undefined,
  });
  document.createElement = makeNode;
  document.documentElement = makeNode('html');
  const window = {
    document,
    location: { href: 'http://composer.test/' },
    HTMLIFrameElement: class {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);

  const { createRoot } = await import('react-dom/client');
  hookRoot = createRoot(makeNode('div') as unknown as Element);
  function Harness({ enabled }: { enabled: boolean }) {
    const controller = useCCLC(enabled, 7);
    useEffect(() => { hook = controller; }, [controller]);
    return null;
  }
  const render = async (enabled: boolean) => {
    await act(async () => { hookRoot?.render(createElement(Harness, { enabled })); });
  };
  await render(true);
  await act(async () => { hook!.setComponentId('component.arm'); });
  await act(async () => { await hook!.refresh(); });
  return render;
}

function acceptedLifecycle(id: string) {
  return { replayStatus: 'accepted_new' as const, requestId: 'readback', transactionId: id,
    incidentRecovery: null, status: { kind: 'lifecycle' as const, transactionId: id,
      phase: 'succeeded', terminalResult: 'succeeded' as const,
      reconciliationRequired: false, error: null } };
}

async function flushHook() {
  await act(async () => { for (let i = 0; i < 20; ++i) await Promise.resolve(); });
}

describe('mounted explicit activation API boundaries', () => {
  let state: CCLCComponentSnapshot['lifecycleState'];
  beforeEach(() => {
    vi.useFakeTimers();
    state = 'declared';
    for (const methods of Object.values(apiMock)) {
      for (const method of Object.values(methods)) method.mockReset();
    }
    apiMock.execution.identity.mockImplementation(async () => execution);
    apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'cclc-owner.1' });
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, componentId: id, lifecycleState: state }));
    apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({ ...templates, componentId: id }));
    apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
      scenarioId: 'fixture', deploymentRevision: 'deployment.1',
      activationOrder: { retainedBundleId: snapshot.retainedBundleId,
        graphRevision: snapshot.graphRevision, componentIds: ['component.other', 'component.arm'] },
    } });
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => {
      state = request.intent === 'prepare' ? 'prepared' : 'active';
      return acceptedLifecycle(request.intent);
    });
  });
  afterEach(async () => {
    await act(async () => { hookRoot?.unmount(); });
    hookRoot = null; hook = null;
    vi.useRealTimers();
  });

  it.each(['prepare', 'enable'])('refuses same-owner configuration exchange on the exact %s submission read', async (intent) => {
    if (intent === 'enable') state = 'prepared';
    await mountCCLC();
    let reads = 0;
    apiMock.execution.identity.mockImplementation(async () => ++reads < 3 ? execution
      : { ...execution, graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' });
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, componentId: id,
      lifecycleState: state, ...(reads < 3 ? {} : { graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' }) }));
    apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({ ...templates, componentId: id,
      ...(reads < 3 ? {} : { graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' }) }));
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toContain('applied configuration changed');
  });

  it.each([false, true])('keeps the selected snapshot and routes coherent when sequence fails=%s', async (fails) => {
    await mountCCLC();
    const selected = hook!.snapshot;
    const selectedRoutes = hook!.routeTemplates;
    if (fails) apiMock.cclc.submitLifecycle.mockRejectedValue(new Error('owner unavailable'));
    await act(async () => { await hook!.activateSequence(); });
    expect(hook!.componentId).toBe('component.arm');
    expect(hook!.snapshot).toBe(selected);
    expect(hook!.routeTemplates).toBe(selectedRoutes);
    expect(apiMock.cclc.submitLifecycle.mock.calls[0]?.[0].controllable_component_id).toBe('component.other');
  });

  it('does not Enable while Prepare is accepted and pending', async () => {
    await mountCCLC();
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => {
      if (request.intent === 'enable') { state = 'active'; return acceptedLifecycle('enable'); }
      return { ...acceptedLifecycle('prepare'), status: { ...acceptedLifecycle('prepare').status, terminalResult: null } };
    });
    apiMock.cclc.lifecycleStatus.mockImplementation(async () => {
      state = 'prepared'; return acceptedLifecycle('prepare').status;
    });
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare']);
    await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare', 'enable']);
  });

  it.each(['cancel', 'disconnect'])('retains exact sent identity after %s loses the POST response; reconciliation never replays or continues', async (stop) => {
    const render = await mountCCLC();
    let sent: unknown;
    apiMock.cclc.submitLifecycle.mockImplementation((request, signal: AbortSignal) => {
      sent = request;
      return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(signal.reason), { once: true }));
    });
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activateSequence(); });
    expect(hook!.pendingActivationRequest).toBe(sent);
    if (stop === 'cancel') await act(async () => { hook!.cancelActivation(); });
    else await render(false);
    await act(async () => { await activation; });
    expect(hook!.pendingTransaction).toBeNull();
    expect(hook!.activationProgress).toContain('unknown request outcome');
    apiMock.cclc.lifecycleRequestResult.mockResolvedValue(null);
    await act(async () => { await hook!.reconcileTransaction(); });
    expect(apiMock.cclc.lifecycleRequestResult).toHaveBeenCalledWith(sent);
    expect(hook!.pendingActivationRequest).toBe(sent);
    expect(hook!.error).toContain('outcome remains unknown');
    apiMock.cclc.lifecycleRequestResult.mockResolvedValue(acceptedLifecycle('readback.transaction'));
    apiMock.cclc.lifecycleStatus.mockResolvedValue(acceptedLifecycle('readback.transaction').status);
    await act(async () => { await hook!.reconcileTransaction(); });
    expect(apiMock.cclc.lifecycleStatus).toHaveBeenCalledWith('readback.transaction');
    expect(hook!.pendingActivationRequest).toBeNull();
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  });

  it.each([null, { retainedBundleId: 'new.bundle', graphRevision: 'new.graph', componentIds: ['component.arm'] }])(
    'stops when the exact applied order disappears or changes before submission', async (replacement) => {
      await mountCCLC();
      apiMock.deploymentWorkspace.get.mockResolvedValueOnce({ managerApplied: {
        scenarioId: 'fixture', deploymentRevision: 'deployment.1', activationOrder: {
          retainedBundleId: snapshot.retainedBundleId, graphRevision: snapshot.graphRevision, componentIds: ['component.other'],
        },
      } }).mockResolvedValue({ managerApplied: { scenarioId: 'fixture', deploymentRevision: 'deployment.1', activationOrder: replacement } });
      await act(async () => { await hook!.activateSequence(); });
      expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
      expect(hook!.error).toContain('applied configuration changed');
      await flushHook();
    },
  );
});
