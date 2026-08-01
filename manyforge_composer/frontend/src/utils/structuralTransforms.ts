import type { QuaternionXyzwTuple } from '../types/api';

export function normalizeQuaternion(
  quaternion: QuaternionXyzwTuple,
): QuaternionXyzwTuple {
  const norm = Math.hypot(...quaternion);
  if (!Number.isFinite(norm) || norm < 1e-12) {
    throw new Error('Cannot render a zero or non-finite quaternion');
  }
  return [
    quaternion[0] / norm,
    quaternion[1] / norm,
    quaternion[2] / norm,
    quaternion[3] / norm,
  ];
}

export type StructuralAssetFailureKind = 'stale_model' | 'asset_unavailable';

export function classifyStructuralAssetFailure(
  error: unknown,
): StructuralAssetFailureKind {
  const message = error instanceof Error ? error.message : String(error);
  const normalized = message.toLowerCase();
  return normalized.includes('409') || normalized.includes('stale urdf')
    ? 'stale_model'
    : 'asset_unavailable';
}
