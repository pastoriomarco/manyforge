import { useRobotControl } from '../hooks/useRobotControl';
import { robotControlHasActiveOperation } from '../state/robotControl';
import type { RobotControlPhase } from '../state/robotControl';

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

export function RobotControlPanel({ enabled }: { enabled: boolean }) {
  const { state, editJoint, approve, revert, stop, retry } = useRobotControl(
    enabled,
  );
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

  return (
    <section className="panel robot-control-panel" aria-label="Robot Control">
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
          <span>Joint authoring remains unavailable in structural-only mode.</span>
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

      {enabled && status !== null && (
        <>
          <div className="robot-control-identity">
            <span>
              component <code>{status.directControl.controllableComponentId}</code>
            </span>
            <span>
              target <code>{status.directControl.commandTargetId}</code>
            </span>
            <span>
              source epoch <code>{status.observedMotion.sourceEpoch}</code>
            </span>
          </div>

          <div className="robot-control-body">
            <div className="robot-control-joints">
              <p className="panel-intro">
                Edits validate a no-motion preview after 150 ms. Only Approve
                submits the exact target to the execution owner.
              </p>
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

            <aside className="robot-control-preview" aria-live="polite">
              <div className="robot-control-preview-heading">
                <div>
                  <p className="eyebrow">No-motion result</p>
                  <h3>Predicted ghost</h3>
                </div>
                {state.ghostStale && state.ghost !== null && (
                  <span className="status-pill status-pill-danger">stale</span>
                )}
              </div>

              {state.ghost === null && (
                <p className="muted">
                  Edit a source joint to plan and validate a predicted final
                  configuration.
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

              <div className="robot-control-actions">
                <button
                  className="approve-button"
                  type="button"
                  disabled={!canApprove}
                  onClick={() => void approve()}
                >
                  Approve
                </button>
                <button
                  type="button"
                  disabled={!canRevert}
                  onClick={revert}
                >
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
            </aside>
          </div>
        </>
      )}
    </section>
  );
}
