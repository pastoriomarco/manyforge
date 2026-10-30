import type { JsonValue } from '../types/api';

/**
 * A gripper row's opt-in grasp confirmation:
 * `grasp_confirmation: {policy: required|not_required, stall_window?: {min_position, max_position}}`.
 * Absent means not required. A position gripper's evidence is its stall
 * window (the finished finger position inside it counts as a grasp); a
 * pneumatic gripper's is its mapped `pneumatic_io.inputs.workpiece_grasped`.
 * The Deployment owner validates the field; this only edits it.
 */
export type GraspConfirmationPolicy = 'required' | 'not_required';

/** The editor's form; the window limits as typed. */
export interface GraspConfirmationForm {
  policy: GraspConfirmationPolicy;
  minPosition: string;
  maxPosition: string;
}

function object(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

export function graspConfirmationForm(value: JsonValue | undefined): GraspConfirmationForm {
  const row = object(value);
  const window = object(row?.stall_window);
  const text = (limit: JsonValue | undefined) => (typeof limit === 'number' ? String(limit) : '');
  return {
    policy: row?.policy === 'required' ? 'required' : 'not_required',
    minPosition: text(window?.min_position),
    maxPosition: text(window?.max_position),
  };
}

function hasWindow(form: GraspConfirmationForm): boolean {
  return form.minPosition.trim() !== '' || form.maxPosition.trim() !== '';
}

/**
 * The row value for a form: absent when Off without a window. A pneumatic
 * gripper never carries a window.
 */
export function graspConfirmationValue(
  form: GraspConfirmationForm,
  positionGripper: boolean,
): JsonValue | undefined {
  const window = positionGripper && hasWindow(form);
  if (form.policy === 'not_required' && !window) return undefined;
  return {
    policy: form.policy,
    ...(window ? {
      stall_window: {
        min_position: Number(form.minPosition.trim()),
        max_position: Number(form.maxPosition.trim()),
      },
    } : {}),
  };
}

/** Why a position gripper's form cannot be written, or null. */
export function graspConfirmationProblem(
  form: GraspConfirmationForm,
  positionGripper: boolean,
): string | null {
  if (!positionGripper) return null;
  if (!hasWindow(form)) {
    return form.policy === 'required'
      ? 'Required needs a stall window: the finished finger positions that count as a grasp.'
      : null;
  }
  const min = Number(form.minPosition.trim());
  const max = Number(form.maxPosition.trim());
  if (form.minPosition.trim() === '' || form.maxPosition.trim() === ''
    || !Number.isFinite(min) || !Number.isFinite(max)) {
    return 'Enter both stall window limits as numbers.';
  }
  return min > max ? 'The stall window minimum must not exceed its maximum.' : null;
}

/** The input mapped as a pneumatic gripper's `workpiece_grasped`, or null. */
export function workpieceGraspedSignal(
  pneumaticIo: JsonValue | undefined,
): { componentId: string; signalName: string } | null {
  const signal = object(object(object(object(pneumaticIo)?.inputs)?.workpiece_grasped)?.signal);
  const componentId = signal?.controllable_component_id;
  const signalName = signal?.signal_name;
  return typeof componentId === 'string' && componentId !== ''
    && typeof signalName === 'string' && signalName !== ''
    ? { componentId, signalName } : null;
}
