import { describe, expect, it } from 'vitest';
import {
  buildCartesianMotionIntent,
  buildGripperCommandIntent,
  buildJointMotionIntent,
  buildPoseMotionIntent,
  buildPreviewIntent,
  initialRobotControlState,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  robotControlCandidatePresentationAllowed,
  robotControlReducer,
  selectedStatusForState,
  type ManualMotionSettings,
  type RobotControlState,
} from './robotControl';

import { statusFixture, receiptFixture, readyState, executingState, gripperReceiptFixture } from '../testFixtures/robotControl';
import { poseTupleFromValue, samePose } from '../utils/poseTransforms';
import { computeCartesianMoveDraft } from '../utils/moveTo';

describe('addressed Robot Control state', () => {
  it('keeps one stable addressed selection and fences the previous robot draft', () => {
    const status = statusFixture();
    const primaryMotion = status.directControl;
    const primaryObservation = status.observedMotion;
    if (primaryMotion === null || primaryObservation === null) {
      throw new Error('motion fixture must contain a motion target and observation');
    }
    const secondMotion = structuredClone(primaryMotion);
    secondMotion.controllableComponentId = 'robot-b';
    secondMotion.programBindingId = 'sha256:robot-b-motion-binding';
    secondMotion.targetDescriptor.controllableComponentId = 'robot-b';
    const secondGripper = structuredClone(status.directControlGripper!);
    secondGripper.controllableComponentId = 'robot-b';
    secondGripper.programBindingId = 'sha256:robot-b-gripper-binding';
    secondGripper.targetDescriptor.controllableComponentId = 'robot-b';
    const secondObservedMotion = {
      ...structuredClone(primaryObservation),
      controllableComponentId: 'robot-b',
      positionsRad: [0.4, 0.5],
      sequence: 8,
    };
    status.directControlTargets = [primaryMotion, secondMotion];
    status.directControlGripperTargets = [status.directControlGripper!, secondGripper];
    status.observedMotions = [primaryObservation, secondObservedMotion];
    status.observedGrippers = [{
      ...status.observedGripper!, targetRoleId: status.directControlGripper!.targetRoleId,
      controllableComponentId: 'robot-a', commandTargetId: 'gripper',
    }, {
      ...status.observedGripper!, targetRoleId: secondGripper.targetRoleId,
      controllableComponentId: 'robot-b', commandTargetId: 'gripper', sequence: 5,
    }];

    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved', status,
    });
    expect(state).toMatchObject({
      selectedComponentId: 'robot-a',
      selectedMotionTargetId: 'arm',
      selectedGripperTargetId: 'gripper',
    });
    state = robotControlReducer(state, { type: 'edit_joint', jointIndex: 0, positionRad: 0.2 });
    const oldGeneration = state.generation;
    const switched = robotControlReducer(state, {
      type: 'select_robot', componentId: 'robot-b', motionTargetId: 'arm',
    });
    expect(switched).toMatchObject({
      selectedComponentId: 'robot-b',
      selectedMotionTargetId: 'arm',
      selectedGripperTargetId: 'gripper',
      phase: 'idle',
      draftPositionsRad: [0.4, 0.5],
      preview: null,
      ghost: null,
    });
    expect(switched.generation).toBeGreaterThan(oldGeneration);
    const refreshed = robotControlReducer(switched, {
      type: 'status_refreshed', status: structuredClone(status),
    });
    expect(refreshed.selectedComponentId).toBe('robot-b');
    expect(refreshed.generation).toBe(switched.generation);
    expect(robotControlReducer(refreshed, {
      type: 'preview_resolved', generation: oldGeneration,
      receipt: receiptFixture(status, 'obsolete-robot-a', [0.2, 0.1]),
    })).toBe(refreshed);

    let planning = robotControlReducer(refreshed, {
      type: 'edit_joint', jointIndex: 0, positionRad: 0.6,
    });
    let selected = selectedStatusForState(planning)!;
    planning = robotControlReducer(planning, {
      type: 'preview_started', generation: planning.generation,
      intent: buildJointMotionIntent(selected, [0.6, 0.5], 'robot-b-motion'),
    });
    let accepted = robotControlReducer(planning, {
      type: 'preview_resolved', generation: planning.generation,
      receipt: receiptFixture(selected, 'robot-b-motion', [0.6, 0.5]),
    });
    expect(accepted.phase).toBe('ready');

    accepted = robotControlReducer(accepted, { type: 'select_mode', mode: 'gripper' });
    planning = robotControlReducer(accepted, { type: 'edit_gripper', position: 0.03 });
    selected = selectedStatusForState(planning)!;
    planning = robotControlReducer(planning, {
      type: 'preview_started', generation: planning.generation,
      intent: buildGripperCommandIntent(selected, 0.03, 'robot-b-gripper'),
    });
    accepted = robotControlReducer(planning, {
      type: 'preview_resolved', generation: planning.generation,
      receipt: gripperReceiptFixture(selected, 'robot-b-gripper', 0.03),
    });
    expect(accepted.phase).toBe('ready');
  });

  it('presents candidates whenever backend manual admission is available and unowned', () => {
    const ready = readyState();
    expect(robotControlCandidatePresentationAllowed(ready, true)).toBe(true);
    expect(robotControlCandidatePresentationAllowed(ready, false)).toBe(false);
    expect(robotControlCandidatePresentationAllowed(executingState(), true)).toBe(false);
    const completed = {
      ...ready,
      phase: 'completed',
      operation: {
        operationId: 'completed-operation', operationRevision: 2, resident: true,
        state: 'completed', progress: 1, activeStopId: null,
        activeStopGeneration: null, terminalError: null,
      },
    } satisfies RobotControlState;
    expect(robotControlCandidatePresentationAllowed(completed, true)).toBe(false);
    expect(robotControlCandidatePresentationAllowed({
      ...completed,
      operation: { ...completed.operation, resident: false },
    }, true)).toBe(true);
    expect(robotControlCandidatePresentationAllowed({
      ...completed,
      operation: {
        ...completed.operation,
        resident: false,
        state: 'failed',
        terminalError: 'execution failed',
      },
    }, true)).toBe(false);
  });

  it('uses one Move To source/offset draft for numeric edits and a released target', () => {
    let state = robotControlReducer(readyState(), { type: 'select_move_source', source: {
      id: 'object:fixture', kind: 'pose', pose: { position: [0.2, 0.3, 0.4], quaternion: [0, 0, Math.SQRT1_2, Math.SQRT1_2] },
    } });
    state = robotControlReducer(state, { type: 'edit_move_options', update: { offsetXyzM: [0.01, 0.02, 0.03] } });
    const authored = robotControlReducer(state, { type: 'edit_pose', pose: { ...state.draftPose, position_m: { ...state.draftPose.position_m, x_m: 0.35 } } });
    const draft = authored.moveToDraft!;
    expect(draft.source.id).toBe('object:fixture');
    expect(draft.options.orientationMode).toBe('keep-current');
    expect(draft.options.offsetXyzM[1]).toBeCloseTo(-0.15);
    expect(draft.source.kind === 'pose' && samePose(poseTupleFromValue(authored.draftPose),
      computeCartesianMoveDraft(draft.source.pose, draft.currentTcpPose, draft.options)!)).toBe(true);
    const currentObservation = authored.status!.observedMotion;
    if (currentObservation === null) throw new Error('motion fixture observation is missing');
    const refreshedStatus = {
      ...authored.status!,
      observedMotion: { ...currentObservation, sequence: 999 },
      observedMotions: authored.status!.observedMotions.map((row) => (
        row.controllableComponentId === currentObservation.controllableComponentId
          && row.commandTargetId === currentObservation.commandTargetId
          ? { ...row, sequence: 999 }
          : row
      )),
    };
    const refreshed = robotControlReducer(authored, {
      type: 'status_refreshed', status: refreshedStatus,
    });
    expect(refreshed.moveToDraft).toBe(draft);
    expect(robotControlReducer(authored, { type: 'revert' }).moveToDraft).toBeNull();
  });

  it('fences replies and approval while dragging; cancel/no-op retains only unchanged evidence', () => {
    const ready = { ...readyState(), mode: 'pose_goal' as const };
    const dragging = robotControlReducer(ready, { type: 'begin_pose_drag' });
    expect(dragging.poseDragging).toBe(true);
    expect(robotControlReducer(dragging, { type: 'approve_started' })).toBe(dragging);
    expect(robotControlReducer(dragging, { type: 'preview_rejected', generation: ready.generation, error: 'old' })).toBe(dragging);
    const cancelled = robotControlReducer(dragging, { type: 'finish_pose_drag', generation: dragging.generation, pose: null });
    expect(cancelled.preview).toBe(ready.preview);
    expect(cancelled.poseDragging).toBe(false);
    const released = robotControlReducer(dragging, { type: 'finish_pose_drag', generation: dragging.generation,
      pose: { ...dragging.draftPose, position_m: { ...dragging.draftPose.position_m, x_m: 0.41 } } });
    expect(released.phase).toBe('planning');
    expect(released.preview).toBeNull();
    const changed = robotControlReducer(dragging, { type: 'status_refreshed', status: {
      ...dragging.status!, sceneRevision: { ...dragging.status!.sceneRevision, revision_id: 99 },
    } });
    expect(changed.poseDragging).toBe(false);
    expect(changed.preview).toBeNull();
    expect(robotControlReducer(changed, { type: 'finish_pose_drag', generation: dragging.generation, pose: released.draftPose })).toBe(changed);
  });
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
      programRunId: status.directControl!.programRunId, operationId: 'completed-1',
      operationRevision: 2, resident: false, state: 'completed', progress: 1,
      activeStopGeneration: null, activeStopId: null, terminalError: null,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const oldDraft = state.draftPositionsRad;
    const firstObservation = status.observedMotions[0];
    if (firstObservation === undefined) throw new Error('motion observation fixture is missing');
    status = {
      ...status,
      observedMotion: { ...status.observedMotion!, positionsRad: [0.5, 0.5] },
      observedMotions: [{ ...firstObservation, positionsRad: [0.5, 0.5] }],
    };
    state = robotControlReducer(state, { type: 'status_refreshed', status });
    expect(state.draftPositionsRad).toBe(oldDraft); // No observation-driven draft copy.
    state = robotControlReducer(state, { type: 'edit_joint', jointIndex: 0, positionRad: 0.6 });
    expect(state.draftPositionsRad).toEqual([0.6, 0.5]);
    state = robotControlReducer(state, { type: 'status_refreshed', status: {
      ...status,
      observedMotion: { ...status.observedMotion!, positionsRad: [0.2, 0.3] },
      observedMotions: [{ ...firstObservation, positionsRad: [0.2, 0.3] }],
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
    const blocked = robotControlReducer(approving, {
      type: 'approve_rejected', generation: ready.generation, error: 'current response',
    });
    expect(blocked.error).toBe('current response');
    const status = structuredClone(blocked.status!);
    status.sceneRevision.revision_id += 1;
    const sceneChanged = robotControlReducer(blocked, { type: 'status_refreshed', status });
    expect(sceneChanged.phase).toBe('blocked');
    expect(sceneChanged.intent).toBe(blocked.intent);
    expect(sceneChanged.error).toBe(blocked.error);
    expect(sceneChanged.generation).toBe(blocked.generation);
  });

  it('keeps a target rejection in the panel until the next edit, without an operational error', () => {
    const ready = readyState();
    const rejected = robotControlReducer(ready, { type: 'preview_rejected', generation: ready.generation,
      error: 'IK failed', targetRejection: 'Target is not reachable.' });
    expect(rejected).toMatchObject({ phase: 'blocked', preview: null, error: null,
      previewIssue: 'Target is not reachable.' });
    expect(robotControlReducer(rejected, { type: 'edit_joint', jointIndex: 0, positionRad: 0.1 }).previewIssue).toBeNull();
    expect(robotControlReducer(rejected, { type: 'select_mode', mode: 'pose_goal' }).previewIssue).toBeNull();
    expect(robotControlReducer(rejected, { type: 'revert' }).previewIssue).toBeNull();
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
    // Retained ceilings belong to the backend, not caller-supplied copies.
    expect(participant.payload.per_joint_dynamics_limits).toEqual({});
  });

  it('carries manual limits through every motion mode and invalidates an older preview', () => {
    const ready = readyState();
    let state = robotControlReducer(ready, { type: 'edit_motion_settings', update: {
      velocityScale: 0.4, accelerationScale: 0.3, tcpLinearSpeedMps: 0.07,
      tcpAngularSpeedRadS: 0.2, tcpLinearAccelerationMps2: 0.15, tcpAngularAccelerationRadS2: 0.6,
      cartesianTranslationToleranceM: 0.003, cartesianRotationToleranceRad: 0.04,
      cartesianMaxBlendDeviationRad: 0.02,
      jointLimits: { joint1: { max_velocity_rad_s: 0.5 } },
    } });
    state = robotControlReducer(state, { type: 'edit_motion_settings', update: {
      jointLimits: { joint1: { max_acceleration_rad_s2: 0.8 }, joint2: { max_velocity_rad_s: 0.7 } },
    } });
    expect(state.phase).toBe('planning');
    expect(state.preview).toBeNull();
    expect(state.generation).toBe(ready.generation + 2);
    for (const mode of ['joint', 'pose_goal', 'move_to', 'cartesian_path'] as const) {
      const payload = buildPreviewIntent({ ...state, mode }, `settings-${mode}`).participants[0].payload;
      expect(payload).toMatchObject({ kind: 'motion',
        speed_profile: { velocity_scale: 0.4, acceleration_scale: 0.3, max_tcp_linear_speed_mps: 0.07 },
        per_joint_dynamics_limits: { joint1: { max_velocity_rad_s: 0.5, max_acceleration_rad_s2: 0.8 },
          joint2: { max_velocity_rad_s: 0.7 } },
      });
      if (mode === 'cartesian_path') expect(payload).toMatchObject({ motion: {
        max_angular_speed_rad_s: 0.2,
        max_linear_acceleration_mps2: 0.15, max_angular_acceleration_rad_s2: 0.6,
        max_blend_deviation_rad: 0.02,
      }, tolerances: { translation_tol_m: 0.003, rotation_tol_rad: 0.04 } });
    }
    expect(robotControlReducer(state, { type: 'revert' }).motionSettings).toBe(state.motionSettings);
    const cleared = robotControlReducer(state, { type: 'edit_motion_settings', update: {
      jointLimits: { joint1: { max_velocity_rad_s: null, max_acceleration_rad_s2: null }, joint2: { max_velocity_rad_s: null } },
    } });
    expect(buildPreviewIntent(cleared, 'cleared').participants[0].payload).toMatchObject({ per_joint_dynamics_limits: {} });
    const active = executingState();
    expect(robotControlReducer(active, { type: 'edit_motion_settings', update: { velocityScale: 0.2 } })).toBe(active);
    for (const value of [0, -1, NaN, Infinity, 1.01]) {
      expect(robotControlReducer(state, { type: 'edit_motion_settings', update: { velocityScale: value } })).toBe(state);
    }
  });

  it('uses Program Cartesian defaults until the manual move overrides them', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
      cartesianDefaults: {
        translationToleranceM: 0.002,
        maxBlendDeviationRad: 0.015,
      },
    });
    expect(state.motionSettings.cartesianTranslationToleranceM).toBe(0.002);
    expect(state.motionSettings.cartesianMaxBlendDeviationRad).toBe(0.015);
    state = robotControlReducer(state, {
      type: 'edit_motion_settings',
      update: {
        cartesianTranslationToleranceM: 0.001,
        cartesianMaxBlendDeviationRad: 0,
      },
    });
    expect(state.motionSettings.cartesianTranslationToleranceM).toBe(0.001);
    expect(state.motionSettings.cartesianMaxBlendDeviationRad).toBe(0);
  });

  it('does not plan when only settings change at idle, and resets settings with the target owner', () => {
    const loaded = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status: statusFixture() });
    const edited = robotControlReducer(loaded, { type: 'edit_motion_settings', update: { velocityScale: 0.4 } });
    expect(edited.phase).toBe('idle');
    expect(edited.generation).toBe(loaded.generation);
    const restarted = robotControlReducer(edited, { type: 'status_refreshed', status: statusFixture('new-owner') });
    expect(restarted.motionSettings.velocityScale).toBe(1);
  });

  it('bounds TCP defaults but preserves larger requests for backend capping', () => {
    const status = statusFixture();
    status.directControl!.targetDescriptor.operationalLimit = {
      maximumTcpLinearSpeedMps: 0.05,
    };
    const loaded = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    expect(loaded.motionSettings.tcpLinearSpeedMps).toBe(0.05);
    for (const intent of [
      buildJointMotionIntent(status, [0.2, -0.4], 'joint'),
      buildPoseMotionIntent(status, loaded.draftPose, 'pose'),
      buildCartesianMotionIntent(status, loaded.draftPose, 'cartesian'),
    ]) expect(intent.participants[0].payload).toMatchObject({
      speed_profile: { max_tcp_linear_speed_mps: 0.05 },
    });
    const capped = robotControlReducer(loaded, {
      type: 'edit_motion_settings', update: { tcpLinearSpeedMps: 0.06 },
    });
    expect(capped.motionSettings.tcpLinearSpeedMps).toBe(0.06);
    expect(buildJointMotionIntent(status, [0.2, -0.4], 'capped', capped.motionSettings)
      .participants[0].payload).toMatchObject({
      speed_profile: { max_tcp_linear_speed_mps: 0.06 },
    });
    const updates: Array<Partial<ManualMotionSettings>> = [
      { jointLimits: { joint1: { max_velocity_rad_s: 1.6 } } },
      { jointLimits: { joint1: { max_acceleration_rad_s2: 2.6 } } },
      { jointLimits: { joint2: { max_velocity_rad_s: 2.1 } } },
    ];
    for (const update of updates) {
      expect(robotControlReducer(loaded, { type: 'edit_motion_settings', update })).toBe(loaded);
      expect(() => buildJointMotionIntent(status, [0.2, -0.4], 'invalid',
        { ...loaded.motionSettings, ...update })).toThrow('Manual motion limits are invalid');
    }
    const atCeilings = robotControlReducer(loaded, { type: 'edit_motion_settings', update: {
      jointLimits: { joint1: { max_velocity_rad_s: 1.5, max_acceleration_rad_s2: 2.5 } },
    } });
    expect(atCeilings.motionSettings.jointLimits.joint1).toEqual({ max_velocity_rad_s: 1.5, max_acceleration_rad_s2: 2.5 });
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
    expect(executing.ghost).toBeNull();
    expect(executing.ghostStale).toBe(false);
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
    expect(state.ghost).toBeNull();
    expect(state.ghostStale).toBe(false);

    const observed = statusFixture();
    observed.observedMotion!.positionsRad = [0.24, 0.49];
    observed.observedGripper = {
      ...observed.observedGripper!,
      position: 0.031,
    };
    observed.observedGrippers = [{ ...observed.observedGrippers[0], position: 0.031 }];
    observed.manualOperations = [{ ...state.operation!, programRunId: observed.directControl!.programRunId }];
    state = robotControlReducer(state, {
      type: 'status_refreshed',
      status: observed,
    });
    expect(state.draftPositionsRad).toEqual([0.24, 0.49]);
    expect(state.draftGripperPosition).toBe(0.031);

    const settledStatus = statusFixture();
    settledStatus.observedMotion!.positionsRad = [0.24, 0.49];
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
      programRunId: status.directControl!.programRunId,
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
      ...executingState().operation!, programRunId: status.directControl!.programRunId,
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
      ...state.operation!, programRunId: status.directControl!.programRunId,
      operationId: 'next-move-from-another-client', resident,
      state: resident ? 'executing' : 'completed',
    }];
    const next = robotControlReducer(state, { type: 'status_refreshed', status });
    expect(next.operation?.operationId).toBe('next-move-from-another-client');
    expect(next.phase).toBe(resident ? 'executing' : 'completed');
    expect(next.error).toBeNull();
  });

  it('does not resurrect a settled result on mode switch, but recovers new owner evidence', () => {
    const status = statusFixture();
    status.manualOperations = [{
      ...executingState().operation!, programRunId: status.directControlGripper!.programRunId,
      state: 'completed', resident: false,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    expect(state.operation).toBeNull();
    state = robotControlReducer(state, { type: 'select_mode', mode: 'gripper' });
    expect(state.operation).toBeNull();
    expect(state.phase).toBe('idle');
    const changed = structuredClone(status);
    changed.ownerEpoch = 'next-owner';
    state = robotControlReducer(state, { type: 'status_refreshed', status: changed });
    expect(state.mode).toBe('gripper');
    expect(state.operation?.state).toBe('completed');
  });

  it.each(['completed', 'stopped'] as const)('does not label a new editor with a previous %s move', (terminal) => {
    const status = statusFixture();
    status.manualOperations = [{
      ...executingState().operation!, programRunId: status.directControl!.programRunId,
      state: terminal, progress: 0.43, resident: false,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    expect(state.phase).toBe(terminal);
    state = robotControlReducer(state, { type: 'select_mode', mode: 'move_to' });
    state = robotControlReducer(state, { type: 'status_refreshed', status: structuredClone(status) });
    expect(state.phase).toBe('idle');
    expect(state.operation).toBeNull();
    expect(state.status?.manualOperations).toEqual(status.manualOperations);
  });

  it('a mode switch cannot hide a still-resident stopped operation', () => {
    const status = statusFixture();
    status.manualOperations = [{
      ...executingState().operation!, programRunId: status.directControl!.programRunId,
      state: 'stopped', progress: 0.43, resident: true,
    }];
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    state = robotControlReducer(state, { type: 'select_mode', mode: 'move_to' });
    expect(state.operation?.resident).toBe(true);
    expect(state.phase).toBe('stopped');
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

  it.each([false, true])('revalidates scene changes, not observations; retained result=%s', (retained) => {
    const ready = readyState();
    if (retained) ready.status!.manualOperations = [{
      programRunId: ready.status!.directControl!.programRunId, operationId: 'completed-1',
      operationRevision: 2, resident: false, state: 'completed', progress: 1,
      activeStopGeneration: null, activeStopId: null, terminalError: null,
    }];
    const fresh = structuredClone(ready.status!);
    fresh.observedMotion!.positionsRad = [0.01, 0.02];
    const stillReady = robotControlReducer(ready, { type: 'status_refreshed', status: fresh });
    expect(stillReady.phase).toBe('ready');
    expect(stillReady.generation).toBe(ready.generation);
    const replacement = structuredClone(fresh);
    replacement.sceneRevision.revision_id += 1;
    const changed = robotControlReducer(stillReady, { type: 'status_refreshed', status: replacement });
    expect(changed.preview).toBeNull();
    expect(changed.phase).toBe('planning');
    expect(changed.draftPositionsRad).toEqual(ready.draftPositionsRad);
    expect(changed.ghost).toBe(ready.ghost);
    expect(changed.ghostStale).toBe(true);
    expect(changed.error).toBeNull();
    expect(changed.generation).toBeGreaterThan(ready.generation);
    const unchanged = robotControlReducer(changed, {
      type: 'status_refreshed', status: structuredClone(replacement),
    });
    expect(unchanged.generation).toBe(changed.generation);
    const late = robotControlReducer(changed, {
      type: 'preview_resolved', generation: ready.generation, receipt: ready.preview!,
    });
    expect(late).toBe(changed);
  });

  it('fails closed instead of choosing among multiple resident operations', () => {
    const status = statusFixture();
    status.manualOperations = ['one', 'two'].map((operationId) => ({
      programRunId: status.directControl!.programRunId,
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

  it('invalidates local preview state with an informational owner notice', () => {
    const ready = readyState();
    const restarted = robotControlReducer(ready, {
      type: 'status_refreshed',
      status: statusFixture('execution-owner-2'),
    });

    expect(restarted.phase).toBe('idle');
    expect(restarted.preview).toBeNull();
    expect(restarted.ghost).toBeNull();
    expect(restarted.error).toBeNull();
    expect(restarted.ownerNotice).toContain('uncommitted manual work');
  });

  it('rebases an idle owner replacement silently', () => {
    const idle = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status: statusFixture(),
    });
    const restarted = robotControlReducer(idle, {
      type: 'status_refreshed',
      status: statusFixture('execution-owner-2'),
    });

    expect(restarted.phase).toBe('idle');
    expect(restarted.error).toBeNull();
    expect(restarted.ownerNotice).toBeNull();
  });

  it('discards a local preview at a Program lifecycle boundary', () => {
    const ready = readyState();
    const rebased = robotControlReducer(ready, {
      type: 'program_activity_changed',
    });

    expect(rebased.phase).toBe('idle');
    expect(rebased.draftPositionsRad).toEqual(
      ready.status?.observedMotion?.positionsRad,
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
      type: 'status_refreshed',
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
      waypoints: [status.observedMotion!.currentTcpPose, target],
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
      changed.status?.observedMotion?.positionsRad,
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

  it('offers no gripper control for an arm-only component', () => {
    const status = statusFixture();
    status.directControlGripper = null;
    status.observedGripper = null;
    status.directControlGripperTargets = [];
    status.observedGrippers = [];

    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    expect(state.draftGripperPosition).toBeNull();
    // Gripper mode and gripper edits are refused; the arm keeps its mode.
    const before = state.mode;
    state = robotControlReducer(state, { type: 'select_mode', mode: 'gripper' });
    expect(state.mode).toBe(before);
    state = robotControlReducer(state, { type: 'edit_gripper', position: 0.01 });
    expect(state.mode).toBe(before);
    expect(() => buildGripperCommandIntent(status, 0.01, 'arm-only'))
      .toThrow(/no gripper target/);
  });

  it('authors the first component-local command without inventing state or limits', () => {
    const status = statusFixture();
    status.directControlGripper = {
      ...status.directControlGripper!,
      sourceCoordinateName: 'gripper_position',
      sourceKind: 'component_local',
      minPosition: null,
      maxPosition: null,
    };
    status.observedGripper = null;
    status.directControlGripperTargets = [status.directControlGripper];
    status.observedGrippers = [];

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
