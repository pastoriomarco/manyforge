import { useEffect } from 'react';

const MAGIC = [0x4d, 0x46, 0x50, 0x43] as const; // MFPC
const VERSION = 1;
const HEADER_BYTES = 40;
const FLAG_COLORS = 1 << 0;
const FLAG_CLEAR = 1 << 1;
const MAX_POINTS = 200_000;

export interface PointCloudFrame {
  streamEpoch: bigint;
  sequence: bigint;
  timestampNs: bigint;
  pointCount: number;
  positions: Float32Array;
  colors: Uint8Array | null;
}

export type PointCloudApplier = (frame: PointCloudFrame | null) => void;

let latestFrame: PointCloudFrame | null = null;
let latestToken = 0;
let lastAppliedToken = -1;
let lastStreamEpoch: bigint | null = null;
let lastSequence = -1n;
const wakeSubscribers = new Set<() => void>();
const appliers = new Set<PointCloudApplier>();

function notifyWakeSubscribers(): void {
  for (const subscriber of [...wakeSubscribers]) subscriber();
}

export function parsePointCloudPacket(buffer: ArrayBuffer): PointCloudFrame | null {
  if (buffer.byteLength < HEADER_BYTES) throw new Error('Point-cloud packet is truncated');
  const bytes = new Uint8Array(buffer);
  if (MAGIC.some((value, index) => bytes[index] !== value)) {
    throw new Error('Point-cloud packet has invalid magic');
  }
  const view = new DataView(buffer);
  const version = view.getUint8(4);
  const flags = view.getUint8(5);
  const headerBytes = view.getUint16(6, true);
  if (version !== VERSION || headerBytes !== HEADER_BYTES) {
    throw new Error('Point-cloud protocol version is unsupported');
  }
  if ((flags & ~(FLAG_COLORS | FLAG_CLEAR)) !== 0) {
    throw new Error('Point-cloud packet has unsupported flags');
  }
  const streamEpoch = view.getBigUint64(8, true);
  const sequence = view.getBigUint64(16, true);
  const timestampNs = view.getBigUint64(24, true);
  const pointCount = view.getUint32(32, true);
  const payloadBytes = view.getUint32(36, true);
  if (pointCount > MAX_POINTS) throw new Error('Point-cloud point limit exceeded');
  const hasColors = (flags & FLAG_COLORS) !== 0;
  const clear = (flags & FLAG_CLEAR) !== 0;
  const expectedPayload = pointCount * 12 + (hasColors ? pointCount * 3 : 0);
  if (clear && (pointCount !== 0 || payloadBytes !== 0)) {
    throw new Error('Point-cloud clear packet carries data');
  }
  if ((!clear && payloadBytes !== expectedPayload) || buffer.byteLength !== HEADER_BYTES + payloadBytes) {
    throw new Error('Point-cloud payload shape is invalid');
  }
  if (clear) return null;
  const positions = new Float32Array(buffer, HEADER_BYTES, pointCount * 3);
  const colors = hasColors
    ? new Uint8Array(buffer, HEADER_BYTES + pointCount * 12, pointCount * 3)
    : null;
  return {
    streamEpoch,
    sequence,
    timestampNs,
    pointCount,
    positions,
    colors,
  };
}

function acceptSequence(streamEpoch: bigint, sequence: bigint): boolean {
  if (lastStreamEpoch !== streamEpoch) {
    lastStreamEpoch = streamEpoch;
    lastSequence = sequence;
    return true;
  }
  if (sequence <= lastSequence) return false;
  lastSequence = sequence;
  return true;
}

function publishPacket(buffer: ArrayBuffer): void {
  const view = new DataView(buffer);
  if (buffer.byteLength < HEADER_BYTES) throw new Error('Point-cloud packet is truncated');
  const streamEpoch = view.getBigUint64(8, true);
  const sequence = view.getBigUint64(16, true);
  const frame = parsePointCloudPacket(buffer);
  if (!acceptSequence(streamEpoch, sequence)) return;
  latestFrame = frame;
  latestToken += 1;
  notifyWakeSubscribers();
}

export function invalidatePointCloud(): void {
  latestFrame = null;
  latestToken += 1;
  lastStreamEpoch = null;
  lastSequence = -1n;
  notifyWakeSubscribers();
}

export function subscribePointCloudWake(subscriber: () => void): () => void {
  wakeSubscribers.add(subscriber);
  return () => wakeSubscribers.delete(subscriber);
}

export function registerPointCloudApplier(applier: PointCloudApplier): () => void {
  appliers.add(applier);
  applier(latestFrame);
  notifyWakeSubscribers();
  return () => appliers.delete(applier);
}

export function applyLatestPointCloudFrame(): boolean {
  if (lastAppliedToken === latestToken) return false;
  lastAppliedToken = latestToken;
  for (const applier of [...appliers]) {
    try {
      applier(latestFrame);
    } catch (error) {
      console.warn('point-cloud binding failed', error);
    }
  }
  return true;
}

let websocket: WebSocket | null = null;
let active = false;
let reconnectTimer: number | null = null;
let reconnectAttempts = 0;
let subscriberCount = 0;
let connectionGeneration = 0;

function ownsPointCloudConnection<T>(
  candidate: T,
  current: T | null,
  generation: number,
  currentGeneration: number,
): boolean {
  return current === candidate && generation === currentGeneration;
}

function websocketUrl(): string {
  if (typeof window === 'undefined') return '';
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const apiBase = import.meta.env.VITE_API_URL as string | undefined;
  if (apiBase) return apiBase.replace(/^https?:/, protocol) + '/ws/scene/pointcloud';
  return `${protocol}//${window.location.host}/ws/scene/pointcloud`;
}

function connect(): void {
  if (!active || typeof window === 'undefined') return;
  const generation = connectionGeneration;
  const socket = new WebSocket(websocketUrl());
  socket.binaryType = 'arraybuffer';
  websocket = socket;
  socket.onopen = () => {
    if (ownsPointCloudConnection(
      socket,
      websocket,
      generation,
      connectionGeneration,
    )) reconnectAttempts = 0;
  };
  socket.onmessage = (event) => {
    if (
      !ownsPointCloudConnection(socket, websocket, generation, connectionGeneration)
      || !(event.data instanceof ArrayBuffer)
    ) return;
    try {
      publishPacket(event.data);
    } catch {
      socket.close(1003, 'Invalid point-cloud frame');
    }
  };
  socket.onclose = () => {
    if (!ownsPointCloudConnection(
      socket,
      websocket,
      generation,
      connectionGeneration,
    )) return;
    websocket = null;
    invalidatePointCloud();
    if (!active) return;
    reconnectAttempts += 1;
    const delay = Math.min(30_000, 1_500 * 2 ** (reconnectAttempts - 1));
    reconnectTimer = window.setTimeout(connect, delay);
  };
  socket.onerror = () => {
    if (ownsPointCloudConnection(
      socket,
      websocket,
      generation,
      connectionGeneration,
    )) socket.close();
  };
}

/** Demand the independent point-cloud channel while this component is mounted. */
export function usePointCloudSubscription(): void {
  useEffect(() => {
    subscriberCount += 1;
    if (subscriberCount === 1) {
      active = true;
      connectionGeneration += 1;
      connect();
    }
    return () => {
      subscriberCount -= 1;
      if (subscriberCount !== 0) return;
      active = false;
      connectionGeneration += 1;
      if (reconnectTimer !== null) window.clearTimeout(reconnectTimer);
      reconnectTimer = null;
      const socket = websocket;
      websocket = null;
      socket?.close();
      invalidatePointCloud();
    };
  }, []);
}

export const __pointCloudStoreTest = {
  ownsPointCloudConnection,
  publishPacket,
  reset(): void {
    latestFrame = null;
    latestToken = 0;
    lastAppliedToken = -1;
    lastStreamEpoch = null;
    lastSequence = -1n;
    wakeSubscribers.clear();
    appliers.clear();
  },
};
