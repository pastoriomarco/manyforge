import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  __pointCloudStoreTest,
  applyLatestPointCloudFrame,
  parsePointCloudPacket,
  registerPointCloudApplier,
} from './pointCloudStore';

const HEADER_BYTES = 40;

function packet({
  epoch = 1n,
  sequence = 1n,
  points = [[1, 2, 3]],
  colors = null,
  clear = false,
}: {
  epoch?: bigint;
  sequence?: bigint;
  points?: number[][];
  colors?: number[][] | null;
  clear?: boolean;
} = {}): ArrayBuffer {
  const count = clear ? 0 : points.length;
  const payloadBytes = count * 12 + (colors === null ? 0 : count * 3);
  const buffer = new ArrayBuffer(HEADER_BYTES + payloadBytes);
  const view = new DataView(buffer);
  for (const [index, byte] of [...'MFPC'].map((char) => char.charCodeAt(0)).entries()) {
    view.setUint8(index, byte);
  }
  view.setUint8(4, 1);
  view.setUint8(5, (colors === null ? 0 : 1) | (clear ? 2 : 0));
  view.setUint16(6, HEADER_BYTES, true);
  view.setBigUint64(8, epoch, true);
  view.setBigUint64(16, sequence, true);
  view.setBigUint64(24, 123_456n, true);
  view.setUint32(32, count, true);
  view.setUint32(36, payloadBytes, true);
  let offset = HEADER_BYTES;
  for (const point of points.slice(0, count)) {
    for (const coordinate of point) {
      view.setFloat32(offset, coordinate, true);
      offset += 4;
    }
  }
  for (const color of (colors ?? []).slice(0, count)) {
    for (const channel of color) {
      view.setUint8(offset, channel);
      offset += 1;
    }
  }
  return buffer;
}

describe('binary point-cloud frame slot', () => {
  afterEach(() => __pointCloudStoreTest.reset());

  it('parses the packed float32 and normalized uint8 blocks without copies', () => {
    const buffer = packet({
      points: [[1.25, -2.5, 3.75], [0, 0.5, 1]],
      colors: [[255, 128, 0], [0, 255, 64]],
    });
    const frame = parsePointCloudPacket(buffer);

    expect(frame?.pointCount).toBe(2);
    expect([...frame!.positions]).toEqual([1.25, -2.5, 3.75, 0, 0.5, 1]);
    expect([...frame!.colors!]).toEqual([255, 128, 0, 0, 255, 64]);
    expect(frame?.positions.buffer).toBe(buffer);
    expect(frame?.colors?.buffer).toBe(buffer);
  });

  it('rejects callbacks from a replaced point-cloud socket generation', () => {
    const currentSocket = {};
    const staleSocket = {};

    expect(__pointCloudStoreTest.ownsPointCloudConnection(
      currentSocket,
      currentSocket,
      4,
      4,
    )).toBe(true);
    expect(__pointCloudStoreTest.ownsPointCloudConnection(
      staleSocket,
      currentSocket,
      4,
      4,
    )).toBe(false);
    expect(__pointCloudStoreTest.ownsPointCloudConnection(
      currentSocket,
      currentSocket,
      3,
      4,
    )).toBe(false);
  });

  it('keeps only the newest sequence and applies it once per render wake', () => {
    const apply = vi.fn();
    registerPointCloudApplier(apply);
    apply.mockClear();

    __pointCloudStoreTest.publishPacket(packet({ sequence: 1n, points: [[1, 0, 0]] }));
    __pointCloudStoreTest.publishPacket(packet({ sequence: 3n, points: [[3, 0, 0]] }));
    __pointCloudStoreTest.publishPacket(packet({ sequence: 2n, points: [[2, 0, 0]] }));

    expect(applyLatestPointCloudFrame()).toBe(true);
    expect(apply).toHaveBeenCalledTimes(1);
    expect(apply.mock.calls[0][0].sequence).toBe(3n);
    expect([...apply.mock.calls[0][0].positions]).toEqual([3, 0, 0]);
    expect(applyLatestPointCloudFrame()).toBe(false);
  });

  it('accepts a fresh stream epoch and represents clear as an empty slot', () => {
    const apply = vi.fn();
    registerPointCloudApplier(apply);
    apply.mockClear();
    __pointCloudStoreTest.publishPacket(packet({ epoch: 1n, sequence: 50n }));
    __pointCloudStoreTest.publishPacket(packet({ epoch: 2n, sequence: 1n }));
    expect(applyLatestPointCloudFrame()).toBe(true);
    expect(apply.mock.calls[0][0].streamEpoch).toBe(2n);

    __pointCloudStoreTest.publishPacket(packet({ epoch: 2n, sequence: 2n, clear: true }));
    expect(applyLatestPointCloudFrame()).toBe(true);
    expect(apply).toHaveBeenLastCalledWith(null);
  });

  it('rejects malformed or oversized payloads before changing sequence state', () => {
    const malformed = packet();
    new Uint8Array(malformed)[0] = 0;
    expect(() => __pointCloudStoreTest.publishPacket(malformed)).toThrow('invalid magic');

    const oversized = packet();
    new DataView(oversized).setUint32(32, 200_001, true);
    expect(() => parsePointCloudPacket(oversized)).toThrow('point limit');

    const apply = vi.fn();
    registerPointCloudApplier(apply);
    apply.mockClear();
    __pointCloudStoreTest.publishPacket(packet({ sequence: 1n }));
    expect(applyLatestPointCloudFrame()).toBe(true);
    expect(apply).toHaveBeenCalledTimes(1);
  });
});
