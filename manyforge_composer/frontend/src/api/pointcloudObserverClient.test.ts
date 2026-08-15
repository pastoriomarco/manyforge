import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  decodePointCloudObservation,
  subscribePointCloudObserver,
} from './pointcloudObserverClient';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

function packet(options: { clear?: boolean; colors?: boolean } = {}): ArrayBuffer {
  const encoder = new TextEncoder();
  const identities = [
    encoder.encode('manyforge.isaac.pointcloud'),
    encoder.encode('pointcloud.epoch.1'),
    encoder.encode('isaac.cell.1'),
    encoder.encode('cell:isaac.cell.1'),
  ];
  const identityBytes = identities.reduce((total, field) => total + field.length, 0);
  const headerBytes = 48 + identityBytes + ((-(48 + identityBytes)) & 3);
  const count = options.clear ? 0 : 2;
  const payloadBytes = count * (options.colors === false ? 12 : 15);
  const buffer = new ArrayBuffer(headerBytes + payloadBytes);
  const bytes = new Uint8Array(buffer);
  bytes.set([0x4d, 0x46, 0x4f, 0x42, 1, options.clear ? 2 : options.colors === false ? 0 : 1]);
  const view = new DataView(buffer);
  view.setUint16(6, headerBytes, true);
  identities.forEach((field, index) => view.setUint16(8 + index * 2, field.length, true));
  view.setBigUint64(16, 4n, true);
  view.setBigUint64(24, 7n, true);
  view.setBigUint64(32, 123_456n, true);
  view.setUint32(40, count, true);
  view.setUint32(44, payloadBytes, true);
  let cursor = 48;
  for (const field of identities) {
    bytes.set(field, cursor);
    cursor += field.length;
  }
  if (!options.clear) {
    new Float32Array(buffer, headerBytes, 6).set([1.25, -2.5, 3.75, 0, 0.5, 1]);
    if (options.colors !== false) {
      bytes.set([255, 128, 0, 0, 255, 64], headerBytes + 24);
    }
  }
  return buffer;
}

describe('MFOB browser decoder', () => {
  it('decodes exact cell-frame positions and colors without copying', () => {
    const decoded = decodePointCloudObservation(packet(), 'isaac.cell.1');

    expect(decoded.streamIdentity).toBe([
      'manyforge.isaac.pointcloud',
      'pointcloud.epoch.1',
      '4',
    ].join('\u0000'));
    expect(decoded.sequence).toBe(7n);
    expect(decoded.frame?.frameId).toBe('cell:isaac.cell.1');
    expect([...decoded.frame!.positions]).toEqual([1.25, -2.5, 3.75, 0, 0.5, 1]);
    expect([...decoded.frame!.colors!]).toEqual([255, 128, 0, 0, 255, 64]);
  });

  it('decodes clear and rejects old, cross-cell, and nonfinite packets', () => {
    expect(decodePointCloudObservation(packet({ clear: true })).frame).toBeNull();
    const old = packet();
    new Uint8Array(old).set([0x4d, 0x46, 0x50, 0x43]);
    expect(() => decodePointCloudObservation(old)).toThrow(/unsupported/);
    expect(() => decodePointCloudObservation(packet(), 'other.cell')).toThrow(/another cell/);
    const nonfinite = packet();
    const header = new DataView(nonfinite).getUint16(6, true);
    new DataView(nonfinite).setFloat32(header, Number.NaN, true);
    expect(() => decodePointCloudObservation(nonfinite)).toThrow(/non-finite/);
  });

  it('rejects a late frame from a replaced socket generation', () => {
    vi.useFakeTimers();
    class MockWebSocket {
      static instances: MockWebSocket[] = [];

      binaryType = '';
      onmessage: ((event: { data: unknown }) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      readonly url: string;

      constructor(url: string) {
        this.url = url;
        MockWebSocket.instances.push(this);
      }

      close(): void {}
    }
    vi.stubGlobal('window', { location: { href: 'http://localhost/' } });
    vi.stubGlobal('WebSocket', MockWebSocket);
    const received: bigint[] = [];
    const unsubscribe = subscribePointCloudObserver(
      'isaac.cell.1',
      (observation) => received.push(observation.sequence),
      () => undefined,
    );
    const replaced = MockWebSocket.instances[0]!;
    replaced.onclose?.();
    vi.advanceTimersByTime(1_000);
    const current = MockWebSocket.instances[1]!;

    replaced.onmessage?.({ data: packet() });
    current.onmessage?.({ data: packet() });

    expect(received).toEqual([7n]);
    unsubscribe();
  });
});
