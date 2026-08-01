import type {
  ActuatedCoordinateKind,
  CoordinateUnit,
} from './gripperAuthoring';

export interface UnitTaggedCoordinate {
  name: string;
  kind: ActuatedCoordinateKind;
  value: number;
  unit: CoordinateUnit;
}

export interface GripperGhostPreviewInput {
  previewEligible: boolean;
  gripperJoint: UnitTaggedCoordinate;
  limitLower: number | null;
  limitUpper: number | null;
  currentCoordinates: UnitTaggedCoordinate[];
  subtreeLinks: string[];
}

export interface GripperGhostPreview {
  coordinates: UnitTaggedCoordinate[];
  linkNames: string[];
}

function withinLimit(target: number, lower: number | null, upper: number | null): boolean {
  if (lower !== null && Number.isFinite(lower) && target < lower) return false;
  if (upper !== null && Number.isFinite(upper) && target > upper) return false;
  return true;
}

/**
 * Build a side-effect-free, unit-tagged preview plan. I6 reconnects this to a
 * qualified FK preview API; no old all-radians request shape is retained.
 */
export function buildGripperGhostPreview(
  input: GripperGhostPreviewInput,
): GripperGhostPreview | null {
  const target = input.gripperJoint;
  if (
    !input.previewEligible
    || !target.name.trim()
    || !Number.isFinite(target.value)
    || (target.kind === 'prismatic' ? target.unit !== 'm' : target.unit !== 'rad')
    || !withinLimit(target.value, input.limitLower, input.limitUpper)
  ) {
    return null;
  }
  const linkNames = Array.from(new Set(
    input.subtreeLinks.map((name) => name.trim()).filter(Boolean),
  ));
  if (linkNames.length === 0) return null;

  const coordinates = input.currentCoordinates.map((coordinate) => ({ ...coordinate }));
  const index = coordinates.findIndex((coordinate) => coordinate.name === target.name);
  if (index >= 0) {
    const current = coordinates[index];
    if (current.kind !== target.kind || current.unit !== target.unit) return null;
    coordinates[index] = { ...target, name: target.name.trim() };
  } else {
    coordinates.push({ ...target, name: target.name.trim() });
  }
  return { coordinates, linkNames };
}
