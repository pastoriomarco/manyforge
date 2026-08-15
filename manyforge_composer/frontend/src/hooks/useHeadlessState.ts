import { useCallback, useEffect, useRef, useState } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import { parseComposerHeadlessState } from '../api/parsers';
import type { ComposerHeadlessState } from '../types/headless';

const RECONNECT_DELAY_MS = 1000;

export interface HeadlessStateResult {
  snapshot: ComposerHeadlessState | null;
  error: string | null;
  reconnect: () => void;
}

export interface HeadlessStateAdmission {
  accepted: boolean;
  reason: string | null;
}

function exactSnapshotEqual(left: unknown, right: unknown): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

function monotonicOwnerSection(
  label: string,
  currentOwnerId: string,
  currentOwnerEpoch: string,
  currentGeneration: number,
  candidateOwnerId: string,
  candidateOwnerEpoch: string,
  candidateGeneration: number,
  currentValue: unknown,
  candidateValue: unknown,
): string | null {
  if (candidateOwnerEpoch !== currentOwnerEpoch) return null;
  if (candidateOwnerId !== currentOwnerId) {
    return `${label} reused an owner epoch with a different owner ID.`;
  }
  if (candidateGeneration < currentGeneration) {
    return `${label} generation regressed within one owner epoch.`;
  }
  if (
    candidateGeneration === currentGeneration
    && !exactSnapshotEqual(candidateValue, currentValue)
  ) {
    return `${label} conflicted at an already observed owner generation.`;
  }
  return null;
}

export function admitHeadlessState(
  current: ComposerHeadlessState | null,
  candidate: ComposerHeadlessState,
): HeadlessStateAdmission {
  if (current === null) return { accepted: true, reason: null };
  const runtimeReason = monotonicOwnerSection(
    'Headless runtime projection',
    current.runtime.projectionOwnerId,
    current.runtime.projectionOwnerEpoch,
    current.runtime.projectionGeneration,
    candidate.runtime.projectionOwnerId,
    candidate.runtime.projectionOwnerEpoch,
    candidate.runtime.projectionGeneration,
    current.runtime,
    candidate.runtime,
  );
  if (runtimeReason !== null) {
    return { accepted: false, reason: runtimeReason };
  }
  const alarmReason = monotonicOwnerSection(
    'Headless alarm projection',
    current.alarms.ownerId,
    current.alarms.ownerEpoch,
    current.alarms.generation,
    candidate.alarms.ownerId,
    candidate.alarms.ownerEpoch,
    candidate.alarms.generation,
    current.alarms,
    candidate.alarms,
  );
  return alarmReason === null
    ? { accepted: true, reason: null }
    : { accepted: false, reason: alarmReason };
}

function websocketUrl(): string {
  const resolved = new URL(
    resolveStructuralAssetUrl('/ws/state'),
    window.location.href,
  );
  resolved.protocol = resolved.protocol === 'https:' ? 'wss:' : 'ws:';
  return resolved.toString();
}

export function useHeadlessState(enabled: boolean): HeadlessStateResult {
  const [snapshot, setSnapshot] = useState<ComposerHeadlessState | null>(null);
  const snapshotRef = useRef<ComposerHeadlessState | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [reconnectToken, setReconnectToken] = useState(0);

  useEffect(() => {
    if (!enabled) {
      snapshotRef.current = null;
      setSnapshot(null);
      setError(null);
      return undefined;
    }
    let active = true;
    let socket: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

    const connect = () => {
      if (!active) return;
      socket = new WebSocket(websocketUrl());
      socket.onmessage = (message) => {
        try {
          const raw: unknown = JSON.parse(String(message.data));
          if (raw !== null && typeof raw === 'object' && !Array.isArray(raw)) {
            const errorValue = (raw as Record<string, unknown>).error;
            if (
              errorValue !== null
              && typeof errorValue === 'object'
              && !Array.isArray(errorValue)
            ) {
              const record = errorValue as Record<string, unknown>;
              const detail = typeof record.message === 'string'
                ? record.message
                : 'Headless projection unavailable.';
              setError(detail);
              return;
            }
          }
          const candidate = parseComposerHeadlessState(raw);
          const admission = admitHeadlessState(snapshotRef.current, candidate);
          if (!admission.accepted) {
            setError(admission.reason);
            socket?.close(1002, 'non-monotonic headless projection');
            return;
          }
          snapshotRef.current = candidate;
          setSnapshot(candidate);
          setError(null);
        } catch (reason) {
          setError(
            reason instanceof Error
              ? reason.message
              : 'Invalid headless WebSocket projection.',
          );
          socket?.close(1002, 'invalid headless projection');
        }
      };
      socket.onerror = () => {
        setError('Headless WebSocket transport failed.');
      };
      socket.onclose = () => {
        if (active) {
          setError((current) => current ?? 'Headless projection disconnected.');
          reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
        }
      };
    };

    connect();
    return () => {
      active = false;
      if (reconnectTimer !== null) clearTimeout(reconnectTimer);
      socket?.close(1000, 'Composer client unmounted');
    };
  }, [enabled, reconnectToken]);

  const reconnect = useCallback(() => {
    setReconnectToken((current) => current + 1);
  }, []);
  return { snapshot, error, reconnect };
}
