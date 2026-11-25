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
  displayRunId,
  persistentValuesFence,
  programPendingAfterProjection,
  programRunProjectionKey,
  programRuntimeAfterSnapshot,
  programCurrentStatusSummary,
  programMotionStopSettled,
  programRuntimeWakeKey,
  programTerminalFailureMessage,
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
      cycleGeneration: 3,
      state: 'idle_after_reset', programRunId: null,
      cycleClaimGeneration: null, terminalOrSettlementReference: null,
      runRevision: null, nodeStatusRevision: null,
      runtimeControlRevision: null,
      executionControlRevision: 1,
      causalRequestKey: null, causalDisplay: null, currentCommandKind: null, runnerCommands: { inProgress: null, ended: [] },
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
  it('uses semantic owner revisions instead of a fixed browser poll', () => {
    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    expect(source).not.toContain('setInterval');
    const snapshot = {
      applicationStartId: '3f9a000000000001',
      runtime: {
        sections: {
          manager: { sourceOwnerId: 'manager.1', sourceGeneration: 4 },
        },
      },
      componentOverview: {
        basis: {
          appliedProgramGeneration: 3,
        },
      },
      authoringAdmission: { admissionGeneration: 7 },
      programCycle: {
        cycleGeneration: 2,
        state: 'idle_after_reset',
        programRunId: null,
        runRevision: null,
        nodeStatusRevision: null,
        runtimeControlRevision: null,
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
      'manager.1:no-retained-bundle:no-graph-revision:3',
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
  // A pending flag clears on the first projection that shows the command's
  // effect, or that the state it was sent from no longer holds; coalesced
  // state messages can skip any one transient state.
  const cycleIn = (state: string, generation = 4, programRunId: number | null = 1) => ({
    cycleGeneration: generation,
    state, programRunId, cycleClaimGeneration: 8,
  }) as Parameters<typeof programPendingAfterProjection>[3];
  const runIn = (state: string, runRevision: number, programRunId = 1) => ({
    state, runRevision, capture: { programRunId },
  }) as unknown as ProgramRunStatus;
  const originOf = (run: ProgramRunStatus | null, cycleGeneration = 4) => ({
    cycleGeneration,
    programRunId: run?.capture.programRunId ?? null,
    runRevision: run?.runRevision ?? null,
  });

  it('clears Execute after Halt when the resumed run is held again before running is seen', () => {
    const halted = runIn('halted', 9);
    const origin = originOf(halted);
    expect(programPendingAfterProjection(
      'resume-halt', halted, null, cycleIn('running'), halted, origin,
    )).toBe('resume-halt');
    expect(programPendingAfterProjection(
      'resume-halt', runIn('intervention_required', 11), null, cycleIn('running'), halted, origin,
    )).toBeNull();
    expect(programPendingAfterProjection(
      'resume-halt', runIn('stopped', 12), null, cycleIn('terminal_awaiting_reset'), halted, origin,
    )).toBeNull();
  });

  it('clears a Halt whose run ends, or is resumed, before halted is seen', () => {
    const running = runIn('intervention_required', 5);
    const origin = originOf(running);
    expect(programPendingAfterProjection(
      'halt', running, null, cycleIn('running'), running, origin,
    )).toBe('halt');
    // Accepted: the owner took it with a new run revision (halting).
    expect(programPendingAfterProjection(
      'halt', runIn('halting', 6), null, cycleIn('running'), running, origin,
    )).toBeNull();
    // Halted and already resumed by another client: never seen halted.
    expect(programPendingAfterProjection(
      'halt', runIn('running', 8), null, cycleIn('running'), running, origin,
    )).toBeNull();
    // Sent while a Forced Program End ends the run: no effect, the run ends.
    const stopping = runIn('stopping', 5);
    expect(programPendingAfterProjection(
      'halt', runIn('stopped', 5), null, cycleIn('terminal_awaiting_reset'), stopping,
      originOf(stopping),
    )).toBeNull();
  });

  it('clears Execute once a run is captured, whatever state it is in', () => {
    const origin = originOf(null, 4);
    expect(programPendingAfterProjection('run', null, null, cycleIn('starting', 4, 1), null, origin))
      .toBe('run');
    // The first node fails or is held at once: never seen running.
    expect(programPendingAfterProjection(
      'run', runIn('intervention_required', 2), null, cycleIn('running', 4), null, origin,
    )).toBeNull();
    // The run ended and the cycle was reset meanwhile.
    expect(programPendingAfterProjection(
      'run', null, null, cycleIn('idle_after_reset', 5, null), null, origin,
    )).toBeNull();
  });

  it('clears Reset once a later cycle is seen, even if its idle state was skipped', () => {
    const ended = runIn('stopped', 7);
    const origin = originOf(ended, 4);
    expect(programPendingAfterProjection(
      'reset', ended, null, cycleIn('resetting', 4), ended, origin,
    )).toBe('reset');
    expect(programPendingAfterProjection(
      'reset', runIn('running', 1, 2), null, cycleIn('running', 5, 2), ended, origin,
    )).toBeNull();
  });

  it.each(['motion-stop', 'resume-motion'] as const)(
    'retires %s delivery feedback when its owner/run is replaced or ends', (pending) => {
      const run = { state: 'running',
        capture: { programRunId: 1 } } as ProgramRunStatus;
      expect(programPendingAfterProjection(pending, run, null, null, run)).toBe(pending);
      for (const replacement of [
        null,
        { ...run, capture: { ...run.capture, programRunId: 2 } },
        { ...run, state: 'succeeded' as const },
      ]) {
        expect(programPendingAfterProjection(pending, replacement, null, null, run)).toBeNull();
      }
    },
  );

  it('mirrors authoritative owner state without waiting for a transport receipt', () => {
    const runningCycle = {
      cycleGeneration: 4,
      state: 'running',
      programRunId: 1,
      cycleClaimGeneration: 8,
    } as const;
    const run = {
      state: 'running',
      capture: { programRunId: 1 },
    } as ProgramRunStatus;

    expect(programPendingAfterProjection(
      'run', run, null, runningCycle,
    )).toBeNull();
  });

  it('does not discard a newer node-status projection within one run revision', () => {
    const current = {
      runRevision: 5,
      nodeStatusRevision: 8,
      capture: { programRunId: 1 },
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
      runRevision: 5,
      nodeStatusRevision: 8,
      capture: { programRunId: 1 },
      nodeStatuses: { first: 'running' },
    } as unknown as ProgramRunStatus;
    const controls = {
      programRunId: run.capture.programRunId,
      runRevision: run.runRevision,
      runtimeControls: { runtimeControlRevision: 9 },
    } as ProgramRuntimeControlProjection;
    const cycle = {
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
  });

  it('never lets an older shared snapshot undo a newer command response', () => {
    const currentRun = {
      runRevision: 5,
      capture: { programRunId: 1 },
    } as ProgramRunStatus;
    const currentControls = {
      programRunId: 1,
      runRevision: 5,
      runtimeControls: { runtimeControlRevision: 7 },
    } as ProgramRuntimeControlProjection;
    const olderRun = { ...currentRun, runRevision: 4 };
    const cycle = {
      cycleGeneration: 3,
      state: 'running',
      programRunId: 1,
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
      state: 'failed',
      runRevision: 11,
      treeStatus: 'failure',
      capture: { programRunId: 7 },
      nodeStatuses: { 'node.move': 'failure' },
      activeOperationCount: 0,
      reason: {
        code: 'owner_request_failed',
        message: 'planning failed after the Run request had returned',
        details: {
          reason: 'frame_unresolved',
          reference_type: 'component_frame',
          reference_id: 'tool0',
          component_id: 'arm',
        },
        origin: { kind: 'owner', nodeInstanceId: 'node.move', ownerApi: 'scene.frames' },
      },
      message: 'planning failed after the Run request had returned',
    } as unknown as ProgramRunStatus;
    const message = programTerminalFailureMessage(run, null, {
      cycleGeneration: 12,
      state: 'terminal_awaiting_reset',
      programRunId: 7,
      cycleClaimGeneration: 22,
      applicationStartId: '3f9a000000000001',
    });
    expect(message).toContain('Program execution failed.');
    expect(message).toContain('Observed run 3f9a/run7: failed, revision 11');
    expect(message).toContain('Run reason: owner_request_failed.');
    expect(message).toContain(
      "Run failure details: Reason: frame_unresolved; Reference: component_frame 'tool0' "
      + "of component 'arm'; Origin: scene.frames for node 'node.move'.",
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
      state: 'running',
      capture: { programRunId: 1 },
    } as ProgramRunStatus;
    const settled = {
      programRunId: 1,
      executionSelections: { holdRevision: 1, motionHeld: true, groups: [
        { groupId: 'all', nodeCount: 1, heldNodeCount: 1, state: 'held',
          stopAllowed: true, resumeAllowed: true, resumeUnavailableReason: null },
      ] },
      recoveryOffer: {
        programRunId: 1,
        recoveryClass: 'ordinary_operator_stop',
        status: 'retry_ready',
      },
    } as unknown as NonNullable<
      Parameters<typeof programPendingAfterProjection>[2]
    >;

    expect(programPendingAfterProjection('motion-stop', run, null)).toBe(
      'motion-stop',
    );
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
      cycleGeneration: 4,
      state: 'running',
      programRunId: 1,
      cycleClaimGeneration: 8,
    })).toBeNull();
    const idleCycle = {
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
        recoveryClass: 'ordinary_operator_stop',
        status: 'reset_required',
      },
    } as unknown as NonNullable<
      Parameters<typeof programMotionStopSettled>[1]
    >;
    expect(programMotionStopSettled(run, resetOnly)).toBe(true);
    expect(programPendingAfterProjection('motion-stop', run, resetOnly)).toBe('motion-stop');
  });

  it('keeps a recovery command pending until its own hold offer changes', () => {
    const run = {
      state: 'running',
      capture: { programRunId: 1 },
    } as unknown as ProgramRunStatus;
    const offerOne = `sha256:${'1'.repeat(64)}`;
    const offerTwo = `sha256:${'2'.repeat(64)}`;
    const holds = (one: number | null, two: number) => [
      ...(one === null ? [] : [{ motionGroupIds: ['left'], offer: { offerId: offerOne, offerRevision: one } }]),
      { motionGroupIds: ['right'], offer: { offerId: offerTwo, offerRevision: two } },
    ];
    const retainedOffer = {
      programRunId: 1,
      recoveryOffer: null,
      recoveryHolds: holds(3, 1),
    } as unknown as NonNullable<
      Parameters<typeof controlPendingAfterProjection>[1]
    >;
    const pending = `recovery:${offerOne}:retry_motion_from_current_state`;
    expect(controlPendingAfterProjection(pending, retainedOffer, retainedOffer, run))
      .toBe(pending);
    // Another hold changing leaves this command pending.
    expect(controlPendingAfterProjection(pending, retainedOffer, {
      ...retainedOffer, recoveryHolds: holds(3, 2),
    } as typeof retainedOffer, run)).toBe(pending);
    expect(controlPendingAfterProjection(pending, retainedOffer, {
      ...retainedOffer, recoveryHolds: holds(4, 1),
    } as typeof retainedOffer, run)).toBeNull();
    expect(controlPendingAfterProjection(pending, retainedOffer, {
      ...retainedOffer, recoveryHolds: holds(null, 1),
    } as typeof retainedOffer, run)).toBeNull();
    expect(controlPendingAfterProjection(
      'override:activate:one',
      retainedOffer,
      retainedOffer,
      run,
    )).toBe('override:activate:one');
  });

  it('keeps an operator Stop Retry pending until its focused offer changes or is retired', () => {
    const run = {
      state: 'intervention_required',
      capture: { programRunId: 1 },
    } as unknown as ProgramRunStatus;
    const stopId = `sha256:${'3'.repeat(64)}`;
    const focused = (offerRevision: number | null) => ({
      programRunId: 1,
      recoveryOffer: offerRevision === null ? null : { offerId: stopId, offerRevision },
      recoveryHolds: [],
    } as unknown as NonNullable<Parameters<typeof controlPendingAfterProjection>[1]>);
    const pending = `recovery:${stopId}:retry_motion_from_current_state`;
    expect(controlPendingAfterProjection(pending, focused(2), focused(2), run)).toBe(pending);
    expect(controlPendingAfterProjection(pending, focused(2), focused(3), run)).toBeNull();
    expect(controlPendingAfterProjection(pending, focused(2), focused(null), run)).toBeNull();
  });

  it('keeps an operator-question answer pending until that question changes or is retired', () => {
    const run = {
      state: 'running',
      capture: { programRunId: 1 },
    } as unknown as ProgramRunStatus;
    const questionId = `sha256:${'4'.repeat(64)}`;
    const otherId = `sha256:${'5'.repeat(64)}`;
    const asking = (revision: number | null, other = 1) => ({
      programRunId: 1,
      recoveryOffer: null,
      recoveryHolds: [],
      operatorQuestions: [
        ...(revision === null ? [] : [{ offerId: questionId, offerRevision: revision }]),
        { offerId: otherId, offerRevision: other },
      ],
    } as unknown as NonNullable<Parameters<typeof controlPendingAfterProjection>[1]>);
    const pending = `recovery:${questionId}:continue`;
    expect(controlPendingAfterProjection(pending, asking(3), asking(3), run)).toBe(pending);
    expect(controlPendingAfterProjection(pending, asking(3), asking(3, 2), run)).toBe(pending);
    expect(controlPendingAfterProjection(pending, asking(3), asking(4), run)).toBeNull();
    expect(controlPendingAfterProjection(pending, asking(3), asking(null), run)).toBeNull();
  });
});

describe('Program runtime authority requests', () => {
  it('names the Applied Program in the run request; readiness is the backend\'s', () => {
    const { applied, workspace } = admissionFixtures();
    expect(buildProgramStartRequest(applied, workspace)).toEqual({
      source_label: 'operator_ui',
      scenario_id: 'applied',
      source_document_digest: digest('a'),
      expected_applied_record_digest: digest('c'),
      draft_program_revision: digest('b'),
    });

    expect(buildProgramStartRequest(null, workspace)).toBeNull();
    expect(buildProgramStartRequest(applied, null)).toBeNull();
  });

  it.each([
    [false, false], [true, false], [false, true], [true, true],
  ])('sends explicit idle Reset selectors %s/%s', (scene, position) => {
    const { snapshot } = admissionFixtures();
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      semanticTarget: {}, allowed: true, disabledReason: null }];
    expect(buildProgramResetRequest(snapshot, {
      reloadInitialScene: scene, resetRobotPosition: position,
    })).toMatchObject({
      program_run_id: null, expected_claim_generation: null,
      terminal_reference: null, settlement_evidence_digest: null,
      reload_initial_scene: scene, reset_robot_position: position,
    });
    expect(buildProgramResetRequest(snapshot))
      .toMatchObject({ reload_initial_scene: false, reset_robot_position: false });
  });

  it('builds Reset only from exact backend-authorized cycle evidence', () => {
    const { snapshot } = admissionFixtures();
    const terminalReference = digest('d');
    const terminalSnapshot = {
      ...snapshot,
      programCycle: {
        cycleGeneration: 4,
        state: 'terminal_awaiting_reset' as const,
        programRunId: 1, cycleClaimGeneration: 12,
        terminalOrSettlementReference: terminalReference,
        runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null,
        executionControlRevision: 1,
        causalRequestKey: null, causalDisplay: null, currentCommandKind: null, runnerCommands: { inProgress: null, ended: [] },
        failure: null, resetPhase: null, resetOptions: null,
        resetRobotPosition: { available: false, reason: 'unsupported' }, allowedCommands: [{
          commandKind: 'program_reset', semanticTarget: {},
          allowed: true, disabledReason: null,
        }],
        run: null, runtimeControls: null,
      },
    };
    // The command names no request, owner or correlation: the control
    // session and its client sequence name it (control protocol v3).
    expect(buildProgramResetRequest(terminalSnapshot)).toEqual({
      source_label: 'operator_ui',
      program_run_id: 1,
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: terminalReference,
      settlement_evidence_digest: terminalReference,
      reload_initial_scene: false, reset_robot_position: false, reset_stored_values: false,
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
    )).toEqual({
      source_label: 'operator_ui',
      program_run_id: 1,
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: null,
      settlement_evidence_digest: null,
      reload_initial_scene: false, reset_robot_position: false, reset_stored_values: false,
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
    )).toBeNull();
  });

});

describe('stored persistent value edits', () => {
  const values = (programRunId: number | null): PersistentKeyValues => ({
    scenarioId: 'cell-pick',
    projectPath: '/projects/cell-pick.scenario',
    programRevision: digest('a'),
    programRunId,
    sequence: 7,
    keys: [],
    edits: [],
  });
  const run = (programRunId: number, runRevision: number) => ({
    capture: { programRunId }, runRevision,
  });

  it('names the live run the operator saw; without a run the last edit wins', () => {
    // Without a run nothing is named: the runner admits the edit unless the
    // same request was applied already.
    expect(persistentValuesFence(values(null), run(1, 4))).toEqual({});
    // With a live run: that run at its current revision.
    expect(persistentValuesFence(values(2), run(2, 4)))
      .toEqual({ program_run_id: 2, expected_run_revision: 4 });
    // Never a run other than the one holding the values.
    expect(persistentValuesFence(values(2), run(1, 4))).toBeNull();
    expect(persistentValuesFence(values(2), null)).toBeNull();
  });
});

describe('run number display', () => {
  it('shows a run number in the display form of the current start, bare without one', () => {
    expect(displayRunId('3f9a000000000001', 3)).toBe('3f9a/run3');
    expect(displayRunId(null, 3)).toBe('run3');
  });
});
