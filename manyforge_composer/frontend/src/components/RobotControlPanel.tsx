import { useMemo, useState } from 'react';

import { useRobotControl } from '../hooks/useRobotControl';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import {
  isCapabilityCommandReceipt,
  quaternionFromRpyDegrees,
  robotControlHasActiveOperation,
  rpyDegreesFromQuaternion,
} from '../state/robotControl';
import type {
  RobotControlMode,
  RobotControlPhase,
} from '../state/robotControl';
import type { Pose3Value } from '../types/robotControl';
import type { ProgramTreeNode } from '../types/api';
import { computeCartesianMoveDraft } from '../utils/moveTo';
import type { CartesianMoveDraftOptions } from '../utils/moveTo';
import type { PoseTuple } from '../utils/poseTransforms';
import { flattenProgramForest } from '../utils/programAuthoring';
import { DraftTextInput } from './common/DraftInputs';

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;
const ANGULAR_STEP_DEG_OPTIONS = [0.1, 0.5, 1, 5, 10, 30] as const;
const LINEAR_STEP_M_OPTIONS = [0.001, 0.01, 0.1] as const;
const COORDINATE_STEP_OPTIONS = [0.001, 0.01, 0.1] as const;

function degrees(radians: number): number {
  return radians * RAD_TO_DEG;
}

function radians(degreesValue: number): number {
  return degreesValue * DEG_TO_RAD;
}

function formatDegrees(radiansValue: number): string {
  return degrees(radiansValue).toFixed(2);
}

function phaseLabel(phase: RobotControlPhase): string {
  switch (phase) {
    case 'loading': return 'Connecting';
    case 'unavailable': return 'Unavailable';
    case 'idle': return 'Ready';
    case 'planning': return 'Validating preview';
    case 'ready': return 'Ready to approve';
    case 'blocked': return 'Preview blocked';
    case 'approving': return 'Submitting';
    case 'executing': return 'Executing';
    case 'stopping': return 'Stopping';
    case 'completed': return 'Completed';
    case 'stopped': return 'Stopped';
    case 'failed': return 'Failed';
    case 'interrupted': return 'Interrupted';
    case 'intervention_required': return 'Intervention required';
    case 'cancelled_pre_dispatch': return 'Cancelled before dispatch';
  }
}

const MODE_LABELS: Array<[RobotControlMode, string]> = [
  ['joint', 'Joint'],
  ['pose_goal', 'Pose'],
  ['move_to', 'Move To'],
  ['cartesian_path', 'Cartesian'],
  ['gripper', 'Gripper'],
];

interface MoveToSource {
  id: string;
  label: string;
  kind: 'pose' | 'joint';
  pose?: PoseTuple;
  positionsRad?: number[];
  node?: ProgramTreeNode;
}

function observedPose(pose: Pose3Value): PoseTuple {
  return {
    position: [pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m],
    quaternion: [
      pose.orientation.x,
      pose.orientation.y,
      pose.orientation.z,
      pose.orientation.w,
    ],
  };
}

function poseValue(value: unknown): PoseTuple | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const pose = value as Record<string, unknown>;
  const frame = pose.frame_ref;
  const position = pose.position_m;
  const orientation = pose.orientation;
  if (
    !frame || typeof frame !== 'object' || Array.isArray(frame)
    || (frame as Record<string, unknown>).kind !== 'cell_frame'
    || !Array.isArray(position) || position.length !== 3
    || !orientation || typeof orientation !== 'object' || Array.isArray(orientation)
  ) return null;
  const orientationRecord = orientation as Record<string, unknown>;
  let quaternion: [number, number, number, number] | null = null;
  if (Array.isArray(orientationRecord.xyzw) && orientationRecord.xyzw.length === 4) {
    quaternion = orientationRecord.xyzw.map(Number) as [number, number, number, number];
  } else if (Array.isArray(orientationRecord.rpy_deg) && orientationRecord.rpy_deg.length === 3) {
    const rpy = orientationRecord.rpy_deg.map(Number);
    const converted = quaternionFromRpyDegrees(rpy[0] ?? 0, rpy[1] ?? 0, rpy[2] ?? 0);
    quaternion = [converted.x, converted.y, converted.z, converted.w];
  }
  const numericPosition = position.map(Number) as [number, number, number];
  return quaternion !== null
    && numericPosition.every(Number.isFinite)
    && quaternion.every(Number.isFinite)
    ? { position: numericPosition, quaternion }
    : null;
}

function poseTarget(tuple: PoseTuple): Pose3Value {
  return {
    position_m: {
      x_m: tuple.position[0],
      y_m: tuple.position[1],
      z_m: tuple.position[2],
    },
    orientation: {
      x: tuple.quaternion[0],
      y: tuple.quaternion[1],
      z: tuple.quaternion[2],
      w: tuple.quaternion[3],
    },
  };
}

function targetIsTemporary(
  phase: RobotControlPhase,
  draft: number,
  observed: number,
): boolean {
  return phase !== 'loading'
    && phase !== 'unavailable'
    && phase !== 'idle'
    && Math.abs(draft - observed) > 1e-6;
}

function RobotNumberInput({
  label,
  value,
  step,
  min,
  max,
  disabled,
  onCommit,
  pending = false,
}: {
  label: string;
  value: number | null;
  step: number | 'any';
  min?: number;
  max?: number;
  disabled: boolean;
  onCommit: (value: number) => void;
  pending?: boolean;
}) {
  return (
    <DraftTextInput
      aria-label={label}
      type="number"
      min={min}
      max={max}
      step={step}
      shiftStepMultiplier={10}
      commitNativeStep
      value={value}
      disabled={disabled}
      className={pending ? 'robot-control-target-pending' : undefined}
      dirtyClassName="robot-control-target-pending"
      validate={(candidate) => {
        const parsed = Number(candidate);
        if (!candidate.trim() || !Number.isFinite(parsed)) return 'Enter a finite number.';
        if (min !== undefined && parsed < min) return `Minimum ${min}.`;
        if (max !== undefined && parsed > max) return `Maximum ${max}.`;
        return null;
      }}
      onCommit={(candidate) => onCommit(Number(candidate))}
    />
  );
}

function StepSelect({
  label,
  unit,
  options,
  value,
  disabled,
  onChange,
}: {
  label: string;
  unit: string;
  options: readonly number[];
  value: number;
  disabled: boolean;
  onChange: (value: number) => void;
}) {
  return (
    <label className="robot-control-step">
      <span>{label}</span>
      <select
        aria-label={`${label} (${unit})`}
        value={value}
        disabled={disabled}
        onChange={(event) => onChange(Number(event.currentTarget.value))}
      >
        {options.map((option) => (
          <option key={option} value={option}>{option}</option>
        ))}
      </select>
      <span>{unit}</span>
    </label>
  );
}

export function RobotControlPanelView({
  enabled,
  controller,
  program,
  draft,
}: {
  enabled: boolean;
  controller: RobotControlController;
  program?: ProgramRuntimeController;
  draft?: ProgramWorkspaceController;
}) {
  const [angularStepDeg, setAngularStepDeg] = useState(5);
  const [linearStepM, setLinearStepM] = useState(0.01);
  const [coordinateStep, setCoordinateStep] = useState(0.01);
  const [moveSourceId, setMoveSourceId] = useState('');
  const [moveOptions, setMoveOptions] = useState<CartesianMoveDraftOptions>({
    orientationMode: 'keep-current',
    offsetXyzM: [0, 0, 0],
    offsetRpyRad: [0, 0, 0],
  });
  const {
    state,
    selectMode,
    editMoveToJoints,
    editJoint,
    editPose,
    editGripper,
    approve,
    revert,
    stop,
    retry,
  } = controller;
  const status = state.status;
  const active = robotControlHasActiveOperation(state);
  const canApprove = state.phase === 'ready'
    && state.preview !== null
    && state.intent !== null;
  const canRevert = !active && (
    state.phase !== 'idle'
    || state.ghost !== null
    || state.operation !== null
  );
  const canStop = active && state.operation !== null;
  const overrideCandidate = program?.state.controls
    ?.observationOverrideCandidate ?? null;
  const overridePending = program?.state.controlPending
    === 'override:stage-observation';
  const rpy = rpyDegreesFromQuaternion(state.draftPose.orientation);
  const authoring = draft?.state.workspace?.draft.authoring ?? null;
  const moveSources = useMemo<MoveToSource[]>(() => {
    if (status === null) return [];
    const planningCapabilities = status.directControl.targetDescriptor.contracts
      .supportingProviders.map((provider) => provider.capabilityId);
    const poseAvailable = planningCapabilities.includes('planning.motion.pose_goal');
    const jointAvailable = planningCapabilities.includes('planning.motion.joint_goal');
    const sources: MoveToSource[] = [];
    if (poseAvailable) {
      sources.push({
        id: 'observed:tcp',
        label: `Current ${status.directControl.targetRoleId} TCP`,
        kind: 'pose',
        pose: observedPose(status.observedMotion.currentTcpPose),
      });
      for (const resource of authoring?.scene ?? []) {
        const pose = poseValue(resource.pose);
        if (pose !== null) {
          sources.push({
            id: `scene:${resource.scene_resource_id}`,
            label: `Object · ${resource.scene_resource_id}`,
            kind: 'pose',
            pose,
          });
        }
      }
    }
    for (const { node } of flattenProgramForest(authoring?.forest ?? [])) {
      if (node.parameters.target_role !== status.directControl.targetRoleId) {
        continue;
      }
      if (poseAvailable && node.node_id === 'motion.move_to_pose') {
        const pose = poseValue(node.parameters.goal);
        if (pose !== null) sources.push({
          id: `node:${node.instance_id}`,
          label: `Stored pose · ${node.instance_id}`,
          kind: 'pose',
          pose,
          node,
        });
      } else if (jointAvailable && node.node_id === 'motion.move_to_joint_target') {
        const goal = node.parameters.goal;
        if (
          !Array.isArray(goal)
          || goal.length === 0
          || !status.observedMotion.complete
          || status.observedMotion.positionsRad.length
            !== status.directControl.targetDescriptor.joints.length
        ) continue;
        const positions = [...status.observedMotion.positionsRad];
        const inventory = new Map(status.directControl.targetDescriptor.joints.map(
          (joint, index) => [joint.sourceJointName, index] as const,
        ));
        const selected = new Set<string>();
        const compatible = goal.every((row) => {
          if (!row || typeof row !== 'object' || Array.isArray(row)) return false;
          const record = row as Record<string, unknown>;
          const name = record.joint_name;
          const position = record.position_rad;
          const index = typeof name === 'string' ? inventory.get(name) : undefined;
          if (
            typeof name !== 'string'
            || selected.has(name)
            || index === undefined
            || typeof position !== 'number'
            || !Number.isFinite(position)
          ) return false;
          selected.add(name);
          positions[index] = position;
          return true;
        });
        if (compatible) {
          sources.push({
            id: `node:${node.instance_id}`,
            label: `Stored joints · ${node.instance_id}`,
            kind: 'joint',
            positionsRad: positions,
            node,
          });
        }
      }
    }
    return sources;
  }, [authoring, status]);

  const applyMoveSource = (
    sourceId: string,
    options: CartesianMoveDraftOptions = moveOptions,
  ) => {
    const source = moveSources.find((row) => row.id === sourceId);
    if (source?.kind === 'joint' && source.positionsRad !== undefined) {
      editMoveToJoints(source.positionsRad);
      return;
    }
    if (source?.pose === undefined || status === null) return;
    const target = computeCartesianMoveDraft(
      source.pose,
      observedPose(status.observedMotion.currentTcpPose),
      options,
    );
    if (target === null) return;
    selectMode('move_to');
    editPose(poseTarget(target));
  };
  const selectedMoveSource = moveSources.find((row) => row.id === moveSourceId);

  const modeAvailable = (mode: RobotControlMode): boolean => {
    if (status === null || mode === 'gripper') return status !== null;
    if (mode === 'move_to') {
      return status.directControl.targetDescriptor.contracts.supportingProviders.some((provider) => (
        provider.capabilityId === 'planning.motion.pose_goal'
        || provider.capabilityId === 'planning.motion.joint_goal'
      ));
    }
    const capability = mode === 'joint'
      ? 'planning.motion.joint_goal'
      : `planning.motion.${mode}`;
    return status.directControl.targetDescriptor.contracts.supportingProviders
      .some((provider) => provider.capabilityId === capability);
  };

  const editPosePosition = (
    field: keyof Pose3Value['position_m'],
    value: number,
  ) => {
    if (!Number.isFinite(value)) return;
    editPose({
      position_m: { ...state.draftPose.position_m, [field]: value },
      orientation: { ...state.draftPose.orientation },
    });
  };

  const editPoseOrientation = (index: number, value: number) => {
    if (!Number.isFinite(value)) return;
    const next = [...rpy] as [number, number, number];
    next[index] = value;
    editPose({
      position_m: { ...state.draftPose.position_m },
      orientation: quaternionFromRpyDegrees(...next),
    });
  };

  const displayedTarget = state.mode === 'gripper'
    ? status?.directControlGripper
    : status?.directControl;
  const observedGripperPosition = status?.observedGripper?.position ?? null;
  const gripperTargetPending = observedGripperPosition !== null
    && state.draftGripperPosition !== null
    && targetIsTemporary(
      state.phase,
      state.draftGripperPosition,
      observedGripperPosition,
    );
  const presentedGripperPosition = gripperTargetPending
    ? state.draftGripperPosition
    : observedGripperPosition ?? state.draftGripperPosition;

  return (
    <section
      className="panel robot-control-panel"
      aria-label="Robot Control"
      data-testid="robot-control-panel"
      title={displayedTarget === undefined
        ? undefined
        : `Control route: ${displayedTarget.controllableComponentId} / ${displayedTarget.commandTargetId}`}
    >
      {!enabled && (
        <div className="panel-state">
          <strong>Headless execution is not connected.</strong>
          <span>Manual control remains unavailable in structural-only mode.</span>
        </div>
      )}

      {enabled && state.phase === 'loading' && (
        <div className="panel-state">Reading the execution owner contract…</div>
      )}

      {enabled && state.phase === 'unavailable' && (
        <div className="panel-state error-state">
          <strong>Robot Control is unavailable.</strong>
          <span>{state.error}</span>
          <button type="button" onClick={retry}>Retry</button>
        </div>
      )}

      {enabled && status !== null && displayedTarget !== undefined && (
        <>
          {state.phase === 'blocked' && (
            <p
              className="central-inspector-notice central-inspector-notice-warning"
              role="alert"
            >
              <strong>Preview is blocked.</strong>{' '}
              {state.error ?? 'The execution owner rejected this target.'}
            </p>
          )}
          <nav className="robot-control-modes" aria-label="Manual control mode">
            {MODE_LABELS.map(([mode, label]) => (
              <button
                key={mode}
                type="button"
                aria-pressed={state.mode === mode}
                disabled={active || !modeAvailable(mode)}
                onClick={() => selectMode(mode)}
              >
                {label}
              </button>
            ))}
          </nav>

          <div
            className="robot-control-step-controls"
            aria-label="Robot control increments"
          >
            {(state.mode === 'joint'
              || (state.mode === 'move_to' && selectedMoveSource?.kind === 'joint')) && (
              <StepSelect
                label="Step"
                unit="deg"
                options={ANGULAR_STEP_DEG_OPTIONS}
                value={angularStepDeg}
                disabled={active}
                onChange={setAngularStepDeg}
              />
            )}
            {(state.mode === 'pose_goal'
              || (state.mode === 'move_to' && selectedMoveSource?.kind !== 'joint')
              || state.mode === 'cartesian_path') && (
              <>
                <StepSelect
                  label="Position step"
                  unit="m"
                  options={LINEAR_STEP_M_OPTIONS}
                  value={linearStepM}
                  disabled={active}
                  onChange={setLinearStepM}
                />
                <StepSelect
                  label="Angle step"
                  unit="deg"
                  options={ANGULAR_STEP_DEG_OPTIONS}
                  value={angularStepDeg}
                  disabled={active}
                  onChange={setAngularStepDeg}
                />
              </>
            )}
            {state.mode === 'gripper' && (
              <StepSelect
                label="Step"
                unit={status.directControlGripper.targetDescriptor.unit}
                options={COORDINATE_STEP_OPTIONS}
                value={coordinateStep}
                disabled={active}
                onChange={setCoordinateStep}
              />
            )}
          </div>

          <div className="robot-control-body">
            <div
              className="robot-control-editor"
              data-testid="robot-control-editor-scroll"
            >
              {state.mode === 'joint' && (
                <div className="robot-control-joints">
                  {status.directControl.targetDescriptor.joints.map((joint, index) => {
                    const live = status.observedMotion.positionsRad[index] ?? 0;
                    const draft = state.draftPositionsRad[index] ?? live;
                    const pending = targetIsTemporary(state.phase, draft, live);
                    const presented = pending ? draft : live;
                    return (
                      <label
                        className="robot-control-joint"
                        key={joint.sourceJointName}
                      >
                        <span className="robot-control-joint-heading">
                          <strong>{joint.sourceJointName}</strong>
                          <span className="robot-control-limits">
                            {formatDegrees(joint.minPositionRad)}° to{' '}
                            {formatDegrees(joint.maxPositionRad)}° · max{' '}
                            {formatDegrees(joint.maxVelocityRadS)}°/s
                          </span>
                        </span>
                        <span className="robot-control-joint-inputs">
                          <input
                            aria-label={`${joint.sourceJointName} target`}
                            type="range"
                            min={degrees(joint.minPositionRad)}
                            max={degrees(joint.maxPositionRad)}
                            step="any"
                            value={degrees(presented)}
                            disabled={active}
                            onChange={(event) => {
                              const value = event.currentTarget.valueAsNumber;
                              if (Number.isFinite(value)) {
                                editJoint(index, radians(value));
                              }
                            }}
                          />
                          <span className="robot-control-number">
                            <RobotNumberInput
                              label={`${joint.sourceJointName} target degrees`}
                              min={degrees(joint.minPositionRad)}
                              max={degrees(joint.maxPositionRad)}
                              step={angularStepDeg}
                              value={degrees(presented)}
                              disabled={active}
                              pending={pending}
                              onCommit={(value) => editJoint(index, radians(value))}
                            />
                            <span>deg</span>
                          </span>
                        </span>
                      </label>
                    );
                  })}
                </div>
              )}

              {(state.mode === 'pose_goal'
                || state.mode === 'move_to'
                || state.mode === 'cartesian_path') && (
                <div className="robot-control-pose-grid">
                  {state.mode === 'move_to' && (
                    <fieldset className="robot-control-move-to">
                      <legend>Move To source</legend>
                      <label>
                        <span>Object, frame, or stored move</span>
                        <select
                          value={selectedMoveSource === undefined ? '' : moveSourceId}
                          disabled={active}
                          onChange={(event) => {
                            const sourceId = event.currentTarget.value;
                            setMoveSourceId(sourceId);
                            applyMoveSource(sourceId);
                          }}
                        >
                          <option value="">Choose a source</option>
                          {moveSources.map((source) => (
                            <option key={source.id} value={source.id}>{source.label}</option>
                          ))}
                        </select>
                      </label>
                      {selectedMoveSource?.kind !== 'joint' && (
                        <>
                          <label>
                            <span>Orientation</span>
                            <select
                              value={moveOptions.orientationMode}
                              disabled={active || !moveSourceId}
                              onChange={(event) => {
                                const next = {
                                  ...moveOptions,
                                  orientationMode: event.currentTarget.value as CartesianMoveDraftOptions['orientationMode'],
                                };
                                setMoveOptions(next);
                                applyMoveSource(moveSourceId, next);
                              }}
                            >
                              <option value="keep-current">Keep current TCP</option>
                              <option value="use-entity">Use source orientation</option>
                              <option value="face-down">Face down</option>
                            </select>
                          </label>
                          <div className="robot-control-move-offsets">
                        {(['X', 'Y', 'Z'] as const).map((axis, index) => (
                          <label key={axis}>
                            <span>{axis} offset (m)</span>
                            <RobotNumberInput
                              label={`${axis} local offset metres`}
                              step={linearStepM}
                              value={moveOptions.offsetXyzM[index]}
                              disabled={active || !moveSourceId}
                              onCommit={(value) => {
                                const offsetXyzM = [...moveOptions.offsetXyzM] as [number, number, number];
                                offsetXyzM[index] = value;
                                const next = { ...moveOptions, offsetXyzM };
                                setMoveOptions(next);
                                applyMoveSource(moveSourceId, next);
                              }}
                            />
                          </label>
                        ))}
                        {(['Roll', 'Pitch', 'Yaw'] as const).map((axis, index) => (
                          <label key={axis}>
                            <span>{axis} offset (deg)</span>
                            <RobotNumberInput
                              label={`${axis} local offset degrees`}
                              step={angularStepDeg}
                              value={degrees(moveOptions.offsetRpyRad[index])}
                              disabled={active || !moveSourceId}
                              onCommit={(value) => {
                                const offsetRpyRad = [...moveOptions.offsetRpyRad] as [number, number, number];
                                offsetRpyRad[index] = radians(value);
                                const next = { ...moveOptions, offsetRpyRad };
                                setMoveOptions(next);
                                applyMoveSource(moveSourceId, next);
                              }}
                            />
                          </label>
                        ))}
                          </div>
                        </>
                      )}
                      <button
                        type="button"
                        disabled={
                          active
                          || !draft?.mutationAvailable
                          || selectedMoveSource?.node === undefined
                        }
                        title="Replace only the selected stored move with the current owner-observed target."
                        onClick={() => {
                          if (selectedMoveSource?.node === undefined) return;
                          void draft?.mutate('program_draft_store_observed_move', {
                            instance_id: selectedMoveSource.node.instance_id,
                            observed_kind: selectedMoveSource.kind === 'joint' ? 'joint' : 'tcp_pose',
                          });
                        }}
                      >
                        Save current into selected move
                      </button>
                    </fieldset>
                  )}
                  {(state.mode !== 'move_to' || selectedMoveSource?.kind !== 'joint') && (<>
                  {(['x_m', 'y_m', 'z_m'] as const).map((field) => (
                    <label key={field}>
                      <span>{field.slice(0, 1).toUpperCase()} position</span>
                      <span className="robot-control-number">
                        <RobotNumberInput
                          label={`${field.slice(0, 1)} target metres`}
                          step={linearStepM}
                          value={state.draftPose.position_m[field]}
                          disabled={active}
                          onCommit={(value) => editPosePosition(field, value)}
                        />
                        <span>m</span>
                      </span>
                    </label>
                  ))}
                  {['Roll', 'Pitch', 'Yaw'].map((label, index) => (
                    <label key={label}>
                      <span>{label}</span>
                      <span className="robot-control-number">
                      <RobotNumberInput
                        label={`${label} target degrees`}
                        step={angularStepDeg}
                        value={rpy[index]}
                        disabled={active}
                        onCommit={(value) => editPoseOrientation(index, value)}
                      />
                        <span>deg</span>
                      </span>
                    </label>
                  ))}
                  <p className="robot-control-limits robot-control-pose-note">
                    Absolute TCP target in logical{' '}
                    <code>{status.directControl.targetDescriptor.frames.find(
                      (frame) => frame.frameRoleId === status.directControl.baseFrameRoleId,
                    )?.compiledFrameName ?? status.directControl.baseFrameRoleId}</code>.{' '}
                    {state.mode === 'cartesian_path'
                      ? 'The current owner-issued TCP pose is the explicit first waypoint.'
                      : state.mode === 'move_to'
                        ? 'The selected portable source and local offsets resolve to this target.'
                        : 'Pose planning chooses a valid joint-space realization.'}
                  </p>
                  </>)}
                </div>
              )}

              {state.mode === 'gripper' && (
                <div className="robot-control-gripper-editor">
                {status.directControlGripper.targetDescriptor.namedPresets.length > 0 && (
                  <label className="robot-control-preset">
                    <span>Named preset</span>
                    <select
                      defaultValue=""
                      disabled={active}
                      onChange={(event) => {
                        const preset = status.directControlGripper.targetDescriptor
                          .namedPresets.find((candidate) => candidate.name === event.target.value);
                        if (preset) editGripper(preset.masterPosition);
                        event.target.value = '';
                      }}
                    >
                      <option value="">Choose retained preset…</option>
                      {status.directControlGripper.targetDescriptor.namedPresets.map((preset) => (
                        <option key={preset.name} value={preset.name}>{preset.name}</option>
                      ))}
                    </select>
                  </label>
                )}
                <label className="robot-control-joint robot-control-gripper">
                  <span className="robot-control-joint-heading">
                    <strong>
                      {status.directControlGripper.targetDescriptor.sourceCoordinateName}
                    </strong>
                    <span className="robot-control-limits">
                      {status.directControlGripper.targetDescriptor.minPosition === null
                        || status.directControlGripper.targetDescriptor.maxPosition === null
                        ? 'Enter the initial target'
                        : `${status.directControlGripper.targetDescriptor.minPosition.toFixed(4)} to ${
                          status.directControlGripper.targetDescriptor.maxPosition.toFixed(4)
                        } ${status.directControlGripper.targetDescriptor.unit}`}
                    </span>
                  </span>
                  <span className="robot-control-joint-inputs">
                    {status.directControlGripper.targetDescriptor.minPosition !== null
                      && status.directControlGripper.targetDescriptor.maxPosition !== null && (
                      <input
                        aria-label="Gripper target"
                        type="range"
                        min={status.directControlGripper.targetDescriptor.minPosition}
                        max={status.directControlGripper.targetDescriptor.maxPosition}
                        step="any"
                        value={presentedGripperPosition ?? ''}
                        disabled={active}
                        onChange={(event) => {
                          const value = event.currentTarget.valueAsNumber;
                          if (Number.isFinite(value)) editGripper(value);
                        }}
                      />
                    )}
                    <span className="robot-control-number">
                      <RobotNumberInput
                        label="Gripper target value"
                        min={status.directControlGripper.targetDescriptor.minPosition ?? undefined}
                        max={status.directControlGripper.targetDescriptor.maxPosition ?? undefined}
                        step={coordinateStep}
                        value={presentedGripperPosition}
                        disabled={active}
                        pending={gripperTargetPending}
                        onCommit={editGripper}
                      />
                      <span>
                        {status.directControlGripper.targetDescriptor.unit}
                      </span>
                    </span>
                  </span>
                </label>
                </div>
              )}
            </div>

            <aside className="robot-control-preview" aria-live="polite">
              <div className="robot-control-preview-heading">
                <h3>Preview</h3>
                <div className="robot-control-preview-actions">
                  {state.phase !== 'blocked'
                    && state.phase !== 'idle'
                    && state.phase !== 'ready' && (
                    <span className={`status-pill robot-control-phase phase-${state.phase}`}>
                      {phaseLabel(state.phase)}
                    </span>
                  )}
                  {state.ghostStale && state.ghost !== null && (
                    <span className="status-pill status-pill-danger">stale</span>
                  )}
                  <button
                    type="button"
                    className="inspector-info-button"
                    aria-label="Preview information"
                    title="The preview is owner-validated and does not move the robot. Only Approve can submit it for execution."
                  >i</button>
                </div>
              </div>

              {state.preview !== null
                && isCapabilityCommandReceipt(state.preview) && (
                <p className="robot-control-tcp">
                  {state.preview.sourceCoordinateName} validated at{' '}
                  <code>
                    {state.preview.targetPosition.toFixed(4)}{' '}
                    {state.preview.coordinateKind === 'angular_rad' ? 'rad' : 'm'}
                  </code>
                </p>
              )}

              {state.ghost !== null && (
                <>
                  <dl className="robot-control-ghost-joints">
                    {state.ghost.sourceJointNames.map((name, index) => (
                      <div key={name}>
                        <dt>{name}</dt>
                        <dd>
                          {formatDegrees(
                            state.ghost?.finalPositionsRad[index] ?? 0,
                          )}°
                        </dd>
                      </div>
                    ))}
                  </dl>
                  <p className="robot-control-tcp">
                    TCP predicted at{' '}
                    <code>
                      {state.ghost.finalTcpPose.position_m.x_m.toFixed(3)}, {' '}
                      {state.ghost.finalTcpPose.position_m.y_m.toFixed(3)}, {' '}
                      {state.ghost.finalTcpPose.position_m.z_m.toFixed(3)} m
                    </code>
                  </p>
                  <p className="robot-control-evidence">
                    scene revision {state.ghost.sceneRevision.revision_id} ·{' '}
                    collision revision{' '}
                    {state.ghost.collisionWorldRevision.revision_id}
                  </p>
                </>
              )}

              {state.operation !== null && (
                <div className="robot-control-operation">
                  <span>
                    Operation <code>{state.operation.operationId}</code>
                  </span>
                  <progress max={1} value={state.operation.progress} />
                  <span>
                    {state.operation.state} ·{' '}
                    {Math.round(state.operation.progress * 100)}%
                  </span>
                </div>
              )}

              {state.error !== null && state.phase !== 'blocked' && (
                <p className="robot-control-error" role="alert">{state.error}</p>
              )}
            </aside>
          </div>

          <footer
            className="robot-control-footer"
            data-testid="robot-control-candidate-footer"
          >
            <div className="robot-control-actions">
              <button
                type="button"
                disabled={
                  overrideCandidate?.available !== true
                  || program?.state.controlPending !== null
                }
                title={
                  overrideCandidate?.available === true
                    ? 'Stage this exact owner-observed Robot snapshot for the held Program attempt.'
                    : overrideCandidate?.reason
                      ?? 'No exact held Program attempt and Robot observation are available.'
                }
                onClick={() => program?.stageObservationOverride()}
              >
                {overridePending ? 'Staging…' : 'Stage snapshot'}
              </button>
              <button
                className="approve-button"
                type="button"
                disabled={!canApprove}
                title="Submit this exact validated target to the execution owner."
                onClick={() => void approve()}
              >
                Approve
              </button>
              <button
                type="button"
                disabled={!canRevert}
                title="Discard only the local no-motion preview."
                onClick={revert}
              >
                Revert
              </button>
              <button
                className="stop-button"
                type="button"
                disabled={!canStop}
                title="Stop the exact active operation; this does not merely discard its preview."
                onClick={() => void stop()}
              >
                Stop operation
              </button>
            </div>
          </footer>
        </>
      )}
    </section>
  );
}

export function RobotControlPanel({ enabled }: { enabled: boolean }) {
  const controller = useRobotControl(enabled);
  return <RobotControlPanelView enabled={enabled} controller={controller} />;
}
