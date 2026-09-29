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
import { appliedExecutionSelectionRows, shownExecutionGroupIds } from '../state/programExecutionGroups';
import { useComposerSettings } from '../hooks/useComposerSettings';
import { MOTION_CONTROL_WIDTH_REM, type ComposerSettings } from '../state/composerSettings';
import { OverlayPanel } from './OverlayPanel';
import { PendingApplyNote, SettingsSubheading } from './SettingsScope';
import { ProgramRunSetup } from './ProgramRunSetup';
import {
  usePublishWorkbenchNotices,
  type WorkbenchNotice,
} from '../state/workbenchNotices';


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

/** Retry variants the backend may offer for a node paused on failure; the
 * Motion panel shows one Retry for whichever is enabled. */
const RETRY_ACTION_IDS: readonly string[] = [
  'retry_motion_from_current_state',
  'retry_after_scene_repair',
  'retry_runtime_step',
];

const MANUAL_RESULT_PENDING = 'Manual result: implementation pending (open point U10). Disabled in Composer until its safety rules are designed.';

const MOTION_STATE_LABEL: Readonly<Record<string, string>> = {
  running: 'Running',
  ready: 'Waiting for Run',
  held: 'Stopped',
  stopping: 'Stopping',
  faulted: 'Stop fault',
  mixed: 'Mixed',
};

/** "2 running · 1 waiting" from the owner's per-node statuses. */
function nodeActivitySummary(statuses: Record<string, string>): string {
  const values = Object.values(statuses);
  const running = values.filter((status) => status === 'running').length;
  const waiting = values.filter((status) => status === 'held').length;
  return `${running} node${running === 1 ? '' : 's'} running · ${waiting} waiting`;
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
    refresh,
    activateRuntimeOverride,
    clearRuntimeOverride,
    promoteRuntimeOverride,
    materializePromotion,
    promotionMaterializationReason,
    writeRuntimeBlackboard,
    stageManualResult,
  } = controller;
  const [blackboardKey, setBlackboardKey] = useState('');
  const [blackboardValue, setBlackboardValue] = useState('');
  const [blackboardError, setBlackboardError] = useState<string | null>(null);
  const [manualOutput, setManualOutput] = useState('null');
  const [manualError, setManualError] = useState<string | null>(null);
  const controls = state.controls?.runtimeControls ?? null;
  const recoveryOffer = state.controls?.recoveryOffer ?? null;
  const ordinaryMotionHeld = recoveryOffer?.recoveryClass === 'ordinary_operator_stop';
  const writableKeys = (state.applied?.program.blackboard?.keys ?? []).filter(
    (key) => key.access === 'write' || key.access === 'read_write',
  );
  // Live motion of the whole Program, from the owner's group projection
  // (the Kernel's active-operation count is only set while a Stop settles).
  const allMotion = state.controls?.executionSelections.groups
    .find((group) => group.groupId === 'all') ?? null;
  // Never turn a missing projection into "not moving".
  const motionSummary = state.run === null
    ? (state.runnerAvailable ? 'None' : 'Unknown')
    : ['full_stopped', 'full_stopping'].includes(state.run.state)
      ? 'Halted'
      : allMotion === null
        ? 'Unknown'
        : MOTION_STATE_LABEL[allMotion.state] ?? 'Unknown';
  const hasAdvanced = enabled && (
    controls !== null
    || (state.run !== null && writableKeys.length > 0)
  );
  const selectedBlackboardKey = writableKeys.find(
    (key) => key.key === blackboardKey,
  ) ?? null;

  return (
    <section className="panel program-runtime-panel" aria-labelledby="program-runtime-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Program execution</p>
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

      {enabled && state.nonExecutable !== null && state.nonExecutable.programId === null && (
        <div className="panel-state" role="status">
          No Program is applied. Add steps and Apply to enable Execute. Manual control remains independent.
        </div>
      )}
      {enabled && state.nonExecutable !== null && state.nonExecutable.programId !== null && (
        <div className="program-feedback" role="alert" aria-label="Applied Program not executable">
          <p>
            Applied Program <strong>{state.nonExecutable.programId}</strong> is not
            executable in the active Deployment. It is kept unchanged; edit it and
            Apply again to run it.
          </p>
          <ul>
            {state.nonExecutable.unresolvedReferences.map((reference) => (
              <li key={`${reference.component_role_id}/${reference.target_role_id ?? ''}`}>
                {reference.target_role_id !== null
                  ? <>Target role <code>{reference.target_role_id}</code></>
                  : <>Component role <code>{reference.component_role_id}</code></>}
                {' → '}
                <code>{reference.controllable_component_id}</code>
                {reference.command_target_id !== null && <code>/{reference.command_target_id}</code>}
                {': '}
                {reference.reason.replaceAll('_', ' ')}
              </li>
            ))}
          </ul>
          {state.nonExecutable.affectedNodes.length > 0 && (
            <p>
              Affected steps: {state.nonExecutable.affectedNodes.join(', ')}
              {state.nonExecutable.affectedExecutionGroups.length > 0
                && `; groups: ${state.nonExecutable.affectedExecutionGroups.join(', ')}`}
            </p>
          )}
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
            <strong>
              {state.applied?.programId ?? state.nonExecutable?.programId ?? 'none'}
            </strong>
            <small>
              {state.applied
                ? `${state.applied.scenarioId} · ${shortIdentity(state.applied.programRevision)}`
                : state.nonExecutable !== null
                  ? `${state.nonExecutable.scenarioId} · ${state.nonExecutable.programId === null ? 'no Program' : 'not executable in this Deployment'}`
                  : 'Apply a validated Draft before Execute.'}
            </small>
            {state.applied && (
              <a
                href="/api/program/applied"
                target="_blank"
                rel="noreferrer"
              >
                View Applied Program and Binding (JSON)
              </a>
            )}
          </div>
          <div className="program-runtime-fact">
            <span>Run state</span>
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
                  ? 'No run is active.'
                  : 'No current owner projection; execution state is unknown.'}
            </small>
          </div>
          <div className="program-runtime-fact">
            <span>Program in this run</span>
            <strong>{state.run?.capture.programId ?? 'none'}</strong>
            <small>
              {state.run
                ? `${state.run.capture.scenarioId} · ${shortIdentity(state.run.capture.programRevision)} · fixed for this run`
                : 'Execute captures the Applied Program for the run.'}
            </small>
          </div>
          <div className="program-runtime-fact">
            <span>Motion</span>
            <strong>{motionSummary}</strong>
            <small>
              {state.run
                ? nodeActivitySummary(state.run.nodeStatuses)
                : state.runnerAvailable
                  ? 'No run is active.'
                  : 'Runner unavailable: this never proves that robot work stopped.'}
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
          <p className="hint">
            Use Retry in the Motion panel to retry this node and resume the
            Program&apos;s motion, or Reset to end the run.
          </p>
          {/* Kept visible but disabled: the backend path exists, but operator
              use is pending design (open point U10). */}
          <details>
            <summary title={MANUAL_RESULT_PENDING}>
              Stage a manual result (not available yet)
            </summary>
            <label className="runtime-control-field">
              <span>Output (JSON)</span>
              <textarea
                rows={2}
                disabled
                title={MANUAL_RESULT_PENDING}
                value={manualOutput}
                onChange={(event) => {
                  setManualOutput(event.currentTarget.value);
                  setManualError(null);
                }}
              />
            </label>
            {manualError && <p className="inline-message severity-error">{manualError}</p>}
            <div className="runtime-control-actions">
              {(['succeeded', 'failed'] as const).map((outcome) => (
                <button
                  key={outcome}
                  type="button"
                  disabled
                  title={MANUAL_RESULT_PENDING}
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

      {/* Rarely needed tools, kept out of the way; they exist only during a
          run, so the section is shown only when it has content. */}
      {hasAdvanced && <details className="runtime-advanced">
        <summary>Advanced: runtime overrides, blackboard and audit</summary>
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
          {blackboardError && <p className="inline-message severity-error">{blackboardError}</p>}
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
      </details>}
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
      && nativeStatus?.directControlGripper != null
      && operation.programRunId === nativeStatus.directControlGripper.programRunId);
  const target = gripper
    ? (gripperTarget ?? nativeStatus?.directControlGripper ?? undefined)
    : (motionTarget ?? nativeStatus?.directControl ?? undefined);
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

/**
 * Execution groups of the Applied Program and which of them the Motion panel
 * shows: the Applied Program's viewer choice, so hiding a group takes effect
 * with Apply like any other Program change.
 */
function useTransportGroups(program: ProgramRuntimeController) {
  const { state } = program;
  const appliedSelections = useMemo(
    () => appliedExecutionSelectionRows(state.applied),
    [state.applied],
  );
  const projectedSelections = state.controls?.executionSelections.groups;
  const selections = projectedSelections !== undefined && projectedSelections.length > 0
    ? projectedSelections
    : appliedSelections;
  const allGroup = selections.find((group) => group.groupId === 'all');
  const namedGroups = selections.filter((group) => group.groupId !== 'all');
  const groupIds = shownExecutionGroupIds(
    selections.map((group) => group.groupId),
    state.applied?.program.viewer?.hidden_execution_groups,
  );
  const visibleGroups = selections.filter((group) => groupIds.includes(group.groupId));
  // Only named groups the Program hides: "all" is hidden by default whenever
  // named groups exist, and is not worth a badge.
  const hiddenGroupCount = namedGroups
    .filter((group) => !groupIds.includes(group.groupId)).length;
  return { selections, allGroup, visibleGroups, hiddenGroupCount };
}

/** The Draft Program's choice of shown execution groups (a Program setting). */
export interface DraftGroupVisibility {
  /** The Draft's groups, including "all". */
  groupIds: string[];
  hidden: string[] | undefined;
  /** The Draft's choice differs from the Applied Program's. */
  pendingApply: boolean;
  /** Undefined while the Draft cannot be edited. */
  onChange?: (hidden: string[]) => void;
}

/**
 * Motion panel settings, shown in the 3D viewer's settings panel. Displayed
 * groups are a Program setting (they take effect with Apply); layout and
 * confirmations are installation settings. Halt and Stop are never hidden or
 * confirmed away.
 */
export function MotionControlSettings({ groups }: { groups: DraftGroupVisibility | null }) {
  const composerSettings = useComposerSettings();
  const preferences = composerSettings.settings.motionPanel;
  const setPreferences = (change: Partial<ComposerSettings['motionPanel']>) => composerSettings.update(
    (current) => ({ ...current, motionPanel: { ...current.motionPanel, ...change } }),
  );
  const shown = groups === null ? [] : shownExecutionGroupIds(groups.groupIds, groups.hidden);
  const toggle = (groupId: string, visible: boolean) => {
    if (groups?.onChange === undefined) return;
    const next = visible
      ? shown.filter((id) => id !== groupId).concat(groupId)
      : shown.filter((id) => id !== groupId);
    // At least one group stays shown.
    if (next.length === 0) return;
    groups.onChange(groups.groupIds.filter((id) => !next.includes(id)));
  };
  return (
    <div className="drows">
      {groups !== null && groups.groupIds.length > 1 && (
        <fieldset
          aria-label="Displayed groups"
          disabled={groups.onChange === undefined}
        >
          <SettingsSubheading title="Displayed groups" scope="program" />
          {groups.groupIds.map((groupId) => (
            <label className="toggle-control" key={groupId}>
              <input
                type="checkbox"
                checked={shown.includes(groupId)}
                disabled={shown.length === 1 && shown.includes(groupId)}
                onChange={(event) => toggle(groupId, event.target.checked)}
              />
              <span title={groupId}>{groupId === 'all' ? 'All groups' : groupId}</span>
            </label>
          ))}
          {groups.pendingApply && <PendingApplyNote />}
          {groups.onChange === undefined && (
            <span className="hint">The Program Draft cannot be edited right now.</span>
          )}
        </fieldset>
      )}
      <fieldset aria-label="Layout and confirmations">
        <SettingsSubheading
          title="Layout and confirmations"
          scope="installation"
          onRestoreDefaults={() => composerSettings.restoreSection('motionPanel')}
        />
        <label>
          <span>Control width</span>
          <span className="dunit">
            <input
              type="range"
              min={MOTION_CONTROL_WIDTH_REM.min}
              max={MOTION_CONTROL_WIDTH_REM.max}
              step="0.25"
              value={preferences.controlWidthRem}
              onChange={(event) => setPreferences({ controlWidthRem: Number(event.target.value) })}
            />
            <output>{preferences.controlWidthRem.toFixed(2).replace(/\.00$/, '')} rem</output>
          </span>
        </label>
        <label className="toggle-control">
          <input
            type="checkbox"
            checked={preferences.confirmHalt}
            onChange={(event) => setPreferences({ confirmHalt: event.target.checked })}
          />
          <span>Confirm before halting the Program</span>
        </label>
        <label className="toggle-control">
          <input
            type="checkbox"
            checked={preferences.confirmReset}
            onChange={(event) => setPreferences({ confirmReset: event.target.checked })}
          />
          <span>Confirm before resetting a running Program</span>
        </label>
      </fieldset>
    </div>
  );
}

export function ProgramTransportControls({
  enabled,
  program,
  robot,
  observationTransport,
  onOpenRuntimeHistory,
  onOpenRobotControls,
  onOpenViewerSettings,
  viewerPreviewActive = false,
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
  observationTransport?: ExecutionObservationTransportStatus;
  onOpenRuntimeHistory?: () => void;
  onOpenRobotControls?: () => void;
  /** The gear opens the 3D viewer settings (which include these controls). */
  onOpenViewerSettings?: () => void;
  /** A Draft scene or target preview is visible in the viewer. */
  viewerPreviewActive?: boolean;
}) {
  const [detailsOpen, setDetailsOpen] = useState(false);
  const [haltIntent, setHaltIntent] = useState<'stopping' | 'confirm' | 'automatic' | null>(null);
  // Reset during execution ends the run and restarts the tree, so it is
  // confirmed like Halt: motion stops first, the question comes after.
  const [resetIntent, setResetIntent] = useState<'stopping' | 'confirm' | 'automatic' | null>(null);
  // Installation settings (UA7W.11); they never add a confirmation to Stop.
  const preferences = useComposerSettings().settings.motionPanel;
  const detailsButton = useRef<HTMLButtonElement>(null);
  const haltButton = useRef<HTMLButtonElement>(null);
  const resetButton = useRef<HTMLButtonElement>(null);
  const { state, cycleState } = program;
  const {
    selections, allGroup, visibleGroups, hiddenGroupCount,
  } = useTransportGroups(program);
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
  const allMotionQuiesced = allGroup?.state === 'ready' || allGroup?.state === 'held';
  // Paused on a failed node: Retry retries it and resumes the motion the
  // failure pause stopped (backend), so it replaces Run/Resume meanwhile.
  const failureOffer = state.controls?.recoveryOffer ?? null;
  // A disabled Retry still replaces Run/Resume: the pause must not be
  // bypassed, and its reason (e.g. containment not confirmed) is shown.
  const retryActions = failureOffer?.actions.filter(
    (action) => RETRY_ACTION_IDS.includes(action.actionId),
  ) ?? [];
  const retryAction = retryActions.find((action) => action.enabled)
    ?? retryActions[0]
    ?? null;
  const retryPending = retryAction !== null
    && state.controlPending === `recovery:${retryAction.actionId}`;
  const canRetry = enabled
    && retryAction !== null
    && retryAction.enabled
    && state.pending === null
    && state.controlPending === null;
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
    if (haltIntent !== 'automatic' || !allMotionQuiesced) return;
    setHaltIntent(null);
    void program.fullStop();
  }, [allMotionQuiesced, haltIntent, program]);

  // Ask only once every motion group has stopped (no groups: nothing moves).
  // A completed Halt has already contained all motion; while it settles,
  // wait for it rather than sending a motion Stop the owner would refuse.
  const motionSettled = allGroup === undefined || allMotionQuiesced || programFullStopped;
  useEffect(() => {
    if (resetIntent === 'stopping' && motionSettled) setResetIntent('confirm');
    if (resetIntent === 'automatic' && motionSettled) {
      setResetIntent(null);
      void program.reset();
    }
  }, [motionSettled, program, resetIntent]);

  const executionActive = cycleState === 'running' || programMotionRunning;
  // Name the Motion-panel button that continues the run, as it is labelled
  // there: Run for a group not started yet, Resume for a stopped one.
  const continueStates = new Set(selections.map((group) => group.state));
  const continueLabel = continueStates.has('held') && continueStates.has('ready')
    ? 'Run or Resume'
    : continueStates.has('held') ? 'Resume' : 'Run';
  const requestProgramReset = async () => {
    if (!canReset) return;
    if (!executionActive) {
      void program.reset();
      return;
    }
    if (!motionSettled) {
      setResetIntent(preferences.confirmReset ? 'stopping' : 'automatic');
      if (programFullStopping) return;
      const accepted = await program.stopMotion(['all']);
      if (!accepted) setResetIntent(null);
      return;
    }
    if (preferences.confirmReset) setResetIntent('confirm');
    else void program.reset();
  };

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
            // Run and Resume are one command: release the group's never-started
            // and stopped work. Only the label differs.
            const releasePending = selectionGroupPending(group.groupId, 'start-motion')
              || selectionGroupPending(group.groupId, 'resume-motion');
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
            const canGroupRelease = enabled
              && state.runnerAvailable
              && cycleState === 'running'
              && !manualResident
              && programMotionRunning
              && state.pending === null
              && !selectionTransitionPending
              && group.resumeAllowed
              && !releasePending
              && state.controlPending === null
              && program.commandAllowed('program_motion_resume');
            const releaseLabel = group.state === 'ready' ? 'Run' : 'Resume';
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
                  disabled={retryAction !== null ? !canRetry : !canGroupRelease}
                  title={retryAction !== null && failureOffer !== null
                    ? retryAction.enabled
                      ? `Retry ${failureOffer.nodeInstanceId} and let all the Program's motion continue.`
                      : retryAction.disabledReasonCode === 'failure_pause_containment_unconfirmed'
                        ? 'Retry waits until stopping the Program\'s other motion is confirmed.'
                        : `Retry is unavailable: ${(retryAction.disabledReasonCode ?? 'not allowed').replaceAll('_', ' ')}.`
                    : canGroupRelease
                      ? `${releaseLabel} ${group.groupId}: its stopped and not-yet-started motion continues.`
                      : group.state === 'running'
                        ? `${group.groupId} motion is running.`
                        : group.resumeUnavailableReason?.replaceAll('_', ' ')
                          ?? `${releaseLabel} is unavailable for this group.`}
                  onClick={() => {
                    if (retryAction !== null) {
                      if (canRetry) program.applyRecoveryAction(retryAction.actionId);
                    } else if (canGroupRelease) void program.resumeMotion([group.groupId]);
                  }}
                >{retryAction !== null ? (retryPending ? 'Retrying' : 'Retry')
                  : releasePending ? (group.state === 'ready' ? 'Starting' : 'Resuming')
                    : group.state === 'running' ? 'Running' : releaseLabel}</button>
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
                ? 'Resume the halted Program tree. Motion stays stopped until Run or Resume in the Motion panel.'
                : canRun
                  ? 'Execute the exact Applied Program without starting robot motion.'
                  : (cycleState === 'idle_after_reset' ? program.startBlockedReason : null)
                    ?? blockedReason
                    ?? 'Execute requires an Applied Program and an idle, reset runner.'}
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
            ref={resetButton}
            type="button"
            className={canReset ? 'transport-reset transport-unique-control meaningful' : 'transport-reset transport-unique-control'}
            disabled={!canReset || resetIntent !== null}
            title={canReset
              ? resetRequired ? 'Reset the completed Program cycle.'
                : executionActive
                  ? 'Stop all Program motion, then confirm ending this run and resetting the Program.'
                  : 'Reset the current Program cycle using its exact owner evidence.'
              : 'Reset is unavailable in the current backend control state.'}
            onClick={() => { void requestProgramReset(); }}
          >{resetIntent === 'stopping' || resetIntent === 'automatic' ? 'Stopping'
            : cycleState === 'resetting' || state.pending === 'reset'
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
            <button
              type="button"
              className="icon-button transport-unique-icon transport-settings-button"
              aria-label={hiddenGroupCount > 0
                ? `Open viewer settings; ${hiddenGroupCount} command groups hidden`
                : 'Open viewer settings'}
              title={viewerPreviewActive
                ? 'A Draft scene or target preview is visible. Open the viewer settings for layer details.'
                : '3D viewer settings'}
              disabled={onOpenViewerSettings === undefined}
              onClick={onOpenViewerSettings}
            >
              <span className="transport-settings-glyph" aria-hidden="true">⚙</span>
              {hiddenGroupCount > 0 && (
                <span
                  className="transport-hidden-group-count"
                  aria-hidden="true"
                >{hiddenGroupCount}</span>
              )}
              {viewerPreviewActive && hiddenGroupCount === 0 && (
                <span className="viewer-preview-dot" aria-hidden="true" />
              )}
            </button>
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
        {fullCellStop?.fault && fullCellStop.fault.state !== 'cleared' && (
          <section className="runtime-detail-group" aria-labelledby="program-stop-fault-title">
            <h4 id="program-stop-fault-title">Stop fault: motion is blocked</h4>
            <p className="inline-message severity-error" role="alert">
              A stop did not complete cleanly ({fullCellStop.fault.reasonCode.replaceAll('_', ' ')}).
              {fullCellStop.fault.recoveryStatus === 'ready'
                ? ' The cell can be recovered.'
                : ' The cell stays blocked until the backend confirms it is safe.'}
            </p>
            {robot.stopFaultRecoveryError && (
              <p className="inline-message severity-error">{robot.stopFaultRecoveryError}</p>
            )}
            <div className="runtime-detail-actions">
              <button
                type="button"
                className="runtime-detail-button"
                disabled={!enabled || robot.stopFaultRecoveryPending}
                title={fullCellStop.fault.recoveryStatus === 'ready'
                  ? 'Clear the stop fault and allow motion again.'
                  : 'Ask the backend to check again; it clears the fault only when every safety check passes.'}
                onClick={() => { void robot.recoverStopFault(); }}
              >
                {robot.stopFaultRecoveryPending
                  ? 'Checking recovery'
                  : fullCellStop.fault.recoveryStatus === 'ready' ? 'Recover cell' : 'Check again'}
              </button>
            </div>
          </section>
        )}
        <details className="runtime-detail-group runtime-detail-disclosure">
          <summary>How execution and motion work</summary>
          <dl className="runtime-stop-levels">
            <div>
              <dt>Execute</dt>
              <dd>Starts the Program tree. Motion does not start: each motion group waits for Run.</dd>
            </div>
            <div>
              <dt>Run / Resume</dt>
              <dd>In the Motion panel: lets a group move. It is one command: the button says Run when nothing in the group has moved yet, Resume otherwise. Stopped work continues from where the robot is now.</dd>
            </div>
            <div>
              <dt>Stop</dt>
              <dd>Stops one group&apos;s motion. The tree keeps its place; Resume continues.</dd>
            </div>
            <div>
              <dt>Retry</dt>
              <dd>Shown instead of Run/Resume when the Program paused on a failed node: retries that node and lets all the Program&apos;s stopped and not-yet-started motion continue. To start over, use Reset.</dd>
            </div>
            <div>
              <dt>Halt</dt>
              <dd>Stops all Program motion and pauses the tree. Execute resumes the tree; motion stays stopped until Run or Resume.</dd>
            </div>
            <div>
              <dt>Reset</dt>
              <dd>Ends the run and returns Scene objects to the Program&apos;s initial Scene; the next Execute starts from the beginning.</dd>
            </div>
            <div>
              <dt>Magenta node</dt>
              <dd>The node is running but its work waits for Run, Resume or Retry.</dd>
            </div>
            <div>
              <dt>Closing Composer</dt>
              <dd>Closing or reloading Composer only stops watching: it never sends Stop and cannot change robot work already sent.</dd>
            </div>
            <div>
              <dt>Failure pause</dt>
              <dd>A node allowed to pause for the operator (its Recovery policy) failed: the tree waits and all Program motion is stopped until Retry or Reset.</dd>
            </div>
          </dl>
        </details>
        <details className="runtime-detail-group runtime-detail-disclosure">
          <summary>3D viewer legend</summary>
          <dl className="runtime-stop-levels">
            <div>
              <dt>Purple object</dt>
              <dd>A Scene object as the owner currently has it.</dd>
            </div>
            <div>
              <dt>Light blue object</dt>
              <dd>The selected object.</dd>
            </div>
            <div>
              <dt>Green object</dt>
              <dd>An object attached to a robot or gripper.</dd>
            </div>
            <div>
              <dt>Amber, see-through</dt>
              <dd>The Program Draft&apos;s Scene: a preview, not the current Scene.</dd>
            </div>
            <div>
              <dt>Robot ghost</dt>
              <dd>A manual-move target preview: cyan when validated, amber while pending, red when rejected. It never moves the robot; only Approve does.</dd>
            </div>
            <div>
              <dt>Orange wireframe</dt>
              <dd>Collision geometry, when shown.</dd>
            </div>
            <div>
              <dt>Light edges</dt>
              <dd>Object outline, when shown: one edge per robot, gripper or object.</dd>
            </div>
            <div>
              <dt>Settings</dt>
              <dd>The gear next to this button opens the viewer settings: what is shown, frames and axes, and these control panels.</dd>
            </div>
          </dl>
        </details>
        <details className="runtime-detail-group runtime-detail-disclosure">
          <summary>Status colours</summary>
          <div className="status-legend">
            <h4>Deployment button: runtime health</h4>
            <dl className="status-legend-rows">
              {([
                ['ready', 'Green', 'every component is ready.'],
                ['degraded', 'Amber', 'degraded: some components need attention.'],
                ['fault', 'Red', 'a component is faulted.'],
                ['offline', 'Dark red', 'offline: no component status.'],
                ['waiting', 'Grey', 'waiting for the first status.'],
              ] as const).map(([readiness, colour, text]) => (
                <div key={readiness}>
                  <dt><span className={`status-legend-button readiness-${readiness}`}>Deployment</span></dt>
                  <dd><strong>{colour}</strong>: {text}</dd>
                </div>
              ))}
            </dl>
            <h4>Dot on the Deployment button</h4>
            <dl className="status-legend-rows">
              <div>
                <dt><span className="status-legend-dot"><span className="deployment-status-indicator" /></span></dt>
                <dd><strong>Green</strong>: the live status feed is connected.</dd>
              </div>
              <div>
                <dt><span className="status-legend-dot offline"><span className="deployment-status-indicator" /></span></dt>
                <dd><strong>Grey</strong>: live status unavailable; what you see may be stale.</dd>
              </div>
              <div>
                <dt><span className="status-legend-dot"><span className="deployment-status-indicator has-attention">2</span></span></dt>
                <dd><strong>Number</strong>: alarms or errors to review in Runtime / history.</dd>
              </div>
            </dl>
            <h4>D / S / A beside the workspace buttons</h4>
            <dl className="status-legend-rows">
              <div>
                <dt>
                  <span className="status-legend-letters">
                    <span className="document-state document-state-letter state-draft">D</span>
                    <span className="document-state document-state-letter state-saved">S</span>
                    <span className="document-state document-state-letter state-different">A</span>
                  </span>
                </dt>
                <dd>
                  <strong>Draft, Saved, Applied</strong> of the Program (left, Scene edits
                  included) and of the Deployment (right). They appear only while
                  something differs. Green matches, amber differs: Save, or Apply
                  (Execute waits until Apply or Discard). Hover a letter for details.
                </dd>
              </div>
            </dl>
          </div>
        </details>
        <details className="runtime-detail-group runtime-detail-disclosure">
          <summary>Current setup</summary>
          <ProgramRunSetup
            applied={state.applied}
            targets={robot.currentStatus?.directControlTargets ?? []}
            groups={selections.map((group) => {
              const scale = program.groupRuntimeScales[group.groupId];
              const requested = scale?.draft ?? scale?.status?.requestedScale ?? null;
              return {
                groupId: group.groupId,
                nodeCount: group.nodeCount,
                state: group.state,
                velocity: requested === null ? '—' : `${Math.round(requested * 100)}%`,
              };
            })}
          />
        </details>
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
      </OverlayPanel>
      <OverlayPanel
        open={resetIntent === 'confirm'}
        title="Reset the running Program?"
        titleId="program-reset-confirmation-title"
        kind="dialog"
        compact
        onClose={() => setResetIntent(null)}
        returnFocusRef={resetButton}
      >
        <div className="dialog-copy transport-confirmation">
          <p>Motion is stopped. Reset also ends this run:</p>
          <ul>
            <li>its progress is discarded;</li>
            <li>Scene objects return to the Program's initial Scene;</li>
            <li>the next Execute starts the tree from the beginning.</li>
          </ul>
          <p>
            {programFullStopped
              ? 'Cancel keeps the run halted; Execute resumes it.'
              : `Cancel keeps the run stopped; ${continueLabel} in the Motion panel continues it.`}
          </p>
          <div className="dialog-actions">
            <button type="button" onClick={() => setResetIntent(null)}>Cancel</button>
            <button
              type="button"
              className="danger-button"
              disabled={!canReset || !motionSettled}
              onClick={() => {
                setResetIntent(null);
                void program.reset();
              }}
            >Reset Program</button>
          </div>
        </div>
      </OverlayPanel>
      <OverlayPanel
        open={haltIntent === 'confirm'}
        title="Halt Program execution?"
        titleId="program-halt-confirmation-title"
        kind="dialog"
        compact
        onClose={() => setHaltIntent(null)}
        returnFocusRef={haltButton}
      >
        <div className="dialog-copy transport-confirmation">
          <p>
            Motion is stopped. Halt also pauses the Program tree; Execute
            resumes it later, with motion still stopped until Run or Resume.
            Choose Cancel if you only meant to stop motion.
          </p>
          {!allMotionQuiesced && (
            <p role="status">Waiting for all motion groups to stop…</p>
          )}
          <div className="dialog-actions">
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
        </div>
      </OverlayPanel>
    </>
  );
}
