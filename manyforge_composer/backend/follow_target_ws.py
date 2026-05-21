"""Streaming target WebSocket handler (ADR-0016).

Wire shape:

    First message (kind=cartesian|joint) opens a kernel streaming session
    directly with `kernel.begin_streaming_session(...)`. Subsequent messages
    push targets into the kernel directly via
    `kernel.push_streaming_target`. Socket close (or explicit
    `{"op":"end"}` message) calls
    `kernel.end_streaming_session(session_id, soft_stop_strategy)`.

The handler is **single-session per socket**: the first `begin` opens one
kernel streaming session directly. Per-frame `push_streaming_target` calls are
acknowledged before the browser sends another frame, so the wire is
latest-wins and cannot build a stale unreachable-target backlog. Multiple
concurrent follow-target sockets are rejected by the kernel's v0.1
"one streaming session per kernel" rule and surfaced as structured acks/errors.

Message schema:

    Frame:
      {
        "op": "frame",            # default if omitted
        "kind": "cartesian"|"joint",
        "position_m": [x,y,z],         # for kind=cartesian
        "orientation_xyzw": [x,y,z,w], # for kind=cartesian
        "joint_names": ["j1", ...],    # for kind=joint (optional)
        "joint_positions_rad": [v, ...],
        "source_timestamp_ns": int,   # optional, for trace correlation
        "source_sequence": int        # optional, monotonic per client
      }

    Begin (optional first frame, otherwise the first frame opens a session
    with defaults derived from the deployment/program):
      {
        "op": "begin",
        "group_id": "...",
        "input_mode": "cartesian"|"joint",
        "soft_stop_strategy": "spring_back"|"velocity_scaling",
        ... // other StreamingSessionConfig fields, all optional
      }

    End (optional; otherwise socket close drives end):
      {"op": "end", "soft_stop_strategy": "spring_back"|"velocity_scaling"}
"""
from __future__ import annotations

import asyncio
import json
import logging
import math
import time
from typing import Any, Optional

from fastapi import WebSocket

from manyforge_behavior.kernel_client import StreamingSessionConfig, StreamingTarget

from .state import ComposerState

logger = logging.getLogger(__name__)

_WS_MAX_MESSAGE_LEN = 64 * 1024
_BEGIN_WAIT_TIMEOUT_S = 2.0


async def handle_follow_target_ws(ws: WebSocket, state: ComposerState) -> None:
    await ws.accept()

    if state.cycle_manager is None or state.kernel is None:
        await _close_error(ws, "no_program_loaded",
                           "Load a program before opening a streaming session")
        return

    session_id: int = 0
    soft_stop_strategy: str = "spring_back"

    try:
        while True:
            raw = await ws.receive_text()
            if len(raw) > _WS_MAX_MESSAGE_LEN:
                await _close_error(ws, "message_too_large",
                                   "single message exceeds size limit")
                return
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError as exc:
                await _send_error(ws, "bad_json", str(exc))
                continue

            op = msg.get("op", "frame")

            if op == "begin":
                if session_id != 0:
                    await _send_error(ws, "already_open",
                                      "session already open on this socket")
                    continue
                session_id, err_code, err = await _open_session(
                    state, msg, ws
                )
                if err is not None:
                    await _close_error(ws, err_code or "begin_failed", err)
                    return
                soft_stop_strategy = str(
                    msg.get("soft_stop_strategy", soft_stop_strategy)
                )
                await ws.send_text(json.dumps({
                    "op": "began",
                    "session_id": session_id,
                }))
                continue

            if op == "frame":
                if session_id == 0:
                    # Implicit begin from the first frame with defaults
                    # derived from message kind.
                    implicit_cfg: dict[str, Any] = {
                        "input_mode": str(msg.get("kind", "cartesian")),
                    }
                    session_id, err_code, err = await _open_session(
                        state, implicit_cfg, ws
                    )
                    if err is not None:
                        await _close_error(ws, err_code or "begin_failed", err)
                        return
                source_sequence = _safe_int(msg.get("source_sequence", 0))
                target = _frame_to_target(msg)
                if target is None:
                    await ws.send_text(json.dumps({
                        "op": "pushed",
                        "session_id": session_id,
                        "source_sequence": source_sequence,
                        "accepted": False,
                        "backend_sequence": 0,
                        "reason_code": "malformed_frame",
                        "reason": "frame missing required fields (kind, "
                                  "position_m/orientation_xyzw for cartesian, "
                                  "or joint_positions_rad for joint)",
                        "kind": str(msg.get("kind", "unknown")),
                    }))
                    continue
                start_ns = time.monotonic_ns()
                push = await asyncio.to_thread(
                    state.kernel.push_streaming_target,
                    session_id,
                    target,
                )
                elapsed_ms = (time.monotonic_ns() - start_ns) / 1_000_000.0
                reason_code = _streaming_reason_code(
                    getattr(push, "error_code", None),
                    push.error,
                )
                # Server-side log on rejection so `docker logs` shows the
                # reason. Per-frame rejections are recoverable (next
                # reachable pose succeeds), but the operator needs the
                # diagnostic available when they ask "why did it stop?".
                if not push.accepted:
                    logger.warning(
                        "streaming push rejected: session=%s seq=%s "
                        "kind=%s reason_code=%s duration_ms=%.1f reason=%s",
                        session_id,
                        source_sequence,
                        target.kind,
                        reason_code,
                        elapsed_ms,
                        push.error or "unknown",
                    )
                # Per-frame structured ack so the client can correlate
                # source_sequence with accepted/rejected and surface
                # rejections in the UI (ADR-0016 reliability follow-up).
                await ws.send_text(json.dumps({
                    "op": "pushed",
                    "session_id": session_id,
                    "source_sequence": source_sequence,
                    "accepted": bool(push.accepted),
                    "backend_sequence": int(push.backend_sequence),
                    "reason_code": reason_code if not push.accepted else "",
                    "reason": (push.error or "") if not push.accepted else "",
                    "kind": target.kind,
                }))
                continue

            if op == "end":
                requested_strategy = msg.get("soft_stop_strategy")
                if isinstance(requested_strategy, str):
                    soft_stop_strategy = requested_strategy
                break

            await _send_error(ws, "unknown_op", f"unknown op: {op}")

    except Exception as exc:  # WebSocketDisconnect or transport error
        logger.info("follow-target WS closed: %s", exc)
    finally:
        end_outcome: Optional[bool] = None
        end_reason: Optional[str] = None
        if session_id != 0:
            try:
                # end_streaming_session is the BIG one: it waits for
                # the C++ backend's streaming worker to exit (up to
                # ~2 s) and runs the soft-stop trajectory. Synchronous
                # pybind11 call → MUST go on a thread or it blocks the
                # FastAPI event loop and freezes every other HTTP
                # request (Run, Reset, scene polls) for the duration.
                end_result = await asyncio.to_thread(
                    state.kernel.end_streaming_session,
                    session_id, soft_stop_strategy,
                )
                end_outcome = bool(getattr(end_result, "accepted", False))
                end_reason = getattr(end_result, "error", None)
            except Exception as exc:  # pragma: no cover — defensive
                end_outcome = False
                end_reason = str(exc)
                logger.warning(
                    "end_streaming_session failed for session=%s: %s",
                    session_id, exc,
                )
        # Best-effort `op: ended` ack so well-behaved clients can
        # distinguish a clean server-driven close from an abnormal one.
        # Swallow failures — the client may have already torn down the
        # socket, especially on the abort path.
        try:
            await ws.send_text(json.dumps({
                "op": "ended",
                "session_id": session_id,
                "soft_stop_strategy": soft_stop_strategy,
                "accepted": end_outcome,
                "reason": end_reason or "",
            }))
        except Exception:
            pass
        try:
            await ws.close(code=1000, reason="ended")
        except Exception:
            pass


async def _open_session(
    state: ComposerState,
    msg: dict[str, Any],
    ws: WebSocket,
) -> tuple[int, Optional[str], Optional[str]]:
    """Open a streaming session by calling kernel.begin_streaming_session
    directly.

    Returns (session_id, error_code, error_message). On success error_message
    is None. error_code is a stable UI-facing code whenever the kernel/backend
    exposes enough structure to classify the begin failure.
    Streaming sessions are managed by the kernel directly, not wrapped in a
    cycle_manager intervention (the kernel owns kStreaming per ADR-0016).
    """
    principal_id = (
        msg.get("principal_id")
        or getattr(ws.state, "principal_id", None)
        or "composer_operator"
    )
    group_id = (
        msg.get("group_id")
        or _default_group_id(state)
        or ""
    )

    # Resolve the Cartesian speed limit from the deployment (ADR-0015's
    # global_max_tcp_linear_speed_mps) and clamp against any per-session
    # override supplied by the caller. The per-session value MUST NOT be
    # allowed to exceed the deployment global — same precedence rule as
    # planned moves in motion.py: min(global, per-call) if both set.
    deployment_cap_mps = _deployment_tcp_speed_cap(state)
    requested_cap_mps = _finite_float_or_default(
        msg.get("max_tcp_linear_speed_mps", 0.0),
        0.0,
    )
    if deployment_cap_mps > 0.0 and requested_cap_mps > 0.0:
        effective_cap_mps = min(deployment_cap_mps, requested_cap_mps)
    elif deployment_cap_mps > 0.0:
        effective_cap_mps = deployment_cap_mps
    elif requested_cap_mps > 0.0:
        effective_cap_mps = requested_cap_mps
    else:
        effective_cap_mps = 0.0  # "unlimited" — only when the deployment
                                  # explicitly didn't set a cap

    # Resolve the session's joint list server-side so we can build the
    # per-joint cap vectors regardless of whether the client supplied
    # `joint_names`. Falling back to the kernel's drivable joint list
    # matches what the kernel's `begin_streaming_session` fallback does
    # for `joint_names` itself — without this, an empty client list
    # left `per_joint_v` / `per_joint_a` empty too, even after the
    # kernel populated the session's joint_names from the snapshot.
    session_joint_names = list(msg.get("joint_names", []) or [])
    if not session_joint_names and state.kernel is not None:
        try:
            session_joint_names = list(state.kernel.get_joint_names() or [])
        except Exception:
            session_joint_names = []

    # Per-joint streaming caps (spec/430 §3.2 streaming-joint-limits +
    # ADR-0016). Build session-joint-order parallel vectors from the
    # deployment's robot_config.streaming_joint_limits map; zero
    # entries signal "use backend engine default" (0.5 rad/s velocity,
    # 2.0 rad/s² acceleration). Only populated when the session
    # already has a non-empty joint list — otherwise the kernel
    # fallback in begin_streaming_session populates joint_names from
    # the live snapshot AND the per-joint cap vectors stay empty,
    # which the backend treats as all-defaults.
    deployment_limits = _deployment_streaming_joint_limits(state)
    per_joint_v = [
        float(deployment_limits.get(name, (None, None))[0] or 0.0)
        for name in session_joint_names
    ]
    per_joint_a = [
        float(deployment_limits.get(name, (None, None))[1] or 0.0)
        for name in session_joint_names
    ]

    config = StreamingSessionConfig(
        group_id=group_id,
        joint_names=session_joint_names,
        input_mode=str(msg.get("input_mode", "cartesian")),
        cartesian_reference_frame=str(
            msg.get("cartesian_reference_frame", "base")
        ),
        tcp_frame=str(msg.get("tcp_frame", "") or ""),
        soft_stop_strategy=str(msg.get("soft_stop_strategy", "spring_back")),
        soft_stop_deceleration_time_s=_finite_float_or_default(
            msg.get("soft_stop_deceleration_time_s", 0.4), 0.4),
        anchor_negative_time_s=_finite_float_or_default(
            msg.get("anchor_negative_time_s", -0.010), -0.010),
        target_time_from_start_min_s=_finite_float_or_default(
            msg.get("target_time_from_start_min_s", 0.030), 0.030),
        max_dispatch_rate_hz=_finite_float_or_default(
            msg.get("max_dispatch_rate_hz", 30.0), 30.0),
        target_max_age_s=_finite_float_or_default(
            msg.get("target_max_age_s", 0.200), 0.200),
        velocity_scale=_finite_float_or_default(
            msg.get("velocity_scale", 1.0), 1.0),
        max_tcp_linear_speed_mps=effective_cap_mps,
        per_joint_velocity_caps_rad_s=per_joint_v,
        per_joint_acceleration_caps_rad_s2=per_joint_a,
        principal_id=str(principal_id),
        authority_source=str(msg.get("authority_source", "human")),
        trigger_reason=str(
            msg.get("trigger_reason", "composer_follow_target")
        ),
    )

    try:
        # begin_streaming_session is a synchronous pybind11 call into
        # C++; for the ros2_control backend it can block up to
        # `wait_for_action_server_timeout_s` (≈2 s). Run it on a thread
        # so the FastAPI event loop stays responsive (Run/Reset/cycle
        # status polls don't queue behind it).
        result = await asyncio.to_thread(
            state.kernel.begin_streaming_session, config
        )
    except Exception as exc:  # pragma: no cover — defensive
        return 0, "begin_failed", f"kernel.begin_streaming_session raised: {exc}"

    if not result.accepted:
        return (
            0,
            _streaming_begin_reason_code(
                getattr(result, "error_code", None),
                result.error,
            ),
            result.error or "kernel rejected begin_streaming_session",
        )

    return int(result.session_id), None, None


def _frame_to_target(msg: dict[str, Any]) -> Optional[StreamingTarget]:
    kind = str(msg.get("kind", "cartesian"))
    target = StreamingTarget(
        kind=kind,
        source_timestamp_ns=int(msg.get("source_timestamp_ns", 0)),
        source_sequence=int(msg.get("source_sequence", 0)),
    )
    if kind == "cartesian":
        pos = msg.get("position_m")
        quat = msg.get("orientation_xyzw")
        if not (isinstance(pos, list) and len(pos) == 3):
            return None
        if not (isinstance(quat, list) and len(quat) == 4):
            return None
        try:
            target.position_m = [_finite_float(v) for v in pos]
            target.orientation_xyzw = [_finite_float(v) for v in quat]
        except (TypeError, ValueError):
            return None
    elif kind == "joint":
        names = msg.get("joint_names") or []
        values = msg.get("joint_positions_rad")
        if not (isinstance(values, list) and values):
            return None
        target.joint_names = [str(n) for n in names]
        try:
            target.joint_positions_rad = [_finite_float(v) for v in values]
        except (TypeError, ValueError):
            return None
    else:
        return None
    return target


def _finite_float(value: Any) -> float:
    out = float(value)
    if not math.isfinite(out):
        raise ValueError("non-finite numeric value")
    return out


def _finite_float_or_default(value: Any, default: float) -> float:
    try:
        return _finite_float(value)
    except (TypeError, ValueError):
        return default


def _safe_int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _deployment_streaming_joint_limits(
    state: ComposerState,
) -> dict[str, tuple[Optional[float], Optional[float]]]:
    """Return per-joint streaming caps from the loaded deployment.

    Maps joint name -> (max_velocity_rad_s, max_acceleration_rad_s2);
    either value may be None when the deployment didn't specify it.
    Empty dict when no deployment is loaded or no limits are set
    (engine defaults apply at the backend).
    """
    deployment = getattr(state, "deployment", None)
    if deployment is None:
        return {}
    robot_config = getattr(deployment, "robot_config", None)
    if robot_config is None:
        return {}
    limits_map = getattr(robot_config, "streaming_joint_limits", None) or {}
    out: dict[str, tuple[Optional[float], Optional[float]]] = {}
    for name, limit in limits_map.items():
        out[str(name)] = (
            getattr(limit, "max_velocity_rad_s", None),
            getattr(limit, "max_acceleration_rad_s2", None),
        )
    return out


def _deployment_tcp_speed_cap(state: ComposerState) -> float:
    """Return the deployment-level global TCP linear speed cap (m/s),
    or 0.0 when no deployment is loaded or no cap is configured.

    ADR-0015: the cap lives at
    `deployment.robot_config.global_max_tcp_linear_speed_mps` and is
    >0 by spec when set. We treat <=0 as "no cap" here so callers can
    distinguish "deployment hasn't set it" from "deployment set it to
    something restrictive".
    """
    deployment = getattr(state, "deployment", None)
    if deployment is None:
        return 0.0
    robot_config = getattr(deployment, "robot_config", None)
    if robot_config is None:
        return 0.0
    cap = getattr(robot_config, "global_max_tcp_linear_speed_mps", 0.0)
    try:
        cap_f = float(cap)
    except (TypeError, ValueError):
        return 0.0
    return cap_f if cap_f > 0.0 else 0.0


def _streaming_reason_code(error_code: Optional[str], message: Optional[str]) -> str:
    """Map kernel/backend errors to stable UI-facing streaming reason codes.

    Prefer the C++ ErrorDTO code when available. The message fallback exists
    only for mock/default clients that predate the explicit code field; the
    frontend never depends on these English strings.
    """
    code = (error_code or "").strip().lower()
    if code == "ik_failed":
        return "ik_failed"
    if code == "unknown_joint_name":
        return "unknown_joint_name"
    if code == "invalid_state_transition":
        return "invalid_state"
    if code == "invalid_request":
        text = (message or "").lower()
        # Kernel partial-joint normalization (spec/200 §2): the validation
        # branches use kInvalidRequest with distinct prefixes that map to
        # stable reason codes for the operator UI. Long-term a structured
        # details field on StreamingPushResult would replace these text
        # matches — tracked as the follow-up to drop English matching.
        if (
            "joint_names.size()" in text
            or "duplicate joint name" in text
            or "carries no joint_positions_rad" in text
        ):
            return "malformed_frame"
        if "current robot state does not cover" in text:
            return "stale_state"
        if "session_id" in text or "active streaming session" in text:
            return "session_lost"
        if "tcp_frame" in text or "tcp frame" in text:
            return "missing_tcp_frame"
        return "invalid_request"
    if code:
        return code

    text = (message or "").lower()
    if "ik failed" in text or "ik_failed" in text:
        return "ik_failed"
    if "tcp_frame" in text or "tcp frame" in text or "did not configure" in text:
        return "missing_tcp_frame"
    if "session_id" in text or "session" in text and "match" in text:
        return "session_lost"
    if "invalid_state" in text or "state transition" in text:
        return "invalid_state"
    if "action server" in text or "controller" in text:
        return "no_controller"
    if "stale" in text or "joint state" in text:
        return "stale_state"
    if "frame missing" in text:
        return "malformed_frame"
    return "rejected"


def _streaming_begin_reason_code(error_code: Optional[str], message: Optional[str]) -> str:
    """Map begin_streaming_session failures to stable WS error codes."""
    code = (error_code or "").strip().lower()
    text = (message or "").lower()
    if "already active" in text or "session_already_active" in text:
        return "session_already_active"
    if code == "invalid_state_transition":
        return "invalid_state"
    if code == "unsupported_capability":
        return "unsupported_capability"
    if code == "invalid_request":
        return "invalid_request"
    if code:
        return code
    if "valid only from idle" in text or "wrong state" in text:
        return "invalid_state"
    return "begin_failed"


def _default_group_id(state: ComposerState) -> Optional[str]:
    if state.deployment is None:
        return None
    # Pull the first robot group from the deployment if available.
    try:
        robots = state.deployment.robots
        if not robots:
            return None
        first = next(iter(robots.values()))
        groups = getattr(first, "groups", None) or {}
        if groups:
            return next(iter(groups.keys()))
    except Exception:
        return None
    return None


async def _send_error(ws: WebSocket, code: str, message: str) -> None:
    try:
        await ws.send_text(json.dumps({
            "op": "error",
            "code": code,
            "message": message,
        }))
    except Exception:
        pass


async def _close_error(ws: WebSocket, code: str, message: str) -> None:
    await _send_error(ws, code, message)
    try:
        await ws.close(code=1011, reason=code)
    except Exception:
        pass
