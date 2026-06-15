import { useCallback, useEffect, useRef, useState } from 'react';
import { useCycleStore } from '../store/cycleStore';
import { useRobotCommandAuthority } from '../store/commandAuthority';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import { api } from '../api/client';
import {
  buildStopProfileView,
  evaluateStopRunGate,
  shouldClearStopInProgress,
  shouldFreezeVelocitySlider,
} from '../store/stopProfile';

/**
 * TransportControls — Run / Stop / Reset + velocity-scale slider.
 *
 * Rendered as a fixed pill overlaying the bottom-center of the scene
 * viewer, teach-pendant style: the execution controls live where their
 * consequences are visible, never move, and never scroll away. Stop is
 * the only red control on the screen; Run carries green only while the
 * cycle is actually armable (color = state, not decoration).
 */
export function TransportControls() {
  const { state, velocityScale, run, stop, reset, setVelocityScale } = useCycleStore();
  const runAuthority = useRobotCommandAuthority({
    requireFreshScene: true,
  });
  // Subscribe to the shared runtime-status poll loop so the stop-profile
  // inspector + run gate track the backend's advertised stop capability.
  useStatusPolling();
  const runtime = useStatusStore((s) => s.runtime);
  const [cycleBusy, setCycleBusy] = useState(false);
  const [stopInProgress, setStopInProgress] = useState(false);
  const [velocityDraft, setVelocityDraft] = useState(velocityScale);
  const pendingVelocityScaleRef = useRef<number | null>(null);
  const velocityScaleInFlightRef = useRef(false);
  const velocityScaleTimerRef = useRef<number | null>(null);
  const lastVelocityScaleSentAtRef = useRef(0);

  const stopProfile = buildStopProfileView(runtime);
  const stopGate = evaluateStopRunGate(runtime);
  // The velocity slider is frozen while a stop is in flight and for the whole
  // stopped hold (the deceleration ramp runs while the cycle is 'stopped').
  const frozen = shouldFreezeVelocitySlider(state, stopInProgress);
  const frozenRef = useRef(frozen);
  const wasFrozenRef = useRef(frozen);
  useEffect(() => {
    frozenRef.current = frozen;
  }, [frozen]);

  const canRun =
    (state === 'idle' || state === 'stopped') &&
    runAuthority.canCommand &&
    !stopGate.blocked;
  // Stop applies to in-flight motion the operator can see — BT
  // execution (state==='running') or an intervention / direct-jog
  // session (direct_control).
  const canStop = state === 'running' || state === 'direct_control';

  const runCycleCommand = useCallback(
    (command: () => Promise<void>) => {
      void (async () => {
        setCycleBusy(true);
        try {
          await command();
        } finally {
          setCycleBusy(false);
        }
      })();
    },
    [],
  );

  const flushVelocityScale = useCallback(() => {
    if (velocityScaleTimerRef.current !== null) {
      window.clearTimeout(velocityScaleTimerRef.current);
      velocityScaleTimerRef.current = null;
    }
    // Suppress sends while frozen. A stop is a hard boundary for velocity
    // intent: the pending draft is dropped (not applied) when the slider
    // unfreezes, so the robot is never retimed mid-stop or with a stale value.
    if (frozenRef.current) {
      return;
    }
    if (velocityScaleInFlightRef.current || pendingVelocityScaleRef.current === null) {
      return;
    }
    const nextScale = pendingVelocityScaleRef.current;
    pendingVelocityScaleRef.current = null;
    velocityScaleInFlightRef.current = true;
    lastVelocityScaleSentAtRef.current = performance.now();
    void setVelocityScale(nextScale).finally(() => {
      velocityScaleInFlightRef.current = false;
      if (pendingVelocityScaleRef.current !== null) {
        const elapsed = performance.now() - lastVelocityScaleSentAtRef.current;
        const delay = Math.max(0, 250 - elapsed);
        velocityScaleTimerRef.current = window.setTimeout(flushVelocityScale, delay);
      }
    });
  }, [setVelocityScale]);

  const queueVelocityScale = useCallback((value: number) => {
    const clamped = Math.max(0, Math.min(1, value));
    setVelocityDraft(clamped);
    pendingVelocityScaleRef.current = clamped;
    if (velocityScaleInFlightRef.current) {
      return;
    }
    const elapsed = performance.now() - lastVelocityScaleSentAtRef.current;
    const delay = Math.max(0, 250 - elapsed);
    if (delay === 0) {
      flushVelocityScale();
    } else if (velocityScaleTimerRef.current === null) {
      velocityScaleTimerRef.current = window.setTimeout(flushVelocityScale, delay);
    }
  }, [flushVelocityScale]);

  useEffect(() => {
    if (pendingVelocityScaleRef.current === null && !velocityScaleInFlightRef.current) {
      setVelocityDraft(velocityScale);
    }
  }, [velocityScale]);

  useEffect(() => () => {
    if (velocityScaleTimerRef.current !== null) {
      window.clearTimeout(velocityScaleTimerRef.current);
      velocityScaleTimerRef.current = null;
    }
  }, []);

  // A stop is a hard boundary for velocity intent: when the slider unfreezes,
  // drop any velocity change drafted before/during the stop and re-sync to the
  // authoritative scale, rather than silently retiming the robot with a stale
  // pre-stop value (Reset is expected to discard it too).
  useEffect(() => {
    const wasFrozen = wasFrozenRef.current;
    wasFrozenRef.current = frozen;
    if (wasFrozen && !frozen) {
      if (velocityScaleTimerRef.current !== null) {
        window.clearTimeout(velocityScaleTimerRef.current);
        velocityScaleTimerRef.current = null;
      }
      pendingVelocityScaleRef.current = null;
      if (!velocityScaleInFlightRef.current) {
        setVelocityDraft(velocityScale);
      }
    }
  }, [frozen, velocityScale]);

  // A stop is "in progress" from the moment Stop is pressed until the cycle
  // settles out of the running/jog state (it then becomes 'stopped', which
  // freezes on its own). Clearing here avoids latching the flag forever.
  useEffect(() => {
    if (shouldClearStopInProgress(state)) {
      setStopInProgress(false);
    }
  }, [state]);

  const running = state === 'running';
  // Reset is a no-op from idle; from any other state it discards live cycle
  // state (stopped position, latched errors, run context) — give it an
  // amber accent exactly when it carries that weight.
  const resetMeaningful = state !== 'idle';
  const canChangeStopProfile =
    (state === 'idle' || state === 'stopped') &&
    !cycleBusy &&
    !stopProfile.queryFailed;

  const deploymentAdaptersForUpdate = (
    adapters: Record<string, { adapterId?: string; adapterVersion?: string; config?: Record<string, unknown>; supportedProfiles?: string[]; capabilityKeys?: string[] }>,
  ) =>
    Object.fromEntries(
      Object.entries(adapters).map(([key, adapter]) => [
        key,
        {
          adapter_id: adapter.adapterId,
          adapter_version: adapter.adapterVersion,
          config: adapter.config ?? {},
          supported_profiles: adapter.supportedProfiles ?? [],
          capability_keys: adapter.capabilityKeys ?? [],
        },
      ]),
    );

  const changeOperatorStopProfile = useCallback((mode: string) => {
    void (async () => {
      setCycleBusy(true);
      try {
        const deployment = await api.deployment.getFull();
        const execution = deployment.adapters.execution;
        if (execution == null) {
          throw new Error('Loaded deployment has no execution adapter');
        }
        const config = { ...execution.config };
        const existingProfile =
          config.operator_stop_profile &&
          typeof config.operator_stop_profile === 'object' &&
          !Array.isArray(config.operator_stop_profile)
            ? { ...(config.operator_stop_profile as Record<string, unknown>) }
            : {};
        config.operator_stop_profile = {
          ...existingProfile,
          profile_id: `operator_${mode}`,
          stop_mode: mode,
        };
        const adapters = deploymentAdaptersForUpdate(deployment.adapters);
        adapters.execution = {
          ...adapters.execution,
          config,
        };
        await api.deployment.update({
          adapters: adapters as unknown as typeof deployment.adapters,
        });
        await useStatusStore.getState().refresh();
      } catch (err) {
        console.error('Failed to update operator stop profile', err);
      } finally {
        setCycleBusy(false);
      }
    })();
  }, []);

  return (
    <div className="pointer-events-none absolute inset-x-0 bottom-3 z-30 flex flex-col items-center gap-2">
      {stopGate.blocked && stopGate.reason !== null && (
        <div
          role="alert"
          className={`pointer-events-auto max-w-md rounded-md border px-3 py-1.5 text-xs shadow-lg ${
            stopGate.severity === 'fault'
              ? 'border-red-600/70 bg-red-950/85 text-red-200'
              : 'border-amber-600/70 bg-amber-950/85 text-amber-200'
          }`}
        >
          {stopGate.reason}
        </div>
      )}
      <div className="pointer-events-auto flex items-center gap-2 rounded-lg border border-gray-600/80 bg-gray-900/90 px-3 py-2 shadow-xl shadow-black/50 backdrop-blur-sm">
        <button
          type="button"
          onClick={() => void run()}
          disabled={cycleBusy || !canRun}
          title={
            canRun
              ? 'Start the loaded program'
              : stopGate.blocked && stopGate.reason !== null
                ? stopGate.reason
                : runAuthority.reason !== null
                  ? runAuthority.reason
                  : `Cycle is ${state}`
          }
          className={`flex items-center gap-1.5 rounded px-3.5 py-1.5 text-sm font-semibold transition-colors ${
            canRun && !cycleBusy
              ? 'bg-emerald-600 text-white hover:bg-emerald-500'
              : running
                ? 'border border-emerald-600/60 bg-emerald-950/60 text-emerald-400'
                : 'border border-gray-600 bg-gray-800 text-gray-500'
          } disabled:cursor-not-allowed`}
        >
          <svg
            viewBox="0 0 16 16"
            className={`h-3 w-3 fill-current ${running ? 'animate-pulse' : ''}`}
            aria-hidden
          >
            <path d="M4 2.5v11l9-5.5z" />
          </svg>
          {/* Stack both labels in one grid cell so the button is always as
              wide as the widest label — no layout shift on state change,
              and it adapts to whatever the labels are. */}
          <span className="grid text-center">
            <span className={`col-start-1 row-start-1 ${running ? 'invisible' : ''}`}>Run</span>
            <span className={`col-start-1 row-start-1 ${running ? '' : 'invisible'}`}>Running</span>
          </span>
        </button>
        <button
          type="button"
          onClick={() => {
            // Freeze the velocity slider immediately on Stop: the cycle FSM
            // jumps running→stopped synchronously, but the deceleration ramp
            // continues afterward, so rescaling now must be deferred.
            setStopInProgress(true);
            // Stop the active cycle, then force a scene refresh so the
            // viewer reflects the kStopped pose without waiting a full
            // poll interval.
            runCycleCommand(async () => {
              await stop();
              try {
                await useSceneStateStore.getState().refresh();
              } catch (err) {
                console.warn('post-stop scene refresh failed', err);
              }
            });
          }}
          disabled={cycleBusy || !canStop}
          title={canStop ? 'Stop the active cycle' : `Cycle is ${state}`}
          className={`flex items-center gap-1.5 rounded px-3.5 py-1.5 text-sm font-semibold transition-colors ${
            canStop && !cycleBusy
              ? 'bg-red-700 text-white hover:bg-red-600'
              : 'border border-gray-600 bg-gray-800 text-gray-500'
          } disabled:cursor-not-allowed`}
        >
          <svg viewBox="0 0 16 16" className="h-3 w-3 fill-current" aria-hidden>
            <rect x="3" y="3" width="10" height="10" rx="1" />
          </svg>
          Stop
        </button>
        <button
          type="button"
          onClick={() => {
            runCycleCommand(reset);
          }}
          disabled={cycleBusy}
          title={
            resetMeaningful
              ? `Reset the cycle to its initial state (discards the current ${state} state)`
              : 'Reset the cycle to its initial state'
          }
          className={`flex items-center gap-1.5 rounded border px-3 py-1.5 text-sm font-medium transition-colors disabled:cursor-not-allowed disabled:text-gray-500 ${
            resetMeaningful
              ? 'border-amber-600/70 bg-amber-950/40 text-amber-300 hover:bg-amber-900/50'
              : 'border-gray-600 bg-gray-800 text-gray-200 hover:bg-gray-700'
          }`}
        >
          <svg viewBox="0 0 16 16" className="h-3 w-3 fill-none stroke-current" strokeWidth="1.8" aria-hidden>
            <path d="M13 8a5 5 0 1 1-1.5-3.6M13 2.8v2.6h-2.6" strokeLinecap="round" strokeLinejoin="round" />
          </svg>
          Reset
        </button>
        {stopProfile.available && (
          <>
            <div className="mx-1 h-6 w-px bg-gray-600/80" />
            <label
              className="flex items-center gap-1.5 text-xs text-gray-300"
              title={
                canChangeStopProfile
                  ? 'Operator stop profile for this loaded deployment session'
                  : 'Operator stop profile can be changed while idle or stopped'
              }
            >
              <span className="font-mono text-[11px] uppercase tracking-wide text-gray-400">
                stop
              </span>
              <select
                aria-label="Configured operator stop mode"
                value={stopProfile.operatorMode ?? ''}
                onChange={(event) => changeOperatorStopProfile(event.target.value)}
                disabled={!canChangeStopProfile}
                className={`rounded border bg-gray-800 px-1.5 py-1 text-[11px] disabled:cursor-not-allowed ${
                  stopProfile.unsupported || stopProfile.queryFailed
                    ? 'border-amber-600/70 text-amber-300'
                    : 'border-gray-600 text-gray-200'
                }`}
              >
                {stopProfile.operatorMode === null && <option value="">— none —</option>}
                {stopProfile.options.map((option) => (
                  <option
                    key={option.mode}
                    value={option.mode}
                    disabled={option.disabled}
                    title={option.reason ?? undefined}
                  >
                    {option.label}
                    {option.disabled ? ' (unsupported)' : ''}
                  </option>
                ))}
              </select>
            </label>
          </>
        )}
        <div className="mx-1 h-6 w-px bg-gray-600/80" />
        <label
          className="flex items-center gap-1.5 text-xs text-gray-300"
          title={
            frozen
              ? 'Velocity locked while the cycle is stopping/stopped'
              : 'Runtime velocity scale (0.0 - 1.0; 0 holds motion)'
          }
        >
          <span className="font-mono text-[11px] uppercase tracking-wide text-gray-400">vel</span>
          <input
            type="range"
            min={0.0}
            max={1.0}
            step={0.01}
            value={velocityDraft}
            onChange={(e) => queueVelocityScale(Number(e.target.value))}
            disabled={frozen}
            className="w-24 accent-emerald-500 disabled:cursor-not-allowed disabled:opacity-40"
          />
          <span className="w-8 text-right font-mono text-[11px] text-gray-200">
            {velocityDraft.toFixed(2)}
          </span>
        </label>
      </div>
    </div>
  );
}
