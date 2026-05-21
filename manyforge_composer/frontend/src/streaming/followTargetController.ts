/**
 * Singleton follow-target session controller (ADR-0016).
 *
 * Owns the WebSocket session lifecycle independent of React components.
 * Previously TcpJogGizmo held the session in local refs which reset on
 * any component remount or re-render race, allowing duplicate WS opens
 * during fast rejection streams. Centralizing here gives us a single
 * authoritative state machine for the streaming session:
 *
 *   idle  --begin()-->  opening  --(ready)-->  streaming
 *                           |                      |
 *                           +----- failure -----> aborted
 *                                                  |
 *                                                  +--> (auto reset to idle)
 *                       streaming --end()--> ending --(close)--> idle
 *                       streaming --abort()--> aborted --> idle
 *
 * Invariants:
 *   - At most one session active globally.
 *   - begin() is idempotent: returns a drag token when admitted/queued,
 *     or null when another active drag already owns the session.
 *   - push() is a no-op outside the streaming phase.
 *   - abort() always closes the underlying WebSocket cleanly.
 *   - The Zustand store carries UI-visible state (rejection label /
 *     anchor); React components subscribe to the store, never to the
 *     controller's internal state directly.
 */

import {
  openFollowTargetSession,
  type BeginOptions,
  type FollowTargetPushAck,
  type FollowTargetSession,
  type StreamingFrame,
} from '../api/followTargetWs';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';

export type FollowTargetPhase =
  | 'idle'
  | 'opening'
  | 'streaming'
  | 'ending'
  | 'aborted';

export type FollowTargetDragToken = number;

/** Coarse classification of per-frame rejection reasons. */
function isFatalRejectionReason(reasonCode: string): boolean {
  const r = (reasonCode || '').toLowerCase();
  // Session-level failures — the session is broken; transient drag
  // can't recover. Anything else (IK fail, missing TCP frame after a
  // mid-session reconfig, etc.) is treated as transient and only
  // surfaces the magenta indicator. Keep this list in sync with the
  // fatal codes enumerated in
  // manyforge_specs/docs/spec/200-kernel-api-state-events.md §2.
  return (
    r === 'session_lost'
    || r === 'invalid_state'
    || r === 'session_already_active'
    || r === 'no_controller'
    || r === 'unsupported_capability'
  );
}

function rejectionReasonToShortLabel(reasonCode: string): string {
  switch ((reasonCode || '').toLowerCase()) {
    case 'ik_failed':
      return 'OUT OF REACH';
    case 'missing_tcp_frame':
      return 'TCP NOT SET';
    case 'no_controller':
      return 'NO CONTROLLER';
    case 'stale_state':
      return 'STALE STATE';
    case 'session_lost':
      return 'SESSION LOST';
    case 'session_already_active':
      return 'SESSION BUSY';
    case 'invalid_state':
      return 'WRONG STATE';
    case 'malformed_frame':
      return 'BAD TARGET';
    case 'unsupported_capability':
      return 'UNSUPPORTED';
    case 'unknown_joint_name':
      return 'BAD JOINT NAME';
    default:
      return 'REJECTED';
  }
}

class FollowTargetController {
  private _phase: FollowTargetPhase = 'idle';
  private session: FollowTargetSession | null = null;
  private listeners: Set<(phase: FollowTargetPhase) => void> = new Set();
  // Latest anchor pose pushed; used by the magenta indicator to follow
  // the gizmo around the unreachable region without coupling the
  // indicator's anchor source to the gizmo's React state.
  private lastTargetPose: {
    position: [number, number, number];
    quaternion: [number, number, number, number];
  } | null = null;
  // Deferred begin: when an old session is still finishing (phase
  // 'ending' or 'aborted'), a fresh begin() request queues here and
  // auto-fires once finalize() lands us in 'idle'. This way the
  // operator can release-then-grab in rapid succession without the
  // second grab being silently dropped — they get a small delay (the
  // kernel's end_streaming_session, ~100 ms with the to_thread fix)
  // instead of a multi-second dead window.
  private pendingBeginOpts: BeginOptions | null = null;
  private pendingBeginToken: FollowTargetDragToken | null = null;
  private pendingFramesByToken: Map<FollowTargetDragToken, StreamingFrame> =
    new Map();
  private activeDragToken: FollowTargetDragToken | null = null;
  private nextDragToken = 1;

  phase(): FollowTargetPhase {
    return this._phase;
  }

  subscribe(cb: (phase: FollowTargetPhase) => void): () => void {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  waitForIdle(timeoutMs = 3000): Promise<boolean> {
    if (this._phase === 'idle') return Promise.resolve(true);
    return new Promise((resolve) => {
      let done = false;
      let unsubscribe: (() => void) | null = null;
      const finish = (ok: boolean): void => {
        if (done) return;
        done = true;
        if (unsubscribe !== null) unsubscribe();
        window.clearTimeout(timer);
        resolve(ok);
      };
      const timer = window.setTimeout(() => finish(false), timeoutMs);
      unsubscribe = this.subscribe((phase) => {
        if (phase === 'idle') finish(true);
      });
    });
  }

  abortAndWaitForIdle(reason: string, timeoutMs = 3000): Promise<boolean> {
    if (this._phase === 'idle') return Promise.resolve(true);
    this.abort(reason);
    return this.waitForIdle(timeoutMs);
  }

  private setPhase(next: FollowTargetPhase): void {
    if (this._phase === next) return;
    this._phase = next;
    for (const cb of this.listeners) {
      try {
        cb(next);
      } catch {
        // listener errors don't propagate
      }
    }
  }

  /**
   * Open a streaming session. Returns a drag token if the request is
   * accepted in any form — either started immediately (phase was
   * idle) or queued for after the in-flight session finishes (phase
   * was ending or aborted). Returns null only if another session is
   * actively running (phase opening/streaming) — true double-begin.
   *
   * The caller (gizmo drag handler) should always proceed with the
   * local drag state when this returns a token, regardless of whether
   * the session opens immediately or queues. Pushes for a queued token
   * are retained latest-wins and replayed once the deferred session
   * opens, so a spinner click cannot lose its only target while the
   * previous session is still ending.
   */
  begin(opts: BeginOptions): FollowTargetDragToken | null {
    const token = this.nextDragToken++;
    if (this._phase === 'opening' || this._phase === 'streaming') {
      console.warn(
        'FollowTargetController.begin rejected: session already active (phase=',
        this._phase,
        ')',
      );
      return null;
    }
    if (this._phase === 'ending' || this._phase === 'aborted') {
      // Queue. Fires from finalize() when we land in idle.
      this.pendingBeginOpts = opts;
      this.pendingBeginToken = token;
      return token;
    }
    return this._begin_now(opts, token) ? token : null;
  }

  /**
   * Cancel any deferred begin (e.g. operator released the gizmo before
   * the queued session had a chance to fire). Safe to call always.
   */
  cancelPendingBegin(token?: FollowTargetDragToken | null): void {
    if (
      token !== undefined
      && token !== null
      && this.pendingBeginToken !== null
      && this.pendingBeginToken !== token
    ) {
      return;
    }
    if (token !== undefined && token !== null) {
      this.pendingFramesByToken.delete(token);
    } else {
      this.pendingFramesByToken.clear();
    }
    this.pendingBeginOpts = null;
    this.pendingBeginToken = null;
  }

  private _begin_now(
    opts: BeginOptions,
    token: FollowTargetDragToken,
  ): boolean {
    this.pendingBeginOpts = null;
    this.pendingBeginToken = null;
    this.lastTargetPose = null;
    this.activeDragToken = token;
    this.setPhase('opening');
    const session = openFollowTargetSession(opts);
    this.session = session;
    const queuedFrame = this.pendingFramesByToken.get(token);
    if (queuedFrame !== undefined) {
      this.pendingFramesByToken.delete(token);
      this.rememberFramePose(queuedFrame);
      session.pushTarget(queuedFrame);
    }

    session.onError((code, message) => {
      this.abort(`server error: ${code} ${message}`);
    });

    session.onAck((ack: FollowTargetPushAck) => {
      if (ack.accepted) {
        useViewerSelectionStore.getState().setCartesianRejection(null, null);
        return;
      }
      // Distinguish fatal session-level rejection from transient per-
      // frame rejection (IK fail). Fatal: close the session. Transient:
      // just surface the magenta indicator and keep the session open.
      if (isFatalRejectionReason(ack.reasonCode)) {
        this.abort(`fatal rejection: ${ack.reasonCode}: ${ack.reason}`);
        return;
      }
      const label = rejectionReasonToShortLabel(ack.reasonCode);
      useViewerSelectionStore
        .getState()
        .setCartesianRejection(label, this.lastTargetPose);
    });

    session.onClose((clean, reason) => {
      // Expected close paths: phase=ending (we initiated end), or
      // phase=aborted (we initiated abort). Anything else is unexpected.
      if (this._phase === 'streaming' || this._phase === 'opening') {
        this.finalize(
          `socket closed${clean ? '' : ' (unclean)'}: ${reason}`,
        );
      } else {
        // Already in ending/aborted/idle — finalize to idle.
        this.finalize('socket closed during cleanup');
      }
    });

    void session.ready
      .then(() => {
        if (this._phase === 'opening') {
          this.setPhase('streaming');
        }
      })
      .catch((err) => {
        this.abort(`begin failed: ${err instanceof Error ? err.message : err}`);
      });

    return true;
  }

  push(token: FollowTargetDragToken | null, frame: StreamingFrame): void {
    if (token === null) return;
    if (token !== this.activeDragToken) {
      if (token === this.pendingBeginToken) {
        this.pendingFramesByToken.set(token, frame);
        this.rememberFramePose(frame);
      }
      return;
    }
    // Accept pushes during 'opening' as well as 'streaming'. The WS
    // client buffers the latest frame in `pendingFrame` and flushes
    // it as soon as the server's `began` message arrives, so the
    // first slider/gizmo drag isn't lost to the WebSocket-connect
    // round-trip (~50-500 ms depending on link). Drop everything
    // else (idle, ending, aborted) — those phases have either no
    // session yet or one being torn down.
    if (this._phase !== 'streaming' && this._phase !== 'opening') return;
    if (this.session === null) return;
    this.rememberFramePose(frame);
    try {
      this.session.pushTarget(frame);
    } catch (err) {
      console.warn('push failed', err);
    }
  }

  /**
   * Clean release. Sends final pose-end + soft-stop strategy. Transitions
   * idle on socket close.
   */
  end(
    token: FollowTargetDragToken | null,
    strategy: 'spring_back' | 'velocity_scaling' = 'spring_back',
  ): void {
    this.cancelPendingBegin(token);
    if (token === null || token !== this.activeDragToken) return;
    if (this._phase !== 'streaming' && this._phase !== 'opening') return;
    const session = this.session;
    if (session === null) {
      this.finalize('end with no session');
      return;
    }
    this.activeDragToken = null;
    this.setPhase('ending');
    void session.end(strategy);
    // finalize will fire when onClose lands; the session client has
    // its own watchdog that force-closes if the server is slow.
  }

  /**
   * Force-abort the session. Always closes the underlying WebSocket;
   * the server's `finally` cleanup runs end_streaming_session with
   * spring_back. Distinct from end() in that we don't wait for a graceful
   * exchange — used on session-level failures and on user-initiated
   * cycle reset / stop where the kernel session may already be gone.
   */
  abort(reason: string): void {
    if (this._phase === 'idle' || this._phase === 'aborted') return;
    console.warn('FollowTargetController.abort:', reason);
    this.setPhase('aborted');
    this.pendingBeginOpts = null;
    this.pendingBeginToken = null;
    this.pendingFramesByToken.clear();
    this.activeDragToken = null;
    const session = this.session;
    if (session !== null) {
      try {
        // Hard client-side abort: drop queued targets and close the socket.
        // The server's finally block remains responsible for releasing any
        // already-open kernel streaming session.
        session.abort(reason);
      } catch {
        // ignore — onClose handler will finalize
      }
    }
    useViewerSelectionStore.getState().setCartesianRejection(null, null);
    useViewerSelectionStore.getState().setDraggingGizmo(false);
  }

  /**
   * Called from session.onClose (always) to drop refs and return to
   * idle so the next begin() is admitted. If a begin was queued
   * while we were ending/aborted, fire it now.
   */
  private finalize(reason: string): void {
    if (this._phase === 'streaming' || this._phase === 'opening') {
      // Unexpected close — log it.
      console.warn(
        'FollowTargetController unexpected finalize:',
        reason,
        'phase was',
        this._phase,
      );
    }
    this.session = null;
    this.lastTargetPose = null;
    this.activeDragToken = null;
    // Don't clear the gizmo-drag UI state here — the gizmo's local
    // mouseUp already handles that. Clearing it here would race the
    // user's next drag (a fresh mouseDown sets draggingGizmo=true
    // before the previous session's finalize runs, and clearing it
    // here would falsely turn the indicator/lock off).
    this.setPhase('idle');
    // Fire any queued begin from a fresh drag the operator started
    // while we were still cleaning up.
    if (this.pendingBeginOpts !== null && this.pendingBeginToken !== null) {
      const opts = this.pendingBeginOpts;
      const token = this.pendingBeginToken;
      this.pendingBeginOpts = null;
      this.pendingBeginToken = null;
      this._begin_now(opts, token);
    }
  }

  private rememberFramePose(frame: StreamingFrame): void {
    if (frame.kind !== 'cartesian') {
      // Joint streaming intentionally does not maintain lastTargetPose:
      // the magenta out-of-reach indicator is a Cartesian-only concept
      // (operator-controlled pose disagreeing with what IK can reach).
      // Joint sliders bypass IK and are bounded by joint limits at the
      // backend, so there is no equivalent indicator anchor to track.
      return;
    }
    this.lastTargetPose = {
      position: [...frame.positionM],
      quaternion: [...frame.orientationXyzw],
    };
  }
}

export const followTargetController = new FollowTargetController();
