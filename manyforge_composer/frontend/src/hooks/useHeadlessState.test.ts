import { StrictMode, act, createElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import localDeltaFixture from '../../../../manyforge_headless_api/tests/fixtures/control-v2-local-delta.json';

import type { ComposerHeadlessState } from '../types/headless';
import { deriveTransportGuidance } from '../components/ProgramRuntimePanel';
import {
  HEADLESS_SHELL_PUBLICATION_MS,
  HeadlessStateSource,
  admitHeadlessState,
  executionCommandPayload,
  getHeadlessStateQualificationSnapshot,
  parseExecutionObservationFrame,
  sameShellPublication,
  sameViewerIdentity,
  sameViewerProjection,
  useHeadlessStateSource,
  useViewerHeadlessState,
} from './useHeadlessState';

function componentOverview(): ComposerHeadlessState['componentOverview'] {
  return {
    schemaVersion: 'manyforge.component_overview.v2',
    overviewRevision: `sha256:${'a'.repeat(64)}`,
    readiness: 'ready',
    summaryLabel: 'Application surfaces ready',
    profileLabel: 'native',
    authorityLabel: 'owner-issued scoped facts',
    userMessage: 'Current.',
    capabilities: ['Observe / Render', 'Plan / Preview', 'Execute / Control'],
    components: [],
    maintenanceOperation: null,
    basis: {
      runtimeProjectionOwnerEpoch: null,
      runtimeProjectionGeneration: null,
      managerSourceOwnerEpoch: null,
      logicalSceneSourceOwnerEpoch: null,
      logicalSceneSourceGeneration: null,
      sceneRuntimeSourceOwnerEpoch: null,
      sceneRuntimeSourceGeneration: null,
      alarmOwnerEpoch: null,
      alarmGeneration: null,
      alarmStateRevision: null,
      admissionGeneration: null,
      programRunnerOwnerEpoch: null,
      programCycleGeneration: null,
      appliedProgramOwnerEpoch: null,
      appliedProgramGeneration: null,
      executionOwnerEpoch: null,
      executionPublicationGeneration: null,
    },
  };
}

function state(
  runtimeEpoch: string,
  runtimeGeneration: number,
  alarmEpoch = 'alarms.epoch.1',
  alarmGeneration = 3,
  alarmStateRevision = alarmGeneration,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    executionControlStatus: null,
    componentOverview: componentOverview(),
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 1 },
    heldRunDraftEditAllowed: false,
    programCycle: {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 1,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      terminalOrSettlementReference: null,
      runRevision: null,
      nodeStatusRevision: null,
      runtimeControlRevision: null,
      executionControlRevision: 1,
      causalCorrelationId: null,
      causalSessionId: null,
      causalClientSequence: null,
      currentCommandKind: null,
      failure: null,
      resetPhase: null, resetOptions: null,
      resetRobotPosition: { available: false, reason: 'unsupported' },
      allowedCommands: [],
      run: null,
      runtimeControls: null,
    },
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 1,
      programDraftId: 'draft-1',
      programDraftRevision: 1,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 1,
    },
    authoringWorkspaceContainment: null,
    receiverEpoch: runtimeEpoch,
    snapshotSequence: Math.max(1, runtimeGeneration),
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      activeConfiguration: null,
      complete: true,
      projectionGeneration: runtimeGeneration,
      projectionOwnerEpoch: runtimeEpoch,
      projectionOwnerId: 'headless.owner',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'manager.epoch.1',
          sourceOwnerId: 'manager.owner',
          receiverAgeNs: 1,
        },
        logicalScene: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'scene.epoch.1',
          sourceOwnerId: 'scene.owner',
          receiverAgeNs: 1,
        },
        sceneRuntime: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'runtime.epoch.1',
          sourceOwnerId: 'runtime.owner',
          receiverAgeNs: 1,
        },
      },
    },
    alarms: {
      schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
      ownerId: 'alarm.owner',
      ownerEpoch: alarmEpoch,
      generation: alarmGeneration,
      stateRevision: alarmStateRevision,
      reconciled: true,
      healthState: 'healthy',
      liveCount: 0,
      unacknowledgedCount: 0,
      activeCount: 0,
      activeAcknowledgedCount: 0,
      motionAttentionCount: 0,
      motionAttentionMembership: [],
      unacknowledgedMembership: [],
      alarms: [],
      truncated: false,
      kernelAttention: null,
    },
  };
}

describe('Headless owner-frame admission', () => {
  it('copies the current native owner fence only onto native commands', () => {
    expect(executionCommandPayload(
      'runtime_scale',
      { scale: 0.25 },
      'kernel.epoch.4',
    )).toEqual({
      scale: 0.25,
      expected_execution_owner_epoch: 'kernel.epoch.4',
    });
    const programPayload = { scenario_id: 'lite6' };
    expect(executionCommandPayload(
      'program_run',
      programPayload,
      null,
    )).toBe(programPayload);
    expect(() => executionCommandPayload('operation_stop', {}, null))
      .toThrow('Execution owner identity is unavailable.');
  });

  it('publishes owner runtime-control revisions without invalidating the viewer', () => {
    const current = state('receiver.epoch.1', 2);
    const next = structuredClone(current);
    next.snapshotSequence += 1;
    next.programCycle.runtimeControlRevision = 2;
    expect(sameViewerProjection(current, next)).toBe(true);
  });

  it('does not invalidate the viewer for Manager observation heartbeats', () => {
    const current = state('receiver.epoch.1', 2);
    const heartbeat = structuredClone(current);
    heartbeat.snapshotSequence += 1;
    heartbeat.runtime.projectionGeneration += 1;
    heartbeat.runtime.sections.manager.sourceGeneration =
      (heartbeat.runtime.sections.manager.sourceGeneration ?? 0) + 1;
    heartbeat.runtime.sections.manager.receiverAgeNs =
      (heartbeat.runtime.sections.manager.receiverAgeNs ?? 0) + 1;
    expect(sameViewerProjection(current, heartbeat)).toBe(true);

    heartbeat.runtime.sections.logicalScene.sourceGeneration =
      (heartbeat.runtime.sections.logicalScene.sourceGeneration ?? 0) + 1;
    expect(sameViewerProjection(current, heartbeat)).toBe(false);
  });

  it.each([
    ['Apply completion', { apply: { records: [{ incomplete: false }] } }],
    ['missing Apply evidence', { apply: { records: [{}] } }],
    ['readiness loss', { manager_ready: false }],
    ['convergence loss', { runtime: { converged: false } }],
    ['composition change', { active: { retained_bundle_id: 'bundle.2', graph_revision: 'graph.2' } }],
    ['cell identity change', { cell_id: 'cell.2' }],
  ])('invalidates the viewer on %s without a Scene change', (_name, change) => {
    const current = state('receiver.epoch.1', 2);
    current.runtime.sections.manager.payload = {
      cell_id: 'cell.1',
      manager_ready: true,
      runtime: { converged: true },
      active: { retained_bundle_id: 'bundle.1', graph_revision: 'graph.1' },
      apply: { records: [{ incomplete: true }] },
    };
    const next = structuredClone(current);
    Object.assign(next.runtime.sections.manager.payload!, change);
    expect(sameViewerProjection(current, next)).toBe(false);
  });

  it('compares the Scene runtime by what it presents, not by its generation', () => {
    const poseSource = (overrides: Record<string, unknown> = {}) => ({
      object_id: 'part', policy_id: 'viewer.measured_object', source_id: 'camera',
      source_epoch: 'camera.1', source_epoch_order: 1, source_sequence: 10,
      observed_at_ns: '100', expires_at_ns: '200', payload_quality: 'good', fresh: true,
      eligible: true, buffered: false,
      payload: {
        api_version: 'manyforge.ros_pose_observation.v2',
        kind: 'simulation_entity_pose',
        source: { entity: 'part' },
        pose: {
          frame_id: 'cell', frame_kind: 'cell_frame', position_m: [1, 2, 3],
          orientation_xyzw: [0, 0, 0, 1], source_stamp_ns: '90',
        },
      },
      ...overrides,
    });
    const withRuntime = (generation: number, sources: unknown[], updatedAt = '1') => {
      const value = state('receiver.epoch.1', 2);
      value.runtime.sections.sceneRuntime.sourceGeneration = generation;
      value.runtime.sections.sceneRuntime.payload = {
        api_version: 'manyforge.scene_runtime_source.v1',
        cell_id: 'cell',
        state: {
          state: 'open', updated_at_ns: updatedAt, counters: { samples: generation },
          pose_sources: sources,
        },
      };
      return value;
    };
    const current = withRuntime(1, [poseSource()]);
    // A new pose sample with the same applied pose: delivery diagnostics only.
    expect(sameViewerProjection(current, withRuntime(2, [poseSource({
      source_sequence: 11, observed_at_ns: '110', expires_at_ns: '210',
    })], '2'))).toBe(true);
    // A freshness flip selects measured vs neutral presentation: publish.
    expect(sameViewerProjection(current, withRuntime(2, [
      poseSource({ fresh: false }),
    ]))).toBe(false);
    expect(sameViewerProjection(current, withRuntime(2, [
      poseSource({ payload_quality: 'unknown' }),
    ]))).toBe(false);
    // A changed applied pose value: publish.
    const moved = poseSource();
    (moved.payload.pose as { position_m: number[] }).position_m = [1, 2, 4];
    expect(sameViewerProjection(current, withRuntime(2, [moved]))).toBe(false);
    // Availability of the source and its generation still publish.
    const unavailable = withRuntime(1, [poseSource()]);
    unavailable.runtime.sections.sceneRuntime.freshness = 'stale';
    expect(sameViewerProjection(current, unavailable)).toBe(false);
    expect(sameViewerProjection(current, withRuntime(1, []))).toBe(false);
  });

  it('skips shell publication only for delivery diagnostics', () => {
    const current = state('receiver.epoch.1', 2);
    const diagnostic = structuredClone(current);
    diagnostic.snapshotSequence += 1;
    diagnostic.runtime.projectionGeneration += 1;
    diagnostic.runtime.sections.manager.receiverAgeNs = 99;
    diagnostic.runtime.sections.manager.sourceGeneration = 7;
    diagnostic.runtime.sections.sceneRuntime.sourceGeneration = 7;
    // Unchanged domains keep their identity in a merged delta.
    for (const name of [
      'alarms', 'componentOverview', 'programCycle', 'authoringAdmission',
      'authoringWorkspace', 'executionControlStatus',
    ] as const) {
      (diagnostic as unknown as Record<string, unknown>)[name] = current[name];
    }
    expect(sameShellPublication(current, diagnostic)).toBe(true);

    const visible = [
      (next: ComposerHeadlessState) => { next.runtime.sections.manager.freshness = 'stale'; },
      (next: ComposerHeadlessState) => { next.runtime.sections.logicalScene.sourceGeneration = 9; },
      (next: ComposerHeadlessState) => { next.runtime.sections.sceneRuntime.sourceGeneration = null; },
      (next: ComposerHeadlessState) => { next.runtime.reasons = ['manager_unavailable']; },
      (next: ComposerHeadlessState) => { next.alarms = { ...next.alarms, liveCount: 1 }; },
      (next: ComposerHeadlessState) => {
        next.authoringAdmission = { ...next.authoringAdmission, admissionGeneration: 2 };
      },
    ];
    for (const change of visible) {
      const next = { ...diagnostic, runtime: structuredClone(diagnostic.runtime) };
      change(next);
      expect(sameShellPublication(current, next)).toBe(false);
    }
  });

  it('keeps a retained viewer frame only while its owner and model identity holds', () => {
    const current = state('receiver.epoch.1', 2);
    const later = structuredClone(current);
    later.runtime.projectionGeneration += 3;
    later.runtime.sections.logicalScene.sourceGeneration = 5;
    expect(sameViewerIdentity(current, later)).toBe(true);
    for (const change of [
      (next: ComposerHeadlessState) => { next.receiverEpoch = 'receiver.epoch.2'; },
      (next: ComposerHeadlessState) => { next.runtime.projectionOwnerEpoch = 'other'; },
      (next: ComposerHeadlessState) => {
        next.runtime.sections.logicalScene.sourceOwnerEpoch = 'scene.epoch.2';
      },
      (next: ComposerHeadlessState) => {
        next.runtime.sections.manager.sourceOwnerEpoch = 'manager.epoch.2';
      },
      (next: ComposerHeadlessState) => {
        next.runtime.sections.manager.payload = {
          active: { retained_bundle_id: 'bundle.2', graph_revision: 'graph.2' },
        };
      },
    ]) {
      const next = structuredClone(current);
      change(next);
      expect(sameViewerIdentity(current, next)).toBe(false);
    }
  });

  it('invalidates the viewer when Manager evidence becomes unavailable', () => {
    const current = state('receiver.epoch.1', 2);
    for (const change of [
      { available: false }, { freshness: 'stale' as const },
      { knownGaps: ['lost_update'] }, { sourceOwnerEpoch: 'manager.epoch.2' },
      { sourceOwnerId: 'other.manager' }, { sourceGeneration: null },
    ]) {
      const next = structuredClone(current);
      Object.assign(next.runtime.sections.manager, change);
      expect(sameViewerProjection(current, next)).toBe(false);
    }
  });

  it('accepts forward stream frames and a replacement receiver epoch', () => {
    expect(admitHeadlessState(null, state('runtime.epoch.1', 2)).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 2),
      state('runtime.epoch.1', 3),
    ).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 20, 'alarms.epoch.1', 30),
      state('runtime.epoch.2', 0, 'alarms.epoch.2', 0),
    ).accepted).toBe(true);
  });

  it('does not duplicate nested owner-generation adjudication in the browser', () => {
    const current = state('receiver.epoch.1', 2, 'alarms.epoch.1', 6, 290);
    const candidate = state('receiver.epoch.1', 1, 'alarms.epoch.1', 5, 291);
    candidate.snapshotSequence = current.snapshotSequence + 1;
    expect(admitHeadlessState(current, candidate)).toEqual({
      accepted: true,
      reason: null,
    });
  });

  it('allows owner deltas at one local sequence but rejects local regression',
    () => {
      const current = state('runtime.epoch.1', 2);
      expect(admitHeadlessState(current, state('runtime.epoch.1', 2)).accepted)
        .toBe(true);
      expect(admitHeadlessState(current, state('runtime.epoch.1', 1)).accepted)
        .toBe(false);
      const changedWithoutSequence = state('runtime.epoch.1', 2);
      changedWithoutSequence.authoringWorkspace = {
        ...changedWithoutSequence.authoringWorkspace,
        programDraftRevision:
          changedWithoutSequence.authoringWorkspace.programDraftRevision + 1,
      };
      expect(admitHeadlessState(current, changedWithoutSequence).accepted)
        .toBe(false);
      expect(admitHeadlessState(current, state('runtime.epoch.2', 99)).accepted)
        .toBe(true);
    });
});

function wireState(generation: number): Record<string, unknown> {
  const section = {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    payload: {},
    source_generation: generation,
    source_owner_epoch: 'source.epoch.1',
    source_owner_id: 'source.owner',
    receiver_age_ns: 1,
  };
  return {
    schema_version: 'manyforge.composer_headless_state.v2',
    component_overview: {
      schema_version: 'manyforge.component_overview.v2',
      overview_revision: `sha256:${'a'.repeat(64)}`,
      readiness: 'ready',
      summary_label: 'Application surfaces ready',
      profile_label: 'native',
      authority_label: 'owner-issued scoped facts',
      user_message: 'Current.',
      capabilities: ['Observe / Render', 'Plan / Preview', 'Execute / Control'],
      components: [],
      maintenance_operation: null,
      basis: {
        runtime_projection_owner_epoch: null,
        runtime_projection_generation: null,
        manager_source_owner_epoch: null,
        logical_scene_source_owner_epoch: null,
        logical_scene_source_generation: null,
        scene_runtime_source_owner_epoch: null,
        scene_runtime_source_generation: null,
        alarm_owner_epoch: null,
        alarm_generation: null,
        alarm_state_revision: null,
        admission_generation: null,
        program_runner_owner_epoch: null,
        program_cycle_generation: null,
        applied_program_owner_epoch: null,
        applied_program_generation: null,
        execution_owner_epoch: null,
        execution_publication_generation: null,
      },
    },
    authoring_admission: { admission_generation: 1, authoring_allowed: true },
    held_run_draft_edit_allowed: false,
    execution_targets: null,
    execution_control_state: null,
    program_cycle: {
      program_runner_owner_epoch: 'runner.epoch.1',
      cycle_generation: 1,
      state: 'idle_after_reset',
      program_run_id: null,
      cycle_claim_generation: null,
      terminal_or_settlement_reference: null,
      run_revision: null,
      node_status_revision: null,
      runtime_control_revision: null,
      execution_control_revision: Math.max(1, generation),
      causal_correlation_id: null,
      causal_session_id: null,
      causal_client_sequence: null,
      current_command_kind: null,
      failure: null,
      reset_phase: null, reset_options: null,
      reset_robot_position: { available: false, reason: 'unsupported' },
      allowed_commands: [],
      recovery_offer: null,
      run: null,
      runtime_controls: null,
    },
    authoring_workspace: {
      workspace_epoch: 'workspace.epoch.1',
      workspace_revision: 1,
      program_draft_id: 'draft-1',
      program_draft_revision: 1,
      deployment_draft_id: 'deployment-draft-1',
      deployment_draft_revision: 1,
    },
    authoring_workspace_containment: null,
    receiver_epoch: 'receiver.epoch.1',
    snapshot_sequence: Math.max(1, generation + 1),
    complete: true,
    runtime: {
      schema_version: 'manyforge.headless_runtime_snapshot.v1',
      complete: true,
      projection_generation: generation,
      projection_owner_epoch: 'runtime.epoch.1',
      projection_owner_id: 'headless.owner',
      reasons: [],
      unavailable_sections: [],
      sections: {
        manager: section,
        logical_scene: section,
        scene_runtime: section,
      },
    },
    alarms: {
      schema_version: 'manyforge.operator_alarm_snapshot.v2',
      owner_id: 'alarm.owner',
      owner_epoch: 'alarms.epoch.1',
      generation,
      state_revision: generation,
      reconciled: true,
      health: { state: 'healthy' },
      live_count: 0,
      unacknowledged_count: 0,
      active_count: 0,
      active_acknowledged_count: 0,
      motion_attention_count: 0,
      motion_attention_membership: [],
      unacknowledged_membership: [],
      alarms: [],
      truncated: false,
    },
  };
}

type ControlTestSocket = {
  onmessage: ((message: MessageEvent) => void) | null;
  testServerSequence?: number;
};

const CONTROL_SESSION_ID = 'control-session.test-1';

function nextControlServerSequence(socket: ControlTestSocket): string {
  socket.testServerSequence = (socket.testServerSequence ?? 0) + 1;
  return String(socket.testServerSequence);
}

function receiveControl(socket: ControlTestSocket, value: unknown): void {
  socket.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
}

function controlFreshness(revision: number): Record<string, unknown> {
  return {
    context_id: `freshness.${revision}`,
    receiver_incarnation: 'headless.receiver.1',
    semantic_owner_epoch: 'runner.epoch.1',
    execution_control_revision: Math.max(1, revision),
    issued_server_sequence: String(Math.max(1, revision)),
    valid_for_ns: 750_000_000,
  };
}

function synchronizeControl(
  socket: ControlTestSocket,
  sessionId = CONTROL_SESSION_ID,
): void {
  socket.testServerSequence = 0;
  receiveControl(socket, {
    schema_version: 'manyforge.execution_control.v2',
    message_kind: 'server_hello',
    receiver_incarnation: 'headless.receiver.1',
    control_session_id: sessionId,
    heartbeat_period_ns: 250_000_000,
    heartbeat_delayed_ns: 500_000_000,
    heartbeat_stale_ns: 1_000_000_000,
    maximum_message_bytes: 65_536,
    maximum_messages_per_second: 64,
  });
  receiveControl(socket, {
    schema_version: 'manyforge.execution_control.v2',
    message_kind: 'heartbeat_challenge',
    control_session_id: sessionId,
    server_sequence: nextControlServerSequence(socket),
    challenge_sequence: '1',
    received_client_sequence: null,
    semantic_status: 'current',
    semantic_reason: null,
    freshness: controlFreshness(1),
  });
  receiveControl(socket, {
    schema_version: 'manyforge.execution_control.v2',
    message_kind: 'heartbeat_challenge',
    control_session_id: sessionId,
    server_sequence: nextControlServerSequence(socket),
    challenge_sequence: '2',
    received_client_sequence: '1',
    semantic_status: 'current',
    semantic_reason: null,
    freshness: controlFreshness(1),
  });
}

function receiveHeartbeat(
  socket: ControlTestSocket,
  challenge: number,
  receivedClientSequence: string,
  sessionId = CONTROL_SESSION_ID,
  controlRevision = 1,
): void {
  receiveControl(socket, {
    schema_version: 'manyforge.execution_control.v2',
    message_kind: 'heartbeat_challenge',
    control_session_id: sessionId,
    server_sequence: nextControlServerSequence(socket),
    challenge_sequence: String(challenge),
    received_client_sequence: receivedClientSequence,
    semantic_status: 'current',
    semantic_reason: null,
    freshness: controlFreshness(controlRevision),
  });
}

function receiveControlSnapshot(
  socket: ControlTestSocket,
  shell: Record<string, unknown>,
  sessionId = CONTROL_SESSION_ID,
  receivedClientSequence = '2',
): void {
  const cycle = shell.program_cycle as Record<string, unknown>;
  const revision = cycle.execution_control_revision as number;
  const serverSequence = nextControlServerSequence(socket);
  receiveControl(socket, {
    schema_version: 'manyforge.execution_control.v2',
    message_kind: 'snapshot',
    control_session_id: sessionId,
    server_sequence: serverSequence,
    state_sequence: serverSequence,
    received_client_sequence: receivedClientSequence,
    control: {
      receiver_incarnation: 'headless.receiver.1',
      semantic_owner_epoch: 'runner.epoch.1',
      execution_control_revision: revision,
      freshness: controlFreshness(revision),
    },
    shell,
    complete: true,
  });
}

function wireObservation(sequence: number): Record<string, unknown> {
  return {
    schema_version: 'manyforge.execution_observation_frame.v3',
    owner_epoch: 'execution-owner.1',
    cell_id: 'cell-1',
    retained_bundle_id: 'bundle-1',
    graph_revision: 'graph-1',
    source_epoch: 'joint-source.1',
    source_sequence: sequence,
    received_at_monotonic_us: 10_000 + sequence,
    targets: [{
      controllable_component_id: 'robot-1', command_target_id: 'arm',
      operation_capability_id: 'execution.trajectory.execute',
      source_epoch: 'arm-source.1', source_sequence: sequence,
      source_joint_names: ['joint1'], compiled_joint_names: ['robot__joint1'],
      coordinate_kinds: ['angular'], positions_si: [sequence / 100],
    }],
  };
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe('zero-physical-component viewer demand', () => {
  class Socket {
    static readonly OPEN = 1;
    static instances: Socket[] = [];
    readonly url: string;
    readonly sent: string[] = [];
    readyState = Socket.OPEN;
    onopen: (() => void) | null = null;
    onmessage: ((message: MessageEvent) => void) | null = null;
    onerror: (() => void) | null = null;
    onclose: (() => void) | null = null;
    constructor(url: string) { this.url = url; Socket.instances.push(this); }
    send(value: string): void { this.sent.push(value); }
    close(): void { this.readyState = 3; this.onclose?.(); }
    receive(value: unknown): void {
      this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
    }
  }

  function publication(generation: number, empty: boolean, canonicalEmpty = empty): Record<string, unknown> {
    const shell = wireState(generation);
    (shell.program_cycle as Record<string, unknown>).execution_control_revision = 1;
    const runtime = shell.runtime as Record<string, unknown>;
    const bundle = empty ? 'bundle.empty' : 'bundle-1';
    const graph = empty ? 'graph.empty' : 'graph-1';
    runtime.active_retained_bundle_id = bundle;
    runtime.active_graph_revision = graph;
    const sections = runtime.sections as Record<string, Record<string, unknown>>;
    // wireState deliberately shares its three generic source sections.
    sections.manager = { ...sections.manager, payload: {
      cell_id: 'cell-1', manager_ready: true, runtime: { converged: true },
      apply: { records: [] },
      active: {
        canonical_empty: canonicalEmpty, retained_bundle_id: bundle, graph_revision: graph,
        publication: { publication_generation: generation,
          projection_revision: { retained_bundle_id: bundle, graph_revision: graph } },
        cell_graph: { cell_id: 'cell-1', graph_revision: graph,
          model_instances: [], controllable_components: empty ? [] : [{ controllable_component_id: 'robot-1' }],
          control_bindings: [], control_domains: [],
          runtime_units: [{ runtime_unit_id: 'robot_cell.ros2_control_execution' }],
        },
      },
    } };
    return shell;
  }

  function setup(demand = true) {
    vi.useFakeTimers();
    Socket.instances = [];
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(() => 1), cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', Socket);
    const source = new HeadlessStateSource();
    if (demand) source.setViewerDemand(true);
    source.setEnabled(true);
    const control = Socket.instances.find((socket) => new URL(socket.url).pathname === '/ws/control')!;
    synchronizeControl(control);
    const viewers = () => Socket.instances.filter((socket) => new URL(socket.url).pathname === '/ws/scene/viewer');
    let challenge = 2;
    const advanceConnected = (milliseconds: number) => {
      for (let elapsed = 0; elapsed < milliseconds; elapsed += 250) {
        vi.advanceTimersByTime(Math.min(250, milliseconds - elapsed));
        const echoes = control.sent.map((value) => JSON.parse(value) as Record<string, unknown>)
          .filter((value) => value.message_kind === 'heartbeat_echo');
        receiveHeartbeat(control, ++challenge, echoes.at(-1)!.client_sequence as string);
      }
    };
    return { source, control, viewers, advanceConnected };
  }

  it.each([true, false])('starts no physical feed or history churn with positive empty facts, canonical=%s', (canonicalEmpty) => {
    const { source, control, viewers, advanceConnected } = setup(false);
    receiveControlSnapshot(control, publication(1, true, canonicalEmpty));
    source.setViewerDemand(true);
    expect(viewers()).toHaveLength(0);
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    const shellFrames = control.sent.length;
    advanceConnected(45_000);
    expect(viewers()).toHaveLength(0);
    expect(source.getLatestExecutionObservation()).toBeNull();
    // Empty observation produces no browser incident transitions for history.
    expect(control.sent.slice(shellFrames).some((frame) => JSON.parse(frame).message_kind === 'client_incident')).toBe(false);
    expect(source.getShellSnapshot().snapshot!.alarms.unacknowledgedCount).toBe(0);
    source.dispose();
  });

  it.each([false, true])('quiets Close after its fresh fact with viewer-close-first=%s', (closeFirst) => {
    const { source, control, viewers, advanceConnected } = setup();
    receiveControlSnapshot(control, publication(1, false));
    const viewer = viewers()[0]!;
    viewer.onopen?.();
    viewer.receive(wireObservation(1));
    source.markExecutionObservationApplied(source.getLatestExecutionObservation()!);
    expect(source.getLatestExecutionObservation()).not.toBeNull();
    if (closeFirst) {
      viewer.onclose?.();
      expect(source.getExecutionObservationTransportStatus().phase).toBe('reconnecting');
      vi.advanceTimersByTime(100);
    }
    receiveControlSnapshot(control, publication(2, true));
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    expect(source.getLatestExecutionObservation()).toBeNull();
    const creditCount = viewer.sent.length;
    // Queued callbacks from the retired socket cannot undo the empty barrier.
    viewer.onopen?.(); viewer.onerror?.(); viewer.onclose?.(); viewer.receive(wireObservation(2));
    advanceConnected(31_000);
    expect(viewers()).toHaveLength(1);
    expect(viewer.sent).toHaveLength(creditCount);
    expect(source.getLatestExecutionObservation()).toBeNull();
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    source.dispose();
  });

  it.each([true, false])('resumes loaded demand and preserves real failure from empty canonical=%s', (canonicalEmpty) => {
    const { source, control, viewers } = setup(false);
    receiveControlSnapshot(control, publication(1, true, canonicalEmpty));
    source.setViewerDemand(true);
    receiveControlSnapshot(control, publication(2, false));
    expect(viewers()).toHaveLength(1);
    const viewer = viewers()[0]!;
    viewer.onopen?.();
    expect(JSON.parse(viewer.sent[0]!)).toMatchObject({ last_applied: null });
    viewer.receive(wireObservation(1));
    expect(source.getExecutionObservationTransportStatus().phase).toBe('live');
    expect(source.getLatestExecutionObservation()!.targets[0]!.positionsSi).toEqual([0.01]);
    viewer.onerror?.(); viewer.onclose?.();
    expect(source.getExecutionObservationTransportStatus().phase).toBe('reconnecting');
    vi.advanceTimersByTime(1000);
    expect(viewers()).toHaveLength(2);
    source.dispose();
  });

  it.each([true, false])('removes quiet absence when the Manager proof becomes stale, canonical=%s', (canonicalEmpty) => {
    const { source, control, viewers } = setup(false);
    receiveControlSnapshot(control, publication(1, true, canonicalEmpty));
    source.setViewerDemand(true);
    const stale = publication(2, true, canonicalEmpty);
    const runtime = stale.runtime as Record<string, unknown>;
    const sections = runtime.sections as Record<string, Record<string, unknown>>;
    sections.manager.freshness = 'stale';
    receiveControlSnapshot(control, stale);
    expect(source.getExecutionObservationTransportStatus().phase).toBe('connecting');
    expect(viewers()).toHaveLength(1);
    viewers()[0]!.onerror?.();
    expect(source.getExecutionObservationTransportStatus().phase).toBe('reconnecting');
    source.dispose();
  });

  it('keeps hidden viewer notices quiet and resumes visibility or reconnect with fresh facts', async () => {
    const { source, control, viewers, advanceConnected } = setup(false);
    receiveControlSnapshot(control, publication(1, true));
    source.setViewerDemand(true);
    source.setViewerDemand(false);
    await Promise.resolve();
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    advanceConnected(31_000);
    // The controls remain mounted while document visibility releases demand.
    // Their absent robot status must not become a new error/incident then.
    const guidance = deriveTransportGuidance({
      enabled: true,
      program: { state: { controls: null, run: null }, cycleState: 'idle_after_reset',
        cycleFailure: null, resetRobotPositionAvailability: null, commandDisabledReason: () => null },
      robot: { currentStatus: null, state: { status: null } },
      motionActive: false, onReviewProgramRecovery: vi.fn(),
      observationTransport: source.getExecutionObservationTransportStatus(),
    } as unknown as Parameters<typeof deriveTransportGuidance>[0]);
    expect(guidance).toEqual([]);
    expect(viewers()).toHaveLength(0);
    source.setViewerDemand(true);
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    expect(viewers()).toHaveLength(0);
    source.reconnect();
    const replacement = Socket.instances.filter((socket) => new URL(socket.url).pathname === '/ws/control')[1]!;
    synchronizeControl(replacement);
    receiveControlSnapshot(replacement, publication(2, true));
    expect(source.getExecutionObservationTransportStatus().phase).toBe('no_robot_loaded');
    expect(viewers().every((socket) => socket.readyState === 3)).toBe(true);
    source.dispose();
  });
});

describe('HeadlessStateSource delivery scheduling', () => {
  it.each([
    [422, 'program_run_started_then_failed', 'Run failed:', true, false],
    [422, 'program_run_superseded_by_stop', 'Run stopped. Reset required', true, false],
    [409, 'program_run_stopped_before_start', 'Run stopped before start. Reset required', true, false],
    [503, 'program_cycle_reconciliation_required', 'Command delivery is unconfirmed', false, false],
    [500, 'program_runner_internal_error', 'Command delivery is unconfirmed', false, false],
    [422, 'program_run_started_then_failed', 'Run outcome could not be read', true, true],
  ] as const)('distinguishes known Run outcomes from unknown delivery for HTTP %s/%s', async (
    status, code, message, confirmed, malformed,
  ) => {
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const fetch = vi.fn().mockResolvedValue({
      ok: false, status,
      json: async () => ({ detail: {
        code, message: code === 'program_run_started_then_failed'
          ? 'Required component changed after claim' : 'Operator Stop won the start',
        details: { program_run_id: 'program-cycle.2',
          claim_generation: code === 'program_run_stopped_before_start' ? null : 9,
          needs_reset: malformed ? 'true' : true, component_id: 'arm.1',
          cause_code: code === 'program_run_started_then_failed'
            ? 'program_required_component_not_active' : 'program_run_superseded_by_stop',
          cause_message: code === 'program_run_started_then_failed'
            ? 'Required component changed after claim' : 'Operator Stop won the start',
          ...(['program_run_superseded_by_stop', 'program_run_stopped_before_start'].includes(code)
            ? { stopped_by_operator: true } : {}),
          ...(code === 'program_run_stopped_before_start' ? { no_effect: true } : {}),
        },
      } }),
    });
    vi.stubGlobal('fetch', fetch);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    try {
      const failure = await source.sendExecutionCommand(
        'program_run', { scenario_id: 'lite6' }, 'run.http.outcome',
      ).catch((error: unknown) => error);
      expect(failure).toBeInstanceOf(Error);
      expect((failure as Error).message).toContain(message);
      if (confirmed && !malformed) {
        expect((failure as Error).message).toContain('program-cycle.2');
        expect((failure as Error).message).toContain(code === 'program_run_started_then_failed'
          ? 'Required component changed after claim' : 'Operator Stop won the start');
        expect((failure as Error).message).toContain('Reset required');
      }
      if (malformed) expect((failure as Error).message).not.toContain('Reset required');
      expect(FakeWebSocket.shell.readyState).toBe(confirmed ? FakeWebSocket.OPEN : 3);
      expect(FakeWebSocket.shell.sent.some((frame) =>
        JSON.parse(frame).incident_kind === 'command_delivery_unconfirmed')).toBe(!confirmed);
      expect(fetch).toHaveBeenCalledTimes(1);
    } finally {
      source.dispose();
    }
  });

  it('sends new execution through Composer admission without consuming a WS command sequence', async () => {
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const sent = vi.fn<(url: string, request: RequestInit) => Promise<
      Pick<Response, 'ok' | 'json'>
    >>(async () => ({
      ok: true,
      json: async () => ({
        receipt: {
          schema_version: 'manyforge.execution_command_receipt.v1',
          correlation_id: 'run.http.1',
          control_session_id: null,
          client_sequence: null,
          receiver_incarnation: 'headless.receiver.1',
          received_server_sequence: '10',
          received_monotonic_ns: '100',
          disposition: 'received',
          code: 'program_run_received',
          semantic_owner_epoch: 'runner.epoch.1',
          execution_control_revision: 1,
        },
        result: { accepted: true },
      }),
    }));
    vi.stubGlobal('fetch', sent);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    await expect(source.sendExecutionCommand(
      'program_run', { scenario_id: 'lite6' }, 'run.http.1',
    )).resolves.toMatchObject({ disposition: 'received', code: 'program_run_received' });
    const [url, request] = sent.mock.calls[0]!;
    expect(new URL(url, 'http://composer.test/').pathname).toBe('/api/control/commands');
    const body = JSON.parse(String(request.body)) as {
      command_envelope: { command_kind: string; control_session_id: null };
    };
    expect(body.command_envelope).toMatchObject({
      command_kind: 'program_run', control_session_id: null,
    });
    expect(FakeWebSocket.shell.sent.every((frame) =>
      (JSON.parse(frame) as { message_kind: string }).message_kind !== 'command',
    )).toBe(true);
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('connection lost')));
    await expect(source.sendExecutionCommand(
      'program_run', { scenario_id: 'lite6' }, 'run.http.unknown',
    )).rejects.toThrow('Command delivery is unconfirmed');
    expect(FakeWebSocket.shell.readyState).toBe(3);
    source.dispose();
  });

  it('shows the owner refusal details on the HTTP, REST receipt and WS receipt paths', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    try {
      // The captured Trial-E Execute refusal (HTTP 409 from Kernel admission).
      vi.stubGlobal('fetch', vi.fn(async () => ({
        ok: false,
        status: 409,
        json: async () => ({
          detail: {
            code: 'stale_generation',
            details: { reason: 'expected_generation_stale' },
            message: 'cell-admission request rejected',
          },
        }),
      })));
      await expect(source.sendExecutionCommand(
        'program_run', { scenario_id: 'lite6' }, 'run.http.refused',
      )).rejects.toThrow(
        /^cell-admission request rejected \(Reason: expected_generation_stale\)$/,
      );

      vi.stubGlobal('fetch', vi.fn(async () => ({
        ok: true,
        status: 200,
        json: async () => ({
          receipt: {
            schema_version: 'manyforge.execution_command_receipt.v1',
            correlation_id: 'run.http.rejected',
            control_session_id: null,
            client_sequence: null,
            receiver_incarnation: 'headless.receiver.1',
            received_server_sequence: '11',
            received_monotonic_ns: '101',
            disposition: 'rejected',
            code: 'invalid_request',
            semantic_owner_epoch: 'runner.epoch.1',
            execution_control_revision: 1,
          },
          result: {
            ok: false,
            error: {
              code: 'invalid_request',
              message: 'cell-admission request rejected',
              details: { reason: 'claim_capacity_exhausted' },
            },
          },
        }),
      })));
      await expect(source.sendExecutionCommand(
        'program_run', { scenario_id: 'lite6' }, 'run.http.rejected',
      )).rejects.toThrow(
        /^invalid_request: cell-admission request rejected \(Reason: claim_capacity_exhausted\)$/,
      );

      const pending = source.sendExecutionCommand('program_reset', {}, 'reset.ws.rejected');
      const command = JSON.parse(FakeWebSocket.shell.sent.at(-1)!) as { client_sequence: string };
      receiveControl(FakeWebSocket.shell, {
        schema_version: 'manyforge.execution_control.v2',
        message_kind: 'command_receipt',
        control_session_id: CONTROL_SESSION_ID,
        server_sequence: nextControlServerSequence(FakeWebSocket.shell),
        received_client_sequence: command.client_sequence,
        emitted_monotonic_ns: '200',
        receipt: {
          schema_version: 'manyforge.execution_command_receipt.v1',
          correlation_id: 'reset.ws.rejected',
          control_session_id: CONTROL_SESSION_ID,
          client_sequence: command.client_sequence,
          receiver_incarnation: 'headless.receiver.1',
          received_server_sequence: '12',
          received_monotonic_ns: '102',
          disposition: 'rejected',
          code: 'stale_generation',
          semantic_owner_epoch: 'runner.epoch.1',
          execution_control_revision: 1,
          current_identity: null,
        },
        result: {
          code: 'stale_generation',
          correlation_id: 'reset.ws.rejected',
          message: 'cell-admission request rejected',
          details: { reason: 'expected_generation_stale' },
        },
      });
      await expect(pending).rejects.toThrow(
        /^stale_generation: cell-admission request rejected \(Reason: expected_generation_stale\)$/,
      );
    } finally {
      source.dispose();
    }
  });

  it('strictly parses compact execution observations', () => {
    expect(parseExecutionObservationFrame(wireObservation(2))).toMatchObject({
      sourceSequence: 2,
      targets: [{ positionsSi: [0.02] }],
    });
    const crossed = wireObservation(3);
    (crossed.targets as Record<string, unknown>[])[0].compiled_joint_names = ['a', 'b'];
    expect(() => parseExecutionObservationFrame(crossed)).toThrow(
      /joint vectors/,
    );
    // H1a-IF.2: a cell may have no observable coordinate (for example only
    // an unsupported capability); each target names its capability.
    const empty = wireObservation(4);
    empty.targets = [];
    expect(parseExecutionObservationFrame(empty).targets).toEqual([]);
    expect(parseExecutionObservationFrame(wireObservation(5)).targets[0].operationCapabilityId)
      .toBe('execution.trajectory.execute');
  });

  it('uses the same UTF-8 identity byte bounds as the owner and proxy', () => {
    const frame = wireObservation(2);
    frame.source_epoch = '界'.repeat(682);
    expect(parseExecutionObservationFrame(frame).sourceEpoch).toBe(frame.source_epoch);
    frame.source_epoch = '界'.repeat(683);
    expect(() => parseExecutionObservationFrame(frame)).toThrow(/source_epoch/);
    frame.source_epoch = 'source';
    (frame.targets as Record<string, unknown>[])[0].source_joint_names = ['界'.repeat(683)];
    expect(() => parseExecutionObservationFrame(frame)).toThrow(/source_joint_names/);
  });

  it('coalesces an observation burst without publishing React state lanes', () => {
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instance: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) { void url; FakeWebSocket.instance = this; }
      close(): void {}
      send(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    let shellNotifications = 0;
    let viewerNotifications = 0;
    let observationNotifications = 0;
    source.subscribeShell(() => { shellNotifications += 1; });
    source.subscribeViewer(() => { viewerNotifications += 1; });
    source.subscribeExecutionObservation(() => { observationNotifications += 1; });
    source.setViewerDemand(true);
    source.setEnabled(true);
    const shellBaseline = shellNotifications;
    const viewerBaseline = viewerNotifications;
    const observationBaseline = observationNotifications;
    for (let sequence = 1; sequence <= 100; sequence += 1) {
      FakeWebSocket.instance.send(wireObservation(sequence));
    }
    expect(source.getExecutionObservationTransportStatus()).toEqual({
      phase: 'live', message: null,
    });
    expect(animationFrames).toHaveLength(1);
    expect(source.getLatestExecutionObservation()?.sourceSequence).toBe(100);
    expect(shellNotifications).toBe(shellBaseline);
    expect(viewerNotifications).toBe(viewerBaseline);
    expect(observationNotifications).toBe(observationBaseline);
    animationFrames[0]!(16);
    expect(observationNotifications).toBe(observationBaseline + 1);
    expect(shellNotifications).toBe(shellBaseline);
    expect(viewerNotifications).toBe(viewerBaseline);
    source.dispose();
  });

  it('replaces a restarted owner epoch and ignores frames from the retired socket', () => {
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) { this.url = url; FakeWebSocket.instances.push(this); }
      close(): void {}
      send(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setViewerDemand(true);
    source.setEnabled(true);
    const firstSocket = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )!;
    firstSocket.send(wireObservation(50));
    animationFrames.shift()!(16);
    expect(source.getLatestExecutionObservation()?.sourceSequence).toBe(50);

    source.reconnect();
    expect(FakeWebSocket.instances).toHaveLength(4);
    const restarted = wireObservation(1);
    restarted.owner_epoch = 'execution-owner.2';
    restarted.source_epoch = 'joint-source.2';
    FakeWebSocket.instances.filter(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )[1]!.send(restarted);
    animationFrames.shift()!(32);
    expect(source.getLatestExecutionObservation()).toMatchObject({
      ownerEpoch: 'execution-owner.2',
      sourceEpoch: 'joint-source.2',
      sourceSequence: 1,
    });

    firstSocket.send(wireObservation(51));
    expect(animationFrames).toHaveLength(0);
    expect(source.getLatestExecutionObservation()?.ownerEpoch)
      .toBe('execution-owner.2');
    source.dispose();
  });

  it('uses separate read-only shell and viewer endpoints without client authority', () => {
    vi.useFakeTimers();
    const urls: string[] = [];
    class FakeWebSocket {
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) { urls.push(url); }
      close(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(), cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setViewerDemand(true);
    source.setEnabled(true);
    source.reconnect();
    expect(urls).toHaveLength(4);
    expect(new URL(urls[0]!).pathname).toBe('/ws/control');
    expect(new URL(urls[0]!).search).toBe('');
    expect(new URL(urls[1]!).pathname).toBe('/ws/scene/viewer');
    expect(new URL(urls[1]!).search).toBe('');
    expect(urls[2]).toBe(urls[0]);
    expect(urls[3]).toBe(urls[1]);
    source.dispose();
  });

  it('publishes admission changes to the shell without waking the viewer', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    const first = wireState(1);
    const second = structuredClone(first);
    second.snapshot_sequence = 3;
    second.authoring_admission = {
      admission_generation: 2, authoring_allowed: true,
    };
    receiveControlSnapshot(FakeWebSocket.shell, first);
    expect(source.getShellSnapshot().error).toBeNull();
    expect(animationFrames).toHaveLength(1);
    animationFrames[0]!(16);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    const viewerBaseline = source.getViewerSnapshot();
    receiveControlSnapshot(FakeWebSocket.shell, second);
    expect(animationFrames).toHaveLength(1);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getViewerSnapshot()).toBe(viewerBaseline);
    expect(source.getShellSnapshot().snapshot?.snapshotSequence).toBe(3);
    expect(source.getShellSnapshot().snapshot?.authoringAdmission.admissionGeneration)
      .toBe(2);
    const changed = vi.fn();
    source.subscribeShell(changed);
    // An ordinary publication is pending when a critical owner delta arrives.
    receiveControlSnapshot(FakeWebSocket.shell, second);
    changed.mockClear();
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: '4',
      received_client_sequence: '2',
      event_class: 'critical',
      control: {
        receiver_incarnation: 'headless.receiver.1',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1,
        freshness: controlFreshness(1),
      },
      shell: {
        complete: false,
        authoring_admission: { admission_generation: 3, authoring_allowed: false },
      },
    });
    expect(source.getShellSnapshot().error).toBeNull();
    expect(source.getShellSnapshot().snapshot?.authoringAdmission.admissionGeneration)
      .toBe(3);
    expect(changed).toHaveBeenCalledTimes(1);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(changed).toHaveBeenCalledTimes(1);
    expect(source.getViewerSnapshot()).toBe(viewerBaseline);
    source.dispose();
  });

  it.each([null, ...localDeltaFixture.invalid_overlays])(
    'shares the local Draft-delta contract with Python: %j', (invalidOverlay) => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    const initial = wireState(1);
    receiveControlSnapshot(FakeWebSocket.shell, initial);
    animationFrames[0]!(16);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);

    receiveControl(FakeWebSocket.shell, {
      ...localDeltaFixture.local_delta,
      ...(invalidOverlay ?? {}),
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
    });
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);

    if (invalidOverlay !== null) {
      expect(source.getShellSnapshot().error).not.toBeNull();
      source.dispose();
      return;
    }
    expect(source.getShellSnapshot().error).toBeNull();
    expect(source.getShellSnapshot().snapshot?.authoringWorkspace).toEqual({
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 7,
      programDraftId: 'draft-1',
      programDraftRevision: 2,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 4,
    });
    expect(
      source.getViewerSnapshot().snapshot?.authoringWorkspace.programDraftRevision,
    ).toBe(1);
    source.dispose();
  });

  it('merges a typed owner delta without retransmitting unchanged domains', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', { location: { href: 'http://composer.test/' } });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    const initial = wireState(1);
    receiveControlSnapshot(FakeWebSocket.shell, initial);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);

    const shellChanged = vi.fn();
    const viewerChanged = vi.fn();
    const progressChanged = vi.fn();
    source.subscribeShell(shellChanged);
    source.subscribeViewer(viewerChanged);
    source.subscribeOperationProgress(progressChanged);
    const progress = [{ owner_epoch: 'owner.1', operation_id: 'op.1', operation_revision: 1, progress: 0.6 }];
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2', message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: '2', received_client_sequence: '2', event_class: 'cold',
      control: {
        receiver_incarnation: 'headless.receiver.1', semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1, freshness: controlFreshness(1),
      },
      shell: { complete: false, operation_progress: progress },
    });
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getOperationProgress()).toEqual(progress);
    expect(progressChanged).toHaveBeenCalledTimes(1);
    expect(shellChanged).not.toHaveBeenCalled();
    expect(viewerChanged).not.toHaveBeenCalled();

    // Global rule states ride the same pump-only delta path; progress stays.
    expect(source.getGlobalRules()).toBeNull();
    const rules = [{ rule_id: 'door', state: 'latched', keys: [] }];
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2', message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: '2', received_client_sequence: '2', event_class: 'cold',
      control: {
        receiver_incarnation: 'headless.receiver.1', semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1, freshness: controlFreshness(1),
      },
      shell: { complete: false, global_rules: rules },
    });
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getGlobalRules()).toEqual([{ ruleId: 'door', state: 'latched', keys: [] }]);
    expect(source.getOperationProgress()).toEqual(progress);
    expect(progressChanged).toHaveBeenCalledTimes(2);
    expect(shellChanged).not.toHaveBeenCalled();
    expect(viewerChanged).not.toHaveBeenCalled();

    const cycle = structuredClone(initial.program_cycle) as Record<string, unknown>;
    cycle.execution_control_revision = 2;
    cycle.state = 'starting';
    const local = {
      complete: false,
      program_cycle: cycle,
      authoring_admission: {
        admission_generation: 2,
        authoring_allowed: false,
      },
      held_run_draft_edit_allowed: true,
    };
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: '2',
      received_client_sequence: '2',
      event_class: 'critical',
      control: {
        receiver_incarnation: 'headless.receiver.1',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 2,
        freshness: controlFreshness(2),
      },
      shell: local,
    });
    receiveHeartbeat(FakeWebSocket.shell, 3, '2', CONTROL_SESSION_ID, 2);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);

    const merged = source.getShellSnapshot().snapshot!;
    expect(source.getShellSnapshot().error).toBeNull();
    expect(merged.programCycle.state).toBe('starting');
    expect(merged.authoringAdmission).toEqual({
      admissionGeneration: 2, authoringAllowed: false,
    });
    // The backend's held-run Draft rule travels beside the admission.
    expect(merged.heldRunDraftEditAllowed).toBe(true);
    expect(merged.authoringWorkspace).toEqual({
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 1,
      programDraftId: 'draft-1',
      programDraftRevision: 1,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 1,
    });
    expect(merged.snapshotSequence).toBe(initial.snapshot_sequence);
    expect(merged.runtime).toBe(source.getViewerSnapshot().snapshot?.runtime);
    expect(merged.alarms.ownerEpoch).toBe('alarms.epoch.1');
    source.dispose();
  });

  it('advances exact capture identity across visually equivalent heartbeats', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    const first = wireState(1);
    const manager = (first.runtime as {
      sections: { manager: { payload: Record<string, unknown> } };
    }).sections.manager;
    manager.payload = { apply: { records: [{ incomplete: true }] } };
    receiveControlSnapshot(FakeWebSocket.shell, first);
    animationFrames[0]!(16);
    const presented = source.getViewerSnapshot().snapshot;
    source.markViewerPresented(presented);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 1,
    });

    const heartbeat = structuredClone(first);
    heartbeat.snapshot_sequence = 3;
    const heartbeatRuntime = heartbeat.runtime as Record<string, unknown>;
    heartbeatRuntime.projection_generation = 2;
    const sections = heartbeatRuntime.sections as Record<string, Record<string, unknown>>;
    sections.manager = { ...sections.manager, source_generation: 2 };
    receiveControlSnapshot(FakeWebSocket.shell, heartbeat);
    expect(animationFrames).toHaveLength(1);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 2,
    });

    const sceneChange = structuredClone(heartbeat);
    sceneChange.snapshot_sequence = 4;
    const sceneRuntime = sceneChange.runtime as Record<string, unknown>;
    sceneRuntime.projection_generation = 3;
    const sceneSections = sceneRuntime.sections as Record<
      string, Record<string, unknown>
    >;
    sceneSections.logical_scene = {
      ...sceneSections.logical_scene,
      source_generation: 2,
    };
    receiveControlSnapshot(FakeWebSocket.shell, sceneChange);
    expect(animationFrames).toHaveLength(2);
    expect(source.getPresentedRuntimeFrame()?.generation).toBe(2);
    animationFrames[1]!(32);
    source.markViewerPresented(source.getViewerSnapshot().snapshot);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 3,
    });

    // The Scene has already published. Apply completion must independently
    // reach the viewer rather than leave it holding the pending snapshot.
    const completed = structuredClone(sceneChange);
    completed.snapshot_sequence = 5;
    const completedRuntime = completed.runtime as typeof sceneRuntime;
    completedRuntime.projection_generation = 4;
    const completedSections = completedRuntime.sections as typeof sceneSections;
    completedSections.manager = {
      ...completedSections.manager,
      payload: { apply: { records: [{ incomplete: false }] } },
    };
    receiveControlSnapshot(FakeWebSocket.shell, completed);
    expect(animationFrames).toHaveLength(3);
    animationFrames[2]!(48);
    expect(source.getViewerSnapshot().snapshot?.runtime.sections.manager.payload)
      .toEqual({ apply: { records: [{ incomplete: false }] } });
    source.dispose();
  });

  it('keeps one socket per plane and coalesces a shell burst by delivery lane', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {}

      open(): void {
        this.onopen?.();
      }

      send(): void {}

      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback);
        return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    let viewerNotifications = 0;
    let shellNotifications = 0;
    source.subscribeViewer(() => { viewerNotifications += 1; });
    source.subscribeShell(() => { shellNotifications += 1; });
    source.setViewerDemand(true);
    source.setEnabled(true);

    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        sourceCount: 1,
        activeSockets: 0,
      }),
    );
    const shellSocket = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/control',
    )!;
    const viewerSocket = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )!;
    viewerSocket.open();
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 1,
        maxActiveSockets: 1,
      }),
    );
    synchronizeControl(shellSocket);
    const initialShell = wireState(1);
    (initialShell.program_cycle as Record<string, unknown>)
      .execution_control_revision = 1;
    receiveControlSnapshot(shellSocket, initialShell);
    const viewerBaseline = viewerNotifications;
    const shellBaseline = shellNotifications;
    for (let generation = 2; generation <= 100; generation += 1) {
      const shell = wireState(generation);
      (shell.program_cycle as Record<string, unknown>).execution_control_revision = 1;
      receiveControlSnapshot(shellSocket, shell);
    }
    expect(animationFrames).toHaveLength(1);
    expect(source.getViewerSnapshot().error).toBeNull();
    expect(viewerNotifications).toBe(viewerBaseline);
    expect(shellNotifications).toBe(shellBaseline);

    animationFrames[0]!(16);
    expect(viewerNotifications).toBe(viewerBaseline + 1);
    expect(source.getViewerSnapshot().snapshot?.runtime.projectionGeneration).toBe(100);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS - 1);
    expect(shellNotifications).toBe(shellBaseline);
    vi.advanceTimersByTime(1);
    expect(shellNotifications).toBe(shellBaseline + 1);
    expect(source.getShellSnapshot().snapshot?.runtime.projectionGeneration).toBe(100);
    expect(FakeWebSocket.instances).toHaveLength(2);
    source.dispose();
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
  });

  it('counts only opened sockets through a failed transport reconnect', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      open(): void {
        this.onopen?.();
      }

      fail(): void {
        this.onerror?.();
        this.onclose?.();
      }

      close(): void {
        this.onclose?.();
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    const initialCreations = getHeadlessStateQualificationSnapshot().socketCreations;
    source.setViewerDemand(true);
    source.setEnabled(true);
    expect(source.getExecutionObservationTransportStatus()).toMatchObject({
      phase: 'connecting',
    });
    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 0,
        socketCreations: initialCreations + 1,
      }),
    );

    const firstViewer = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )!;
    firstViewer.open();
    expect(source.getExecutionObservationTransportStatus()).toMatchObject({
      phase: 'waiting',
    });
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(1);
    firstViewer.fail();
    expect(source.getExecutionObservationTransportStatus()).toMatchObject({
      phase: 'reconnecting',
    });
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);

    vi.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(3);
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
    FakeWebSocket.instances[2]!.open();
    expect(source.getExecutionObservationTransportStatus()).toMatchObject({
      phase: 'waiting',
    });
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 1,
        maxActiveSockets: 1,
      }),
    );
    source.dispose();
    expect(source.getExecutionObservationTransportStatus()).toEqual({
      phase: 'disabled', message: null,
    });
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
  });

  it.each(['disconnect', 'model rejection', 'explicit reconnect'])(
    'keeps viewer delivery independent of control health and drops unapplied frames on %s', (recovery) => {
    vi.useFakeTimers();
    const animationFrames = new Map<number, FrameRequestCallback>();
    let nextAnimationFrame = 0;
    class FakeWebSocket {
      static readonly OPEN = 1;
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      open(): void { this.onopen?.(); }
      send(value: string): void { this.sent.push(value); }
      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
      fail(): void {
        this.readyState = 3;
        this.onerror?.();
        this.onclose?.();
      }
      close(code = 1000): void {
        if (code !== 1000 && (code < 3000 || code > 4999)) {
          throw new Error('browser WebSocket.close rejects reserved protocol codes');
        }
        if (this.readyState !== FakeWebSocket.OPEN) return;
        this.readyState = 3;
        this.onclose?.();
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        nextAnimationFrame += 1;
        animationFrames.set(nextAnimationFrame, callback);
        return nextAnimationFrame;
      },
      cancelAnimationFrame: (id: number) => { animationFrames.delete(id); },
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    const applied: number[] = [];
    source.subscribeExecutionObservation(() => {
      const frame = source.getLatestExecutionObservation();
      if (frame === null || applied.at(-1) === frame.sourceSequence) return;
      applied.push(frame.sourceSequence);
      source.markExecutionObservationApplied(frame);
    });
    source.setViewerDemand(true);
    source.setEnabled(true);
    const viewer = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )!;
    viewer.open();
    viewer.sendFrame(wireObservation(1));
    const firstCallback = animationFrames.get(1)!;
    animationFrames.delete(1);
    firstCallback(16);
    expect(applied).toEqual([1]);

    viewer.sendFrame(wireObservation(2));
    expect(animationFrames.size).toBe(1);
    const control = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/control',
    )!;
    control.onerror?.();
    expect(source.getShellSnapshot().transportStatus).toBe('disconnected');
    expect(animationFrames.size).toBe(1);
    const [secondId, secondCallback] = [...animationFrames.entries()][0]!;
    animationFrames.delete(secondId);
    secondCallback(32);
    expect(applied).toEqual([1, 2]);

    viewer.sendFrame(wireObservation(3));
    if (recovery === 'disconnect') viewer.fail();
    else if (recovery === 'model rejection') {
      source.rejectExecutionObservation(source.getLatestExecutionObservation()!);
      // A model transition discards the incompatible frame in place. It must
      // not replace the socket and create reconnect churn while the structural
      // model refreshes.
      expect(source.getExecutionObservationTransportStatus()).toMatchObject({
        phase: 'synchronizing',
      });
      expect(viewer.readyState).toBe(FakeWebSocket.OPEN);
      expect(JSON.parse(viewer.sent.at(-1)!)).toMatchObject({
        last_applied: { source_sequence: 3 },
      });
      source.markExecutionObservationApplied(source.getLatestExecutionObservation()!);
      expect(source.getExecutionObservationTransportStatus()).toMatchObject({
        phase: 'live',
      });
    } else source.reconnect();
    expect(animationFrames.size).toBe(0);
    expect(source.getLatestExecutionObservation()?.sourceSequence).toBe(
      recovery === 'model rejection' ? 3 : 2,
    );

    let replacement = viewer;
    if (recovery !== 'model rejection') {
      vi.advanceTimersByTime(1_000);
      replacement = FakeWebSocket.instances.filter(
        (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
      ).at(-1)!;
      replacement.open();
    }
    replacement.sendFrame(wireObservation(4));
    // A repeated unrendered frame is not an applied acknowledgement.
    replacement.sendFrame(wireObservation(4));
    expect(replacement.sent.length).toBeGreaterThanOrEqual(
      recovery === 'model rejection' ? 2 : 1,
    );
    expect(animationFrames.size).toBe(1);
    const [currentId, currentCallback] = [...animationFrames.entries()][0]!;
    animationFrames.delete(currentId);
    currentCallback(48);

    expect(applied).toEqual([1, 2, 4]);
    expect(source.getLatestExecutionObservation()?.sourceSequence).toBe(4);
    expect(JSON.parse(replacement.sent.at(-1)!)).toMatchObject({
      last_applied: { source_sequence: 4 },
    });
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('renews one visible static demand for more than 24 hours without frame work', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static readonly OPEN = 1;
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {
        this.readyState = 3;
      }

      open(): void {
        this.onopen?.();
      }

      send(message: string): void {
        this.sent.push(message);
      }

      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback);
        return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    let observationNotifications = 0;
    source.subscribeExecutionObservation(() => { observationNotifications += 1; });
    source.setViewerDemand(true);
    source.setEnabled(true);
    const viewer = FakeWebSocket.instances.find(
      (socket) => new URL(socket.url).pathname === '/ws/scene/viewer',
    )!;
    viewer.open();
    viewer.sendFrame(wireObservation(1));
    expect(animationFrames).toHaveLength(1);
    animationFrames.shift()!(16);
    const frame = source.getLatestExecutionObservation();
    expect(frame).not.toBeNull();
    source.markExecutionObservationApplied(frame!);
    const appliedNotifications = observationNotifications;

    vi.advanceTimersByTime(25 * 60 * 60 * 1000);

    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(animationFrames).toHaveLength(0);
    expect(observationNotifications).toBe(appliedNotifications);
    expect(source.getLatestExecutionObservation()).toBe(frame);
    expect(viewer.sent.length).toBeGreaterThan(6_000);
    expect(viewer.sent.every((message) => {
      const credit = JSON.parse(message) as Record<string, unknown>;
      return credit.schema_version
          === 'manyforge.execution_observation_credit.v1';
    })).toBe(true);
    expect(JSON.parse(viewer.sent.at(-1)!)).toMatchObject({
      last_applied: {
        owner_epoch: 'execution-owner.1',
        source_epoch: 'joint-source.1',
        source_sequence: 1,
      },
    });
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('fails shell authority closed while retaining coherent viewer structure', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void {}

      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback);
        return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(7));
    animationFrames.shift()!(16);
    const coherent = source.getViewerSnapshot().snapshot;
    expect(coherent?.runtime.projectionGeneration).toBe(7);
    source.markViewerPresented(coherent);

    FakeWebSocket.shell.onerror?.();

    expect(source.getShellSnapshot()).toMatchObject({
      snapshot: coherent,
      error: 'Headless WebSocket transport failed.',
    });
    expect(source.getViewerSnapshot()).toMatchObject({
      snapshot: coherent,
      error: 'Headless WebSocket transport failed.',
    });
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 7,
    });
    source.dispose();
  });

  it('disables control while delayed or stale and recovers without reconnect churn', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readyState = FakeWebSocket.OPEN;
      closed = false;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void {
        this.closed = true;
        this.readyState = 3;
        this.onclose?.();
      }

      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getShellSnapshot().error).toBeNull();

    vi.advanceTimersByTime(450);
    expect(source.getShellSnapshot()).toMatchObject({
      snapshot: expect.any(Object),
      error: 'Control connection is delayed; commands are disabled.',
      transportStatus: 'delayed',
      semanticStatus: 'current',
    });

    receiveHeartbeat(FakeWebSocket.shell, 3, '2');
    expect(source.getShellSnapshot().error).toContain('delayed');
    receiveHeartbeat(FakeWebSocket.shell, 4, '3');
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getShellSnapshot().error).toBeNull();

    vi.advanceTimersByTime(500);
    expect(source.getShellSnapshot().error).toContain('delayed');
    vi.advanceTimersByTime(500);
    expect(FakeWebSocket.shell.closed).toBe(false);
    expect(source.getShellSnapshot()).toMatchObject({
      snapshot: expect.any(Object),
      error: 'Control connection is stale; commands are disabled.',
      transportStatus: 'stale',
      semanticStatus: 'current',
    });

    const retainedSocket = FakeWebSocket.shell;
    // The first delayed challenge is echoed but cannot itself prove recovery.
    receiveHeartbeat(retainedSocket, 5, '4');
    expect(source.getShellSnapshot().error).toContain('stale');
    // The following challenge confirms that post-delay echo and restores the
    // exact same session without a cold snapshot or a replacement socket.
    receiveHeartbeat(retainedSocket, 6, '5');
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(FakeWebSocket.shell).toBe(retainedSocket);
    expect(retainedSocket.closed).toBe(false);
    expect(source.getShellSnapshot()).toMatchObject({
      snapshot: expect.any(Object),
      error: null,
    });
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('marks retained global rules not current while delayed, stale or unavailable', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void { this.readyState = 3; this.onclose?.(); }
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    const rulesChanged = vi.fn();
    source.subscribeGlobalRules(rulesChanged);
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, {
      ...wireState(1), global_rules: [{ rule_id: 'door', state: 'clear', keys: [] }],
    });
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    const rows = source.getGlobalRules();
    expect(rows).toEqual([{ ruleId: 'door', state: 'clear', keys: [] }]);
    expect(source.getGlobalRulesCurrent()).toBe(true);

    // Delayed: same session, rows retained, not current; subscribers woken.
    rulesChanged.mockClear();
    vi.advanceTimersByTime(450);
    expect(source.getShellSnapshot().transportStatus).toBe('delayed');
    expect(source.getGlobalRulesCurrent()).toBe(false);
    expect(source.getGlobalRules()).toBe(rows);
    expect(rulesChanged).toHaveBeenCalledTimes(1);

    // Stale: still not current, rows still retained.
    vi.advanceTimersByTime(500);
    expect(source.getShellSnapshot().transportStatus).toBe('stale');
    expect(source.getGlobalRulesCurrent()).toBe(false);
    expect(source.getGlobalRules()).toBe(rows);

    // Recovery in the same session: current again, subscribers woken.
    rulesChanged.mockClear();
    receiveHeartbeat(FakeWebSocket.shell, 3, '2');
    receiveHeartbeat(FakeWebSocket.shell, 4, '3');
    expect(source.getShellSnapshot().transportStatus).toBe('connected');
    expect(source.getGlobalRulesCurrent()).toBe(true);
    expect(rulesChanged).toHaveBeenCalledTimes(1);

    // Semantic unavailable on a connected transport: not current.
    rulesChanged.mockClear();
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'heartbeat_challenge',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      challenge_sequence: '5',
      received_client_sequence: '4',
      semantic_status: 'unavailable',
      semantic_reason: 'owner_unavailable',
      freshness: null,
    });
    expect(source.getShellSnapshot()).toMatchObject({
      transportStatus: 'connected', semanticStatus: 'unavailable',
    });
    expect(source.getGlobalRulesCurrent()).toBe(false);
    expect(source.getGlobalRules()).toBe(rows);
    expect(rulesChanged).toHaveBeenCalledTimes(1);

    rulesChanged.mockClear();
    receiveHeartbeat(FakeWebSocket.shell, 6, '5');
    expect(source.getGlobalRulesCurrent()).toBe(true);
    expect(rulesChanged).toHaveBeenCalledTimes(1);
    source.dispose();
  });

  it('marks global rules not current as soon as a projection error revokes control', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      static closeRequests = 0;
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      // The browser delivers the close event asynchronously.
      close(): void { FakeWebSocket.closeRequests += 1; }
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    const rulesChanged = vi.fn();
    source.subscribeGlobalRules(rulesChanged);
    source.setEnabled(true);
    const withRules = { ...wireState(1), global_rules: [{ rule_id: 'door', state: 'clear', keys: [] }] };
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, withRules);
    const rows = source.getGlobalRules();
    expect(source.getGlobalRulesCurrent()).toBe(true);

    // An invalid projection revokes control before its close arrives.
    rulesChanged.mockClear();
    const failing = FakeWebSocket.shell;
    failing.onmessage?.({ data: '{"not":"a control message"' } as MessageEvent);
    expect(FakeWebSocket.closeRequests).toBe(1);
    expect(source.getShellSnapshot().transportStatus).toBe('connected');
    expect(source.getGlobalRulesCurrent()).toBe(false);
    expect(source.getGlobalRules()).toBe(rows);
    expect(rulesChanged).toHaveBeenCalledTimes(1);

    // The close clears the session's rows.
    rulesChanged.mockClear();
    failing.readyState = 3;
    failing.onclose?.();
    expect(source.getGlobalRules()).toBeNull();
    expect(source.getGlobalRulesCurrent()).toBe(false);
    expect(rulesChanged).toHaveBeenCalledTimes(1);

    // Reconnect and recovery: a new session's snapshot is current again.
    vi.advanceTimersByTime(1000); // RECONNECT_DELAY_MS
    expect(FakeWebSocket.shell).not.toBe(failing);
    synchronizeControl(FakeWebSocket.shell, 'control-session.test-2');
    receiveControlSnapshot(FakeWebSocket.shell, withRules, 'control-session.test-2');
    expect(source.getGlobalRules()).toEqual([{ ruleId: 'door', state: 'clear', keys: [] }]);
    // The re-sent disconnect incident took a client sequence: rows stay not
    // current until the heartbeat confirms the latest echo.
    expect(source.getGlobalRulesCurrent()).toBe(false);
    receiveHeartbeat(FakeWebSocket.shell, 3, '3', 'control-session.test-2');
    expect(source.getShellSnapshot().transportStatus).toBe('connected');
    expect(source.getGlobalRulesCurrent()).toBe(true);
    source.dispose();
  });

  it('freezes the viewer frame while delayed or stale until identity changes', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    let stateSequence = 1;
    const runtimeDelta = (runtime: unknown) => receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: String(++stateSequence),
      received_client_sequence: '2',
      event_class: 'cold',
      control: {
        receiver_incarnation: 'headless.receiver.1',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1,
        freshness: controlFreshness(1),
      },
      shell: { complete: false, runtime },
    });

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    animationFrames.shift()!(16);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    const coherent = source.getViewerSnapshot().snapshot;

    vi.advanceTimersByTime(500);
    expect(source.getViewerSnapshot()).toMatchObject({
      transportStatus: 'delayed', snapshot: coherent,
    });
    // An unverified Scene update in the same identity does not move the
    // retained frame, neither on arrival nor at the stale classification.
    const sceneUpdate = wireState(1).runtime as Record<string, unknown>;
    const sections = sceneUpdate.sections as Record<string, Record<string, unknown>>;
    sections.logical_scene = { ...sections.logical_scene, source_generation: 2 };
    runtimeDelta(sceneUpdate);
    expect(animationFrames).toHaveLength(0);
    vi.advanceTimersByTime(500);
    expect(source.getViewerSnapshot()).toMatchObject({
      transportStatus: 'stale', snapshot: coherent,
    });
    expect(source.getShellSnapshot().snapshot?.runtime.sections.logicalScene.sourceGeneration)
      .toBe(2);

    // A new owner identity crosses the barrier: the frozen frame is dropped.
    const ownerChange = structuredClone(sceneUpdate);
    (ownerChange.sections as Record<string, Record<string, unknown>>).manager.source_owner_epoch =
      'source.epoch.2';
    runtimeDelta(ownerChange);
    expect(animationFrames).toHaveLength(1);
    animationFrames.shift()!(32);
    expect(source.getViewerSnapshot().snapshot?.runtime.sections.manager.sourceOwnerEpoch)
      .toBe('source.epoch.2');
    source.dispose();
  });

  it('does not publish the shell for deltas that change only delivery diagnostics', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static shell: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void {}
      send(): void {}
    }
    vi.stubGlobal('window', { location: { href: 'http://composer.test/' } });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    let stateSequence = 1;
    const runtimeDelta = (runtime: unknown) => receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'delta',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      state_sequence: String(++stateSequence),
      received_client_sequence: '2',
      event_class: 'cold',
      control: {
        receiver_incarnation: 'headless.receiver.1',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1,
        freshness: controlFreshness(1),
      },
      shell: { complete: false, runtime },
    });
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    const shellChanged = vi.fn();
    source.subscribeShell(shellChanged);
    const published = source.getShellSnapshot();

    const diagnosticRuntime = (projectionGeneration: number, ageNs: number) => {
      const runtime = wireState(1).runtime as Record<string, unknown>;
      runtime.projection_generation = projectionGeneration;
      const sections = runtime.sections as Record<string, Record<string, unknown>>;
      sections.manager = {
        ...sections.manager, source_generation: projectionGeneration, receiver_age_ns: ageNs,
      };
      sections.scene_runtime = {
        ...sections.scene_runtime, source_generation: projectionGeneration, receiver_age_ns: ageNs,
      };
      return runtime;
    };
    runtimeDelta(diagnosticRuntime(2, 5));
    runtimeDelta(diagnosticRuntime(3, 9));
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(shellChanged).not.toHaveBeenCalled();
    expect(source.getShellSnapshot()).toBe(published);

    // A visible change still publishes, carrying the newest data.
    const visible = diagnosticRuntime(4, 11);
    (visible.sections as Record<string, Record<string, unknown>>).manager.freshness = 'stale';
    runtimeDelta(visible);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(shellChanged).toHaveBeenCalledTimes(1);
    expect(source.getShellSnapshot().snapshot?.runtime.projectionGeneration).toBe(4);
    source.dispose();
  });

  it('echoes a newer semantic heartbeat while its replaceable projection catches up', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      closed = false;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void {
        this.closed = true;
        this.readyState = 3;
      }

      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getShellSnapshot().error).toBeNull();

    receiveHeartbeat(FakeWebSocket.shell, 3, '2', CONTROL_SESSION_ID, 2);

    expect(FakeWebSocket.shell.closed).toBe(false);
    expect(JSON.parse(FakeWebSocket.shell.sent.at(-1)!)).toMatchObject({
      message_kind: 'heartbeat_echo',
      challenge_sequence: '3',
    });
    expect(source.getShellSnapshot().error).toBe(
      'Control state is synchronizing; commands are disabled.',
    );
    expect(source.getShellSnapshot()).toMatchObject({
      transportStatus: 'connected',
      transportError: null,
      semanticStatus: 'synchronizing',
      semanticReason: 'control_projection_synchronizing',
    });

    receiveControlSnapshot(FakeWebSocket.shell, wireState(2));

    expect(FakeWebSocket.shell.closed).toBe(false);
    expect(source.getShellSnapshot().error).toBeNull();
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('classifies missing heartbeat independently during initial synchronization', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readyState = FakeWebSocket.OPEN;
      closed = false;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void {
        this.closed = true;
        this.readyState = 3;
        this.onclose?.();
      }

      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'server_hello',
      receiver_incarnation: 'headless.receiver.1',
      control_session_id: CONTROL_SESSION_ID,
      heartbeat_period_ns: 250_000_000,
      heartbeat_delayed_ns: 500_000_000,
      heartbeat_stale_ns: 1_000_000_000,
      maximum_message_bytes: 65_536,
      maximum_messages_per_second: 64,
    });

    vi.advanceTimersByTime(10_000);
    expect(FakeWebSocket.shell.closed).toBe(false);
    expect(source.getShellSnapshot()).toMatchObject({
      snapshot: null,
      error: 'Control connection is stale; commands are disabled.',
    });
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('reports one durable disconnect episode after the same browser reconnects', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static instances: FakeWebSocket[] = [];
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor() { FakeWebSocket.instances.push(this); }
      send(value: string): void { this.sent.push(value); }
      close(): void { this.readyState = 3; }
      fail(): void {
        this.readyState = 3;
        this.onclose?.();
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    let retainedIdentity: string | null = null;
    vi.stubGlobal('localStorage', {
      getItem: () => retainedIdentity,
      setItem: (_key: string, value: string) => { retainedIdentity = value; },
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    const first = FakeWebSocket.instances[0]!;
    synchronizeControl(first);
    receiveControlSnapshot(first, wireState(1));
    first.fail();
    expect(source.getShellSnapshot().error).toContain('disconnected');

    vi.advanceTimersByTime(1_000);
    const replacement = FakeWebSocket.instances[1]!;
    replacement.onopen?.();
    synchronizeControl(replacement);
    receiveHeartbeat(replacement, 3, '3');
    receiveControlSnapshot(replacement, wireState(1), CONTROL_SESSION_ID, '3');
    const transitions = replacement.sent
      .map((value) => JSON.parse(value) as Record<string, unknown>)
      .filter((value) => value.message_kind === 'client_incident');
    expect(transitions.map((value) => ({
      kind: value.incident_kind,
      phase: value.phase,
    }))).toEqual([
      { kind: 'control_disconnected', phase: 'activated' },
      { kind: 'control_disconnected', phase: 'resolved' },
    ]);
    expect(source.getShellSnapshot().error).toBeNull();
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('does not renew readiness from unconfirmed one-way heartbeat traffic', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void { this.readyState = 3; }
      send(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    vi.advanceTimersByTime(250);
    receiveHeartbeat(FakeWebSocket.shell, 3, '2');
    vi.advanceTimersByTime(250);
    receiveHeartbeat(FakeWebSocket.shell, 4, '2');
    vi.advanceTimersByTime(249);
    receiveHeartbeat(FakeWebSocket.shell, 5, '2');
    expect(source.getShellSnapshot().error).toBeNull();
    vi.advanceTimersByTime(1);
    expect(source.getShellSnapshot().error).toContain('delayed');
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('correlates a receipt under a newer cumulative watermark without inventing lifecycle state', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    const before = source.getShellSnapshot().snapshot?.programCycle.state;
    const receiptPromise = source.sendExecutionCommand(
      'program_reset',
      {},
      'correlation.test-1',
    );
    const command = JSON.parse(FakeWebSocket.shell.sent.at(-1)!) as {
      client_sequence: string;
    };
    for (const draftRevision of [2, 3]) {
      receiveControl(FakeWebSocket.shell, {
        schema_version: 'manyforge.execution_control.v2',
        message_kind: 'delta',
        control_session_id: CONTROL_SESSION_ID,
        server_sequence: nextControlServerSequence(FakeWebSocket.shell),
        event_class: 'cold',
        control: null,
        shell: {
          schema_version: 'manyforge.composer_headless_state.v2',
          complete: false,
          authoring_workspace_containment: null,
          receiver_epoch: 'receiver.epoch.1',
          snapshot_sequence: draftRevision + 1,
          authoring_workspace: {
            workspace_epoch: 'workspace.epoch.1',
            workspace_revision: draftRevision,
            program_draft_id: 'draft-1',
            program_draft_revision: draftRevision,
            deployment_draft_id: 'deployment-draft-1',
            deployment_draft_revision: 1,
          },
        },
      });
    }
    expect(
      source.getShellSnapshot().snapshot?.authoringWorkspace.programDraftRevision,
    ).toBe(1);
    receiveControl(FakeWebSocket.shell, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'command_receipt',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(FakeWebSocket.shell),
      received_client_sequence: String(BigInt(command.client_sequence) + 2n),
      emitted_monotonic_ns: '200',
      receipt: {
        schema_version: 'manyforge.execution_command_receipt.v1',
        correlation_id: 'correlation.test-1',
        control_session_id: CONTROL_SESSION_ID,
        client_sequence: command.client_sequence,
        receiver_incarnation: 'headless.receiver.1',
        received_server_sequence: '10',
        received_monotonic_ns: '100',
        disposition: 'received',
        code: 'program_reset_received',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 1,
        current_identity: { cycle_generation: 1 },
      },
      result: { accepted: true },
    });

    await expect(receiptPromise).resolves.toMatchObject({
      correlationId: 'correlation.test-1',
      disposition: 'received',
      receivedServerSequence: '10',
      receivedMonotonicNs: '100',
      emittedMonotonicNs: '200',
    });
    await Promise.resolve();
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getShellSnapshot().snapshot?.programCycle.state).toBe(before);
    expect(
      source.getShellSnapshot().snapshot?.authoringWorkspace.programDraftRevision,
    ).toBe(3);
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('fails closed once for unconfirmed delivery until a fresh session snapshot', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static instances: FakeWebSocket[] = [];
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor() { FakeWebSocket.instances.push(this); }

      close(): void {
        if (this.readyState !== FakeWebSocket.OPEN) return;
        this.readyState = 3;
        this.onclose?.();
      }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    const first = FakeWebSocket.instances[0]!;
    synchronizeControl(first);
    receiveControlSnapshot(first, wireState(1));
    (source as unknown as { clearControlHeartbeatWatch: () => void })
      .clearControlHeartbeatWatch();
    const pending = source.sendExecutionCommand(
      'program_reset', {}, 'correlation.unconfirmed-1',
    );
    const rejection = expect(pending).rejects.toThrow(
      'Command delivery is unconfirmed',
    );

    vi.advanceTimersByTime(1_000);
    await rejection;
    expect(first.readyState).toBe(3);
    const activated = first.sent
      .map((value) => JSON.parse(value) as Record<string, unknown>)
      .find((value) => value.message_kind === 'client_incident');
    expect(activated).toMatchObject({
      incident_kind: 'command_delivery_unconfirmed',
      phase: 'activated',
    });

    vi.advanceTimersByTime(1_000);
    const replacement = FakeWebSocket.instances[1]!;
    replacement.onopen?.();
    synchronizeControl(replacement, 'control-session.test-2');
    expect(source.getShellSnapshot().error).not.toBeNull();
    receiveControlSnapshot(replacement, wireState(1), 'control-session.test-2');
    expect(replacement.sent.some((value) => (
      JSON.parse(value).incident_kind === 'command_delivery_unconfirmed'
      && JSON.parse(value).phase === 'resolved'
    ))).toBe(false);
    receiveHeartbeat(replacement, 3, '4', 'control-session.test-2');
    expect(source.getShellSnapshot().error).toBeNull();
    const transitions = replacement.sent
      .map((value) => JSON.parse(value) as Record<string, unknown>)
      .filter((value) => value.message_kind === 'client_incident');
    expect(transitions).toEqual(expect.arrayContaining([
      expect.objectContaining({
        incident_kind: 'command_delivery_unconfirmed',
        phase: 'resolved',
        reason_code: 'client_incident_recovered',
      }),
    ]));
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('keeps a watermarked command pending while applying backend lifecycle', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }
      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    const socket = FakeWebSocket.shell;
    synchronizeControl(socket);
    receiveControlSnapshot(socket, wireState(1));
    const pending = source.sendExecutionCommand(
      'program_reset', {}, 'correlation.receipt-missing-1',
    );
    const command = JSON.parse(socket.sent.at(-1)!) as { client_sequence: string };
    receiveHeartbeat(socket, 3, command.client_sequence);
    (source as unknown as { clearControlHeartbeatWatch: () => void })
      .clearControlHeartbeatWatch();
    let settled = false;
    void pending.finally(() => { settled = true; });

    vi.advanceTimersByTime(1_000);
    await Promise.resolve();
    expect(settled).toBe(false);
    expect(socket.readyState).toBe(FakeWebSocket.OPEN);
    expect(source.getShellSnapshot().error).toBeNull();

    const resetting = wireState(2);
    (resetting.program_cycle as Record<string, unknown>).state = 'resetting';
    Object.assign(resetting.program_cycle as Record<string, unknown>, {
      current_command_kind: 'program_reset', reset_phase: 'restoring_scene',
      reset_options: { reload_initial_scene: true, reset_robot_position: false },
    });
    receiveControlSnapshot(socket, resetting, CONTROL_SESSION_ID, command.client_sequence);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getShellSnapshot().snapshot?.programCycle.state).toBe('resetting');
    expect(settled).toBe(false);

    receiveControl(socket, {
      schema_version: 'manyforge.execution_control.v2',
      message_kind: 'command_receipt',
      control_session_id: CONTROL_SESSION_ID,
      server_sequence: nextControlServerSequence(socket),
      received_client_sequence: command.client_sequence,
      emitted_monotonic_ns: '200',
      receipt: {
        schema_version: 'manyforge.execution_command_receipt.v1',
        correlation_id: 'correlation.receipt-missing-1',
        control_session_id: CONTROL_SESSION_ID,
        client_sequence: command.client_sequence,
        receiver_incarnation: 'headless.receiver.1',
        received_server_sequence: '10',
        received_monotonic_ns: '100',
        disposition: 'received',
        code: 'program_reset_received',
        semantic_owner_epoch: 'runner.epoch.1',
        execution_control_revision: 2,
        current_identity: { cycle_generation: 1 },
      },
      result: null,
    });
    await expect(pending).resolves.toMatchObject({
      correlationId: 'correlation.receipt-missing-1',
      disposition: 'received',
    });
    source.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('does not couple a coalesced velocity update to one discrete command', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static readonly OPEN = 1;
      static shell: FakeWebSocket;
      readonly sent: string[] = [];
      readyState = FakeWebSocket.OPEN;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        if (new URL(url).pathname === '/ws/control') FakeWebSocket.shell = this;
      }

      close(): void { this.readyState = 3; }
      send(value: string): void { this.sent.push(value); }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    synchronizeControl(FakeWebSocket.shell);
    receiveControlSnapshot(FakeWebSocket.shell, wireState(1));
    const internal = source as unknown as {
      latest: { executionControlStatus: { ownerEpoch: string } | null } | null;
    };
    expect(internal.latest).not.toBeNull();
    internal.latest!.executionControlStatus = { ownerEpoch: 'execution.epoch.1' };

    const scale = source.sendExecutionCommand(
      'runtime_scale', { scale: 0.5 }, 'correlation.scale-1',
    );
    const reset = source.sendExecutionCommand(
      'program_reset', {}, 'correlation.reset-1',
    );
    const commands = FakeWebSocket.shell.sent
      .map((value) => JSON.parse(value) as Record<string, unknown>)
      .filter((value) => value.message_kind === 'command') as Array<{
        command: { command_envelope: { command_kind: string } };
      }>;
    expect(commands.map((value) => value.command.command_envelope.command_kind))
      .toEqual(['runtime_scale', 'program_reset']);
    await expect(source.sendExecutionCommand(
      'runtime_scale', { scale: 0.6 }, 'correlation.scale-2',
    )).rejects.toThrow('runtime velocity update is pending');
    await expect(source.sendExecutionCommand(
      'program_reset', {}, 'correlation.reset-2',
    )).rejects.toThrow('ordinary control command is pending');

    const scaleRejected = expect(scale).rejects.toThrow(
      'Command delivery unconfirmed',
    );
    const resetRejected = expect(reset).rejects.toThrow(
      'Command delivery unconfirmed',
    );
    source.dispose();
    await scaleRejected;
    await resetRejected;
    expect(vi.getTimerCount()).toBe(0);
  });
});

describe('useHeadlessStateSource lifecycle', () => {
  it('keeps one live socket per plane through StrictMode and none after unmount', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      active = true;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {
        this.active = false;
        this.onclose?.();
      }

      open(): void {
        this.onopen?.();
      }

      send(): void {}

      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }

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
      visibilityState: 'visible';
      defaultView: Record<string, unknown>;
      addEventListener: () => void;
      removeEventListener: () => void;
      createElement: (tagName: string) => FakeNode;
    };
    const document = {
      nodeType: 9,
      documentElement: null,
      activeElement: null,
      visibilityState: 'visible',
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
    vi.stubGlobal('WebSocket', FakeWebSocket);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);

    const { createRoot } = await import('react-dom/client');
    const root = createRoot(makeNode('div') as unknown as Element);
    function Harness() {
      const source = useHeadlessStateSource(true);
      useViewerHeadlessState(source, true);
      return null;
    }

    await act(async () => {
      root.render(createElement(StrictMode, null, createElement(Harness)));
    });

    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(FakeWebSocket.instances.filter((socket) => socket.active)).toHaveLength(2);
    expect(new Set(FakeWebSocket.instances.filter((socket) => socket.active).map(
      (socket) => new URL(socket.url).pathname,
    ))).toEqual(new Set(['/ws/control', '/ws/scene/viewer']));
    const activeShell = FakeWebSocket.instances.find(
      (socket) => socket.active && new URL(socket.url).pathname === '/ws/control',
    );
    activeShell?.open();
    if (activeShell !== undefined) {
      synchronizeControl(activeShell);
      receiveControlSnapshot(activeShell, wireState(1));
    }
    // Shell publication, viewer credit renewal, and one control-heartbeat
    // watchdog are the only live timers.
    expect(vi.getTimerCount()).toBe(3);

    await act(async () => root.unmount());
    expect(FakeWebSocket.instances.filter((socket) => socket.active)).toHaveLength(0);
    expect(vi.getTimerCount()).toBe(0);

    vi.advanceTimersByTime(10_000);
    expect(FakeWebSocket.instances.filter((socket) => socket.active)).toHaveLength(0);
  });
});
