import { describe, expect, it } from 'vitest';
import {
  buildCartesianMotionIntent,
  buildGripperCommandIntent,
  buildJointMotionIntent,
  buildPoseMotionIntent,
  buildPreviewIntent,
  initialRobotControlState,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  robotControlReducer,
} from './robotControl';

import { statusFixture, receiptFixture, readyState, executingState, gripperReceiptFixture } from '../testFixtures/robotControl';

describe('addressed Robot Control state', () => {
  it('invalidates a Move To preview when its semantic source kind changes at the same pose', () => {
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status: statusFixture() });
    state = robotControlReducer(state, { type: 'select_mode', mode: 'move_to' });
    const joint = robotControlReducer(state, { type: 'edit_move_to_joint', positionsRad: [0.25, 0.35] });
    const pose = robotControlReducer(joint, { type: 'edit_pose', pose: joint.draftPose });
    expect(pose.moveToKind).toBe('pose');
    expect(pose.generation).toBe(joint.generation + 1);
    expect(buildPreviewIntent(pose, 'pose-new').participants[0].payload).toMatchObject({
      kind: 'motion', motion: { kind: 'pose_goal' },
    });
    const back = robotControlReducer(pose, { type: 'edit_move_to_joint', positionsRad: joint.draftPositionsRad });
    expect(back.moveToKind).toBe('joint');
    expect(back.generation).toBe(pose.generation + 1);
  });
  it('accumulates individual joint edits without using a stale rendered vector', () => {
    const loaded = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status: statusFixture() });
    const first = robotControlReducer(loaded, { type: 'edit_joint', jointIndex: 0, positionRad: 0.25 });
    const second = robotControlReducer(first, { type: 'edit_joint', jointIndex: 1, positionRad: 0.35 });
    expect(second.draftPositionsRad).toEqual([0.25, 0.35]);
    expect(second.generation).toBe(first.generation + 1);
    for (const jointIndex of [-1, 2, 0.5, NaN]) {
      expect(robotControlReducer(second, { type: 'edit_joint', jointIndex, positionRad: 0 })).toBe(second);
    }
  });

  it.each([false, true])('seeds only the first edit from current observation, retained result=%s', (retained) => {
    let status = statusFixture();
    if (retained) status.manualOperations = [{
      programRunId: status.directControl.programRunId, operationId: 'completed-1',
      operationRevision: 2, resident: false, state: 'completed', progress: 1,
      activeStopGeneration: null, activeStopId: null, terminalError: null,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const oldDraft = state.draftPositionsRad;
    status = { ...status, observedMotion: { ...status.observedMotion, positionsRad: [0.5, 0.5] } };
    state = robotControlReducer(state, { type: 'status_refreshed', status });
    expect(state.draftPositionsRad).toBe(oldDraft); // No observation-driven draft copy.
    state = robotControlReducer(state, { type: 'edit_joint', jointIndex: 0, positionRad: 0.6 });
    expect(state.draftPositionsRad).toEqual([0.6, 0.5]);
    state = robotControlReducer(state, { type: 'status_refreshed', status: {
      ...status, observedMotion: { ...status.observedMotion, positionsRad: [0.2, 0.3] },
    } });
    state = robotControlReducer(state, { type: 'edit_joint', jointIndex: 1, positionRad: 0.7 });
    expect(state.draftPositionsRad).toEqual([0.6, 0.7]);
  });

  it('retries only the local rejected preview and fences the old reply', () => {
    const ready = readyState();
    const failed = robotControlReducer(ready, { type: 'preview_rejected', generation: ready.generation, error: 'planner unavailable' });
    const retry = robotControlReducer(failed, { type: 'retry_preview' });
    expect(retry.phase).toBe('planning');
    expect(retry.draftPositionsRad).toEqual(failed.draftPositionsRad);
    expect(retry.generation).toBe(failed.generation + 1);
    expect(robotControlReducer(retry, { type: 'preview_rejected', generation: failed.generation, error: 'old failure' })).toBe(retry);
    const active = executingState();
    expect(robotControlReducer(active, { type: 'retry_preview' })).toBe(active);
  });

  it('does not apply an old approval failure to a newer draft or owner', () => {
    const ready = readyState();
    const approving = robotControlReducer(ready, { type: 'approve_started' });
    expect(robotControlReducer(approving, {
      type: 'approve_rejected', generation: ready.generation - 1, error: 'old response',
    })).toBe(approving);
    expect(robotControlReducer(approving, {
      type: 'approve_rejected', generation: ready.generation, error: 'current response',
    }).error).toBe('current response');
  });

  it('preserves the historical bounded authoring cadence', () => {
    expect(ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS).toBe(150);
  });

  it('builds an ordinary operation from exact source-local owner facts', () => {
    const status = statusFixture();
    const intent = buildJointMotionIntent(status, [0.2, -0.4], 'request-9');
    const participant = intent.participants[0];
    if (participant.payload.kind !== 'motion') {
      throw new Error('expected motion payload');
    }

    expect(intent.program_id).toBe('manyforge.direct_control.motion');
    expect(intent.program_binding_id).toBe('sha256:direct-binding');
    expect(intent.request_context.request_owner_epoch).toBe('execution-owner-1');
    expect(participant.component_role_id).toBe('controlled_component');
    expect(participant.target_role_id).toBe('command_target');
    expect(participant.payload.base_frame_role_id).toBe('base');
    expect(participant.payload.tcp_frame_role_id).toBe('tcp');
    expect(participant.payload.motion).toEqual({
      kind: 'joint_goal',
      joint_names: ['joint1', 'joint2'],
      positions_rad: [0.2, -0.4],
    });
    expect(participant.payload.per_joint_dynamics_limits).toEqual({
      joint1: {
        max_velocity_rad_s: 1.5,
        max_acceleration_rad_s2: 2.5,
        max_jerk_rad_s3: 10,
      },
    });
  });

  it('keeps a stored joint source inside Move To and previews it as a joint goal', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, { type: 'select_mode', mode: 'move_to' });
    state = robotControlReducer(state, {
      type: 'edit_move_to_joint',
      positionsRad: [0.2, -0.4],
    });

    const intent = buildPreviewIntent(state, 'move-to-joints');
    expect(state.mode).toBe('move_to');
    expect(state.moveToKind).toBe('joint');
    expect(intent.participants[0].payload).toMatchObject({
      kind: 'motion',
      motion: {
        kind: 'joint_goal',
        positions_rad: [0.2, -0.4],
      },
    });
  });

  it('makes a prior ready preview stale and non-approvable immediately', () => {
    const ready = readyState();
    const edited = robotControlReducer(ready, {
      type: 'edit',
      positionsRad: [0.7, 0.5],
    });

    expect(ready.phase).toBe('ready');
    expect(edited.phase).toBe('planning');
    expect(edited.preview).toBeNull();
    expect(edited.intent).toBeNull();
    expect(edited.ghost).toBe(ready.ghost);
    expect(edited.ghostStale).toBe(true);
  });

  it('ignores a preview response superseded by a newer edit', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.2, 0.1],
    });
    const oldGeneration = state.generation;
    const oldIntent = buildJointMotionIntent(status, [0.2, 0.1], 'old');
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: oldGeneration,
      intent: oldIntent,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.8, 0.1],
    });
    const stale = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: oldGeneration,
      receipt: receiptFixture(status, 'old', [0.2, 0.1]),
    });

    expect(stale).toBe(state);
    expect(stale.phase).toBe('planning');
    expect(stale.preview).toBeNull();
  });

  it('shows the owner-validated planned final state without requiring it to equal the goal', () => {
    const status = statusFixture();
    const positions = [0.25, 0.5];
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, { type: 'edit', positionsRad: positions });
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent: buildJointMotionIntent(status, positions, 'request-final'),
    });
    state = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: receiptFixture(
        status,
        'request-final',
        [0.249, 0.501],
      ),
    });

    expect(state.phase).toBe('ready');
    expect(state.ghost?.finalPositionsRad).toEqual([0.249, 0.501]);
  });

  it('fails closed on an out-of-limit draft', () => {
    const status = statusFixture();
    const loaded = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    const blocked = robotControlReducer(loaded, {
      type: 'edit',
      positionsRad: [2, 0.1],
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.preview).toBeNull();
    expect(blocked.error).toContain('outside');
  });

  it('waits for the owner projection before tracking the exact operation', () => {
    const ready = readyState();
    const approving = robotControlReducer(ready, { type: 'approve_started' });
    const executing = executingState();

    expect(approving.phase).toBe('approving');
    expect(executing.phase).toBe('executing');
    expect(executing.operation).toMatchObject({
      operationId: 'operation-1',
      operationRevision: 1,
      state: 'queued',
    });
    expect(executing.preview).toBeNull();
  });

  it('does not let Edit or Revert pretend to cancel execution', () => {
    const executing = executingState();

    expect(robotControlReducer(executing, {
      type: 'edit',
      positionsRad: [0.8, 0.5],
    })).toBe(executing);
    expect(robotControlReducer(executing, { type: 'revert' })).toBe(executing);
  });

  it('changes lifecycle only when a newer owner operation is projected', () => {
    const executing = executingState();
    const stillExecuting = robotControlReducer(executing, {
      type: 'operation_observed',
      operation: {
        ...executing.operation!,
        operationRevision: 2,
        state: 'executing',
        progress: 0.2,
      },
    });
    expect(stillExecuting.phase).toBe('executing');

    const stopping = robotControlReducer(stillExecuting, {
      type: 'operation_observed',
      operation: {
        ...stillExecuting.operation!,
        operationRevision: 3,
        state: 'stopping',
        progress: 0.3,
      },
    });
    expect(stopping.phase).toBe('stopping');

    const failed = robotControlReducer(stopping, {
      type: 'operation_control_failed',
      ownerEpoch: stopping.status!.ownerEpoch,
      operationId: stopping.operation!.operationId,
      error: 'Stop request was rejected.',
    });
    expect(failed.phase).toBe('stopping');
    expect(failed.error).toBe('Stop request was rejected.');
  });

  it('keeps a resident terminal operation until exact owner settlement', () => {
    let state = executingState();
    state = robotControlReducer(state, {
      type: 'operation_observed',
      operation: {
        operationId: 'operation-1',
        operationRevision: 8,
        resident: true,
        state: 'completed',
        progress: 1,
        activeStopId: null,
        activeStopGeneration: null,
        terminalError: null,
      },
    });
    expect(state.phase).toBe('completed');
    expect(state.ghostStale).toBe(true);

    const observed = statusFixture();
    observed.observedMotion.positionsRad = [0.24, 0.49];
    observed.observedGripper = {
      ...observed.observedGripper!,
      position: 0.031,
    };
    observed.manualOperations = [{ ...state.operation!, programRunId: observed.directControl.programRunId }];
    state = robotControlReducer(state, {
      type: 'status_refreshed',
      status: observed,
    });
    expect(state.draftPositionsRad).toEqual([0.24, 0.49]);
    expect(state.draftGripperPosition).toBe(0.031);

    const settledStatus = statusFixture();
    settledStatus.observedMotion.positionsRad = [0.24, 0.49];
    state = robotControlReducer(state, {
      type: 'status_refreshed',
      status: settledStatus,
    });
    expect(state.phase).toBe('idle');
    expect(state.operation).toBeNull();
    expect(state.ghost).toBeNull();
  });

  it('recovers the exact resident operation on a fresh client load', () => {
    const status = statusFixture();
    status.manualOperations = [{
      programRunId: status.directControl.programRunId,
      operationId: 'operation-recovered',
      operationRevision: 4,
      resident: true,
      state: 'executing',
      progress: 0.5,
      activeStopId: null,
      activeStopGeneration: null,
      terminalError: null,
    }];
    const recovered = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    expect(recovered.phase).toBe('executing');
    expect(recovered.operation).toEqual(status.manualOperations[0]);
  });

  it('shows a short move that completed before the first active projection, without overwriting the next draft', () => {
    let state = robotControlReducer(readyState(), { type: 'approve_started' });
    const status = statusFixture();
    status.manualOperations = [{
      ...executingState().operation!, programRunId: status.directControl.programRunId,
      operationId: 'short-move', state: 'completed', progress: 1, resident: false,
    }];
    state = robotControlReducer(state, { type: 'status_refreshed', status });
    expect(state.phase).toBe('completed');
    expect(state.operation?.resident).toBe(false);
    state = robotControlReducer(state, { type: 'edit', positionsRad: [0.1, 0.2] });
    const generation = state.generation;
    state = robotControlReducer(state, { type: 'status_refreshed', status: structuredClone(status) });
    expect(state.phase).toBe('planning');
    expect(state.generation).toBe(generation);
    expect(state.draftPositionsRad).toEqual([0.1, 0.2]);
  });

  it.each([true, false])('follows a subsequent owner move even when the predecessor terminal frame was skipped (resident=%s)', (resident) => {
    const state = executingState();
    const status = statusFixture();
    status.manualOperations = [{
      ...state.operation!, programRunId: status.directControl.programRunId,
      operationId: 'next-move-from-another-client', resident,
      state: resident ? 'executing' : 'completed',
    }];
    const next = robotControlReducer(state, { type: 'status_refreshed', status });
    expect(next.operation?.operationId).toBe('next-move-from-another-client');
    expect(next.phase).toBe(resident ? 'executing' : 'completed');
    expect(next.error).toBeNull();
  });

  it('selects the retained gripper result independently from the arm and keeps mode coherent after replacement', () => {
    const status = statusFixture();
    status.manualOperations = [{
      ...executingState().operation!, programRunId: status.directControlGripper.programRunId,
      state: 'completed', resident: false,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    expect(state.operation).toBeNull();
    state = robotControlReducer(state, { type: 'select_mode', mode: 'gripper' });
    expect(state.operation?.state).toBe('completed');
    const changed = structuredClone(status);
    changed.ownerEpoch = 'next-owner';
    state = robotControlReducer(state, { type: 'status_refreshed', status: changed });
    expect(state.mode).toBe('gripper');
    expect(state.operation?.state).toBe('completed');
  });

  it('keeps resident fault evidence on local Revert and does not authorize a new edit', () => {
    let state = executingState();
    state = robotControlReducer(state, { type: 'operation_observed', operation: {
      ...state.operation!, state: 'intervention_required', terminalError: 'physical rest unproven',
    } });
    const reverted = robotControlReducer(state, { type: 'revert' });
    expect(reverted.operation).toBe(state.operation);
    expect(reverted.phase).toBe('intervention_required');
    expect(reverted.error).toBe('physical rest unproven');
    expect(reverted.ghost).toBeNull();
    expect(robotControlReducer(reverted, { type: 'edit', positionsRad: [0.2, 0.3] })).toBe(reverted);
  });

  it('fences old previews on an exact scene change but not on a fresh observation', () => {
    const ready = readyState();
    const fresh = structuredClone(ready.status!);
    fresh.observedMotion.positionsRad = [0.01, 0.02];
    const stillReady = robotControlReducer(ready, { type: 'status_refreshed', status: fresh });
    expect(stillReady.phase).toBe('ready');
    expect(stillReady.generation).toBe(ready.generation);
    const replacement = structuredClone(fresh);
    replacement.sceneRevision.revision_id += 1;
    const changed = robotControlReducer(stillReady, { type: 'status_refreshed', status: replacement });
    expect(changed.preview).toBeNull();
    expect(changed.generation).toBeGreaterThan(ready.generation);
    const late = robotControlReducer(changed, {
      type: 'preview_resolved', generation: ready.generation, receipt: ready.preview!,
    });
    expect(late).toBe(changed);
  });

  it('fails closed instead of choosing among multiple resident operations', () => {
    const status = statusFixture();
    status.manualOperations = ['one', 'two'].map((operationId) => ({
      programRunId: status.directControl.programRunId,
      operationId,
      operationRevision: 1,
      resident: true,
      state: 'completed' as const,
      progress: 1,
      activeStopId: null,
      activeStopGeneration: null,
      terminalError: null,
    }));
    const blocked = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    expect(blocked.phase).toBe('blocked');
    expect(blocked.operation).toBeNull();
    expect(blocked.error).toContain('More than one');
  });

  it('invalidates all local preview and operation state on owner restart', () => {
    const ready = readyState();
    const restarted = robotControlReducer(ready, {
      type: 'status_refreshed',
      status: statusFixture('execution-owner-2'),
    });

    expect(restarted.phase).toBe('idle');
    expect(restarted.preview).toBeNull();
    expect(restarted.ghost).toBeNull();
    expect(restarted.error).toContain('restarted');
  });

  it('discards a local preview at a Program lifecycle boundary', () => {
    const ready = readyState();
    const rebased = robotControlReducer(ready, {
      type: 'program_activity_changed',
    });

    expect(rebased.phase).toBe('idle');
    expect(rebased.draftPositionsRad).toEqual(
      ready.status?.observedMotion.positionsRad,
    );
    expect(rebased.intent).toBeNull();
    expect(rebased.preview).toBeNull();
    expect(rebased.ghost).toBeNull();
    expect(rebased.generation).toBe(ready.generation + 1);
  });

  it('does not adopt Program-owned resident operations as Robot Control work', () => {
    const ready = readyState();
    const status = statusFixture();
    status.manualOperations = ['program-motion-1', 'program-motion-2'].map(
      (operationId) => ({
        programRunId: 'program-run.1',
        operationId,
        operationRevision: 1,
        resident: true,
        state: 'completed' as const,
        progress: 1,
        activeStopId: null,
        activeStopGeneration: null,
        terminalError: null,
      }),
    );

    const observed = robotControlReducer(ready, {
      type: 'program_status_refreshed',
      status,
    });

    expect(observed.status).toBe(status);
    expect(observed.operation).toBeNull();
    expect(observed.error).toBeNull();
  });

  it('preserves absolute pose and explicit current-to-target Cartesian meaning', () => {
    const status = statusFixture();
    const target = {
      position_m: { x_m: 0.4, y_m: -0.1, z_m: 0.2 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    };
    const pose = buildPoseMotionIntent(status, target, 'pose-request');
    const cartesian = buildCartesianMotionIntent(
      status,
      target,
      'cartesian-request',
    );
    const posePayload = pose.participants[0].payload;
    const cartesianPayload = cartesian.participants[0].payload;
    if (
      posePayload.kind !== 'motion'
      || cartesianPayload.kind !== 'motion'
    ) throw new Error('expected motion payloads');

    expect(posePayload.motion).toEqual({ kind: 'pose_goal', goal: target });
    expect(cartesianPayload.motion).toMatchObject({
      kind: 'cartesian_path',
      waypoints: [status.observedMotion.currentTcpPose, target],
      fallback_policy: 'fail_closed',
    });
  });

  it('authors gripper commands with the exact schema and source-local unit', () => {
    const status = statusFixture();
    const intent = buildGripperCommandIntent(status, 0.03, 'gripper-request');
    const payload = intent.participants[0].payload;
    if (payload.kind !== 'capability_command') {
      throw new Error('expected capability command');
    }

    expect(intent.program_id).toBe('manyforge.direct_control.gripper');
    expect(payload.command_schema_id).toBe('gripper.command_position');
    expect(payload.command_value.position).toEqual({
      kind: 'linear_m',
      value_m: 0.03,
    });
  });

  it('discards an unsubmitted preview without planning on a mode change', () => {
    const ready = readyState();
    const changed = robotControlReducer(ready, {
      type: 'select_mode',
      mode: 'pose_goal',
    });

    expect(changed.phase).toBe('idle');
    expect(changed.intent).toBeNull();
    expect(changed.preview).toBeNull();
    expect(changed.ghost).toBeNull();
    expect(changed.ghostStale).toBe(false);
    expect(changed.draftPositionsRad).toEqual(
      changed.status?.observedMotion.positionsRad,
    );
  });

  it('accepts only the latest exact gripper preview and fabricates no ghost', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.03,
    });
    const intent = buildGripperCommandIntent(status, 0.03, 'gripper-new');
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent,
    });
    const staleSceneReceipt = gripperReceiptFixture(
      status,
      'gripper-new',
      0.03,
    );
    staleSceneReceipt.sceneRevision.revision_id += 1;
    const rejected = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: staleSceneReceipt,
    });
    expect(rejected.phase).toBe('blocked');
    expect(rejected.preview).toBeNull();
    state = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: gripperReceiptFixture(status, 'gripper-new', 0.03),
    });

    expect(state.phase).toBe('ready');
    expect(state.preview?.requestId).toBe('gripper-new');
    expect(state.ghost).toBeNull();
  });

  it('keeps approval fail-closed after the current preview is rejected', () => {
    const ready = readyState();
    const planning = robotControlReducer(ready, {
      type: 'edit',
      positionsRad: [0.6, 0.5],
    });
    const blocked = robotControlReducer(planning, {
      type: 'preview_rejected',
      generation: planning.generation,
      error: 'state changed before validation',
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.intent).toBeNull();
    expect(blocked.preview).toBeNull();
    expect(robotControlReducer(blocked, { type: 'approve_started' }))
      .toBe(blocked);
  });

  it('rejects a late receipt that targets an older request identity', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.3, 0.1],
    });
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent: buildJointMotionIntent(status, [0.3, 0.1], 'new-request'),
    });
    const blocked = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: receiptFixture(status, 'old-request', [0.3, 0.1]),
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.preview).toBeNull();
  });

  it('Revert clears a settled local command without issuing cancellation state', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.03,
    });
    const reverted = robotControlReducer(state, { type: 'revert' });

    expect(reverted.phase).toBe('idle');
    expect(reverted.operation).toBeNull();
    expect(reverted.preview).toBeNull();
    expect(reverted.draftGripperPosition).toBe(
      status.observedGripper?.position,
    );
  });

  it('authors the first component-local command without inventing state or limits', () => {
    const status = statusFixture();
    status.directControlGripper = {
      ...status.directControlGripper,
      sourceCoordinateName: 'gripper_position',
      sourceKind: 'component_local',
      minPosition: null,
      maxPosition: null,
    };
    status.observedGripper = null;

    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    expect(state.draftGripperPosition).toBeNull();
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.01,
    });
    expect(state.phase).toBe('planning');

    const intent = buildGripperCommandIntent(status, 0.01, 'local-command-1');
    const payload = intent.participants[0].payload;
    expect(payload.kind).toBe('capability_command');
    if (payload.kind !== 'capability_command') return;
    expect(payload.caller_options[0].value_s).toBe(0.5);
  });
});
