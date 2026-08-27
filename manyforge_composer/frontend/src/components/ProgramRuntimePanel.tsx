import { useRef, useState } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import { robotControlHasActiveOperation } from '../state/robotControl';
import { OverlayPanel } from './OverlayPanel';


interface ProgramRuntimePanelProps {
  enabled: boolean;
  controller: ProgramRuntimeController;
}

function shortIdentity(value: string | null): string {
  if (!value) return 'none';
  return value.length > 26 ? `${value.slice(0, 22)}…` : value;
}

function stateLabel(state: string): string {
  return state.replaceAll('_', ' ');
}

export function ProgramRuntimePanel({
  enabled,
  controller,
}: ProgramRuntimePanelProps) {
  const { state, cycleState, refresh, run, stop, reset } = controller;
  const canStop = state.run?.state === 'running'
    || state.run?.state === 'intervention_required';

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
          disabled={!enabled || state.pending !== null}
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
                ? stateLabel(state.run.state)
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
      {state.error && (
        <div className="program-feedback error-state" role="alert">
          <span>{state.error}</span>
        </div>
      )}
      {cycleState === 'terminal_awaiting_reset' && (
        <button
          type="button"
          disabled={!enabled || state.pending !== null}
          onClick={() => { void reset(); }}
        >
          {state.pending === 'reset' ? 'Resetting…' : 'Reset Program cycle'}
        </button>
      )}

      <div className="program-runtime-actions">
        <button
          type="button"
          disabled={
            !enabled
            || !state.runnerAvailable
            || state.applied === null
            || state.pending !== null
            || cycleState !== 'idle_after_reset'
          }
          onClick={() => { void run(); }}
        >
          {state.pending === 'run' ? 'Starting…' : 'Run Applied Program'}
        </button>
        <button
          type="button"
          className="danger-button"
          disabled={
            !enabled
            || !state.runnerAvailable
            || !canStop
            || state.pending !== null
          }
          onClick={() => { void stop(); }}
        >
          {state.pending === 'stop' ? 'Stopping…' : 'Stop Program'}
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
}: {
  enabled: boolean;
  program: ProgramRuntimeController;
  robot: RobotControlController;
}) {
  const [detailsOpen, setDetailsOpen] = useState(false);
  const detailsButton = useRef<HTMLButtonElement>(null);
  const { state, cycleState } = program;
  const motionActive = robotControlHasActiveOperation(robot.state);
  const motionPaused = robot.state.operation?.state === 'paused';
  const canPause = robot.state.operation?.state === 'executing';
  const programActive = state.run?.state === 'running'
    || state.run?.state === 'intervention_required';
  const canRun = enabled
    && state.runnerAvailable
    && state.applied !== null
    && state.pending === null
    && cycleState === 'idle_after_reset';
  const canReset = enabled
    && state.pending === null
    && cycleState === 'terminal_awaiting_reset';
  const velocityAvailable = enabled
    && robot.runtimeScaleStatus !== null
    && robot.runtimeScaleStatus.supported
    && (robot.state.operation?.state === 'executing'
      || robot.state.operation?.state === 'paused');
  const velocityReason = robot.runtimeScaleError
    ?? (motionActive
      ? 'The exact active operation does not currently advertise qualified runtime-scale control.'
      : 'Runtime velocity is available only while an addressed motion operation is active.');
  const fullStopReason = 'The current execution projection does not publish the cell Stop generation required for a safely fenced full-cell Stop.';

  return (
    <>
      <div className="viewer-transport" aria-label="Program and motion controls">
        <button
          type="button"
          disabled={!enabled || robot.controlPending !== null || (!canPause && !motionPaused)}
          title={motionPaused
            ? 'Resume the exact paused manual-control operation.'
            : canPause
              ? 'Pause the exact active manual-control operation.'
              : 'Pause or Resume is available only for an owner-observed executing or paused operation.'}
          onClick={() => { void (motionPaused ? robot.resume() : robot.pause()); }}
        >
          {robot.controlPending === 'pause'
            ? 'Pausing…'
            : robot.controlPending === 'resume'
              ? 'Resuming…'
              : motionPaused ? 'Resume' : 'Pause'}
        </button>
        <button
          type="button"
          className="transport-run"
          disabled={!canRun}
          title={canRun ? 'Run the exact Applied Program.' : 'Run requires an Applied Program and an idle, reset runner.'}
          onClick={() => { void program.run(); }}
        >
          {state.pending === 'run' ? 'Starting…' : 'Run'}
        </button>
        <button
          type="button"
          className="danger-button"
          disabled={!enabled || !motionActive || robot.state.phase === 'stopping'}
          title={motionActive
            ? 'Stop the exact active manual-control operation.'
            : 'No addressed manual-control operation is active.'}
          onClick={() => { void robot.stop(); }}
        >
          {robot.state.phase === 'stopping' ? 'Stopping…' : 'Stop motion'}
        </button>
        <button
          type="button"
          className="transport-stop-program"
          disabled={!enabled || !programActive || state.pending !== null}
          title={programActive
            ? 'Stop Program execution. This is separate from stopping one manual-control operation.'
            : 'No Program run is active.'}
          onClick={() => { void program.stop(); }}
        >
          {state.pending === 'stop' ? 'Stopping…' : 'Stop Program'}
        </button>
        <button
          type="button"
          className={canReset ? 'transport-reset meaningful' : 'transport-reset'}
          disabled={!canReset}
          title={canReset ? 'Reset the completed Program cycle.' : 'Reset is available after a Program run reaches a terminal state.'}
          onClick={() => { void program.reset(); }}
        >
          {state.pending === 'reset' ? 'Resetting…' : 'Reset'}
        </button>
        <span className="viewer-tool-separator" aria-hidden="true" />
        <label className="transport-velocity" title={velocityAvailable ? 'Scale the exact active operation.' : velocityReason}>
          <span>Velocity</span>
          <input
            type="range"
            min="0"
            max="1"
            step="0.01"
            value={robot.runtimeScaleDraft}
            disabled={!velocityAvailable}
            onChange={(event) => robot.setRuntimeScale(Number(event.target.value))}
          />
          <output>
            {velocityAvailable
              ? `${Math.round(robot.runtimeScaleDraft * 100)}%${robot.runtimeScalePending ? '…' : ''}`
              : '—'}
          </output>
        </label>
        <button
          ref={detailsButton}
          type="button"
          className="icon-button"
          aria-label="Program run details"
          title="Program run details"
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
          Runtime velocity is operation-scoped. The slider keeps only the latest
          pending value, re-reads the owner's applied value, and drops pending
          input when Stop begins.
          {!velocityAvailable && <> Currently unavailable: {velocityReason}</>}
        </p>
        <p className="panel-intro transport-unavailable-reason">
          <button type="button" className="danger-button" disabled title={fullStopReason}>
            Full-cell Stop unavailable
          </button>
          {' '}{fullStopReason}
        </p>
      </OverlayPanel>
    </>
  );
}
