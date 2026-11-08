export type ActuatedCoordinateKind = 'revolute' | 'continuous' | 'prismatic';
export type CoordinateUnit = 'rad' | 'm';
export type GripperDraftSource = 'gripper_slider' | 'gripper_preset';

export interface GripperPositionDraft {
  source: GripperDraftSource;
  address: {
    componentId: string;
    targetId: string;
  };
  coordinate: {
    kind: ActuatedCoordinateKind;
    value: number;
    unit: CoordinateUnit;
  };
}

export function coordinateUnitForKind(
  kind: ActuatedCoordinateKind,
): CoordinateUnit {
  return kind === 'prismatic' ? 'm' : 'rad';
}

/**
 * Retain an addressed, unit-tagged UI draft. It cannot be submitted directly;
 * the exact gripper.command_position transport shape and validation live elsewhere.
 */
export function gripperPositionDraft(
  componentId: string,
  targetId: string,
  kind: ActuatedCoordinateKind,
  value: number,
  source: GripperDraftSource,
): GripperPositionDraft | null {
  const component = componentId.trim();
  const target = targetId.trim();
  if (!component || !target || !Number.isFinite(value)) return null;
  return {
    source,
    address: { componentId: component, targetId: target },
    coordinate: { kind, value, unit: coordinateUnitForKind(kind) },
  };
}
