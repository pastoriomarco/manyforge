import { resolveStructuralAssetUrl } from './client';
import { ownsConnectionGeneration } from '../state/latestFrameSlot';

const FIXED_HEADER_BYTES = 48;
const MAXIMUM_PACKET_BYTES = 3_002_048;
const MAXIMUM_POINTS = 200_000;
const FLAG_COLORS = 1 << 0;
const FLAG_CLEAR = 1 << 1;
const RECONNECT_DELAY_MS = 1_000;
const identityPattern = /^[A-Za-z0-9][A-Za-z0-9._:/-]*$/;

export interface PointCloudFrame {
  ownerId: string;
  ownerEpoch: string;
  cellId: string;
  frameId: string;
  streamGeneration: bigint;
  sequence: bigint;
  timestampNs: bigint;
  pointCount: number;
  positions: Float32Array<ArrayBuffer>;
  colors: Uint8Array<ArrayBuffer> | null;
}

export interface PointCloudObservationFrame {
  streamIdentity: string;
  sequence: bigint;
  frame: PointCloudFrame | null;
}

function boundedIdentity(value: string, maximumBytes: number, label: string): string {
  if (
    !identityPattern.test(value)
    || new TextEncoder().encode(value).byteLength > maximumBytes
  ) {
    throw new Error(`${label} is not a bounded observation identity`);
  }
  return value;
}

function decodeIdentity(
  bytes: Uint8Array<ArrayBuffer>,
  offset: number,
  length: number,
  maximum: number,
  label: string,
): string {
  if (length < 1 || length > maximum) {
    throw new Error(`${label} byte length is invalid`);
  }
  const value = new TextDecoder('utf-8', { fatal: true }).decode(
    bytes.subarray(offset, offset + length),
  );
  return boundedIdentity(value, maximum, label);
}

export function decodePointCloudObservation(
  buffer: ArrayBuffer,
  expectedCellId?: string,
): PointCloudObservationFrame {
  if (buffer.byteLength < FIXED_HEADER_BYTES) {
    throw new Error('point-cloud observation is shorter than its header');
  }
  if (buffer.byteLength > MAXIMUM_PACKET_BYTES) {
    throw new Error('point-cloud observation exceeds the packet byte bound');
  }
  const bytes = new Uint8Array(buffer);
  if (
    bytes[0] !== 0x4d
    || bytes[1] !== 0x46
    || bytes[2] !== 0x4f
    || bytes[3] !== 0x42
    || bytes[4] !== 1
  ) {
    throw new Error('unsupported point-cloud observation protocol');
  }
  const flags = bytes[5] ?? 0;
  if ((flags & ~(FLAG_COLORS | FLAG_CLEAR)) !== 0) {
    throw new Error('unsupported point-cloud observation flags');
  }
  const view = new DataView(buffer);
  const headerBytes = view.getUint16(6, true);
  const lengths = [
    view.getUint16(8, true),
    view.getUint16(10, true),
    view.getUint16(12, true),
    view.getUint16(14, true),
  ];
  const identityEnd = FIXED_HEADER_BYTES
    + lengths.reduce((total, length) => total + length, 0);
  const expectedHeaderBytes = identityEnd + ((-identityEnd) & 3);
  if (
    headerBytes !== expectedHeaderBytes
    || headerBytes > buffer.byteLength
    || headerBytes % 4 !== 0
  ) {
    throw new Error('point-cloud observation header size is inconsistent');
  }
  for (let index = identityEnd; index < headerBytes; index += 1) {
    if (bytes[index] !== 0) {
      throw new Error('point-cloud observation header padding is not zero');
    }
  }
  let cursor = FIXED_HEADER_BYTES;
  const ownerId = decodeIdentity(bytes, cursor, lengths[0] ?? 0, 128, 'ownerId');
  cursor += lengths[0] ?? 0;
  const ownerEpoch = decodeIdentity(bytes, cursor, lengths[1] ?? 0, 256, 'ownerEpoch');
  cursor += lengths[1] ?? 0;
  const cellId = decodeIdentity(bytes, cursor, lengths[2] ?? 0, 256, 'cellId');
  cursor += lengths[2] ?? 0;
  const frameId = decodeIdentity(bytes, cursor, lengths[3] ?? 0, 261, 'frameId');
  if (frameId !== `cell:${cellId}`) {
    throw new Error('point-cloud observation is not in its canonical cell frame');
  }
  if (expectedCellId !== undefined && cellId !== expectedCellId) {
    throw new Error('point-cloud observation belongs to another cell');
  }
  const streamGeneration = view.getBigUint64(16, true);
  const sequence = view.getBigUint64(24, true);
  const timestampNs = view.getBigUint64(32, true);
  const pointCount = view.getUint32(40, true);
  const payloadBytes = view.getUint32(44, true);
  if (pointCount > MAXIMUM_POINTS) {
    throw new Error('point-cloud observation exceeds the point bound');
  }
  const hasColors = (flags & FLAG_COLORS) !== 0;
  const clear = (flags & FLAG_CLEAR) !== 0;
  const expectedPayloadBytes = pointCount * (hasColors ? 15 : 12);
  if (
    (clear && (pointCount !== 0 || payloadBytes !== 0 || hasColors))
    || (!clear && (pointCount === 0 || payloadBytes !== expectedPayloadBytes))
    || buffer.byteLength !== headerBytes + payloadBytes
  ) {
    throw new Error('point-cloud observation payload shape is inconsistent');
  }
  const streamIdentity = `${ownerId}\u0000${ownerEpoch}\u0000${streamGeneration}`;
  if (clear) return { streamIdentity, sequence, frame: null };
  const positions = new Float32Array(buffer, headerBytes, pointCount * 3);
  for (const coordinate of positions) {
    if (!Number.isFinite(coordinate)) {
      throw new Error('point-cloud observation contains a non-finite coordinate');
    }
  }
  const colors = hasColors
    ? new Uint8Array(buffer, headerBytes + pointCount * 12, pointCount * 3)
    : null;
  return {
    streamIdentity,
    sequence,
    frame: {
      ownerId,
      ownerEpoch,
      cellId,
      frameId,
      streamGeneration,
      sequence,
      timestampNs,
      pointCount,
      positions,
      colors,
    },
  };
}

function websocketUrl(): string {
  const resolved = new URL(
    resolveStructuralAssetUrl('/ws/scene/pointcloud'),
    window.location.href,
  );
  resolved.protocol = resolved.protocol === 'https:' ? 'wss:' : 'ws:';
  return resolved.toString();
}

export function subscribePointCloudObserver(
  expectedCellId: string,
  handler: (observation: PointCloudObservationFrame) => void,
  onError: (error: Error) => void,
): () => void {
  boundedIdentity(expectedCellId, 256, 'expectedCellId');
  let active = true;
  let socket: WebSocket | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let connectionGeneration = 0;
  const connect = () => {
    if (!active) return;
    connectionGeneration += 1;
    const candidateGeneration = connectionGeneration;
    let candidate: WebSocket;
    try {
      candidate = new WebSocket(websocketUrl());
    } catch (error) {
      onError(error instanceof Error ? error : new Error('Point-cloud connection failed.'));
      reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
      return;
    }
    socket = candidate;
    candidate.binaryType = 'arraybuffer';
    candidate.onmessage = (message) => {
      if (!active || !ownsConnectionGeneration(
        candidate,
        socket,
        candidateGeneration,
        connectionGeneration,
      )) return;
      if (!(message.data instanceof ArrayBuffer)) {
        onError(new Error('Point-cloud observer received a non-binary frame.'));
        candidate.close(1002, 'binary observation required');
        return;
      }
      try {
        handler(decodePointCloudObservation(message.data, expectedCellId));
      } catch (error) {
        onError(error instanceof Error ? error : new Error('Point-cloud frame is invalid.'));
        candidate.close(1002, 'invalid point-cloud observation');
      }
    };
    candidate.onerror = () => onError(new Error('Point-cloud observer transport failed.'));
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
    socket?.close(1000, 'point-cloud observer unmounted');
  };
}
