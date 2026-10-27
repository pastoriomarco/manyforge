import {
  useEffect, useMemo, useRef, useState, type CSSProperties, type RefObject,
} from 'react';

import {
  PROGRAM_RESET_READINESS_MESSAGE,
  programMotionStopSettled,
  type ProgramResetOptions,
  type ProgramRuntimeController,
} from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type {
  AutomaticRetryAuditEvent,
  CycleRecoveryOffer,
  JsonValue,
  ProgramInterventionStopScope,
  RuntimeControlAuditEvent,
  TakeoverFallbackAuditEvent,
} from '../types/api';
import type { ExecutionObservationTransportStatus } from '../types/viewerObservation';
import { manualOperationRequiresRecovery, robotControlHasActiveOperation } from '../state/robotControl';
import {
  MOTION_PRODUCING_RECOVERY_ACTIONS,
  recoveryOfferResourceIds,
} from '../state/operatorAlarmMotionAttention';
import {
  appliedExecutionSelectionRows, executionGroupDisplay, executionGroupScopes,
  executionGroupTargetLabels, executionGroupVisibilityChange, executionGroupVisibilityUnavailableReason,
  executionSelectionIsQuiescent, type ExecutionGroupFallback, type ExecutionGroupScope,
} from '../state/programExecutionGroups';
import { useComposerSettings } from '../hooks/useComposerSettings';
import { MOTION_CONTROL_WIDTH_REM, type ComposerSettings } from '../state/composerSettings';
import { OverlayPanel } from './OverlayPanel';
import { NamespaceGroupOptions, useNamespaceGroups } from './common/NamespaceGroupOptions';
import { groupByNamespace } from '../utils/blackboardNamespaces';
import { incidentFacts } from '../state/componentLifecyclePresentation';
import { PendingApplyNote, SettingsSubheading } from './SettingsScope';
import { ProgramRunSetup, type AdmittedSpeedsView } from './ProgramRunSetup';
import { api } from '../api/client';
import {
  isAutomaticRetryAuditEvent,
  isTakeoverFallbackAuditEvent,
  parseApiErrorDto,
} from '../api/parsers';
import {
  BlackboardValueEditor,
  blackboardValueDraft,
  blackboardValueDraftIssue,
  blackboardValueEditable,
  parseBlackboardValueDraft,
} from './BlackboardValueEditor';
import { PersistentKeyValuesSection } from './PersistentKeyValues';
import { UncertainOutputConfirmation } from './UncertainOutputConfirmation';
import { globalRuleLatchNotices, type GlobalRuleLatch } from '../state/globalRuleLatches';
import { useGlobalRuleRecovery } from '../hooks/useGlobalRuleRecovery';
import { GLOBAL_INTERRUPT_GATED, motionWaitRefusalMessage } from '../state/motionWaitRefusal';
import {
  allUncertainOutputsApproved,
  mergeUncertainOutputs,
  programRunUncertainOutputs,
  uncertainOutputs,
} from '../utils/uncertainOutputs';
import {
  useBriefNotice,
  usePublishWorkbenchNotices,
  type WorkbenchNotice,
} from '../state/workbenchNotices';
import {
  formatStructuredError,
  humanizeReasonCode,
  structuredErrorText,
} from '../state/structuredError';
import { useExactAlarmAcknowledgement } from '../hooks/useExactAlarmAcknowledgement';


/** One recovery command as the Motion or Program panel offers it. */
export interface ProgramRecoveryCommand {
  label: string;
  enabled: boolean;
  /** Why it is disabled, shown next to it. */
  reason: string | null;
  run: () => void;
}

/**
 * The held offer's commands for the Recovery block: the Motion panel's Retry
 * and Continue and the Program panel's Reset, with their handlers and
 * enabled state, plus acknowledgement of the alarms that block them.
 */
export interface ProgramRecoveryCommands {
  acknowledge: { busy: boolean; error: string | null; run: () => void } | null;
  retry: ProgramRecoveryCommand | null;
  continue: ProgramRecoveryCommand | null;
  reset: ProgramRecoveryCommand;
}

interface ProgramRuntimePanelProps {
  enabled: boolean;
  controller: ProgramRuntimeController;
  recovery?: ProgramRecoveryCommands | null;
  recoveryRef?: RefObject<HTMLElement | null>;
}

const RECOVERY_COMMAND_MEANING = {
  retry: 'Re-run the node.',
  continue: 'Fail the node; the Program tree handles the FAILURE.',
  reset: 'End the run.',
} as const;

function RecoveryCommandRow({
  command,
  meaning,
  className,
}: {
  command: ProgramRecoveryCommand;
  meaning: string;
  className?: string;
}) {
  return (
    <div className="program-recovery-command">
      <button
        type="button"
        className={className}
        disabled={!command.enabled}
        onClick={command.run}
      >{command.label}</button>
      <span>{meaning}</span>
      {command.reason !== null && (
        <span className="inline-message severity-error">{command.reason}</span>
      )}
    </div>
  );
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

const AUTOMATIC_RETRY_CAUSE_LABELS: Readonly<Record<string, string>> = {
  collision_stop_nominal: 'collision Stop',
  planning_no_effect: 'planning failed',
};

/** The cause as words; an unnamed cause is humanised like any stable code. */
function automaticRetryCauseLabel(cause: string): string {
  return AUTOMATIC_RETRY_CAUSE_LABELS[cause] ?? humanizeReasonCode(cause);
}

/**
 * The failed attempt, as its Kernel failure: "Collision Stop on move: <code> ·
 * <message>", or the cause alone when the owner has no Kernel failure. Shown in
 * red before the retry line. Null for another event.
 */
export function automaticRetryFailureText(event: RuntimeControlAuditEvent): string | null {
  if (!isAutomaticRetryAuditEvent(event)) return null;
  const { nodeInstanceId, cause, failure } = event.facts;
  const head = `${actionLabel(automaticRetryCauseLabel(cause))} on ${nodeInstanceId}`;
  if (failure === null) return head;
  const { title, message } = formatStructuredError(failure);
  return `${head}: ${title} · ${message}`;
}

/** "Attempt 2 of 4 for move after collision Stop" (an automatic retry), or null for another event. */
export function automaticRetryText(event: RuntimeControlAuditEvent): string | null {
  if (!isAutomaticRetryAuditEvent(event)) return null;
  const {
    nodeInstanceId, attempt, maxAttempts, cause,
  } = event.facts;
  return `Attempt ${attempt} of ${maxAttempts} for ${nodeInstanceId} after ${automaticRetryCauseLabel(cause)}`;
}

/** The brief warning for one automatic retry: "Collision Stop on move · attempt 2 of 4". */
function automaticRetryBriefNotice(
  programRunId: string | null,
  event: AutomaticRetryAuditEvent,
): WorkbenchNotice {
  const {
    nodeInstanceId, attempt, maxAttempts, cause,
  } = event.facts;
  return {
    id: `program.automatic-retry.${programRunId}.${event.eventRevision}`,
    severity: 'warning',
    domain: 'runtime',
    title: `${actionLabel(automaticRetryCauseLabel(cause))} on ${nodeInstanceId} · attempt ${attempt} of ${maxAttempts}`,
    message: 'Recorded in the runtime-control events.',
  };
}

function newestAutomaticRetry(
  audit: readonly RuntimeControlAuditEvent[],
): AutomaticRetryAuditEvent | null {
  return audit.findLast(isAutomaticRetryAuditEvent) ?? null;
}

/**
 * "Spring-back fallback on move · planning timeout during <stage> (prep 534 of
 * 500 ms)", the stage only when the Kernel names it: a motion
 * takeover that did not complete as a moving replacement. Null for another event.
 */
export function takeoverFallbackText(event: RuntimeControlAuditEvent): string | null {
  if (!isTakeoverFallbackAuditEvent(event)) return null;
  const {
    nodeInstanceId, path, code, failureStage, preparationMs, budgetMs,
  } = event.facts;
  const head = path === 'spring_back_hold' ? 'Spring-back fallback' : 'Stop fallback';
  const label = [
    code === null ? null : humanizeReasonCode(code),
    failureStage === null ? null : `during ${failureStage}`,
  ].filter((part) => part !== null).join(' ');
  const reason = label === '' ? '' : ` · ${label}`;
  return `${head} on ${nodeInstanceId}${reason} (prep ${Math.round(preparationMs)} of ${Math.round(budgetMs)} ms)`;
}

/** The brief warning for one takeover fallback (like an automatic retry's). */
function takeoverFallbackBriefNotice(
  programRunId: string | null,
  event: TakeoverFallbackAuditEvent,
): WorkbenchNotice {
  return {
    id: `program.takeover-fallback.${programRunId}.${event.eventRevision}`,
    severity: 'warning',
    domain: 'runtime',
    title: takeoverFallbackText(event) as string,
    message: 'Recorded in the runtime-control events.',
  };
}

function newestTakeoverFallback(
  audit: readonly RuntimeControlAuditEvent[],
): TakeoverFallbackAuditEvent | null {
  return audit.findLast(isTakeoverFallbackAuditEvent) ?? null;
}

const IO_SOURCE_UNAVAILABLE = 'program_node_io_source_unavailable';
const IO_SOURCE_AVAILABLE = 'program_node_io_source_available';

interface IoSourceTransition {
  unavailable: boolean;
  nodeInstanceId: string;
  quality: string | null;
}

/** The facts of an io wait's source transition, or null for another event. */
function ioSourceTransition(event: RuntimeControlAuditEvent): IoSourceTransition | null {
  if (event.kind !== IO_SOURCE_UNAVAILABLE && event.kind !== IO_SOURCE_AVAILABLE) return null;
  const facts = event.facts as Record<string, JsonValue>;
  if (typeof facts.node_instance_id !== 'string') return null;
  return {
    unavailable: event.kind === IO_SOURCE_UNAVAILABLE,
    nodeInstanceId: facts.node_instance_id,
    quality: typeof facts.quality === 'string' ? facts.quality : null,
  };
}

/**
 * "Input source unavailable for wait_part (stale) · still waiting" /
 * "Input source available again for wait_part", or null for another event.
 */
export function ioSourceAuditText(event: RuntimeControlAuditEvent): string | null {
  const transition = ioSourceTransition(event);
  if (transition === null) return null;
  return transition.unavailable
    ? `Input source unavailable for ${transition.nodeInstanceId}${transition.quality === null ? '' : ` (${transition.quality})`} · still waiting`
    : `Input source available again for ${transition.nodeInstanceId}`;
}

/**
 * The running io waits whose newest source transition is "unavailable": the
 * Runner keeps such a node RUNNING and records the transition as an event.
 */
export function ioSourceUnavailableNodes(
  audit: readonly RuntimeControlAuditEvent[],
  nodeStatuses: Readonly<Record<string, string>>,
): IoSourceTransition[] {
  const newest = new Map<string, IoSourceTransition>();
  for (const event of audit) {
    const transition = ioSourceTransition(event);
    if (transition !== null) newest.set(transition.nodeInstanceId, transition);
  }
  return [...newest.values()].filter((transition) => (
    transition.unavailable && nodeStatuses[transition.nodeInstanceId] === 'running'
  ));
}

const MOTION_GATED = 'program_node_motion_gated';
const MOTION_GATE_RELEASED = 'program_node_motion_gate_released';

interface MotionGateWait {
  waiting: boolean;
  nodeInstanceId: string;
  ruleId: string;
}

/** A motion node's gate wait transition (Rule 2), or null for another event. */
function motionGateTransition(event: RuntimeControlAuditEvent): MotionGateWait | null {
  if (event.kind !== MOTION_GATED && event.kind !== MOTION_GATE_RELEASED) return null;
  const facts = event.facts as Record<string, JsonValue>;
  if (typeof facts.node_instance_id !== 'string' || typeof facts.rule_id !== 'string') return null;
  return { waiting: event.kind === MOTION_GATED, nodeInstanceId: facts.node_instance_id, ruleId: facts.rule_id };
}

/**
 * The running motion nodes whose newest gate transition is "waiting": a gate
 * rule refused their motion and the Runner keeps them RUNNING until it clears.
 */
export function motionGateWaitNodes(
  audit: readonly RuntimeControlAuditEvent[],
  nodeStatuses: Readonly<Record<string, string>>,
): MotionGateWait[] {
  const newest = new Map<string, MotionGateWait>();
  for (const event of audit) {
    const transition = motionGateTransition(event);
    if (transition !== null) newest.set(transition.nodeInstanceId, transition);
  }
  return [...newest.values()].filter((wait) => (
    wait.waiting && nodeStatuses[wait.nodeInstanceId] === 'running'
  ));
}

/** "Motion waits: global interrupt rule X is active. …" for one node. */
export function motionGateWaitText(wait: MotionGateWait): string {
  return `${wait.nodeInstanceId}: ${motionWaitRefusalMessage(GLOBAL_INTERRUPT_GATED, { rule_id: wait.ruleId })}`;
}

/** The audit row of a gate wait transition, or null for another event. */
export function motionGateAuditText(event: RuntimeControlAuditEvent): string | null {
  const transition = motionGateTransition(event);
  if (transition === null) return null;
  return transition.waiting
    ? `Motion of ${transition.nodeInstanceId} waits for global interrupt rule ${transition.ruleId}`
    : `Global interrupt rule ${transition.ruleId} cleared: motion of ${transition.nodeInstanceId} admitted`;
}

function ioSourceUnavailableText(transition: IoSourceTransition): string {
  return `Waiting on ${transition.nodeInstanceId}: input source unavailable${transition.quality === null ? '' : ` (${transition.quality})`}. The wait continues once the source is fresh again.`;
}

function newestIoSourceUnavailable(
  audit: readonly RuntimeControlAuditEvent[],
): RuntimeControlAuditEvent | null {
  return audit.findLast((event) => ioSourceTransition(event)?.unavailable === true) ?? null;
}

function ioSourceBriefNotice(
  programRunId: string | null,
  event: RuntimeControlAuditEvent,
): WorkbenchNotice {
  const transition = ioSourceTransition(event) as IoSourceTransition;
  return {
    id: `program.io-source-unavailable.${programRunId}.${event.eventRevision}`,
    severity: 'warning',
    domain: 'runtime',
    title: `Input source unavailable on ${transition.nodeInstanceId} · still waiting`,
    message: 'Recorded in the runtime-control events.',
  };
}

/**
 * The brief warning for the newest event `newestOf` selects that the panel
 * has not yet announced. The first loaded audit is history and announces
 * nothing, so an event already in it never re-shows, even after a remount. A
 * later revision, or the first such event of a new run, is new. `useBriefNotice`
 * keys the notice by its run and revision, so an unchanged event never
 * restarts its timer.
 */
function useBriefNewestAuditEvent<E extends RuntimeControlAuditEvent>(
  programRunId: string | null,
  audit: readonly RuntimeControlAuditEvent[] | null,
  newestOf: (audit: readonly RuntimeControlAuditEvent[]) => E | null,
  noticeOf: (programRunId: string | null, event: E) => WorkbenchNotice,
): WorkbenchNotice | null {
  const [baseline, setBaseline] = useState<{ runId: string | null; revision: number } | null>(null);
  if (audit !== null && baseline === null) {
    setBaseline({ runId: programRunId, revision: newestOf(audit)?.eventRevision ?? 0 });
  }
  const newest = audit === null ? null : newestOf(audit);
  const fresh = newest !== null && baseline !== null
    && (baseline.runId !== programRunId || newest.eventRevision > baseline.revision)
    ? newest
    : null;
  return useBriefNotice(fresh === null ? null : noticeOf(programRunId, fresh));
}

/** The brief warning for a newly unavailable io wait source (see the retry notice). */
export function useBriefIoSourceUnavailable(
  programRunId: string | null,
  audit: readonly RuntimeControlAuditEvent[] | null,
): WorkbenchNotice | null {
  return useBriefNewestAuditEvent(
    programRunId,
    audit,
    newestIoSourceUnavailable,
    ioSourceBriefNotice,
  );
}

/**
 * The brief warning for the newest automatic retry the panel has not yet
 * announced. The first loaded audit is history and announces nothing, so a
 * retry already in it never re-shows, even after a remount. A later revision,
 * or the first retry of a new run, is new. `useBriefNotice` keys the notice by
 * its run and revision, so an unchanged retry never restarts its timer.
 */
export function useBriefAutomaticRetry(
  programRunId: string | null,
  audit: readonly RuntimeControlAuditEvent[] | null,
): WorkbenchNotice | null {
  return useBriefNewestAuditEvent(
    programRunId,
    audit,
    newestAutomaticRetry,
    automaticRetryBriefNotice,
  );
}

/** The brief warning for the newest takeover fallback (see the retry notice). */
export function useBriefTakeoverFallback(
  programRunId: string | null,
  audit: readonly RuntimeControlAuditEvent[] | null,
): WorkbenchNotice | null {
  return useBriefNewestAuditEvent(
    programRunId,
    audit,
    newestTakeoverFallback,
    takeoverFallbackBriefNotice,
  );
}

function actionLabel(value: string): string {
  const label = value.replace(/^recovery\./, '').replaceAll('_', ' ');
  return label.charAt(0).toUpperCase() + label.slice(1);
}

function cycleFailureMessage(failure: Record<string, JsonValue> | null): string | null {
  const structured = parseApiErrorDto({ detail: failure });
  if (structured !== null) return structuredErrorText(structured);
  const message = failure?.message;
  if (typeof message === 'string' && message.length > 0) return message;
  const code = failure?.code;
  return typeof code === 'string' ? humanizeReasonCode(code) : null;
}

export function ProgramRuntimePanel({
  enabled,
  controller,
  recovery = null,
  recoveryRef,
}: ProgramRuntimePanelProps) {
  const {
    state,
    cycleState,
    cycleFailure,
    refresh,
    writeRuntimeBlackboard,
    persistentValues,
    commandAllowed,
    commandDisabledReason,
    stageManualResult,
  } = controller;
  const [blackboardKey, setBlackboardKey] = useState('');
  const keyNamespaces = useNamespaceGroups();
  const [blackboardDraft, setBlackboardDraft] = useState('');
  const [blackboardPoseIssue, setBlackboardPoseIssue] = useState<string | null>(null);
  const [blackboardError, setBlackboardError] = useState<string | null>(null);
  const [blackboardWritten, setBlackboardWritten] = useState<string | null>(null);
  const [manualOutput, setManualOutput] = useState('null');
  const [manualError, setManualError] = useState<string | null>(null);
  const controls = state.controls?.runtimeControls ?? null;
  const recoveryOffer = state.controls?.recoveryOffer ?? null;
  const ordinaryMotionHeld = recoveryOffer?.recoveryClass === 'ordinary_operator_stop';
  const ioSourceUnavailable = state.run === null || controls === null
    ? []
    : ioSourceUnavailableNodes(controls.audit, state.run.nodeStatuses);
  const motionGateWaits = state.run === null || controls === null
    ? []
    : motionGateWaitNodes(controls.audit, state.run.nodeStatuses);
  const runReason = state.run?.reason ? formatStructuredError(state.run.reason) : null;
  // A held failure carries its cause on the offer; the run's failure is set
  // only once the cycle itself requires recovery.
  const failureHold = recoveryOffer !== null && !ordinaryMotionHeld;
  const recoveryCause = cycleFailureMessage(cycleFailure)
    ?? (failureHold ? recoveryOffer.reason.message || humanizeReasonCode(recoveryOffer.reason.code) : null);
  const declaredKeys = state.applied?.program.blackboard?.keys ?? [];
  // Operator writes follow the key's access and use its typed editor; a
  // computed key always holds its formula's result.
  const writableKeys = declaredKeys.filter((key) => (
    (key.access === 'write' || key.access === 'read_write')
    && key.expression === undefined
    && blackboardValueEditable(key.type)
  ));
  const hasPersistentKeys = declaredKeys.some((key) => key.persistence === 'persistent');
  const sceneResourceIds = useMemo(
    () => (state.applied?.program.scene ?? []).map((resource) => resource.scene_resource_id),
    [state.applied],
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
    (controls !== null && controls.audit.length > 0)
    || (state.run !== null && writableKeys.length > 0)
    || hasPersistentKeys
  );
  const selectedBlackboardKey = writableKeys.find(
    (key) => key.key === blackboardKey,
  ) ?? null;
  // The backend says when writes are refused (for example after Halt).
  const blackboardWriteRefusal = commandAllowed('program_runtime_blackboard_write')
    ? null
    : commandDisabledReason('program_runtime_blackboard_write')
      ?? 'writes are allowed only while the run is running or paused on a failure';
  const blackboardIssue = selectedBlackboardKey === null
    ? null
    : blackboardPoseIssue ?? blackboardValueDraftIssue(
      selectedBlackboardKey.type,
      blackboardDraft,
      sceneResourceIds,
    );

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
                  ? `${state.nonExecutable.origin.kind === 'scenario' ? state.nonExecutable.origin.scenarioId : state.nonExecutable.origin.cellId} · ${state.nonExecutable.programId === null ? 'no Program' : 'not executable in this Deployment'}`
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
        <div className="program-feedback error-state" role="status">
          <span>{cycleFailureMessage(cycleFailure)}</span>
        </div>
      ) : ordinaryMotionHeld ? (
        <div className="program-feedback" role="status">
          <span>Program motion stopped and held. Resume Motion replans from current state.</span>
        </div>
      ) : runReason !== null && (
        <div className={`program-feedback${failureHold ? ' error-state' : ''}`} role="status">
          <span>
            {runReason.title}
            {runReason.message ? `: ${runReason.message}` : ''}
            {runReason.rows.map((row) => (
              <small key={row.label}>{` · ${row.label}: ${row.value}`}</small>
            ))}
          </span>
        </div>
      )}
      {ioSourceUnavailable.map((transition) => (
        <p
          key={transition.nodeInstanceId}
          className="inline-message severity-warning"
          role="status"
        >
          {ioSourceUnavailableText(transition)}
        </p>
      ))}
      {motionGateWaits.map((wait) => (
        <p
          key={wait.nodeInstanceId}
          className="inline-message severity-warning"
          role="status"
        >
          {motionGateWaitText(wait)}
        </p>
      ))}
      {enabled && recoveryOffer !== null && (
        <section
          ref={recoveryRef}
          tabIndex={-1}
          className={`runtime-control-section${recoveryCause === null ? '' : ' program-recovery-failed'}`}
          aria-labelledby="runtime-recovery-title"
        >
          <div className="runtime-control-heading">
            <div>
              <span>Current held attempt</span>
              <h3 id="runtime-recovery-title">Recovery</h3>
            </div>
            <code>offer {recoveryOffer.offerRevision}</code>
          </div>
          {recoveryCause !== null && <p className="program-recovery-cause">{recoveryCause}</p>}
          <p>
            {actionLabel(recoveryOffer.recoveryClass)} · node{' '}
            <code>{recoveryOffer.nodeInstanceId}</code>
          </p>
          <ol className="program-recovery-steps">
            {recovery?.acknowledge != null && (
              <li>
                <strong>Acknowledge the alarm.</strong>{' '}
                Retry and Continue are blocked until it is acknowledged.
                <div className="program-recovery-command">
                  <button
                    type="button"
                    disabled={recovery.acknowledge.busy}
                    title="Acknowledge these exact alarm activations and revisions. This does not clear the condition or resume motion."
                    onClick={recovery.acknowledge.run}
                  >{recovery.acknowledge.busy ? 'Acknowledging' : 'Acknowledge'}</button>
                  {recovery.acknowledge.error !== null && (
                    <span className="inline-message severity-error" role="alert">
                      {recovery.acknowledge.error}
                    </span>
                  )}
                </div>
              </li>
            )}
            <li>
              <strong>Fix the cause.</strong>{' '}
              To correct its inputs, change a runtime value for this run under
              Advanced below, or edit the Draft: Apply then Resets the run, and
              Retry, Resume and Execute wait until the edits are Applied or
              Discarded.
            </li>
            {recovery !== null && (
              <li>
                <strong>Go on.</strong>
                {recovery.retry !== null && (
                  <RecoveryCommandRow
                    command={recovery.retry}
                    meaning={RECOVERY_COMMAND_MEANING.retry}
                    className="transport-group-resume"
                  />
                )}
                {recovery.continue !== null && (
                  <RecoveryCommandRow
                    command={recovery.continue}
                    meaning={RECOVERY_COMMAND_MEANING.continue}
                    className="danger-button"
                  />
                )}
                <RecoveryCommandRow
                  command={recovery.reset}
                  meaning={RECOVERY_COMMAND_MEANING.reset}
                  className="transport-reset"
                />
              </li>
            )}
          </ol>
          <p className="hint">
            Retry limit {recoveryOffer.policy.maxRetries === 0
              ? 'Unlimited'
              : `${recoveryOffer.policy.maxRetries}`} · {recoveryOffer.retriesUsed} used
            {recoveryOffer.policy.allowIntervention ? '' : ' · intervention not allowed'}
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
        <summary>Advanced: runtime blackboard, stored values and audit</summary>
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
                if (keyNamespaces.handleToggle(event.currentTarget.value)) return;
                const key = writableKeys.find((row) => row.key === event.currentTarget.value);
                setBlackboardKey(event.currentTarget.value);
                setBlackboardDraft(key === undefined
                  ? ''
                  : blackboardValueDraft(key.type, key.initial, sceneResourceIds));
                setBlackboardPoseIssue(null);
                setBlackboardError(null);
                setBlackboardWritten(null);
              }}
            >
              <option value="">Choose a key</option>
              <NamespaceGroupOptions
                groups={groupByNamespace(writableKeys, (key) => key.key)}
                expanded={keyNamespaces.expanded}
                current={blackboardKey}
                valueOf={(key) => key.key}
                option={(key) => (
                  <option key={key.key} value={key.key}>
                    {key.key} · {key.type}
                  </option>
                )}
              />
            </select>
          </label>
          {selectedBlackboardKey !== null && (
            <BlackboardValueEditor
              // Another key starts its own fields: unfinished text of the
              // previous key's pose is not shown over the new draft.
              key={selectedBlackboardKey.key}
              id="runtime-blackboard-value"
              label={`Value (${selectedBlackboardKey.type})`}
              type={selectedBlackboardKey.type}
              draft={blackboardDraft}
              sceneResourceIds={sceneResourceIds}
              frameScope={state.applied === null ? undefined : {
                program: state.applied.program,
                ...(selectedBlackboardKey.allowed_frames === undefined
                  ? {}
                  : { allowedFrames: selectedBlackboardKey.allowed_frames }),
              }}
              disabled={state.controlPending !== null}
              onDraftChange={(draft) => {
                setBlackboardDraft(draft);
                setBlackboardError(null);
                setBlackboardWritten(null);
              }}
              onInvalidChange={setBlackboardPoseIssue}
            />
          )}
          {blackboardWriteRefusal !== null && (
            <p className="hint" role="status">
              Writes are refused now: {blackboardWriteRefusal.replaceAll('_', ' ')}.
            </p>
          )}
          {blackboardIssue !== null && (
            <p className="inline-message severity-error">{blackboardIssue}</p>
          )}
          {blackboardError && (
            <p className="inline-message severity-error" role="alert">
              Not written: {blackboardError}
            </p>
          )}
          {blackboardWritten && <p className="hint" role="status">{blackboardWritten}</p>}
          <button
            type="button"
            disabled={
              selectedBlackboardKey === null
              || blackboardIssue !== null
              || blackboardWriteRefusal !== null
              || state.controlPending !== null
            }
            onClick={() => {
              if (selectedBlackboardKey === null) return;
              const { key, type } = selectedBlackboardKey;
              const value = parseBlackboardValueDraft(type, blackboardDraft, sceneResourceIds);
              setBlackboardError(null);
              setBlackboardWritten(null);
              void writeRuntimeBlackboard(key, value).then((refusal) => {
                if (refusal === null) setBlackboardWritten(`Wrote ${key}.`);
                else setBlackboardError(refusal);
              });
            }}
          >
            Write current-run value
          </button>
        </section>
      )}

      {enabled && hasPersistentKeys && state.applied !== null && (
        <PersistentKeyValuesSection
          projectPath={state.applied.projectPath}
          appliedRevision={state.applied.programRevision}
          runId={state.run?.capture.programRunId ?? null}
          sceneResourceIds={sceneResourceIds}
          frameProgram={state.applied.program}
          client={persistentValues}
        />
      )}

      {controls !== null && controls.audit.length > 0 && (
        <details className="runtime-control-section">
          <summary>Recent runtime-control events ({controls.audit.length})</summary>
          {controls.audit[0].eventRevision > 1 && (
            <p>Earlier events are outside this recent window. Use Runtime / history for retained operational history.</p>
          )}
          <ol className="runtime-control-audit">
            {controls.audit.map((event) => {
              const failure = automaticRetryFailureText(event);
              return (
                <li key={event.eventRevision}>
                  {failure !== null && <p className="inline-message severity-error">{failure}</p>}
                  <code>{event.eventRevision}</code>{' '}
                  {ioSourceTransition(event)?.unavailable === true ? (
                    <span className="inline-message severity-warning">{ioSourceAuditText(event)}</span>
                  ) : automaticRetryText(event)
                    ?? takeoverFallbackText(event)
                    ?? ioSourceAuditText(event)
                    ?? motionGateAuditText(event)
                    ?? actionLabel(event.kind)}
                </li>
              );
            })}
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
  // The held node's own failure first: parallel branches can hold several
  // nodes, and the run status names only the run's reason.
  const programRecoveryMessage = cycleFailureMessage(cycleFailure)
    ?? (recoveryOffer == null ? null : structuredErrorText(recoveryOffer.reason))
    ?? (state.run?.reason ? structuredErrorText(state.run.reason) : state.run?.message)
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
  globalRuleLatches = [],
  onClearGlobalRuleLatch,
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
  observationTransport?: ExecutionObservationTransportStatus;
  motionActive: boolean;
  onOpenManualRecovery?: (operation: ManualRecoveryOperation) => void;
  onReviewProgramRecovery: () => void;
  /** Latched global rules (contracts 78/86) from the owner's recovery offers. */
  globalRuleLatches?: readonly GlobalRuleLatch[];
  onClearGlobalRuleLatch?: (latch: GlobalRuleLatch) => void;
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
  if (robot.currentStatus == null && phase !== 'no_robot_loaded') {
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
      alarmDefinitionIds: ['manyforge.motion.stop_fault'],
      message: `${humanizeReasonCode(fault.reasonCode)} · ${
        fault.recoveryStatus.replaceAll('_', ' ')
      }. Motion remains blocked.${robot.componentIncidents.map(({ controllableComponentId, incident }) =>
        ` Incident on ${controllableComponentId}: ${incidentFacts(incident)}.`).join('')}`,
      busy: robot.stopFaultRecoveryPending,
      actions: [{
        id: 'recover-cell',
        label: robot.stopFaultRecoveryPending
          ? 'Checking recovery'
          : fault.recoveryStatus === 'ready' || robot.componentIncidents.length > 0
            ? 'Recover cell' : 'Retry recovery',
        disabled: robot.stopFaultRecoveryPending,
        run: () => { void robot.recoverStopFault(); },
      }],
    });
  }
  // Contracts 78/86: latched global rules, a source-loss latch shown distinctly.
  notices.push(...globalRuleLatchNotices(globalRuleLatches, onClearGlobalRuleLatch));
  if (facts.programRecovery) {
    notices.push({
      id: 'recovery.program',
      severity: 'error',
      domain: 'recovery',
      title: 'Program recovery required',
      alarmDefinitionIds: ['manyforge.program.operation_failure'],
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
      severity: program.cycleFailure === null ? 'info' : 'error',
      domain: 'runtime',
      title: program.cycleFailure === null ? 'Program finished' : 'Run failed',
      message: program.cycleFailure === null
        ? 'Use Reset in the Program controls before another Execute.'
        : facts.programRecoveryMessage,
    });
  } else if (facts.cellFault === null && facts.manualRecovery.length === 0
    && program.commandDisabledReason('program_run') === PROGRAM_RESET_READINESS_MESSAGE) {
    notices.push({
      id: 'program.reset-readiness',
      severity: 'info',
      domain: 'runtime',
      title: PROGRAM_RESET_READINESS_MESSAGE,
      message: 'Run stays disabled until the current cell admission is ready.',
    });
  }
  return notices;
}

/** Operator explanation for an automatically revealed All column. */
export function executionGroupFallbackMessage(fallback: ExecutionGroupFallback): string {
  switch (fallback) {
    case 'uncovered':
      return 'All is shown because the chosen groups leave some move, gripper or I/O nodes without a visible control.';
    case 'empty':
      return 'All is shown because the chosen visibility shows no group.';
    case 'membership_unavailable':
      return 'All is shown because group membership is unavailable: the current run was captured from a different Program revision than the Applied Program, so coverage is not inferred.';
  }
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
  const capturedRevision = state.run?.capture.programRevision;
  // Applied membership describes the run only when the run captured that revision.
  const membershipKnown = state.applied !== null
    && (capturedRevision === undefined || capturedRevision === state.applied.programRevision);
  const scopes = useMemo(() => !membershipKnown || state.applied === null ? [] : executionGroupScopes(
    [state.applied.program.tree], Object.keys(state.applied.program.execution_groups ?? {}),
  ), [state.applied, membershipKnown]);
  // Robot names only from the binding of exactly that Applied revision.
  const targetLabels = useMemo(() => !membershipKnown || state.applied === null ? {}
    : executionGroupTargetLabels(state.applied.program.target_roles, state.applied.programBinding),
  [state.applied, membershipKnown]);
  const projectedSelections = state.controls?.executionSelections.groups;
  const selections = projectedSelections !== undefined && projectedSelections.length > 0
    ? projectedSelections
    : appliedSelections;
  const allGroup = selections.find((group) => group.groupId === 'all');
  const namedGroups = selections.filter((group) => group.groupId !== 'all');
  const display = executionGroupDisplay(
    selections.map((group) => group.groupId),
    state.applied?.program.viewer?.hidden_execution_groups,
    scopes,
  );
  const visibleGroups = selections.filter((group) => display.shown.includes(group.groupId));
  // Count deliberately hidden named groups; All is a coverage fallback, not a hidden-group badge.
  const hiddenGroupCount = namedGroups
    .filter((group) => !display.shown.includes(group.groupId)).length;
  const groupTitle = (groupId: string) => {
    const targets = scopes.find((scope) => scope.groupId === groupId)?.targetRoleIds ?? [];
    return targets.length === 0 ? groupId
      : `${groupId}: ${targets.map((id) => targetLabels[id] ?? id).join(', ')}`;
  };
  return {
    selections, allGroup, visibleGroups, hiddenGroupCount, fallback: display.fallback, groupTitle,
  };
}

/** The Draft Program's choice of shown execution groups (a Program setting). */
export interface DraftGroupVisibility {
  /** The Draft's memberships, including "all". Never used for runtime admission. */
  scopes: ExecutionGroupScope[];
  /** Target-role labels; robot names only from an authoritative binding. */
  targetLabels?: Record<string, string>;
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
 * confirmed away. Checkboxes show the saved preference; an automatically
 * revealed All is marked, never saved.
 */
export function MotionControlSettings({ groups }: { groups: DraftGroupVisibility | null }) {
  const composerSettings = useComposerSettings();
  const preferences = composerSettings.settings.motionPanel;
  const setPreferences = (change: Partial<ComposerSettings['motionPanel']>) => composerSettings.update(
    (current) => ({ ...current, motionPanel: { ...current.motionPanel, ...change } }),
  );
  const groupIds = groups?.scopes.map((scope) => scope.groupId) ?? [];
  const display = groups === null ? null : executionGroupDisplay(groupIds, groups.hidden, groups.scopes);
  const preferred = display?.preferred ?? [];
  const toggle = (groupId: string, visible: boolean) => {
    if (groups?.onChange === undefined) return;
    const change = executionGroupVisibilityChange(groupIds, groups.hidden, groups.scopes, groupId, visible);
    if ('hidden' in change) groups.onChange(change.hidden);
  };
  return (
    <div className="drows">
      {groups !== null && groupIds.length > 0 && (
        <fieldset
          aria-label="Displayed groups"
          disabled={groups.onChange === undefined}
        >
          <SettingsSubheading title="Displayed groups" scope="program" />
          {groups.scopes.map(({ groupId, hardwareNodeIds, targetRoleIds }) => {
            const reason = preferred.includes(groupId)
              ? executionGroupVisibilityUnavailableReason(groups.scopes, preferred.filter((id) => id !== groupId))
              : null;
            const automatic = groupId === 'all' && display?.fallback != null;
            return <label className="toggle-control" key={groupId} title={reason ?? undefined}>
              <input
                type="checkbox"
                checked={preferred.includes(groupId)}
                disabled={reason !== null}
                onChange={(event) => toggle(groupId, event.target.checked)}
              />
              <span title={groupId}>{groupId === 'all' ? 'All groups' : groupId}</span>
              {automatic && <span className="hint"> (shown automatically)</span>}
              <span className="hint" title="Move, gripper command and I/O output nodes">
                {hardwareNodeIds.length} hardware node{hardwareNodeIds.length === 1 ? '' : 's'}
                {targetRoleIds.length > 0 && ` · ${targetRoleIds.map((id) => groups.targetLabels?.[id] ?? id).join(', ')}`}
              </span>
            </label>;
          })}
          {display?.fallback != null && (
            <span className="hint">
              {executionGroupFallbackMessage(display.fallback)} The saved choice is kept; check All to keep it shown.
            </span>
          )}
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
          <span>Confirm before resetting a running or ended Program</span>
        </label>
      </fieldset>
    </div>
  );
}

// Both Reset options start off each time the confirmation opens (spec 450).
const NO_RESET_OPTIONS: Required<ProgramResetOptions> = {
  reloadInitialScene: false,
  resetRobotPosition: false,
};

/** What a Reset with these options leaves as it is, for the confirmation. */
export function resetKeptMessage(options: Required<ProgramResetOptions>): string | null {
  if (options.reloadInitialScene && options.resetRobotPosition) return null;
  if (options.reloadInitialScene) return 'Robot positions stay as they are.';
  if (options.resetRobotPosition) return 'The Scene, carried objects and gripper state stay as they are.';
  return 'The Scene, carried objects, gripper state and robot positions stay as they are.';
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
  const [resetSelection, setResetSelection] = useState(NO_RESET_OPTIONS);
  // Contract 66: retained outputs with unknown acceptance are released only by
  // targeted operation Resets that acknowledge each one explicitly.
  // Manual operations and the held Program-run operations (recovery offers).
  const resetUncertain = useMemo(
    () => mergeUncertainOutputs(
      uncertainOutputs(robot.currentStatus?.manualOperations),
      programRunUncertainOutputs([
        ...(program.state.controls?.recoveryHolds ?? []).map((hold) => hold.offer),
        program.state.controls?.recoveryOffer,
      ]),
    ),
    [robot.currentStatus?.manualOperations, program.state.controls],
  );
  const [approvedUncertain, setApprovedUncertain] = useState<ReadonlySet<string>>(() => new Set());
  const [uncertainResetError, setUncertainResetError] = useState<string | null>(null);
  const uncertainApproved = allUncertainOutputsApproved(resetUncertain, approvedUncertain);
  // Continue fails the held node; it is confirmed when the backend asks.
  const [continueIntent, setContinueIntent] = useState<
    Pick<CycleRecoveryOffer, 'offerId' | 'offerRevision' | 'nodeInstanceId'> | null
  >(null);
  // Installation settings (UA7W.11); they never add a confirmation to Stop.
  const preferences = useComposerSettings().settings.motionPanel;
  const detailsButton = useRef<HTMLButtonElement>(null);
  const haltButton = useRef<HTMLButtonElement>(null);
  const resetButton = useRef<HTMLButtonElement>(null);
  const { state, cycleState } = program;
  // Run details reads the owner-admitted speeds only while it is open, and
  // again when the run advances; it never estimates one.
  const [admittedSpeeds, setAdmittedSpeeds] = useState<AdmittedSpeedsView>({ kind: 'no_run' });
  const detailsRunId = state.run?.capture.programRunId ?? null;
  const detailsRunRevision = state.run?.runRevision ?? null;
  useEffect(() => {
    if (!detailsOpen) return undefined;
    if (detailsRunId === null) {
      setAdmittedSpeeds({ kind: 'no_run' });
      return undefined;
    }
    const controller = new AbortController();
    setAdmittedSpeeds((current) => (current.kind === 'ready' && current.speeds.programRunId === detailsRunId
      ? current
      : { kind: 'loading' }));
    api.execution.admittedSpeeds(detailsRunId, controller.signal).then(
      (speeds) => setAdmittedSpeeds({ kind: 'ready', speeds }),
      (error: unknown) => {
        if (controller.signal.aborted) return;
        setAdmittedSpeeds({ kind: 'unavailable', reason: error instanceof Error ? error.message : String(error) });
      },
    );
    return () => controller.abort();
  }, [detailsOpen, detailsRunId, detailsRunRevision]);
  const {
    selections, allGroup, visibleGroups, hiddenGroupCount, fallback: groupFallback, groupTitle,
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
    && state.pending === null
    && program.draftUnappliedReason === null;
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
  const allMotionQuiesced = state.runnerAvailable && executionSelectionIsQuiescent(allGroup);
  // Paused on failed nodes: each active hold has its own offer and owns its
  // motion groups (a hold in no named group: every group no other hold
  // names). A group's Retry (in place of Run/Resume) and Continue (in place
  // of Stop) target the hold that owns it; the first hold in arrival order
  // wins when two hold the same group. The focused offer when it is no
  // hold (an operator Stop) keeps its Retry on every group, as it did before.
  const recoveryHolds = state.controls?.recoveryHolds ?? [];
  const focusedOffer = state.controls?.recoveryOffer ?? null;
  const operatorOffer = focusedOffer !== null
    && !recoveryHolds.some((hold) => hold.offer.offerId === focusedOffer.offerId)
    ? focusedOffer
    : null;
  const recoveryCommandAllowed = enabled
    && state.pending === null
    && state.controlPending === null
    && program.draftUnappliedReason === null;
  const offerControls = (
    offer: CycleRecoveryOffer,
    interventionStopScope: ProgramInterventionStopScope,
  ) => {
    // A disabled Retry still replaces Run/Resume: the pause must not be
    // bypassed, and its reason (e.g. containment not confirmed) is shown.
    const retryActions = offer.actions.filter(
      (action) => RETRY_ACTION_IDS.includes(action.actionId),
    );
    const retryAction = retryActions.find((action) => action.enabled)
      ?? retryActions[0]
      ?? null;
    // Operator-alarm attention on the held operation's exact resources
    // refuses a motion-producing Retry, and Continue, which releases the held
    // groups; Composer disables both with the remedy.
    const alarmBlock = program.motionAttentionBlock(recoveryOfferResourceIds(offer));
    const retryAlarmBlock = retryAction !== null
      && MOTION_PRODUCING_RECOVERY_ACTIONS.has(retryAction.actionId)
      ? alarmBlock
      : null;
    const continueAction = offer.actions.find(
      (action) => action.actionId === 'continue_with_failure',
    ) ?? null;
    const continueAlarmBlock = continueAction === null ? null : alarmBlock;
    return {
      offer,
      interventionStopScope,
      retryAction,
      retryAlarmBlock,
      canRetry: recoveryCommandAllowed
        && retryAction?.enabled === true
        && retryAlarmBlock === null,
      retryPending: retryAction !== null
        && state.controlPending === `recovery:${offer.offerId}:${retryAction.actionId}`,
      continueAction,
      continueAlarmBlock,
      canContinue: recoveryCommandAllowed
        && continueAction?.enabled === true
        && continueAlarmBlock === null,
      continuePending: state.controlPending === `recovery:${offer.offerId}:continue_with_failure`,
    };
  };
  const holdsControls = recoveryHolds.map((hold) => ({
    hold,
    ...offerControls(hold.offer, hold.interventionStopScope),
  }));
  const operatorControls = operatorOffer === null
    ? null
    : offerControls(operatorOffer, 'all');
  // A hold whose node is in no named group (['all']) owns every group that
  // no other hold names (contract item 43).
  const groupHold = (groupId: string) => holdsControls.find(
    (controls) => controls.hold.motionGroupIds.includes(groupId),
  ) ?? holdsControls.find(
    (controls) => controls.hold.motionGroupIds.length === 1
      && controls.hold.motionGroupIds[0] === 'all',
  ) ?? null;
  const heldNodeNames = recoveryHolds.map((hold) => hold.offer.nodeInstanceId).join(', ');
  const retryTitle = (controls: ReturnType<typeof offerControls>): string => {
    const { retryAction, offer } = controls;
    if (retryAction === null) return '';
    if (!retryAction.enabled) {
      return retryAction.disabledReasonCode === 'failure_pause_containment_unconfirmed'
        ? 'Retry waits until stopping the Program\'s other motion is confirmed.'
        : `Retry is unavailable: ${(retryAction.disabledReasonCode ?? 'not allowed').replaceAll('_', ' ')}.`;
    }
    return program.draftUnappliedReason
      ?? controls.retryAlarmBlock?.message
      ?? (controls.interventionStopScope === 'own_groups'
        ? `Retry ${offer.nodeInstanceId} and let its groups continue.`
        : recoveryHolds.length > 1
          ? `Retry ${offer.nodeInstanceId} and let the Program's motion continue, except groups other held nodes own.`
          : `Retry ${offer.nodeInstanceId} and let all the Program's motion continue.`);
  };
  const continueTarget = continueIntent === null ? null : holdsControls.find((controls) => (
    controls.offer.offerId === continueIntent.offerId
    && controls.offer.offerRevision === continueIntent.offerRevision
  )) ?? null;
  const requestContinue = (controls: ReturnType<typeof offerControls>) => {
    if (!controls.canContinue || controls.continueAction === null) return;
    if (controls.continueAction.requiresConfirmation) {
      setContinueIntent({
        offerId: controls.offer.offerId,
        offerRevision: controls.offer.offerRevision,
        nodeInstanceId: controls.offer.nodeInstanceId,
      });
    } else {
      program.applyRecoveryAction(controls.offer, 'continue_with_failure');
      setDetailsOpen(false);
    }
  };
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
  // Idle, Reset restores only what its options select (possibly nothing, contract 141), so it always asks.
  const idleReset = cycleState === 'idle_after_reset' && !executionActive;
  // A finished, stopped or failed run asks too when Reset confirmation is on,
  // so its options can be chosen in the same step.
  const endedReset = cycleState === 'terminal_awaiting_reset' && !executionActive;
  const positionAvailability = program.resetRobotPositionAvailability;
  const positionAvailable = positionAvailability?.available === true;
  const positionUnavailableReason = positionAvailability === null
    ? 'The backend does not report whether robot positions can be reset.'
    : positionAvailability.reason;
  const resetSelectionApplied = {
    reloadInitialScene: resetSelection.reloadInitialScene,
    resetRobotPosition: positionAvailable && resetSelection.resetRobotPosition,
  };
  const resetKept = resetKeptMessage(resetSelectionApplied);
  // Uncertain outputs always ask: each needs its own acknowledgement.
  const confirmReset = preferences.confirmReset || resetUncertain.length > 0;
  const requestProgramReset = async () => {
    if (!canReset) return;
    setApprovedUncertain(new Set());
    setUncertainResetError(null);
    setResetSelection(NO_RESET_OPTIONS);
    if (idleReset) {
      setResetIntent('confirm');
      return;
    }
    if (!executionActive) {
      if (endedReset && confirmReset) setResetIntent('confirm');
      else void program.reset();
      return;
    }
    if (!motionSettled) {
      setResetIntent(confirmReset ? 'stopping' : 'automatic');
      if (programFullStopping) return;
      const accepted = await program.stopMotion(['all']);
      if (!accepted) setResetIntent(null);
      return;
    }
    if (confirmReset) setResetIntent('confirm');
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
              ? observationTransport?.phase === 'no_robot_loaded'
                ? 'No robot loaded'
                : 'Robot status unavailable. Last-known manual and cell recovery status is not current.' : null;
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
  const briefRetry = useBriefAutomaticRetry(
    program.state.run?.capture.programRunId ?? null,
    program.state.run === null ? null : program.state.controls?.runtimeControls?.audit ?? null,
  );
  usePublishWorkbenchNotices('program-retry', briefRetry === null ? [] : [briefRetry]);
  const briefIoSource = useBriefIoSourceUnavailable(
    program.state.run?.capture.programRunId ?? null,
    program.state.run === null ? null : program.state.controls?.runtimeControls?.audit ?? null,
  );
  usePublishWorkbenchNotices('program-io-source', briefIoSource === null ? [] : [briefIoSource]);
  const briefTakeover = useBriefTakeoverFallback(
    program.state.run?.capture.programRunId ?? null,
    program.state.run === null ? null : program.state.controls?.runtimeControls?.audit ?? null,
  );
  usePublishWorkbenchNotices('program-takeover', briefTakeover === null ? [] : [briefTakeover]);
  const resetLabel = resetIntent === 'stopping' || resetIntent === 'automatic' ? 'Stopping'
    : cycleState === 'resetting' || state.pending === 'reset'
      || state.controlPending?.endsWith(':reset_cycle') ? 'Resetting' : 'Reset';
  // The Recovery block in Program run details offers the focused offer's
  // Motion-panel Retry and Continue and the Program-panel Reset: the same
  // handlers and enabled state, each disabled one with its reason.
  const alarmAcknowledgement = useExactAlarmAcknowledgement();
  const focusedHoldControls = focusedOffer === null ? undefined : holdsControls.find(
    (controls) => controls.offer.offerId === focusedOffer.offerId,
  );
  const focusedControls = focusedHoldControls ?? operatorControls;
  const gatingBlock = focusedControls?.retryAlarmBlock ?? focusedControls?.continueAlarmBlock ?? null;
  const unacknowledgedGating = gatingBlock?.members.filter(
    (member) => member.lifecycleState !== 'active_acknowledged',
  ) ?? [];
  const gatingOwnerEpoch = gatingBlock?.ownerEpoch ?? null;
  const recoveryCommands: ProgramRecoveryCommands | null = focusedControls === null ? null : {
    acknowledge: unacknowledgedGating.length === 0 || gatingOwnerEpoch === null ? null : {
      busy: alarmAcknowledgement.busyActivation !== null,
      error: alarmAcknowledgement.error,
      run: () => { void alarmAcknowledgement.acknowledge(unacknowledgedGating, gatingOwnerEpoch); },
    },
    retry: focusedControls.retryAction === null ? null : {
      label: focusedControls.retryPending ? 'Retrying' : 'Retry',
      enabled: focusedControls.canRetry,
      reason: focusedControls.canRetry ? null : retryTitle(focusedControls),
      run: () => {
        if (focusedControls.canRetry && focusedControls.retryAction !== null) {
          program.applyRecoveryAction(focusedControls.offer, focusedControls.retryAction.actionId);
          // Close Program run details so the operator can follow the execution.
          setDetailsOpen(false);
        }
      },
    },
    // Continue exists only for a failure hold, as in the Motion panel.
    continue: focusedHoldControls?.continueAction == null ? null : {
      label: focusedHoldControls.continuePending ? 'Continuing' : 'Continue',
      enabled: focusedHoldControls.canContinue,
      reason: focusedHoldControls.canContinue ? null
        : focusedHoldControls.continueAlarmBlock?.message
          ?? program.draftUnappliedReason
          ?? (focusedHoldControls.continueAction.enabled ? null
            : `Continue is unavailable: ${(focusedHoldControls.continueAction.disabledReasonCode ?? 'not allowed').replaceAll('_', ' ')}.`),
      run: () => requestContinue(focusedHoldControls),
    },
    reset: {
      label: resetLabel,
      enabled: canReset && resetIntent === null,
      reason: canReset ? null : 'Reset is unavailable in the current backend control state.',
      run: () => { void requestProgramReset(); },
    },
  };
  // "Review Program recovery" opens Program run details on the Recovery
  // block, after the overlay has focused its close button.
  const recoverySection = useRef<HTMLElement>(null);
  const [focusRecovery, setFocusRecovery] = useState(false);
  useEffect(() => {
    if (!focusRecovery || !detailsOpen) return;
    window.requestAnimationFrame(() => {
      setFocusRecovery(false);
      recoverySection.current?.scrollIntoView?.({ block: 'start' });
      recoverySection.current?.focus({ preventScroll: true });
    });
  }, [detailsOpen, focusRecovery]);
  const globalRuleRecovery = useGlobalRuleRecovery(
    enabled,
    robot.currentStatus?.ownerEpoch ?? null,
  );
  usePublishWorkbenchNotices('transport', deriveTransportGuidance({
    enabled,
    program,
    robot,
    observationTransport,
    motionActive,
    onOpenManualRecovery: onOpenRobotControls === undefined ? undefined : openManualRecovery,
    onReviewProgramRecovery: () => {
      setDetailsOpen(true);
      setFocusRecovery(true);
    },
    globalRuleLatches: globalRuleRecovery.latches,
    onClearGlobalRuleLatch: globalRuleRecovery.clear,
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
            {groupFallback !== null && (
              <p className="hint" role="status">{executionGroupFallbackMessage(groupFallback)}</p>
            )}
            <div className="transport-group-grid" aria-label="Motion group controls">
          {visibleGroups.length === 0 && (
            <p className="transport-groups-empty">
              {observationTransport?.phase === 'no_robot_loaded'
                ? 'No robot loaded'
                : selections.length === 0
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
            // Run/Resume of this group is refused while a gating alarm is
            // cell-global or names a resource the release would reserve.
            const releaseAlarmBlock = group.resumeAllowed
              ? program.motionAttentionBlock(group.resourceIds)
              : null;
            const canGroupRelease = enabled
              && state.runnerAvailable
              && cycleState === 'running'
              && !manualResident
              && programMotionRunning
              && state.pending === null
              && !selectionTransitionPending
              && group.resumeAllowed
              && releaseAlarmBlock === null
              && !releasePending
              && state.controlPending === null
              && program.commandAllowed('program_motion_resume');
            const releaseLabel = group.state === 'ready' ? 'Run' : 'Resume';
            // The hold that owns this group takes its Retry and Continue. A group
            // no hold owns takes an operator Stop's Retry when it has one;
            // otherwise a stopped or held group waits while any hold is active.
            // A running group keeps its ordinary state.
            const owner = groupHold(group.groupId);
            const retryTarget = owner ?? operatorControls;
            const retry = retryTarget?.retryAction ?? null;
            const canRetryGroup = retryTarget?.canRetry === true;
            const retryPending = retryTarget?.retryPending === true;
            const waitsForHold = owner === null
              && recoveryHolds.length > 0
              && (group.state === 'held' || group.state === 'ready');
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
                  disabled={retry !== null ? !canRetryGroup : waitsForHold || !canGroupRelease}
                  title={retry !== null && retryTarget !== null
                    ? retryTitle(retryTarget)
                    : waitsForHold
                      ? `${group.groupId} waits for held node${recoveryHolds.length > 1 ? 's' : ''} ${heldNodeNames}: Retry or Continue ${recoveryHolds.length > 1 ? 'them' : 'it'} first.`
                      : canGroupRelease
                        ? `${releaseLabel} ${group.groupId}: its stopped and not-yet-started motion continues.`
                        : group.state === 'running'
                          ? `${group.groupId} motion is running.`
                          : program.draftUnappliedReason
                            ?? releaseAlarmBlock?.message
                            ?? group.resumeUnavailableReason?.replaceAll('_', ' ')
                            ?? `${releaseLabel} is unavailable for this group.`}
                  onClick={() => {
                    if (retryTarget !== null && retry !== null) {
                      if (canRetryGroup) program.applyRecoveryAction(retryTarget.offer, retry.actionId);
                    } else if (!waitsForHold && canGroupRelease) void program.resumeMotion([group.groupId]);
                  }}
                >{retry !== null ? (retryPending ? 'Retrying' : 'Retry')
                  : releasePending ? (group.state === 'ready' ? 'Starting' : 'Resuming')
                    : group.state === 'running' ? 'Running' : releaseLabel}</button>
                {owner?.continueAction != null ? (
                  <button
                    type="button"
                    className="danger-button transport-group-stop"
                    disabled={!owner.canContinue}
                    title={owner.continueAlarmBlock?.message ?? 'Continuing fails the node'}
                    aria-label={`Continue ${group.groupId}: fail held node ${owner.offer.nodeInstanceId}`}
                    onClick={() => requestContinue(owner)}
                  >{owner.continuePending ? 'Continuing' : 'Continue'}</button>
                ) : (
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
                )}
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
                <span className="transport-group-name" title={groupTitle(group.groupId)}>
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
            disabled={!canRunOrResume || program.activationPending || program.activationOffer !== null && program.activationOffer !== undefined}
            title={canResumeFullStop
                ? 'Resume the halted Program tree. Motion stays stopped until Run or Resume in the Motion panel.'
                : canRun
                  ? 'Execute the exact Applied Program without starting robot motion.'
                  : program.draftUnappliedReason
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
                  : idleReset ? 'Reload the initial Scene or reset robot positions.'
                    : 'Reset the current Program cycle using its exact owner evidence.'
              : 'Reset is unavailable in the current backend control state.'}
            onClick={() => { void requestProgramReset(); }}
          >{resetLabel}</button>
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
        open={program.activationOffer != null}
        title="Activate components and execute?"
        titleId="activate-execute-title"
        kind="dialog"
        compact
        onClose={() => program.cancelActivationExecute()}
      >
        <div className="dialog-copy transport-confirmation">
          <p>{program.activationPending ? 'Component activation status:' : 'These components are not active yet:'}</p>
          <ul>{program.activationOffer?.components.filter((component) => program.activationPending || component.lifecycleState !== 'active').map((component) => {
            const health = program.activationComponentHealth[component.controllableComponentId];
            const current = health?.snapshot;
            const matching = current !== null && current !== undefined
              && current.controllableComponentId === component.controllableComponentId
              && current.retainedBundleId === component.retainedBundleId
              && current.graphRevision === component.graphRevision
              && current.publicationGeneration === component.publicationGeneration
              && current.activePublicPointerDigest === component.activePublicPointerDigest
              && current.readiness.requestOwnerEpoch === component.readiness.requestOwnerEpoch;
            const status = !program.activationPending ? component.lifecycleState
              : matching ? current.lifecycleState : health?.error ?? 'Current component health has not been read.';
            return <li key={component.controllableComponentId}>{component.controllableComponentId} ({status})</li>;
          })}</ul>
          <p>
            Activate prepares and enables every configured component in Deployment order,
            then executes the Program once. Components this Program does not use are activated too;
            Execute can still be refused for another reason.
          </p>
          {program.activationPending && <p role="status">{program.activationWaiting ? 'Waiting for current Execute readiness…'
            : program.activationProgress ?? 'Activating components…'}</p>}
          <div className="dialog-actions">
            <button type="button" onClick={() => program.cancelActivationExecute()}>Cancel</button>
            <button type="button" className="transport-run" disabled={program.activationPending}
              onClick={() => { void program.confirmActivationExecute(); }}>Activate and execute</button>
          </div>
        </div>
      </OverlayPanel>
      <OverlayPanel
        open={detailsOpen}
        title="Program run details"
        titleId="program-run-details-title"
        kind="info"
        onClose={() => setDetailsOpen(false)}
        returnFocusRef={detailsButton}
      >
        <ProgramRuntimePanel
          enabled={enabled}
          controller={program}
          recovery={recoveryCommands}
          recoveryRef={recoverySection}
        />
        {fullCellStop?.fault && fullCellStop.fault.state !== 'cleared' && (
          <section className="runtime-detail-group" aria-labelledby="program-stop-fault-title">
            <h4 id="program-stop-fault-title">Stop fault: motion is blocked</h4>
            <p className="inline-message severity-error" role="alert">
              A stop did not complete cleanly ({humanizeReasonCode(fullCellStop.fault.reasonCode)}).
              {fullCellStop.fault.recoveryStatus === 'ready'
                ? ' The cell can be recovered.'
                : ' The cell stays blocked until the backend confirms it is safe.'}
            </p>
            {robot.componentIncidents.length > 0 && (
              <ul aria-label="Open component incidents">
                {robot.componentIncidents.map(({ controllableComponentId, incident }) => (
                  <li key={incident.incidentId}>
                    <strong>{controllableComponentId}</strong>: {incidentFacts(incident)}
                  </li>
                ))}
              </ul>
            )}
            {robot.componentIncidents.length > 0 && (
              <p className="muted">
                Recover cell acknowledges these incidents, then recovers the cell when it is ready.
                Motion never resumes by itself.
              </p>
            )}
            {robot.componentIncidentsUnavailableReason && (
              <p className="inline-message severity-warning">
                Component incidents are unavailable: {robot.componentIncidentsUnavailableReason}
              </p>
            )}
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
                  : fullCellStop.fault.recoveryStatus === 'ready' || robot.componentIncidents.length > 0
                    ? 'Recover cell' : 'Check again'}
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
              <dd>Ends the run; the next Execute starts from the beginning. The Scene and robot positions stay as they are unless you select Reload initial scene or (simulators only) Reset robot position.</dd>
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
            admitted={admittedSpeeds}
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
        title={idleReset ? 'Reset the Program?'
          : endedReset ? 'Reset the ended Program run?' : 'Reset the running Program?'}
        titleId="program-reset-confirmation-title"
        kind="dialog"
        compact
        onClose={() => setResetIntent(null)}
        returnFocusRef={resetButton}
      >
        <div className="dialog-copy transport-confirmation">
          {idleReset
            ? <p>No run is active. Choose what Reset restores:</p>
            : <>
              <p>{endedReset ? 'The run has ended. Reset clears it:' : 'Motion is stopped. Reset also ends this run:'}</p>
              <ul>
                <li>its progress is discarded;</li>
                <li>the next Execute starts the tree from the beginning.</li>
              </ul>
            </>}
          <fieldset className="transport-reset-options">
            <label className="toggle-control">
              <input
                type="checkbox"
                checked={resetSelection.reloadInitialScene}
                onChange={(event) => {
                  const reloadInitialScene = event.target.checked;
                  setResetSelection((current) => ({ ...current, reloadInitialScene }));
                }}
              />
              <span>Reload initial scene</span>
            </label>
            <small className="transport-reset-option-note">
              Scene objects, including carried ones, are replaced by the Program&apos;s initial Scene.
            </small>
            <label className="toggle-control" title={positionUnavailableReason ?? undefined}>
              <input
                type="checkbox"
                checked={resetSelectionApplied.resetRobotPosition}
                disabled={!positionAvailable}
                onChange={(event) => {
                  const resetRobotPosition = event.target.checked;
                  setResetSelection((current) => ({ ...current, resetRobotPosition }));
                }}
              />
              <span>Reset robot position</span>
            </label>
            <small className="transport-reset-option-note">
              {positionAvailable
                ? 'Simulator only: robots are set to the Deployment\'s initial joint positions.'
                : positionUnavailableReason}
            </small>
          </fieldset>
          {resetKept !== null && <p>{resetKept}</p>}
          <UncertainOutputConfirmation outputs={resetUncertain} approved={approvedUncertain}
            onToggle={(key, approved) => setApprovedUncertain((current) => {
              const next = new Set(current);
              if (approved) next.add(key); else next.delete(key);
              return next;
            })} />
          {uncertainResetError !== null && (
            <p className="inline-message severity-error" role="alert">{uncertainResetError}</p>
          )}
          {!idleReset && !endedReset && <p>
            {programFullStopped
              ? 'Cancel keeps the run halted; Execute resumes it.'
              : `Cancel keeps the run stopped; ${continueLabel} in the Motion panel continues it.`}
          </p>}
          <div className="dialog-actions">
            <button type="button" onClick={() => setResetIntent(null)}>Cancel</button>
            <button
              type="button"
              className="danger-button"
              // An ended run, or no run at all, has no live motion selections
              // (all groups are unavailable); the owner's Reset admission
              // (canReset) is authoritative there.
              // An idle Reset may restore nothing (contract 141).
              disabled={!canReset || (!motionSettled && !endedReset && !idleReset) || !uncertainApproved}
              title={!uncertainApproved ? 'Confirm each output with unknown acceptance first.' : undefined}
              onClick={() => {
                void (async () => {
                  const failure = await robot.resetUncertainOutputs(resetUncertain);
                  if (failure !== null) {
                    setUncertainResetError(`${failure} The Program was not reset.`);
                    return;
                  }
                  setResetIntent(null);
                  void program.reset(resetSelectionApplied);
                })();
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
      <OverlayPanel
        open={continueIntent !== null}
        title="Continue with a failed node?"
        titleId="program-continue-confirmation-title"
        kind="dialog"
        compact
        onClose={() => setContinueIntent(null)}
      >
        <div className="dialog-copy transport-confirmation">
          <p>
            Continuing fails the node <code>{continueIntent?.nodeInstanceId}</code>:
            its held operation is closed, it returns FAILURE to the Program
            tree, and the motion groups it holds resume.
          </p>
          {continueTarget === null || !continueTarget.canContinue ? (
            <p role="status">Continue is no longer available for this hold.</p>
          ) : null}
          <div className="dialog-actions">
            <button type="button" onClick={() => setContinueIntent(null)}>Cancel</button>
            <button
              type="button"
              className="danger-button"
              disabled={continueTarget === null || !continueTarget.canContinue}
              onClick={() => {
                setContinueIntent(null);
                if (continueTarget !== null) {
                  program.applyRecoveryAction(continueTarget.offer, 'continue_with_failure');
                  setDetailsOpen(false);
                }
              }}
            >Continue</button>
          </div>
        </div>
      </OverlayPanel>
    </>
  );
}
