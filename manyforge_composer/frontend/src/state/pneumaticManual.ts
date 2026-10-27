import type { ExecutionOwnerStatus, OperationObservation } from '../types/robotControl';
import type { IoSignalBinding } from '../api/parsers/ioSignals';

interface ManualValve {
  binding: IoSignalBinding;
  resourceRoleId: string;
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

/** Resolve only the Applied mapping to one exact intrinsic I/O binding per valve. */
export function manualPneumaticValves(status: ExecutionOwnerStatus, position: number): ManualValve[] {
  const outputs = status.directControlGripper?.targetDescriptor.pneumaticOutputs;
  if (outputs === undefined) throw new Error('The Applied pneumatic output mapping is unavailable.');
  const selected = manualPneumaticEndpoint(status, position);
  const valves = outputs.map((output) => {
    const matches = (status.ioSignalBindings ?? []).flatMap((binding) => (
      binding.controllableComponentId === output.controllableComponentId && binding.outputCommand !== undefined
        ? binding.signalRoleBindings.filter((signal) => signal.direction === 'output'
          && signal.signalName === output.signalName).map((signal) => ({ binding, signal })) : []
    ));
    if (matches.length !== 1) throw new Error(`Applied output ${output.controllableComponentId}/${output.signalName} has no unique direct I/O binding.`);
    return {
      binding: matches[0].binding,
      resourceRoleId: matches[0].signal.resourceRoleId,
      value: selected.open ? output.openValue : output.closeValue,
      componentId: output.controllableComponentId,
      signalName: output.signalName,
    };
  });
  return valves.sort((left, right) => Number(left.value) - Number(right.value)
    || (left.componentId < right.componentId ? -1 : left.componentId > right.componentId ? 1 : 0)
    || (left.signalName < right.signalName ? -1 : left.signalName > right.signalName ? 1 : 0));
}

export interface ManualValveIntent {
  request_context: { request_id: string; request_owner_epoch: string; source_label: 'composer_ui'; correlation_id: null };
  program_id: string;
  program_revision: string;
  program_binding_id: string;
  program_run_id: string;
  expected_graph_revision: string;
  expected_retained_bundle_id: string;
  expected_scene_revision: null;
  participants: [{ component_role_id: string; target_role_id: string; required_capability_id: string;
    action_span: null; payload: { kind: 'capability_command'; command_schema_id: string;
      command_schema_revision: string; command_value: { kind: 'io_signal_write'; resource_role_id: string; value: boolean };
      caller_options: [] } }];
  coordination_policy: 'independent';
}

export function manualValveIntent(status: ExecutionOwnerStatus, valve: ManualValve, requestId: string): ManualValveIntent {
  const command = valve.binding.outputCommand;
  if (command === undefined || requestId === '') throw new Error('The direct I/O command identity is unavailable.');
  return {
    request_context: { request_id: requestId, request_owner_epoch: status.ownerEpoch,
      source_label: 'composer_ui', correlation_id: null },
    program_id: command.programId, program_revision: command.programRevision,
    program_binding_id: valve.binding.programBindingId, program_run_id: command.programRunId,
    expected_graph_revision: status.graphRevision, expected_retained_bundle_id: status.retainedBundleId,
    expected_scene_revision: null,
    participants: [{ component_role_id: command.componentRoleId, target_role_id: valve.binding.targetRoleId,
      required_capability_id: command.capabilityId, action_span: null,
      payload: { kind: 'capability_command', command_schema_id: command.commandSchemaId,
        command_schema_revision: command.commandSchemaRevision, command_value: { kind: 'io_signal_write',
          resource_role_id: valve.resourceRoleId, value: valve.value }, caller_options: [] } }],
    coordination_policy: 'independent',
  };
}

/** Wait only for observation of each exact write; never resend an effect. */
export async function writeManualPneumaticValves(
  status: ExecutionOwnerStatus,
  position: number,
  io: {
    nextRequestId: () => string;
    checkCurrent: () => void;
    submit: (intent: ManualValveIntent) => Promise<{ result: unknown }>;
    read: (operationId: string, intent: ManualValveIntent, valve: ManualValve) => Promise<OperationObservation>;
    wait: () => Promise<void>;
  },
): Promise<void> {
  const valves = manualPneumaticValves(status, position);
  for (const valve of valves) {
    io.checkCurrent();
    const intent = manualValveIntent(status, valve, io.nextRequestId());
    const receipt = await io.submit(intent);
    const result = receipt.result;
    const accepted = result !== null && typeof result === 'object' && !Array.isArray(result)
      ? (result as Record<string, unknown>).accepted : null;
    if (accepted === null || typeof accepted !== 'object' || Array.isArray(accepted)) {
      throw new Error('Valve write acceptance is unconfirmed; inspect I/O status before another command.');
    }
    const row = accepted as Record<string, unknown>;
    if (typeof row.operation_id !== 'string' || row.operation_id === ''
      || row.request_id !== intent.request_context.request_id) {
      throw new Error('Valve write acceptance does not match the submitted request.');
    }
    for (;;) {
      io.checkCurrent();
      const observed = await io.read(row.operation_id, intent, valve);
      io.checkCurrent();
      if (observed.state === 'completed') {
        if (observed.terminalError !== null
          || (observed.outputEvidence !== 'source_accepted' && observed.outputEvidence !== 'submitted')) {
          throw new Error(`Valve write ${observed.operationId} has no successful delivery evidence (${observed.outputEvidence ?? 'unknown'}).`);
        }
        if (!observed.resident) break;
      } else if (['failed', 'stopped', 'interrupted', 'intervention_required', 'cancelled_pre_dispatch'].includes(observed.state)) {
        throw new Error(`Valve write ${observed.operationId} (${observed.outputEvidence ?? observed.state}): ${observed.terminalError ?? 'the output was not completed'}.`);
      }
      await io.wait();
    }
  }
}
