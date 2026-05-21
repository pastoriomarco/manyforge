import { useCallback, useEffect, useRef } from 'react';

import type { BeginOptions, StreamingFrame } from '../api/followTargetWs';
import {
  followTargetController,
  type FollowTargetDragToken,
} from './followTargetController';

export interface UseFollowTargetSessionOptions {
  // Build the kernel `begin` payload at the moment the session needs
  // to open. Return null to skip opening (e.g. no joint names known
  // yet, or no tcp_frame available). The caller's `push` is dropped
  // silently in that case rather than spamming begin rejections.
  buildBeginParams: () => BeginOptions | null;
  // Probe whether the session should close. Receives the timestamp of
  // the first push since the last close (in performance.now() time,
  // or null if no push has happened yet). Return true to close
  // (converged OR timed out — the caller decides whether to warn).
  arrived: (firstPendingAtMs: number | null) => boolean;
  // Called whenever the session closes for any reason (timer probe,
  // external idle/aborted from the controller, or unmount). The
  // caller uses this to clear its own bookkeeping (last-target maps,
  // throttle state, etc.).
  onSessionClose: () => void;
  // Re-probe / close interval in ms after the last push. The arrived
  // callback runs at this cadence until it returns true.
  endDebounceMs?: number;
  // Used as a prefix on the controller-abort message at unmount, so
  // observability traces can tell apart the two callers.
  hookDescription: string;
}

export interface UseFollowTargetSessionApi {
  push: (frame: StreamingFrame) => void;
}

// Shared streaming-session lifecycle for direct-control hooks. Used by
// useJointStreaming and useCartesianStreaming. Owns the token, the
// end-debounce timer, the first-pending-target timestamp, the lazy
// begin, and the recovery channel that clears local refs when the
// controller drops to idle/aborted under us (ControlBar Stop/Reset,
// fatal kernel rejection, network drop).
//
// What it intentionally does NOT own:
//   - the caller's target storage (per-joint map vs single pose etc).
//     The caller's `arrived` reads its own ref and compares to the
//     live scene snapshot.
//   - the caller's push throttle. Apply throttling before calling
//     `push` (the joint hook does this for its slider stream).
export function useFollowTargetSession({
  buildBeginParams,
  arrived,
  onSessionClose,
  endDebounceMs = 500,
  hookDescription,
}: UseFollowTargetSessionOptions): UseFollowTargetSessionApi {
  const tokenRef = useRef<FollowTargetDragToken | null>(null);
  // 'queued' = begin() returned a token but the controller was in
  // 'ending' / 'aborted', so the session is parked behind a previous
  // one finalising. 'live' = controller has reached 'opening' or
  // 'streaming' for our token. The recovery handler uses this to
  // skip clearing local state on the 'idle' that promotes our queued
  // begin to live: clearing there would orphan token_2 the moment
  // the controller calls _begin_now() for it.
  const tokenStatusRef = useRef<'queued' | 'live' | null>(null);
  const endTimerRef = useRef<number | null>(null);
  const firstPendingTargetAtRef = useRef<number | null>(null);

  // Refs for caller callbacks so the timer + subscription paths always
  // see the latest closures without forcing useEffect re-runs.
  const buildBeginRef = useRef(buildBeginParams);
  const arrivedRef = useRef(arrived);
  const onSessionCloseRef = useRef(onSessionClose);
  useEffect(() => {
    buildBeginRef.current = buildBeginParams;
    arrivedRef.current = arrived;
    onSessionCloseRef.current = onSessionClose;
  }, [buildBeginParams, arrived, onSessionClose]);

  const armEndRef = useRef<() => void>(() => {});
  const hookDescriptionRef = useRef(hookDescription);
  useEffect(() => {
    hookDescriptionRef.current = hookDescription;
  }, [hookDescription]);

  const clearLocalState = useCallback((): void => {
    tokenRef.current = null;
    tokenStatusRef.current = null;
    firstPendingTargetAtRef.current = null;
    if (endTimerRef.current !== null) {
      window.clearTimeout(endTimerRef.current);
      endTimerRef.current = null;
    }
    onSessionCloseRef.current();
  }, []);

  const markFirstPendingTarget = useCallback((): void => {
    if (firstPendingTargetAtRef.current === null) {
      firstPendingTargetAtRef.current = performance.now();
    }
  }, []);

  const tryCloseOnArrival = useCallback((): boolean => {
    const token = tokenRef.current;
    if (token === null) return true;
    const shouldClose = arrivedRef.current(firstPendingTargetAtRef.current);
    if (!shouldClose) return false;
    followTargetController.end(token, 'spring_back');
    clearLocalState();
    return true;
  }, [clearLocalState]);

  const armEnd = useCallback((): void => {
    if (endTimerRef.current !== null) window.clearTimeout(endTimerRef.current);
    endTimerRef.current = window.setTimeout(() => {
      endTimerRef.current = null;
      if (!tryCloseOnArrival()) armEndRef.current();
    }, endDebounceMs);
  }, [endDebounceMs, tryCloseOnArrival]);
  useEffect(() => {
    armEndRef.current = armEnd;
  }, [armEnd]);

  const ensureSession = useCallback((): FollowTargetDragToken | null => {
    if (tokenRef.current !== null) return tokenRef.current;
    const beginOpts = buildBeginRef.current();
    if (beginOpts === null) return null;
    const token = followTargetController.begin(beginOpts);
    if (token === null) return null;
    tokenRef.current = token;
    // Mark queued; the recovery handler promotes to 'live' when it
    // observes phase='opening' or 'streaming' for our token. Begin
    // calls into a controller in phase='ending'/'aborted' park behind
    // the previous session's finalize — without this status flag, the
    // 'idle' fired by that finalize would clear our refs and orphan
    // the queued session.
    tokenStatusRef.current = 'queued';
    return token;
  }, []);

  const push = useCallback((frame: StreamingFrame): void => {
    const token = ensureSession();
    if (token === null) return;
    if (tokenStatusRef.current !== 'queued') {
      markFirstPendingTarget();
    }
    followTargetController.push(token, frame);
    armEnd();
  }, [armEnd, ensureSession, markFirstPendingTarget]);

  useEffect(() => {
    // Recovery channel: if the controller drops out from under us,
    // clear local refs so the next push opens a fresh session
    // instead of pushing into a dead one and looking frozen. Two
    // teardown paths to handle:
    //   - 'aborted': controller-initiated abort (ControlBar Stop/Reset,
    //     fatal server rejection, network drop). Our session is dead
    //     even if it was only queued — abort() also clears the
    //     controller's pendingBeginToken. Always clear here.
    //   - 'idle' on a *live* session: the controller closed the
    //     socket without our end() (server disconnect mid-stream).
    //     Clear so the next push reopens.
    // NOT cleared:
    //   - 'idle' after our own end(): clearLocalState already ran
    //     inside tryCloseOnArrival, so tokenRef is null and the
    //     guard skips.
    //   - 'idle' while our token is 'queued': this is the finalize()
    //     of the *previous* session that owned the controller. The
    //     controller is about to fire _begin_now() for our queued
    //     token. Clearing here would orphan that token the instant
    //     the next phase change ('opening') lands.
    const unsubscribe = followTargetController.subscribe((phase) => {
      if (
        (phase === 'opening' || phase === 'streaming')
        && tokenRef.current !== null
      ) {
        const wasQueued = tokenStatusRef.current === 'queued';
        tokenStatusRef.current = 'live';
        if (wasQueued) {
          // Start convergence/timeout accounting only after the
          // controller has actually admitted the session. A push that
          // queues behind a previous session may sit for hundreds of
          // milliseconds before the new WS begins; charging that time
          // against arrival can close slow robot motion early.
          markFirstPendingTarget();
        }
        return;
      }
      if (tokenRef.current === null) return;
      if (phase === 'aborted') {
        clearLocalState();
        return;
      }
      if (phase === 'idle' && tokenStatusRef.current === 'live') {
        clearLocalState();
      }
    });
    return () => {
      unsubscribe();
      if (endTimerRef.current !== null) {
        window.clearTimeout(endTimerRef.current);
        endTimerRef.current = null;
      }
      if (tokenRef.current !== null) {
        followTargetController.abort(`${hookDescriptionRef.current} unmount`);
        tokenRef.current = null;
      }
      firstPendingTargetAtRef.current = null;
    };
  }, [clearLocalState, markFirstPendingTarget]);

  return { push };
}
