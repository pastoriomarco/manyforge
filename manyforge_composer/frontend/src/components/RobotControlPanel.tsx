import { createContext, useCallback, useContext, useEffect, useId, useMemo, useRef, useState } from 'react';

import { useRobotControl } from '../hooks/useRobotControl';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import {
  isCapabilityCommandReceipt,
  operationStateIsTerminal,
  quaternionFromRpyDegrees,
  robotControlHasActiveOperation,
  robotControlHasLocalDraft,
  rpyDegreesFromQuaternion,
} from '../state/robotControl';
import type {
  RobotControlMode,
  RobotControlPhase,
} from '../state/robotControl';
import type { Pose3Value } from '../types/robotControl';
import type { ProgramTreeNode } from '../types/api';
import {
  boundProgramTargetRoleIds,
  canonicalProgramPose,
  computeCartesianMoveDraft,
  resolveStoredProgramPose,
} from '../utils/moveTo';
import type { CartesianMoveDraftOptions } from '../utils/moveTo';
import { relativePose, type PoseTuple } from '../utils/poseTransforms';
import { flattenProgramForest } from '../utils/programAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import { OverlayPanel } from './OverlayPanel';

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
  ['pose_goal', 'Pose plan'],
  ['move_to', 'Move To'],
  ['cartesian_path', 'Cartesian path'],
  ['gripper', 'Gripper'],
];

const MOVE_SOURCE_GROUPS: Array<[MoveToSource['group'], string]> = [
  ['current', 'Current state'],
  ['object', 'Objects'],
  ['frame', 'Frames'],
  ['stored', 'Stored moves'],
];

interface MoveToSource {
  id: string;
  label: string;
  kind: 'pose' | 'joint';
  group: 'current' | 'object' | 'frame' | 'stored';
  pose?: PoseTuple;
  positionsRad?: number[];
  node?: ProgramTreeNode;
}

export type RobotTcpTransformMode = 'translate' | 'rotate';
export type RobotTcpTransformSpace = 'local' | 'world';

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

function cellPoseValue(value: unknown): PoseTuple | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const pose = value as Record<string, unknown>;
  const frame = pose.frame_ref;
  if (
    !frame || typeof frame !== 'object' || Array.isArray(frame)
    || (frame as Record<string, unknown>).kind !== 'cell_frame'
  ) return null;
  return canonicalProgramPose(value);
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

const RobotInputContext = createContext<{
  resetKey: string;
  reportPending: (label: string, pending: boolean) => void;
}>({
  resetKey: '',
  reportPending: () => undefined,
});

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
  const { resetKey, reportPending } = useContext(RobotInputContext);
  const onPendingChange = useCallback((dirty: boolean) => reportPending(label, dirty), [label, reportPending]);
  useEffect(() => () => reportPending(label, false), [label, reportPending]);
  const displayValue = value === null
    ? null
    : value === 0
      ? '0'
      : Math.abs(value) < 1e-4 || Math.abs(value) >= 1e5
        ? value.toExponential(6).replace(/\.0+(?=e)/, '').replace(/(\.\d*?)0+(?=e)/, '$1')
        : value.toPrecision(9).replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1');
  return (
    <DraftTextInput
      aria-label={label}
      type="number"
      min={min}
      max={max}
      step={step}
      shiftStepMultiplier={10}
      commitNativeStep
      value={displayValue}
      resetKey={resetKey}
      onPendingChange={onPendingChange}
      title={value === null ? undefined : `Exact value: ${String(value)}`}
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
  tcpTransformMode = 'translate',
  tcpTransformSpace = 'world',
  sourceFrameNames = {},
  controlBasePoseInCell = null,
  onTcpTransformModeChange = () => undefined,
  onTcpTransformSpaceChange = () => undefined,
}: {
  enabled: boolean;
  controller: RobotControlController;
  program?: ProgramRuntimeController;
  draft?: ProgramWorkspaceController;
  tcpTransformMode?: RobotTcpTransformMode;
  tcpTransformSpace?: RobotTcpTransformSpace;
  sourceFrameNames?: Readonly<Record<string, string>>;
  controlBasePoseInCell?: PoseTuple | null;
  onTcpTransformModeChange?: (mode: RobotTcpTransformMode) => void;
  onTcpTransformSpaceChange?: (space: RobotTcpTransformSpace) => void;
}) {
  const [angularStepDeg, setAngularStepDeg] = useState(5);
  const [linearStepM, setLinearStepM] = useState(0.01);
  const [coordinateStep, setCoordinateStep] = useState(0.01);
  const [moveSourceId, setMoveSourceId] = useState('');
  const [moveSourceCapture, setMoveSourceCapture] = useState<string | null>(null);
  const [storedMoveId, setStoredMoveId] = useState('');
  const [previewInfoOpen, setPreviewInfoOpen] = useState(false);
  const previewInfoButton = useRef<HTMLButtonElement>(null);
  const previewInfoId = useId();
  const [inputRebase, setInputRebase] = useState(0);
  const [pendingInputs, setPendingInputs] = useState<ReadonlySet<string>>(new Set());
  const reportPending = useCallback((label: string, dirty: boolean) => {
    setPendingInputs((previous) => {
      if (previous.has(label) === dirty) return previous;
      const next = new Set(previous);
      if (dirty) next.add(label);
      else next.delete(label);
      return next;
    });
  }, []);
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
    controlPending,
    pause,
    resume,
    reset,
    retry,
  } = controller;
  const status = state.status;
  const programActive = program?.cycleState !== null
    && program?.cycleState !== undefined
    && program.cycleState !== 'idle_after_reset';
  const inputResetKey = [status?.ownerEpoch, status?.directControl.programBindingId,
    status?.directControlGripper.programBindingId, status?.sceneRevision.scene_id,
    status?.sceneRevision.revision_id, status?.sceneRevision.content_hash,
    status?.retainedBundleId, status?.graphRevision, programActive,
    state.mode, inputRebase].join('\u0000');
  const inputContext = useMemo(() => ({ resetKey: inputResetKey, reportPending }), [inputResetKey, reportPending]);
  const operationActive = robotControlHasActiveOperation(state);
  const active = operationActive || programActive;
  const authoringBlocked = active || state.operation?.resident === true;
  const canApprove = !programActive && pendingInputs.size === 0 && state.phase === 'ready'
    && state.preview !== null
    && state.intent !== null;
  const canRevert = !active && (
    state.phase !== 'idle'
    || state.ghost !== null
    || state.operation !== null
    || pendingInputs.size > 0
  );
  const canStop = operationActive && state.operation !== null;
  const overrideCandidate = program?.state.controls
    ?.observationOverrideCandidate ?? null;
  const overridePending = program?.state.controlPending
    === 'override:stage-observation';
  const presentedPose = robotControlHasLocalDraft(state) || status === null
    ? state.draftPose : status.observedMotion.currentTcpPose;
  const rpy = rpyDegreesFromQuaternion(presentedPose.orientation);
  const authoring = draft?.state.workspace?.draft.authoring ?? null;
  const appliedBinding = program?.state.applied?.programBinding ?? null;
  const moveSources = useMemo<MoveToSource[]>(() => {
    if (status === null) return [];
    const planningCapabilities = status.directControl.targetDescriptor.contracts
      .supportingProviders.map((provider) => provider.capabilityId);
    const poseAvailable = planningCapabilities.includes('planning.motion.pose_goal');
    const jointAvailable = planningCapabilities.includes('planning.motion.joint_goal');
    const sources: MoveToSource[] = [];
    const boundTargetRoles = new Set(boundProgramTargetRoleIds(
      appliedBinding,
      status.directControl.controllableComponentId,
      status.directControl.commandTargetId,
    ));
    const frameRolePoses = new Map<string, PoseTuple>([
      [status.directControl.baseFrameRoleId, {
        position: [0, 0, 0],
        quaternion: [0, 0, 0, 1],
      }],
      [status.directControl.tcpFrameRoleId, observedPose(
        status.observedMotion.currentTcpPose,
      )],
    ]);
    const sceneResourcePoses = new Map<string, PoseTuple>();
    if (controlBasePoseInCell !== null) {
      for (const resource of authoring?.scene ?? []) {
        const cellPose = cellPoseValue(resource.pose);
        if (cellPose !== null) {
          sceneResourcePoses.set(
            resource.scene_resource_id,
            relativePose(cellPose, controlBasePoseInCell),
          );
        }
      }
    }
    if (poseAvailable) {
      sources.push({
        id: 'observed:tcp',
        label: `Current ${status.directControl.commandTargetId} TCP`,
        kind: 'pose',
        group: 'current',
        pose: observedPose(status.observedMotion.currentTcpPose),
      });
      for (const frame of status.directControl.targetDescriptor.frames) {
        const pose = frame.frameRoleId === status.directControl.baseFrameRoleId
          ? {
              position: [0, 0, 0] as [number, number, number],
              quaternion: [0, 0, 0, 1] as [number, number, number, number],
            }
          : frame.frameRoleId === status.directControl.tcpFrameRoleId
            ? observedPose(status.observedMotion.currentTcpPose)
            : null;
        if (pose !== null) sources.push({
          id: `frame:${frame.frameRoleId}`,
          label: `Frame · ${frame.frameRoleId} · ${
            sourceFrameNames[frame.compiledFrameName] ?? 'source name unavailable'
          }`,
          kind: 'pose',
          group: 'frame',
          pose,
        });
      }
      for (const [sceneResourceId, pose] of sceneResourcePoses) {
        sources.push({
          id: `scene:${sceneResourceId}`,
          label: `Object · ${sceneResourceId}`,
          kind: 'pose',
          group: 'object',
          pose,
        });
      }
    }
    for (const { node } of flattenProgramForest(authoring?.forest ?? [])) {
      const programTargetRole = node.parameters.target_role;
      if (typeof programTargetRole !== 'string'
        || !boundTargetRoles.has(programTargetRole)) continue;
      if (poseAvailable && node.node_id === 'motion.move_to_pose') {
        const pose = resolveStoredProgramPose(node.parameters, {
          programTargetRoleId: programTargetRole,
          baseFrameRoleId: status.directControl.baseFrameRoleId,
          sceneResourcePoses,
          frameRolePoses,
        });
        if (pose !== null) sources.push({
            id: `node:${node.instance_id}`,
            label: `Stored pose · ${node.instance_id}`,
            kind: 'pose',
            group: 'stored',
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
            group: 'stored',
            positionsRad: positions,
            node,
          });
        }
      }
    }
    return sources;
  }, [
    appliedBinding,
    authoring,
    controlBasePoseInCell,
    sourceFrameNames,
    status,
  ]);

  const eligibleStoredMoves = useMemo(() => moveSources.filter((source) => (
    source.node !== undefined
    && (state.mode === 'joint'
      ? source.kind === 'joint'
      : state.mode === 'pose_goal' || state.mode === 'cartesian_path'
        ? source.kind === 'pose'
        : false)
  )), [moveSources, state.mode]);
  const selectedStoredMove = eligibleStoredMoves.find(
    (source) => source.id === storedMoveId,
  );

  const applyMoveSource = (
    sourceId: string,
    options: CartesianMoveDraftOptions = moveOptions,
  ) => {
    const source = moveSources.find((row) => row.id === sourceId);
    setMoveSourceCapture(source === undefined ? null
      : JSON.stringify([source.id, source.kind, source.positionsRad ?? source.pose]));
    if (source === undefined) {
      revert();
      setInputRebase((value) => value + 1);
      return;
    }
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
  const selectedMoveSourceKey = selectedMoveSource === undefined ? null
    : JSON.stringify([selectedMoveSource.id, selectedMoveSource.kind,
      selectedMoveSource.positionsRad ?? selectedMoveSource.pose]);
  const moveSourceCurrent = moveSourceCapture === null
    || moveSourceCapture === selectedMoveSourceKey;
  useEffect(() => {
    if (state.mode !== 'move_to' || authoringBlocked || state.phase === 'approving'
      || moveSourceCurrent) return;
    setMoveSourceCapture(null);
    setMoveSourceId('');
    setInputRebase((value) => value + 1);
    revert();
  }, [authoringBlocked, moveSourceCurrent, revert, state.mode, state.phase]);

  const saveObservedIntoStoredMove = () => {
    if (selectedStoredMove?.node === undefined) return;
    void draft?.mutate('program_draft_store_observed_move', {
      instance_id: selectedStoredMove.node.instance_id,
      observed_kind: selectedStoredMove.kind === 'joint' ? 'joint' : 'tcp_pose',
    });
  };

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
      position_m: { ...presentedPose.position_m, [field]: value },
      orientation: { ...presentedPose.orientation },
    });
  };

  const editPoseOrientation = (index: number, value: number) => {
    if (!Number.isFinite(value)) return;
    const next = [...rpy] as [number, number, number];
    next[index] = value;
    editPose({
      position_m: { ...presentedPose.position_m },
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
    <RobotInputContext.Provider value={inputContext}>
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
          <span>Current owner facts and connection recovery update this panel automatically.</span>
        </div>
      )}

      {enabled && status !== null && displayedTarget !== undefined && (
        <>
          {programActive && (
            <p className="central-inspector-notice" role="status">
              <strong>Manual Robot Control is paused.</strong>{' '}
              The backend Program lifecycle currently owns robot motion.
            </p>
          )}
          {state.phase === 'blocked' && (
            <p
              className="central-inspector-notice central-inspector-notice-warning"
              role="alert"
            >
              <strong>Preview is blocked.</strong>{' '}
              {state.error ?? 'The execution owner rejected this target.'}
              <button type="button" disabled={authoringBlocked} onClick={retry}>Retry preview</button>
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
            {state.mode === 'joint' && (
              <StepSelect
                label="Step"
                unit="deg"
                options={ANGULAR_STEP_DEG_OPTIONS}
                value={angularStepDeg}
                disabled={authoringBlocked}
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
                  disabled={authoringBlocked}
                  onChange={setLinearStepM}
                />
                <StepSelect
                  label="Angle step"
                  unit="deg"
                  options={ANGULAR_STEP_DEG_OPTIONS}
                  value={angularStepDeg}
                  disabled={authoringBlocked}
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
                disabled={authoringBlocked}
                onChange={setCoordinateStep}
              />
            )}
            {(state.mode === 'pose_goal' || state.mode === 'cartesian_path') && (
              <div className="robot-control-tcp-tools" aria-label="TCP transform controls">
                <span className="robot-control-target-frame">
                  <span>Target frame</span>{' '}
                  <code
                    aria-label="TCP target frame"
                    title="The current direct-control contract permits this logical base frame."
                  >
                    {status.directControl.baseFrameRoleId}
                  </code>
                </span>
                <span className="robot-control-segment" role="group" aria-label="TCP gizmo mode">
                  <button
                    type="button"
                    aria-pressed={tcpTransformMode === 'translate'}
                    disabled={authoringBlocked}
                    onClick={() => onTcpTransformModeChange('translate')}
                  >Translate</button>
                  <button
                    type="button"
                    aria-pressed={tcpTransformMode === 'rotate'}
                    disabled={authoringBlocked}
                    onClick={() => onTcpTransformModeChange('rotate')}
                  >Rotate</button>
                </span>
                <span className="robot-control-segment" role="group" aria-label="TCP gizmo coordinates">
                  <button
                    type="button"
                    aria-pressed={tcpTransformSpace === 'local'}
                    disabled={authoringBlocked}
                    onClick={() => onTcpTransformSpaceChange('local')}
                  >Local</button>
                  <button
                    type="button"
                    aria-pressed={tcpTransformSpace === 'world'}
                    disabled={authoringBlocked}
                    onClick={() => onTcpTransformSpaceChange('world')}
                  >World</button>
                </span>
              </div>
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
                            {joint.maxAccelerationRadS2 === null
                              ? ' · acceleration not declared'
                              : ` · ${formatDegrees(joint.maxAccelerationRadS2)}°/s²`}
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
                            disabled={authoringBlocked}
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
                              disabled={authoringBlocked}
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
                          disabled={authoringBlocked}
                          onChange={(event) => {
                            const sourceId = event.currentTarget.value;
                            setMoveSourceId(sourceId);
                            applyMoveSource(sourceId);
                          }}
                        >
                          <option value="">Choose a source</option>
                          {MOVE_SOURCE_GROUPS.map(([group, label]) => {
                            const rows = moveSources.filter((source) => source.group === group);
                            return rows.length === 0 ? null : (
                              <optgroup key={group} label={label}>
                                {rows.map((source) => (
                                  <option key={source.id} value={source.id}>{source.label}</option>
                                ))}
                              </optgroup>
                            );
                          })}
                        </select>
                      </label>
                      {selectedMoveSource?.kind !== 'joint' && (
                        <>
                          <label>
                            <span>Orientation</span>
                            <select
                              value={moveOptions.orientationMode}
                              disabled={authoringBlocked || !moveSourceId}
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
                              disabled={authoringBlocked || !moveSourceId}
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
                              disabled={authoringBlocked || !moveSourceId}
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
                          value={presentedPose.position_m[field]}
                          disabled={authoringBlocked}
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
                        disabled={authoringBlocked}
                        onCommit={(value) => editPoseOrientation(index, value)}
                      />
                        <span>deg</span>
                      </span>
                    </label>
                  ))}
                  <p className="robot-control-limits robot-control-pose-note">
                    Absolute TCP target in{' '}
                    <code>{(() => {
                      const base = status.directControl.targetDescriptor.frames.find(
                        (frame) => frame.frameRoleId === status.directControl.baseFrameRoleId,
                      );
                      return base === undefined
                        ? status.directControl.baseFrameRoleId
                        : `${base.frameRoleId} · ${
                            sourceFrameNames[base.compiledFrameName] ?? 'source name unavailable'
                          }`;
                    })()}</code>.{' '}
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
                <p className="robot-control-pose-note">
                  Completion confirms the observed coordinate, not an object grasp.
                </p>
                {status.directControlGripper.targetDescriptor.namedPresets.length > 0 && (
                  <label className="robot-control-preset">
                    <span>Named preset</span>
                    <select
                      defaultValue=""
                      disabled={authoringBlocked}
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
                        disabled={authoringBlocked}
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
                        disabled={authoringBlocked}
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

              {(state.mode === 'joint'
                || state.mode === 'pose_goal'
                || state.mode === 'cartesian_path') && (
                <fieldset className="robot-control-stored-move">
                  <legend>Apply observed state to a stored move</legend>
                  <label>
                    <span>Eligible Program Draft move</span>
                    <select
                      value={selectedStoredMove === undefined ? '' : storedMoveId}
                      disabled={authoringBlocked || eligibleStoredMoves.length === 0}
                      onChange={(event) => setStoredMoveId(event.currentTarget.value)}
                    >
                      <option value="">Choose a stored move</option>
                      {eligibleStoredMoves.map((source) => (
                        <option key={source.id} value={source.id}>{source.label}</option>
                      ))}
                    </select>
                  </label>
                  <div className="robot-control-stored-actions">
                    <button
                      type="button"
                      disabled={
                        overrideCandidate?.available !== true
                        || program?.state.controlPending !== null
                      }
                      title={overrideCandidate?.reason
                        ?? 'Stage the exact source-owned observation for the held Program attempt.'}
                      onClick={() => program?.stageObservationOverride()}
                    >
                      {overridePending ? 'Staging' : 'Stage as runtime override'}
                    </button>
                    <button
                      type="button"
                      disabled={
                        active
                        || !draft?.mutationAvailable
                        || selectedStoredMove?.node === undefined
                      }
                      title="Save the exact source-owned current joints or TCP pose into this Program Draft move."
                      onClick={saveObservedIntoStoredMove}
                    >
                      Save to Program Draft
                    </button>
                  </div>
                </fieldset>
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
                    ref={previewInfoButton}
                    type="button"
                    className="inspector-info-button"
                    aria-label="Preview information"
                    aria-haspopup="dialog"
                    aria-expanded={previewInfoOpen}
                    aria-controls={previewInfoOpen ? previewInfoId : undefined}
                    onClick={() => setPreviewInfoOpen(true)}
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
                  <p className="robot-control-tcp">
                    TCP predicted at{' '}
                    <code>
                      {state.ghost.finalTcpPose.position_m.x_m.toFixed(3)}, {' '}
                      {state.ghost.finalTcpPose.position_m.y_m.toFixed(3)}, {' '}
                      {state.ghost.finalTcpPose.position_m.z_m.toFixed(3)} m
                    </code>
                  </p>
              )}

              {state.operation !== null && (
                <div className="robot-control-operation">
                  <progress aria-label="Operation progress" max={1} value={state.operation.progress} />
                  <span>
                    {state.operation.state} ·{' '}
                    {Math.round(state.operation.progress * 100)}%
                  </span>
                  {state.operation.resident && <div className="robot-control-operation-actions">
                    <button
                      type="button"
                      disabled={state.operation.state !== 'executing' || controlPending !== null}
                      onClick={() => { void pause(); }}
                    >
                      {controlPending === 'pause' ? 'Sending Pause' : 'Pause'}
                    </button>
                    <button
                      type="button"
                      disabled={state.operation.state !== 'paused' || controlPending !== null}
                      onClick={() => { void resume(); }}
                    >
                      {controlPending === 'resume' ? 'Sending Resume' : 'Resume'}
                    </button>
                    <button
                      type="button"
                      disabled={
                        !state.operation.resident
                        || !operationStateIsTerminal(state.operation.state)
                        || controlPending !== null
                      }
                      title="Release this exact terminal direct operation after the owner has certified containment."
                      onClick={() => { void reset(); }}
                    >
                      {controlPending === 'reset' ? 'Sending Reset' : 'Reset operation'}
                    </button>
                  </div>}
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
                className="approve-button"
                type="button"
                disabled={!canApprove || (state.mode === 'move_to' && !moveSourceCurrent)}
                title="Submit this exact validated target to the execution owner."
                onClick={() => void approve()}
              >
                Approve
              </button>
              <button
                type="button"
                disabled={!canRevert}
                title="Discard only the local no-motion preview."
                onClick={() => {
                  setInputRebase((previous) => previous + 1);
                  setPendingInputs(new Set());
                  revert();
                }}
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
          {previewInfoOpen && <OverlayPanel
            open={previewInfoOpen}
            title="Preview information"
            titleId={previewInfoId}
            kind="info"
            onClose={() => setPreviewInfoOpen(false)}
            returnFocusRef={previewInfoButton}
          >
            <p>The preview is owner-validated and does not move the robot. Only Approve submits the exact validated target.</p>
            <p>Target coordinates are relative to <code>{status.directControl.baseFrameRoleId}</code>. Local/World selects gizmo axes, not the target reference frame.</p>
            {state.error !== null && <p className="robot-control-error">{state.error}</p>}
            {state.ghost !== null && <>
              <dl className="robot-control-ghost-joints">
                {state.ghost.sourceJointNames.map((name, index) => (
                  <div key={name}>
                    <dt>{name}</dt>
                    <dd>{state.ghost?.finalPositionsRad[index]} rad</dd>
                  </div>
                ))}
              </dl>
              <p>Scene revision {state.ghost.sceneRevision.revision_id} · collision revision {state.ghost.collisionWorldRevision.revision_id}</p>
            </>}
            {state.operation !== null && <p>Operation <code>{state.operation.operationId}</code> · revision {state.operation.operationRevision} · {state.operation.resident ? 'retained by owner' : 'settled by owner'}</p>}
            <details>
              <summary>Target and owner contract</summary>
              <pre>{JSON.stringify({
                ownerEpoch: status.ownerEpoch,
                bindingId: state.mode === 'gripper' ? status.directControlGripper.programBindingId : status.directControl.programBindingId,
                target: state.mode === 'gripper' ? status.directControlGripper.targetDescriptor : status.directControl.targetDescriptor,
              }, null, 2)}</pre>
            </details>
          </OverlayPanel>}
        </>
      )}
    </section>
    </RobotInputContext.Provider>
  );
}

export function RobotControlPanel({ enabled }: { enabled: boolean }) {
  const controller = useRobotControl(enabled);
  return <RobotControlPanelView enabled={enabled} controller={controller} />;
}
