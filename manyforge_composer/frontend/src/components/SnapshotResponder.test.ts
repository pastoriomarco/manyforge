import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { validateSnapshotRequest } from './SnapshotResponder';

describe('snapshot request presentation adapter', () => {
  it('accepts exact positive dimensions and rejects malformed requests', () => {
    expect(validateSnapshotRequest({
      requestId: 'snapshot-1', width: 640, height: 480, mime: 'image/png',
      timeoutMs: 2_000,
      requiredFrame: { ownerEpoch: 'owner.1', generation: 7 },
    })).toBeNull();
    expect(validateSnapshotRequest({
      requestId: '', width: 640, height: 480, mime: 'image/png',
      timeoutMs: 2_000,
      requiredFrame: { ownerEpoch: 'owner.1', generation: 7 },
    })).toContain('ID');
    expect(validateSnapshotRequest({
      requestId: 'snapshot-1', width: 0, height: 480, mime: 'image/jpeg',
      timeoutMs: 2_000,
      requiredFrame: { ownerEpoch: 'owner.1', generation: 7 },
    })).toContain('qualified bounds');
  });

  it('keeps one observer subscription while presented-frame callbacks advance', () => {
    const source = readFileSync(
      new URL('./SnapshotResponder.tsx', import.meta.url),
      'utf8',
    );
    expect(source).toContain('uploadRef.current = upload');
    expect(source).toContain('ensureFrameRef.current = ensureFrame');
    expect(source).toContain('applyFrameRef.current = applyFrame');
    expect(source).toContain('}, [store, subscribe]);');
    expect(source).not.toContain(
      '[applyFrame, ensureFrame, store, subscribe, upload]',
    );
  });
});
