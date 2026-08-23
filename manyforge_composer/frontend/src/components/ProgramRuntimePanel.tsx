import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';


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
