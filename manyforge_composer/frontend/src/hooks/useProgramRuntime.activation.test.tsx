import { StrictMode, act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type {
  AppliedProgramSelection,
  ProgramRuntimeControlProjection,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { CCLCComponentSnapshot, CCLCLifecycleRequestBody } from '../types/cclc';
import { heldRun } from '../testFixtures/heldRun';
import { useCCLC, type CCLCController } from './useCCLC';
import { useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import { CommandRefusalError } from '../state/commandRefusal';

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
  return { controllableComponentId: id, lifecycleState: state, lifecycleGeneration: 1,
    retainedBundleId: 'bundle', graphRevision: 'graph', publicationGeneration: 1,
    activePublicPointerDigest: 'pointer.1', providers: [], domains: [], lifecycleTarget: null,
    lifecycleProgress: null, readiness: { requestOwnerEpoch: 'cclc.1', evaluatedAtNs: 1,
      nextIntent: state === 'disabled' || state === 'declared' ? 'prepare' : state === 'prepared' ? 'enable' : null,
      requestedState: state === 'disabled' || state === 'declared' ? 'prepared' : state === 'prepared' ? 'active' : null,
      ready: true, error: null, providerOccurrences: [], measuredState: { required: false, available: null, error: null } } };
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
        resetRobotPosition: { available: false, reason: 'unsupported' },
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
  apiMock.programs.currentScenario.mockResolvedValue({ ready: false,
    unavailableReasons: ['scenario_not_ready'], currentScenario: null });
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
    controllableComponentId: id, graphRevision: 'graph', retainedBundleId: 'bundle', templates: [] }));
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
  expect(test.runtime.activationOffer?.components.map((row) => [row.controllableComponentId, row.lifecycleState]))
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

it('retains the owner refusal code for a motion wait until dismissed', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  const refusal = { code: 'invalid_state_transition', message: 'waiting',
    details: { reason: 'global_interrupt_source_not_ready', rule_id: 'door' }, origin: null };
  test.send.mockRejectedValueOnce(new CommandRefusalError(refusal));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.state.error).toContain('Motion waits:');
  expect(test.runtime.state.errorRefusal).toEqual(refusal);
  await act(async () => { test.runtime.dismissError(); });
  expect(test.runtime.state.error).toBeNull();
  expect(test.runtime.state.errorRefusal).toBeNull();
});

it('keeps an exact selection refusal visible and projects the next owner snapshot without resending', async () => {
  const test = await mount();
  const run = heldRun();
  const makeControls = (
    allowed: boolean,
    reason: string | null,
  ): ProgramRuntimeControlProjection => ({
    schemaVersion: 'manyforge.program_runtime_control_projection.v9',
    ownerId: 'program-runner.development-workbench',
    ownerEpoch: run.ownerEpoch,
    programRunId: run.capture.programRunId,
    runRevision: run.runRevision,
    runtimeControls: { runtimeControlRevision: 1 } as never,
    recoveryOffer: null,
    recoveryHolds: [],
    executionSelections: {
      selectionRevision: 7,
      motionHeld: true,
      groups: [{
        groupId: 'all', nodeCount: 1, heldNodeCount: 1, state: 'held',
        stopAllowed: true,
        startAllowed: allowed, startUnavailableReason: reason,
        resumeAllowed: allowed, resumeUnavailableReason: reason,
        runtimeScaleSelector: null, runtimeScaleUnavailableReason: null,
        resourceIds: [],
      }],
    },
  } as unknown as ProgramRuntimeControlProjection);
  const runningControls = makeControls(true, null);
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      state: 'running',
      programRunId: run.capture.programRunId,
      cycleClaimGeneration: 4,
      runRevision: run.runRevision,
      nodeStatusRevision: 1,
      runtimeControlRevision: 1,
      run,
      runtimeControls: runningControls,
    };
  });
  const currentScenarioReads = apiMock.programs.currentScenario.mock.calls.length;
  const appliedReads = apiMock.programs.applied.mock.calls.length;
  const refusal = 'Cell motion admission is unreconciled; Program Run or Resume remains blocked';
  test.send.mockRejectedValueOnce(new Error(refusal));

  await act(async () => {
    expect(await test.runtime.resumeMotion('all')).toBe(false);
  });

  expect(test.send).toHaveBeenCalledTimes(1);
  expect(apiMock.programs.currentScenario).toHaveBeenCalledTimes(currentScenarioReads);
  expect(apiMock.programs.applied).toHaveBeenCalledTimes(appliedReads);
  expect(test.runtime.state.error).toContain(refusal);
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();

  const blockedControls = makeControls(false, 'cell_motion_admission_unreconciled');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      runtimeControls: blockedControls,
    };
  });
  const group = test.runtime.state.controls?.executionSelections.groups[0];
  expect(group?.resumeAllowed).toBe(false);
  expect(group?.resumeUnavailableReason).toBe('cell_motion_admission_unreconciled');
  expect(test.send).toHaveBeenCalledTimes(1);
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
    return { ...result, status: { ...result.status, terminalResult: 'failed',
      error: { code: 'activation_failed', message: 'Provider refused', details: {} } } };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  expect(test.send).not.toHaveBeenCalled();
  expect(test.runtime.state.error).toContain('Execute was not sent');
  expect(test.cclc.error).toContain(failure === 'unknown' ? 'Lifecycle reply lost' : 'Provider refused');
});

it.each(['draft', 'unmount'])('an intervening %s fences the pending Execute', async (change) => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  if (change === 'draft') {
    await test.observe(({ snapshot, workspace }) => {
      snapshot.authoringWorkspace.programDraftRevision += 1;
      workspace.draft.draftRevision += 1;
      workspace.draft.programRevision = 'changed-program';
    });
  } else {
    await test.unmount(); cleanup = null;
  }
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.send).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});

// F1-ACT / F1-CYCLE: a server-owned owner change alone is not a GUI change; the
// lifecycle requests carry the current CCLC owner and Execute is sent once.
it.each(['cclc-owner', 'runner-owner'])('an intervening %s change alone does not fence the pending Execute', async (change) => {
  const test = await mount();
  await act(async () => { await test.runtime.run(); });
  const prepare = deferred<void>();
  apiMock.cclc.submitLifecycle.mockImplementationOnce((request: CCLCLifecycleRequestBody) => (
    prepare.promise.then(() => test.completeLifecycle(request))
  ));
  if (change === 'cclc-owner') apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'cclc.replacement' });
  let confirmation!: Promise<void>;
  await act(async () => { confirmation = test.runtime.confirmActivationExecute(); });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  if (change === 'runner-owner') await test.observe(({ snapshot }) => {
    snapshot.programCycle.programRunnerOwnerEpoch = 'replacement.runner';
  });
  await act(async () => { prepare.resolve(); await confirmation; });
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  expect(test.effectOrder).toEqual(['robot.z:prepare', 'robot.z:enable']);
  if (change === 'cclc-owner') {
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.request_context.request_owner_epoch))
      .toEqual(['cclc.replacement', 'cclc.replacement']);
  }
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[0]).toBe('program_run');
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

it('keeps the clicked Applied configuration when an Applied read becomes unavailable during the offer read', async () => {
  const test = await mount();
  let releaseRead!: () => void;
  let signalReadStarted!: () => void;
  const readStarted = new Promise<void>((resolve) => { signalReadStarted = resolve; });
  const readGate = new Promise<void>((resolve) => { releaseRead = resolve; });
  apiMock.cclc.component.mockImplementationOnce(async (id: string) => {
    signalReadStarted();
    await readGate;
    return component(id, 'disabled');
  });
  let readOffer!: Promise<void>;
  await act(async () => { readOffer = test.runtime.run(); });
  await readStarted;
  apiMock.programs.applied.mockRejectedValueOnce(new Error('Applied projection temporarily unavailable'));
  await act(async () => { await test.runtime.refresh(); });
  releaseRead();
  await act(async () => { await readOffer; });
  expect(test.runtime.activationOffer?.components.map((row) => row.controllableComponentId)).toEqual(ORDER);
  expect(test.runtime.state.error).toBeNull();
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
    // F1-CYCLE: a new Runner cycle (not an owner epoch alone) is the GUI-owned change.
    snapshot.programCycle.cycleGeneration += 1;
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


const KNOWN_SCENARIO = { ready: true, unavailableReasons: [], currentScenario: { scenarioId: 'scenario',
  sourceDocumentDigest: 'source', deploymentRevision: 'deployment.revision', retainedBundleId: 'bundle',
  graphRevision: 'graph', programRevision: 'program.revision', programBindingId: 'binding' } };

it.each(['scenario', 'draft', 'runner-cycle'])('a changed %s while waiting for permission consumes the continuation without Execute', async (change) => {
  const test = await mount();
  if (change === 'scenario') {
    // F1-EXEC: an unknown Scenario becoming known is not a change, so start from a known one.
    apiMock.programs.currentScenario.mockResolvedValue(KNOWN_SCENARIO);
    await act(async () => { await test.runtime.refresh(); });
  }
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
  } else await test.observe(({ snapshot, workspace }) => {
    if (change === 'draft') {
      snapshot.authoringWorkspace.programDraftRevision += 1;
      workspace.draft.draftRevision += 1;
    } else snapshot.programCycle.cycleGeneration += 1;
  });
  expect(test.runtime.activationWaiting).toBe(false);
  expect(test.runtime.state.error).toContain('changed');
  await test.observe(({ snapshot }) => {
    snapshot.authoringAdmission = { authoringAllowed: true, admissionGeneration: 21 };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.send).not.toHaveBeenCalled();
});

// F1-EXEC: server-owned configuration and execution-owner projections are not
// part of Execute's identity; the waiting continuation survives them.
it.each(['configuration', 'execution-owner'])('a changed %s while waiting for permission keeps the continuation', async (change) => {
  const test = await mount();
  if (change === 'execution-owner') {
    await test.observe(({ snapshot }) => { snapshot.executionControlStatus = executionOwner('execution.1'); });
  }
  await act(async () => { await test.runtime.run(); });
  await test.observe(({ snapshot }) => {
    snapshot.authoringAdmission = { authoringAllowed: false, admissionGeneration: 20 };
  });
  await act(async () => { await test.runtime.confirmActivationExecute(); });
  expect(test.runtime.activationWaiting).toBe(true);
  await test.observe(({ snapshot }) => {
    if (change === 'configuration') snapshot.runtime.activeConfiguration!.graphRevision = 'another.graph';
    else snapshot.executionControlStatus = executionOwner('replacement.execution');
  });
  expect(test.runtime.activationWaiting).toBe(true);
  expect(test.runtime.state.error ?? '').not.toContain('changed');
  expect(test.send).not.toHaveBeenCalled();
});


function executionOwner(ownerEpoch: string) {
  return { ownerEpoch } as unknown as NonNullable<ComposerHeadlessState['executionControlStatus']>;
}

// Execute keeps its GUI-owned identity while a robot Prepare changes the
// execution owner projection.
it.each([
  ['becomes available', null, ['execution.1'], 1],
  ['is briefly unavailable', 'execution.1', [null, 'execution.1'], 1],
  ['changes to another owner', 'execution.1', [null, 'replacement.execution'], 1],
] as const)('an execution owner that %s during Execute\'s own Prepare', async (_case, initial, during, sends) => {
  const test = await mount({ 'robot.z': 'disabled', 'robot.a': 'active' });
  if (initial !== null) {
    const owner = executionOwner(initial);
    await test.observe(({ snapshot }) => { snapshot.executionControlStatus = owner; });
  }
  await act(async () => { await test.runtime.run(); });
  const prepare = deferred<void>();
  apiMock.cclc.submitLifecycle.mockImplementationOnce((request: CCLCLifecycleRequestBody) => (
    prepare.promise.then(() => test.completeLifecycle(request))
  ));
  let confirmation!: Promise<void>;
  await act(async () => { confirmation = test.runtime.confirmActivationExecute(); });
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  for (const epoch of during) {
    const owner = epoch === null ? null : executionOwner(epoch);
    await test.observe(({ snapshot }) => { snapshot.executionControlStatus = owner; });
  }
  await act(async () => { prepare.resolve(); await confirmation; });
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  expect(test.send).toHaveBeenCalledTimes(sends);
  expect(test.effectOrder).toEqual(['robot.z:prepare', 'robot.z:enable']);
  expect(test.send.mock.calls[0]?.[0]).toBe('program_run');
  expect(test.runtime.state.error ?? '').not.toContain('changed');
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

it('Reset ignores a confirmation-time owner change and keeps the send-time owner fence', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
    snapshot.programCycle.programRunnerOwnerEpoch = 'runner.epoch.2';
  });
  let result;
  await act(async () => {
    result = await test.runtime.reset({ expected: { programRunId: null, cycleGeneration: 1 } });
  });
  expect(test.send).toHaveBeenCalledExactlyOnceWith('program_reset', expect.objectContaining({
    expected_owner_epoch: 'runner.epoch.2', expected_cycle_generation: 1,
  }), expect.any(String));
  expect(result).toMatchObject({ status: 'sent', reset: {
    programRunnerOwnerEpoch: 'runner.epoch.2', cycleGeneration: 1,
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

it('forwards current activation health and the named wait without replacing the captured offer', async () => {
  const test = await mount({ 'robot.z': 'disabled', 'robot.a': 'disabled' });
  await act(async () => { await test.runtime.run(); });
  const offer = test.runtime.activationOffer;
  apiMock.cclc.component.mockImplementation(async (id: string) => {
    const current = component(id, test.states[id]);
    return id !== 'robot.a' ? current : { ...current, readiness: { ...current.readiness,
      ready: false, error: { code: 'component_lifecycle_transition_missing',
        message: 'No lifecycle transition is satisfied', details: { reason: 'no_transition_satisfied' } } } };
  });
  let confirmation!: Promise<void>;
  await act(async () => { confirmation = test.runtime.confirmActivationExecute(); });
  expect(test.runtime.activationPending).toBe(true);
  expect(test.runtime.activationComponentHealth['robot.z'].snapshot?.lifecycleState).toBe('active');
  expect(test.runtime.activationComponentHealth['robot.a'].snapshot?.lifecycleState).toBe('disabled');
  expect(test.runtime.activationProgress).toBe('robot.a: Not ready · [component_lifecycle_transition_missing] No lifecycle transition is satisfied · no_transition_satisfied');
  expect(test.runtime.activationOffer).toBe(offer);
  expect(offer?.components.map((row) => row.lifecycleState)).toEqual(['disabled', 'disabled']);
  expect(test.effectOrder).toEqual(['robot.z:prepare', 'robot.z:enable']);
  expect(test.send).not.toHaveBeenCalled();
  await act(async () => { test.runtime.cancelActivationExecute(); await confirmation; });
  expect(test.send).not.toHaveBeenCalled();
});

// The synthesized failed-run notice: once per run revision, cleared by Reset
// reaching idle or by another run, never rebuilt after Dismiss or a command.
function failRun(value: ReturnType<typeof facts>, programRunId = 'run.1', runRevision = 4) {
  value.snapshot.programCycle = { ...value.snapshot.programCycle,
    state: 'terminal_awaiting_reset', programRunId, runRevision,
    allowedCommands: [{ commandKind: 'program_reset', allowed: true, disabledReason: null, semanticTarget: {} }],
    run: { ...heldRun(), state: 'failed', runRevision, treeStatus: 'failure',
      nodeStatuses: { move: 'failure' }, activeOperationCount: 0, reason: null,
      capture: { ...heldRun().capture, programRunId } } };
}
function cycleState(
  value: ReturnType<typeof facts>,
  state: ComposerHeadlessState['programCycle']['state'],
) {
  value.snapshot.programCycle.state = state;
  if (state === 'idle_after_reset') {
    value.snapshot.programCycle = { ...value.snapshot.programCycle,
      programRunId: null, runRevision: null, run: null };
  }
}

it('keeps a dismissed failed-run notice dismissed for that run revision', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await act(async () => { test.runtime.dismissError(); });
  expect(test.runtime.state.error).toBeNull();
  await test.observe(({ snapshot }) => { snapshot.programCycle.executionControlRevision = 2; });
  await test.observe(({ snapshot }) => { snapshot.programCycle.executionControlRevision = 3; });
  expect(test.runtime.state.error).toBeNull();
  // A new revision of the failed run is a new notice.
  await test.observe((value) => failRun(value, 'run.1', 5));
  expect(test.runtime.state.error).toContain('run.1: failed, revision 5');
});

it('clears the failed-run notice when Reset reaches idle and never rebuilds it during Reset', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  // A Reset observed from the owner alone clears the notice at idle.
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await test.observe((value) => cycleState(value, 'resetting'));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await test.observe((value) => cycleState(value, 'idle_after_reset'));
  expect(test.runtime.state.error).toBeNull();
  // This client's Reset supersedes it at once; the resetting window keeps the
  // failed run but does not rebuild the notice.
  await test.observe((value) => failRun(value, 'run.2'));
  expect(test.runtime.state.error).toContain('run.2: failed');
  await act(async () => { await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledWith('program_reset', expect.anything(), expect.anything());
  expect(test.runtime.state.error).toBeNull();
  await test.observe((value) => cycleState(value, 'resetting'));
  expect(test.runtime.state.error).toBeNull();
  await test.observe((value) => cycleState(value, 'idle_after_reset'));
  expect(test.runtime.state.error).toBeNull();
});

it('clears the failed-run notice when a new Run starts', async () => {
  const test = await mount({ 'robot.z': 'active', 'robot.a': 'active' });
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  // Observed without the idle snapshot in between: the next run replaces it.
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = { ...snapshot.programCycle, state: 'running', programRunId: 'run.2',
      runRevision: 1, run: { ...heldRun(), state: 'running', runRevision: 1, nodeStatuses: {},
        capture: { ...heldRun().capture, programRunId: 'run.2' } } };
  });
  expect(test.runtime.state.error).toBeNull();
});
