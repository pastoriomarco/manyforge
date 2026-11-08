import {
  coordinateUnitForKind,
  type ActuatedCoordinateKind,
  type CoordinateUnit,
} from './gripperAuthoring';

export interface JointCoordinateDraft {
  jointName: string;
  kind: ActuatedCoordinateKind;
  value: number;
  unit: CoordinateUnit;
}

export type JointCoordinateDrafts = Record<string, JointCoordinateDraft>;

/** Keep all edited coordinates and preserve their authored coordinate kinds. */
export function accumulateJointDrafts(
  previous: JointCoordinateDrafts,
  jointName: string,
  kind: ActuatedCoordinateKind,
  value: number,
): JointCoordinateDrafts {
  const name = jointName.trim();
  if (!name) throw new TypeError('Joint name must not be empty');
  if (!Number.isFinite(value)) throw new TypeError('Joint value must be finite');
  return {
    ...previous,
    [name]: {
      jointName: name,
      kind,
      value,
      unit: coordinateUnitForKind(kind),
    },
  };
}

/** A deterministic local list, intentionally not a command payload. */
export function jointDraftList(
  drafts: JointCoordinateDrafts,
): JointCoordinateDraft[] {
  return Object.keys(drafts).map((name) => drafts[name]);
}
