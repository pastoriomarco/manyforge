import { act, createElement, useEffect, useMemo } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  heldAuthoring,
  heldCatalog,
  heldOffer,
  heldProgram,
  heldRun,
} from '../testFixtures/heldRun';
import type {
  ApplyCommandRow,
  ApplyCommandRows,
  DeploymentWorkspaceResponse,
  JsonValue,
  ManagerApplyStatus,
  ProgramAuthoringDocument,
  ProgramCycleStatus,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { DirectControlMotionStatus } from '../types/robotControl';

// The owners' transports are faked; the controllers under test are the real
// authoring gate, workspace, runtime, Apply-after-Reset and preview hooks,
// wired as App wires them.
const apiMock = vi.hoisted(() => ({
  programs: {
    currentScenario: vi.fn(),
    applied: vi.fn(),
    appliedStatus: vi.fn(),
    nodeCorrectionPreview: vi.fn(),
  },
  programWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
    apply: vi.fn(),
    revert: vi.fn(),
    discard: vi.fn(),
  },
  deploymentWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
    discard: vi.fn(),
    openDraft: vi.fn(),
  },
  cellApply: { preview: vi.fn(), apply: vi.fn(), revertPreview: vi.fn(), revert: vi.fn() },
  scenarioDocuments: { discard: vi.fn() },
  authoringWorkspace: { recordResetApplyContext: vi.fn() },
  execution: { runtimeScaleControl: vi.fn() },
}));
vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(),
  api: apiMock,
}));

import { ApiError } from '../api/client';

import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  draftEditUnavailableReason,
  scenarioReplacementUnavailableReason,
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import {
  deploymentActionUnavailableReason,
  deploymentSavedDiffersFromApplied,
  deploymentUnapplied,
  scenarioActionRoute,
  type ScenarioAction,
} from '../state/documentStates';
import { CommandRefusalError } from '../state/commandRefusal';
import { codedErrorText } from '../state/componentPresentation';
import { managerApplySettlementKey } from '../state/managerApply';
import { resetApplyPhaseText } from '../state/resetApply';
import { programActionUnavailableReason } from '../state/authoringWorkspace';
import { useAppliedSettlementRefresh } from './useAppliedSettlementRefresh';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { useDeploymentWorkspace, type DeploymentWorkspaceController } from './useDeploymentWorkspace';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import { useHeldNodePreview, type HeldNodePreviewController } from './useHeldNodePreview';
import { useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { useProgramWorkspace, type ProgramWorkspaceController } from './useProgramWorkspace';
import { useResetApply, type ResetApplyController } from './useResetApply';
import { useInitialSceneReload, type InitialSceneReloadController } from './useInitialSceneReload';
import { applyPreviewFixture } from '../test/applyPreview';
import { endedApplyRow, endedProgramApplyRow } from '../test/applyCommandRow';

const digest = (fill: string) => `sha256:${fill.repeat(64)}`;
const NO_TARGETS: readonly DirectControlMotionStatus[] = [];
const APPLIED_DEPLOYMENT = digest('d');

type Hold = 'failure_pause' | 'operator_stop' | 'halt' | 'never_run' | 'stopping' | 'running'
  | 'containment_unconfirmed' | 'manual_operation' | 'idle';

interface Environment {
  previewVisible?: boolean;
  hold: Hold;
  cycle: Partial<ProgramCycleStatus>;
  admissionGeneration: number;
  draftRevision: number;
  programRevision: string;
  /** The Draft authoring source; the captured Program's by default. */
  authoring?: ProgramAuthoringDocument;
  deploymentRevision: number;
  deploymentContent: string;
  activation: ManagerApplyStatus | null;
  /** The Applied Program revision; the captured Program's by default. */
  appliedRevision?: string;
  /** Overrides the observed authoring admission (Idle opens it by default). */
  admissionOpen?: boolean;
  /** The Saved Deployment revision; the Applied one by default. */
  savedDeployment?: string;
  /** The application start of the snapshot and its execution status. */
  applicationStartId?: string;
  /** Composer's Apply-level command rows (OD-214); none by default. */
  applyCommands?: ApplyCommandRows;
}

/** The holds in which nothing moves and nothing can start. */
const STOPPED_HOLDS: readonly Hold[] = ['failure_pause', 'operator_stop', 'halt', 'never_run'];

/** Held on a failed node, the other motion stopped; one unapplied Program edit. */
const HELD: Environment = {
  hold: 'failure_pause',
  cycle: {},
  admissionGeneration: 9,
  draftRevision: 3,
  programRevision: digest('f'),
  deploymentRevision: 1,
  deploymentContent: APPLIED_DEPLOYMENT,
  activation: null,
};

function heldCycle(hold: Hold, patch: Partial<ProgramCycleStatus>): ProgramCycleStatus {
  const run = {
    ...heldRun(),
    state: hold === 'halt' ? 'halted'
      : hold === 'operator_stop' || hold === 'running' || hold === 'never_run'
        ? 'running' : 'intervention_required',
    activeOperationCount: hold === 'halt' ? 0 : null,
  };
  const offer = hold === 'halt' || hold === 'operator_stop' || hold === 'running'
    || hold === 'never_run'
    ? null
    : heldOffer({
      actions: [{
        actionId: 'retry_motion_from_current_state',
        requiresConfirmation: false, performsBackendReset: false,
        enabled: hold !== 'containment_unconfirmed',
        disabledReasonCode: hold === 'containment_unconfirmed'
          ? 'failure_pause_containment_unconfirmed' : null,
      }, {
        actionId: 'continue_with_failure',
        requiresConfirmation: true, performsBackendReset: false,
        enabled: true, disabledReasonCode: null,
      }, {
        actionId: 'reset_cycle',
        requiresConfirmation: true, performsBackendReset: true,
        enabled: true, disabledReasonCode: null,
      }],
    });
  const groupState = hold === 'stopping' ? 'stopping'
    : hold === 'running' ? 'running'
      : hold === 'halt' ? 'unavailable'
        : hold === 'never_run' ? 'ready' : 'held';
  return {
    cycleGeneration: 7,
    state: 'running',
    programRunId: 1,
    cycleClaimGeneration: 2,
    terminalOrSettlementReference: null,
    runRevision: 4,
    nodeStatusRevision: 1,
    runtimeControlRevision: 1,
    executionControlRevision: 1,
    causalRequestKey: null,
    causalDisplay: null,
    currentCommandKind: null, runnerCommands: { inProgress: null, ended: [] },
    failure: null,
    resetPhase: null, resetOptions: null,
    resetRobotPosition: { available: false, reason: 'unsupported' },
    allowedCommands: ['program_reset', 'program_motion_resume', 'program_resume_halt']
      .map((commandKind) => ({ commandKind, semanticTarget: {}, allowed: true, disabledReason: null })),
    run,
    runtimeControls: {
      programRunId: 1,
      runRevision: 4,
      runtimeControls: { runtimeControlRevision: 1 },
      recoveryOffer: offer,
      recoveryHolds: offer === null ? [] : [{
        motionGroupIds: ['all'], interventionStopScope: 'all', offer,
      }],
      operatorQuestions: [],
      executionSelections: {
        holdRevision: 1,
        motionHeld: groupState === 'held',
        groups: [{
          groupId: 'all', nodeCount: 3, heldNodeCount: groupState === 'held' ? 3 : 0,
          state: groupState, stopAllowed: false,
          startAllowed: false, startUnavailableReason: null,
          resumeAllowed: true, resumeUnavailableReason: null,
          runtimeScaleSelector: null, runtimeScaleUnavailableReason: null,
          resourceIds: [], stoppedBy: null, stoppedByNodeInstanceId: null,
        }],
      },
    },
    ...patch,
  } as unknown as ProgramCycleStatus;
}

/** The WebSocket Reset this suite's transport accepts: session 4, client sequence 118. */
const RESET_COMMAND = { controlSessionNumber: 4, clientSequence: '118' };
/** Another browser's command on the same Headless start. */
const OTHER_COMMAND = { controlSessionNumber: 5, clientSequence: '118' };

/** The Idle cycle whose last accepted command was `command`. */
function idleCycle(
  command: { controlSessionNumber: number; clientSequence: string } | null,
): Partial<ProgramCycleStatus> {
  return {
    state: 'idle_after_reset', cycleGeneration: 8, programRunId: null,
    // Class 0 key: session number in bytes 1-7, client sequence in bytes 8-15.
    causalRequestKey: command === null
      ? null
      : `00${command.controlSessionNumber.toString(16).padStart(14, '0')}${BigInt(command.clientSequence).toString(16).padStart(16, '0')}`,
    allowedCommands: [],
    run: null, runtimeControls: null,
  };
}

function programWorkspace(environment: Environment): ProgramWorkspaceResponse {
  const program = heldProgram();
  const applied = environment.appliedRevision ?? digest('a');
  const matches = environment.programRevision === applied;
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: identity(environment),
    draft: {
      draftId: 'draft-1',
      draftRevision: environment.draftRevision,
      programLoaded: true,
      authoring: environment.authoring ?? heldAuthoring(program),
      candidateAvailable: true,
      programRevision: environment.programRevision,
      program,
      diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} },
    },
    applicability: { applyReady: true, unavailableReason: null },
    saved: { available: true, matchesDraft: environment.programRevision === digest('a'), path: '/cell/program.yaml', programRevision: digest('a') },
    applied: { available: true, matchesDraft: matches, programRevision: applied, unavailableReason: null },
    hasUnsavedChanges: !matches,
    hasUnappliedChanges: !matches,
  } as unknown as ProgramWorkspaceResponse;
}

function deploymentWorkspace(environment: Environment): DeploymentWorkspaceResponse {
  return {
    schemaVersion: 'manyforge.composer_deployment_workspace.v2',
    applyCommands: { inProgress: null, ended: [] },
    authoringWorkspace: identity(environment),
    draft: {
      id: 'deployment-draft-1',
      revision: environment.deploymentRevision,
      document: { api_version: 'manyforge.deployment_intent.v3' },
      deploymentRevision: environment.deploymentContent,
      graphRevision: digest('9'),
      differsFromSaved: environment.deploymentContent !== APPLIED_DEPLOYMENT,
    },
    saved: {
      path: '/cell/cell.deployment.yaml',
      deploymentRevision: environment.savedDeployment ?? APPLIED_DEPLOYMENT,
      differsFromApplied: (environment.savedDeployment ?? APPLIED_DEPLOYMENT) !== APPLIED_DEPLOYMENT,
    },
    managerApplied: { activationOrder: null, deploymentRevision: APPLIED_DEPLOYMENT, scenarioId: 'cell' },
    runAdmission: { ready: environment.deploymentContent === APPLIED_DEPLOYMENT, reason: null },
    binding: null,
    scenarioCandidates: [],
    savedScenarioCandidates: [],
    unavailableReasons: [],
  };
}

function identity(environment: Environment) {
  return {
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: environment.draftRevision + environment.deploymentRevision,
    programDraftId: 'draft-1',
    programDraftRevision: environment.draftRevision,
    deploymentDraftId: 'deployment-draft-1',
    deploymentDraftRevision: environment.deploymentRevision,
  };
}

function publication(environment: Environment): PublishedAuthoringWorkspace {
  const current = programWorkspace(environment);
  const snapshot = {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v3' as const,
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: environment.draftRevision + environment.deploymentRevision,
    program: { draft: current.draft, saved: current.saved },
    deployment: {
      draftId: 'deployment-draft-1', draftRevision: environment.deploymentRevision,
      document: { api_version: 'manyforge.deployment_intent.v3' },
      savedPath: environment.savedDeployment === undefined ? null : '/cell/cell.deployment.yaml',
      savedDocument: environment.savedDeployment === undefined ? null
        : { api_version: 'manyforge.deployment_intent.v3', deployment_revision: environment.savedDeployment },
      differsFromSaved: environment.deploymentContent !== APPLIED_DEPLOYMENT
        || environment.savedDeployment !== undefined,
    },
    project: {
      documentPath: '/cell', readOnly: false, documentId: null, documentName: null, documentDescription: null,
      sourceDiagnostics: [], sourceDocumentDigest: null,
      sourceProgramDigest: null, sourceDeploymentDigest: null,
    },
  };
  return { identity: authoringWorkspaceIdentity(snapshot), snapshot };
}

function headlessSnapshot(environment: Environment): ComposerHeadlessState {
  const cycle = environment.hold === 'idle'
    ? heldCycle('failure_pause', { ...idleCycle(null), ...environment.cycle })
    : heldCycle(environment.hold, environment.cycle);
  return {
    applicationStartId: environment.applicationStartId ?? '3f9a0000000000a1',
    executionControlStatus: {
      applicationStartId: environment.applicationStartId ?? '3f9a0000000000a1',
      manualOperations: environment.hold === 'manual_operation'
        ? [{ resident: true, state: 'executing', operationId: 1 }]
        : [],
      cellStopAction: {
        available: true, unavailableReason: null, lastEventId: null, lastCompleted: null,
      },
    },
    componentOverview: { basis: { appliedProgramGeneration: 1 } },
    runtime: { sections: { manager: { sourceOwnerId: 'manager.1', payload: {} } } },
    authoringAdmission: {
      authoringAllowed: environment.admissionOpen ?? cycle.state === 'idle_after_reset',
      admissionGeneration: environment.admissionGeneration,
    },
    // The backend's published held-run verdict for each hold (the rule itself
    // is tested in Headless test_held_run_authoring.py).
    heldRunDraftEditAllowed: cycle.state === 'running'
      && STOPPED_HOLDS.includes(environment.hold),
    programCycle: cycle,
    applyCommands: environment.applyCommands ?? { inProgress: null, ended: [] },
    authoringWorkspace: identity(environment),
    authoringWorkspaceContainment: null,
  } as unknown as ComposerHeadlessState;
}

function authoringController(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
): AuthoringWorkspaceController {
  return {
    published,
    current: publicationIsCurrent(published, snapshot),
    unavailableReason: authoringUnavailableReason(published, snapshot),
    editUnavailableReason: draftEditUnavailableReason(published, snapshot),
    replaceUnavailableReason: scenarioReplacementUnavailableReason(published, snapshot),
    failedApplyHoldsCell: false,
    project: projectFacts(published),
    reading: false,
    error: null,
    refresh: async () => undefined,
    operation: null,
    beginMutation: () => true,
    finishMutation: () => undefined,
    observeApply: () => undefined,
    submissionUnconfirmed: false,
    correlatedApply: null,
    rechecking: false,
    recheckMessage: null,
    recheckSubmission: async () => undefined,
  };
}

interface Controllers {
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  runtime: ProgramRuntimeController;
  resetApply: ResetApplyController;
  preview: HeldNodePreviewController;
  sceneReload: InitialSceneReloadController;
}

const transport = { sendExecutionCommand: vi.fn() };
let root: { render: (node: unknown) => void; unmount: () => void } | null = null;
let hooks: Controllers | null = null;
// The workspace owners serve whatever Drafts the environment currently holds.
let served: Environment = HELD;

function Host({ environment }: { environment: Environment }) {
  const snapshot = useMemo(() => headlessSnapshot(environment), [environment]);
  const published = useMemo(() => publication(environment), [environment]);
  const authoring = useMemo(
    () => authoringController(published, snapshot),
    [published, snapshot],
  );
  const program = useProgramWorkspace(snapshot, authoring);
  const deployment = useDeploymentWorkspace(true, snapshot, authoring);
  const runtime = useProgramRuntime(
    true, snapshot, program.state.workspace,
    transport as unknown as HeadlessStateSource,
    deploymentUnapplied(deployment.workspace),
  );
  const resetApply = useResetApply({
    runtime, program, deployment, snapshot, activation: environment.activation,
  });
  const preview = useHeldNodePreview({
    visible: environment.previewVisible ?? true,
    runtime, workspace: program, snapshot, catalogEntries: heldCatalog, motionTargets: NO_TARGETS,
  });
  useAppliedSettlementRefresh({
    settlement: managerApplySettlementKey(environment.activation), runtime, program,
  });
  const sceneReload = useInitialSceneReload({
    runtime, snapshot, authoringAllowed: snapshot.authoringAdmission.authoringAllowed,
    deploymentApplyEnd: deployment.applyEnd, stoppedResetApply: null,
  });
  useEffect(() => {
    hooks = { program, deployment, runtime, resetApply, preview, sceneReload };
  }, [deployment, preview, program, resetApply, runtime, sceneReload]);
  return null;
}

async function mount(environment: Environment) {
  const node = (): Record<string, unknown> => ({
    nodeType: 1, nodeName: 'DIV', tagName: 'DIV', namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document, style: {}, addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {}, visibilityState: 'visible',
    addEventListener: () => undefined, removeEventListener: () => undefined, createElement: node,
  };
  document.documentElement = node();
  const window = {
    document, location: { href: 'http://composer.test/' }, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout.bind(globalThis),
    clearTimeout: globalThis.clearTimeout.bind(globalThis),
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const { createRoot } = await import('react-dom/client');
  root = createRoot(node() as unknown as Element) as unknown as typeof root;
  const render = (next: Environment) => act(async () => {
    served = next;
    root?.render(createElement(Host, { environment: next }));
  });
  await render(environment);
  // The Applied Program and the workspace projections arrive from the owners.
  await act(async () => { await Promise.resolve(); });
  await act(async () => { await Promise.resolve(); });
  return render;
}

const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve(); });

beforeEach(() => {
  apiMock.programs.currentScenario.mockResolvedValue({
    ready: true, unavailableReasons: [], currentScenario: null,
  });
  apiMock.programs.applied.mockResolvedValue({
    sourceGeneration: 1, recordDigest: digest('r'),
    programRevision: digest('a'), program: heldProgram(), programBinding: null,
  });
  apiMock.programs.nodeCorrectionPreview.mockReturnValue(new Promise(() => undefined));
  apiMock.programWorkspace.get.mockImplementation(async () => programWorkspace(served));
  apiMock.deploymentWorkspace.get.mockImplementation(async () => deploymentWorkspace(served));
  apiMock.authoringWorkspace.recordResetApplyContext.mockResolvedValue(undefined);
  transport.sendExecutionCommand.mockResolvedValue({
    ...RESET_COMMAND, disposition: 'received', code: 'received', result: null,
    receivedServerSequence: '1', receivedMonotonicNs: '1', emittedMonotonicNs: '1',
    requestKey: null, command: null,
  });
});

afterEach(async () => {
  if (root !== null) await act(async () => root?.unmount());
  root = null;
  hooks = null;
  served = HELD;
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('A refused stale Deployment fence', () => {
  // Refused before any effect, and the workspace moved: the hook refreshes it
  // instead of only reporting the refusal.
  const stale = () => new ApiError('Deployment Draft changed', 409, {
    detail: { code: 'composer_draft_revision_stale', message: 'Deployment Draft changed', effect: 'none' },
  });

  it('refreshes the workspace after a stale Draft edit', async () => {
    await mount({ ...HELD, hold: 'operator_stop' });
    const reads = apiMock.deploymentWorkspace.get.mock.calls.length;
    apiMock.deploymentWorkspace.mutate.mockRejectedValueOnce(stale());
    await act(async () => { await hooks!.deployment.mutate(['tick'], 'set', 1); });
    expect(apiMock.deploymentWorkspace.mutate).toHaveBeenCalledOnce();
    expect(apiMock.deploymentWorkspace.get).toHaveBeenCalledTimes(reads + 1);
    expect(hooks!.deployment.feedback).toBeNull();
    expect(hooks!.deployment.error).toBe('Deployment Draft changed');
    expect(hooks!.deployment.mutationAvailable).toBe(true);
  });
});

describe('Deployment Apply through the server', () => {
  const preview = applyPreviewFixture([
    { component_id: 'arm', disposition: 'replace', reasons: ['mount'], restarts: true },
    { component_id: 'gripper', disposition: 'retain', reasons: [], restarts: false }]);
  const edited = { ...HELD, hold: 'idle' as const, cycle: idleCycle(null), deploymentRevision: 2,
    deploymentContent: digest('e') };

  it('previews first and sends Apply once, only after the operator confirms', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    apiMock.cellApply.apply.mockResolvedValue(endedApplyRow({ outcome: 'succeeded', components: [
      { component_id: 'arm', disposition: 'replace', lifecycle_after: 'prepared' }] }, 'deployment_apply'));
    await act(async () => { await hooks!.deployment.applyDraft(); });
    expect(apiMock.cellApply.preview).toHaveBeenCalledOnce();
    expect(apiMock.cellApply.apply).not.toHaveBeenCalled();
    expect(hooks!.deployment.applyPreview).toEqual(preview);
    await act(async () => { await hooks!.deployment.confirmApply(); });
    // The Apply is bound to the Preview the operator saw.
    expect(apiMock.cellApply.apply).toHaveBeenCalledExactlyOnceWith(preview.scope, []);
    expect(hooks!.deployment.applyPreview).toBeNull();
    expect(hooks!.deployment.feedback).toBe('Applied: arm prepared.');
    // A second confirm without a new Preview sends nothing.
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(apiMock.cellApply.apply).toHaveBeenCalledOnce();
  });

  it('Cancel sends no Apply', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    await act(async () => { await hooks!.deployment.applyDraft(); });
    act(() => hooks!.deployment.cancelApplyConfirmation());
    expect(hooks!.deployment.applyPreview).toBeNull();
    expect(apiMock.cellApply.apply).not.toHaveBeenCalled();
  });

  it('shows the refusal during a run with its typed text, without retrying', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    const message = 'A Program run is in progress; Reset first.';
    apiMock.cellApply.apply.mockRejectedValueOnce(new ApiError(message, 409, {
      detail: { code: 'apply_refused_run_in_progress', message },
    }));
    await act(async () => { await hooks!.deployment.applyDraft(); });
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(apiMock.cellApply.apply).toHaveBeenCalledOnce();
    expect(hooks!.deployment.error).toBe(codedErrorText({ code: 'apply_refused_run_in_progress', message }));
    expect(hooks!.deployment.feedback).toBeNull();
    act(() => hooks!.deployment.dismissError());
    expect(hooks!.deployment.error).toBeNull();
  });

  it('shows a Draft changed after the Preview as a refusal to review again', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    const message = 'Draft changed after confirmation; review again';
    apiMock.cellApply.apply.mockRejectedValueOnce(new ApiError(message, 409, {
      detail: { code: 'activation_input_changed_after_confirmation', message },
    }));
    await act(async () => { await hooks!.deployment.applyDraft(); });
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.error).toBe(
      codedErrorText({ code: 'activation_input_changed_after_confirmation', message }));
    expect(apiMock.cellApply.apply).toHaveBeenCalledOnce();
  });

  it('answers at acceptance, shows the published step while it runs, and the outcome at its end', async () => {
    const render = await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    const row: ApplyCommandRow = {
      kind: 'deployment_apply', requestKey: 'f'.repeat(32), step: 'resetting_cycle',
      stepSource: null, acceptedAtNs: 1, end: null,
    };
    apiMock.cellApply.apply.mockResolvedValueOnce(row);
    await act(async () => { await hooks!.deployment.applyDraft(); });
    let settled = false;
    await act(async () => { void hooks!.deployment.confirmApply().then(() => { settled = true; }); });
    await render({ ...edited, applyCommands: { inProgress: {
      ...row, step: 'disabling', stepSource: { status: 'component_action', componentId: 'arm' },
    }, ended: [] } });
    // The step's words are tested with the busy tooltip (state/commandRows).
    expect(hooks!.deployment.applying).toBe(true);
    expect(settled).toBe(false);
    await render({ ...edited, applyCommands: { inProgress: null, ended: [{
      ...row, step: 'preparing_components', end: { outcome: 'succeeded', code: null, finishedAtNs: 2,
        details: { components: [{ component_id: 'arm', disposition: 'replace', lifecycle_after: 'prepared' }] } },
    }] } });
    await flush();
    expect(settled).toBe(true);
    expect(hooks!.deployment.feedback).toBe('Applied: arm prepared.');
    expect(hooks!.deployment.applying).toBe(false);
  });

  it('shows an Apply interrupted by a Stop with the catalogue text of its code', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    apiMock.cellApply.apply.mockResolvedValueOnce({
      kind: 'deployment_apply', requestKey: 'e'.repeat(32), step: 'preparing_components',
      stepSource: null, acceptedAtNs: 1,
      end: { outcome: 'interrupted', code: 'component_action_interrupted_by_stop', details: null, finishedAtNs: 2 },
    } satisfies ApplyCommandRow);
    await act(async () => { await hooks!.deployment.applyDraft(); });
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.error).toBe(
      `Apply failed: ${codedErrorText({ code: 'component_action_interrupted_by_stop', message: '' })}.`);
    expect(hooks!.deployment.error).not.toContain('component_action_interrupted_by_stop');
  });

  it('shows an Apply refused after acceptance as a refusal, with the preparer\'s typed cause', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    apiMock.cellApply.apply.mockResolvedValueOnce({
      kind: 'deployment_apply', requestKey: 'c'.repeat(32), step: 'previewing',
      stepSource: null, acceptedAtNs: '1',
      end: { outcome: 'refused', code: 'activation_preparation_failed',
        details: { cause_code: 'program_binding_unresolved' }, finishedAtNs: '2' },
    } satisfies ApplyCommandRow);
    await act(async () => { await hooks!.deployment.applyDraft(); });
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.error).toBe(codedErrorText({ code: 'activation_preparation_failed', message: '' }));
    expect(hooks!.deployment.errorCauseCode).toBe('program_binding_unresolved');
    expect(hooks!.deployment.feedback).toBeNull();
  });

  it('shows a failed ApplyResult as the error', async () => {
    await mount(edited);
    apiMock.cellApply.preview.mockResolvedValue(preview);
    apiMock.cellApply.apply.mockResolvedValueOnce(endedApplyRow({ outcome: 'failed', components: [],
      error: { code: 'fault_protective_stop', message: 'protective stop' } }, 'deployment_apply'));
    await act(async () => { await hooks!.deployment.applyDraft(); });
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.error).toBe(`Apply failed: ${codedErrorText({ code: 'fault_protective_stop', message: 'protective stop' })}.`);
  });
});

describe('Draft editing while a Program run exists', () => {
  it.each<Hold>(STOPPED_HOLDS)(
    'is editable when all motion is confirmed stopped (%s)',
    async (hold) => {
      await mount({ ...HELD, hold });
      expect(hooks!.program.mutationUnavailableReason).toBeNull();
      expect(hooks!.deployment.mutationUnavailableReason).toBeNull();
      // Other document actions keep requiring Idle.
      expect(hooks!.program.documentUnavailableReason).toContain('authoring requires Idle');
      expect(hooks!.deployment.documentUnavailableReason).toContain('authoring requires Idle');
      // A Program (and Scene) edit goes through the ordinary mutation path.
      apiMock.programWorkspace.mutate.mockResolvedValueOnce({
        mutation: 'viewer_draft_replace', details: {},
        workspace: programWorkspace({ ...HELD, hold, draftRevision: 4 }),
        authoringWorkspace: publication({ ...HELD, hold, draftRevision: 4 }).snapshot,
      });
      await act(async () => {
        await hooks!.program.mutate('viewer_draft_replace', { viewer: {} });
      });
      expect(apiMock.programWorkspace.mutate).toHaveBeenCalledExactlyOnceWith(
        expect.objectContaining({ draftId: 'draft-1', expectedAdmissionGeneration: 9 }),
        'viewer_draft_replace',
        { viewer: {} },
      );
      apiMock.deploymentWorkspace.mutate.mockResolvedValueOnce(
        deploymentWorkspace({ ...HELD, hold, deploymentRevision: 2, deploymentContent: digest('e') }),
      );
      await act(async () => { await hooks!.deployment.mutate(['tick'], 'set', 1); });
      expect(apiMock.deploymentWorkspace.mutate).toHaveBeenCalledOnce();
    },
  );

  it.each<Hold>(['stopping', 'running', 'containment_unconfirmed', 'manual_operation'])(
    'stays locked while motion is not confirmed stopped (%s)',
    async (hold) => {
      await mount({ ...HELD, hold });
      expect(hooks!.program.mutationUnavailableReason)
        .toContain('editing requires Idle or all Program motion stopped');
      expect(hooks!.deployment.mutationAvailable).toBe(false);
      await act(async () => {
        await hooks!.program.mutate('viewer_draft_replace', { viewer: {} });
      });
      expect(apiMock.programWorkspace.mutate).not.toHaveBeenCalled();
    },
  );

  it.each<Hold>(['failure_pause', 'never_run'])('locks again when motion starts (%s)', async (hold) => {
    const render = await mount({ ...HELD, hold });
    expect(hooks!.program.mutationAvailable).toBe(true);
    await render({ ...HELD, hold: 'running' });
    expect(hooks!.program.mutationAvailable).toBe(false);
  });
});

describe('Resume, Retry and Execute with an unapplied Draft', () => {
  it('are refused until the Draft is Applied or Discarded; Reset is not', async () => {
    const render = await mount(HELD);
    const runtime = hooks!.runtime;
    expect(runtime.composerRefusalReason).toMatch(/^Apply or Discard first: unapplied changes in Program/);
    expect(runtime.runnerAllows('program_reset')).toBe(true);
    const currentOffer = () => hooks!.runtime.state.controls!.recoveryHolds[0].offer;
    await act(async () => {
      runtime.applyRecoveryAction(currentOffer(), 'retry_motion_from_current_state');
      await runtime.resumeMotion('all');
    });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();

    // Discarded: the Draft is exactly what is Applied again.
    await render({ ...HELD, draftRevision: 4, programRevision: digest('a') });
    await flush();
    expect(hooks!.runtime.composerRefusalReason).toBeNull();
    await act(async () => {
      hooks!.runtime.applyRecoveryAction(currentOffer(), 'retry_motion_from_current_state');
    });
    expect(transport.sendExecutionCommand).toHaveBeenCalledExactlyOnceWith(
      'execution_recovery',
      expect.objectContaining({ action_id: 'retry_motion_from_current_state' }),
    );
  });

  it('sends Continue for the exact hold offer, and nothing for a stale revision', async () => {
    await mount({ ...HELD, draftRevision: 4, programRevision: digest('a') });
    const offer = hooks!.runtime.state.controls!.recoveryHolds[0].offer;
    await act(async () => {
      hooks!.runtime.applyRecoveryAction(
        { offerId: offer.offerId, offerRevision: offer.offerRevision + 1 }, 'continue_with_failure',
      );
    });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
    await act(async () => { hooks!.runtime.applyRecoveryAction(offer, 'continue_with_failure'); });
    expect(transport.sendExecutionCommand).toHaveBeenCalledExactlyOnceWith(
      'execution_recovery',
      expect.objectContaining({
        offer_id: offer.offerId,
        offer_revision: offer.offerRevision,
        action_id: 'continue_with_failure',
      }),
    );
    expect(hooks!.runtime.state.controlPending)
      .toBe(`recovery:${offer.offerId}:continue_with_failure`);
  });

  it('sends an operator Stop Retry for its focused offer, which no hold owns', async () => {
    const base = heldCycle('failure_pause', {});
    const stop = heldOffer({
      offerId: digest('5'),
      offerRevision: 2,
      recoveryClass: 'ordinary_operator_stop',
      actions: [{
        actionId: 'retry_motion_from_current_state',
        requiresConfirmation: false, performsBackendReset: false,
        enabled: true, disabledReasonCode: null,
      }],
    });
    await mount({
      ...HELD,
      draftRevision: 4,
      programRevision: digest('a'),
      cycle: { runtimeControls: { ...base.runtimeControls!, recoveryOffer: stop, recoveryHolds: [] } },
    });
    expect(hooks!.runtime.state.controls!.recoveryHolds).toEqual([]);
    await act(async () => {
      hooks!.runtime.applyRecoveryAction(
        { offerId: stop.offerId, offerRevision: stop.offerRevision + 1 }, 'retry_motion_from_current_state',
      );
    });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
    await act(async () => { hooks!.runtime.applyRecoveryAction(stop, 'retry_motion_from_current_state'); });
    expect(transport.sendExecutionCommand).toHaveBeenCalledExactlyOnceWith(
      'execution_recovery',
      expect.objectContaining({
        offer_id: stop.offerId,
        offer_revision: stop.offerRevision,
        action_id: 'retry_motion_from_current_state',
      }),
    );
    expect(hooks!.runtime.state.controlPending)
      .toBe(`recovery:${stop.offerId}:retry_motion_from_current_state`);
  });

  it('keeps a held operator Stop offer actionable and shows a stale press refusal with its typed reason', async () => {
    // Offers carry no expiry: freshness is checked when the action is pressed.
    const base = heldCycle('failure_pause', {});
    const stop = heldOffer({
      offerId: digest('6'),
      offerRevision: 2,
      recoveryClass: 'ordinary_operator_stop',
      actions: [{
        actionId: 'replan_from_stopped_state',
        requiresConfirmation: false, performsBackendReset: false,
        enabled: true, disabledReasonCode: null,
      }],
    });
    await mount({
      ...HELD,
      draftRevision: 4,
      programRevision: digest('a'),
      cycle: { runtimeControls: { ...base.runtimeControls!, recoveryOffer: stop, recoveryHolds: [] } },
    });
    const refusal = {
      code: 'recovery_action_rejected',
      message: 'Recovery action rejected',
      details: { reason: 'observed_state_stale' },
      origin: null,
    };
    transport.sendExecutionCommand.mockRejectedValueOnce(new CommandRefusalError(refusal));
    await act(async () => { hooks!.runtime.applyRecoveryAction(stop, 'replan_from_stopped_state'); });
    await flush();
    expect(transport.sendExecutionCommand).toHaveBeenCalledExactlyOnceWith(
      'execution_recovery',
      expect.objectContaining({ offer_id: stop.offerId, action_id: 'replan_from_stopped_state' }),
    );
    expect(hooks!.runtime.state.controlPending).toBeNull();
    expect(hooks!.runtime.state.errorRefusal).toEqual(refusal);
    expect(hooks!.runtime.state.error).toContain('Recovery refused');
    expect(hooks!.runtime.state.error).toContain('observed_state_stale');
  });

  it('refuse Run of a run Executed but never Run until the Draft is Applied', async () => {
    await mount({ ...HELD, hold: 'never_run' });
    const runtime = hooks!.runtime;
    expect(runtime.composerRefusalReason).toMatch(/^Apply or Discard first/);
    await act(async () => { await runtime.resumeMotion('all'); });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });

  it('also refuse with an unapplied Deployment edit', async () => {
    await mount({ ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') });
    expect(hooks!.runtime.composerRefusalReason).toContain('unapplied changes in Deployment');
  });
});

describe('Apply while a run exists', () => {
  async function confirm() {
    let refusal: string | null = 'not called';
    await act(async () => { refusal = await hooks!.resetApply.confirm(); });
    return refusal;
  }

  it('waits while the Runner publishes another Program command in progress (OD-144)', async () => {
    const busy = 'Not available while another Program command is running: Resume, applying.';
    const render = await mount({ ...HELD, cycle: { runnerCommands: { inProgress: {
      kind: 'resume_motion', requestKey: 'a'.repeat(32), step: 'applying', acceptedAtNs: 1, end: null,
    }, ended: [] } } });
    expect(hooks!.runtime.runnerBusyReason).toBe(busy);
    expect(hooks!.resetApply.unavailableReason()).toBe(busy);
    await render(HELD);
    expect(hooks!.runtime.runnerBusyReason).toBeNull();
    expect(hooks!.resetApply.unavailableReason()).toBeNull();
  });

  it('Resets exactly the held run, waits for its Idle, then Applies exactly the confirmed Program Draft', async () => {
    const render = await mount(HELD);
    expect(hooks!.resetApply.runExists).toBe(true);
    expect(hooks!.resetApply.unavailableReason()).toBeNull();
    expect(await confirm()).toBeNull();
    const [context] = apiMock.authoringWorkspace.recordResetApplyContext.mock.calls[0];
    expect(context).toMatchObject({
      document: 'program', programRunId: 1, cycleGeneration: 7,
      draftId: 'draft-1', draftRevision: 3, draftContentRevision: digest('f'), appliedRevision: digest('a'),
    });
    const [kind, request] = transport.sendExecutionCommand.mock.calls[0] as [
      string, Record<string, JsonValue>,
    ];
    expect(kind).toBe('program_reset');
    // The Reset is a WebSocket command: no correlation or request id names it.
    expect(transport.sendExecutionCommand.mock.calls[0]).toHaveLength(2);
    expect(request).not.toHaveProperty('correlation_id');
    expect(request).toMatchObject({ program_run_id: 1, expected_cycle_generation: 7 });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'confirmed' });
    // A receipt is not settlement: nothing is Applied while the run is held.
    await render({ ...HELD, admissionGeneration: 10 });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();

    apiMock.programWorkspace.apply.mockResolvedValue(
      endedProgramApplyRow('program_apply', { applyId: 1, applyGeneration: 2 }),
    );
    const settled = { ...HELD, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) };
    await render(settled);
    await flush();
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ draftId: 'draft-1', expectedDraftRevision: 3, expectedAdmissionGeneration: 11 }),
    );
    expect(apiMock.programWorkspace.mutate).not.toHaveBeenCalled();
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'awaiting_apply', apply: { applyId: 1, applyGeneration: 2 },
    });
    // Only the exact activation's success completes it; nothing else follows.
    await render({
      ...settled,
      activation: { applyId: 1, applyGeneration: 2, state: 'succeeded', failure: null } as ManagerApplyStatus,
    });
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'applied', apply: { applyId: 1, applyGeneration: 2 },
    });
    expect(transport.sendExecutionCommand).toHaveBeenCalledOnce();
  });

  it('Deployment: Resets the held run, waits for its Idle, then Applies exactly the confirmed Preview', async () => {
    const edited = { ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') };
    const render = await mount(edited);
    expect(hooks!.resetApply.deploymentRunReset).toBe(true);
    const scope = { deployment_draft_revision: 2 };
    let refusal: string | null = 'not called';
    await act(async () => { refusal = await hooks!.resetApply.confirm('deployment', scope); });
    expect(refusal).toBeNull();
    const [context] = apiMock.authoringWorkspace.recordResetApplyContext.mock.calls[0];
    expect(context).toMatchObject({
      document: 'deployment', programRunId: 1, draftId: 'deployment-draft-1', draftRevision: 2,
      draftContentRevision: digest('e'), appliedRevision: APPLIED_DEPLOYMENT,
    });
    expect(apiMock.cellApply.apply).not.toHaveBeenCalled();
    apiMock.cellApply.apply.mockResolvedValue(endedApplyRow({ outcome: 'succeeded', components: [
      { component_id: 'arm', disposition: 'replace', lifecycle_after: 'prepared' }] }, 'deployment_apply'));
    await render({ ...edited, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) });
    await flush();
    expect(apiMock.cellApply.apply).toHaveBeenCalledExactlyOnceWith(scope, []);
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'applied', apply: null });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('Deployment: a failed server Apply after Reset ends the chain with its typed text', async () => {
    const edited = { ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') };
    const render = await mount(edited);
    await act(async () => { await hooks!.resetApply.confirm('deployment', {}); });
    apiMock.cellApply.apply.mockResolvedValue(endedApplyRow({ outcome: 'failed', components: [],
      error: { code: 'fault_protective_stop', message: 'protective stop' } }, 'deployment_apply'));
    await render({ ...edited, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) });
    await flush();
    expect(apiMock.cellApply.apply).toHaveBeenCalledOnce();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'failed', stage: 'apply' });
  });

  it('Deployment: refuses Reset and Apply without a confirmed Preview', async () => {
    await mount({ ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') });
    let refusal: string | null = null;
    await act(async () => { refusal = await hooks!.resetApply.confirm('deployment', null); });
    expect(refusal).toBe('Preview the Deployment Draft before Reset and Apply.');
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });

  it('sends a Scenario Discard whose only unapplied change is the Deployment', async () => {
    // The Program Draft matches Applied; only the Deployment Draft differs.
    const edited = {
      ...HELD, hold: 'idle' as const, cycle: idleCycle(null), programRevision: digest('a'),
      deploymentRevision: 2, deploymentContent: digest('e'),
    };
    await mount(edited);
    expect(hooks!.program.scenarioDraftInputResetEpoch).toBe(0);
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'discard', null))
      .toMatch(/^Nothing to apply/);
    apiMock.scenarioDocuments.discard.mockResolvedValue(programWorkspace(HELD));

    await act(async () => { hooks!.program.discardScenario(); });
    await flush();

    expect(apiMock.scenarioDocuments.discard).toHaveBeenCalledOnce();
    expect(apiMock.scenarioDocuments.discard.mock.calls[0][0]).toEqual(expect.objectContaining({
      deploymentDraftId: identity(edited).deploymentDraftId,
      expectedDeploymentDraftRevision: 2,
    }));
    expect(apiMock.programWorkspace.discard).not.toHaveBeenCalled();
    expect(hooks!.program.scenarioDraftInputResetEpoch).toBe(1);
  });

  it('keeps Deployment resets scoped and resets the Scenario only after Revert commits', async () => {
    const edited = {
      ...HELD, hold: 'idle' as const, cycle: idleCycle(null), programRevision: digest('a'),
      deploymentRevision: 2, deploymentContent: digest('e'), savedDeployment: digest('s'),
    };
    const render = await mount(edited);
    expect(hooks!.program.programDraftInputResetEpoch).toBe(0);
    expect(hooks!.program.scenarioDraftInputResetEpoch).toBe(0);
    expect(hooks!.deployment.deploymentDraftInputResetEpoch).toBe(0);
    expect(hooks!.deployment.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.deploymentWorkspace.discard.mockRejectedValueOnce(new Error('discard failed'));
    await act(async () => { await hooks!.deployment.discard(); });
    expect(hooks!.deployment.deploymentDraftInputResetEpoch).toBe(0);

    const discarded = { ...edited, deploymentRevision: 3, deploymentContent: APPLIED_DEPLOYMENT };
    apiMock.deploymentWorkspace.discard.mockResolvedValueOnce(deploymentWorkspace(discarded));
    await act(async () => { await hooks!.deployment.discard(); });
    expect(hooks!.deployment.deploymentDraftInputResetEpoch).toBe(1);
    expect(hooks!.program.programDraftInputResetEpoch).toBe(0);
    expect(hooks!.program.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.deploymentWorkspace.openDraft.mockRejectedValueOnce(new Error('open failed'));
    await act(async () => { await hooks!.deployment.open('/cell/other.deployment.yaml', true); });
    expect(hooks!.deployment.deploymentDraftInputResetEpoch).toBe(1);
    apiMock.deploymentWorkspace.openDraft.mockResolvedValueOnce(deploymentWorkspace(edited));
    await act(async () => { await hooks!.deployment.open('/cell/other.deployment.yaml', true); });
    expect(hooks!.deployment.deploymentDraftInputResetEpoch).toBe(2);
    expect(hooks!.program.programDraftInputResetEpoch).toBe(0);

    // Opening a document consumes the current admission generation. The owner
    // must publish the next generation before a Scenario-wide Revert can run.
    expect(hooks!.deployment.documentUnavailableReason).toMatch(/Waiting for updated backend authoring permissions/);
    let staleRevert: unknown = 'not called';
    await act(async () => { staleRevert = await hooks!.deployment.revert(); });
    expect(staleRevert).toBeNull();
    expect(apiMock.cellApply.revertPreview).not.toHaveBeenCalled();

    // `edited` carries the old generation. Advancing it mirrors the backend's
    // post-open admission update and clears only that stale-generation gate.
    const reopened = { ...edited, admissionGeneration: edited.admissionGeneration + 1 };
    await render(reopened);

    apiMock.cellApply.revertPreview.mockResolvedValueOnce(applyPreviewFixture([]));
    await act(async () => { await hooks!.deployment.revert(); });
    expect(apiMock.cellApply.revertPreview).toHaveBeenCalledOnce();
    expect(hooks!.deployment.applyPreviewKind).toBe('revert');
    expect(hooks!.deployment.scenarioDraftInputResetEpoch).toBe(0);
    apiMock.cellApply.revert.mockResolvedValueOnce(endedApplyRow({ outcome: 'failed', components: [],
      error: { code: 'fault_protective_stop', message: 'protective stop' } }, 'deployment_revert'));
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.cellApply.revertPreview.mockResolvedValueOnce(applyPreviewFixture([]));
    await act(async () => { await hooks!.deployment.revert(); });
    apiMock.deploymentWorkspace.get.mockRejectedValueOnce(new Error('postimage unavailable'));
    apiMock.cellApply.revert.mockResolvedValueOnce(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_revert'));
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.scenarioDraftInputResetEpoch).toBe(0);

    apiMock.cellApply.revertPreview.mockResolvedValueOnce(applyPreviewFixture([]));
    await act(async () => { await hooks!.deployment.revert(); });
    expect(apiMock.cellApply.revertPreview).toHaveBeenCalledTimes(3);
    apiMock.cellApply.revert.mockResolvedValueOnce(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_revert'));
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(hooks!.deployment.scenarioDraftInputResetEpoch).toBe(1);
    expect(hooks!.program.programDraftInputResetEpoch).toBe(0);
  });

  it('re-reads the Deployment owner after a Scenario Discard, so nothing is left to apply, revert or discard', async () => {
    // The Scenario routes as ScenarioActionControls computes them (no run).
    const route = (action: ScenarioAction) => {
      const program = hooks!.program;
      const deployment = hooks!.deployment;
      const programWorkspace = program.state.workspace;
      const workspace = deployment.workspace;
      return scenarioActionRoute(action, {
        reason: programActionUnavailableReason(programWorkspace, action, action === 'discard'
          ? program.mutationUnavailableReason : program.documentUnavailableReason),
        pending: action === 'revert'
          ? programWorkspace?.saved.programRevision !== programWorkspace?.applied.programRevision
          : action === 'apply' ? programWorkspace?.applied.matchesDraft === false
            : programWorkspace?.hasUnappliedChanges === true,
      }, {
        reason: deploymentActionUnavailableReason(workspace, action, action === 'discard'
          ? deployment.mutationUnavailableReason : deployment.documentUnavailableReason),
        pending: action === 'revert'
          ? deploymentSavedDiffersFromApplied(workspace) : deploymentUnapplied(workspace),
      }, deployment.comparisonUnavailableReason);
    };
    const edited = {
      ...HELD, hold: 'idle' as const, cycle: idleCycle(null), programRevision: digest('a'),
      deploymentRevision: 2, deploymentContent: digest('e'),
    };
    const render = await mount(edited);
    expect(route('discard').target).toBe('scenario');
    expect(route('apply').target).toBe('deployment');
    const discarded = { ...edited, deploymentRevision: 3, deploymentContent: APPLIED_DEPLOYMENT };
    apiMock.scenarioDocuments.discard.mockResolvedValue(programWorkspace(discarded));
    await act(async () => { hooks!.program.discardScenario(); });
    await flush();
    const reads = apiMock.deploymentWorkspace.get.mock.calls.length;
    // The owner re-read is held so the interim view can be observed.
    let land: () => void = () => undefined;
    apiMock.deploymentWorkspace.get.mockImplementationOnce(() => new Promise((resolve) => {
      land = () => resolve(deploymentWorkspace(discarded));
    }));
    // The shared publication of the discarded Draft arrives before the owner.
    await render(discarded);
    expect(apiMock.deploymentWorkspace.get).toHaveBeenCalledTimes(reads + 1);
    // Until it lands, the old owner comparisons are unknown, not certain.
    expect(hooks!.deployment.workspace?.draft.revision).toBe(3);
    expect(hooks!.deployment.workspace?.runAdmission.ready).toBe(false);
    expect(hooks!.deployment.workspace?.saved.differsFromApplied).toBeNull();
    // No Scenario action is offered on unknown comparisons; Draft edits stay available.
    const rechecking = { target: null, reason: 'Rechecking the Deployment Draft against Manager Applied…' };
    expect(route('discard')).toEqual(rechecking);
    expect(route('apply')).toEqual(rechecking);
    expect(route('revert')).toEqual(rechecking);
    expect(hooks!.deployment.mutationUnavailableReason).toBeNull();
    await act(async () => { land(); });
    await flush();
    expect(hooks!.deployment.workspace?.runAdmission.ready).toBe(true);
    expect(route('discard')).toEqual({ target: null, reason: 'Nothing to discard: the Scenario Drafts match Applied.' });
    expect(route('apply')).toEqual({ target: null, reason: 'Nothing to apply: the Scenario Drafts match Applied.' });
    expect(route('revert')).toEqual({ target: null, reason: 'Nothing to revert: the Saved Scenario matches Applied.' });
    expect(apiMock.deploymentWorkspace.get).toHaveBeenCalledTimes(reads + 1);
  });

  it('offers no Scenario action while a failed Deployment recheck leaves the comparisons unknown', async () => {
    const edited = {
      ...HELD, hold: 'idle' as const, cycle: idleCycle(null), programRevision: digest('a'),
      deploymentRevision: 2, deploymentContent: digest('e'),
    };
    const render = await mount(edited);
    const discarded = { ...edited, deploymentRevision: 3, deploymentContent: APPLIED_DEPLOYMENT };
    apiMock.deploymentWorkspace.get.mockRejectedValueOnce(new Error('owner unreachable'));
    await render(discarded);
    await flush();
    const reason = hooks!.deployment.comparisonUnavailableReason;
    expect(reason).toMatch(/^The Deployment could not be rechecked against Manager Applied: owner unreachable/);
    expect(scenarioActionRoute('discard', { reason: null, pending: false }, { reason: null, pending: true }, reason))
      .toEqual({ target: null, reason });
    expect(hooks!.deployment.mutationUnavailableReason).toBeNull();
    // A later successful read makes the comparisons current again.
    await act(async () => { await hooks!.deployment.refresh(); });
    await flush();
    expect(hooks!.deployment.comparisonUnavailableReason).toBeNull();
  });

  it('sends Revert on the one Apply path, never as an Apply of the Draft', async () => {
    // Saved differs from Applied while the Draft is the Applied one.
    const idle = {
      ...HELD, hold: 'idle' as const, cycle: idleCycle(null), admissionGeneration: 12,
      savedDeployment: digest('5'),
    };
    await mount(idle);
    const preview = applyPreviewFixture([
      { component_id: 'arm', disposition: 'replace', reasons: ['mount'], restarts: true }]);
    apiMock.cellApply.revertPreview.mockResolvedValue(preview);
    apiMock.cellApply.revert.mockResolvedValue(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_revert'));
    // Revert goes through Preview → confirm → Apply.
    await act(async () => { await hooks!.deployment.revert(); });
    expect(apiMock.cellApply.revertPreview).toHaveBeenCalledOnce();
    expect(apiMock.cellApply.revert).not.toHaveBeenCalled();
    expect(hooks!.deployment.applyPreviewKind).toBe('revert');
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(apiMock.cellApply.revert).toHaveBeenCalledExactlyOnceWith(preview.scope, []);
    expect(apiMock.cellApply.apply).not.toHaveBeenCalled();
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('rechecks the settled Reset identity before Apply: a later cycle refuses it', async () => {
    const render = await mount(HELD);
    expect(await confirm()).toBeNull();
    // Idle reached by this Reset, but authoring admission is not yet open.
    const idle = { ...HELD, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND), admissionOpen: false };
    await render(idle);
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'applying', sent: false, idle: { cycleGeneration: 8 } });
    // Meanwhile another request moved the cycle on; admission then opens.
    await render({
      ...idle, admissionOpen: true, admissionGeneration: 12,
      cycle: { ...idleCycle(OTHER_COMMAND), cycleGeneration: 9 },
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({
      kind: 'failed', stage: 'apply', unknown: false,
      message: expect.stringContaining('cycle changed after Reset reached Idle'),
    });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('reconciles a lost Reset reply from observation: unknown, no Apply, no resend', async () => {
    transport.sendExecutionCommand.mockRejectedValue(
      new CommandDeliveryUnconfirmedError(
        'Command delivery is unconfirmed; commands other than Stop are disabled.',
        { command: RESET_COMMAND },
      ),
    );
    const render = await mount(HELD);
    expect(await confirm()).toBeNull();
    const phase = hooks!.resetApply.sequence!.phase;
    expect(phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'unconfirmed' });
    expect(resetApplyPhaseText(phase)).toContain('unknown: reconciling');
    // The resynchronized snapshot still shows the held run.
    await render({ ...HELD, admissionGeneration: 10 });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'unconfirmed' });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
    // This Reset ends the run: settlement is proven by observation.
    apiMock.programWorkspace.apply.mockResolvedValue(endedProgramApplyRow('program_apply', null));
    await render({ ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) });
    await flush();
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'applied', apply: null });
    expect(transport.sendExecutionCommand).toHaveBeenCalledOnce();
  });

  it('never Applies on an Idle another request reached, nor a Draft changed meanwhile', async () => {
    const render = await mount(HELD);
    expect(await confirm()).toBeNull();
    await render({ ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(OTHER_COMMAND) });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'failed', stage: 'reset' });

    await act(async () => { hooks!.resetApply.dismiss(); });
    await render(HELD);
    expect(await confirm()).toBeNull();
    await render({
      ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND), draftRevision: 4,
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'failed', stage: 'apply', unknown: false });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('Resets a run Executed but never Run before Applying', async () => {
    await mount({ ...HELD, hold: 'never_run' });
    expect(hooks!.resetApply.unavailableReason()).toBeNull();
    expect(await confirm()).toBeNull();
    const [kind, request] = transport.sendExecutionCommand.mock.calls[0] as [
      string, Record<string, JsonValue>, string,
    ];
    expect(kind).toBe('program_reset');
    expect(request).toMatchObject({ program_run_id: 1, expected_cycle_generation: 7 });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('is unavailable, with a reason, while motion is not stopped', async () => {
    await mount({ ...HELD, hold: 'stopping' });
    expect(hooks!.resetApply.unavailableReason()).toContain('all Program motion stopped');
    expect(await confirm()).toContain('all Program motion stopped');
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });
});

describe('Revert while a run exists (Reset and Revert)', () => {
  it('Program: Resets exactly the held run, waits for its Idle, then Reverts, never Applies', async () => {
    // Saved (a) differs from Applied (b); the Draft is the Applied one.
    const held = { ...HELD, programRevision: digest('b'), appliedRevision: digest('b') };
    const render = await mount(held);
    expect(hooks!.resetApply.unavailableReason('program', 'revert')).toBeNull();
    let refusal: string | null = 'not called';
    await act(async () => { refusal = await hooks!.resetApply.confirm('program', null, 'revert'); });
    expect(refusal).toBeNull();
    const [context] = apiMock.authoringWorkspace.recordResetApplyContext.mock.calls[0];
    expect(context).toMatchObject({ document: 'program', action: 'revert', programRunId: 1 });
    const [kind, request] = transport.sendExecutionCommand.mock.calls[0] as [string, Record<string, JsonValue>];
    expect(kind).toBe('program_reset');
    // The run's Reset never reloads: the Revert has not made the Saved Program active yet.
    expect(request).toMatchObject({ program_run_id: 1, reload_initial_scene: false });
    expect(apiMock.programWorkspace.revert).not.toHaveBeenCalled();
    apiMock.programWorkspace.revert.mockResolvedValue(
      endedProgramApplyRow('program_revert', { applyId: 3, applyGeneration: 1 }),
    );
    await render({ ...held, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) });
    await flush();
    expect(apiMock.programWorkspace.revert).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ draftId: 'draft-1', expectedDraftRevision: 3, expectedAdmissionGeneration: 11 }),
    );
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'awaiting_apply', apply: { applyId: 3, applyGeneration: 1 },
    });
  });

  it('Deployment: Resets the held run, waits for its Idle, then Reverts exactly the confirmed Preview', async () => {
    const held = { ...HELD, programRevision: digest('a'), savedDeployment: digest('5') };
    const render = await mount(held);
    expect(hooks!.resetApply.deploymentRunReset).toBe(true);
    const scope = { deployment_draft_revision: 1 };
    await act(async () => { await hooks!.resetApply.confirm('deployment', scope, 'revert'); });
    expect(apiMock.authoringWorkspace.recordResetApplyContext.mock.calls[0][0])
      .toMatchObject({ document: 'deployment', action: 'revert' });
    apiMock.cellApply.revert.mockResolvedValue(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_revert'));
    await render({ ...held, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(RESET_COMMAND) });
    await flush();
    expect(apiMock.cellApply.revert).toHaveBeenCalledExactlyOnceWith(scope, []);
    expect(apiMock.cellApply.apply).not.toHaveBeenCalled();
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'applied', apply: null });
  });

  it('Deployment: refuses Reset and Revert without a confirmed Preview', async () => {
    await mount({ ...HELD, programRevision: digest('a'), savedDeployment: digest('5') });
    let refusal: string | null = null;
    await act(async () => { refusal = await hooks!.resetApply.confirm('deployment', null, 'revert'); });
    expect(refusal).toBe('Preview the Deployment Revert before Reset and Revert.');
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });

  it('Deployment: previews a Revert while the run is held, as Apply', async () => {
    await mount({ ...HELD, programRevision: digest('a'), savedDeployment: digest('5') });
    apiMock.cellApply.revertPreview.mockResolvedValue(applyPreviewFixture([]));
    await act(async () => { await hooks!.deployment.revert(); });
    expect(apiMock.cellApply.revertPreview).toHaveBeenCalledOnce();
    expect(apiMock.cellApply.revert).not.toHaveBeenCalled();
  });
});

describe('Reload initial scene after a Program Apply or Revert', () => {
  const RESET_ALLOWED = [{ commandKind: 'program_reset', semanticTarget: {}, allowed: true, disabledReason: null }];
  const moved = { ...heldProgram(), scene: [{ resource_id: 'box', pose: { position: [1, 0, 0] } }] };
  const resetCalls = () => transport.sendExecutionCommand.mock.calls
    .filter(([kind]) => kind === 'program_reset')
    .map(([, request]) => request as Record<string, JsonValue>);
  async function applyNewScene(revision: string) {
    apiMock.programs.applied.mockResolvedValue({
      sourceGeneration: 2, recordDigest: digest('s'), programRevision: revision, program: moved, programBinding: null,
    });
    await act(async () => { await hooks!.runtime.refresh(); });
    await flush();
  }

  it('with no run: one idle Reset with reload, only after the new Program is Applied', async () => {
    const idle = {
      ...HELD, hold: 'idle' as const, admissionGeneration: 12,
      cycle: { ...idleCycle(null), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
    };
    await mount(idle);
    act(() => { hooks!.sceneReload.expect({ target: 'program', targetProgramRevision: digest('f'), reload: true }); });
    await flush();
    expect(resetCalls()).toEqual([]);
    await applyNewScene(digest('f'));
    expect(resetCalls()).toEqual([expect.objectContaining({
      program_run_id: null, reload_initial_scene: true, reset_robot_position: false,
    })]);
    // Sent once, whatever re-renders.
    await flush();
    expect(resetCalls()).toHaveLength(1);
  });

  it('without the option: no Reset', async () => {
    const idle = {
      ...HELD, hold: 'idle' as const, admissionGeneration: 12,
      cycle: { ...idleCycle(null), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
    };
    await mount(idle);
    act(() => { hooks!.sceneReload.expect({ target: 'program', targetProgramRevision: digest('f'), reload: false }); });
    await applyNewScene(digest('f'));
    expect(resetCalls()).toEqual([]);
    expect(hooks!.sceneReload.tracking).toMatchObject({ expectation: null, reload: null });
  });

  it('with a run: the run\'s Reset first, then the Apply, then the reload Reset', async () => {
    const render = await mount(HELD);
    act(() => { hooks!.sceneReload.expect({ target: 'program', targetProgramRevision: digest('f'), reload: true }); });
    await act(async () => { await hooks!.resetApply.confirm(); });
    expect(resetCalls()).toEqual([expect.objectContaining({ program_run_id: 1, reload_initial_scene: false })]);
    apiMock.programWorkspace.apply.mockResolvedValue(
      endedProgramApplyRow('program_apply', { applyId: 1, applyGeneration: 2 }),
    );
    await render({
      ...HELD, hold: 'idle' as const, admissionGeneration: 11,
      cycle: { ...idleCycle(RESET_COMMAND), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
    });
    await flush();
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
    // Not before the new Program is Applied.
    expect(resetCalls()).toHaveLength(1);
    await applyNewScene(digest('f'));
    expect(resetCalls()).toHaveLength(2);
    expect(resetCalls()[1]).toMatchObject({ program_run_id: null, reload_initial_scene: true });
  });

  it('a refused reload leaves the Apply applied and is not resent', async () => {
    const idle = {
      ...HELD, hold: 'idle' as const, admissionGeneration: 12,
      cycle: { ...idleCycle(null), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
    };
    await mount(idle);
    transport.sendExecutionCommand.mockRejectedValueOnce(new CommandRefusalError({
      code: 'authoring_not_ready', message: 'not ready', details: null, origin: null,
    }));
    act(() => { hooks!.sceneReload.expect({ target: 'program', targetProgramRevision: digest('f'), reload: true }); });
    await applyNewScene(digest('f'));
    await flush();
    expect(resetCalls()).toHaveLength(1);
    expect(hooks!.sceneReload.tracking).toMatchObject({ reload: null });
    expect(hooks!.runtime.state.applied?.programRevision).toBe(digest('f'));
    // The refusal is the Reset's own typed error, shown where a Reset's is.
    expect(hooks!.runtime.state.errorRefusal?.code).toBe('authoring_not_ready');
  });
});

describe('Reload initial scene after a Deployment Apply or Revert', () => {
  const RESET_ALLOWED = [{ commandKind: 'program_reset', semanticTarget: {}, allowed: true, disabledReason: null }];
  const resetCalls = () => transport.sendExecutionCommand.mock.calls
    .filter(([kind]) => kind === 'program_reset')
    .map(([, request]) => request as Record<string, JsonValue>);
  const idle = {
    ...HELD, hold: 'idle' as const, admissionGeneration: 12, deploymentRevision: 2, deploymentContent: digest('e'),
    savedDeployment: digest('5'),
    cycle: { ...idleCycle(null), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
  };

  it('Apply: one idle Reset with reload, only after its Apply command ended without error', async () => {
    await mount(idle);
    apiMock.cellApply.preview.mockResolvedValue(applyPreviewFixture([]));
    await act(async () => { await hooks!.deployment.applyDraft(); });
    act(() => { hooks!.sceneReload.expect({ target: 'deployment', reload: true }); });
    await flush();
    expect(resetCalls()).toEqual([]);
    let resolveApply: (row: unknown) => void = () => undefined;
    apiMock.cellApply.apply.mockReturnValueOnce(new Promise((resolve) => { resolveApply = resolve; }));
    let applying: Promise<unknown> = Promise.resolve();
    act(() => { applying = hooks!.deployment.confirmApply(); });
    await flush();
    // Never before the Apply is Applied.
    expect(resetCalls()).toEqual([]);
    await act(async () => {
      resolveApply(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_apply'));
      await applying;
    });
    await flush();
    expect(resetCalls()).toEqual([expect.objectContaining({
      program_run_id: null, reload_initial_scene: true, reset_robot_position: false,
    })]);
    await flush();
    expect(resetCalls()).toHaveLength(1);
  });

  it('Revert: one idle Reset with reload after it is Applied; none after a failed one or unchecked', async () => {
    await mount(idle);
    const revert = async (reload: boolean, outcome: 'succeeded' | 'failed') => {
      apiMock.cellApply.revertPreview.mockResolvedValueOnce(applyPreviewFixture([]));
      await act(async () => { await hooks!.deployment.revert(); });
      act(() => { hooks!.sceneReload.expect({ target: 'deployment', reload }); });
      apiMock.cellApply.revert.mockResolvedValueOnce(endedApplyRow(outcome === 'succeeded'
        ? { outcome, components: [] }
        : { outcome, components: [], error: { code: 'fault_protective_stop', message: 'protective stop' } },
      'deployment_revert'));
      await act(async () => { await hooks!.deployment.confirmApply(); });
      await flush();
    };
    await revert(true, 'failed');
    await revert(false, 'succeeded');
    expect(resetCalls()).toEqual([]);
    await revert(true, 'succeeded');
    expect(resetCalls()).toEqual([expect.objectContaining({ program_run_id: null, reload_initial_scene: true })]);
  });

  it('with a run: the run\'s Reset first, then the Revert, then the reload Reset', async () => {
    const held = { ...HELD, programRevision: digest('a'), savedDeployment: digest('5') };
    const render = await mount(held);
    act(() => { hooks!.sceneReload.expect({ target: 'deployment', reload: true }); });
    await act(async () => { await hooks!.resetApply.confirm('deployment', { deployment_draft_revision: 1 }, 'revert'); });
    expect(resetCalls()).toEqual([expect.objectContaining({ program_run_id: 1, reload_initial_scene: false })]);
    apiMock.cellApply.revert.mockResolvedValue(endedApplyRow({ outcome: 'succeeded', components: [] }, 'deployment_revert'));
    await render({
      ...held, hold: 'idle' as const, admissionGeneration: 11,
      cycle: { ...idleCycle(RESET_COMMAND), cycleClaimGeneration: null, allowedCommands: RESET_ALLOWED },
    });
    await flush();
    expect(apiMock.cellApply.revert).toHaveBeenCalledOnce();
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'applied', apply: null });
    await flush();
    expect(resetCalls()).toHaveLength(2);
    expect(resetCalls()[1]).toMatchObject({ program_run_id: null, reload_initial_scene: true });
  });
});

describe('Applied view after an Apply settles', () => {
  it('re-reads both Applied views on the settlement event, so Execute is enabled', async () => {
    const programRun = [{ commandKind: 'program_run', semanticTarget: {}, allowed: true, disabledReason: null }];
    const before: Environment = {
      ...HELD, hold: 'idle', admissionGeneration: 11,
      cycle: { allowedCommands: programRun } as Partial<ProgramCycleStatus>,
    };
    const render = await mount(before);
    expect(hooks!.runtime.composerRefusalReason).toMatch(/^Apply or Discard first/);
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'apply', null)).toBeNull();

    apiMock.programWorkspace.apply.mockResolvedValue(
      endedProgramApplyRow('program_apply', { applyId: 3, applyGeneration: 6 }),
    );
    await act(async () => { await hooks!.program.apply(); });
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
    const appliedReads = apiMock.programs.applied.mock.calls.length;

    // The Manager published and settled the Apply. The owners now serve the
    // new Applied Program; the control snapshot's Applied basis is unchanged
    // (its earlier wake raced the readback), so only settlement can re-read.
    const after: Environment = {
      ...before, admissionGeneration: 12, appliedRevision: digest('f'),
      activation: { applyId: 3, applyGeneration: 6, state: 'succeeded', failure: null } as ManagerApplyStatus,
    };
    apiMock.programs.applied.mockResolvedValue({
      sourceGeneration: 2, recordDigest: digest('q'),
      programRevision: digest('f'), program: heldProgram(), programBinding: null,
    });
    await render(after);
    await flush();
    await flush();
    expect(apiMock.programs.applied.mock.calls.length).toBeGreaterThan(appliedReads);
    expect(hooks!.runtime.state.applied?.programRevision).toBe(digest('f'));
    expect(hooks!.runtime.composerRefusalReason).toBeNull();
    // Nothing is left to apply or discard.
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'apply', null))
      .toMatch(/^Nothing to apply/);
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'discard', null))
      .toMatch(/^Nothing to apply/);
  });
});

describe('Advisory preview of the failed node in the ordinary editor', () => {
  it('clears a hidden preview and cannot restore an earlier reply after reopening', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      const authoring = heldAuthoring();
      authoring.forest[0].children[0].parameters.speed = 0.5;
      let resolveOld!: (value: unknown) => void;
      apiMock.programs.nodeCorrectionPreview.mockImplementationOnce(() => new Promise((resolve) => { resolveOld = resolve; }));
      const render = await mount({ ...HELD, authoring, previewVisible: true });
      await act(async () => { vi.advanceTimersByTime(1_000); });
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledTimes(1);
      await render({ ...HELD, authoring, previewVisible: false });
      expect(hooks!.preview.preview).toBeNull();
      expect(hooks!.preview.ghost).toBeNull();
      await render({ ...HELD, authoring, previewVisible: true });
      await act(async () => { vi.advanceTimersByTime(1_000); });
      await act(async () => { resolveOld({ unavailableReason: null, target: null,
        preview: { ok: false, error: { code: 'old', message: 'stale' } } }); await Promise.resolve(); });
      expect(hooks!.preview.preview?.status).toBe('pending');
      expect(hooks!.preview.statusText).not.toContain('stale');
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledTimes(2);
    } finally {
      vi.useRealTimers();
    }
  });
  it('is no longer ready once the native execution owner is replaced', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      const authoring = heldAuthoring();
      authoring.forest[0].children[0].parameters.speed = 0.5;
      apiMock.programs.nodeCorrectionPreview.mockResolvedValueOnce({
        unavailableReason: null, target: null,
        preview: { ok: false, error: { code: 'ik_failed', message: 'unreachable' } },
      });
      const render = await mount({ ...HELD, authoring });
      await act(async () => { vi.advanceTimersByTime(1_000); });
      await flush();
      expect(hooks!.preview.preview?.status).toBe('ready');
      await render({ ...HELD, authoring, applicationStartId: '3f9a0000000000a2' });
      expect(hooks!.preview.preview?.status).toBe('pending');
      await act(async () => { vi.advanceTimersByTime(1_000); });
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledTimes(2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('previews the failed node’s Draft values against the captured Program', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      const authoring = heldAuthoring();
      authoring.forest[0].children[0].parameters.speed = 0.5;
      const render = await mount({ ...HELD, authoring });
      expect(hooks!.preview.nodeInstanceId).toBe('move');
      expect(hooks!.preview.statusText).toBe('Previewing the edited motion…');
      await act(async () => { vi.advanceTimersByTime(1_000); });
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledExactlyOnceWith(
        expect.objectContaining({
          program_run_id: 1, node_instance_id: 'move', node_attempt_id: 1,
          proposed_inputs: [{ path: 'speed', value: 0.5 }],
        }),
      );
      // Unchanged Draft for that node: nothing to preview, no status line.
      await render({ ...HELD, authoring: heldAuthoring() });
      expect(hooks!.preview.statusText).toBeNull();
      expect(hooks!.preview.ghost).toBeNull();
    } finally {
      vi.useRealTimers();
    }
  });
});
