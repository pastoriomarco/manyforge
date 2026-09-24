import { useEffect, useMemo, useRef, useState, type CSSProperties } from 'react';

import {
  programMotionStopSettled,
  type ProgramRuntimeController,
} from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type {
  JsonValue,
} from '../types/api';
import type { ExecutionObservationTransportStatus } from '../types/viewerObservation';
import { manualOperationRequiresRecovery, robotControlHasActiveOperation } from '../state/robotControl';
import { appliedExecutionSelectionRows } from '../state/programExecutionGroups';
import { OverlayPanel } from './OverlayPanel';
import {
  usePublishWorkbenchNotices,
  type WorkbenchNotice,
} from '../state/workbenchNotices';


interface ProgramRuntimePanelProps {
  enabled: boolean;
  controller: ProgramRuntimeController;
}

const TRANSPORT_PREFERENCES_KEY = 'manyforge.composer.transport_preferences.v1';

interface TransportPreferences {
  controlWidthRem: number;
  confirmHalt: boolean;
}

const DEFAULT_TRANSPORT_PREFERENCES: TransportPreferences = {
  controlWidthRem: 7,
  confirmHalt: true,
};

function loadTransportPreferences(): TransportPreferences {
  try {
    const raw = globalThis.localStorage?.getItem(TRANSPORT_PREFERENCES_KEY);
    if (raw === null || raw === undefined) return DEFAULT_TRANSPORT_PREFERENCES;
    const parsed = JSON.parse(raw) as Partial<TransportPreferences>;
    return {
      controlWidthRem: typeof parsed.controlWidthRem === 'number'
        && Number.isFinite(parsed.controlWidthRem)
        ? Math.min(11, Math.max(6, parsed.controlWidthRem))
        : DEFAULT_TRANSPORT_PREFERENCES.controlWidthRem,
      confirmHalt: typeof parsed.confirmHalt === 'boolean'
        ? parsed.confirmHalt : DEFAULT_TRANSPORT_PREFERENCES.confirmHalt,
    };
  } catch {
    return DEFAULT_TRANSPORT_PREFERENCES;
  }
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

function cycleFailureMessage(failure: Record<string, JsonValue> | null): string | null {
  const message = failure?.message;
  if (typeof message === 'string' && message.length > 0) return message;
  const code = failure?.code;
  return typeof code === 'string' ? code.replaceAll('_', ' ') : null;
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
    cycleFailure,
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
  const ordinaryMotionHeld = recoveryOffer?.recoveryClass === 'ordinary_operator_stop';
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
                : 'Apply a validated Draft before Execute.'}
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
                : 'Execute captures the exact Applied selection once.'}
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

      {cycleState === 'recovery_required' && cycleFailure !== null ? (
        <div className="program-feedback" role="status">
          <span>{cycleFailureMessage(cycleFailure)}</span>
        </div>
      ) : ordinaryMotionHeld ? (
        <div className="program-feedback" role="status">
          <span>Program motion stopped and held. Resume Motion replans from current state.</span>
        </div>
      ) : state.run?.reasonCode && (
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
          disabled={
            !enabled
            || state.pending !== null
            || state.controlPending !== null
          }
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
            ? 'Executing'
            : state.pending === 'run' || state.pending === 'resume-full-stop'
            ? 'Sending Execute'
            : fullStopped
              ? 'Execute'
              : 'Execute Applied Program'}
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
 * Compact viewport transport. It renders one owner-backed control column per
 * displayed Program scope and keeps whole-Program/cell commands separate.
 */
type ManualRecoveryOperation = NonNullable<
  RobotControlController['currentStatus']
>['manualOperations'][number];

/** Owner-projected recovery facts shared by the controls and their notices. */
function transportRecoveryFacts(
  program: ProgramRuntimeController,
  robot: RobotControlController,
) {
  const { state, cycleState, cycleFailure } = program;
  const fullCellStop = robot.currentStatus?.cellStopAction
    ?? robot.state.status?.cellStopAction
    ?? null;
  const manualRecovery = robot.currentStatus?.manualOperations
    .filter(manualOperationRequiresRecovery) ?? [];
  const cellFault = fullCellStop?.fault?.state !== 'cleared'
    ? fullCellStop?.fault ?? null : null;
  const recoveryOffer = state.controls?.recoveryOffer;
  const programRecoveryMessage = cycleFailureMessage(cycleFailure)
    ?? state.run?.message
    ?? recoveryOffer?.reasonCode.replaceAll('_', ' ')
    ?? 'Review the backend recovery requirements before continuing.';
  const programRecovery = cycleState === 'recovery_required'
    || (state.run?.state === 'intervention_required'
      && recoveryOffer != null && recoveryOffer.recoveryClass !== 'ordinary_operator_stop');
  const resetRequired = cycleState === 'terminal_awaiting_reset';
  return {
    manualRecovery,
    cellFault,
    programRecovery,
    programRecoveryMessage,
    resetRequired,
  };
}

function manualRecoveryTarget(
  robot: RobotControlController,
  operation: ManualRecoveryOperation,
) {
  const nativeStatus = robot.currentStatus;
  const motionTarget = operation.controllableComponentId === undefined
    || operation.commandTargetId === undefined
    ? undefined
    : (robot.motionTargets ?? []).find((candidate) => (
      candidate.controllableComponentId === operation.controllableComponentId
      && candidate.commandTargetId === operation.commandTargetId
    ));
  const gripperTarget = operation.controllableComponentId === undefined
    || operation.commandTargetId === undefined
    ? undefined
    : (robot.gripperTargets ?? []).find((candidate) => (
      candidate.controllableComponentId === operation.controllableComponentId
      && candidate.commandTargetId === operation.commandTargetId
    ));
  const gripper = gripperTarget !== undefined
    || (motionTarget === undefined
      && operation.programRunId === nativeStatus?.directControlGripper.programRunId);
  const target = gripper
    ? (gripperTarget ?? nativeStatus?.directControlGripper)
    : (motionTarget ?? nativeStatus?.directControl);
  return { motionTarget, gripperTarget, gripper, target };
}

/**
 * Execution guidance as typed notices for the one notification stack.
 * Recovery conditions are errors carrying their existing guarded owner
 * actions (the single place those actions appear); they have no
 * acknowledgement and disappear when the owner reports recovery. A missing
 * control connection is reported once by the connection condition, not here.
 */
export function deriveTransportGuidance({
  enabled,
  program,
  robot,
  observationTransport,
  motionActive,
  onOpenManualRecovery,
  onReviewProgramRecovery,
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
  observationTransport?: ExecutionObservationTransportStatus;
  motionActive: boolean;
  onOpenManualRecovery?: (operation: ManualRecoveryOperation) => void;
  onReviewProgramRecovery: () => void;
}): WorkbenchNotice[] {
  const notices: WorkbenchNotice[] = [];
  const phase = observationTransport?.phase;
  if (phase === 'synchronizing') {
    notices.push({
      id: 'observation.live-view',
      severity: 'info',
      domain: 'viewer',
      title: 'Live robot view synchronizing',
      message: observationTransport?.message
        ?? 'The current robot state is being reloaded for the active model.',
      busy: true,
    });
  } else if (phase === 'waiting') {
    notices.push({
      id: 'observation.live-view',
      severity: 'info',
      domain: 'viewer',
      title: 'Waiting for the live robot view',
      message: 'The displayed pose is not yet confirmed by the current runtime.',
      busy: true,
    });
  } else if (phase === 'reconnecting') {
    notices.push({
      id: 'observation.live-view',
      severity: 'error',
      domain: 'viewer',
      title: 'Live robot view disconnected',
      message: `${observationTransport?.message ?? 'The measured-state stream is reconnecting.'} The displayed pose is stale; do not rely on it.`,
      expectedDuringTransition: true,
      incidentKind: 'viewer_observation_stale',
    });
  }
  if (!enabled) return notices;
  const facts = transportRecoveryFacts(program, robot);
  if (robot.currentStatus == null) {
    notices.push({
      id: 'recovery.robot-status',
      severity: 'error',
      domain: 'recovery',
      title: 'Robot status unavailable',
      message: 'Last-known manual and cell recovery status is not current.',
    });
    return notices;
  }
  for (const operation of facts.manualRecovery) {
    const { gripper, target } = manualRecoveryTarget(robot, operation);
    notices.push({
      id: `recovery.manual.${operation.operationId}`,
      severity: 'error',
      domain: 'recovery',
      title: `Manual ${gripper ? 'gripper' : 'motion'} recovery required`,
      message: `${target?.controllableComponentId} / ${target?.commandTargetId}. ${
        operation.terminalError ?? operation.state.replaceAll('_', ' ')
      }. Program Run remains blocked. Use Robot control → Reset operation.`,
      actions: onOpenManualRecovery === undefined ? undefined : [{
        id: 'open-recovery',
        label: `Open ${gripper ? 'gripper' : 'robot'} recovery`,
        title: "Open this target's Robot panel. This does not reset or move the robot.",
        disabled: motionActive && robot.state.operation?.operationId !== operation.operationId,
        run: () => onOpenManualRecovery(operation),
      }],
    });
  }
  if (facts.cellFault != null) {
    const fault = facts.cellFault;
    notices.push({
      id: 'recovery.cell',
      severity: 'error',
      domain: 'recovery',
      title: 'Cell recovery required',
      message: `${fault.reasonCode.replaceAll('_', ' ')} · ${
        fault.recoveryStatus.replaceAll('_', ' ')
      }. Motion remains blocked.`,
      busy: robot.stopFaultRecoveryPending,
      actions: [{
        id: 'recover-cell',
        label: robot.stopFaultRecoveryPending
          ? 'Checking recovery'
          : fault.recoveryStatus === 'ready' ? 'Recover cell' : 'Retry recovery',
        disabled: robot.stopFaultRecoveryPending,
        run: () => { void robot.recoverStopFault(); },
      }],
    });
  }
  if (facts.programRecovery) {
    notices.push({
      id: 'recovery.program',
      severity: 'error',
      domain: 'recovery',
      title: 'Program recovery required',
      message: facts.programRecoveryMessage,
      actions: [{
        id: 'review-program-recovery',
        label: 'Review Program recovery',
        title: 'Open the Program recovery details and owner-offered actions.',
        run: onReviewProgramRecovery,
      }],
    });
  } else if (facts.resetRequired) {
    notices.push({
      id: 'program.reset-required',
      severity: 'info',
      domain: 'runtime',
      title: 'Program finished',
      message: 'Use Reset in the Program controls before another Execute.',
    });
  }
  return notices;
}

export function ProgramTransportControls({
  enabled,
  program,
  robot,
  observationTransport,
  onOpenRuntimeHistory,
  onOpenRobotControls,
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
  observationTransport?: ExecutionObservationTransportStatus;
  onOpenRuntimeHistory?: () => void;
  onOpenRobotControls?: () => void;
}) {
  const [detailsOpen, setDetailsOpen] = useState(false);
  const [groupSettingsOpen, setGroupSettingsOpen] = useState(false);
  const [haltIntent, setHaltIntent] = useState<'stopping' | 'confirm' | 'automatic' | null>(null);
  const [preferences, setPreferences] = useState<TransportPreferences>(loadTransportPreferences);
  const [selection, setSelection] = useState({ scopeKey: '', groupIds: ['all'] });
  const detailsButton = useRef<HTMLButtonElement>(null);
  const groupSettingsButton = useRef<HTMLButtonElement>(null);
  const haltButton = useRef<HTMLButtonElement>(null);
  const { state, cycleState } = program;
  const scopeKey = state.applied === null
    ? ''
    : `${state.applied.programId}:${state.applied.programRevision}:${state.applied.recordDigest}`;
  const appliedSelections = useMemo(
    () => appliedExecutionSelectionRows(state.applied),
    [state.applied],
  );
  const projectedSelections = state.controls?.executionSelections.groups;
  const selections = projectedSelections !== undefined && projectedSelections.length > 0
    ? projectedSelections
    : appliedSelections;
  const availableGroupIds = selections.map((group) => group.groupId);
  const allGroup = selections.find((group) => group.groupId === 'all');
  const namedGroups = selections.filter((group) => group.groupId !== 'all');
  const hasMultipleScopes = selections.length > 1;
  const defaultGroupIds = namedGroups.length > 0
    ? namedGroups.map((group) => group.groupId)
    : allGroup === undefined ? [] : ['all'];
  const requestedGroupIds = selection.scopeKey === scopeKey
    ? selection.groupIds.filter((groupId) => availableGroupIds.includes(groupId))
    : [];
  const groupIds = requestedGroupIds.length > 0 ? requestedGroupIds : defaultGroupIds;
  const visibleGroups = selections.filter((group) => groupIds.includes(group.groupId));
  const hiddenGroupCount = hasMultipleScopes
    ? Math.max(0, selections.length - visibleGroups.length)
    : 0;
  const selectionGroupPending = (
    groupId: string,
    kind: 'start-motion' | 'motion-stop' | 'resume-motion',
  ) => (
    state.selectionPending.some((request) => (
      request.kind === kind && request.groupIds.includes(groupId)
    ))
  );
  const motionActive = robotControlHasActiveOperation(robot.state);
  // Includes the other manual mode, not just the operation currently shown
  // in Robot Control. The native owner independently fences Program Resume.
  const manualResident = robot.currentStatus?.manualOperations.some((operation) => operation.resident) === true;
  const programMotionRunning = ['running', 'intervention_required'].includes(state.run?.state ?? '');
  // The command receipt can precede the next owner projection. Treat that
  // short accepted-Run interval as starting so Stop remains available and
  // the owner-scoped velocity controls do not flicker off.
  const programStopping = cycleState === 'stopping';
  const programFullStopping = state.run?.state === 'full_stopping';
  const programFullStopped = state.run?.state === 'full_stopped';
  const canRun = enabled
    && !manualResident
    && state.runnerAvailable
    && state.applied !== null
    && state.pending === null
    && !programMotionRunning
    && program.commandAllowed('program_run');
  const stopCommandPending = ['stop', 'full-stop', 'reset']
    .includes(state.pending ?? '');
  const canResumeFullStop = enabled
    && !manualResident
    && programFullStopped
    && state.pending === null;
  const canRunOrResume = canRun || canResumeFullStop;
  const canReset = enabled
    && state.pending === null
    && state.selectionPending.length === 0
    && state.controlPending === null
    && program.commandAllowed('program_reset');
  // Cell Full Stop is independent of the selected Robot panel target. Use the
  // owner-wide projection first so changing the selected robot cannot hide a
  // valid cell command.
  const fullCellStop = robot.currentStatus?.cellStopAction
    ?? robot.state.status?.cellStopAction
    ?? null;
  const fullStopReason = fullCellStop === null
    ? 'The current execution owner status is unavailable.'
    : fullCellStop.available
      ? 'Stop all currently addressed motion in this cell using the exact owner-issued generation fence.'
      : fullCellStop.unavailableReason
        ?? 'The execution owner has not made Full Stop available.';
  const allMotionQuiesced = allGroup?.state === 'ready' || allGroup?.state === 'held';
  const canProgramHalt = enabled
    && state.runnerAvailable
    && cycleState === 'running'
    && ['running', 'intervention_required'].includes(state.run?.state ?? '')
    && state.controls !== null
    && allGroup !== undefined
    && allGroup.state !== 'unavailable'
    && state.pending === null
    && state.selectionPending.length === 0
    && state.controlPending === null
    && !programFullStopping
    && program.commandAllowed('program_full_stop');

  useEffect(() => {
    try {
      globalThis.localStorage?.setItem(
        TRANSPORT_PREFERENCES_KEY,
        JSON.stringify(preferences),
      );
    } catch {
      // A blocked preference store must never disable execution controls.
    }
  }, [preferences]);

  useEffect(() => {
    if (haltIntent !== 'automatic' || !allMotionQuiesced) return;
    setHaltIntent(null);
    void program.fullStop();
  }, [allMotionQuiesced, haltIntent, program]);

  const requestProgramHalt = async () => {
    if (!canProgramHalt) return;
    if (!allMotionQuiesced) {
      setHaltIntent('stopping');
      const accepted = await program.stopMotion(['all']);
      if (!accepted) {
        setHaltIntent(null);
        return;
      }
    }
    setHaltIntent(preferences.confirmHalt ? 'confirm' : 'automatic');
  };
  const nativeStatus = robot.currentStatus;
  const {
    manualRecovery,
    cellFault,
    programRecovery,
    resetRequired,
  } = transportRecoveryFacts(program, robot);
  const recoveryRequired = manualRecovery.length > 0 || cellFault != null || programRecovery;
  const groupScaleStopAffects = (groupId: string): boolean => {
    if (['stopping', 'full_stopping'].includes(state.run?.state ?? '')) return true;
    if (selections.some((group) => group.groupId === groupId && group.state === 'stopping')) {
      return true;
    }
    return state.selectionPending.some((request) => (
      request.kind === 'motion-stop'
      && (groupId === 'all'
        || request.groupIds.includes(groupId)
        || request.groupIds.includes('all'))
    ));
  };
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
  const openManualRecovery = (operation: ManualRecoveryOperation) => {
    const target = manualRecoveryTarget(robot, operation);
    // Select the exact addressed robot before opening the panel. A gripper
    // operation selects its owning motion target first; the tool then follows.
    const owningMotion = target.motionTarget
      ?? (target.target === undefined ? undefined : (robot.motionTargets ?? [])
        .find((candidate) => (
          candidate.controllableComponentId
            === target.target?.controllableComponentId
        )));
    if (owningMotion !== undefined) {
      robot.selectRobot(
        owningMotion.controllableComponentId,
        owningMotion.commandTargetId,
      );
    }
    if (target.gripperTarget !== undefined) {
      robot.selectTool(target.gripperTarget.commandTargetId);
    }
    robot.selectMode(target.gripper ? 'gripper' : 'joint');
    onOpenRobotControls?.();
  };
  usePublishWorkbenchNotices('transport', deriveTransportGuidance({
    enabled,
    program,
    robot,
    observationTransport,
    motionActive,
    onOpenManualRecovery: onOpenRobotControls === undefined ? undefined : openManualRecovery,
    onReviewProgramRecovery: () => setDetailsOpen(true),
  }));
  // Keep the control panels at their established sizes. Only the center
  // guidance track is elastic; additional Motion groups widen the left panel
  // by exactly one control column plus its inter-column gap.
  const motionPanelWidthRem = visibleGroups.length === 0
    ? 12.725
    : (visibleGroups.length * preferences.controlWidthRem)
      + (Math.max(0, visibleGroups.length - 1) * 0.35)
      + 0.725;

  return (
    <>
      <div
        className="viewer-transport"
        aria-label="Program and motion controls"
        style={{
          '--viewer-transport-column-width': `${preferences.controlWidthRem}rem`,
          '--viewer-transport-motion-panel-width': `${motionPanelWidthRem}rem`,
        } as CSSProperties}
      >
        <div className="transport-control-layout">
          <section className="transport-motion-panel" aria-label="Motion controls">
            <h3 className="transport-panel-title">Motion</h3>
            <div className="transport-group-grid" aria-label="Motion group controls">
          {visibleGroups.length === 0 && (
            <p className="transport-groups-empty">
              {selections.length === 0
                ? 'Group controls require an Applied Program.'
                : 'No command groups are displayed. Open Settings to choose one.'}
            </p>
          )}
          {visibleGroups.map((group) => {
            const stopPending = selectionGroupPending(group.groupId, 'motion-stop');
            const startPending = selectionGroupPending(group.groupId, 'start-motion');
            const resumePending = selectionGroupPending(group.groupId, 'resume-motion');
            const selectionTransitionPending = state.selectionPending.length > 0;
            const canGroupStop = enabled
              && state.runnerAvailable
              && !stopCommandPending
              && !programStopping
              && programMotionRunning
              && group.stopAllowed
              && !selectionTransitionPending
              && !stopPending
              && group.state !== 'held'
              && group.state !== 'ready'
              && group.state !== 'stopping'
              && program.commandAllowed('program_motion_stop');
            const canGroupStart = enabled
              && state.runnerAvailable
              && cycleState === 'running'
              && !manualResident
              && programMotionRunning
              && state.pending === null
              && !selectionTransitionPending
              && state.controlPending === null
              && group.startAllowed
              && program.commandAllowed('program_motion_start');
            const canGroupResume = enabled
              && state.runnerAvailable
              && cycleState === 'running'
              && !manualResident
              && programMotionRunning
              && state.pending === null
              && !selectionTransitionPending
              && group.resumeAllowed
              && !resumePending
              && state.controlPending === null
              && program.commandAllowed('program_motion_resume');
            const scale = program.groupRuntimeScales[group.groupId];
            const scaleAvailable = enabled
              && blockedReason === null
              && state.applied !== null
              && state.run !== null
              && state.controls !== null
              && program.commandAllowed('runtime_scale')
              && !groupScaleStopAffects(group.groupId)
              && group.state !== 'stopping'
              && scale?.status?.supported === true
              && scale.status.frozen === false;
            const scaleReason = scale?.error
              ?? group.runtimeScaleUnavailableReason
              ?? scale?.status?.reason
              ?? 'The group has no qualified runtime velocity control.';
            const requestedScale = scale?.draft ?? scale?.status?.requestedScale ?? 1;
            const effectiveScale = scale?.status?.effectiveScale ?? null;
            const effectiveDiffers = effectiveScale !== null
              && Math.abs(effectiveScale - requestedScale) > 0.005;
            const effectiveMixed = effectiveScale === null
              && scale?.status !== null
              && scale?.status !== undefined
              && (scale.status.applicationState === 'applied'
                || scale.status.activeOperationId !== null
                || ['running', 'mixed'].includes(group.state));
            const scaleOutput = scaleAvailable
              ? `${Math.round(requestedScale * 100)}%${effectiveDiffers || effectiveMixed ? '*' : ''}`
              : '—';
            return (
              <section
                className="transport-group-column"
                key={group.groupId}
                aria-label={`${group.groupId} controls`}
              >
                <button
                  type="button"
                  className="transport-group-resume"
                  disabled={!canGroupStart && !canGroupResume}
                  title={canGroupStart
                    ? `Run initial motion in ${group.groupId}.`
                    : canGroupResume
                      ? `Resume ${group.groupId} from current observations.`
                      : group.state === 'running'
                        ? `${group.groupId} motion is running.`
                        : (group.state === 'ready'
                          ? group.startUnavailableReason
                          : group.resumeUnavailableReason)?.replaceAll('_', ' ')
                            ?? 'Run is unavailable for this group.'}
                  onClick={() => {
                    if (canGroupStart) void program.startMotion([group.groupId]);
                    else if (canGroupResume) void program.resumeMotion([group.groupId]);
                  }}
                >{startPending ? 'Starting'
                    : resumePending ? 'Resuming'
                      : group.state === 'ready' ? 'Run'
                        : group.state === 'held' ? 'Resume'
                          : group.state === 'running' ? 'Running' : 'Run'}</button>
                <button
                  type="button"
                  className="danger-button transport-group-stop"
                  disabled={!canGroupStop}
                  title={canGroupStop
                    ? `Stop motion in ${group.groupId} without resetting the Program.`
                    : group.state === 'held'
                      ? 'This group is already stopped.'
                      : 'Stop is unavailable for this group.'}
                  onClick={() => { void program.stopMotion([group.groupId]); }}
                >{stopPending || group.state === 'stopping' ? 'Stopping'
                    : group.state === 'held' || group.state === 'ready' ? 'Stopped' : 'Stop'}</button>
                <label className="transport-group-scale" title={scaleAvailable
                  ? `Scale current and subsequent moves for ${group.groupId}.`
                    + (effectiveDiffers
                      ? ` Requested ${Math.round(requestedScale * 100)}%; effective ${Math.round(effectiveScale! * 100)}%.`
                      : effectiveMixed
                        ? ' Effective member scales are mixed or not yet settled.'
                        : '')
                  : scaleReason}
                >
                  <span>Velocity</span>
                  <input
                    type="range"
                    min="0"
                    max="1"
                    step="0.01"
                    value={requestedScale}
                    disabled={!scaleAvailable}
                    onChange={(event) => program.setGroupRuntimeScale(group.groupId, Number(event.target.value))}
                    onPointerUp={() => program.commitGroupRuntimeScale()}
                    onKeyUp={() => program.commitGroupRuntimeScale()}
                    onBlur={() => program.commitGroupRuntimeScale()}
                  />
                  <output
                    aria-busy={scale?.pending || undefined}
                    aria-label={`Requested velocity ${scaleOutput}`}
                  >{scale?.pending ? '…' : scaleOutput}</output>
                </label>
                <span className="transport-group-name" title={group.groupId}>
                  {group.groupId === 'all' ? 'All groups' : group.groupId}
                </span>
              </section>
            );
          })}
            </div>
          </section>
          <section className="transport-unique-column" aria-label="Program and cell controls">
          <h3 className="transport-panel-title">Program</h3>
          <button
            type="button"
            className="transport-run transport-unique-control"
            disabled={!canRunOrResume}
            title={canResumeFullStop
                ? 'Execute the preserved Program tree from fresh current-state resolution.'
                : canRun
                  ? 'Execute the exact Applied Program without starting robot motion.'
                  : blockedReason ?? 'Execute requires an Applied Program and an idle, reset runner.'}
            onClick={() => {
              if (canResumeFullStop) void program.resumeFullStop();
              else void program.run();
            }}
          >{state.pending === 'run' || state.pending === 'resume-full-stop'
            ? 'Executing'
            : canResumeFullStop
              ? 'Execute'
              : cycleState === 'starting'
                ? 'Executing'
                : programMotionRunning ? 'Executing' : 'Execute'}</button>
          <button
            ref={haltButton}
            type="button"
            className="danger-button transport-full-stop transport-unique-control"
            disabled={!canProgramHalt || haltIntent !== null}
            title={canProgramHalt
              ? 'Stop all Program motion, then halt Program execution.'
              : 'Halt is available only while a Program is executing.'}
            onClick={() => { void requestProgramHalt(); }}
          >{haltIntent !== null || programFullStopping ? 'Halting' : 'Halt'}</button>
          <button
            type="button"
            className={canReset ? 'transport-reset transport-unique-control meaningful' : 'transport-reset transport-unique-control'}
            disabled={!canReset}
            title={canReset
              ? resetRequired ? 'Reset the completed Program cycle.'
                : 'Reset the current Program cycle using its exact owner evidence.'
              : 'Reset is unavailable in the current backend control state.'}
            onClick={() => { void program.reset(); }}
          >{cycleState === 'resetting' || state.pending === 'reset'
            || state.controlPending === 'recovery:reset_cycle' ? 'Resetting' : 'Reset'}</button>
          <div className="transport-unique-icons">
            <button
              ref={detailsButton}
              type="button"
              className="icon-button transport-unique-icon"
              data-recovery-required={enabled && recoveryRequired || undefined}
              aria-label="Program run information"
              title={blockedReason ?? 'Program and cell run information'}
              onClick={() => setDetailsOpen(true)}
            >i</button>
            <div
              className="transport-group-settings"
              onBlur={(event) => {
                if (!event.currentTarget.contains(event.relatedTarget)) setGroupSettingsOpen(false);
              }}
              onKeyDown={(event) => {
                if (event.key === 'Escape') {
                  setGroupSettingsOpen(false);
                  event.stopPropagation();
                }
              }}
            >
              <button
                ref={groupSettingsButton}
                type="button"
                className="icon-button transport-unique-icon transport-settings-button"
                aria-label={hiddenGroupCount > 0
                  ? `Open control settings; ${hiddenGroupCount} command groups hidden`
                  : 'Open control settings'}
                aria-haspopup="dialog"
                aria-expanded={groupSettingsOpen}
                title="Control layout and Program Halt settings."
                onClick={() => setGroupSettingsOpen((open) => !open)}
              >
                <span className="transport-settings-glyph" aria-hidden="true">⚙</span>
                {hiddenGroupCount > 0 && (
                  <span
                    className="transport-hidden-group-count"
                    aria-hidden="true"
                  >{hiddenGroupCount}</span>
                )}
              </button>
              {groupSettingsOpen && (
                <div className="transport-group-settings-menu" role="dialog" aria-label="Control settings">
                  {hasMultipleScopes && <>
                    <strong>Displayed groups</strong>
                    {selections.map((group) => (
                      <label className="transport-settings-option" key={group.groupId}>
                        <input
                          type="checkbox"
                          checked={groupIds.includes(group.groupId)}
                          onChange={(event) => {
                            const nextGroupIds = event.target.checked
                              ? [...new Set([...groupIds, group.groupId])]
                              : groupIds.filter((id) => id !== group.groupId);
                            if (nextGroupIds.length === 0) return;
                            setSelection({ scopeKey, groupIds: nextGroupIds });
                          }}
                        />
                        <span title={group.groupId}>{group.groupId === 'all' ? 'All groups' : group.groupId}</span>
                      </label>
                    ))}
                  </>}
                  <strong>Layout</strong>
                  <label className="transport-settings-range">
                    <span>Control width</span>
                    <input
                      type="range"
                      min="6"
                      max="11"
                      step="0.25"
                      value={preferences.controlWidthRem}
                      onChange={(event) => setPreferences((current) => ({
                        ...current,
                        controlWidthRem: Number(event.target.value),
                      }))}
                    />
                    <output>{preferences.controlWidthRem.toFixed(2).replace(/\.00$/, '')} rem</output>
                  </label>
                  <label className="transport-settings-option">
                    <input
                      type="checkbox"
                      checked={preferences.confirmHalt}
                      onChange={(event) => setPreferences((current) => ({
                        ...current,
                        confirmHalt: event.target.checked,
                      }))}
                    />
                    <span>Confirm before halting Program execution</span>
                  </label>
                </div>
              )}
            </div>
          </div>
          </section>
        </div>
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
          Each displayed group has its own owner-scoped runtime velocity. A
          slider keeps only the latest pending value, reconciles the owner&apos;s
          applied value, and drops pending input when Stop begins. A disabled
          slider means that the selected group has no qualified runtime-scale
          target or is currently held for Stop/recovery.
        </p>
        <section className="runtime-detail-group" aria-label="Program Halt">
          <h4>Program Halt</h4>
          <p className="panel-intro transport-unavailable-reason">
            Quiesce the current Program while retaining its captured run for a
            later Execute or Reset. This is distinct from the cell-wide Full Stop.
          </p>
          <button
            type="button"
            className="runtime-detail-button danger-button"
            disabled={!canProgramHalt || haltIntent !== null}
            onClick={() => { void requestProgramHalt(); }}
          >
            {haltIntent !== null || state.pending === 'full-stop'
              ? 'Halting Program' : 'Halt Program'}
          </button>
        </section>
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
            className="runtime-detail-button danger-button"
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
          {fullCellStop?.fault && fullCellStop.fault.state !== 'cleared' && (
            <>
              <p className="panel-intro transport-unavailable-reason">
                Stop-fault recovery: {fullCellStop.fault.recoveryStatus.replaceAll('_', ' ')}.
                {fullCellStop.fault.recoveryStatus !== 'ready'
                  ? ' The cell remains motion-blocked until the backend confirms configuration and quiescence.'
                  : ' The backend has confirmed the cell can be recovered.'}
              </p>
              {robot.stopFaultRecoveryError && (
                <p className="panel-intro">
                  {robot.stopFaultRecoveryError}
                </p>
              )}
              <button
                type="button"
                className="runtime-detail-button"
                disabled={
                  !enabled
                  || robot.stopFaultRecoveryPending
                }
                title={fullCellStop.fault.recoveryStatus === 'ready'
                  ? 'Clear the cell stop-fault latch using the current backend recovery offer.'
                  : 'Ask the backend to refresh the recovery assessment. It will clear the latch only when all safety predicates are ready.'}
                onClick={() => { void robot.recoverStopFault(); }}
              >
                {robot.stopFaultRecoveryPending
                  ? 'Checking recovery'
                  : fullCellStop.fault.recoveryStatus === 'ready' ? 'Recover cell' : 'Retry recovery'}
              </button>
            </>
          )}
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
              className="runtime-detail-button"
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
      <OverlayPanel
        open={haltIntent === 'confirm'}
        title="Halt Program execution?"
        titleId="program-halt-confirmation-title"
        kind="dialog"
        onClose={() => setHaltIntent(null)}
        returnFocusRef={haltButton}
      >
        <p className="panel-intro">
          Program motion has been stopped. Halting also pauses Program-tree
          execution; choose Cancel if you intended to stop motion only.
        </p>
        {!allMotionQuiesced && (
          <p className="panel-intro" role="status">Waiting for all motion groups to stop…</p>
        )}
        <div className="program-halt-confirmation-actions">
          <button type="button" onClick={() => setHaltIntent(null)}>Cancel</button>
          <button
            type="button"
            className="danger-button"
            disabled={!allMotionQuiesced}
            onClick={() => {
              setHaltIntent(null);
              void program.fullStop();
            }}
          >Halt Program</button>
        </div>
      </OverlayPanel>
    </>
  );
}
