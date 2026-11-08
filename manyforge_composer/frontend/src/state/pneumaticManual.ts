import { ioSignalId, type SetOutputResult } from '../types/components';
import type { ExecutionOwnerStatus } from '../types/robotControl';
import { codedErrorText } from './componentPresentation';

interface ManualValve {
  /** The wire signal id `<component>/<signal>`. */
  signalId: string;
  value: boolean;
  componentId: string;
  signalName: string;
}

/** Resolve a numeric request to the nearest configured endpoint (ties choose Close). */
export function manualPneumaticEndpoint(status: ExecutionOwnerStatus, position: number): {
  open: boolean; position: number;
} {
  const descriptor = status.directControlGripper?.targetDescriptor;
  const outputs = descriptor?.pneumaticOutputs;
  if (descriptor === undefined || outputs === undefined || outputs.length === 0) {
    throw new Error('The Applied pneumatic output mapping is unavailable.');
  }
  const preset = (name: string | null) => descriptor.namedPresets.filter((row) => row.name === name);
  const open = preset(descriptor.openStateRef);
  const closed = preset(descriptor.closedStateRef);
  if (open.length !== 1 || closed.length !== 1 || !Number.isFinite(position)
    || !Number.isFinite(open[0].masterPosition) || !Number.isFinite(closed[0].masterPosition)) {
    throw new Error('The configured Open and Close gripper presets are unavailable.');
  }
  if (open[0].masterPosition === closed[0].masterPosition) {
    throw new Error('The configured Open and Close gripper presets must have different positions.');
  }
  const isOpen = Math.abs(position - open[0].masterPosition) < Math.abs(position - closed[0].masterPosition);
  return { open: isOpen, position: isOpen ? open[0].masterPosition : closed[0].masterPosition };
}

/** The Applied pneumatic outputs for one endpoint, false writes first. */
export function manualPneumaticValves(status: ExecutionOwnerStatus, position: number): ManualValve[] {
  const outputs = status.directControlGripper?.targetDescriptor.pneumaticOutputs;
  if (outputs === undefined) throw new Error('The Applied pneumatic output mapping is unavailable.');
  const selected = manualPneumaticEndpoint(status, position);
  const valves = outputs.map((output) => ({
    signalId: ioSignalId(output.controllableComponentId, output.signalName),
    value: selected.open ? output.openValue : output.closeValue,
    componentId: output.controllableComponentId,
    signalName: output.signalName,
  }));
  return valves.sort((left, right) => Number(left.value) - Number(right.value)
    || (left.componentId < right.componentId ? -1 : left.componentId > right.componentId ? 1 : 0)
    || (left.signalName < right.signalName ? -1 : left.signalName > right.signalName ? 1 : 0));
}

/**
 * Write each valve through the one output write path, one
 * after the other; a failed or unknown write stops the sequence with its
 * code and is never resent.
 */
export async function writeManualPneumaticValves(
  status: ExecutionOwnerStatus,
  position: number,
  io: {
    checkCurrent: () => void;
    setOutput: (signalId: string, value: boolean) => Promise<SetOutputResult>;
  },
): Promise<void> {
  for (const valve of manualPneumaticValves(status, position)) {
    io.checkCurrent();
    const result = await io.setOutput(valve.signalId, valve.value);
    if (result.outcome === 'succeeded') continue;
    const reason = result.error === undefined ? '' : `: ${codedErrorText(result.error)} (${result.error.code})`;
    throw new Error(result.outcome === 'unknown'
      ? `Valve write ${valve.signalId} outcome unknown (not resent)${reason}.`
      : `Valve write ${valve.signalId} failed${reason}.`);
  }
}
