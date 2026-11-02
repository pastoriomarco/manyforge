// Real browser/React interaction seam, with explicitly controlled transport.
// Native movement and settlement are qualified separately; this fixture never
// connects to a product stack and is not imported by the application entrypoint.
import { useLayoutEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { api, ApiError } from '../../src/api/client';
import { RobotControlPanelView } from '../../src/components/RobotControlPanel';
import { ProgramTransportControls } from '../../src/components/ProgramRuntimePanel';
import { useRobotControl, type RobotControlController } from '../../src/hooks/useRobotControl';
import type { HeadlessStateSource } from '../../src/hooks/useHeadlessState';
import type { ProgramRuntimeController } from '../../src/hooks/useProgramRuntime';
import type { ComposerHeadlessState } from '../../src/types/headless';
import type { ExecutionOwnerStatus, OperationObservation } from '../../src/types/robotControl';
import { statusFixture, receiptFixture, gripperReceiptFixture } from '../../src/testFixtures/robotControl';
import '../../src/styles/index.css';

let controller: RobotControlController;
let publish: (status: ExecutionOwnerStatus) => void;
let publishProgram: (active: boolean) => void;
let showPanel: (visible: boolean) => void;
let showTransport: (visible: boolean) => void;
let showGroups: () => void;
const groupCommands: string[][] = [];
let setNativeAvailable: (available: boolean) => void;
let currentStatus = statusFixture();
currentStatus.directControl.targetDescriptor.operationalLimit = {
  maximumTcpLinearSpeedMps: 0.2,
};
let previewCount = 0;
let availabilityCount = 0;
let manualReady = false;
let cleanupRequired = false;
let selectionRevision = 0;
let holdPreviews = false;
const heldPreviews: (() => void)[] = [];
let rejectPreview = false;
let previewFailureReason: string | null = null;
const commands: { kind: string; payload: unknown; resolve: () => void; reject: () => void }[] = [];
const source = {
  sendExecutionCommand(kind: string, payload: Record<string, unknown>, requestId: string) {
    if (commands.length >= 32) throw new Error('Test command bound exceeded');
    return new Promise((resolve, reject) => commands.push({
      kind, payload,
      resolve: () => { if (kind === 'program_manual_handover') cleanupRequired = false; resolve(kind === 'runtime_scale' ? { result: {
        ok: true, api_version: 'v1', command: 'set_runtime_scale_control', accepted: {
        accepted_at_ns: '1234',
        request_id: requestId, replay_status: 'accepted_new', selector: payload.selector,
        control_generation: Number(payload.expected_control_generation) + 1,
        requested_scale: payload.scale, active_operation_id: null,
        active_operation_revision: null, active_operation_scale_generation: null,
      } } } : {}); }, reject: () => reject(new Error('deliberate delayed rejection')),
    }));
  },
  reconnect() { throw new Error('Local preview retry must not reconnect'); },
} as unknown as HeadlessStateSource;

api.execution.manualAvailability = async (intent) => {
  availabilityCount += 1;
  const available = manualReady && !currentStatus.manualOperations.some((operation) => operation.resident);
  return {
    requestId: intent.request_context.request_id, requestOwnerEpoch: currentStatus.ownerEpoch,
    intentDigest: `sha256:${'a'.repeat(64)}`, available,
    unavailableReason: available ? null : 'manual_handover_producer_not_held',
    cleanupRequired: available && cleanupRequired,
    manualHandover: available ? { program_runner_owner_epoch: 'runner.1',
      program_run_id: 'program.1', claim_generation: 1, selection_revision: selectionRevision } : null,
  };
};

api.execution.previewMotion = async (intent) => {
  previewCount += 1;
  if (rejectPreview) {
    if (previewFailureReason !== null) throw new ApiError('deliberate target rejection', 500, {
      detail: {
        code: previewFailureReason === 'native_ik_no_solution' ? 'ik_failed' : 'planning_failed',
        details: { reason: previewFailureReason },
      },
    });
    throw new Error('deliberate preview rejection');
  }
  const payload = intent.participants[0].payload;
  const positions = payload.kind === 'motion' && payload.motion.kind === 'joint_goal'
    ? payload.motion.positions_rad : controller.state.draftPositionsRad;
  const receipt = receiptFixture(currentStatus, intent.request_context.request_id, positions);
  if (holdPreviews) await new Promise<void>((resolve) => heldPreviews.push(resolve));
  return receipt;
};
api.execution.previewCapabilityCommand = async (intent) => {
  previewCount += 1;
  return gripperReceiptFixture(currentStatus, intent.request_context.request_id,
    controller.state.draftGripperPosition!);
};

function App() {
  const [status, setStatus] = useState(currentStatus);
  const [programActive, setProgramActive] = useState(false);
  const [panelVisible, setPanelVisible] = useState(true);
  const [transportVisible, setTransportVisible] = useState(false);
  const [groupsVisible, setGroupsVisible] = useState(false);
  const [nativeAvailable, updateNativeAvailable] = useState(true);
  const result = useRobotControl(true, programActive, false,
    { executionControlStatus: nativeAvailable ? status : null,
      authoringAdmission: { authoringAllowed: !programActive, admissionGeneration: 1 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 1,
        state: programActive ? 'running' : 'idle_after_reset',
        programRunId: programActive ? 'program.1' : null, runRevision: 1,
        executionControlRevision: selectionRevision + 1,
      },
    } as ComposerHeadlessState, source);
  useLayoutEffect(() => {
    publish = (next) => { currentStatus = next; setStatus(next); };
    publishProgram = setProgramActive;
    showPanel = setPanelVisible;
    showTransport = setTransportVisible;
    showGroups = () => { setGroupsVisible(true); setTransportVisible(true); setPanelVisible(false); };
    setNativeAvailable = updateNativeAvailable;
    controller = result;
  }, [result]);
  const program = {
    cycleState: programActive ? 'running' : 'idle_after_reset',
    state: { runnerAvailable: true, applied: null, pending: null, controlPending: null,
      run: groupsVisible ? { state: 'running', capture: { programRunId: 'groups.1' } } : null,
      controls: groupsVisible ? { executionSelections: { groups: ['all', 'arm', 'grippers'].map((groupId) => ({
        groupId, state: 'running', stopAllowed: true, resumeAllowed: false,
      })) } } : null },
    runnerAllows: (command: string) => groupsVisible && command === 'program_motion_stop',
    stopMotion: (groups: string[]) => { groupCommands.push([...groups]); },
  } as unknown as ProgramRuntimeController;
  return <>
  {panelVisible && <div className="central-inspector-section viewer-inspector-section" style={{ width: '360px', height: '640px' }}>
    <div className="central-inspector-body robot-inspector-body">
      <RobotControlPanelView enabled controller={result} program={program} />
    </div>
  </div>}
  {transportVisible && <div style={{ position: 'absolute', left: 400, top: 0, width: 540, height: '100vh' }}>
    <ProgramTransportControls enabled program={program} robot={result}
      onOpenRobotControls={() => setPanelVisible(true)} />
  </div>}
  </>;
}

const harness = {
  showGroups() { showGroups(); },
  groupCommands() { return groupCommands; },
  snapshot() { return {
    phase: controller.state.phase, positions: controller.state.draftPositionsRad,
    ghost: controller.state.ghost !== null, ghostStale: controller.state.ghostStale,
    error: controller.state.error, controlPending: controller.controlPending,
    previewIssue: controller.state.previewIssue,
    operation: controller.state.operation, previewCount,
    availabilityCount, manualControlAllowed: controller.manualControlAllowed,
    intent: controller.state.intent, motionSettings: controller.state.motionSettings,
    runtimeScalePending: controller.runtimeScalePending, runtimeScaleError: controller.runtimeScaleError,
    runtimeScale: controller.runtimeScaleStatus,
    pose: controller.state.draftPose, gripper: controller.state.draftGripperPosition,
    dragging: controller.state.poseDragging, moveTo: controller.state.moveToDraft,
    authoringPending: controller.authoringPending,
    commands: commands.map(({ kind, payload }) => ({ kind, payload })),
  }; },
  editBoth() { controller.editJoint(0, 0.25); controller.editJoint(1, 0.35); },
  editJoint(index: number, value: number) { controller.editJoint(index, value); },
  mode(mode: Parameters<RobotControlController['selectMode']>[0]) { controller.selectMode(mode); },
  beginDrag() { return controller.beginPoseDrag(); },
  endDrag(dx: number | null) { controller.finishPoseDrag(dx === null ? null : {
    ...controller.state.draftPose, position_m: { ...controller.state.draftPose.position_m, x_m: controller.state.draftPose.position_m.x_m + dx },
  }); },
  editPoseX(x: number) { controller.editPose({ ...controller.state.draftPose,
    position_m: { ...controller.state.draftPose.position_m, x_m: x } }); },
  holdPreviews(hold: boolean) { holdPreviews = hold; },
  releasePreview(index: number) { heldPreviews[index](); },
  showPanel(visible: boolean) { showPanel(visible); },
  showTransport(visible: boolean) { showTransport(visible); },
  nativeAvailable(available: boolean) { setNativeAvailable(available); },
  releaseOperation() { publish({ ...currentStatus, manualOperations: currentStatus.manualOperations
    .map((operation) => ({ ...operation, resident: false, operationRevision: operation.operationRevision + 1 })) }); },
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
  manualReady(ready: boolean, cleanup = false) {
    manualReady = ready;
    cleanupRequired = cleanup;
    selectionRevision += 1;
    publish({ ...currentStatus });
  },
  setPreviewFailure(failed: boolean, reason: string | null = null) {
    rejectPreview = failed;
    previewFailureReason = reason;
  },
  refresh() { publish({ ...currentStatus, observedMotion: { ...currentStatus.observedMotion,
    sequence: currentStatus.observedMotion.sequence + 1 } }); },
  operation(id: string, state: OperationObservation['state'] = 'executing', error: string | null = null,
    target: 'motion' | 'gripper' = 'motion') {
    publish({ ...currentStatus, manualOperations: [{
      programRunId: (target === 'gripper' ? currentStatus.directControlGripper : currentStatus.directControl).programRunId,
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
  command(kind: 'stop') { void controller[kind](); },
  finish(index: number, reject = false) { if (reject) commands[index].reject(); else commands[index].resolve(); },
  retry() { controller.retry(); },
  revert() { controller.revert(); },
};
Object.assign(window, { robotControlHarness: harness });
createRoot(document.getElementById('root')!).render(<App />);
