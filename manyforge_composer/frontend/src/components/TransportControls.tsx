import { useCallback, useEffect, useRef, useState } from 'react';
import { useCycleStore } from '../store/cycleStore';
import {
  ROBOT_COMMAND_AUTHORITY_REASONS,
  useRobotCommandAuthority,
} from '../store/commandAuthority';
import { useSceneStateStore } from '../store/sceneStateStore';
import {
  operatorAttentionBlocksMotion,
  useOperatorAlarmPolling,
  useOperatorAlarmStore,
} from '../store/operatorAlarmStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import {
  buildStopExecutionWarning,
  evaluateStopRunGate,
  shouldClearStopInProgress,
  shouldFreezeVelocitySlider,
} from '../store/stopProfile';
import {
  RecoveryCommandNotice,
  type RecoveryConfirmation,
  recoveryActionConfirmationText,
  recoveryActionLabel,
  recoveryActionsForTransport,
  recoveryCommandFailureMessage,
  recoveryConfirmationAction,
  recoveryOfferHeadline,
  isMotionProducingRecoveryAction,
} from './RecoveryActionControls';

/**
 * TransportControls — Run / Stop / Reset + velocity-scale slider.
 *
 * Rendered as a fixed pill overlaying the bottom-center of the scene
 * viewer, teach-pendant style: the execution controls live where their
 * consequences are visible, never move, and never scroll away. Stop is
 * the only red control on the screen; Run carries green only while the
 * cycle is actually armable (color = state, not decoration).
 */
export function runtimeAllowsMotion(
  runtime: { canExecute: boolean } | null,
): boolean {
  // Unknown runtime state is not permission to arm motion. This briefly
  // disables Run during the first status fetch on every lane and, critically,
  // prevents a real-robot control from appearing eligible before its disabled
  // lifecycle snapshot arrives.
  return runtime?.canExecute === true;
}

export function TransportControls() {
  useOperatorAlarmPolling();
  const {
    state,
    motionStopState,
    velocityScale,
    errorDetails,
    run,
    stop,
    fullStop,
    reset,
    recoveryOffer,
    applyRecoveryAction,
    setVelocityScale,
  } = useCycleStore();
  const operatorAlarmsLoaded = useOperatorAlarmStore((store) => store.loaded);
  const operatorAlarmAttentionCount = useOperatorAlarmStore(
    (store) => store.motionAttentionCount,
  );
  const operatorAlarmsError = useOperatorAlarmStore((store) => store.error);
  const operatorAlarmsReconciled = useOperatorAlarmStore((store) => store.reconciled);
  const operatorAlarmHealth = useOperatorAlarmStore((store) => store.health);
  const runAuthority = useRobotCommandAuthority({
    // Run admission is owned by the backend and kernel. Visual-state freshness
    // must not keep the viewer producer alive or become a second safety gate.
    requireFreshScene: false,
  });
  // Subscribe to the shared runtime-status poll loop so the stop-profile
  // inspector + run gate track the backend's advertised stop capability.
  useStatusPolling();
  const runtime = useStatusStore((s) => s.runtime);
  const [cycleBusy, setCycleBusy] = useState(false);
  const [stopInProgress, setStopInProgress] = useState(false);
  const [showFullStopMenu, setShowFullStopMenu] = useState(false);
  const [pendingRecoveryConfirmation, setPendingRecoveryConfirmation] =
    useState<RecoveryConfirmation | null>(null);
  const [recoveryCommandNotice, setRecoveryCommandNotice] = useState<string | null>(null);
  const [velocityDraft, setVelocityDraft] = useState(velocityScale);
  const pendingVelocityScaleRef = useRef<number | null>(null);
  const velocityScaleInFlightRef = useRef(false);
  const velocityScaleTimerRef = useRef<number | null>(null);
  const lastVelocityScaleSentAtRef = useRef(0);

  const stopGate = evaluateStopRunGate(runtime);
  const runtimeBlocksMotion = !runtimeAllowsMotion(runtime);
  const stopExecutionWarning = buildStopExecutionWarning(errorDetails);
  const {
    primaryAction: primaryRecoveryAction,
    resetAction: resetRecoveryAction,
  } = recoveryActionsForTransport(recoveryOffer);
  const confirmationAction = recoveryOffer
    ? recoveryConfirmationAction(recoveryOffer, pendingRecoveryConfirmation)
    : undefined;
  const primaryIsAcknowledgement =
    primaryRecoveryAction?.actionId === 'acknowledge_and_continue';
  const primaryStartsMotion = primaryRecoveryAction
    ? isMotionProducingRecoveryAction(primaryRecoveryAction.actionId)
    : recoveryOffer === null;
  const operatorAttentionBlocksPrimary = operatorAttentionBlocksMotion(
    primaryStartsMotion,
    operatorAlarmsLoaded,
    operatorAlarmsError,
    operatorAlarmsReconciled,
    operatorAlarmHealth.state,
    operatorAlarmAttentionCount,
  );
  useEffect(() => {
    if (pendingRecoveryConfirmation === null) {
      return;
    }
    if (
      recoveryOffer !== null
      && recoveryOffer.offerId === pendingRecoveryConfirmation.offerId
      && recoveryOffer.offerRevision === pendingRecoveryConfirmation.offerRevision
    ) {
      return;
    }
    setPendingRecoveryConfirmation(null);
    setRecoveryCommandNotice(
      recoveryOffer === null
        ? 'The recovery action is no longer available.'
        : 'Recovery state changed. Review and confirm the current action.',
    );
  }, [pendingRecoveryConfirmation, recoveryOffer]);
  // The velocity slider is frozen only while a stop is in flight; after the
  // robot reaches STOPPED the operator may choose the resume scale.
  const frozen = shouldFreezeVelocitySlider(state, stopInProgress, motionStopState);
  const frozenRef = useRef(frozen);
  const wasFrozenRef = useRef(frozen);
  useEffect(() => {
    frozenRef.current = frozen;
  }, [frozen]);

  const motionHeld = motionStopState === 'held';
  const running = state === 'running' && motionStopState === 'idle';
  const stopping = state === 'stopping' || motionStopState === 'stopping' || stopInProgress;
  const runAuthorityAllowsResume =
    runAuthority.canCommand ||
    (
      motionHeld &&
      runAuthority.reason === ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot
    );
  const canRunWithoutRecovery =
    (state === 'idle' || state === 'stopped' || motionHeld) &&
    !stopping &&
    runAuthorityAllowsResume &&
    !stopGate.blocked &&
    !runtimeBlocksMotion;
  const canRun = !operatorAttentionBlocksPrimary && (
    recoveryOffer !== null
      ? Boolean(
          primaryRecoveryAction?.enabled
          && (
            !primaryRecoveryAction
            || !isMotionProducingRecoveryAction(primaryRecoveryAction.actionId)
            || !runtimeBlocksMotion
          )
        )
      : canRunWithoutRecovery
  );
  // Stop applies to in-flight motion the operator can see — BT
  // execution (state==='running') or an intervention / direct-jog
  // session (direct_control).
  const canStop =
    !stopping &&
    motionStopState === 'idle' &&
    (state === 'running' || state === 'direct_control');
  const canFullStop = !cycleBusy && state === 'running';

  const runCycleCommand = useCallback(
    async (command: () => Promise<unknown>) => {
      setCycleBusy(true);
      setRecoveryCommandNotice(null);
      try {
        await command();
      } catch (error) {
        // The cycle store preserves the authoritative cycle error and newest
        // offer. Keep command rejection separate and visible to the operator.
        setRecoveryCommandNotice(recoveryCommandFailureMessage(error));
      } finally {
        setCycleBusy(false);
      }
    },
    [],
  );

  const requestRecoveryAction = useCallback(
    (actionId: string, requiresConfirmation: boolean) => {
      if (!recoveryOffer) {
        return;
      }
      setRecoveryCommandNotice(null);
      if (requiresConfirmation) {
        setPendingRecoveryConfirmation({
          offerId: recoveryOffer.offerId,
          offerRevision: recoveryOffer.offerRevision,
          actionId,
        });
        return;
      }
      if (isMotionProducingRecoveryAction(actionId)) {
        // Submit current motion intent. The backend resolves the latest offer
        // atomically, avoiding the stale cached-revision two-click failure.
        void runCycleCommand(run);
        return;
      }
      void runCycleCommand(() => applyRecoveryAction(recoveryOffer, actionId));
    },
    [applyRecoveryAction, recoveryOffer, run, runCycleCommand],
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

  // A stop is "in progress" from the moment Stop is pressed until motion is
  // held or the cycle reaches a terminal non-running state. Clearing here
  // avoids latching the flag forever.
  useEffect(() => {
    if (shouldClearStopInProgress(state, motionStopState)) {
      setStopInProgress(false);
    }
  }, [state, motionStopState]);

  // Reset is a no-op from idle; from any other state it discards live cycle
  // state (stopped position, latched errors, run context) — give it an
  // amber accent exactly when it carries that weight.
  const resetMeaningful = state !== 'idle';
  const resetBlockedByStop = state === 'stopping' || motionStopState === 'stopping' || stopInProgress;
  const resetTitle = resetBlockedByStop
    ? 'Wait for the robot to report STOPPED before resetting'
    : motionHeld
      ? 'Reset from held stop: clears the held motion branch and rebuilds runtime state'
      : resetMeaningful
        ? `Reset the cycle to its initial state (discards the current ${state} state)`
        : 'Reset the cycle to its initial state';
  const primaryControlLabel = primaryRecoveryAction
    ? recoveryActionLabel(primaryRecoveryAction)
    : recoveryOffer
      ? recoveryOffer.status === 'waiting'
        ? 'Recovery waiting'
        : recoveryOffer.status === 'reset_required'
          ? 'Reset required'
          : 'Recovery unavailable'
      : running
        ? 'Running'
        : motionHeld
          ? 'Resume Motion'
          : 'Run';
  const primaryControlTitle = operatorAttentionBlocksPrimary
    ? operatorAlarmsError
      ? 'Operator-alarm status cannot be read; motion remains disabled'
      : !operatorAlarmsLoaded
        ? 'Checking operator-alarm state before enabling motion'
        : operatorAlarmHealth.state !== 'available'
          ? 'Operator-alarm manager is unavailable; motion remains disabled'
          : !operatorAlarmsReconciled
            ? 'Operator-alarm sources are not reconciled; motion remains disabled'
            : `Acknowledge ${operatorAlarmAttentionCount} alarm${operatorAlarmAttentionCount === 1 ? '' : 's'} before starting motion`
    : recoveryOffer
    ? primaryRecoveryAction
      ? primaryRecoveryAction.disabledReasonCode
        ?? (
          primaryRecoveryAction.requiresConfirmation
            ? `${recoveryActionLabel(primaryRecoveryAction)} requires confirmation`
            : recoveryActionLabel(primaryRecoveryAction)
        )
      : `${recoveryOfferHeadline(recoveryOffer)}: ${recoveryOffer.reasonCode}`
    : canRun
      ? motionHeld
        ? 'Resume the held motion branch'
        : 'Start the loaded program'
      : stopGate.blocked && stopGate.reason !== null
        ? stopGate.reason
        : runtimeBlocksMotion
          ? runtime?.userMessage ?? 'Execution admission is closed'
          : runAuthority.reason !== null && !runAuthorityAllowsResume
            ? runAuthority.reason
            : stopping
              ? 'Robot is stopping'
              : `Cycle is ${state}`;
  const resetControlDisabled = cycleBusy || (
    resetRecoveryAction
      ? !resetRecoveryAction.enabled
      : resetBlockedByStop
  );
  const resetControlTitle = resetRecoveryAction
    ? resetRecoveryAction.disabledReasonCode
      ?? (
        resetRecoveryAction.requiresConfirmation
          ? 'Reset cycle requires confirmation'
          : recoveryActionLabel(resetRecoveryAction)
      )
    : resetTitle;

  return (
    <div className="pointer-events-none absolute inset-x-0 bottom-3 z-30 flex flex-col items-center gap-2">
      {operatorAlarmsLoaded && operatorAlarmAttentionCount > 0 && (
        <div
          role="status"
          className="pointer-events-auto max-w-md rounded-md border border-red-700/70 bg-red-950/85 px-3 py-1.5 text-xs text-red-200 shadow-lg"
        >
          Acknowledge {operatorAlarmAttentionCount} alarm{operatorAlarmAttentionCount === 1 ? '' : 's'}
          {' '}before resuming, retrying, or replanning motion.
        </div>
      )}
      {(operatorAlarmsError || (
        operatorAlarmsLoaded
        && (!operatorAlarmsReconciled || operatorAlarmHealth.state !== 'available')
      )) && (
        <div
          role="alert"
          className="pointer-events-auto max-w-md rounded-md border border-red-700/70 bg-red-950/85 px-3 py-1.5 text-xs text-red-200 shadow-lg"
        >
          Operator-alarm state is unavailable or not reconciled; motion controls remain disabled.
        </div>
      )}
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
      {stopExecutionWarning && (
        <div
          role="status"
          className="pointer-events-auto max-w-md rounded-md border border-amber-600/70 bg-amber-950/85 px-3 py-1.5 text-xs text-amber-200 shadow-lg"
        >
          {stopExecutionWarning}
        </div>
      )}
      {recoveryCommandNotice && (
        <RecoveryCommandNotice
          message={recoveryCommandNotice}
          onDismiss={() => setRecoveryCommandNotice(null)}
        />
      )}
      {confirmationAction && recoveryOffer && (
        <div
          role="dialog"
          aria-label={`${recoveryActionLabel(confirmationAction)} confirmation`}
          className="pointer-events-auto w-[min(520px,calc(100vw-24px))] rounded border border-amber-500/70 bg-gray-950/95 px-3 py-2 text-xs text-amber-100 shadow-xl shadow-black/50"
        >
          <div className="font-semibold">Confirm {recoveryActionLabel(confirmationAction)}</div>
          <div className="mt-1 leading-5 text-amber-100/85">
            {recoveryActionConfirmationText(confirmationAction)}
          </div>
          <div className="mt-2 flex gap-2">
            <button
              type="button"
              disabled={
                cycleBusy
                || !confirmationAction.enabled
                || (
                  isMotionProducingRecoveryAction(confirmationAction.actionId)
                  && runtimeBlocksMotion
                )
              }
              onClick={() => {
                setPendingRecoveryConfirmation(null);
                if (isMotionProducingRecoveryAction(confirmationAction.actionId)) {
                  void runCycleCommand(run);
                } else {
                  void runCycleCommand(() =>
                    applyRecoveryAction(recoveryOffer, confirmationAction.actionId),
                  );
                }
              }}
              className="rounded border border-amber-400/70 bg-amber-800 px-2.5 py-1 font-semibold text-white hover:bg-amber-700 disabled:cursor-not-allowed disabled:opacity-50"
            >
              Confirm
            </button>
            <button
              type="button"
              disabled={cycleBusy}
              onClick={() => setPendingRecoveryConfirmation(null)}
              className="rounded border border-gray-600 bg-gray-800 px-2.5 py-1 font-semibold text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
            >
              Cancel
            </button>
          </div>
        </div>
      )}
      <div className="pointer-events-auto flex items-center gap-2 rounded-lg border border-gray-600/80 bg-gray-900/90 px-3 py-2 shadow-xl shadow-black/50 backdrop-blur-sm">
        <button
          type="button"
          onClick={() => {
            if (recoveryOffer && primaryRecoveryAction) {
              requestRecoveryAction(
                primaryRecoveryAction.actionId,
                primaryRecoveryAction.requiresConfirmation,
              );
            } else {
              void runCycleCommand(run);
            }
          }}
          disabled={cycleBusy || !canRun}
          title={primaryControlTitle}
          className={`flex items-center gap-1.5 rounded px-3.5 py-1.5 text-sm font-semibold transition-colors ${
            canRun && !cycleBusy
              ? primaryIsAcknowledgement
                ? 'bg-amber-700 text-white hover:bg-amber-600'
                : 'bg-emerald-600 text-white hover:bg-emerald-500'
              : running
                ? 'border border-emerald-600/60 bg-emerald-950/60 text-emerald-400'
                : 'border border-gray-600 bg-gray-800 text-gray-500'
          } disabled:cursor-not-allowed`}
        >
          {primaryIsAcknowledgement ? (
            <svg viewBox="0 0 16 16" className="h-3.5 w-3.5 fill-none stroke-current" strokeWidth="1.8" aria-hidden>
              <path d="m3 8 3 3 7-7" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
          ) : (
            <svg
              viewBox="0 0 16 16"
              className={`h-3 w-3 fill-current ${running ? 'animate-pulse' : ''}`}
              aria-hidden
            >
              <path d="M4 2.5v11l9-5.5z" />
            </svg>
          )}
          <span className="min-w-[6.5rem] text-center">{primaryControlLabel}</span>
        </button>
        <div className="relative flex">
          <button
            type="button"
            onClick={() => {
              setShowFullStopMenu(false);
              // Freeze the velocity slider immediately on Stop, before the next
              // status poll reports the stopping/stopped state.
              setStopInProgress(true);
              // Stop active robot motion, then force a scene refresh so the
              // viewer reflects the held pose without waiting a full poll interval.
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
            title={
              stopping
                ? 'Stop command is in progress'
                : canStop
                  ? 'Stop active robot motion; eligible background branches keep ticking'
                  : `Cycle is ${state}`
            }
            className={`flex items-center gap-1.5 rounded-l px-3.5 py-1.5 text-sm font-semibold transition-colors ${
              canStop && !cycleBusy
                ? 'bg-red-700 text-white hover:bg-red-600'
                : stopping
                  ? 'border border-amber-600/70 bg-amber-950/50 text-amber-300'
                : 'border border-gray-600 bg-gray-800 text-gray-500'
            } disabled:cursor-not-allowed`}
          >
            <svg
              viewBox="0 0 16 16"
              className={`h-3 w-3 fill-current ${stopping ? 'animate-pulse' : ''}`}
              aria-hidden
            >
              <rect x="3" y="3" width="10" height="10" rx="1" />
            </svg>
            <span className="grid text-center">
              <span className={`col-start-1 row-start-1 ${stopping ? 'invisible' : ''}`}>Stop</span>
              <span className={`col-start-1 row-start-1 ${stopping ? '' : 'invisible'}`}>Stopping</span>
            </span>
          </button>
          <button
            type="button"
            disabled={!canFullStop}
            onClick={() => setShowFullStopMenu((current) => !current)}
            title="More stop actions. Full Stop halts background tree branches too; use it when the whole program must freeze, not just robot motion."
            className={`border-l px-2 text-xs font-semibold transition-colors ${
              canFullStop
                ? 'rounded-r bg-red-800 text-white hover:bg-red-700'
                : 'rounded-r border-gray-600 bg-gray-800 text-gray-500'
            } disabled:cursor-not-allowed`}
            aria-haspopup="menu"
            aria-expanded={showFullStopMenu}
          >
            v
          </button>
          {showFullStopMenu && canFullStop && (
            <div
              role="menu"
              className="absolute bottom-9 right-0 z-40 w-56 rounded-md border border-gray-600 bg-gray-950 p-1 text-xs text-gray-100 shadow-xl"
            >
              <button
                type="button"
                role="menuitem"
                className="w-full rounded px-2 py-1.5 text-left hover:bg-red-950/80"
                title="Stops active motion and halts every behavior-tree branch until Run or Reset."
                onClick={() => {
                  setShowFullStopMenu(false);
                  setStopInProgress(true);
                  runCycleCommand(async () => {
                    await fullStop();
                    try {
                      await useSceneStateStore.getState().refresh();
                    } catch (err) {
                      console.warn('post-full-stop scene refresh failed', err);
                    }
                  });
                }}
              >
                <div className="font-semibold">Full Stop</div>
                <div className="mt-0.5 text-[11px] leading-4 text-gray-400">
                  Halt background tree branches too.
                </div>
              </button>
            </div>
          )}
        </div>
        <button
          type="button"
          onClick={() => {
            if (resetRecoveryAction) {
              requestRecoveryAction(
                resetRecoveryAction.actionId,
                resetRecoveryAction.requiresConfirmation,
              );
            } else {
              void runCycleCommand(reset);
            }
          }}
          disabled={resetControlDisabled}
          title={resetControlTitle}
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
        <div className="mx-1 h-6 w-px bg-gray-600/80" />
        <label
          className="flex items-center gap-1.5 text-xs text-gray-300"
          title={
            frozen
              ? 'Velocity locked while the robot is stopping'
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
