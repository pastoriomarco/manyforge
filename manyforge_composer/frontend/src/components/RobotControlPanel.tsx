import { useState } from 'react';

import { useRobotControl } from '../hooks/useRobotControl';
import type { RobotControlController } from '../hooks/useRobotControl';
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
  ['cartesian_path', 'Cartesian'],
  ['gripper', 'Gripper'],
];

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
}: {
  enabled: boolean;
  controller: RobotControlController;
}) {
  const [angularStepDeg, setAngularStepDeg] = useState(5);
  const [linearStepM, setLinearStepM] = useState(0.01);
  const [coordinateStep, setCoordinateStep] = useState(0.01);
  const {
    state,
    selectMode,
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
  const rpy = rpyDegreesFromQuaternion(state.draftPose.orientation);

  const modeAvailable = (mode: RobotControlMode): boolean => {
    if (status === null || mode === 'gripper') return status !== null;
    const capability = mode === 'joint'
      ? 'planning.motion.joint_goal'
      : `planning.motion.${mode}`;
    return status.directControl.planningCapabilityIds.includes(capability);
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
            {state.mode === 'joint' && (
              <StepSelect
                label="Step"
                unit="deg"
                options={ANGULAR_STEP_DEG_OPTIONS}
                value={angularStepDeg}
                disabled={active}
                onChange={setAngularStepDeg}
              />
            )}
            {(state.mode === 'pose_goal' || state.mode === 'cartesian_path') && (
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
                unit={status.directControlGripper.coordinateKind === 'angular_rad'
                  ? 'rad'
                  : 'm'}
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
                  {status.directControl.joints.map((joint, index) => {
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

              {(state.mode === 'pose_goal' || state.mode === 'cartesian_path') && (
                <div className="robot-control-pose-grid">
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
                    <code>{status.directControl.baseFrameRoleId}</code>.{' '}
                    {state.mode === 'cartesian_path'
                      ? 'The current owner-issued TCP pose is the explicit first waypoint.'
                      : 'Pose planning chooses a valid joint-space realization.'}
                  </p>
                </div>
              )}

              {state.mode === 'gripper' && (
                <label className="robot-control-joint robot-control-gripper">
                  <span className="robot-control-joint-heading">
                    <strong>
                      {status.directControlGripper.sourceCoordinateName}
                    </strong>
                    <span className="robot-control-limits">
                      {status.directControlGripper.minPosition === null
                        || status.directControlGripper.maxPosition === null
                        ? 'Enter the initial target'
                        : `${status.directControlGripper.minPosition.toFixed(4)} to ${
                          status.directControlGripper.maxPosition.toFixed(4)
                        } ${status.directControlGripper.coordinateKind === 'angular_rad'
                          ? 'rad'
                          : 'm'}`}
                    </span>
                  </span>
                  <span className="robot-control-joint-inputs">
                    {status.directControlGripper.minPosition !== null
                      && status.directControlGripper.maxPosition !== null && (
                      <input
                        aria-label="Gripper target"
                        type="range"
                        min={status.directControlGripper.minPosition}
                        max={status.directControlGripper.maxPosition}
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
                        min={status.directControlGripper.minPosition ?? undefined}
                        max={status.directControlGripper.maxPosition ?? undefined}
                        step={coordinateStep}
                        value={presentedGripperPosition}
                        disabled={active}
                        pending={gripperTargetPending}
                        onCommit={editGripper}
                      />
                      <span>
                        {status.directControlGripper.coordinateKind === 'angular_rad'
                          ? 'rad'
                          : 'm'}
                      </span>
                    </span>
                  </span>
                </label>
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
