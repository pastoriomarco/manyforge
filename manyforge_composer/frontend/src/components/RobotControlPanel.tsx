import { createContext, useCallback, useContext, useEffect, useId, useMemo, useRef, useState } from 'react';

import { useRobotControl } from '../hooks/useRobotControl';
import { useCardPanelLayout } from '../hooks/useCardPanelLayout';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import {
  isCapabilityCommandReceipt,
  manualOperationRequiresRecovery,
  operationStateIsTerminal,
  quaternionFromRpyDegrees,
  robotControlHasActiveOperation,
  robotControlHasLocalDraft,
  robotControlPreviewApprovable,
  rpyDegreesFromQuaternion,
} from '../state/robotControl';
import type {
  ManualMotionSettings,
  RobotControlMode,
  RobotControlPhase,
} from '../state/robotControl';
import type { Pose3Value } from '../types/robotControl';
import type { LogicalSceneProjection, ProgramTreeNode } from '../types/api';
import {
  boundProgramTargetRoleIds,
  DEFAULT_MOVE_TO_OPTIONS,
  resolveStoredProgramPose,
} from '../utils/moveTo';
import type { CartesianMoveDraftOptions } from '../utils/moveTo';
import { relativePose, type PoseTuple } from '../utils/poseTransforms';
import { flattenProgramForest } from '../utils/programAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import { OverlayPanel } from './OverlayPanel';
import { OperationProgress } from './OperationProgress';
import { GizmoSteps, StepSelect, useGizmoSettings } from './common/GizmoControls';
import type { GizmoController } from './common/GizmoControls';
import { ANGULAR_STEP_OPTIONS } from '../utils/gizmo';
import {
  allUncertainOutputsApproved,
  uncertainOutputKey,
  uncertainOutputs,
} from '../utils/uncertainOutputs';
import { UncertainOutputConfirmation } from './UncertainOutputConfirmation';

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;
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
    case 'blocked': return 'Not executable';
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
  ['cartesian_path', 'Cartesian'],
  ['move_to', 'Move To'],
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
  limitUnit,
  hint,
}: {
  label: string;
  value: number | null;
  step: number | 'any';
  min?: number;
  max?: number;
  disabled: boolean;
  onCommit: (value: number) => void;
  pending?: boolean;
  limitUnit?: string;
  hint?: string;
}) {
  const { resetKey, reportPending } = useContext(RobotInputContext);
  const [invalidMessage, setInvalidMessage] = useState<string | null>(null);
  const errorId = useId();
  const onPendingChange = useCallback((dirty: boolean) => reportPending(label, dirty), [label, reportPending]);
  useEffect(() => () => reportPending(label, false), [label, reportPending]);
  const displayValue = value === null
    ? null
    : value === 0
      ? '0'
      : Math.abs(value) < 1e-4 || Math.abs(value) >= 1e5
        ? value.toExponential(6).replace(/\.0+(?=e)/, '').replace(/(\.\d*?)0+(?=e)/, '$1')
        : value.toPrecision(9).replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1');
  const input = (
    <DraftTextInput
      aria-label={label}
      type="number"
      min={min}
      max={max}
      step={step}
      shiftStepMultiplier={10}
      commitNativeStep
      validateOnInput={limitUnit !== undefined}
      onInvalidChange={limitUnit === undefined ? undefined : setInvalidMessage}
      aria-describedby={invalidMessage === null ? undefined : errorId}
      value={displayValue}
      resetKey={resetKey}
      onPendingChange={onPendingChange}
      title={[
        hint,
        limitUnit !== undefined && max !== undefined
          ? `Global limit: ${Number(max.toPrecision(6))} ${limitUnit}.`
          : value === null ? undefined : `Exact value: ${String(value)}`,
      ].filter(Boolean).join(' ') || undefined}
      disabled={disabled}
      className={pending ? 'robot-control-target-pending' : undefined}
      dirtyClassName="robot-control-target-pending"
      validate={(candidate) => {
        const parsed = Number(candidate);
        if (!candidate.trim() || !Number.isFinite(parsed)) return 'Enter a finite number.';
        if (min !== undefined && parsed < min) return `Minimum ${min}.`;
        if (max !== undefined && parsed > max) return limitUnit === undefined
          ? `Maximum ${max}.`
          : `Exceeds global limit (${Number(max.toPrecision(6))} ${limitUnit}).`;
        return null;
      }}
      onCommit={(candidate) => onCommit(Number(candidate))}
    />
  );
  return limitUnit === undefined ? input : <span className="robot-control-limit-input">
    {input}
    {invalidMessage !== null && <span id={errorId} className="robot-control-limit-error inline-message severity-error" role="status">{invalidMessage}</span>}
  </span>;
}

function ManualMotionSettingsControls({ controller, disabled }: {
  controller: RobotControlController;
  disabled: boolean;
}) {
  const { state, editMotionSettings } = controller;
  // Rendered only for a selected motion target (never in Gripper mode).
  const descriptor = controller.selectedStatus!.directControl!.targetDescriptor;
  const settings = state.motionSettings;
  const cartesian = state.mode === 'cartesian_path';
  type MotionSettingField = {
    key: Exclude<keyof ManualMotionSettings, 'jointLimits'>;
    label: string; fullLabel: string; unit: string; factor: number; step: number;
    min?: number; max?: number;
  };
  const commonFields: MotionSettingField[] = [
    { key: 'velocityScale', label: 'Vel. (%)', fullLabel: 'Velocity scale (%)', unit: '%', factor: 100, step: 5, max: 100 },
    { key: 'accelerationScale', label: 'Acc. (%)', fullLabel: 'Acceleration scale (%)', unit: '%', factor: 100, step: 5, max: 100 },
    { key: 'tcpLinearSpeedMps', label: 'TCP Sp. (m/s)', fullLabel: 'TCP speed ceiling (m/s); capped by the applied Deployment maximum', unit: 'm/s', factor: 1, step: 0.01 },
  ];
  const advancedFields: MotionSettingField[] = cartesian ? [
    { key: 'tcpAngularSpeedRadS', label: 'TCP Ang. (deg/s)', fullLabel: 'TCP angular speed (deg/s)', unit: 'deg/s', factor: RAD_TO_DEG, step: 5 },
    { key: 'tcpLinearAccelerationMps2', label: 'TCP Acc. (m/s²)', fullLabel: 'TCP linear acceleration (m/s²)', unit: 'm/s²', factor: 1, step: 0.1 },
    { key: 'tcpAngularAccelerationRadS2', label: 'Ang. Acc. (deg/s²)', fullLabel: 'TCP angular acceleration (deg/s²)', unit: 'deg/s²', factor: RAD_TO_DEG, step: 10 },
    { key: 'cartesianTranslationToleranceM', label: 'Lin. Tol. (m)', fullLabel: 'Cartesian linear path tolerance (m)', unit: 'm', factor: 1, step: 0.001 },
    { key: 'cartesianRotationToleranceRad', label: 'Rot. Tol. (deg)', fullLabel: 'Cartesian rotation path tolerance (deg)', unit: 'deg', factor: RAD_TO_DEG, step: 0.5 },
    { key: 'cartesianMaxBlendDeviationRad', label: 'Blend (rad)', fullLabel: 'Maximum Cartesian joint-path blend deviation (rad); zero disables blending', unit: 'rad', factor: 1, step: 0.005, min: 0 },
  ] : [];
  const settingControl = ({ key, label, fullLabel, unit, factor, step, min, max }: MotionSettingField) => (
    <label key={key} title={fullLabel}>
      <span>{label}</span>
      <RobotNumberInput label={`Manual ${fullLabel}`} value={settings[key] * factor}
        min={min ?? (key.endsWith('Scale') ? 1 : 0.000001)} max={max} step={step}
        disabled={disabled} limitUnit={unit} hint={fullLabel}
        onCommit={(value) => editMotionSettings({ [key]: value / factor })} />
    </label>
  );
  return (
    <section className="robot-control-motion-settings dcard" aria-label="Manual speed and acceleration">
      <h4 title="These defaults apply to every preview and approval without needing an edit. Percentages independently scale the planned velocity and acceleration within Deployment/model ceilings. The live Velocity bar is separate. TCP angular speed and acceleration limits are supported only in Cartesian mode.">
        Speed &amp; acceleration <span className="inspector-info-button" aria-hidden="true">i</span>
      </h4>
      <div className="robot-control-motion-settings-grid">
        {commonFields.map(settingControl)}
      </div>
      {descriptor.operationalLimit !== null && (
        <p className="muted">
          Applied Deployment maximum: {descriptor.operationalLimit.maximumTcpLinearSpeedMps} m/s.
          {' '}Effective TCP ceiling: {Math.min(settings.tcpLinearSpeedMps,
            descriptor.operationalLimit.maximumTcpLinearSpeedMps)} m/s; the request is unchanged.
        </p>
      )}
      <details className="robot-control-joint-settings">
        <summary title="Cartesian path fidelity, smoothing and optional tighter per-joint limits.">Advanced</summary>
        {advancedFields.length > 0 && <div className="robot-control-advanced-settings-grid">
          {advancedFields.map(settingControl)}
        </div>}
        <h5>Joint limits</h5>
        <div className="robot-control-joint-settings-grid">
          <span>Joint</span><span>Velocity (deg/s)</span><span>Acceleration (deg/s²)</span>
          {descriptor.joints.map((joint) => {
            const ceiling = descriptor.operationalJointLimits.find((limit) => limit.sourceJointName === joint.sourceJointName);
            const velocity = ceiling?.effectiveMaxVelocityRadS ?? joint.maxVelocityRadS;
            const acceleration = ceiling?.effectiveMaxAccelerationRadS2 ?? joint.maxAccelerationRadS2;
            const limits = settings.jointLimits[joint.sourceJointName];
            return <div className="robot-control-joint-settings-row" key={joint.sourceJointName}>
              <span>{joint.sourceJointName}</span>
              <RobotNumberInput label={`Manual ${joint.sourceJointName} velocity (deg/s)`}
                value={degrees(limits?.max_velocity_rad_s ?? velocity)} min={0.000001} limitUnit="deg/s"
                max={degrees(velocity)} step={5} disabled={disabled}
                onCommit={(value) => editMotionSettings({ jointLimits: { [joint.sourceJointName]: { max_velocity_rad_s: Math.min(radians(value), velocity) } } })} />
              <RobotNumberInput label={`Manual ${joint.sourceJointName} acceleration (deg/s²)`}
                value={limits?.max_acceleration_rad_s2 != null ? degrees(limits.max_acceleration_rad_s2)
                  : acceleration === null ? null : degrees(acceleration)} min={0.000001} limitUnit="deg/s²"
                max={acceleration === null ? undefined : degrees(acceleration)} step={10}
                disabled={disabled || acceleration === null}
                onCommit={(value) => editMotionSettings({ jointLimits: { [joint.sourceJointName]: { max_acceleration_rad_s2: Math.min(radians(value), acceleration ?? Infinity) } } })} />
            </div>;
          })}
        </div>
        <button type="button" disabled={disabled || !Object.values(settings.jointLimits).some((limit) =>
          limit.max_velocity_rad_s != null || limit.max_acceleration_rad_s2 != null)}
          onClick={() => editMotionSettings({ jointLimits: Object.fromEntries(Object.keys(settings.jointLimits).map((name) => [
            name, { max_velocity_rad_s: null, max_acceleration_rad_s2: null },
          ])) })}>Use ceilings</button>
      </details>
    </section>
  );
}

export function RobotControlPanelView({
  enabled,
  controller,
  program,
  gizmo: sharedGizmo,
  sourceFrameNames = {},
  controlBasePoseInCell = null,
  runtimeScene = null,
}: {
  enabled: boolean;
  controller: RobotControlController;
  program?: ProgramRuntimeController;
  gizmo?: GizmoController;
  sourceFrameNames?: Readonly<Record<string, string>>;
  controlBasePoseInCell?: PoseTuple | null;
  runtimeScene?: LogicalSceneProjection | null;
}) {
  const localGizmo = useGizmoSettings();
  const cardLayout = useCardPanelLayout();
  const gizmo = sharedGizmo ?? localGizmo;
  const { angularStepDeg, linearStepM, space: tcpTransformSpace } = gizmo.settings;
  const tcpTransformMode = gizmo.settings.mode === 'scale' ? 'translate' : gizmo.settings.mode;
  const [coordinateStep, setCoordinateStep] = useState(0.01);
  const [moveSourceGroup, setMoveSourceGroup] = useState<MoveToSource['group'] | ''>('');
  const [previewInfoOpen, setPreviewInfoOpen] = useState(false);
  const [dismissedPreviewNotice, setDismissedPreviewNotice] = useState<string | null>(null);
  const previewInfoButton = useRef<HTMLButtonElement>(null);
  const previewInfoId = useId();
  const [inputRebase, setInputRebase] = useState(0);
  const [pendingInputs, setPendingInputs] = useState<ReadonlySet<string>>(new Set());
  const setManualPanelVisible = controller.setManualPanelVisible;
  useEffect(() => {
    setManualPanelVisible(true);
    return () => setManualPanelVisible(false);
  }, [setManualPanelVisible]);
  const reportAuthoringPending = controller.reportAuthoringPending;
  const reportPending = useCallback((label: string, dirty: boolean) => {
    reportAuthoringPending(label, dirty);
    setPendingInputs((previous) => {
      if (previous.has(label) === dirty) return previous;
      const next = new Set(previous);
      if (dirty) next.add(label);
      else next.delete(label);
      return next;
    });
  }, [reportAuthoringPending]);
  const {
    state,
    selectMode,
    editJoint,
    editPose,
    editGripper,
    approve,
    revert,
    stop,
    controlPending,
    reset,
    retry,
  } = controller;
  const moveSourceId = state.moveToDraft?.source.id ?? '';
  const moveOptions = state.moveToDraft?.options ?? DEFAULT_MOVE_TO_OPTIONS;
  // All target-specific controls use the selected addressed projection. The
  // controller keeps the complete owner projection separately so changing
  // this selection never creates a second browser-side ownership model.
  const status = controller.selectedStatus;
  // Null for an arm-only component; gripper controls then never render.
  const gripper = status?.directControlGripper ?? null;
  // Null for a component without motion (gripper-only, H1a-IF.2).
  const motion = status?.directControl ?? null;
  const observedMotion = status?.observedMotion ?? null;
  const programActive = program?.cycleState !== null
    && program?.cycleState !== undefined
    && program.cycleState !== 'idle_after_reset';
  const inputResetKey = [status?.ownerEpoch, motion?.programBindingId,
    gripper?.programBindingId,
    status?.retainedBundleId, status?.graphRevision, programActive,
    state.selectedControllableComponentId, state.selectedMotionTargetId, state.selectedGripperTargetId,
    state.mode, inputRebase].join('\u0000');
  const inputContext = useMemo(() => ({ resetKey: inputResetKey, reportPending }), [inputResetKey, reportPending]);
  const operationActive = robotControlHasActiveOperation(state);
  const active = operationActive || !controller.manualControlAllowed;
  const authoringBlocked = active || state.operation?.resident === true;
  const previewNoticeKey = [status?.ownerEpoch, state.generation, state.error, state.previewIssue].join('\u0000');
  const expectedTargetRejection = state.previewIssue != null;
  const previewRejectionLabel = expectedTargetRejection ? 'Target not executable' : 'Preview unavailable';
  const canApprove = controller.manualControlAllowed && !state.poseDragging && pendingInputs.size === 0
    && robotControlPreviewApprovable(state);
  const canRevert = !active && (
    state.phase !== 'idle'
    || state.ghost !== null
    || state.operation !== null
    || pendingInputs.size > 0
  );
  const canStop = operationActive && state.operation !== null;
  // While the robot moves, Stop has the keyboard focus (Enter/Space stops).
  const stopButton = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (canStop) stopButton.current?.focus();
  }, [canStop]);
  const recoveryRequired = manualOperationRequiresRecovery(state.operation);
  // Contract 66: an output with unknown acceptance is released only by a
  // targeted Reset that acknowledges it explicitly.
  const operationUncertainOutputs = useMemo(
    () => uncertainOutputs(state.operation === null ? [] : [state.operation]),
    [state.operation],
  );
  const [approvedUncertain, setApprovedUncertain] = useState<ReadonlySet<string>>(() => new Set());
  const uncertainApproved = allUncertainOutputsApproved(operationUncertainOutputs, approvedUncertain);
  const resetOperationControl = (
    <>
      <UncertainOutputConfirmation outputs={operationUncertainOutputs} approved={approvedUncertain}
        disabled={controlPending !== null}
        onToggle={(key, approved) => setApprovedUncertain((current) => {
          const next = new Set(current);
          if (approved) next.add(key); else next.delete(key);
          return next;
        })} />
      <button
        type="button"
        disabled={!enabled || controller.currentStatus == null || state.operation?.resident !== true
          || !operationStateIsTerminal(state.operation.state) || controlPending !== null || !uncertainApproved}
        aria-label="Reset operation"
        title={uncertainApproved
          ? 'Release this exact terminal direct operation after the owner has certified containment.'
          : 'Confirm each output with unknown acceptance first.'}
        onClick={() => {
          void reset(operationUncertainOutputs.filter((output) => approvedUncertain.has(uncertainOutputKey(output))));
        }}
      >{controlPending === 'reset' ? 'Sending Reset' : 'Reset operation'}</button>
    </>
  );
  const presentedPose = robotControlHasLocalDraft(state) || observedMotion === null
    ? state.draftPose : observedMotion.currentTcpPose;
  const rpy = rpyDegreesFromQuaternion(presentedPose.orientation);
  const applied = program?.state.applied ?? null;
  const appliedBinding = applied?.programBinding ?? null;
  const appliedProgram = applied?.graphRevision === status?.graphRevision
    && applied?.retainedBundleId === status?.retainedBundleId ? applied?.program : null;
  const moveSources = useMemo<MoveToSource[]>(() => {
    if (status === null || motion === null || observedMotion === null) return [];
    const planningCapabilities = motion.targetDescriptor.contracts
      .supportingProviders.map((provider) => provider.capabilityId);
    const poseAvailable = planningCapabilities.includes('planning.motion.pose_goal');
    const jointAvailable = planningCapabilities.includes('planning.motion.joint_goal');
    const sources: MoveToSource[] = [];
    const boundTargetRoles = new Set(boundProgramTargetRoleIds(
      appliedBinding,
      motion.controllableComponentId,
      motion.commandTargetId,
    ));
    const frameRolePoses = new Map<string, PoseTuple>([
      [motion.baseFrameRoleId, {
        position: [0, 0, 0],
        quaternion: [0, 0, 0, 1],
      }],
      [motion.tcpFrameRoleId, observedPose(
        observedMotion.currentTcpPose,
      )],
    ]);
    const sceneResourcePoses = new Map<string, PoseTuple>();
    if (controlBasePoseInCell !== null) {
      for (const resource of runtimeScene?.snapshot?.objects ?? []) {
        // Never fall back to authored poses, including for carried objects.
        // Only the owner-published cell-fixed pose is available here.
        if (runtimeScene?.access.projection_leases_open
          && resource.placement.kind === 'cell_fixed') {
          const pose = resource.placement.pose_in_cell;
          const cellPose: PoseTuple = {
            position: [pose.position_m.x, pose.position_m.y, pose.position_m.z],
            quaternion: pose.orientation_xyzw,
          };
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
        label: `Current ${motion.commandTargetId} TCP`,
        kind: 'pose',
        group: 'current',
        pose: observedPose(observedMotion.currentTcpPose),
      });
      for (const frame of motion.targetDescriptor.frames) {
        const pose = frame.frameRoleId === motion.baseFrameRoleId
          ? {
              position: [0, 0, 0] as [number, number, number],
              quaternion: [0, 0, 0, 1] as [number, number, number, number],
            }
          : frame.frameRoleId === motion.tcpFrameRoleId
            ? observedPose(observedMotion.currentTcpPose)
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
    for (const { node } of flattenProgramForest(appliedProgram ? [appliedProgram.tree] : [])) {
      const programTargetRole = node.parameters.target_role;
      if (typeof programTargetRole !== 'string'
        || !boundTargetRoles.has(programTargetRole)) continue;
      if (poseAvailable && node.node_id === 'motion.move_to_pose') {
        const pose = resolveStoredProgramPose(node.parameters, {
          programTargetRoleId: programTargetRole,
          baseFrameRoleId: motion.baseFrameRoleId,
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
          || !observedMotion.complete
          || observedMotion.positionsRad.length
            !== motion.targetDescriptor.joints.length
        ) continue;
        const positions = [...observedMotion.positionsRad];
        const inventory = new Map(motion.targetDescriptor.joints.map(
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
    appliedProgram,
    runtimeScene,
    controlBasePoseInCell,
    motion,
    observedMotion,
    sourceFrameNames,
    status,
  ]);

  const applyMoveSource = (sourceId: string) => {
    const source = moveSources.find((row) => row.id === sourceId);
    if (source === undefined) {
      revert();
      setInputRebase((value) => value + 1);
      return;
    }
    if (source?.kind === 'joint' && source.positionsRad !== undefined) {
      controller.selectMoveToSource({ id: source.id, kind: 'joint', positionsRad: source.positionsRad });
      return;
    }
    if (source?.pose === undefined || status === null) return;
    controller.selectMoveToSource({ id: source.id, kind: 'pose', pose: source.pose });
  };
  const availableSourceGroups = MOVE_SOURCE_GROUPS.filter(([group]) => (
    moveSources.some((source) => source.group === group)
  ));
  const selectedMoveSource = moveSources.find((row) => row.id === moveSourceId);
  // The retained source owns its category too. Closing/reopening the panel
  // must not discard a valid controller draft because local UI state reset.
  const selectedSourceGroup = selectedMoveSource?.group
    ?? (availableSourceGroups.some(([group]) => group === moveSourceGroup) ? moveSourceGroup : '');
  const selectedMoveSourceKey = selectedMoveSource === undefined ? null
    : JSON.stringify([selectedMoveSource.id, selectedMoveSource.kind,
      selectedMoveSource.positionsRad ?? selectedMoveSource.pose]);
  const capturedSource = state.moveToDraft?.source;
  const moveSourceCapture = capturedSource === undefined ? null : JSON.stringify([
    capturedSource.id, capturedSource.kind, capturedSource.kind === 'pose' ? capturedSource.pose : capturedSource.positionsRad,
  ]);
  const moveSourceCurrent = moveSourceCapture === null
    || moveSourceCapture === selectedMoveSourceKey;
  useEffect(() => {
    if (state.mode !== 'move_to' || authoringBlocked || state.phase === 'approving'
      || moveSourceCurrent) return;
    setInputRebase((value) => value + 1);
    revert();
  }, [authoringBlocked, moveSourceCurrent, revert, state.mode, state.phase]);

  const modeAvailable = (mode: RobotControlMode): boolean => {
    if (status === null) return false;
    if (mode === 'gripper') return gripper !== null;
    // Motion modes need a selected motion target (none for gripper-only).
    if (motion === null) return false;
    if (mode === 'move_to') {
      return motion.targetDescriptor.contracts.supportingProviders.some((provider) => (
        provider.capabilityId === 'planning.motion.pose_goal'
        || provider.capabilityId === 'planning.motion.joint_goal'
      ));
    }
    const capability = mode === 'joint'
      ? 'planning.motion.joint_goal'
      : `planning.motion.${mode}`;
    return motion.targetDescriptor.contracts.supportingProviders
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

  const displayedTarget = state.mode === 'gripper' && gripper !== null
    ? gripper
    : motion;
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
  const selectedTools = controller.gripperTargets.filter((target) => (
    status !== null
      && target.controllableComponentId
        === (motion ?? gripper)?.controllableComponentId
  ));
  const targetLabel = (target: { commandTargetId: string; controllableComponentId: string }) =>
    `${target.controllableComponentId} · ${target.commandTargetId}`;

  return (
    <RobotInputContext.Provider value={inputContext}>
    <section
      ref={cardLayout}
      className="panel robot-control-panel"
      aria-label="Robot Control"
      data-testid="robot-control-panel"
      title={displayedTarget === undefined || displayedTarget === null
        ? undefined
        : `Control route: ${displayedTarget.controllableComponentId} / ${displayedTarget.commandTargetId}`}
    >
      {controller.unappliedChanges && (
        <p className="central-inspector-notice notice severity-warning notice-strip" role="status">
          Unapplied changes are not used for motion. Robots use the current runtime Scene and Applied Deployment.
        </p>
      )}
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

      {enabled && status !== null && displayedTarget !== undefined && displayedTarget !== null && (
        <>
          {programActive && !controller.manualControlAllowed && !state.operation?.resident && (
            <p className="central-inspector-notice notice severity-info notice-strip" role="status">
              <strong>Manual control unavailable.</strong>{' '}
              {controller.manualControlUnavailableReason}
              {' '}<button type="button" onClick={retry}>Check again</button>
            </p>
          )}
          {!state.poseDragging && state.phase === 'blocked' && dismissedPreviewNotice !== previewNoticeKey && (
            <div
              className="central-inspector-notice notice severity-warning notice-floating robot-control-preview-notice"
              role={expectedTargetRejection ? 'status' : 'alert'}
            >
              <div>
                <strong>{previewRejectionLabel}.</strong>{' '}
                {state.error ?? state.previewIssue ?? 'The execution owner rejected this target.'}
                {!expectedTargetRejection && (
                  <button type="button" disabled={authoringBlocked} onClick={retry}>Retry preview</button>
                )}
              </div>
              <button type="button" className="robot-control-notice-dismiss"
                aria-label="Hide preview notice"
                title="Hide this notice; preview details and disabled Approve remain."
                onClick={() => {
                  setDismissedPreviewNotice(previewNoticeKey);
                  previewInfoButton.current?.focus();
                }}>×</button>
            </div>
          )}
          {controller.motionTargets.length > 0 && (
            <div className="robot-control-target-selectors" aria-label="Direct control target">
              {controller.motionTargets.length > 1 && (
                <label>
                  <span>Robot</span>
                  <select
                    aria-label="Robot target"
                    value={`${state.selectedControllableComponentId ?? ''}\u0000${state.selectedMotionTargetId ?? ''}`}
                    disabled={state.phase === 'approving'}
                    onChange={(event) => {
                      const [controllableComponentId, motionTargetId] = event.currentTarget.value.split('\u0000');
                      if (controllableComponentId !== undefined && motionTargetId !== undefined) {
                        controller.selectRobot(controllableComponentId, motionTargetId);
                      }
                    }}
                  >
                    {controller.motionTargets.map((target) => (
                      <option
                        key={`${target.controllableComponentId}\u0000${target.commandTargetId}`}
                        value={`${target.controllableComponentId}\u0000${target.commandTargetId}`}
                      >{targetLabel(target)}</option>
                    ))}
                  </select>
                </label>
              )}
              {controller.motionTargets.length === 1 && (
                <span className="robot-control-target-identity" title="The only addressed robot currently offered by the execution owner.">
                  <span>Robot</span>
                  <code>{(motion ?? gripper) === null ? '' : targetLabel((motion ?? gripper)!)}</code>
                </span>
              )}
              {state.mode === 'gripper' && selectedTools.length > 1 && (
                <label>
                  <span>Tool</span>
                  <select
                    aria-label="Gripper target"
                    value={state.selectedGripperTargetId ?? ''}
                    disabled={state.phase === 'approving'}
                    onChange={(event) => controller.selectTool(event.currentTarget.value)}
                  >
                    {selectedTools.map((target) => (
                      <option
                        key={`${target.controllableComponentId}\u0000${target.commandTargetId}`}
                        value={target.commandTargetId}
                      >{targetLabel(target)}</option>
                    ))}
                  </select>
                </label>
              )}
              {state.mode === 'gripper' && selectedTools.length === 1 && (
                <span className="robot-control-target-identity" title="The only qualified attached tool currently offered for this robot.">
                  <span>Tool</span>
                  <code>{targetLabel(selectedTools[0])}</code>
                </span>
              )}
              {state.mode === 'gripper' && selectedTools.length === 0 && (
                <span className="robot-control-target-identity robot-control-target-unavailable" role="status">
                  <span>Tool</span>
                  <span>No qualified attached tool</span>
                </span>
              )}
            </div>
          )}
          <nav className="robot-control-modes dseg" aria-label="Manual control mode">
            {MODE_LABELS.map(([mode, label]) => (
              <button
                key={mode}
                type="button"
                title={mode === 'cartesian_path' ? 'Cartesian path: move the TCP along a straight path' : label}
                aria-pressed={state.mode === mode}
                disabled={operationActive || !modeAvailable(mode)}
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
                options={ANGULAR_STEP_OPTIONS}
                value={angularStepDeg}
                disabled={authoringBlocked}
                onChange={(angularStepDeg) => gizmo.update({ angularStepDeg })}
              />
            )}
            {(state.mode === 'pose_goal'
              || (state.mode === 'move_to' && selectedMoveSource?.kind !== 'joint')
              || state.mode === 'cartesian_path') && (
              <GizmoSteps gizmo={gizmo} disabled={authoringBlocked || state.poseDragging} />
            )}
            {state.mode === 'gripper' && gripper !== null && (
              <StepSelect
                label="Step"
                unit={gripper.targetDescriptor.unit}
                options={COORDINATE_STEP_OPTIONS}
                value={coordinateStep}
                disabled={authoringBlocked}
                onChange={setCoordinateStep}
              />
            )}
            {(state.mode === 'pose_goal' || state.mode === 'cartesian_path'
              || (state.mode === 'move_to' && selectedMoveSource?.kind === 'pose')) && (
              <div className="robot-control-tcp-tools" aria-label="TCP transform controls">
                <span className="robot-control-target-frame">
                  <span>Target frame</span>{' '}
                  <code
                    aria-label="TCP target frame"
                    title="The current direct-control contract permits this logical base frame."
                  >
                    {motion?.baseFrameRoleId}
                  </code>
                </span>
                <div className="robot-control-tool-group">
                <span>Gizmo</span>
                <span className="robot-control-segment" role="group" aria-label="TCP gizmo mode">
                  <button
                    type="button"
                    aria-pressed={tcpTransformMode === 'translate'}
                    disabled={authoringBlocked}
                    onClick={() => gizmo.update({ mode: 'translate' })}
                  >Translate</button>
                  <button
                    type="button"
                    aria-pressed={tcpTransformMode === 'rotate'}
                    disabled={authoringBlocked}
                    onClick={() => gizmo.update({ mode: 'rotate' })}
                  >Rotate</button>
                </span>
                </div>
                <div className="robot-control-tool-group">
                <span>Axes</span>
                <span className="robot-control-segment" role="group" aria-label="TCP gizmo coordinates">
                  <button
                    type="button"
                    aria-pressed={tcpTransformSpace === 'local'}
                    disabled={authoringBlocked}
                    onClick={() => gizmo.update({ space: 'local' })}
                  >Local</button>
                  <button
                    type="button"
                    aria-pressed={tcpTransformSpace === 'world'}
                    disabled={authoringBlocked}
                    onClick={() => gizmo.update({ space: 'world' })}
                  >World</button>
                </span>
                </div>
              </div>
            )}
          </div>

          <div className="robot-control-body">
            <div
              className="robot-control-editor"
              data-testid="robot-control-editor-scroll"
            >
              {state.mode !== 'gripper' && motion !== null && <ManualMotionSettingsControls controller={controller} disabled={authoringBlocked} />}
              {state.mode === 'joint' && motion !== null && observedMotion !== null && (
                <section className="dcard" aria-label="Joint targets">
                  <h4>Joints</h4>
                  {motion.targetDescriptor.joints.map((joint, index) => {
                    const live = observedMotion.positionsRad[index] ?? 0;
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
                </section>
              )}

              {(state.mode === 'pose_goal'
                || state.mode === 'move_to'
                || state.mode === 'cartesian_path') && (
                <div className="robot-control-pose-grid">
                  {state.mode === 'move_to' && (
                    <fieldset className="robot-control-move-to">
                      <legend>Move To source</legend>
                      <p className="robot-control-limits">
                        Choose a target type and target, adjust offsets if needed, then check the preview and Approve.
                      </p>
                      <label>
                        <span>Target type</span>
                        <select
                          aria-label="Move To target type"
                          value={selectedSourceGroup}
                          disabled={authoringBlocked || availableSourceGroups.length === 0}
                          onChange={(event) => {
                            setMoveSourceGroup(event.currentTarget.value as MoveToSource['group'] | '');
                            applyMoveSource('');
                          }}
                        >
                          <option value="">{availableSourceGroups.length === 0
                            ? 'No targets available' : 'Choose a type'}</option>
                          {availableSourceGroups.map(([group, label]) => (
                            <option key={group} value={group}>{label}</option>
                          ))}
                        </select>
                      </label>
                      <label>
                        <span>Target</span>
                        <select
                          aria-label="Move To target"
                          value={selectedMoveSource === undefined ? '' : moveSourceId}
                          disabled={authoringBlocked || selectedSourceGroup === ''}
                          onChange={(event) => {
                            const sourceId = event.currentTarget.value;
                            applyMoveSource(sourceId);
                          }}
                        >
                          <option value="">{selectedSourceGroup === ''
                            ? 'Choose a type first' : 'Choose a target'}</option>
                          {moveSources.filter((source) => source.group === selectedSourceGroup)
                            .map((source) => (
                              <option key={source.id} value={source.id}>{source.label}</option>
                            ))}
                        </select>
                      </label>
                      {selectedMoveSource?.kind === 'pose' && (
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
                                controller.editMoveToOptions(next);
                              }}
                            >
                              <option value="keep-current">Keep current TCP</option>
                              <option value="use-entity">Use source orientation</option>
                              <option value="face-down">Face down</option>
                            </select>
                          </label>
                          <details className="robot-control-offset-details">
                          <summary>Offsets in source frame</summary>
                          <div className="robot-control-move-offsets">
                        {(['X', 'Y', 'Z'] as const).map((axis, index) => (
                          <label key={axis}>
                            <span>{axis} (m)</span>
                            <RobotNumberInput
                              label={`${axis} local offset metres`}
                              step={linearStepM}
                              value={moveOptions.offsetXyzM[index]}
                              disabled={authoringBlocked || !moveSourceId}
                              onCommit={(value) => {
                                const offsetXyzM = [...moveOptions.offsetXyzM] as [number, number, number];
                                offsetXyzM[index] = value;
                                const next = { ...moveOptions, offsetXyzM };
                                controller.editMoveToOptions(next);
                              }}
                            />
                          </label>
                        ))}
                        {(['Roll', 'Pitch', 'Yaw'] as const).map((axis, index) => (
                          <label key={axis}>
                            <span>{axis === 'Roll' ? 'R' : axis === 'Pitch' ? 'P' : axis} (deg)</span>
                            <RobotNumberInput
                              label={`${axis} local offset degrees`}
                              step={angularStepDeg}
                              value={degrees(moveOptions.offsetRpyRad[index])}
                              disabled={authoringBlocked || !moveSourceId}
                              onCommit={(value) => {
                                const offsetRpyRad = [...moveOptions.offsetRpyRad] as [number, number, number];
                                offsetRpyRad[index] = radians(value);
                                const next = { ...moveOptions, offsetRpyRad };
                                controller.editMoveToOptions(next);
                              }}
                            />
                          </label>
                        ))}
                          </div>
                          </details>
                        </>
                      )}
                      {selectedMoveSource !== undefined && (
                        <details className="robot-control-resolved-target">
                          <summary>Resolved target · {motion?.baseFrameRoleId}</summary>
                          {selectedMoveSource.kind === 'pose' ? <>
                            <dl>
                              <div><dt>Position (m)</dt><dd>{Object.values(presentedPose.position_m).map((v) => v.toFixed(4)).join(', ')}</dd></div>
                              <div><dt>Roll / pitch / yaw (deg)</dt><dd>{rpy.map((v) => v.toFixed(2)).join(', ')}</dd></div>
                            </dl>
                            <button type="button" disabled={authoringBlocked} onClick={() => {
                              selectMode('pose_goal');
                              editPose(presentedPose);
                            }}>Edit target as pose</button>
                          </> : <dl>
                            {(motion?.targetDescriptor.joints ?? []).map((joint, index) => (
                              <div key={joint.sourceJointName}><dt>{joint.sourceJointName}</dt><dd>{formatDegrees(state.draftPositionsRad[index])}°</dd></div>
                            ))}
                          </dl>}
                        </details>
                      )}
                    </fieldset>
                  )}
                  {state.mode !== 'move_to' && (<>
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
                  <p className="robot-control-limits robot-control-pose-note hint">
                    Absolute TCP target in{' '}
                    <code>{(() => {
                      if (motion === null) return '';
                      const base = motion.targetDescriptor.frames.find(
                        (frame) => frame.frameRoleId === motion.baseFrameRoleId,
                      );
                      return base === undefined
                        ? motion.baseFrameRoleId
                        : `${base.frameRoleId} · ${
                            sourceFrameNames[base.compiledFrameName] ?? 'source name unavailable'
                          }`;
                    })()}</code>.{' '}
                    {state.mode === 'cartesian_path'
                      ? 'The current owner-issued TCP pose is the explicit first waypoint.'
                      : 'Pose planning chooses a valid joint-space realization.'}
                  </p>
                  </>)}
                </div>
              )}

              {state.mode === 'gripper' && gripper !== null && (
                <div className="robot-control-gripper-editor">
                <p className="robot-control-pose-note hint">
                  {gripper.targetDescriptor.actuationKind === 'pneumatic'
                    ? 'Completion covers the command transaction, not grasp verification.'
                    : 'Completion confirms the observed coordinate, not an object grasp.'}
                </p>
                {gripper.targetDescriptor.namedPresets.length > 0 && (
                  <label className="robot-control-preset">
                    <span>Named preset</span>
                    <select
                      defaultValue=""
                      disabled={authoringBlocked}
                      onChange={(event) => {
                        const preset = gripper.targetDescriptor
                          .namedPresets.find((candidate) => candidate.name === event.target.value);
                        if (preset) editGripper(preset.masterPosition);
                        event.target.value = '';
                      }}
                    >
                      <option value="">Choose retained preset…</option>
                      {gripper.targetDescriptor.namedPresets.map((preset) => (
                        <option key={preset.name} value={preset.name}>{preset.name}</option>
                      ))}
                    </select>
                  </label>
                )}
                <label className="robot-control-joint robot-control-gripper">
                  <span className="robot-control-joint-heading">
                    <strong>
                      {gripper.targetDescriptor.sourceCoordinateName}
                    </strong>
                    <span className="robot-control-limits">
                      {gripper.targetDescriptor.minPosition === null
                        || gripper.targetDescriptor.maxPosition === null
                        ? 'Enter the initial target'
                        : `${gripper.targetDescriptor.minPosition.toFixed(4)} to ${
                          gripper.targetDescriptor.maxPosition.toFixed(4)
                        } ${gripper.targetDescriptor.unit}`}
                    </span>
                  </span>
                  <span className="robot-control-joint-inputs">
                    {gripper.targetDescriptor.minPosition !== null
                      && gripper.targetDescriptor.maxPosition !== null && (
                      <input
                        aria-label="Gripper target"
                        type="range"
                        min={gripper.targetDescriptor.minPosition}
                        max={gripper.targetDescriptor.maxPosition}
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
                        min={gripper.targetDescriptor.minPosition ?? undefined}
                        max={gripper.targetDescriptor.maxPosition ?? undefined}
                        step={coordinateStep}
                        value={presentedGripperPosition}
                        disabled={authoringBlocked}
                        pending={gripperTargetPending}
                        onCommit={editGripper}
                      />
                      <span>
                        {gripper.targetDescriptor.unit}
                      </span>
                    </span>
                  </span>
                </label>
                </div>
              )}
            </div>

            <aside className="robot-control-preview" aria-live="polite">
              <div className="robot-control-preview-heading">
                <h3>{state.operation !== null ? 'Operation' : 'Preview'}</h3>
                <div className="robot-control-preview-actions">
                  {state.poseDragging && <span className="status-pill">Editing target</span>}
                  {!state.poseDragging && state.phase !== 'blocked'
                    && state.phase !== 'idle'
                    && state.phase !== 'ready' && (
                    <span className={`status-pill robot-control-phase phase-${state.phase}`}>
                      {state.operation?.state.replaceAll('_', ' ') ?? phaseLabel(state.phase)}
                    </span>
                  )}
                  {!state.poseDragging && (state.phase === 'blocked' ? (
                    <span className="status-pill status-pill-danger"
                      title={state.error ?? state.previewIssue ?? undefined}>
                      {expectedTargetRejection ? 'Not executable' : 'Unavailable'}
                    </span>
                  ) : state.ghostStale && state.ghost !== null && (
                    <span className="status-pill status-pill-danger">stale</span>
                  ))}
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

              {state.error !== null && state.phase !== 'blocked' && (
                <p className="robot-control-error notice severity-error" role="alert">{state.error}</p>
              )}
              {state.ownerNotice !== null && (
                <p className="notice severity-info">{state.ownerNotice}</p>
              )}
              {recoveryRequired && (
                <p className="robot-control-recovery-guidance">
                  {controller.currentStatus == null ? <>Robot status unavailable.
                    Last-known recovery status is not current; waiting for the execution owner.</> : <>
                  <strong>Reset operation required.</strong> This manual operation still blocks
                  new execution and editing. Use <strong>Reset operation</strong> below to
                  request release. History acknowledgement does not release it.
                  </>}
                </p>
              )}

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

              {state.poseDragging && <p className="robot-control-tcp">Unvalidated target. Release to validate.</p>}
              {!state.poseDragging && state.phase === 'blocked' && (
                <p className="robot-control-tcp">Approve is disabled. Check the preview details.</p>
              )}
              {!state.poseDragging && state.ghost !== null && state.phase === 'ready' && (
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
                  <OperationProgress operation={state.operation} ownerEpoch={status?.ownerEpoch} source={controller.controlSource} />
                  {state.operation.resident && !recoveryRequired && <div className="robot-control-operation-actions">
                    {resetOperationControl}
                  </div>}
                </div>
              )}

            </aside>
          </div>

          <footer
            className="robot-control-footer"
            data-testid="robot-control-candidate-footer"
          >
            <div className="robot-control-actions">
              {recoveryRequired ? resetOperationControl : <>
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
                ref={stopButton}
                className="stop-button"
                type="button"
                disabled={!canStop}
                title="Stop the exact active operation; this does not merely discard its preview."
                onClick={() => void stop()}
              >
                Stop operation
              </button>
              </>}
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
            <p>Target coordinates are relative to <code>{motion?.baseFrameRoleId}</code>. Local/World selects gizmo axes, not the target reference frame.</p>
            <p>A red tool marks an unvalidated TCP target, not a solved robot configuration or proof of collision. Preview rejection does not mean the Program or robot has faulted.</p>
            <p>Joint velocity/acceleration caps and the TCP speed ceiling are configured in Deployment → Robot model and target limits. The runtime Velocity slider is separate from those limits.</p>
            {state.mode === 'cartesian_path' && <p>Cartesian preview currently requests 0.1 m/s linear speed, 0.5 m/s² linear acceleration, 0.5 rad/s angular speed and 2.5 rad/s² angular acceleration, subject to the backend limits. Per-move editing of these values is not yet available.</p>}
            {state.error !== null && <p className="robot-control-error notice severity-error">{state.error}</p>}
            {state.previewIssue != null && <p>{state.previewIssue}</p>}
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
                bindingId: state.mode === 'gripper' && gripper !== null ? gripper.programBindingId : motion?.programBindingId,
                target: state.mode === 'gripper' && gripper !== null ? gripper.targetDescriptor : motion?.targetDescriptor,
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
