import type { ProgramSignalReference, ProgramTargetReference } from '../types/api';
import { IO_INPUT_NODE_IDS, ioSignalChoices } from './ioAuthoring';

export interface ProgramReferenceChoice<T> {
  value: T;
  label: string;
  reason?: string;
}

/** Selector facts needed to present a target reference in a Program field. */
export interface TargetReferenceChoiceFact {
  target: ProgramTargetReference;
  capabilities: readonly {
    capability_id: string;
    available: boolean;
    reason: string | null;
  }[];
}

/** Selector facts needed to present a signal reference in a Program field. */
export interface SignalReferenceChoiceFact {
  reference: ProgramSignalReference;
  ambiguous: boolean;
  target: ProgramTargetReference | null;
  value_type: string | null;
  direction: 'input' | 'output' | null;
  provider_profile: string | null;
}

function unavailableCapability(
  capabilities: TargetReferenceChoiceFact['capabilities'],
  requiredCapabilityIds: readonly string[],
): string | undefined {
  return requiredCapabilityIds.map((capabilityId) => {
    const capability = capabilities.find((item) => item.capability_id === capabilityId);
    return capability?.available ? null : capability?.reason ?? `Missing ${capabilityId}`;
  }).find((reason) => reason !== null);
}

/** Builds choices without collapsing equal target ids from different components. */
export function programTargetReferenceChoices(
  facts: readonly TargetReferenceChoiceFact[] | null,
  requiredCapabilityIds: readonly string[],
): ProgramReferenceChoice<ProgramTargetReference>[] {
  return (facts ?? []).map((row) => {
    const reason = unavailableCapability(row.capabilities, requiredCapabilityIds);
    return {
      value: { ...row.target },
      label: `${row.target.component_id} · ${row.target.command_target_id}`,
      ...(reason ? { reason } : {}),
    };
  });
}

function sameTarget(a: ProgramTargetReference, b: ProgramTargetReference): boolean {
  return a.component_id === b.component_id && a.command_target_id === b.command_target_id;
}

/**
 * Projects qualified signal facts for a Program field, preserving the existing
 * I/O direction/type/ambiguity checks and target capability precedence.
 */
export function programSignalReferenceChoices(
  nodeId: string,
  signals: readonly SignalReferenceChoiceFact[] | null,
  targets: readonly TargetReferenceChoiceFact[] | null,
  requiredCapabilityIds: readonly string[],
): ProgramReferenceChoice<ProgramSignalReference>[] {
  const ioChoices = ioSignalChoices(nodeId, signals === null ? null : signals.map((row) => ({
    reference: { ...row.reference },
    ambiguous: row.ambiguous,
    value_type: row.value_type,
    direction: row.direction,
  })));

  return (signals ?? []).map((row) => {
    const ioChoice = ioChoices?.find((choice) => (
      choice.value.component_id === row.reference.component_id
      && choice.value.signal_name === row.reference.signal_name
    ));
    const target = row.target;
    const signalTarget = target === null
      ? undefined
      : targets?.find((candidate) => sameTarget(candidate.target, target));
    const input = IO_INPUT_NODE_IDS.has(nodeId);
    const capabilityIssue = input && row.provider_profile === 'system_carried_object'
      ? undefined
      : signalTarget === undefined
        ? unavailableCapability([], requiredCapabilityIds)
        : unavailableCapability(signalTarget.capabilities, requiredCapabilityIds);
    const reason = row.ambiguous ? 'Ambiguous signal identity'
      : ioChoice?.reason ?? capabilityIssue;
    return {
      value: { ...row.reference },
      label: `${row.reference.component_id} · ${row.reference.signal_name}`,
      ...(reason ? { reason } : {}),
    };
  });
}
