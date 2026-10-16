import { StrictMode, act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type { AppliedProgramSelection, ProgramWorkspaceResponse } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { CCLCComponentSnapshot, CCLCLifecycleRequestBody } from '../types/cclc';
import { heldRun } from '../testFixtures/heldRun';
import { useCCLC, type CCLCController } from './useCCLC';
import { useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';

const apiMock = vi.hoisted(() => ({
  programs: { currentScenario: vi.fn(), applied: vi.fn() },
  deploymentWorkspace: { get: vi.fn() }, execution: { identity: vi.fn() },
  cclc: { owner: vi.fn(), component: vi.fn(), routeTemplates: vi.fn(),
    submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(), lifecycleRequestResult: vi.fn() },
}));
vi.mock('../api/client', async (original) => ({
  ...await original<typeof import('../api/client')>(), api: apiMock,
}));

// Both controllers run as App shares them. Only the owner transports and a
// minimal React DOM host are substituted; sequence/admission/ref timing is real.
function domHost() {
  const document: Record<string, unknown> = { nodeType: 9, activeElement: null,
    addEventListener: () => undefined, removeEventListener: () => undefined };
  const node = (tag: string): Record<string, unknown> => ({
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document,
    style: {}, addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, location: { href: 'http://composer.test/' },
    HTMLIFrameElement: class {}, setTimeout, clearTimeout };
  document.defaultView = window;
  vi.stubGlobal('document', document); vi.stubGlobal('window', window);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return node('div') as unknown as Element;
}
const ORDER = ['robot.z', 'robot.a']; // Deliberately differs from alphabetical order.
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
function component(id: string, state: CCLCComponentSnapshot['lifecycleState']): CCLCComponentSnapshot {
  return { componentId: id, lifecycleState: state, lifecycleGeneration: 1,
    retainedBundleId: 'bundle', graphRevision: 'graph', publicationGeneration: 1,
    activePublicPointerDigest: 'pointer.1', providers: [], domains: [], lifecycleTarget: null,
    lifecycleProgress: null, readiness: { requestOwnerEpoch: 'cclc.1', evaluatedAtNs: 1,
      nextIntent: state === 'disabled' || state === 'declared' ? 'prepare' : state === 'prepared' ? 'enable' : null,
      requestedState: state === 'disabled' || state === 'declared' ? 'prepared' : state === 'prepared' ? 'active' : null,
      ready: true, error: null, domains: [], measuredState: { required: false, available: null, error: null } } };
}
function accepted(transactionId: string) {
  return { replayStatus: 'accepted_new', requestId: transactionId, transactionId,
    incidentRecovery: null, status: { kind: 'lifecycle', transactionId, phase: 'succeeded',
      terminalResult: 'succeeded', reconciliationRequired: false, error: null } };
}
function facts(): { snapshot: ComposerHeadlessState; workspace: ProgramWorkspaceResponse } {
  const authoringWorkspace = { workspaceEpoch: 'workspace.1', workspaceRevision: 1,
    programDraftId: 'draft.1', programDraftRevision: 1,
    deploymentDraftId: 'deployment.1', deploymentDraftRevision: 1 };
  return {
    workspace: { authoringWorkspace, draft: { draftId: 'draft.1', draftRevision: 1,
      programLoaded: true, programRevision: 'program.revision' } } as ProgramWorkspaceResponse,
    snapshot: { receiverEpoch: 'receiver.1', authoringWorkspace,
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
      componentOverview: { basis: { appliedProgramOwnerEpoch: 'applied.1', appliedProgramGeneration: 1 } },
      runtime: { activeConfiguration: { retainedBundleId: 'bundle', graphRevision: 'graph' },
        sections: { manager: { sourceOwnerEpoch: 'manager.1', payload: { active: {
          retained_bundle_id: 'bundle', graph_revision: 'graph' } } } } },
      executionControlStatus: null,
      programCycle: { programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 1,
        state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
        terminalOrSettlementReference: null, runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null, executionControlRevision: 1,
        causalCorrelationId: null, causalSessionId: null, causalClientSequence: null,
        currentCommandKind: null, failure: null, resetPhase: null, resetOptions: null,
        resetRobotPosition: { available: false, reason: 'unsupported' }, recoveryOffer: null,
        run: null, runtimeControls: null,
        allowedCommands: [{ commandKind: 'program_run', allowed: true,
          disabledReason: null, semanticTarget: {} }] } } as unknown as ComposerHeadlessState,
  };
}
let cleanup: (() => Promise<void>) | null = null;
afterEach(async () => {
  await cleanup?.(); cleanup = null; vi.unstubAllGlobals(); vi.useRealTimers();
});
async function mount(states: Record<string, CCLCComponentSnapshot['lifecycleState']> = {
  'robot.z': 'disabled', 'robot.a': 'active',
}, resumed = false) {
  vi.useFakeTimers();
  for (const section of Object.values(apiMock)) for (const method of Object.values(section)) method.mockReset();
  const host = domHost();
  let { snapshot, workspace } = facts();
  if (resumed) snapshot = { ...snapshot, programCycle: { ...snapshot.programCycle,
    state: 'running', programRunId: 'run.1', run: { ...heldRun(), state: 'full_stopped' } } };
  apiMock.programs.currentScenario.mockResolvedValue({ ready: true, unavailableReasons: [], currentScenario: null });
  apiMock.programs.applied.mockResolvedValue({ scenarioId: 'scenario', sourceDocumentDigest: 'source',
    programRevision: 'program.revision', retainedBundleId: 'bundle', graphRevision: 'graph',
    recordDigest: 'record.1', sourceOwnerEpoch: 'applied.1',
    sourceGeneration: 1 } as AppliedProgramSelection);
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: { scenarioId: 'scenario',
    deploymentRevision: 'deployment.revision', activationOrder: {
      componentIds: ORDER, retainedBundleId: 'bundle', graphRevision: 'graph' } } });
  apiMock.execution.identity.mockResolvedValue({ ownerEpoch: 'execution.1', graphRevision: 'graph',
    retainedBundleId: 'bundle', activePublicationGeneration: 1, controlManagementMode: 'component_lifecycle' });
  apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'cclc.1' });
  apiMock.cclc.component.mockImplementation(async (id: string) => component(id, states[id]));
  apiMock.cclc.routeTemplates.mockImplementation(async (id: string) => ({
    componentId: id, graphRevision: 'graph', retainedBundleId: 'bundle', templates: [] }));
  const effectOrder: string[] = [];
  const completeLifecycle = (request: CCLCLifecycleRequestBody) => {
    effectOrder.push(`${request.controllable_component_id}:${request.intent}`);
    states[request.controllable_component_id] = request.intent === 'prepare' ? 'prepared' : 'active';
    return accepted(`transaction.${effectOrder.length}`);
  };
  apiMock.cclc.submitLifecycle.mockImplementation(async (request: CCLCLifecycleRequestBody) => completeLifecycle(request));
  const admissionsAtSend: number[] = [];
  const send = vi.fn(async (_kind: string, _payload: unknown, _correlation: string) => {
    admissionsAtSend.push(snapshot.authoringAdmission.admissionGeneration);
    return { disposition: 'received' };
  });
  const source = { sendExecutionCommand: send } as unknown as HeadlessStateSource;
  let runtime!: ProgramRuntimeController;
  let cclc!: CCLCController;
  function Harness() {
    cclc = useCCLC(true, snapshot.authoringAdmission.admissionGeneration, ORDER);
    runtime = useProgramRuntime(true, snapshot, workspace, source, false, cclc);
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host);
  const render = async () => { await act(async () => { root.render(<StrictMode><Harness /></StrictMode>); }); };
  await render();
  const unmount = async () => { await act(async () => { root.unmount(); }); };
  cleanup = unmount;
  expect(runtime.state.loading).toBe(false);
  const observe = async (change: (value: ReturnType<typeof facts>) => void) => {
    const next = { snapshot: structuredClone(snapshot), workspace: structuredClone(workspace) };
    change(next); snapshot = next.snapshot; workspace = next.workspace; await render();
  };
  return { get runtime() { return runtime; }, get cclc() { return cclc; }, send, states,
    effectOrder, admissionsAtSend, completeLifecycle, observe, unmount };
}

it('offers all configured inactive components and Cancel sends neither lifecycle nor Execute', async () => {
  const test = await mount();
  expect(test.runtime.commandAllowed('program_run')).toBe(true);
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.activationOffer?.components.map((row) => [row.componentId, row.lifecycleState]))
    .toEqual([['robot.z', 'disabled'], ['robot.a', 'active']]);
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  await act(async () => { test.runtime.cancelActivationExecute(); });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.runtime.activationOffer).toBeNull();
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});

it('all-Active sends one ordinary Execute, and an authoritative refusal does not reopen activation', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  test.send.mockRejectedValueOnce(new Error('Simulator unavailable'));
  await act(async () => { await test.runtime.run(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[0]).toBe('program_run');
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(test.runtime.activationOffer).toBeNull();
  expect(test.runtime.state.error).toContain('Simulator unavailable');
});

it('confirms all configured components in Applied priority, then waits for fresh permission and Executes once', async () => {
  const test = await mount({ 'robot.z': 'disabled', 'robot.a': 'disabled' });
  await act(async () => { await test.runtime.run(); });
  await test.observe(({ snapshot }) => { snapshot.authoringAdmission = { authoringAllowed: false, admissionGeneration: 20 }; });
  const prepare = deferred<void>();
  apiMock.cclc.submitLifecycle.mockImplementationOnce((request: CCLCLifecycleRequestBody) => (
    prepare.promise.then(() => test.completeLifecycle(request))
  ));
  let confirmation!: Promise<void>;
  await act(async () => {
    confirmation = test.runtime.confirmActivationExecute();
    await test.runtime.confirmActivationExecute();
  });
  expect(test.cclc.busy).toBe(true);
  expect(test.send).not.toHaveBeenCalled();
  await act(async () => { prepare.resolve(); await confirmation; });
  expect(test.effectOrder).toEqual(['robot.z:prepare', 'robot.z:enable', 'robot.a:prepare', 'robot.a:enable']);
  expect(test.send).not.toHaveBeenCalled();
  expect(test.runtime.activationWaiting).toBe(true);
  await test.observe(({ snapshot }) => { snapshot.authoringAdmission = { authoringAllowed: true, admissionGeneration: 21 }; });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.admissionsAtSend).toEqual([21]);
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.send).toHaveBeenCalledTimes(1);
});

it.each(['failed', 'unknown'])('a lifecycle %s outcome stops the sequence without Execute', async (failure) => {
  const test = await mount({ 'robot.z': 'disabled', 'robot.a': 'disabled' });
  await act(async () => { await test.runtime.run(); });
  apiMock.cclc.submitLifecycle.mockImplementationOnce(async () => {
    if (failure === 'unknown') throw new Error('Lifecycle reply lost');
    const result = accepted('failed');
    return { ...result, status: { ...result.status, terminalResult: 'failed_known_no_effect',
      error: { code: 'activation_failed', message: 'Provider refused', details: {} } } };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  expect(test.send).not.toHaveBeenCalled();
  expect(test.runtime.state.error).toContain('Execute was not sent');
  expect(test.cclc.error).toContain(failure === 'unknown' ? 'Lifecycle reply lost' : 'Provider refused');
});

it.each(['draft', 'owner', 'unmount'])('an intervening %s fences the pending Execute', async (change) => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  if (change === 'draft') {
    await test.observe(({ snapshot, workspace }) => {
      snapshot.authoringWorkspace.programDraftRevision += 1;
      workspace.draft.draftRevision += 1;
      workspace.draft.programRevision = 'changed-program';
    });
  } else if (change === 'owner') {
    apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'cclc.replacement' });
  } else {
    await test.unmount(); cleanup = null;
  }
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});

it('unknown fresh component observation shows an error with no offer or effects', async () => {
  const test = await mount();
  apiMock.cclc.component.mockRejectedValueOnce(new Error('Current component source is stale'));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.activationOffer).toBeNull();
  expect(test.runtime.state.error).toContain('Current component source is stale');
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});

it('Resume keeps its ordinary command and never enters activation precheck', async () => {
  const test = await mount(undefined, true);
  await act(async () => { await test.runtime.resumeFullStop(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[0]).toBe('program_resume_full_stop');
  expect(apiMock.deploymentWorkspace.get).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(test.runtime.activationOffer).toBeNull();
});


it('a busy shared lifecycle sequence returns false and never falls through to Execute', async () => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  const prepare = deferred<void>();
  apiMock.cclc.submitLifecycle.mockImplementationOnce((request: CCLCLifecycleRequestBody) => (
    prepare.promise.then(() => test.completeLifecycle(request))
  ));
  let panelActivation!: Promise<boolean>;
  await act(async () => { panelActivation = test.cclc.activateSequence(); });
  expect(test.cclc.busy).toBe(true);
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.runtime.state.error).toContain('Execute was not sent');
  expect(test.send).not.toHaveBeenCalled();
  await act(async () => { prepare.resolve(); await panelActivation; });
  expect(test.send).not.toHaveBeenCalled();
});

it.each(['runner', 'unmount'])('an intervening %s during accepted lifecycle work prevents late Execute', async (change) => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  const prepare = deferred<void>();
  apiMock.cclc.submitLifecycle.mockImplementationOnce((request: CCLCLifecycleRequestBody) => (
    prepare.promise.then(() => test.completeLifecycle(request))
  ));
  let confirmation!: Promise<void>;
  await act(async () => { confirmation = test.runtime.confirmActivationExecute(); });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  if (change === 'runner') await test.observe(({ snapshot }) => {
    snapshot.programCycle.programRunnerOwnerEpoch = 'replacement.runner';
  });
  else { await test.unmount(); cleanup = null; }
  await act(async () => { prepare.resolve(); await confirmation; });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  expect(test.send).not.toHaveBeenCalled();
});


it('a previously activated component becoming inactive before sequence completion prevents Execute', async () => {
  const test = await mount({ 'robot.z': 'disabled', 'robot.a': 'disabled' });
  await act(async () => { await test.runtime.run(); });
  apiMock.cclc.submitLifecycle.mockImplementation(async (request: CCLCLifecycleRequestBody) => {
    const result = test.completeLifecycle(request);
    if (request.controllable_component_id === 'robot.a' && request.intent === 'enable') {
      test.states['robot.z'] = 'disabled';
    }
    return result;
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.effectOrder).toEqual(['robot.z:prepare', 'robot.z:enable', 'robot.a:prepare', 'robot.a:enable']);
  expect(test.send).not.toHaveBeenCalled();
  expect(test.cclc.error).not.toBeNull();
  expect(test.runtime.state.error).toContain('Execute was not sent');
});

it.each(['graph', 'bundle'])('a fresh CCLC %s differing from the displayed Applied Program cannot offer activation', async (field) => {
  const test = await mount();
  const graph = field === 'graph' ? 'other.graph' : 'graph';
  const bundle = field === 'bundle' ? 'other.bundle' : 'bundle';
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: { scenarioId: 'scenario',
    deploymentRevision: 'deployment.revision', activationOrder: {
      componentIds: ORDER, retainedBundleId: bundle, graphRevision: graph } } });
  apiMock.execution.identity.mockResolvedValue({ ownerEpoch: 'execution.1', graphRevision: graph,
    retainedBundleId: bundle, activePublicationGeneration: 1, controlManagementMode: 'component_lifecycle' });
  apiMock.cclc.component.mockImplementation(async (id: string) => ({ ...component(id, test.states[id]),
    graphRevision: graph, retainedBundleId: bundle }));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.activationOffer).toBeNull();
  expect(test.runtime.state.error).not.toBeNull();
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});


it.each(['scenario', 'configuration', 'execution-owner'])('a changed %s while waiting for permission consumes the continuation without Execute', async (change) => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  await test.observe(({ snapshot }) => {
    snapshot.authoringAdmission = { authoringAllowed: false, admissionGeneration: 20 };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.runtime.activationWaiting).toBe(true);
  expect(test.send).not.toHaveBeenCalled();
  if (change === 'scenario') {
    apiMock.programs.currentScenario.mockResolvedValue({ ready: true, unavailableReasons: [],
      currentScenario: { scenarioId: 'another-scenario', sourceDocumentDigest: 'another-source',
        deploymentRevision: 'another-deployment', retainedBundleId: 'bundle', graphRevision: 'graph',
        programRevision: 'program.revision', programBindingId: 'another-binding' } });
    await act(async () => { await test.runtime.refresh(); });
  } else await test.observe(({ snapshot }) => {
    if (change === 'configuration') snapshot.runtime.activeConfiguration!.graphRevision = 'another.graph';
    else snapshot.executionControlStatus = { ownerEpoch: 'replacement.execution' } as unknown as
      NonNullable<ComposerHeadlessState['executionControlStatus']>;
  });
  expect(test.runtime.activationWaiting).toBe(false);
  expect(test.runtime.state.error).toContain('changed');
  await test.observe(({ snapshot }) => {
    snapshot.authoringAdmission = { authoringAllowed: true, admissionGeneration: 21 };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.send).not.toHaveBeenCalled();
});


it.each([
  [false, false], [true, false], [false, true], [true, true],
])('Reset forwards selectors %s/%s once and keeps its correlated identity', async (scene, position) => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  let result;
  await act(async () => { result = await test.runtime.reset({ correlationId: 'reset.correlation',
    reloadInitialScene: scene, resetRobotPosition: position }); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send).toHaveBeenCalledWith('program_reset', expect.objectContaining({
    correlation_id: 'reset.correlation', reload_initial_scene: scene, reset_robot_position: position,
  }), 'reset.correlation');
  expect(result).toMatchObject({ status: 'sent', reset: {
    requestId: (test.send.mock.calls[0]?.[1] as { request_id: string }).request_id,
    correlationId: 'reset.correlation', programRunId: null,
    programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 1,
  } });
});

it('plain Reset defaults both effects off and never resends unconfirmed delivery', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  test.send.mockRejectedValueOnce(new CommandDeliveryUnconfirmedError('reply lost'));
  let result;
  await act(async () => { result = await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[1]).toMatchObject({
    reload_initial_scene: false, reset_robot_position: false,
  });
  expect(result).toMatchObject({ status: 'unconfirmed', reset: {
    requestId: (test.send.mock.calls[0]?.[1] as { request_id: string }).request_id,
  } });
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(test.send).toHaveBeenCalledTimes(1);
});
