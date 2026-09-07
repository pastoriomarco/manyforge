// Real browser/React interaction seam, with explicitly controlled transport.
// Native movement and settlement are qualified separately; this fixture never
// connects to a product stack and is not imported by the application entrypoint.
import { useLayoutEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { api } from '../../src/api/client';
import { RobotControlPanelView } from '../../src/components/RobotControlPanel';
import { useRobotControl, type RobotControlController } from '../../src/hooks/useRobotControl';
import type { HeadlessStateSource } from '../../src/hooks/useHeadlessState';
import type { ProgramRuntimeController } from '../../src/hooks/useProgramRuntime';
import type { ComposerHeadlessState } from '../../src/types/headless';
import type { ExecutionOwnerStatus, OperationObservation } from '../../src/types/robotControl';
import { statusFixture, receiptFixture, gripperReceiptFixture } from '../../src/testFixtures/robotControl';
import '../../src/index.css';

let controller: RobotControlController;
let publish: (status: ExecutionOwnerStatus) => void;
let publishProgram: (active: boolean) => void;
let currentStatus = statusFixture();
let previewCount = 0;
let rejectPreview = false;
const commands: { kind: string; payload: unknown; resolve: () => void; reject: () => void }[] = [];
const source = {
  sendExecutionCommand(kind: string, payload: Record<string, unknown>, requestId: string) {
    if (commands.length >= 32) throw new Error('Test command bound exceeded');
    return new Promise((resolve, reject) => commands.push({
      kind, payload,
      resolve: () => resolve(kind === 'runtime_scale' ? { result: {
        ok: true, api_version: 'v1', command: 'set_runtime_scale_control', accepted: {
        accepted_at_ns: '1234',
        request_id: requestId, replay_status: 'accepted_new', selector: payload.selector,
        control_generation: Number(payload.expected_control_generation) + 1,
        requested_scale: payload.scale, active_operation_id: null,
        active_operation_revision: null, active_operation_scale_generation: null,
      } } } : {}), reject: () => reject(new Error('deliberate delayed rejection')),
    }));
  },
  reconnect() { throw new Error('Local preview retry must not reconnect'); },
} as unknown as HeadlessStateSource;

api.execution.previewMotion = async (intent) => {
  previewCount += 1;
  if (rejectPreview) throw new Error('deliberate preview rejection');
  const payload = intent.participants[0].payload;
  const positions = payload.kind === 'motion' && payload.motion.kind === 'joint_goal'
    ? payload.motion.positions_rad : controller.state.draftPositionsRad;
  return receiptFixture(currentStatus, intent.request_context.request_id, positions);
};
api.execution.previewCapabilityCommand = async (intent) => {
  previewCount += 1;
  return gripperReceiptFixture(currentStatus, intent.request_context.request_id,
    controller.state.draftGripperPosition!);
};

function App() {
  const [status, setStatus] = useState(currentStatus);
  const [programActive, setProgramActive] = useState(false);
  const result = useRobotControl(true, programActive, false,
    { executionControlStatus: status } as ComposerHeadlessState, source);
  useLayoutEffect(() => {
    publish = (next) => { currentStatus = next; setStatus(next); };
    publishProgram = setProgramActive;
    controller = result;
  }, [result]);
  return <div className="viewer-inspector-section" style={{ width: '360px', height: '640px' }}>
    <RobotControlPanelView enabled controller={result} program={{
      cycleState: programActive ? 'running' : 'idle_after_reset', state: {},
    } as ProgramRuntimeController} />
  </div>;
}

const harness = {
  snapshot() { return {
    phase: controller.state.phase, positions: controller.state.draftPositionsRad,
    error: controller.state.error, controlPending: controller.controlPending,
    operation: controller.state.operation, previewCount,
    fullStopPending: controller.fullStopPending, fullStopError: controller.fullStopError,
    runtimeScalePending: controller.runtimeScalePending, runtimeScaleError: controller.runtimeScaleError,
    runtimeScale: controller.runtimeScaleStatus,
    pose: controller.state.draftPose, gripper: controller.state.draftGripperPosition,
    commands: commands.map(({ kind, payload }) => ({ kind, payload })),
  }; },
  editBoth() { controller.editJoint(0, 0.25); controller.editJoint(1, 0.35); },
  editJoint(index: number, value: number) { controller.editJoint(index, value); },
  mode(mode: Parameters<RobotControlController['selectMode']>[0]) { controller.selectMode(mode); },
  settled() { publish({ ...currentStatus, manualOperations: [{
    programRunId: currentStatus.directControl.programRunId, operationId: 'settled',
    operationRevision: 2, resident: false, state: 'completed', progress: 1,
    activeStopGeneration: null, activeStopId: null, terminalError: null,
  }] }); },
  measured(positions: number[]) { publish({ ...currentStatus, observedMotion: {
    ...currentStatus.observedMotion, positionsRad: positions,
    currentTcpPose: { position_m: { x_m: 0.3, y_m: 0.4, z_m: 0.5 },
      orientation: { x: 0, y: 0, z: 0, w: 1 } },
  }, observedGripper: { ...currentStatus.observedGripper!, position: 0.3 } }); },
  program(active: boolean) { publishProgram(active); },
  setPreviewFailure(failed: boolean) { rejectPreview = failed; },
  refresh() { publish({ ...currentStatus, observedMotion: { ...currentStatus.observedMotion,
    sequence: currentStatus.observedMotion.sequence + 1 } }); },
  operation(id: string, state: OperationObservation['state'] = 'executing', error: string | null = null) {
    publish({ ...currentStatus, manualOperations: [{
      programRunId: currentStatus.directControl.programRunId,
      operationId: id, operationRevision: 1, resident: true, state,
      progress: 0.5, activeStopGeneration: null, activeStopId: null, terminalError: error,
    }] });
  },
  replaceOwner() { publish(statusFixture(`${currentStatus.ownerEpoch}-next`)); },
  scale(value: number) { controller.setRuntimeScale(value); controller.commitRuntimeScale(); },
  freezeScale() { publish({ ...currentStatus, runtimeScaleControl: {
    ...currentStatus.runtimeScaleControl!, controlGeneration: 2,
    requestedScale: 0.5, frozen: true, effectiveScale: 0.5, applicationState: 'applied',
  } }); },
  enableScale() { publish({ ...currentStatus, runtimeScaleControl: {
    selector: { programBindingId: currentStatus.directControl.programBindingId,
      componentRoleId: currentStatus.directControl.componentRoleId,
      targetRoleId: currentStatus.directControl.targetRoleId },
    controlGeneration: 1, requestedScale: 1, supported: true, frozen: false,
    controlDomainId: 'domain', controlDomainRevision: 'revision', controlBindingId: 'binding',
    controlBindingRevision: 'revision', scalingDomainId: 'scale',
    activeControlConfigurationId: 'configuration', activeControlConfigurationGeneration: 1,
    activeOperationId: null, activeOperationRevision: null, activeOperationScaleGeneration: null,
    effectiveScale: null, applicationState: null, reason: null,
  } }); },
  command(kind: 'pause' | 'stop' | 'fullStop') { void controller[kind](); },
  finish(index: number, reject = false) { if (reject) commands[index].reject(); else commands[index].resolve(); },
  retry() { controller.retry(); },
  revert() { controller.revert(); },
};
Object.assign(window, { robotControlHarness: harness });
createRoot(document.getElementById('root')!).render(<App />);
