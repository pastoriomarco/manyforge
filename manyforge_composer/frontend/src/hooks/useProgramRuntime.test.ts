import { readFileSync } from 'node:fs';

import { afterEach, describe, expect, it, vi } from 'vitest';

import type {
  AppliedProgramSelection,
  PersistentKeyValues,
  ProgramRuntimeControlProjection,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import {
  buildProgramResetRequest,
  buildProgramStartRequest,
  controlPendingAfterProjection,
  persistentValuesFence,
  programPendingAfterProjection,
  programRunProjectionKey,
  programRuntimeAfterSnapshot,
  programCurrentStatusSummary,
  programMotionStopSettled,
  programRuntimeWakeKey,
  programTerminalFailureMessage,
  programResetReadinessMessage,
  PROGRAM_RESET_READINESS_MESSAGE,
} from './useProgramRuntime';

afterEach(() => {
  vi.restoreAllMocks();
});

const digest = (character: string) => `sha256:${character.repeat(64)}`;

function admissionFixtures() {
  const applied = {
    scenarioId: 'applied',
    sourceDocumentDigest: digest('a'),
    programRevision: digest('b'),
    recordDigest: digest('c'),
  } as AppliedProgramSelection;
  const workspace = {
    draft: {
      draftId: 'draft-1', draftRevision: 5, programLoaded: true,
      programRevision: digest('b'),
    },
  } as ProgramWorkspaceResponse;
  const snapshot = {
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 9 },
    programCycle: {
      programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 3,
      state: 'idle_after_reset', programRunId: null,
      cycleClaimGeneration: null, terminalOrSettlementReference: null,
      runRevision: null, nodeStatusRevision: null,
      runtimeControlRevision: null, recoveryOffer: null,
      executionControlRevision: 1,
      causalCorrelationId: null, causalSessionId: null,
      causalClientSequence: null, currentCommandKind: null,
      failure: null, resetPhase: null, resetOptions: null,
      resetRobotPosition: { available: false, reason: 'unsupported' }, allowedCommands: [],
      run: null, runtimeControls: null,
    },
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 1,
      programDraftId: 'draft-1',
      programDraftRevision: 5,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 1,
    },
  } as unknown as ComposerHeadlessState;
  return { applied, workspace, snapshot };
}

describe('Program runtime idle selection', () => {
  it('explains post-Reset readiness without enabling Run or labeling startup as Reset', () => {
    const { applied, workspace, snapshot } = admissionFixtures();
    const waiting = {
      ...snapshot, authoringAdmission: { authoringAllowed: false, admissionGeneration: 32 },
    };
    expect(programResetReadinessMessage(waiting)).toBe(PROGRAM_RESET_READINESS_MESSAGE);
    expect(buildProgramStartRequest(applied, true, null, waiting, workspace, 'operator', 'run')).toBeNull();
    expect(programResetReadinessMessage(snapshot)).toBeNull();
    expect(buildProgramStartRequest(applied, true, null, snapshot, workspace, 'operator', 'run')).not.toBeNull();
    expect(programResetReadinessMessage(null)).toBeNull();
    expect(programResetReadinessMessage({
      ...waiting, programCycle: { ...waiting.programCycle, cycleGeneration: 1 },
    })).toBeNull();
    expect(programResetReadinessMessage({
      ...waiting, programCycle: { ...waiting.programCycle, state: 'resetting' },
    })).toBeNull();
    expect(programResetReadinessMessage({
      ...waiting, programCycle: { ...waiting.programCycle,
        failure: { code: 'program_scene_preparation_failed', message: 'Scene convergence failed', details: {} } },
    })).toBeNull();
  });
  it('uses semantic owner revisions instead of a fixed browser poll', () => {
    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    expect(source).not.toContain('setInterval');
    const snapshot = {
      receiverEpoch: 'receiver.1',
      runtime: {
        sections: {
          manager: { sourceOwnerEpoch: 'manager.1', sourceGeneration: 4 },
        },
      },
      componentOverview: {
        basis: {
          appliedProgramOwnerEpoch: 'applied.1',
          appliedProgramGeneration: 3,
        },
      },
      authoringAdmission: { admissionGeneration: 7 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.1',
        cycleGeneration: 2,
        state: 'idle_after_reset',
        programRunId: null,
        runRevision: null,
        nodeStatusRevision: null,
        runtimeControlRevision: null,
        recoveryOffer: null,
      },
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1',
        workspaceRevision: 1,
        programDraftId: 'draft-1',
        programDraftRevision: 5,
        deploymentDraftId: 'deployment-draft-1',
        deploymentDraftRevision: 1,
      },
    } as ComposerHeadlessState;
    expect(programRuntimeWakeKey(snapshot)).toContain(
      'manager.1:no-retained-bundle:no-graph-revision:applied.1:3',
    );
    expect(programRuntimeWakeKey({
      ...snapshot,
      runtime: {
        ...snapshot.runtime,
        sections: {
          ...snapshot.runtime.sections,
          manager: { ...snapshot.runtime.sections.manager, sourceGeneration: 99 },
        },
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        runRevision: 2,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(source).not.toContain('PROGRAM_RUN_OBSERVATION_POLL_MS');
    expect(source).not.toContain('api.programs.read(activeRunId');
    expect(programRuntimeWakeKey({
      ...snapshot,
      authoringWorkspace: {
        ...snapshot.authoringWorkspace,
        programDraftRevision: 6,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        cycleGeneration: 99,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      authoringAdmission: {
        ...snapshot.authoringAdmission,
        admissionGeneration: 8,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      componentOverview: {
        ...snapshot.componentOverview,
        basis: {
          ...snapshot.componentOverview.basis,
          appliedProgramGeneration: 4,
        },
      },
    })).not.toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        runtimeControlRevision: 2,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
  });

  it('does not derive effects while refreshing the Applied projection', () => {
    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    const refresh = source.slice(
      source.indexOf('const refresh = useCallback'),
      source.indexOf('useEffect(() =>', source.indexOf('const refresh = useCallback')),
    );
    expect(refresh).not.toContain('api.programs.start');
    expect(refresh).not.toContain('applyDraft');
    expect(refresh).toContain('api.programs.currentScenario()');
    expect(refresh).not.toContain('api.programs.runs(');
    expect(refresh).not.toContain('api.programs.runtimeControls(');
    expect(source).toContain('programRuntimeAfterSnapshot(');
    expect(source).toContain('const pending = programPendingAfterProjection(');
  });

});

describe('Program command settlement', () => {
  it.each(['motion-stop', 'resume-motion'] as const)(
    'retires %s delivery feedback when its owner/run is replaced or ends', (pending) => {
      const run = { ownerEpoch: 'owner.1', state: 'running',
        capture: { programRunId: 'run.1' } } as ProgramRunStatus;
      expect(programPendingAfterProjection(pending, run, null, null, run)).toBe(pending);
      for (const replacement of [
        null,
        { ...run, ownerEpoch: 'owner.2' },
        { ...run, capture: { ...run.capture, programRunId: 'run.2' } },
        { ...run, state: 'succeeded' as const },
      ]) {
        expect(programPendingAfterProjection(pending, replacement, null, null, run)).toBeNull();
      }
    },
  );

  it('mirrors authoritative owner state without waiting for a transport receipt', () => {
    const runningCycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'running',
      programRunId: 'run-1',
      cycleClaimGeneration: 8,
    } as const;
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;

    expect(programPendingAfterProjection(
      'run', run, null, runningCycle,
    )).toBeNull();
  });

  it('does not discard a newer node-status projection within one run revision', () => {
    const current = {
      ownerEpoch: 'runner.epoch.1',
      runRevision: 5,
      nodeStatusRevision: 8,
      capture: { programRunId: 'run-1' },
      nodeStatuses: { first: 'running', second: 'idle' },
    } as unknown as ProgramRunStatus;
    const advanced = {
      ...current,
      nodeStatusRevision: 9,
      nodeStatuses: { first: 'success', second: 'running' },
    } as unknown as ProgramRunStatus;

    expect(programRunProjectionKey(advanced)).not.toBe(
      programRunProjectionKey(current),
    );
    expect(programRunProjectionKey({ ...advanced })).toBe(
      programRunProjectionKey(advanced),
    );
  });

  it('retains unchanged run data while accepting new control and node revisions', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      runRevision: 5,
      nodeStatusRevision: 8,
      capture: { programRunId: 'run-1' },
      nodeStatuses: { first: 'running' },
    } as unknown as ProgramRunStatus;
    const controls = {
      ownerEpoch: run.ownerEpoch,
      programRunId: run.capture.programRunId,
      runRevision: run.runRevision,
      runtimeControls: { runtimeControlRevision: 9 },
    } as ProgramRuntimeControlProjection;
    const cycle = {
      programRunnerOwnerEpoch: run.ownerEpoch,
      programRunId: run.capture.programRunId,
      run: structuredClone(run),
      runtimeControls: controls,
    } as Parameters<typeof programRuntimeAfterSnapshot>[2];
    const unchanged = programRuntimeAfterSnapshot(run, null, cycle);
    expect(unchanged.run).toBe(run);
    expect(unchanged.controls).toBe(controls);

    const advanced = {
      ...run,
      nodeStatusRevision: 9,
      nodeStatuses: { first: 'success' },
    } as ProgramRunStatus;
    expect(programRuntimeAfterSnapshot(run, controls, {
      ...cycle, run: advanced,
    }).run).toBe(advanced);
    const successor = { ...run, ownerEpoch: 'runner.epoch.2' };
    expect(programRuntimeAfterSnapshot(run, controls, {
      ...cycle, programRunnerOwnerEpoch: successor.ownerEpoch, run: successor,
    }).run).toBe(successor);
  });

  it('never lets an older shared snapshot undo a newer command response', () => {
    const currentRun = {
      ownerEpoch: 'runner.epoch.1',
      runRevision: 5,
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;
    const currentControls = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      runRevision: 5,
      runtimeControls: { runtimeControlRevision: 7 },
    } as ProgramRuntimeControlProjection;
    const olderRun = { ...currentRun, runRevision: 4 };
    const cycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 3,
      state: 'running',
      programRunId: 'run-1',
      runRevision: 4,
      nodeStatusRevision: 2,
      runtimeControlRevision: 6,
      run: olderRun,
      runtimeControls: {
        ...currentControls,
        runRevision: 4,
        runtimeControls: { runtimeControlRevision: 6 },
      },
    } as Parameters<typeof programRuntimeAfterSnapshot>[2];

    expect(programRuntimeAfterSnapshot(
      currentRun,
      currentControls,
      cycle,
    )).toEqual({ run: currentRun, controls: currentControls });
    expect(programRuntimeAfterSnapshot(
      currentRun,
      currentControls,
      { ...cycle, programRunId: null, run: null, runtimeControls: null },
    )).toEqual({ run: null, controls: null });
  });

  it('turns an asynchronously observed terminal failure into one detailed error', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'failed',
      runRevision: 11,
      treeStatus: 'failure',
      capture: { programRunId: 'run-background-failure' },
      nodeStatuses: { 'node.move': 'failure' },
      activeOperationCount: 0,
      reason: {
        code: 'owner_request_failed',
        message: 'planning failed after the Run request had returned',
        details: {
          reason: 'frame_role_unresolved',
          reference_type: 'frame_role',
          reference_id: 'tool0',
          target_role_id: 'arm',
        },
        origin: { kind: 'owner', nodeInstanceId: 'node.move', ownerApi: 'scene.frames' },
      },
      message: 'planning failed after the Run request had returned',
    } as unknown as ProgramRunStatus;
    const message = programTerminalFailureMessage(run, null, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 12,
      state: 'terminal_awaiting_reset',
      programRunId: 'run-background-failure',
      cycleClaimGeneration: 22,
    });
    expect(message).toContain('Program execution failed.');
    expect(message).toContain('run-background-failure: failed, revision 11');
    expect(message).toContain('Run reason: owner_request_failed.');
    expect(message).toContain(
      "Run failure details: Reason: frame_role_unresolved; Reference: frame_role 'tool0' "
      + "of target role 'arm'; Origin: scene.frames for node 'node.move'.",
    );
    expect(message).toContain('Current nodes: node.move=failure.');
    expect(programTerminalFailureMessage({ ...run, state: 'running' }, null, null))
      .toBeNull();
  });

  it('summarizes the held node failure carried by the recovery offer', () => {
    const controls = {
      recoveryOffer: {
        recoveryClass: 'node_failure_pause',
        status: 'action_required',
        offerRevision: 2,
        nodeInstanceId: 'wait',
        operationId: null,
        actions: [],
        reason: {
          code: 'node_input_unavailable',
          message: "missing input duration_s from blackboard_key 'delay'",
          details: {
            input_problem: 'missing',
            path: 'duration_s',
            source_kind: 'blackboard_key',
            source_reference: 'delay',
          },
          origin: { kind: 'program_node', nodeInstanceId: 'wait', ownerApi: null },
        },
      },
    } as unknown as ProgramRuntimeControlProjection;
    const summary = programCurrentStatusSummary(null, controls, null);
    expect(summary).toContain('Recovery reason: node_input_unavailable.');
    expect(summary).toContain("Recovery message: missing input duration_s from blackboard_key 'delay'.");
    expect(summary).toContain(
      "Recovery failure details: Input: duration_s from blackboard key 'delay' (missing); "
      + "Origin: node 'wait'.",
    );
  });

  it('keeps selected delivery feedback separate from physical Stop presentation', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;
    const settled = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      executionSelections: { selectionRevision: 1, motionHeld: true, groups: [
        { groupId: 'all', nodeCount: 1, heldNodeCount: 1, state: 'held',
          stopAllowed: true, resumeAllowed: true, resumeUnavailableReason: null },
      ] },
      recoveryOffer: {
        programRunId: 'run-1',
        recoveryClass: 'ordinary_operator_stop',
        status: 'retry_ready',
        stopId: 'stop-1',
        stopGeneration: 1,
        stopCertificateId: 'certificate-1',
        stopCertificateRevision: 1,
      },
    } as unknown as NonNullable<
      Parameters<typeof programPendingAfterProjection>[2]
    >;

    expect(programPendingAfterProjection('motion-stop', run, null)).toBe(
      'motion-stop',
    );
    expect(programPendingAfterProjection(
      'motion-stop',
      run,
      { ...settled, ownerEpoch: 'runner.epoch.stale' },
    )).toBe('motion-stop');
    // Sending feedback ends on transport receipt, not guessed physical status.
    expect(programPendingAfterProjection('motion-stop', run, settled)).toBe('motion-stop');
    expect(programMotionStopSettled(run, settled)).toBe(true);
    expect(programPendingAfterProjection(
      'motion-stop',
      { ...run, state: 'stopped' },
      null,
    )).toBeNull();
    expect(programPendingAfterProjection('run', run, settled)).toBe('run');
    expect(programPendingAfterProjection('run', run, settled, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'running',
      programRunId: 'run-1',
      cycleClaimGeneration: 8,
    })).toBeNull();
    expect(programPendingAfterProjection('stop', null, null, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'terminal_awaiting_reset',
      programRunId: 'run-1',
      cycleClaimGeneration: null,
    })).toBeNull();
    const idleCycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 5,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      admissionGeneration: 12,
      authoringAllowed: true,
    } as const;
    expect(programPendingAfterProjection(
      'reset', null, null, idleCycle,
    )).toBeNull();
    expect(programPendingAfterProjection(
      'reset', null, null,
      { ...idleCycle, authoringAllowed: false },
    )).toBeNull();

    const resetOnly = {
      ...settled,
      recoveryOffer: {
        ...settled.recoveryOffer,
        recoveryClass: 'stop_fault_intervention',
        status: 'reset_required',
      },
    } as unknown as NonNullable<
      Parameters<typeof programMotionStopSettled>[1]
    >;
    expect(programMotionStopSettled(run, resetOnly)).toBe(true);
    expect(programPendingAfterProjection('motion-stop', run, resetOnly)).toBe('motion-stop');
  });

  it('keeps Resume disabled until the exact recovery offer changes', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as unknown as ProgramRunStatus;
    const retainedOffer = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      recoveryOffer: { offerId: 'offer-1', offerRevision: 3 },
    } as unknown as NonNullable<
      Parameters<typeof controlPendingAfterProjection>[1]
    >;
    expect(controlPendingAfterProjection(
      'recovery:replan_from_stopped_state',
      retainedOffer,
      retainedOffer,
      run,
    )).toBe('recovery:replan_from_stopped_state');
    expect(controlPendingAfterProjection(
      'recovery:replan_from_stopped_state',
      retainedOffer,
      { ...retainedOffer, recoveryOffer: null },
      run,
    )).toBeNull();
    expect(controlPendingAfterProjection(
      'override:activate:one',
      retainedOffer,
      retainedOffer,
      run,
    )).toBe('override:activate:one');
  });
});

describe('Program runtime authority requests', () => {
  it('waits for an unapplied Deployment Draft exactly like a Program Draft', () => {
    const { applied, workspace, snapshot } = admissionFixtures();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, workspace, 'owner', 'start', false,
    )).not.toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, workspace, 'owner', 'start', true,
    )).toBeNull();
    const programChanged = {
      ...workspace, draft: { ...workspace.draft, programRevision: digest('d') },
    } as typeof workspace;
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, programChanged, 'owner', 'start', false,
    )).toBeNull();
  });

  it('starts only from one complete Idle snapshot and carries exact fences', () => {
    const { applied, workspace, snapshot } = admissionFixtures();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toEqual({
      request_id: 'program-run-start.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'program-run-start.fixture',
      scenario_id: 'applied',
      source_document_digest: digest('a'),
      expected_applied_record_digest: digest('c'),
      draft_program_revision: digest('b'),
    });

    expect(buildProgramStartRequest(
      applied, true, null, null, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null,
      { ...snapshot, programCycle: { ...snapshot.programCycle, state: 'running' } },
      workspace, 'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot,
      { ...workspace, draft: { ...workspace.draft, draftRevision: 6 } },
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
  });

  it.each([
    [false, false], [true, false], [false, true], [true, true],
  ])('sends explicit idle Reset selectors %s/%s with exact request identity', (scene, position) => {
    const { snapshot } = admissionFixtures();
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      semanticTarget: {}, allowed: true, disabledReason: null }];
    expect(buildProgramResetRequest(snapshot, 'caller.1', 'request.1', 'correlation.1', {
      reloadInitialScene: scene, resetRobotPosition: position,
    })).toMatchObject({
      request_id: 'request.1', request_owner_epoch: 'caller.1', correlation_id: 'correlation.1',
      program_run_id: null, expected_claim_generation: null,
      terminal_reference: null, settlement_evidence_digest: null,
      reload_initial_scene: scene, reset_robot_position: position,
    });
    expect(buildProgramResetRequest(snapshot, 'caller.1', 'plain.1'))
      .toMatchObject({ reload_initial_scene: false, reset_robot_position: false });
  });

  it('builds Reset only from exact backend-authorized cycle evidence', () => {
    const { snapshot } = admissionFixtures();
    const terminalReference = digest('d');
    const terminalSnapshot = {
      ...snapshot,
      programCycle: {
        programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 4,
        state: 'terminal_awaiting_reset' as const,
        programRunId: 'run-1', cycleClaimGeneration: 12,
        terminalOrSettlementReference: terminalReference,
        runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null, recoveryOffer: null,
        executionControlRevision: 1,
        causalCorrelationId: null, causalSessionId: null,
        causalClientSequence: null, currentCommandKind: null,
        failure: null, resetPhase: null, resetOptions: null,
        resetRobotPosition: { available: false, reason: 'unsupported' }, allowedCommands: [{
          commandKind: 'program_reset', semanticTarget: {},
          allowed: true, disabledReason: null,
        }],
        run: null, runtimeControls: null,
      },
    };
    expect(buildProgramResetRequest(
      terminalSnapshot, 'composer-program-owner.fixture', 'reset.fixture',
    )).toEqual({
      request_id: 'reset.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'reset.fixture',
      program_run_id: 'run-1',
      expected_owner_epoch: 'runner.epoch.1',
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: terminalReference,
      settlement_evidence_digest: terminalReference,
      reload_initial_scene: false, reset_robot_position: false,
    });
    expect(buildProgramResetRequest(
      {
        ...terminalSnapshot,
        programCycle: {
          ...terminalSnapshot.programCycle,
          state: 'recovery_required',
          terminalOrSettlementReference: null,
        },
      },
      'composer-program-owner.fixture', 'reset.fixture',
    )).toEqual({
      request_id: 'reset.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'reset.fixture',
      program_run_id: 'run-1',
      expected_owner_epoch: 'runner.epoch.1',
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: null,
      settlement_evidence_digest: null,
      reload_initial_scene: false, reset_robot_position: false,
    });
    expect(buildProgramResetRequest(
      {
        ...terminalSnapshot,
        programCycle: {
          ...terminalSnapshot.programCycle,
          allowedCommands: terminalSnapshot.programCycle.allowedCommands.map(
            (command) => ({ ...command, allowed: false }),
          ),
        },
      },
      'composer-program-owner.fixture', 'reset.fixture',
    )).toBeNull();
  });

});

describe('stored persistent value edits', () => {
  const values = (programRunId: string | null): PersistentKeyValues => ({
    scenarioId: 'cell-pick',
    programRevision: digest('a'),
    programRunId,
    sequence: 7,
    keys: [],
    edits: [],
    storageFault: null,
    applyObservationFault: null,
  });
  const run = (programRunId: string, runRevision: number) => ({
    capture: { programRunId }, runRevision,
  });

  it('names the live run the operator saw; without a run the last edit wins', () => {
    // Without a run nothing is named: the runner admits the edit unless the
    // same request was applied already.
    expect(persistentValuesFence(values(null), run('program-cycle.1', 4))).toEqual({});
    // With a live run: that run at its current revision.
    expect(persistentValuesFence(values('program-cycle.2'), run('program-cycle.2', 4)))
      .toEqual({ program_run_id: 'program-cycle.2', expected_run_revision: 4 });
    // Never a run other than the one holding the values.
    expect(persistentValuesFence(values('program-cycle.2'), run('program-cycle.1', 4))).toBeNull();
    expect(persistentValuesFence(values('program-cycle.2'), null)).toBeNull();
  });
});
