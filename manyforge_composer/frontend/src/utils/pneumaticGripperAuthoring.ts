import type { JsonValue } from '../types/api';

export const PNEUMATIC_INPUT_KEYS = ['open', 'closed', 'pressure', 'workpiece_grasped'] as const;
export type PneumaticInputKey = typeof PNEUMATIC_INPUT_KEYS[number];

export interface PneumaticGripperDraft {
  componentId: string;
  targetId: string;
  actuationKind: JsonValue | undefined;
  outputMode: JsonValue | undefined;
  pneumaticIo: JsonValue | undefined;
  path: Array<string | number>;
}

function object(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

/** Read the authored gripper rows, independently of any Applied descriptor. */
export function pneumaticGripperDrafts(document: Record<string, JsonValue> | null): PneumaticGripperDraft[] {
  if (document === null) return [];
  const intent = document.api_version === 'manyforge.deployment_intent.v2';
  const holders = intent ? document.components : object(document.graph)?.runtime_units;
  if (!Array.isArray(holders)) return [];
  return holders.flatMap((holder, index) => {
    const configuration = object(object(holder)?.configuration);
    if (configuration === null) return [];
    const nested = object(configuration.runtime_configuration);
    const runtime = nested ?? configuration;
    const rows = runtime.gripper_target_configurations;
    if (!Array.isArray(rows)) return [];
    const prefix: Array<string | number> = intent
      ? ['components', index, 'configuration'] : ['graph', 'runtime_units', index, 'configuration'];
    if (nested !== null) prefix.push('runtime_configuration');
    return rows.flatMap((value, rowIndex) => {
      const row = object(value);
      const componentId = row?.controllable_component_id;
      const targetId = row?.command_target_id;
      return typeof componentId !== 'string' || typeof targetId !== 'string' ? [] : [{
        componentId,
        targetId,
        actuationKind: row?.actuation_kind,
        outputMode: row?.output_mode,
        pneumaticIo: row?.pneumatic_io,
        path: [...prefix, 'gripper_target_configurations', rowIndex, 'pneumatic_io'],
      }];
    });
  });
}

/** Logical keys configured for a target; the Program never stores hardware signal names. */
export function declaredPneumaticInputs(
  document: Record<string, JsonValue> | null,
  componentId: string | undefined,
  targetId: string | undefined,
): PneumaticInputKey[] {
  const target = pneumaticGripperDrafts(document).find((row) => (
    row.componentId === componentId && row.targetId === targetId
  ));
  const inputs = object(object(target?.pneumaticIo)?.inputs);
  return PNEUMATIC_INPUT_KEYS.filter((key) => {
    const signal = object(object(inputs?.[key])?.signal);
    return typeof signal?.controllable_component_id === 'string'
      && signal.controllable_component_id !== ''
      && typeof signal.signal_name === 'string' && signal.signal_name !== '';
  });
}
