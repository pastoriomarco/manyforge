import { useRobotControl } from '../hooks/useRobotControl';
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

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;

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

export function RobotControlPanel({ enabled }: { enabled: boolean }) {
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
  } = useRobotControl(enabled);
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

  return (
    <section
      className="panel robot-control-panel"
      aria-label="Robot Control"
      data-testid="robot-control-panel"
    >
      <header className="panel-header robot-control-header">
        <div>
          <p className="eyebrow">Addressed operation</p>
          <h2>Robot Control</h2>
        </div>
        <span className={`status-pill robot-control-phase phase-${state.phase}`}>
          {phaseLabel(state.phase)}
        </span>
      </header>

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
          <div className="robot-control-identity">
            <span>
              component <code>{displayedTarget.controllableComponentId}</code>
            </span>
            <span>
              target <code>{displayedTarget.commandTargetId}</code>
            </span>
            <span>
              source epoch <code>{state.mode === 'gripper'
                ? status.observedGripper.sourceEpoch
                : status.observedMotion.sourceEpoch}</code>
            </span>
          </div>

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

          <div className="robot-control-body">
            <div
              className="robot-control-editor"
              data-testid="robot-control-editor-scroll"
            >
              <p className="panel-intro">
                Edits validate a no-motion preview after 150 ms. Only Approve
                submits the exact target to the execution owner.
              </p>

              {state.mode === 'joint' && (
                <div className="robot-control-joints">
                  {status.directControl.joints.map((joint, index) => {
                    const live = status.observedMotion.positionsRad[index] ?? 0;
                    const draft = state.draftPositionsRad[index] ?? live;
                    return (
                      <label
                        className="robot-control-joint"
                        key={joint.sourceJointName}
                      >
                        <span className="robot-control-joint-heading">
                          <strong>{joint.sourceJointName}</strong>
                          <span>observed {formatDegrees(live)}°</span>
                        </span>
                        <span className="robot-control-joint-inputs">
                          <input
                            aria-label={`${joint.sourceJointName} target`}
                            type="range"
                            min={degrees(joint.minPositionRad)}
                            max={degrees(joint.maxPositionRad)}
                            step="any"
                            value={degrees(draft)}
                            disabled={active}
                            onChange={(event) => {
                              const value = event.currentTarget.valueAsNumber;
                              if (Number.isFinite(value)) {
                                editJoint(index, radians(value));
                              }
                            }}
                          />
                          <span className="robot-control-number">
                            <input
                              aria-label={`${joint.sourceJointName} target degrees`}
                              type="number"
                              min={degrees(joint.minPositionRad)}
                              max={degrees(joint.maxPositionRad)}
                              step={0.1}
                              value={degrees(draft)}
                              disabled={active}
                              onChange={(event) => {
                                const value = event.currentTarget.valueAsNumber;
                                if (Number.isFinite(value)) {
                                  editJoint(index, radians(value));
                                }
                              }}
                            />
                            <span>deg</span>
                          </span>
                        </span>
                        <span className="robot-control-limits">
                          {formatDegrees(joint.minPositionRad)}° to{' '}
                          {formatDegrees(joint.maxPositionRad)}° · max{' '}
                          {formatDegrees(joint.maxVelocityRadS)}°/s
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
                        <input
                          aria-label={`${field.slice(0, 1)} target metres`}
                          type="number"
                          step={0.001}
                          value={state.draftPose.position_m[field]}
                          disabled={active}
                          onChange={(event) => editPosePosition(
                            field,
                            event.currentTarget.valueAsNumber,
                          )}
                        />
                        <span>m</span>
                      </span>
                    </label>
                  ))}
                  {['Roll', 'Pitch', 'Yaw'].map((label, index) => (
                    <label key={label}>
                      <span>{label}</span>
                      <span className="robot-control-number">
                        <input
                          aria-label={`${label} target degrees`}
                          type="number"
                          step={0.1}
                          value={rpy[index]}
                          disabled={active}
                          onChange={(event) => editPoseOrientation(
                            index,
                            event.currentTarget.valueAsNumber,
                          )}
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
                    <span>
                      observed {status.observedGripper.position.toFixed(4)}{' '}
                      {status.directControlGripper.coordinateKind === 'angular_rad'
                        ? 'rad'
                        : 'm'}
                    </span>
                  </span>
                  <span className="robot-control-joint-inputs">
                    <input
                      aria-label="Gripper target"
                      type="range"
                      min={status.directControlGripper.minPosition}
                      max={status.directControlGripper.maxPosition}
                      step="any"
                      value={state.draftGripperPosition}
                      disabled={active}
                      onChange={(event) => {
                        const value = event.currentTarget.valueAsNumber;
                        if (Number.isFinite(value)) editGripper(value);
                      }}
                    />
                    <span className="robot-control-number">
                      <input
                        aria-label="Gripper target value"
                        type="number"
                        min={status.directControlGripper.minPosition}
                        max={status.directControlGripper.maxPosition}
                        step="any"
                        value={state.draftGripperPosition}
                        disabled={active}
                        onChange={(event) => {
                          const value = event.currentTarget.valueAsNumber;
                          if (Number.isFinite(value)) editGripper(value);
                        }}
                      />
                      <span>
                        {status.directControlGripper.coordinateKind === 'angular_rad'
                          ? 'rad'
                          : 'm'}
                      </span>
                    </span>
                  </span>
                  <span className="robot-control-limits">
                    {status.directControlGripper.minPosition.toFixed(4)} to{' '}
                    {status.directControlGripper.maxPosition.toFixed(4)}{' '}
                    {status.directControlGripper.coordinateKind === 'angular_rad'
                      ? 'rad'
                      : 'm'}
                  </span>
                </label>
              )}
            </div>

            <aside className="robot-control-preview" aria-live="polite">
              <div className="robot-control-preview-heading">
                <div>
                  <p className="eyebrow">No-effect result</p>
                  <h3>{state.mode === 'gripper'
                    ? 'Validated command'
                    : 'Predicted ghost'}</h3>
                </div>
                {state.ghostStale && state.ghost !== null && (
                  <span className="status-pill status-pill-danger">stale</span>
                )}
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

              {state.ghost === null && (
                <p className="muted">
                  Edit a target to validate it without moving the robot.
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

              {state.error !== null && (
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
                onClick={() => void approve()}
              >
                Approve
              </button>
              <button type="button" disabled={!canRevert} onClick={revert}>
                Revert
              </button>
              <button
                className="stop-button"
                type="button"
                disabled={!canStop}
                onClick={() => void stop()}
              >
                Stop operation
              </button>
            </div>
            <p className="robot-control-boundary">
              Revert only discards the local preview. It never cancels an
              executing operation; Stop addresses that exact operation.
            </p>
          </footer>
        </>
      )}
    </section>
  );
}
