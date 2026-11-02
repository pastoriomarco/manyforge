import { describe, expect, it } from 'vitest';

import type { RuntimeLinkTransform } from '../state/runtimeScenePresentation';
import {
  candidateDragReferenceIsStable,
  snapshotRuntimeLinkTransform,
  type CandidateDragReference,
} from './robotControlDragStability';

const identityParts = {
  epoch: 'owner.epoch.1',
  bundle: 'bundle.1',
  graph: 'graph.1',
  binding: 'binding.1',
  base: { modelInstanceId: 'robot', compiledFrameName: 'base_link' },
  tcp: { modelInstanceId: 'robot', compiledFrameName: 'tcp' },
};

function identity(overrides: Partial<typeof identityParts> = {}): string {
  const parts = { ...identityParts, ...overrides };
  return JSON.stringify([
    parts.epoch,
    parts.bundle,
    parts.graph,
    parts.binding,
    parts.base,
    parts.tcp,
  ]);
}

function transform(
  position_m: [number, number, number] = [0, 0, 0],
  orientation_xyzw: [number, number, number, number] = [0, 0, 0, 1],
): RuntimeLinkTransform {
  return { position_m, orientation_xyzw };
}

function reference(
  overrides: Partial<Pick<CandidateDragReference, 'basePose' | 'tcpPose'>> = {},
  key = identity(),
): CandidateDragReference {
  return {
    identity: key,
    basePose: overrides.basePose ?? transform(),
    tcpPose: overrides.tcpPose ?? transform(),
  };
}

function rotationZ(angleRad: number): [number, number, number, number] {
  return [0, 0, Math.sin(angleRad / 2), Math.cos(angleRad / 2)];
}

const ANGULAR_TOLERANCE_RAD = 0.2 * Math.PI / 180;

describe('Robot Control candidate drag stability', () => {
  it('accepts small independent base and TCP pose jitter', () => {
    const start = reference();
    const current = reference({
      basePose: transform([0.0004, -0.0002, 0.0001], rotationZ(0.0034)),
      tcpPose: transform([-0.0003, 0.0002, 0.0001], rotationZ(-0.0032)),
    });

    expect(candidateDragReferenceIsStable(start, current)).toBe(true);
  });

  it('accepts position movement exactly at the 1 mm boundary', () => {
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ basePose: transform([0.001, 0, 0]) }),
    )).toBe(true);
  });

  it('accepts rotation exactly at the 0.2 degree boundary', () => {
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ tcpPose: transform([0, 0, 0], rotationZ(ANGULAR_TOLERANCE_RAD)) }),
    )).toBe(true);
  });

  it('snapshots measured tuples so press-time evidence cannot change in place', () => {
    const position_m: [number, number, number] = [0.1, 0.2, 0.3];
    const orientation_xyzw: [number, number, number, number] = [0, 0, 0, 1];
    const snapshot = snapshotRuntimeLinkTransform({ position_m, orientation_xyzw });
    position_m[0] = 9;
    orientation_xyzw[3] = -1;

    expect(snapshot).toEqual({ position_m: [0.1, 0.2, 0.3], orientation_xyzw: [0, 0, 0, 1] });
  });

  it('rejects a base displacement whose Euclidean distance exceeds 1 mm', () => {
    // Each axis delta is below 1 mm; the combined displacement is above it.
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ basePose: transform([0.0008, 0.0008, 0]) }),
    )).toBe(false);
  });

  it('rejects TCP position movement above 1 mm', () => {
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ tcpPose: transform([0, 0, -0.0011]) }),
    )).toBe(false);
  });

  it('rejects base or TCP rotation above 0.2 degrees', () => {
    const start = reference();
    expect(candidateDragReferenceIsStable(
      start,
      reference({ basePose: transform([0, 0, 0], rotationZ(0.0036)) }),
    )).toBe(false);
    expect(candidateDragReferenceIsStable(
      start,
      reference({ tcpPose: transform([0, 0, 0], rotationZ(-0.0036)) }),
    )).toBe(false);
  });

  it('treats quaternion sign and scale as the same rotation', () => {
    const start = reference({
      basePose: transform([0, 0, 0], [0, 0, 0, 2]),
    });
    const antipodal = reference({
      basePose: transform([0, 0, 0], [0, 0, 0, -1]),
      tcpPose: transform([0, 0, 0], [0, 0, 0, -1]),
    });

    expect(candidateDragReferenceIsStable(start, antipodal)).toBe(true);
  });

  it.each([
    ['observation epoch', { epoch: 'owner.epoch.2' }],
    ['retained bundle', { bundle: 'bundle.2' }],
    ['graph revision', { graph: 'graph.2' }],
    ['program binding', { binding: 'binding.2' }],
    ['base frame', { base: { modelInstanceId: 'robot', compiledFrameName: 'base_2' } }],
    ['selected TCP', { tcp: { modelInstanceId: 'tool', compiledFrameName: 'tcp_2' } }],
  ] as Array<[string, Partial<typeof identityParts>]>)('rejects an exact %s identity change', (_name, changed) => {
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({}, identity(changed)),
    )).toBe(false);
  });

  it('rejects invalid or zero-norm orientation data instead of accepting it as stable', () => {
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ tcpPose: transform([0, 0, 0], [0, 0, 0, 0]) }),
    )).toBe(false);
    expect(candidateDragReferenceIsStable(
      reference(),
      reference({ basePose: transform([0, 0, 0], [Number.NaN, 0, 0, 1]) }),
    )).toBe(false);
  });
});
