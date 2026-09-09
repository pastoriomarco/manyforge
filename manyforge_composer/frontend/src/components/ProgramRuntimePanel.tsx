import { useRef, useState } from 'react';

import {
  programMotionStopSettled,
  type ProgramRuntimeController,
} from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { JsonValue } from '../types/api';
import { manualOperationRequiresRecovery, robotControlHasActiveOperation } from '../state/robotControl';
import { OverlayPanel } from './OverlayPanel';


interface ProgramRuntimePanelProps {
  enabled: boolean;
  controller: ProgramRuntimeController;
}

function shortIdentity(value: string | null): string {
  if (!value) return 'none';
  return value.length > 26 ? `${value.slice(0, 22)}…` : value;
}

function stateLabel(state: string, motionStopSettled = false): string {
  if (
    motionStopSettled
    && (state === 'running' || state === 'intervention_required')
  ) return 'Stopped';
  if (state === 'stopped' || state === 'full_stopped') return 'Stopped';
  if (state === 'full_stopping') return 'Stopping';
  return state.replaceAll('_', ' ');
}

function actionLabel(value: string): string {
  const label = value.replace(/^recovery\./, '').replaceAll('_', ' ');
  return label.charAt(0).toUpperCase() + label.slice(1);
}

function parseRuntimeValue(type: string, source: string): JsonValue {
  const trimmed = source.trim();
  if (type === 'string') return source;
  if (type === 'bool' || type === 'boolean') {
    if (trimmed === 'true') return true;
    if (trimmed === 'false') return false;
    throw new Error('Enter true or false.');
  }
  if (/^(u?int)(8|16|32|64)?$/.test(type)) {
    if (!/^-?\d+$/.test(trimmed)) throw new Error('Enter a whole number.');
    const value = Number(trimmed);
    if (!Number.isSafeInteger(value)) throw new Error('Enter a safe whole number.');
    return value;
  }
  if (/^(float|double)(32|64)?$/.test(type)) {
    const value = Number(trimmed);
    if (!Number.isFinite(value)) throw new Error('Enter a finite number.');
    return value;
  }
  return JSON.parse(trimmed) as JsonValue;
}

export function ProgramRuntimePanel({
  enabled,
  controller,
}: ProgramRuntimePanelProps) {
  const {
    state,
    cycleState,
    commandAllowed,
    refresh,
    run,
    resumeFullStop,
    stop,
    reset,
    applyRecoveryAction,
    activateRuntimeOverride,
    clearRuntimeOverride,
    promoteRuntimeOverride,
    materializePromotion,
    promotionMaterializationReason,
    writeRuntimeBlackboard,
    stageManualResult,
  } = controller;
  const [confirmation, setConfirmation] = useState<string | null>(null);
  const [blackboardKey, setBlackboardKey] = useState('');
  const [blackboardValue, setBlackboardValue] = useState('');
  const [blackboardError, setBlackboardError] = useState<string | null>(null);
  const [manualOutput, setManualOutput] = useState('null');
  const [manualError, setManualError] = useState<string | null>(null);
  const canStop = cycleState === 'starting'
    || state.run?.state === 'running'
    || state.run?.state === 'intervention_required';
  // A selected Stop receipt must not block Stop for a different selection or
  // the broader Full Stop. The owner, not pending click feedback, holds nodes.
  const stopCommandPending = ['stop', 'full-stop', 'reset']
    .includes(state.pending ?? '');
  const fullStopped = state.run?.state === 'full_stopped';
  const canRun = state.runnerAvailable
    && state.applied !== null
    && state.pending === null
    && (
      fullStopped
        ? commandAllowed('program_resume_full_stop')
        : cycleState === 'idle_after_reset'
          && commandAllowed('program_run')
    );
  const controls = state.controls?.runtimeControls ?? null;
  const recoveryOffer = state.controls?.recoveryOffer ?? null;
  const writableKeys = (state.applied?.program.blackboard?.keys ?? []).filter(
    (key) => key.access === 'write' || key.access === 'read_write',
  );
  const selectedBlackboardKey = writableKeys.find(
    (key) => key.key === blackboardKey,
  ) ?? null;

  return (
    <section className="panel program-runtime-panel" aria-labelledby="program-runtime-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Headless-owned execution</p>
          <h2 id="program-runtime-title">Program run</h2>
        </div>
        <button
          type="button"
          className="quiet-button"
          disabled={!enabled || state.pending !== null || state.controlPending !== null}
          onClick={() => { void refresh(); }}
        >
          Refresh status
        </button>
      </header>

      {!enabled && (
        <div className="panel-state">
          Program execution requires the configured headless product.
        </div>
      )}

      {enabled && state.loading && state.applied === null && (
        <div className="panel-state" role="status">
          Reading Applied Program and runner status…
        </div>
      )}

      {enabled && state.observationError !== null && (
        <div className="program-feedback" role="status">
          Runtime facts are reconnecting automatically: {state.observationError}
        </div>
      )}

      {enabled && (
        <div className="program-runtime-grid">
          <div className="program-runtime-fact">
            <span>Applied Program</span>
            <strong>{state.applied?.programId ?? 'none'}</strong>
            <small>
              {state.applied
                ? `${state.applied.scenarioId} · ${shortIdentity(state.applied.programRevision)}`
                : 'Apply a validated Draft before Run.'}
            </small>
            {state.applied && (
              <a
                href="/api/program/applied"
                target="_blank"
                rel="noreferrer"
              >
                Inspect exact Applied Program and Binding JSON
              </a>
            )}
          </div>
          <div className="program-runtime-fact">
            <span>Owner-issued run state</span>
            <strong>
              {state.run
                ? stateLabel(
                  state.run.state,
                  programMotionStopSettled(state.run, state.controls),
                )
                : state.runnerAvailable
                  ? 'idle'
                  : 'unavailable'}
            </strong>
            <small>
              {state.run
                ? `${shortIdentity(state.run.capture.programRunId)} · revision ${state.run.runRevision}`
                : state.runnerAvailable
                  ? 'No active or locally observed terminal run.'
                  : 'No current owner projection; execution state is unknown.'}
            </small>
          </div>
          <div className="program-runtime-fact">
            <span>Captured run Program</span>
            <strong>{state.run?.capture.programId ?? 'none'}</strong>
            <small>
              {state.run
                ? `${state.run.capture.scenarioId} · ${shortIdentity(state.run.capture.programRevision)} · immutable for this run`
                : 'A new Run captures the exact Applied selection once.'}
            </small>
          </div>
          <div className="program-runtime-fact">
            <span>Active robot work</span>
            <strong>
              {state.run
                ? state.run.activeOperationCount ?? 'unknown'
                : state.runnerAvailable
                  ? 0
                  : 'unknown'}
            </strong>
            <small>
              {state.run
                ? `Tree ${state.run.treeStatus}; owner ${shortIdentity(state.run.ownerEpoch)}`
                : state.runnerAvailable
                  ? 'Read from the independent Program runner.'
                  : 'Runner loss never proves that dispatched robot work stopped.'}
            </small>
          </div>
        </div>
      )}

      {state.run?.reasonCode && (
        <div className="program-feedback" role="status">
          <span>
            {state.run.reasonCode}
            {state.run.message ? `: ${state.run.message}` : ''}
          </span>
        </div>
      )}
      {enabled && recoveryOffer !== null && (
        <section className="runtime-control-section" aria-labelledby="runtime-recovery-title">
          <div className="runtime-control-heading">
            <div>
              <span>Current held attempt</span>
              <h3 id="runtime-recovery-title">Recovery</h3>
            </div>
            <code>offer {recoveryOffer.offerRevision}</code>
          </div>
          <p>
            {actionLabel(recoveryOffer.recoveryClass)} · node{' '}
            <code>{recoveryOffer.nodeInstanceId}</code>
          </p>
          <div className="runtime-control-actions">
            {recoveryOffer.actions.map((action) => {
              const confirming = confirmation === action.actionId;
              return (
                <span key={action.actionId} className="runtime-control-action">
                  <button
                    type="button"
                    disabled={
                      !action.enabled
                      || state.controlPending !== null
                    }
                    title={action.disabledReasonCode ?? undefined}
                    onClick={() => {
                      if (action.requiresConfirmation && !confirming) {
                        setConfirmation(action.actionId);
                        return;
                      }
                      setConfirmation(null);
                      applyRecoveryAction(action.actionId);
                    }}
                  >
                    {confirming ? 'Confirm ' : ''}{actionLabel(action.labelKey)}
                  </button>
                  {confirming && (
                    <button type="button" onClick={() => setConfirmation(null)}>
                      Cancel
                    </button>
                  )}
                </span>
              );
            })}
          </div>
          <details>
            <summary
              title={recoveryOffer.manualResult.disabledReasonCode ?? undefined}
            >
              Stage a manual result
            </summary>
            <label className="runtime-control-field">
              <span>Output (JSON)</span>
              <textarea
                rows={2}
                value={manualOutput}
                onChange={(event) => {
                  setManualOutput(event.currentTarget.value);
                  setManualError(null);
                }}
              />
            </label>
            {manualError && <p className="field-error">{manualError}</p>}
            <div className="runtime-control-actions">
              {(['succeeded', 'failed'] as const).map((outcome) => (
                <button
                  key={outcome}
                  type="button"
                  disabled={
                    !recoveryOffer.manualResult.enabled
                    || state.controlPending !== null
                  }
                  title={
                    recoveryOffer.manualResult.disabledReasonCode ?? undefined
                  }
                  onClick={() => {
                    try {
                      const output = JSON.parse(manualOutput) as JsonValue;
                      setManualError(null);
                      stageManualResult(outcome, output);
                    } catch {
                      setManualError('Output must be valid JSON.');
                    }
                  }}
                >
                  Mark {outcome === 'succeeded' ? 'success' : 'failure'}
                </button>
              ))}
            </div>
          </details>
        </section>
      )}

      {enabled && controls !== null && (
        <section className="runtime-control-section" aria-labelledby="runtime-overrides-title">
          <div className="runtime-control-heading">
            <div>
              <span>Current run only</span>
              <h3 id="runtime-overrides-title">Runtime overrides</h3>
            </div>
            <code>revision {controls.runtimeControlRevision}</code>
          </div>
          {controls.overrides.length === 0 ? (
            <p>No runtime overrides have been staged.</p>
          ) : controls.overrides.map((override) => (
            <details key={override.overrideId} className="runtime-control-record">
              <summary>
                {override.nodeInstanceId} · {override.parameterId} · {override.status}
              </summary>
              <pre>{JSON.stringify(override.value, null, 2)}</pre>
              <div className="runtime-control-actions">
                <button
                  type="button"
                  disabled={override.status !== 'staged' || state.controlPending !== null}
                  onClick={() => activateRuntimeOverride(override)}
                >
                  Activate
                </button>
                <button
                  type="button"
                  disabled={
                    !['staged', 'active'].includes(override.status)
                    || state.controlPending !== null
                  }
                  onClick={() => clearRuntimeOverride(override)}
                >
                  Clear
                </button>
                <button
                  type="button"
                  disabled={
                    !['staged', 'active'].includes(override.status)
                    || state.controlPending !== null
                  }
                  onClick={() => promoteRuntimeOverride(override)}
                >
                  Request Draft promotion
                </button>
              </div>
            </details>
          ))}
          {controls.promotionProposals.map((proposal) => {
            const unavailable = promotionMaterializationReason(proposal);
            return (
              <div key={proposal.proposalId} className="runtime-control-record">
                <span>
                  Draft promotion · {proposal.nodeInstanceId} · {proposal.status}
                </span>
                <button
                  type="button"
                  disabled={unavailable !== null}
                  title={unavailable ?? undefined}
                  onClick={() => { void materializePromotion(proposal); }}
                >
                  Apply to Draft
                </button>
              </div>
            );
          })}
        </section>
      )}

      {enabled && state.run !== null && writableKeys.length > 0 && (
        <section className="runtime-control-section" aria-labelledby="runtime-blackboard-title">
          <div className="runtime-control-heading">
            <div>
              <span>Typed current-run value</span>
              <h3 id="runtime-blackboard-title">Runtime blackboard</h3>
            </div>
          </div>
          <label className="runtime-control-field">
            <span>Writable key</span>
            <select
              value={blackboardKey}
              onChange={(event) => {
                setBlackboardKey(event.currentTarget.value);
                setBlackboardValue('');
                setBlackboardError(null);
              }}
            >
              <option value="">Choose a key</option>
              {writableKeys.map((key) => (
                <option key={key.key} value={key.key}>
                  {key.key} · {key.type}
                </option>
              ))}
            </select>
          </label>
          <label className="runtime-control-field">
            <span>Value{selectedBlackboardKey ? ` (${selectedBlackboardKey.type})` : ''}</span>
            <input
              value={blackboardValue}
              disabled={selectedBlackboardKey === null}
              onChange={(event) => {
                setBlackboardValue(event.currentTarget.value);
                setBlackboardError(null);
              }}
            />
          </label>
          {blackboardError && <p className="field-error">{blackboardError}</p>}
          <button
            type="button"
            disabled={
              selectedBlackboardKey === null
              || state.controlPending !== null
            }
            onClick={() => {
              if (selectedBlackboardKey === null) return;
              try {
                const value = parseRuntimeValue(
                  selectedBlackboardKey.type,
                  blackboardValue,
                );
                setBlackboardError(null);
                writeRuntimeBlackboard(selectedBlackboardKey.key, value);
              } catch (error) {
                setBlackboardError(
                  error instanceof Error ? error.message : 'Value is invalid.',
                );
              }
            }}
          >
            Write current-run value
          </button>
        </section>
      )}

      {controls !== null && controls.audit.length > 0 && (
        <details className="runtime-control-section">
          <summary>Recent runtime-control events ({controls.audit.length})</summary>
          {controls.audit[0].eventRevision > 1 && (
            <p>Earlier events are outside this recent window. Use Runtime / history for retained operational history.</p>
          )}
          <ol className="runtime-control-audit">
            {controls.audit.map((event) => (
              <li key={event.eventRevision}>
                <code>{event.eventRevision}</code> {actionLabel(event.kind)}
              </li>
            ))}
          </ol>
        </details>
      )}
      {commandAllowed('program_reset') && (
        <button
          type="button"
          disabled={!enabled || state.pending !== null}
          onClick={() => { void reset(); }}
        >
          {cycleState === 'resetting' ? 'Resetting' : 'Reset Program cycle'}
        </button>
      )}

      <div className="program-runtime-actions">
        <button
          type="button"
          disabled={!enabled || !canRun}
          onClick={() => {
            if (fullStopped) {
              void resumeFullStop();
            } else {
              void run();
            }
          }}
        >
          {cycleState === 'starting'
            ? 'Starting'
            : state.pending === 'run' || state.pending === 'resume-full-stop'
            ? 'Sending Run'
            : fullStopped
              ? 'Run'
              : 'Run Applied Program'}
        </button>
        <button
          type="button"
          className="danger-button"
          disabled={
            !enabled
            || !state.runnerAvailable
            || !canStop
            || stopCommandPending
          }
          onClick={() => { void stop(); }}
        >
          {state.run?.state === 'stopping'
            ? 'Stopping'
            : state.pending === 'stop'
              ? 'Sending Stop'
              : 'Program Stop'}
        </button>
      </div>
      <p className="panel-intro">
        Closing or reloading Composer stops observation only. It never sends
        Stop implicitly and cannot change already-dispatched robot work.
      </p>
    </section>
  );
}

/**
 * Compact viewport transport. It renders only commands backed by the current
 * owner contracts; unsupported velocity control stays visibly unavailable
 * instead of being simulated in the browser.
 */
export function ProgramTransportControls({
  enabled,
  program,
  robot,
  onOpenRuntimeHistory,
  onOpenRobotControls,
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
  onOpenRuntimeHistory?: () => void;
  onOpenRobotControls?: () => void;
}) {
  const [detailsOpen, setDetailsOpen] = useState(false);
  const [stopMenuOpen, setStopMenuOpen] = useState(false);
  const [selection, setSelection] = useState({ runId: '', groupId: 'all' });
  const detailsButton = useRef<HTMLButtonElement>(null);
  const { state, cycleState } = program;
  const runId = state.run?.capture.programRunId ?? '';
  const selections = state.controls?.executionSelections.groups ?? [];
  const selectedGroup = selections.find((group) => group.groupId === (
    selection.runId === runId ? selection.groupId : 'all'
  )) ?? selections.find((group) => group.groupId === 'all');
  const motionActive = robotControlHasActiveOperation(robot.state);
  const motionPaused = robot.state.operation?.state === 'paused';
  const programMotionRunning = ['running', 'intervention_required'].includes(state.run?.state ?? '');
  const programStarting = cycleState === 'starting';
  const programStopping = cycleState === 'stopping';
  const programFullStopping = state.run?.state === 'full_stopping';
  const programFullStopped = state.run?.state === 'full_stopped';
  const canRun = enabled
    && state.runnerAvailable
    && state.applied !== null
    && state.pending === null
    && !programMotionRunning
    && program.commandAllowed('program_run');
  const motionStopSettled = selectedGroup?.state === 'held' && programMotionRunning;
  const motionStopped = motionStopSettled
    || programFullStopped
    || state.run?.state === 'stopped'
    || (
      !programMotionRunning
      && robot.state.operation?.state === 'stopped'
    );
  const stopCommandPending = ['stop', 'full-stop', 'reset']
    .includes(state.pending ?? '');
  const canStopMotion = enabled
    && !stopCommandPending
    && !programStopping
    && !motionStopSettled
    && selectedGroup?.state !== 'stopping'
    && (
      (state.runnerAvailable && (programStarting || (programMotionRunning
        && program.commandAllowed('program_motion_stop')
        && (selectedGroup?.stopAllowed ?? true))))
      || (robot.controlPending === null && motionActive)
    );
  const canResumeOperatorStop = enabled
    && selectedGroup?.resumeAllowed === true
    && program.commandAllowed('program_motion_resume')
    && state.pending === null
    && state.controlPending === null;
  const canResume = enabled
    && motionPaused
    && state.pending === null
    && robot.controlPending === null;
  const canResumeFullStop = enabled
    && programFullStopped
    && state.pending === null;
  const canRunOrResume = canRun || canResume || canResumeOperatorStop
    || canResumeFullStop;
  const canFullStop = enabled
    && state.runnerAvailable
    && !stopCommandPending
    && ['running', 'intervention_required'].includes(state.run?.state ?? '');
  const canReset = enabled
    && state.pending === null
    && state.controlPending === null
    && program.commandAllowed('program_reset');
  const velocityAvailable = enabled
    && (state.pending === null || state.pending === 'run')
    && !['stopping', 'full_stopping'].includes(state.run?.state ?? '')
    && robot.state.phase !== 'stopping'
    && !robot.fullStopPending
    && robot.runtimeScaleStatus !== null
    && robot.runtimeScaleStatus.supported
    && !robot.runtimeScaleStatus.frozen;
  const velocityReason = robot.runtimeScaleError
    ?? robot.runtimeScaleStatus?.reason
    ?? 'The addressed target does not currently expose qualified runtime velocity control.';
  const fullCellStop = robot.state.status?.cellStopAction ?? null;
  const fullStopReason = fullCellStop === null
    ? 'The current execution owner status is unavailable.'
    : fullCellStop.available
      ? 'Stop all currently addressed motion in this cell using the exact owner-issued generation fence.'
      : fullCellStop.unavailableReason
        ?? 'The execution owner has not made Full Stop available.';
  const nativeStatus = robot.currentStatus;
  const manualRecovery = nativeStatus?.manualOperations
    .filter(manualOperationRequiresRecovery) ?? [];
  const cellFault = nativeStatus?.cellStopAction.fault?.state !== 'cleared'
    ? nativeStatus?.cellStopAction.fault : null;
  const recoveryOffer = state.controls?.recoveryOffer;
  const programRecovery = cycleState === 'recovery_required'
    || (state.run?.state === 'intervention_required'
      && recoveryOffer != null && recoveryOffer.recoveryClass !== 'ordinary_operator_stop');
  const resetRequired = cycleState === 'terminal_awaiting_reset';
  const recoveryRequired = manualRecovery.length > 0 || cellFault != null || programRecovery;
  const blockedReason = !enabled
    ? 'Control status unavailable. Commands are disabled until a current backend status is received.'
    : manualRecovery.length > 0
      ? 'A manual operation requires recovery. Open Robot recovery and Reset operation.'
      : cellFault != null
        ? 'A cell Stop fault still blocks motion. Review the current owner recovery requirements.'
        : programRecovery
          ? 'The Program requires recovery. Review the current owner-offered actions.'
          : resetRequired ? 'Reset the Program before starting another run.'
            : nativeStatus == null
              ? 'Robot status unavailable. Last-known manual and cell recovery status is not current.' : null;

  return (
    <>
      <div className="viewer-transport" aria-label="Program and motion controls">
        {blockedReason !== null && (
          <div
            className={`transport-guidance${recoveryRequired && enabled ? ' recovery-required' : ''}`}
            role="status"
          >
            {!enabled ? <p>{blockedReason} Last-known recovery status is not current.</p> : <>
              {nativeStatus == null && <p>
                Robot status unavailable. Last-known manual and cell recovery status is not current.
              </p>}
              {manualRecovery.map((operation) => {
                const gripper = operation.programRunId === nativeStatus?.directControlGripper.programRunId;
                const target = gripper ? nativeStatus?.directControlGripper : nativeStatus?.directControl;
                return (
                  <div className="transport-guidance-row" key={operation.operationId}>
                    <p>
                      <strong>Manual {gripper ? 'gripper' : 'motion'} recovery required</strong>
                      {' · '}{target?.controllableComponentId} / {target?.commandTargetId}.
                      {' '}{operation.terminalError ?? operation.state.replaceAll('_', ' ')}.
                      {' '}Program Run remains blocked. Use Robot control → Reset operation.
                    </p>
                    {onOpenRobotControls && <button
                      type="button"
                      disabled={motionActive && robot.state.operation?.operationId !== operation.operationId}
                      title="Open this target's Robot panel. This does not reset or move the robot."
                      onClick={() => {
                        robot.selectMode(gripper ? 'gripper' : 'joint');
                        onOpenRobotControls();
                      }}
                    >Open {gripper ? 'gripper' : 'robot'} recovery</button>}
                  </div>
                );
              })}
              {cellFault != null && <div className="transport-guidance-row">
                <p><strong>Cell recovery required.</strong> {cellFault.reasonCode.replaceAll('_', ' ')}
                  {' · '}{cellFault.recoveryStatus.replaceAll('_', ' ')}. Motion remains blocked.</p>
                <button type="button" onClick={() => setDetailsOpen(true)}>Review cell status</button>
              </div>}
              {programRecovery ? <div className="transport-guidance-row">
                <p><strong>Program recovery required.</strong> {state.run?.message
                  || recoveryOffer?.reasonCode.replaceAll('_', ' ')
                  || 'Review the backend recovery requirements before continuing.'}</p>
                <button type="button" onClick={() => setDetailsOpen(true)}>Review Program recovery</button>
              </div> : resetRequired && <p>Program finished. Use <strong>Reset</strong> below before another Run.</p>}
            </>}
          </div>
        )}
        {selections.length > 1 && (
          <label className="transport-selection" title={selectedGroup?.resumeUnavailableReason?.replaceAll('_', ' ')}>
            <span>Scope</span>
            <select
              aria-label="Motion control group"
              value={selectedGroup?.groupId ?? 'all'}
              onChange={(event) => setSelection({ runId, groupId: event.target.value })}
            >
              {selections.map((group) => (
                <option key={group.groupId} value={group.groupId}>
                  {group.groupId === 'all' ? 'All nodes' : group.groupId} · {group.state}
                </option>
              ))}
            </select>
          </label>
        )}
        <button
          type="button"
          className="transport-run"
          disabled={!canRunOrResume}
          title={canResume
            ? 'Resume the exact paused motion operation.'
            : canResumeFullStop
              ? 'Run the preserved Program tree from fresh current-state resolution.'
            : canResumeOperatorStop
              ? 'Resume only this selection after fresh backend assessment.'
            : canRun
              ? 'Run the exact Applied Program.'
              : blockedReason ?? selectedGroup?.resumeUnavailableReason?.replaceAll('_', ' ')
                ?? 'Run requires an Applied Program and an idle, reset runner.'}
          onClick={() => {
            if (canResume) {
              void robot.resume();
            } else if (canResumeFullStop) {
              void program.resumeFullStop();
            } else if (canResumeOperatorStop) {
              void program.resumeMotion(selectedGroup?.groupId);
            } else {
              void program.run();
            }
          }}
        >
          {robot.controlPending === 'resume'
            ? 'Sending Resume'
            : cycleState === 'starting'
              ? 'Starting'
            : state.pending === 'resume-full-stop'
              ? 'Sending Run'
            : state.pending === 'resume-motion' || state.controlPending?.startsWith('recovery:')
              ? 'Sending Resume'
            : state.pending === 'run'
              ? 'Sending Run'
              : canResume || canResumeOperatorStop
                ? 'Resume Motion'
                : programMotionRunning
                  ? 'Running'
                  : 'Run'}
        </button>
        <div
          className="transport-stop-group"
          onBlur={(event) => {
            if (!event.currentTarget.contains(event.relatedTarget)) {
              setStopMenuOpen(false);
            }
          }}
          onKeyDown={(event) => {
            if (event.key === 'Escape') {
              setStopMenuOpen(false);
              event.stopPropagation();
            }
          }}
        >
          <button
            type="button"
            className="danger-button transport-stop-primary"
            disabled={!canStopMotion || robot.state.phase === 'stopping'}
            title={programStarting
              ? 'Stop the Program before its pending start can launch motion.'
              : programMotionRunning
              ? 'Stop addressed motion in the current Program without terminating its execution.'
              : motionActive
                ? 'Stop the exact active Robot Control motion.'
                : 'No addressed robot motion is active.'}
            onClick={() => {
              if (programStarting) {
                void program.stop();
              } else if (programMotionRunning) {
                void program.stopMotion(selectedGroup?.groupId);
              } else {
                void robot.stop();
              }
            }}
          >
            {programStopping || selectedGroup?.state === 'stopping'
              ? 'Stopping'
              : robot.controlPending === 'stop'
              ? 'Sending Stop'
              : robot.state.phase === 'stopping'
              ? 'Stopping'
              : state.pending === 'motion-stop'
                ? 'Sending Stop'
              : motionStopped
                ? 'Stopped'
                : 'Stop'}
          </button>
          <button
            type="button"
            className="danger-button transport-stop-disclosure"
            disabled={
              !enabled
              || !canFullStop
              || programFullStopping
            }
            aria-label="More Stop actions"
            aria-haspopup="menu"
            aria-expanded={stopMenuOpen}
            title={canFullStop
              ? 'Show the deliberate Full Stop action.'
              : 'Full Stop requires a current Program run.'}
            onClick={() => setStopMenuOpen((open) => !open)}
          >
            ▾
          </button>
          {stopMenuOpen && (
            <div className="transport-stop-menu" role="menu">
              <button
                type="button"
                className="danger-button"
                role="menuitem"
                disabled={
                  !enabled
                  || !canFullStop
                  || programFullStopping
                }
                title="Contain motion, then quiesce the current Program while preserving it for Run or Reset."
                onClick={() => {
                  setStopMenuOpen(false);
                  void program.fullStop();
                }}
              >
                {programFullStopping
                  ? 'Stopping'
                  : state.pending === 'full-stop'
                    ? 'Sending Full Stop'
                  : 'Full Stop'}
              </button>
            </div>
          )}
        </div>
        <button
          type="button"
          className={canReset ? 'transport-reset meaningful' : 'transport-reset'}
          disabled={!canReset}
          title={canReset
            ? motionStopSettled
              ? 'Discard the held Program context and reset the Program cycle.'
              : programFullStopped
              ? 'Discard the Full-stopped tree and rebuild an idle Program cycle.'
              : 'Reset the completed Program cycle.'
            : 'Reset is available after a terminal run or Full Stop.'}
          onClick={() => {
            void program.reset();
          }}
        >
          {cycleState === 'resetting'
            ? 'Resetting'
            : state.pending === 'reset'
              || state.controlPending === 'recovery:reset_cycle'
              ? 'Sending Reset'
            : 'Reset'}
        </button>
        <span className="viewer-tool-separator" aria-hidden="true" />
        <label className="transport-velocity" title={velocityAvailable
          ? 'Scale current and subsequent moves for this addressed target.'
          : velocityReason}
        >
          <span>Velocity</span>
          <input
            type="range"
            min="0"
            max="1"
            step="0.01"
            value={robot.runtimeScaleDraft}
            disabled={!velocityAvailable}
            onChange={(event) => robot.setRuntimeScale(Number(event.target.value))}
            onPointerUp={robot.commitRuntimeScale}
            onKeyUp={robot.commitRuntimeScale}
            onBlur={robot.commitRuntimeScale}
          />
          <output>
            {velocityAvailable
              ? `${Math.round(robot.runtimeScaleDraft * 100)}%`
              : '—'}
          </output>
        </label>
        <button
          ref={detailsButton}
          type="button"
          className="icon-button"
          data-recovery-required={enabled && recoveryRequired || undefined}
          aria-label="Program run details"
          title={blockedReason ?? 'Program run details'}
          onClick={() => setDetailsOpen(true)}
        >i</button>
      </div>
      <OverlayPanel
        open={detailsOpen}
        title="Program run details"
        titleId="program-run-details-title"
        kind="info"
        onClose={() => setDetailsOpen(false)}
        returnFocusRef={detailsButton}
      >
        <ProgramRuntimePanel enabled={enabled} controller={program} />
        <p className="panel-intro transport-unavailable-reason">
          Runtime velocity is retained for this addressed robot target across
          current and subsequent moves. The slider keeps only the latest pending
          value, reconciles the owner&apos;s applied value, and drops pending input
          when Stop begins.
          {!velocityAvailable && <> Currently unavailable: {velocityReason}</>}
        </p>
        <section className="runtime-detail-group" aria-label="Cell Full Stop">
          <h4>Cell Full Stop</h4>
          <p className="panel-intro transport-unavailable-reason">
            Generation {fullCellStop?.expectedStopGeneration ?? '—'} · {fullStopReason}
          </p>
          {robot.fullStopError && (
            <p className="panel-intro">
              Cell Full Stop requires attention in Runtime/history.
            </p>
          )}
          <button
            type="button"
            className="danger-button"
            disabled={
              !enabled
              || fullCellStop?.available !== true
              || robot.fullStopPending
            }
            title={fullStopReason}
            onClick={() => { void robot.fullStop(); }}
          >
            {robot.fullStopPending ? 'Sending Full Stop' : 'Cell Full Stop'}
          </button>
          {fullCellStop?.latestTerminal && (
            <p className="panel-intro">
              Latest certificate {fullCellStop.latestTerminal.certificate.stopCertificateId}
              {' · '}{fullCellStop.latestTerminal.result}
              {fullCellStop.latestTerminal.escalated ? ' · escalated' : ''}
              {fullCellStop.latestTerminal.hardStopFloorUsed
                ? ' · hard-stop floor used'
                : ''}
              {fullCellStop.latestTerminal.executedStages.map((stage) => (
                <span key={`${stage.operationId}:${stage.participantId}:${stage.stageIndex}`}>
                  <br />{stage.participantId}: {stage.executedMode ?? stage.actionClass}
                  {stage.substitutionReason
                    ? ` (fallback: ${stage.substitutionReason})`
                    : ''}
                </span>
              ))}
            </p>
          )}
          {fullCellStop?.fault && (
            <p className="error-banner" role="alert">
              Stop fault {fullCellStop.fault.state}: {fullCellStop.fault.reasonCode}
              {' · recovery '}{fullCellStop.fault.recoveryStatus}
            </p>
          )}
          {onOpenRuntimeHistory && (
            <button
              type="button"
              onClick={() => {
                setDetailsOpen(false);
                onOpenRuntimeHistory();
              }}
            >
              Open Runtime/history
            </button>
          )}
        </section>
      </OverlayPanel>
    </>
  );
}
