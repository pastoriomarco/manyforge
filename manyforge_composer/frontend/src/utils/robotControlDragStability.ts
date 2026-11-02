import type { RuntimeLinkTransform } from '../state/runtimeScenePresentation';

export interface CandidateDragReference {
  identity: string;
  basePose: RuntimeLinkTransform;
  tcpPose: RuntimeLinkTransform;
}

const POSITION_TOLERANCE_M = 0.001;
const ANGULAR_TOLERANCE_RAD = 0.2 * Math.PI / 180;

export function snapshotRuntimeLinkTransform(
  transform: RuntimeLinkTransform,
): RuntimeLinkTransform {
  return {
    position_m: [...transform.position_m],
    orientation_xyzw: [...transform.orientation_xyzw],
  };
}

function positionWithinTolerance(
  reference: RuntimeLinkTransform['position_m'],
  current: RuntimeLinkTransform['position_m'],
): boolean {
  if (![...reference, ...current].every(Number.isFinite)) return false;
  const distance = Math.hypot(
    current[0] - reference[0],
    current[1] - reference[1],
    current[2] - reference[2],
  );
  return Number.isFinite(distance) && distance <= POSITION_TOLERANCE_M;
}

function normalizedQuaternion(
  orientation: RuntimeLinkTransform['orientation_xyzw'],
): RuntimeLinkTransform['orientation_xyzw'] | null {
  if (!orientation.every(Number.isFinite)) return null;
  const norm = Math.hypot(...orientation);
  if (!Number.isFinite(norm) || norm === 0) return null;
  return orientation.map((component) => component / norm) as RuntimeLinkTransform['orientation_xyzw'];
}

function rotationWithinTolerance(
  reference: RuntimeLinkTransform['orientation_xyzw'],
  current: RuntimeLinkTransform['orientation_xyzw'],
): boolean {
  const referenceUnit = normalizedQuaternion(reference);
  const currentUnit = normalizedQuaternion(current);
  if (referenceUnit === null || currentUnit === null) return false;

  const [x1, y1, z1, w1] = referenceUnit;
  const [x2, y2, z2, w2] = currentUnit;
  const relativeX = w1 * x2 - x1 * w2 - y1 * z2 + z1 * y2;
  const relativeY = w1 * y2 + x1 * z2 - y1 * w2 - z1 * x2;
  const relativeZ = w1 * z2 - x1 * y2 + y1 * x2 - z1 * w2;
  const relativeW = w1 * w2 + x1 * x2 + y1 * y2 + z1 * z2;
  if (![relativeX, relativeY, relativeZ, relativeW].every(Number.isFinite)) return false;
  // q and -q encode the same rotation. atan2 keeps small-angle precision.
  const angle = 2 * Math.atan2(
    Math.hypot(relativeX, relativeY, relativeZ),
    Math.abs(relativeW),
  );
  return Number.isFinite(angle) && angle <= ANGULAR_TOLERANCE_RAD;
}

function poseWithinTolerance(
  reference: RuntimeLinkTransform,
  current: RuntimeLinkTransform,
): boolean {
  return positionWithinTolerance(reference.position_m, current.position_m)
    && rotationWithinTolerance(reference.orientation_xyzw, current.orientation_xyzw);
}

export function candidateDragReferenceIsStable(
  reference: CandidateDragReference,
  current: CandidateDragReference,
): boolean {
  return reference.identity === current.identity
    && poseWithinTolerance(reference.basePose, current.basePose)
    && poseWithinTolerance(reference.tcpPose, current.tcpPose);
}
