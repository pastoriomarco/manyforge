import { act, createElement, useEffect, type ReactNode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError } from '../api/client';

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
  CCLCLifecycleIntent,
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

function readiness(state: CCLCComponentSnapshot['lifecycleState'], ready = true): CCLCComponentSnapshot['readiness'] {
  return { requestOwnerEpoch: 'cclc-owner.1', evaluatedAtNs: 100,
    nextIntent: state === 'declared' || state === 'disabled' ? 'prepare' : state === 'prepared' ? 'enable' : null,
    requestedState: state === 'declared' || state === 'disabled' ? 'prepared' : state === 'prepared' ? 'active' : null,
    ready, error: ready ? null : { code: 'missing_reference', message: 'Measured feed unavailable', details: {} },
    providerOccurrences: [], measuredState: { required: false, available: null, error: null } };
}

const snapshot: CCLCComponentSnapshot = {
  controllableComponentId: 'component.arm',
  lifecycleGeneration: 4,
  lifecycleState: 'prepared',
  retainedBundleId: 'bundle.1',
    publicationGeneration: 1, activePublicPointerDigest: 'pointer.1',
  graphRevision: 'graph.1',
  providers: [],
  lifecycleTarget: null,
  lifecycleProgress: null,
  readiness: readiness('prepared'),
  domains: [{
    kind: 'controller',
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
    resources: ['joint.1'],
  }],
};

const templates: CCLCRouteTemplateSet = {
  controllableComponentId: 'component.arm',
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
        kind: 'controller',
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
      { ...accepted, terminalResult: 'failed' }, read, wait,
    )).rejects.toThrow('failed');
    await expect(awaitActivationCompletion(
      { ...accepted, terminalResult: 'connection_lost' }, read, wait,
    )).rejects.toThrow('connection_lost');
  });

  it('waits for Prepare to finish before Enable and positive Active', async () => {
    let state: CCLCComponentSnapshot['lifecycleState'] = 'declared';
    const commands: string[] = [];
    await activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: state, readiness: readiness(state) }, ownerEpoch: 'owner.1' }),
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

  it('never enables after an unknown Prepare outcome and tolerates an owner rejoin', async () => {
    const commands: string[] = [];
    await expect(activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: 'declared' }, ownerEpoch: 'owner.1' }),
      async (intent) => { commands.push(intent); throw new Error('outcome unknown'); },
    )).rejects.toThrow('outcome unknown');
    expect(commands).toEqual(['prepare']);
    let reads = 0;
    let state: CCLCComponentSnapshot['lifecycleState'] = 'declared';
    const rejoinCommands: CCLCLifecycleIntent[] = [];
    await activateThroughOrdinaryLifecycle(
      async () => ({ snapshot: { ...snapshot, lifecycleState: state, readiness: readiness(state) },
        ownerEpoch: ++reads === 1 ? 'owner.1' : 'owner.2' }),
      async (intent) => {
        rejoinCommands.push(intent);
        state = intent === 'prepare' ? 'prepared' : 'active';
      },
    );
    expect(rejoinCommands).toEqual(['prepare', 'enable']);
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
    async () => ({ snapshot: { ...snapshot, lifecycleState: state, readiness: readiness(state) }, ownerEpoch: 'owner.1' }),
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
async function mountCCLC(configuredComponentIds?: readonly string[]) {
  let renderedConfiguredComponentIds = configuredComponentIds;
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
  function Harness({ enabled, generation }: { enabled: boolean; generation: number | null }) {
    const controller = useCCLC(enabled, generation, renderedConfiguredComponentIds);
    useEffect(() => { hook = controller; }, [controller]);
    return null;
  }
  const render = async (
    enabled: boolean,
    generation: number | null = 7,
    ...nextConfiguredIds: [readonly string[] | null] | []
  ) => {
    if (nextConfiguredIds.length > 0) renderedConfiguredComponentIds = nextConfiguredIds[0] ?? undefined;
    await act(async () => { hookRoot?.render(createElement(Harness, { enabled, generation })); });
  };
  await render(true);
  await act(async () => { hook!.setControllableComponentId('component.arm'); });
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
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) }));
    apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({ ...templates, controllableComponentId: id }));
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

  function componentReadChanged(
    code = 'component_lifecycle_facts_stale', reason = 'component_read_changed', status = 409,
  ) {
    const message = 'the current component context changed during its read';
    return new ApiError(message, status, {
      detail: { code, message, details: { reason, admission_changed: 'true',
        admission_generation_before: '21', admission_generation_after: '22',
        authoring_allowed_before: 'false', authoring_allowed_after: 'true',
        owner_changed: 'false', snapshot_changed: 'false', target_facts_changed: 'false' } },
    });
  }

  it.each(['initial', 'before-prepare', 'after-prepare'] as const)(
    'rereads a transient %s component GET from fresh context without replaying lifecycle effects', async (phase) => {
      const render = await mountCCLC();
      await render(true, 21);
      let reads = 0;
      let conflicted = false;
      apiMock.cclc.component.mockImplementation(async (id: string) => {
        reads += 1;
        if (!conflicted && ((phase === 'initial' && reads === 1)
          || (phase === 'before-prepare' && reads === 3)
          || (phase === 'after-prepare' && state === 'prepared'))) {
          conflicted = true;
          throw componentReadChanged();
        }
        return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state),
          lifecycleGeneration: conflicted ? 41 : snapshot.lifecycleGeneration };
      });
      let activation!: Promise<void>;
      await act(async () => { activation = hook!.activate(); });
      expect(conflicted).toBe(true);
      expect(hook!.busy).toBe(true);
      expect(hook!.error).toBeNull();
      expect(hook!.componentHealth['component.arm']?.error).toBeNull();
      const readsAtConflict = reads;
      const effectsAtConflict = apiMock.cclc.submitLifecycle.mock.calls.length;
      const identityReadsAtConflict = apiMock.execution.identity.mock.calls.length;
      const ownerReadsAtConflict = apiMock.cclc.owner.mock.calls.length;
      await render(true, 22);
      await act(async () => { await vi.advanceTimersByTimeAsync(249); });
      expect(reads).toBe(readsAtConflict);
      expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(effectsAtConflict);
      await act(async () => { await vi.advanceTimersByTimeAsync(1); await activation; });
      expect(apiMock.execution.identity.mock.calls.length).toBeGreaterThan(identityReadsAtConflict);
      expect(apiMock.cclc.owner.mock.calls.length).toBeGreaterThan(ownerReadsAtConflict);
      expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent))
        .toEqual(['prepare', 'enable']);
      const freshRequest = apiMock.cclc.submitLifecycle.mock.calls[phase === 'after-prepare' ? 1 : 0][0];
      expect(freshRequest.expected_component_lifecycle_generation).toBe(41);
      expect(freshRequest.expected_admission_generation).toBe(22);
      expect(freshRequest.request_context.request_owner_epoch).toBe('cclc-owner.1');
      expect(hook!.snapshot?.lifecycleState).toBe('active');
      expect(hook!.error).toBeNull();
      expect(hook!.activationUnresolved).toBe(false);
    },
  );

  it('keeps the captured applied order when rereading a component conflict', async () => {
    await mountCCLC();
    apiMock.cclc.component.mockRejectedValueOnce(componentReadChanged());
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(); });
    const appliedReads = apiMock.deploymentWorkspace.get.mock.calls.length;
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
    expect(apiMock.deploymentWorkspace.get.mock.calls.length).toBe(appliedReads);
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent))
      .toEqual(['prepare', 'enable']);
    expect(apiMock.cclc.submitLifecycle.mock.calls.every(([request]) => (
      request.controllable_component_id === 'component.other'
    ))).toBe(true);
    expect(hook!.error).toBeNull();
    expect(hook!.activationProgress).toBe('Activation sequence completed.');
  });

  it('keeps control-mode checks for Execute offer and panel single-Activate reads', async () => {
    await mountCCLC();
    apiMock.execution.identity.mockResolvedValue({ ...execution, controlManagementMode: 'direct' });
    await act(async () => { await hook!.refresh(); });
    const directModeReason = hook!.readUnavailableReason;
    expect(directModeReason).toContain('direct native control');
    await expect(hook!.readActivationSequence(undefined, 'execute'))
      .rejects.toThrow('direct native control');
    await act(async () => { await hook!.activate(); });
    expect(hook!.error).toBe(directModeReason);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('uses the generic activation stop reason when current state is unavailable without a read reason', async () => {
    await mountCCLC();
    await act(async () => { hook!.setControllableComponentId('component.other'); });
    expect(hook!.snapshot).toBeNull();
    expect(hook!.readUnavailableReason).toBeNull();
    await act(async () => { await hook!.activate(); });
    expect(hook!.error).toBe('Activation stopped: current component state is unavailable.');
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('skips the execution projection only during sequence activation verification', async () => {
    await mountCCLC();
    // Each ordered component progresses on its own, so the sequence activates both.
    const states: Record<string, CCLCComponentSnapshot['lifecycleState']> = {
      'component.other': 'declared', 'component.arm': 'declared' };
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: states[id]!, readiness: readiness(states[id]!) }));
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => {
      states[request.controllable_component_id] = request.intent === 'prepare' ? 'prepared' : 'active';
      return acceptedLifecycle(request.intent);
    });
    apiMock.execution.identity.mockResolvedValue({ ...execution, controlManagementMode: 'direct' });
    await act(async () => { await hook!.refresh(); });
    expect(hook!.readUnavailableReason).toContain('direct native control');
    apiMock.execution.identity.mockResolvedValue(execution);
    const observation = await hook!.readActivationSequence(undefined, 'execute');
    expect(hook!.readUnavailableReason).toContain('direct native control');
    apiMock.execution.identity.mockClear();
    apiMock.execution.identity.mockResolvedValue({ ...execution, controlManagementMode: 'direct' });
    let succeeded = false;
    await act(async () => {
      succeeded = await hook!.activateSequence(observation.identity, observation.activationOrder);
    });
    expect(succeeded).toBe(true);
    expect(apiMock.execution.identity).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent))
      .toEqual(['prepare', 'enable', 'prepare', 'enable']);
    expect(hook!.readUnavailableReason).toContain('direct native control');
  });

  it('keeps the ordinary read torn-projection check', async () => {
    await mountCCLC();
    apiMock.execution.identity.mockResolvedValue({ ...execution, graphRevision: 'graph.torn' });
    await act(async () => { await hook!.refresh(); });
    expect(hook!.readUnavailableReason).toContain('projections are torn');
  });

  it('does not replace the captured component set with a later Deployment read', async () => {
    await mountCCLC();
    apiMock.cclc.component.mockRejectedValueOnce(componentReadChanged());
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(); });
    apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
      scenarioId: 'fixture', deploymentRevision: 'deployment.replacement',
      activationOrder: { retainedBundleId: snapshot.retainedBundleId,
        graphRevision: snapshot.graphRevision, componentIds: ['component.arm'] },
    } });
    await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.other', 'prepare'], ['component.other', 'enable']]);
    expect(hook!.error).toBeNull();
    expect(hook!.busy).toBe(false);
  });

  it('refuses when a successful confirmation read has no activation order', async () => {
    await mountCCLC();
    let observation!: Awaited<ReturnType<NonNullable<typeof hook>['readActivationSequence']>>;
    await act(async () => { observation = await hook!.readActivationSequence(undefined, 'execute'); });
    apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
      scenarioId: 'fixture', deploymentRevision: 'temporarily unavailable', activationOrder: null,
    } });
    let succeeded = false;
    await act(async () => {
      succeeded = await hook!.activateSequence(observation.identity, observation.activationOrder);
    });
    expect(succeeded).toBe(false);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toContain('no captured Deployment');
  });

  it('refuses a successful initial Deployment read with no activation order', async () => {
    await mountCCLC();
    apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
      scenarioId: 'fixture', deploymentRevision: 'deployment.1', activationOrder: null,
    } });
    await expect(hook!.readActivationSequence(undefined, 'execute'))
      .rejects.toThrow('no captured Deployment');
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('binds the first matching configured set that appears during the Execute offer read', async () => {
    const render = await mountCCLC(undefined);
    let releaseRead!: () => void;
    let signalReadStarted!: () => void;
    const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
    const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
    apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
      signalReadStarted();
      await readGate;
      return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) };
    });
    let offer!: ReturnType<NonNullable<typeof hook>['readActivationSequence']>;
    await act(async () => { offer = hook!.readActivationSequence(undefined, 'execute'); });
    await readStarted;
    await render(true, 7, ['component.other', 'component.arm']);
    releaseRead();
    let observation!: Awaited<typeof offer>;
    await act(async () => { observation = await offer; });
    expect(observation.activationOrder.componentIds).toEqual(['component.other', 'component.arm']);
    expect(observation.identity).toBe(JSON.stringify([
      JSON.stringify(['component.other', 'component.arm']),
      JSON.stringify(['bundle.1', 'graph.1', ['component.other', 'component.arm']]),
    ]));
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('accepts a matching first configured set that appears between Execute offer and confirmation', async () => {
    const render = await mountCCLC(undefined);
    const observation = await hook!.readActivationSequence(undefined, 'execute');
    await render(true, 7, ['component.other', 'component.arm']);
    let succeeded = false;
    await act(async () => {
      succeeded = await hook!.activateSequence(observation.identity, observation.activationOrder);
    });
    expect(succeeded).toBe(true);
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.other', 'prepare'], ['component.other', 'enable']]);
  });

  it('accepts a matching first configured set that appears during activation', async () => {
    const render = await mountCCLC(undefined);
    const observation = await hook!.readActivationSequence(undefined, 'execute');
    let releaseRead!: () => void;
    let signalReadStarted!: () => void;
    const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
    const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
    apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
      signalReadStarted();
      await readGate;
      return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) };
    });
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(observation.identity, observation.activationOrder); });
    await readStarted;
    await render(true, 7, ['component.other', 'component.arm']);
    releaseRead();
    let succeeded = false;
    await act(async () => { succeeded = await activation; });
    expect(succeeded).toBe(true);
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.other', 'prepare'], ['component.other', 'enable']]);
  });

  it('refuses a different first configured set after an unknown Execute offer', async () => {
    const render = await mountCCLC(undefined);
    const observation = await hook!.readActivationSequence(undefined, 'execute');
    await render(true, 7, ['component.arm']);
    let succeeded = true;
    await act(async () => {
      succeeded = await hook!.activateSequence(observation.identity, observation.activationOrder);
    });
    expect(succeeded).toBe(false);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toContain('confirmed component set changed');
  });

  it('aborts an in-flight step when the first configured set differs from the confirmed order', async () => {
    const render = await mountCCLC(undefined);
    const observation = await hook!.readActivationSequence(undefined, 'execute');
    let signalStarted!: () => void;
    const dispatchStarted = new Promise<void>((resolve) => { signalStarted = resolve; });
    const dispatch = { signal: null as AbortSignal | null };
    apiMock.cclc.submitLifecycle.mockImplementation((_request, signal: AbortSignal) => {
      dispatch.signal = signal;
      signalStarted();
      return new Promise((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(signal.reason), { once: true });
      });
    });
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(observation.identity, observation.activationOrder); });
    await dispatchStarted;
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
    await render(true, 7, ['component.arm']);
    expect(dispatch.signal?.aborted).toBe(true);
    await act(async () => { await activation; });
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
    expect(hook!.pendingActivationRequest).not.toBeNull();
    expect(hook!.activationProgress).toContain('unknown request outcome');
  });

  it('uses the last known admission generation when the current value is empty', async () => {
    const render = await mountCCLC();
    await render(true, null);
    await act(async () => { await hook!.activateSequence(); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.expected_admission_generation))
      .toEqual([7, 7]);
  });

  it('uses the latest admission generation observed during activation if the current value is empty at send', async () => {
    const render = await mountCCLC();
    let releaseRead!: () => void;
    let signalReadStarted!: () => void;
    const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
    const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
    apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
      signalReadStarted();
      await readGate;
      return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) };
    });
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(); });
    await readStarted;
    await render(true, 8);
    await render(true, null);
    releaseRead();
    await act(async () => { await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.expected_admission_generation))
      .toEqual([8, 8]);
  });

  it('keeps the captured component set across an undefined Deployment projection during activation', async () => {
    const configuredIds = ['component.other', 'component.arm'];
    const render = await mountCCLC(configuredIds);
    let releaseRead!: () => void;
    let signalReadStarted!: () => void;
    const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
    const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
    apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
      signalReadStarted();
      await readGate;
      return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) };
    });
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(); });
    await readStarted;
    // `null` is the harness's explicit way to pass an undefined prop.
    await render(true, 7, null);
    await render(true, 7, configuredIds);
    releaseRead();
    await act(async () => { await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.other', 'prepare'], ['component.other', 'enable']]);
    expect(hook!.error).toBeNull();
  });

  it('stops activation when a genuinely loaded empty Deployment set replaces the captured set', async () => {
    const configuredIds = ['component.other', 'component.arm'];
    const render = await mountCCLC(configuredIds);
    let releaseRead!: () => void;
    let signalReadStarted!: () => void;
    const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
    const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
    apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
      signalReadStarted();
      await readGate;
      return { ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) };
    });
    let activation!: Promise<boolean>;
    await act(async () => { activation = hook!.activateSequence(); });
    await readStarted;
    await render(true, 7, []);
    releaseRead();
    await act(async () => { await activation; });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toContain('configured components changed');
  });

  it('keeps persistent component-read conflicts effect-free until explicit Cancel', async () => {
    await mountCCLC();
    apiMock.cclc.component.mockClear();
    apiMock.cclc.component.mockRejectedValue(componentReadChanged());
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(249); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(2);
    await act(async () => { await vi.advanceTimersByTimeAsync(20_000); });
    expect(hook!.busy).toBe(true);
    expect(hook!.error).toBeNull();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    await act(async () => { hook!.cancelActivation(); await activation; });
    expect(hook!.busy).toBe(false);
    expect(hook!.error).toContain('cancelled');
    expect(hook!.activationUnresolved).toBe(false);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it.each([
    ['other-code', 'different_code', 'component_read_changed', 409],
    ['other-reason', 'component_lifecycle_facts_stale', 'different_reason', 409],
    ['other-status', 'component_lifecycle_facts_stale', 'component_read_changed', 500],
  ] as const)('surfaces %s component GET errors without retrying', async (_label, code, reason, status) => {
    await mountCCLC();
    apiMock.cclc.component.mockClear();
    const failure = componentReadChanged(code, reason, status);
    apiMock.cclc.component.mockRejectedValue(failure);
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toBe(failure.message);
    expect(hook!.busy).toBe(false);
  });

  it.each(['owner', 'route-templates', 'lifecycle-mutation'] as const)(
    'never retries the same conflict reported by %s instead of the component GET', async (source) => {
      if (source === 'route-templates') state = 'prepared';
      await mountCCLC();
      const failure = componentReadChanged();
      if (source === 'owner') apiMock.cclc.owner.mockRejectedValue(failure);
      else if (source === 'route-templates') apiMock.cclc.routeTemplates.mockRejectedValue(failure);
      else apiMock.cclc.submitLifecycle.mockRejectedValue(failure);
      await act(async () => { await hook!.activate(); });
      expect(hook!.error).toBe(failure.message);
      expect(hook!.busy).toBe(false);
      expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(source === 'lifecycle-mutation' ? 1 : 0);
      expect(hook!.activationUnresolved).toBe(source === 'lifecycle-mutation');
      if (source === 'lifecycle-mutation') {
        expect(hook!.pendingActivationRequest).toBe(apiMock.cclc.submitLifecycle.mock.calls[0][0]);
        expect(hook!.activationProgress).toContain('unknown request outcome');
      }
    },
  );

  it.each(['host-owner', 'cclc-owner', 'publication'] as const)(
    'does not stop activation on server-owned %s changes after a transient GET conflict', async (change) => {
      await mountCCLC();
      apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
        lifecycleState: state, readiness: readiness(state, false) }));
      let activation!: Promise<void>;
      await act(async () => { activation = hook!.activate(); });
      apiMock.cclc.component.mockRejectedValueOnce(componentReadChanged());
      await act(async () => { await vi.advanceTimersByTimeAsync(250); });
      expect(hook!.busy).toBe(true);
      expect(hook!.error).toBeNull();
      if (change === 'host-owner') {
        apiMock.execution.identity.mockResolvedValue({ ...execution, ownerEpoch: 'replacement-host' });
      } else if (change === 'cclc-owner') {
        apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'replacement-cclc' });
      } else {
        apiMock.execution.identity.mockResolvedValue({ ...execution, activePublicationGeneration: 2 });
      }
      apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
        lifecycleState: state, readiness: { ...readiness(state),
          requestOwnerEpoch: change === 'cclc-owner' ? 'replacement-cclc' : 'cclc-owner.1' },
        ...(change === 'publication' ? { publicationGeneration: 2, activePublicPointerDigest: 'pointer.2' } : {}) }));
      await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
      expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare', 'enable']);
      expect(hook!.error).toBeNull();
      expect(hook!.snapshot?.lifecycleState).toBe('active');
      expect(hook!.busy).toBe(false);
    },
  );

  // M6 (item 36): Recover acknowledges only the incident on screen.
  const withIncident = (incidentId: string | null): CCLCComponentSnapshot['lifecycleProgress'] => ({
    phase: 'blocked', nextIntent: 'recover', blockingCauseClass: 'collision',
    blockingReason: null, waitingSinceNs: 1, delayed: false,
    incident: incidentId === null ? null : {
      incidentId, causeClass: 'collision', vendorCodes: ['C31'], detectedAtNs: 1,
      summary: 'Collision detected', acknowledged: false,
    },
  });

  it('sends the shown incident with Recover and words a replaced incident refusal', async () => {
    let shown = 'incident.a';
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: readiness(state), lifecycleProgress: withIncident(shown) }));
    await mountCCLC();
    expect(hook!.snapshot?.lifecycleProgress?.incident?.incidentId).toBe('incident.a');
    shown = 'incident.b';
    apiMock.cclc.submitLifecycle.mockRejectedValue(new ApiError('changed', 409, {
      detail: { code: 'cclc_incident_changed', message: 'changed',
        details: { effect: 'not_started', expected_incident_id: 'incident.a', incident_id: 'incident.b' } },
    }));
    await act(async () => { await hook!.submitLifecycle('recover'); });
    expect(apiMock.cclc.submitLifecycle.mock.calls[0]![0].expected_incident_id).toBe('incident.a');
    expect(hook!.error).toContain('The incident changed since it was shown');
    // The refusal re-reads: incident B is now on screen.
    expect(hook!.snapshot?.lifecycleProgress?.incident?.incidentId).toBe('incident.b');
  });

  it('sends no expected incident when none is shown or for other intents', async () => {
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: readiness(state), lifecycleProgress: withIncident(null) }));
    await mountCCLC();
    await act(async () => { await hook!.submitLifecycle('recover'); });
    await act(async () => { await hook!.submitLifecycle('prepare'); });
    for (const [request] of apiMock.cclc.submitLifecycle.mock.calls) {
      expect(request.expected_incident_id).toBeUndefined();
    }
  });

  it('keeps ordinary explicit Refresh conflicts on the existing error path', async () => {
    await mountCCLC();
    apiMock.cclc.component.mockClear();
    const failure = componentReadChanged();
    apiMock.cclc.component.mockRejectedValue(failure);
    await act(async () => { await hook!.refresh(); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(1);
    expect(hook!.readUnavailableReason).toBe(failure.message);
    expect(hook!.snapshot).toBeNull();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it.each(['prepare', 'enable'])('refuses same-owner configuration exchange on the exact %s submission read', async (intent) => {
    if (intent === 'enable') state = 'prepared';
    await mountCCLC();
    let reads = 0;
    apiMock.execution.identity.mockImplementation(async () => ++reads < 3 ? execution
      : { ...execution, graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' });
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: readiness(state), ...(reads < 3 ? {} : { graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' }) }));
    apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({ ...templates, controllableComponentId: id,
      ...(reads < 3 ? {} : { graphRevision: 'replacement.graph', retainedBundleId: 'replacement.bundle' }) }));
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.error).toContain('applied configuration changed');
  });

  it.each(['retainedBundleId', 'graphRevision'] as const)(
    'uses the confirmed snapshot when %s changes before the first activation read', async (field) => {
      await mountCCLC();
      const changed = { [field]: `replacement.${field}` };
      apiMock.execution.identity.mockResolvedValue({ ...execution, ...changed });
      apiMock.cclc.component.mockImplementation(async (id: string) => ({
        ...snapshot, ...changed, controllableComponentId: id,
        lifecycleState: state, readiness: readiness(state),
      }));
      apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({
        ...templates, ...changed, controllableComponentId: id,
      }));
      await act(async () => { await hook!.activate(); });
      expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
      expect(hook!.error).toContain('applied configuration changed');
    },
  );

  it.each([false, true])('keeps the selected snapshot and routes coherent when sequence fails=%s', async (fails) => {
    await mountCCLC();
    const selected = hook!.snapshot;
    const selectedRoutes = hook!.routeTemplates;
    if (fails) apiMock.cclc.submitLifecycle.mockRejectedValue(new Error('owner unavailable'));
    await act(async () => { await hook!.activateSequence(); });
    expect(hook!.controllableComponentId).toBe('component.arm');
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
    let activation!: Promise<boolean>;
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
      // F1a (F1-CYCLE, audit L6): the order is bound once, at confirmation.
      const observation = await hook!.readActivationSequence(undefined, 'execute');
      apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
        scenarioId: 'fixture', deploymentRevision: 'deployment.1', activationOrder: replacement } });
      await act(async () => { await hook!.activateSequence(observation.identity, observation.activationOrder); });
      expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
      expect(hook!.error).toContain(replacement === null ? 'no captured Deployment' : 'applied configuration changed');
      await flushHook();
    },
  );
  it('names the provider-domain blocker before the owner error while waiting for readiness', async () => {
    await mountCCLC();
    const blocked = readiness(state, false);
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: { ...blocked, providerOccurrences: [{
        providerOccurrence: { controlDomainId: 'domain.io', controlDomainRevision: 'revision.1',
          lifecyclePlanCatalogDigest: 'catalog.1', providerInstanceId: 'provider.1', providerManifestId: 'manifest.1',
          providerManifestRevision: 'revision.1', providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
        health: { observedState: 'declared', facts: [], diagnostic: { code: 'io_provider_transport_unavailable',
          message: 'ROS service /plc_io/start is not available', details: {} } },
        decisionKind: null, error: null }] } }));
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(250); });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.activationProgress).toBe(
      'component.arm: Not ready · ROS service /plc_io/start is not available · [missing_reference] Measured feed unavailable');
    await act(async () => { hook!.cancelActivation(); await activation; });
  });
  it('keeps indefinite missing-measurement or fault readiness waits effect-free and cancelable', async () => {
    await mountCCLC();
    const readsBefore = apiMock.cclc.component.mock.calls.length;
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: { ...readiness(state, false), error: {
        code: 'component_fault', message: 'Controller fault requires repair', details: { reason: 'fault_present' } } } }));
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(20_000); });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.busy).toBe(true);
    expect(apiMock.cclc.component.mock.calls.length - readsBefore).toBe(82);
    expect(hook!.activationProgress).toBe('component.arm: Not ready · [component_fault] Controller fault requires repair · fault_present');
    await act(async () => { hook!.cancelActivation(); await activation; });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(hook!.activationUnresolved).toBe(false);
  });

  it('uses the existing passive cycle for all configured rows without effects or selected-route changes', async () => {
    await mountCCLC(['component.other', 'component.arm']);
    const selected = hook!.controllableComponentId;
    apiMock.cclc.component.mockClear();
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(apiMock.cclc.component.mock.calls.map(([id]) => id)).toEqual(['component.other', 'component.arm']);
    expect(Object.keys(hook!.componentHealth).sort()).toEqual(['component.arm', 'component.other']);
    expect(hook!.snapshot?.controllableComponentId).toBe(selected);
    expect(hook!.routeTemplates?.controllableComponentId).toBe(selected);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('does not overlap passive cycles while a component GET remains unresolved', async () => {
    await mountCCLC(['component.other', 'component.arm']);
    let resolveRead!: (value: CCLCComponentSnapshot) => void;
    apiMock.cclc.component.mockClear();
    apiMock.cclc.component.mockImplementation((id: string) => id === 'component.other'
      ? new Promise((resolve) => { resolveRead = resolve; })
      : Promise.resolve({ ...snapshot, controllableComponentId: id, lifecycleState: state, readiness: readiness(state) }));
    await act(async () => { await vi.advanceTimersByTimeAsync(20_000); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(1);
    await act(async () => { resolveRead({ ...snapshot, controllableComponentId: 'component.other', lifecycleState: state, readiness: readiness(state) }); });
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(2);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('requires new Prepared-step readiness after the explicit Prepare finishes', async () => {
    await mountCCLC();
    let enabled = false;
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: readiness(state, state !== 'prepared' || enabled) }));
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare']);
    enabled = true;
    await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare', 'enable']);
  });

  it('retains the accepted nonterminal transaction after Cancel and prohibits a new effect until exact terminal readback', async () => {
    await mountCCLC();
    const pending = { ...acceptedLifecycle('prepare'), status: { ...acceptedLifecycle('prepare').status,
      phase: 'accepted', terminalResult: null } };
    apiMock.cclc.submitLifecycle.mockResolvedValueOnce(pending);
    apiMock.cclc.lifecycleStatus.mockResolvedValue(pending.status);
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    expect(hook!.pendingActivationRequest).toBeNull();
    await act(async () => { hook!.cancelActivation(); await activation; });
    expect(hook!.activationUnresolved).toBe(true);
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
    state = 'prepared';
    apiMock.cclc.lifecycleStatus.mockResolvedValue(acceptedLifecycle('prepare').status);
    await act(async () => { await hook!.reconcileTransaction(); });
    expect(hook!.activationUnresolved).toBe(false);
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare', 'enable']);
  });

  it('does not publish a late positive readiness response or submit after Cancel', async () => {
    await mountCCLC();
    let resolveRead!: (value: CCLCComponentSnapshot) => void;
    apiMock.cclc.component.mockImplementation(() => new Promise((resolve) => { resolveRead = resolve; }));
    const previous = hook!.snapshot;
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    await act(async () => { hook!.cancelActivation(); resolveRead({ ...snapshot, lifecycleState: 'declared', readiness: readiness('declared') }); await activation; });
    expect(hook!.snapshot).toBe(previous);
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('keeps a readiness Prepare race refusal truthful without retrying or choosing another component', async () => {
    await mountCCLC();
    apiMock.cclc.submitLifecycle.mockResolvedValue({ ...acceptedLifecycle('prepare'), status: {
      ...acceptedLifecycle('prepare').status, terminalResult: 'failed', error: {
        code: 'component_lifecycle_transition_missing', message: 'No transition satisfied',
        details: { reason: 'no_transition_satisfied', observed_state: 'declared', observed_controller_loaded: 'unknown',
          observed_fault_present: 'false', observed_transport_ready: 'false' } } } });
    await act(async () => { await hook!.activate(); });
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
    expect(hook!.controllableComponentId).toBe('component.arm');
    expect(hook!.activationRefusal?.error.details.observed_controller_loaded).toBe('unknown');
    expect(hook!.activationUnresolved).toBe(false);
    expect(hook!.error).toBeNull();
  });

  it.each(['publication', 'host-owner'])('continues activation when only the server-owned %s changes', async (change) => {
    await mountCCLC();
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: state, readiness: readiness(state, false) }));
    let activation!: Promise<void>;
    await act(async () => { activation = hook!.activate(); });
    if (change === 'publication') {
      apiMock.execution.identity.mockResolvedValue({ ...execution, activePublicationGeneration: 2 });
      apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
        lifecycleState: state, readiness: readiness(state), publicationGeneration: 2, activePublicPointerDigest: 'pointer.2' }));
    } else {
      apiMock.execution.identity.mockResolvedValue({ ...execution, ownerEpoch: 'replacement-host-owner' });
      apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
        lifecycleState: state, readiness: readiness(state) }));
    }
    await act(async () => { await vi.advanceTimersByTimeAsync(250); await activation; });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['prepare', 'enable']);
    expect(hook!.componentHealth['component.arm']?.snapshot?.lifecycleState).toBe('active');
    expect(hook!.error).toBeNull();
  });

  it.each(['refresh', 'activate'] as const)('invalidates prior Ready when a newer %s GET fails', async (action) => {
    await mountCCLC();
    expect(hook!.componentHealth['component.arm']?.snapshot?.readiness.ready).toBe(true);
    apiMock.cclc.component.mockRejectedValue(new Error('Invalid current readiness schema'));
    await act(async () => { await hook![action](); });
    expect(hook!.snapshot).toBeNull();
    expect(hook!.componentHealth['component.arm']?.snapshot).toBeNull();
    expect(hook!.componentHealth['component.arm']?.error).toContain('Invalid current readiness schema');
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('does not let an obsolete failed GET erase a newer healthy read', async () => {
    await mountCCLC();
    let rejectOld!: (error: Error) => void;
    apiMock.cclc.component.mockImplementationOnce(() => new Promise((_resolve, reject) => { rejectOld = reject; }));
    let old!: Promise<void>;
    await act(async () => { old = hook!.refresh(); });
    await act(async () => { await hook!.refresh(); });
    const current = hook!.snapshot;
    await act(async () => { rejectOld(new Error('Obsolete failure')); await old; });
    expect(hook!.snapshot).toBe(current);
    expect(hook!.componentHealth['component.arm']?.error).toBeNull();
  });

  it('runs Retry as a new whole-current-list command and skips already Active components', async () => {
    await mountCCLC();
    const states: Record<string, CCLCComponentSnapshot['lifecycleState']> = {
      'component.other': 'active', 'component.arm': 'declared' };
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: states[id]!, readiness: readiness(states[id]!) }));
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => {
      states[request.controllable_component_id] = request.intent === 'prepare' ? 'prepared' : 'active';
      return acceptedLifecycle(request.intent);
    });
    await act(async () => { await hook!.activateSequence(); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.arm', 'prepare'], ['component.arm', 'enable']]);
    expect(hook!.controllableComponentId).toBe('component.arm');
    apiMock.cclc.submitLifecycle.mockClear();
    await act(async () => { await hook!.activateSequence(); });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('does not stop activation on a publication generation change', async () => {
    await mountCCLC();
    apiMock.cclc.component.mockClear();
    let publication = 1;
    apiMock.execution.identity.mockImplementation(async () => ({ ...execution, activePublicationGeneration: publication }));
    apiMock.cclc.component.mockImplementation(async (id: string) => {
      if (id === 'component.arm') publication = 2;
      return { ...snapshot, controllableComponentId: id, publicationGeneration: publication,
        activePublicPointerDigest: `pointer.${publication}`, lifecycleState: id === 'component.other' ? 'active' : state,
        readiness: readiness(id === 'component.other' ? 'active' : state) };
    });
    // The lifecycle owner handles publication freshness; activation does not pin it in the GUI.
    apiMock.execution.identity.mockImplementation(async () => {
      if (apiMock.cclc.component.mock.calls.length >= 2) publication = 2;
      return { ...execution, activePublicationGeneration: publication };
    });
    await act(async () => { await hook!.activateSequence(); });
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.arm', 'prepare'], ['component.arm', 'enable']]);
    expect(hook!.error).toBeNull();
  });

  it('deactivates exactly the listed changed components in the given order with positive Disabled observations', async () => {
    await mountCCLC();
    const states: Record<string, CCLCComponentSnapshot['lifecycleState']> = {
      'component.other': 'active', 'component.arm': 'active' };
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: states[id]!, readiness: readiness(states[id]!) }));
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => {
      states[request.controllable_component_id] = 'disabled';
      return acceptedLifecycle(request.intent);
    });
    let observation!: Awaited<ReturnType<NonNullable<typeof hook>['readActivationSequence']>>;
    await act(async () => { observation = await hook!.readActivationSequence(); });
    let result: Awaited<ReturnType<NonNullable<typeof hook>['deactivateComponents']>> = { succeeded: false };
    await act(async () => {
      result = await hook!.deactivateComponents(['component.arm', 'component.other'], observation.identity);
    });
    expect(result.succeeded).toBe(true);
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => [request.controllable_component_id, request.intent]))
      .toEqual([['component.arm', 'disable'], ['component.other', 'disable']]);
    // Already inactive components are observed, never cycled.
    apiMock.cclc.submitLifecycle.mockClear();
    await act(async () => { observation = await hook!.readActivationSequence(); });
    await act(async () => {
      result = await hook!.deactivateComponents(['component.arm'], observation.identity);
    });
    // Nothing was disabled, so no admission was consumed.
    expect(result).toEqual({ succeeded: true, consumedAdmissionGeneration: null });
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  });

  it('stops deactivation at the first refusal and never touches the next component', async () => {
    await mountCCLC();
    const states: Record<string, CCLCComponentSnapshot['lifecycleState']> = {
      'component.other': 'active', 'component.arm': 'active' };
    apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...snapshot, controllableComponentId: id,
      lifecycleState: states[id]!, readiness: readiness(states[id]!) }));
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => ({
      ...acceptedLifecycle(request.intent),
      status: { ...acceptedLifecycle(request.intent).status, terminalResult: 'failed' as const,
        error: { code: 'cell_not_idle', message: 'Only while the cell is idle', details: {} } },
    }));
    let observation!: Awaited<ReturnType<NonNullable<typeof hook>['readActivationSequence']>>;
    await act(async () => { observation = await hook!.readActivationSequence(); });
    let result: Awaited<ReturnType<NonNullable<typeof hook>['deactivateComponents']>> = { succeeded: true, consumedAdmissionGeneration: null };
    await act(async () => {
      result = await hook!.deactivateComponents(['component.arm', 'component.other'], observation.identity);
    });
    expect(result.succeeded).toBe(false);
    expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledOnce();
    expect(hook!.error).toBe('Only while the cell is idle');
  });

});
