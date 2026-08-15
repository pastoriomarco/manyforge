import { resolveStructuralAssetUrl } from './client';
import type { SnapshotRequestView } from '../components/SnapshotResponder';

const RECONNECT_DELAY_MS = 1_000;
const MAXIMUM_SNAPSHOT_BYTES = 4 * 1024 * 1024;

function objectAt(value: unknown, label: string): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

export function parseSnapshotObserverRequest(value: unknown): SnapshotRequestView {
  const record = objectAt(value, 'snapshot request');
  const fields = Object.keys(record).sort();
  const expected = [
    'height',
    'mime',
    'requestId',
    'requiredFrame',
    'timeoutMs',
    'type',
    'width',
  ];
  if (JSON.stringify(fields) !== JSON.stringify(expected)) {
    throw new Error('snapshot request fields are invalid');
  }
  if (record.type !== 'snapshot_request') {
    throw new Error('snapshot request type is invalid');
  }
  const frame = objectAt(record.requiredFrame, 'snapshot required frame');
  if (JSON.stringify(Object.keys(frame).sort()) !== JSON.stringify(['generation', 'ownerEpoch'])) {
    throw new Error('snapshot required frame fields are invalid');
  }
  if (
    typeof record.requestId !== 'string'
    || !/^[0-9a-f]{32}$/.test(record.requestId)
    || typeof record.width !== 'number'
    || typeof record.height !== 'number'
    || (record.mime !== 'image/png' && record.mime !== 'image/jpeg')
    || typeof record.timeoutMs !== 'number'
    || typeof frame.ownerEpoch !== 'string'
    || typeof frame.generation !== 'number'
  ) {
    throw new Error('snapshot request value types are invalid');
  }
  const request: SnapshotRequestView = {
    requestId: record.requestId,
    width: record.width,
    height: record.height,
    mime: record.mime,
    timeoutMs: record.timeoutMs,
    requiredFrame: {
      ownerEpoch: frame.ownerEpoch,
      generation: frame.generation,
    },
  };
  if (
    !Number.isSafeInteger(request.width)
    || request.width < 64
    || request.width > 1920
    || !Number.isSafeInteger(request.height)
    || request.height < 64
    || request.height > 1080
    || !Number.isSafeInteger(request.timeoutMs)
    || request.timeoutMs < 1
    || request.timeoutMs > 2_000
    || !/^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$/.test(
      request.requiredFrame.ownerEpoch,
    )
    || !Number.isSafeInteger(request.requiredFrame.generation)
    || request.requiredFrame.generation < 0
  ) {
    throw new Error('snapshot request exceeds the observer bounds');
  }
  return request;
}

function websocketUrl(): string {
  const resolved = new URL(
    resolveStructuralAssetUrl('/ws/scene/snapshot'),
    window.location.href,
  );
  resolved.protocol = resolved.protocol === 'https:' ? 'wss:' : 'ws:';
  return resolved.toString();
}

export function subscribeSnapshotObserver(
  handler: (request: SnapshotRequestView) => void,
): () => void {
  let active = true;
  let socket: WebSocket | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  const connect = () => {
    if (!active) return;
    let candidate: WebSocket;
    try {
      candidate = new WebSocket(websocketUrl());
    } catch {
      reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
      return;
    }
    socket = candidate;
    candidate.onmessage = (message) => {
      try {
        handler(parseSnapshotObserverRequest(JSON.parse(String(message.data))));
      } catch (error) {
        console.warn('snapshot observer request rejected', error);
        candidate.close(1002, 'invalid snapshot request');
      }
    };
    candidate.onclose = () => {
      if (active && socket === candidate) {
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
      }
    };
  };
  connect();
  return () => {
    active = false;
    if (reconnectTimer !== null) clearTimeout(reconnectTimer);
    socket?.close(1000, 'snapshot responder unmounted');
  };
}

export async function uploadSnapshotObserverResult(
  requestId: string,
  result: Blob | null,
  observedFrame: SnapshotRequestView['requiredFrame'] | null,
): Promise<void> {
  if (!/^[0-9a-f]{32}$/.test(requestId)) {
    throw new Error('snapshot request ID is invalid');
  }
  if (result !== null) {
    if (observedFrame === null) {
      throw new Error('snapshot result requires its observed frame identity');
    }
    if (
      result.size < 1
      || result.size > MAXIMUM_SNAPSHOT_BYTES
      || (result.type !== 'image/png' && result.type !== 'image/jpeg')
      || !/^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$/.test(observedFrame.ownerEpoch)
      || !Number.isSafeInteger(observedFrame.generation)
      || observedFrame.generation < 0
    ) {
      throw new Error('snapshot result exceeds the upload contract');
    }
  }
  const response = await fetch(
    resolveStructuralAssetUrl(
      `/api/scene/snapshot/upload/${encodeURIComponent(requestId)}`,
    ),
    {
      method: 'POST',
      headers: result === null || observedFrame === null ? undefined : {
        'Content-Type': result.type,
        'X-ManyForge-Viewer-Owner-Epoch': observedFrame.ownerEpoch,
        'X-ManyForge-Viewer-Generation': String(observedFrame.generation),
      },
      body: result ?? new Blob(),
    },
  );
  if (!response.ok) {
    throw new Error(`snapshot upload returned HTTP ${response.status}`);
  }
}
