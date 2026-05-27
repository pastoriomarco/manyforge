/**
 * Streaming-target WebSocket client (ADR-0016).
 *
 * Single-session per client. Open one with `openFollowTargetSession`; push
 * frames as the operator drags / streams; close when released. The client
 * is ack-gated latest-wins: at most one frame is in flight, and if
 * `pushTarget` is called faster than the server can process frames only the
 * latest pending pose is retained for the next send. This prevents stale
 * unreachable targets from building up in the browser/WebSocket buffers.
 *
 * Wire format mirrors `manyforge_composer/backend/follow_target_ws.py`.
 */

const BASE_URL = import.meta.env.VITE_API_URL || '';

type Kind = 'cartesian' | 'joint';

export interface CartesianFrame {
  kind: 'cartesian';
  positionM: [number, number, number];
  orientationXyzw: [number, number, number, number];
}

export interface JointFrame {
  kind: 'joint';
  jointNames?: string[];
  jointPositionsRad: number[];
}

export type StreamingFrame = CartesianFrame | JointFrame;

export interface BeginOptions {
  groupId?: string;
  jointNames?: string[];
  inputMode?: Kind;
  cartesianReferenceFrame?: 'base' | 'world' | 'tcp';
  /** TCP frame name. Required for cartesian-mode sessions (ADR-0016 IK pre-resolution). */
  tcpFrame?: string;
  softStopStrategy?: 'spring_back' | 'velocity_scaling';
  softStopDecelerationTimeS?: number;
  anchorNegativeTimeS?: number;
  targetTimeFromStartMinS?: number;
  maxDispatchRateHz?: number;
  targetMaxAgeS?: number;
  velocityScale?: number;
  principalId?: string;
  authoritySource?: 'human' | 'llm' | 'program_node' | 'policy';
  triggerReason?: string;
  maxSessionDurationS?: number;
}

export interface FollowTargetPushAck {
  sessionId: number;
  sourceSequence: number;
  accepted: boolean;
  backendSequence: number;
  reasonCode: string;
  reason: string;
  details: Record<string, unknown>;
  durationMs: number;
  kind: 'cartesian' | 'joint';
}

export interface FollowTargetEndAck {
  sessionId: number;
  accepted: boolean | null;
  reason: string;
}

export interface FollowTargetSession {
  pushTarget: (frame: StreamingFrame) => void;
  end: (
    softStopStrategy?: 'spring_back' | 'velocity_scaling',
  ) => Promise<void>;
  abort: (reason?: string) => void;
  readonly ready: Promise<void>;
  readonly state: 'connecting' | 'ready' | 'closing' | 'closed';
  /** Fires when the server sends an `op: error` message. */
  readonly onError: (cb: (code: string, message: string) => void) => void;
  /** Fires when the server sends a per-frame `op: pushed` ack (accepted or rejected). */
  readonly onAck: (cb: (ack: FollowTargetPushAck) => void) => void;
  /** Fires when the socket closes for any reason. `clean` mirrors WebSocket spec. */
  readonly onClose: (cb: (clean: boolean, reason: string) => void) => void;
  readonly sessionId: () => number;
  /** Most-recent rejected ack received, or null if none. UI can read this on close to explain stalls. */
  readonly lastRejected: () => FollowTargetPushAck | null;
  readonly lastEnded: () => FollowTargetEndAck | null;
}

function buildWsUrl(): string {
  // Match the convention used by the existing state WS (StateWebSocket).
  if (BASE_URL) {
    const httpUrl = new URL(BASE_URL, window.location.origin);
    httpUrl.pathname = '/ws/follow-target';
    httpUrl.protocol = httpUrl.protocol === 'https:' ? 'wss:' : 'ws:';
    return httpUrl.toString();
  }
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${proto}//${window.location.host}/ws/follow-target`;
}

function toServerPayload(beginOpts: BeginOptions | undefined): Record<string, unknown> {
  const out: Record<string, unknown> = { op: 'begin' };
  if (!beginOpts) return out;
  const remap: Record<string, string> = {
    groupId: 'group_id',
    jointNames: 'joint_names',
    inputMode: 'input_mode',
    cartesianReferenceFrame: 'cartesian_reference_frame',
    tcpFrame: 'tcp_frame',
    softStopStrategy: 'soft_stop_strategy',
    softStopDecelerationTimeS: 'soft_stop_deceleration_time_s',
    anchorNegativeTimeS: 'anchor_negative_time_s',
    targetTimeFromStartMinS: 'target_time_from_start_min_s',
    maxDispatchRateHz: 'max_dispatch_rate_hz',
    targetMaxAgeS: 'target_max_age_s',
    velocityScale: 'velocity_scale',
    principalId: 'principal_id',
    authoritySource: 'authority_source',
    triggerReason: 'trigger_reason',
    maxSessionDurationS: 'max_session_duration_s',
  };
  for (const [k, v] of Object.entries(beginOpts)) {
    if (v === undefined) continue;
    const serverKey = remap[k] ?? k;
    out[serverKey] = v;
  }
  return out;
}

function frameToServerPayload(
  frame: StreamingFrame,
  sequence: number,
): Record<string, unknown> {
  const base: Record<string, unknown> = {
    op: 'frame',
    kind: frame.kind,
    source_timestamp_ns: Math.floor(performance.now() * 1e6),
    source_sequence: sequence,
  };
  if (frame.kind === 'cartesian') {
    base.position_m = frame.positionM;
    base.orientation_xyzw = frame.orientationXyzw;
  } else {
    if (frame.jointNames) base.joint_names = frame.jointNames;
    base.joint_positions_rad = frame.jointPositionsRad;
  }
  return base;
}

/**
 * Open a streaming-target session. The returned promise resolves once
 * the server acknowledges the session is open (or rejects on failure).
 */
export function openFollowTargetSession(
  beginOpts?: BeginOptions,
): FollowTargetSession {
  const ws = new WebSocket(buildWsUrl());
  let state: 'connecting' | 'ready' | 'closing' | 'closed' = 'connecting';
  let sessionId = 0;
  let sequence = 1;
  let pendingFrame: StreamingFrame | null = null;
  let inFlightSequence: number | null = null;
  let resolveReady!: () => void;
  let rejectReady!: (err: Error) => void;
  const ready = new Promise<void>((res, rej) => {
    resolveReady = res;
    rejectReady = rej;
  });
  let onErrorCb: ((code: string, message: string) => void) | null = null;
  let onAckCb: ((ack: FollowTargetPushAck) => void) | null = null;
  let onCloseCb: ((clean: boolean, reason: string) => void) | null = null;
  let resolveEnd: (() => void) | null = null;
  let endWatchdog: ReturnType<typeof setTimeout> | null = null;
  let lastRejectedAck: FollowTargetPushAck | null = null;
  let lastEndedAck: FollowTargetEndAck | null = null;

  ws.onopen = () => {
    ws.send(JSON.stringify(toServerPayload(beginOpts)));
  };

  ws.onmessage = (ev) => {
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(typeof ev.data === 'string' ? ev.data : '');
    } catch {
      return;
    }
    const op = msg.op;
    if (op === 'began') {
      sessionId = Number(msg.session_id) || 0;
      state = 'ready';
      resolveReady();
      flushIfReady();
    } else if (op === 'pushed') {
      const ack: FollowTargetPushAck = {
        sessionId: Number(msg.session_id) || 0,
        sourceSequence: Number(msg.source_sequence) || 0,
        accepted: Boolean(msg.accepted),
        backendSequence: Number(msg.backend_sequence) || 0,
        reasonCode: String(msg.reason_code ?? ''),
        reason: String(msg.reason ?? ''),
        details:
          msg.details !== null && typeof msg.details === 'object'
            ? (msg.details as Record<string, unknown>)
            : {},
        durationMs: Number(msg.duration_ms) || 0,
        kind:
          msg.kind === 'joint' || msg.kind === 'cartesian'
            ? (msg.kind as 'joint' | 'cartesian')
            : 'cartesian',
      };
      const matchedInFlight = ack.sourceSequence === inFlightSequence;
      if (matchedInFlight) {
        inFlightSequence = null;
      }
      if (state !== 'ready') {
        return;
      }
      if (!ack.accepted) lastRejectedAck = ack;
      if (onAckCb) onAckCb(ack);
      if (matchedInFlight) {
        flushIfReady();
      }
    } else if (op === 'ended') {
      lastEndedAck = {
        sessionId: Number(msg.session_id) || 0,
        accepted:
          typeof msg.accepted === 'boolean' ? Boolean(msg.accepted) : null,
        reason: String(msg.reason ?? ''),
      };
      if (state === 'closing') {
        finishEnd();
        try {
          ws.close(1000, 'ended');
        } catch {
          // ignore
        }
      }
    } else if (op === 'error') {
      const code = String(msg.code ?? 'unknown');
      const message = String(msg.message ?? '');
      if (onErrorCb) onErrorCb(code, message);
      if (state === 'connecting') {
        state = 'closed';
        rejectReady(new Error(`${code}: ${message}`));
      }
    }
  };

  ws.onclose = (ev) => {
    const wasConnecting = state === 'connecting';
    state = 'closed';
    pendingFrame = null;
    inFlightSequence = null;
    if (wasConnecting) {
      rejectReady(
        new Error(
          `socket closed before session begin${ev.reason ? `: ${ev.reason}` : ''}`,
        ),
      );
    }
    finishEnd();
    if (onCloseCb) {
      try {
        onCloseCb(Boolean(ev.wasClean), String(ev.reason ?? ''));
      } catch {
        // swallow handler errors so we still fulfill end-promise
      }
    }
  };

  function finishEnd(): void {
    if (endWatchdog !== null) {
      clearTimeout(endWatchdog);
      endWatchdog = null;
    }
    if (resolveEnd) {
      resolveEnd();
      resolveEnd = null;
    }
  }

  ws.onerror = () => {
    if (onErrorCb) onErrorCb('socket_error', 'WebSocket transport error');
  };

  function flushIfReady(): void {
    if (state !== 'ready') return;
    if (inFlightSequence !== null) return;
    if (!pendingFrame) return;
    const frame = pendingFrame;
    pendingFrame = null;
    const currentSequence = sequence++;
    inFlightSequence = currentSequence;
    try {
      ws.send(JSON.stringify(frameToServerPayload(frame, currentSequence)));
    } catch (err) {
      inFlightSequence = null;
      pendingFrame = frame;
      if (onErrorCb) {
        onErrorCb(
          'socket_send_failed',
          err instanceof Error ? err.message : String(err),
        );
      }
    }
  }

  return {
    pushTarget(frame: StreamingFrame): void {
      pendingFrame = frame; // latest-wins
      flushIfReady();
    },
    end(
      softStopStrategy?: 'spring_back' | 'velocity_scaling',
    ): Promise<void> {
      if (state === 'closed') return Promise.resolve();
      pendingFrame = null;
      inFlightSequence = null;
      state = 'closing';
      return new Promise<void>((resolve) => {
        resolveEnd = resolve;
        try {
          ws.send(
            JSON.stringify({
              op: 'end',
              ...(softStopStrategy
                ? { soft_stop_strategy: softStopStrategy }
                : {}),
            }),
          );
        } catch {
          // ignore — close will fire below
        }
        // Prefer the server's `op:ended` acknowledgement. Force-close only
        // as a watchdog so a flaky server cannot strand UI cleanup forever.
        endWatchdog = setTimeout(() => {
          try {
            ws.close();
          } catch {
            // ignore
          }
          finishEnd();
        }, 5000);
      });
    },
    abort(reason?: string): void {
      pendingFrame = null;
      inFlightSequence = null;
      if (state === 'closed') return;
      state = 'closing';
      try {
        ws.close(1000, reason ? reason.slice(0, 120) : 'abort');
      } catch {
        finishEnd();
      }
    },
    ready,
    get state() {
      return state;
    },
    onError(cb): void {
      onErrorCb = cb;
    },
    onAck(cb): void {
      onAckCb = cb;
    },
    onClose(cb): void {
      onCloseCb = cb;
    },
    sessionId(): number {
      return sessionId;
    },
    lastRejected(): FollowTargetPushAck | null {
      return lastRejectedAck;
    },
    lastEnded(): FollowTargetEndAck | null {
      return lastEndedAck;
    },
  };
}
