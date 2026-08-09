import { useCallback, useEffect, useState } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import { parseComposerHeadlessState } from '../api/parsers';
import type { ComposerHeadlessState } from '../types/headless';

const RECONNECT_DELAY_MS = 1000;

export interface HeadlessStateResult {
  snapshot: ComposerHeadlessState | null;
  error: string | null;
  reconnect: () => void;
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
  const [error, setError] = useState<string | null>(null);
  const [reconnectToken, setReconnectToken] = useState(0);

  useEffect(() => {
    if (!enabled) {
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
          setSnapshot(parseComposerHeadlessState(raw));
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
