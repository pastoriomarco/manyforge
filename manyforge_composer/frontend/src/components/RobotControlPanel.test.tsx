import { readFileSync } from 'node:fs';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { RobotControlPanel, RobotControlPanelView } from './RobotControlPanel';
import { useRobotControl } from '../hooks/useRobotControl';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { LogicalSceneProjection } from '../types/api';
import { buildGripperCommandIntent, initialRobotControlState, robotControlReducer, selectedStatusForState } from '../state/robotControl';
import { gripperReceiptFixture, statusFixture } from '../testFixtures/robotControl';
import { tcpEntryIdentityKey } from '../types/robotControl';

const controller = vi.hoisted(() => ({
  state: {
    mountedToolByRobot: {},
    status: {
      ownerEpoch: 'owner-1',
      cellId: 'cell-1',
      activePublicationGeneration: 1,
      graphRevision: 'sha256:graph',
      retainedBundleId: 'sha256:bundle',
      directControl: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        baseFrameRoleId: 'base',
        tcpFrameRoleId: 'tcp',
        activeTcp: { entry: { componentId: 'robot-1', targetId: 'arm', frameRoleId: 'tcp' }, display: 'robot-1.tcp',
          ownerEpoch: 'owner-1', registrationGeneration: 1, sequence: 0 },
        operationCapabilityId: 'execution.trajectory.execute',
        planningCapabilityIds: ['planning.motion.joint_goal'],
        frameRoleIds: ['base', 'tcp'],
        programId: 'manyforge.direct_control.motion',
        programRevision: 'sha256:motion-program',
        programBindingId: 'sha256:motion-binding',
        programRunId: 'motion-run',
        joints: [{
          sourceJointName: 'joint_1',
          minPositionRad: -1,
          maxPositionRad: 1,
          maxVelocityRadS: 2,
          maxAccelerationRadS2: 3,
        }],
        targetDescriptor: {
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v6',
          ordinaryJerk: null,
          controllableComponentId: 'robot-1', commandTargetId: 'arm',
          endEffectorComponentId: null,
          componentRoleId: 'controlled_component', targetRoleId: 'command_target',
          frames: [
            { frameRoleId: 'base', compiledFrameName: 'base_link', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
            { frameRoleId: 'tcp', compiledFrameName: 'tool0', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
          ],
          tcps: [{
            entry: { componentId: 'robot-1', targetId: 'arm', frameRoleId: 'tcp' },
            targetRoleId: 'command_target', controllableComponentId: 'robot-1', commandTargetId: 'arm',
            frameRoleId: 'tcp', qualifiedName: 'robot-1.tcp', compiledFrameName: 'tool0',
            modelInstanceId: 'robot-model', modelRevision: 'sha256:model', isDefault: true,
            mountedHere: true, mountedElsewhere: false, contactBodyRoleIds: [],
          }],
          joints: [{ sourceJointName: 'joint_1', compiledJointName: 'robot__joint_1', unit: 'rad', minPositionRad: -1, maxPositionRad: 1, maxVelocityRadS: 2, maxAccelerationRadS2: 3 }],
          operationalJointLimits: [{
            sourceJointName: 'joint_1', modelMaxVelocityRadS: 2,
            modelMaxAccelerationRadS2: 3, deploymentMaxVelocityRadS: null,
            deploymentMaxAccelerationRadS2: null, deploymentMaxJerkRadS3: null,
            effectiveMaxVelocityRadS: 2, effectiveMaxAccelerationRadS2: 3,
            effectiveMaxJerkRadS3: null,
          }],
          operationalLimit: null,
          contracts: {
            operationCapabilityId: 'execution.trajectory.execute', operationCapabilityDefinitionRevision: 'sha256:execution',
            commandSchema: null, controlPath: null,
            supportingProviders: [{ capabilityId: 'planning.motion.joint_goal', capabilityDefinitionRevision: 'sha256:planning', capabilityProviderBindingId: 'planning', capabilityProviderBindingRevision: 'sha256:provider', providerManifestId: 'planner', providerManifestRevision: 'sha256:manifest' }],
          },
        },
      },
      directControlGripper: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'gripper',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        operationCapabilityId: 'tool.gripper.command',
        programId: 'manyforge.direct_control.gripper',
        programRevision: 'sha256:gripper-program',
        programBindingId: 'sha256:gripper-binding',
        programRunId: 'gripper-run',
        commandSchemaId: 'gripper.command_position',
        commandSchemaRevision: 'sha256:gripper-schema',
        sourceCoordinateName: 'finger_joint',
        sourceKind: 'model_role',
        coordinateKind: 'linear_m',
        minPosition: 0,
        maxPosition: 0.04,
        targetDescriptor: {
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v3',
          controllableComponentId: 'robot-1', commandTargetId: 'gripper', componentRoleId: 'controlled_component', targetRoleId: 'command_target',
          description: 'Test gripper', commandKind: 'gripper_position',
          actuationKind: 'position_commanded', transportKind: 'resolved_binding',
          outputMode: 'commanded',
          controlInterfaceContractId: null, stateAuthorityId: null,
          stateAuthorityClass: null, completionPolicy: 'goal_fenced_target_reached',
          graspConfirmationPolicy: 'binding_feedback_required', sourceGroupName: 'gripper',
          openStateRef: 'open', closedStateRef: 'open', commandDurationS: 0.5,
          touchLinks: ['finger_link'], mountFrame: 'tool0', attachFrame: 'tool0',
          tcpFrame: 'tool0', auxiliaryFrames: [],
          sourceCoordinateName: 'finger_joint', sourceKind: 'model_role', coordinateKind: 'linear_m', unit: 'm', minPosition: 0, maxPosition: 0.04,
          mimicFollowers: [], namedPresets: [{ name: 'open', masterPosition: 0.04, coordinates: [{ sourceCoordinateName: 'finger_joint', position: 0.04 }] }],
          contracts: {
            operationCapabilityId: 'tool.gripper.command', operationCapabilityDefinitionRevision: 'sha256:gripper',
            commandSchema: { commandSchemaId: 'gripper.command_position', commandSchemaRevision: 'sha256:gripper-schema' }, controlPath: null, supportingProviders: [],
          },
        },
      },
      observedMotion: {
        targetRoleId: 'arm',
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        sequence: 1,
        sourceEpoch: 'motion-source',
        sourceJointNames: ['joint_1'],
        positionsRad: [0],
        complete: true,
        currentTcpPose: {
          position_m: { x_m: 0, y_m: 0, z_m: 0.2 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
        tcpPoses: {
          '["robot-1","arm","tcp"]': {
            position_m: { x_m: 0, y_m: 0, z_m: 0.2 },
            orientation: { x: 0, y: 0, z: 0, w: 1 },
          },
        },
      },
      observedGripper: {
        sequence: 1,
        sourceEpoch: 'gripper-source',
        sourceCoordinateName: 'finger_joint',
        position: 0.02,
        coordinateKind: 'linear_m',
        complete: true,
      },
      cellStopAction: {
        available: true,
        unavailableReason: null,
        expectedStopGeneration: 0,
        latestTerminal: null,
      },
    },
    phase: 'ready',
    mode: 'joint',
    draftPositionsRad: [0.2],
    draftPose: {
      position_m: { x_m: 0, y_m: 0, z_m: 0.2 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    },
    draftGripperPosition: 0.02,
    motionSettings: { velocityScale: 1, accelerationScale: 1, tcpLinearSpeedMps: 0.1,
      tcpAngularSpeedRadS: 0.5, tcpLinearAccelerationMps2: 0.5, tcpAngularAccelerationRadS2: 2.5,
      cartesianTranslationToleranceM: 0.005, cartesianRotationToleranceRad: 0.05,
      clearanceM: null, jointLimits: {} },
    generation: 1,
    intent: {},
    preview: {
      sceneRevision: {
        scene_id: 'scene-1',
        revision_id: 1,
        content_hash: 'sha256:scene',
      },
    },
    ghost: null,
    ghostStale: false,
    operation: null,
    error: null,
  },
  controlPending: null,
  runtimeScaleStatus: null,
  runtimeScaleDraft: 1,
  runtimeScalePending: false,
  runtimeScaleError: null,
  motionTargets: [] as Array<Record<string, unknown>>,
  gripperTargets: [] as Array<Record<string, unknown>>,
  selectRobot: vi.fn(),
  selectTool: vi.fn(),
  selectTcp: vi.fn(),
  tcpApplying: false,
  tcpError: null,
  selectMode: vi.fn(),
  editJoint: vi.fn(),
  editPose: vi.fn(),
  editGripper: vi.fn(),
  editMotionSettings: vi.fn(),
  approve: vi.fn(),
  revert: vi.fn(),
  stop: vi.fn(),
  fullStop: vi.fn(),
  reset: vi.fn(),
  setRuntimeScale: vi.fn(),
  retry: vi.fn(),
}));

vi.mock('../hooks/useRobotControl', () => ({
  useRobotControl: () => ({ ...controller, currentStatus: controller.state.status,
    selectedStatus: controller.state.status,
    motionTargets: controller.motionTargets.length === 0
      ? [controller.state.status.directControl] : controller.motionTargets,
    gripperTargets: controller.gripperTargets.length === 0
      ? [controller.state.status.directControlGripper!] : controller.gripperTargets,
    manualControlAllowed: true, manualControlUnavailableReason: null }),
}));

describe('RobotControlPanel layout', () => {
  it('TCP-C: lists the robot\'s and the mounted tool\'s TCPs, the default selected', () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    status.directControl!.targetDescriptor.tcps = [
      { ...row, qualifiedName: 'lite_gripper.tcp' },
      { ...row, entry: { ...row.entry, frameRoleId: 'flange' }, frameRoleId: 'flange',
        qualifiedName: 'robot-a.flange', compiledFrameName: 'link_eef', isDefault: false },
      { ...row, entry: { componentId: 'lite_gripper', targetId: 'gripper', frameRoleId: 'cup_edge' },
        targetRoleId: 'mounted_tool_target', controllableComponentId: 'lite_gripper',
        commandTargetId: 'gripper', frameRoleId: 'cup_edge', qualifiedName: 'lite_gripper.cup_edge',
        compiledFrameName: 'cup_edge_link', isDefault: false, mountedHere: true,
        mountedElsewhere: false },
      { ...row, entry: { componentId: 'workbench_tool', targetId: 'tool', frameRoleId: 'tcp' },
        targetRoleId: null, controllableComponentId: 'workbench_tool', commandTargetId: 'tool',
        frameRoleId: 'tcp', qualifiedName: 'workbench_tool.tcp', isDefault: false,
        mountedHere: false, mountedElsewhere: true },
      { ...row, entry: { componentId: 'second_target', targetId: 'tool', frameRoleId: 'tcp' },
        targetRoleId: null, controllableComponentId: 'second_target', commandTargetId: 'tool',
        frameRoleId: 'tcp', qualifiedName: 'second_target.tcp', isDefault: false,
        mountedHere: true, mountedElsewhere: false },
    ];
    status.observedMotion!.tcpPoses = {
      [tcpEntryIdentityKey(status.directControl!.targetDescriptor.tcps[0].entry)]: status.observedMotion!.currentTcpPose!,
      [tcpEntryIdentityKey(status.directControl!.targetDescriptor.tcps[1].entry)]: status.observedMotion!.currentTcpPose!,
      [tcpEntryIdentityKey(status.directControl!.targetDescriptor.tcps[2].entry)]: status.observedMotion!.currentTcpPose!,
    };
    const state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state,
      selectedStatus: selectedStatusForState(state), motionTargets: [status.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null };
    const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    expect(markup).toContain('aria-label="Robot Control TCP"');
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;tcp&quot;]" selected="">lite_gripper.tcp (default)</option>');
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;flange&quot;]">robot-a.flange</option>');
    expect(markup).toContain('<option value="[&quot;lite_gripper&quot;,&quot;gripper&quot;,&quot;cup_edge&quot;]">lite_gripper.cup_edge</option>');
    expect(markup).toContain('Show TCPs of tools mounted on other robots');
    expect(markup).not.toContain('workbench_tool.tcp');
  });

  it.each(['approving', 'executing', 'stopping'] as const)(
    'disables the TCP picker while a manual operation is %s', (phase) => {
      const status = statusFixture();
      const row = status.directControl!.targetDescriptor.tcps[0];
      status.directControl!.targetDescriptor.tcps.push({ ...row,
        entry: { ...row.entry, frameRoleId: 'flange' }, frameRoleId: 'flange',
        qualifiedName: 'robot-a.flange', isDefault: false });
      const state = { ...robotControlReducer(initialRobotControlState, { type: 'status_resolved', status }), phase };
      const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state,
        selectedStatus: selectedStatusForState(state), motionTargets: [status.directControl!],
        manualControlAllowed: true, manualControlUnavailableReason: null };
      const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
      expect(markup).toMatch(/aria-label="Robot Control TCP"[^>]*disabled=""/);
      expect(markup).toContain('Sets this robot&#x27;s Active TCP. TCPs of tools on other robots are listed with the reason they can&#x27;t be used here.');
    },
  );

  it('keeps an existing TCP pick visible without a pose after it becomes unmounted', () => {
    const status = statusFixture();
    const baseTcp = status.directControl!.targetDescriptor.tcps[0];
    const toolTcp = {
      ...baseTcp,
      entry: { componentId: 'lite_gripper', targetId: 'gripper', frameRoleId: 'cup_edge' },
      targetRoleId: 'mounted_tool_target',
      controllableComponentId: 'lite_gripper', commandTargetId: 'gripper',
      frameRoleId: 'cup_edge', qualifiedName: 'lite_gripper.cup_edge',
      compiledFrameName: 'cup_edge_link', isDefault: false,
    };
    status.directControl!.targetDescriptor.tcps.push(toolTcp);
    const key = tcpEntryIdentityKey(toolTcp.entry);
    status.observedMotion!.tcpPoses[key] = status.observedMotion!.currentTcpPose!;
    status.directControl!.activeTcp = { entry: toolTcp.entry, display: toolTcp.qualifiedName,
      ownerEpoch: status.ownerEpoch, registrationGeneration: 1, sequence: 1 };
    let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });

    const unmounted = structuredClone(status);
    const changedRow = unmounted.directControl!.targetDescriptor.tcps.find((tcp) => (
      tcpEntryIdentityKey(tcp.entry) === key
    ))!;
    changedRow.mountedHere = false;
    changedRow.mountedElsewhere = true;
    changedRow.targetRoleId = null;
    delete unmounted.observedMotion!.tcpPoses[key];
    state = robotControlReducer(state, { type: 'status_refreshed', status: unmounted });
    state = robotControlReducer(state, { type: 'select_mode', mode: 'cartesian_path' });

    expect(state.mode).toBe('cartesian_path');
    const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state,
      selectedStatus: selectedStatusForState(state), motionTargets: [unmounted.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null };
    const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    expect(markup).toContain('TCP pose');
    expect(markup).toContain('<code>—</code>');
    expect(markup).toContain('Tool not mounted on this robot.');
    expect(markup).toContain('The current TCP pose is unavailable. Select a TCP with a reported pose to preview a Cartesian path.');
    expect(markup).not.toContain('The current owner-issued TCP pose is the explicit first waypoint.');
    expect(markup).toMatch(/class="approve-button"[^>]*disabled=""/);

    const withoutObservation = structuredClone(unmounted);
    withoutObservation.observedMotion = null;
    // The selected status is derived from the per-target list.
    withoutObservation.observedMotions = [];
    state = robotControlReducer(state, { type: 'status_refreshed', status: withoutObservation });
    const withoutObservationController: ReturnType<typeof useRobotControl> = {
      ...useRobotControl(true), state,
      selectedStatus: selectedStatusForState(state), motionTargets: [withoutObservation.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null,
    };
    const withoutObservationMarkup = renderToStaticMarkup(
      <RobotControlPanelView enabled controller={withoutObservationController} />,
    );
    expect(withoutObservationMarkup).not.toContain('aria-label="Selected TCP pose"');
  });

  it('keeps a joint Move To approval available when the selected TCP is unmounted', () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    row.mountedHere = false;
    row.mountedElsewhere = true;
    row.targetRoleId = null;
    status.directControl!.selectedTcpEntryKey = tcpEntryIdentityKey(row.entry);
    status.observedMotion!.currentTcpPose = null;
    status.observedMotion!.tcpPoses = {};
    const ready = useRobotControl(true).state;
    const state = { ...ready, status, mode: 'move_to' as const, moveToKind: 'joint' as const };
    const panelController: ReturnType<typeof useRobotControl> = {
      ...useRobotControl(true), state, selectedStatus: status, motionTargets: [status.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null,
    };
    const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    expect(markup).toMatch(/class="approve-button"(?![^>]*disabled)[^>]*>/);
  });

  it('B1: without an end-effector TCP offers joint jog only, with a plain note', () => {
    const status = statusFixture();
    status.directControl!.tcpFrameRoleId = null;
    status.directControl!.activeTcp = { entry: null, display: null, ownerEpoch: status.ownerEpoch,
      registrationGeneration: 1, sequence: 0 };
    status.observedMotion!.currentTcpPose = null;
    const row = status.directControl!.targetDescriptor.tcps[0];
    status.directControl!.targetDescriptor.tcps = [
      { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
        frameRoleId: 'flange', qualifiedName: 'robot-a.flange', compiledFrameName: 'link_eef', isDefault: false },
    ];
    status.observedMotion!.tcpPoses = {
      [tcpEntryIdentityKey(status.directControl!.targetDescriptor.tcps[0].entry)]:
        status.observedMotion!.tcpPoses[tcpEntryIdentityKey(row.entry)],
    };
    const state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state,
      selectedStatus: selectedStatusForState(state), motionTargets: [status.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null };
    const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    expect(markup).toMatch(/<button type="button" title="Joint" aria-pressed="true">Joint<\/button>/);
    for (const label of ['Pose plan', 'Cartesian', 'Move To']) {
      expect(markup).toMatch(new RegExp(`disabled="">${label}</button>`));
    }
    expect(markup).toContain('No TCP: joint jog only. Choose a TCP for Cartesian moves.');
    expect(markup).toContain('<option value="" disabled="" selected="">No End-effector TCP; choose one</option>');
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;flange&quot;]">robot-a.flange</option>');
  });

  it('does not offer Set TCP when the robot has no owner status', () => {
    const status = statusFixture();
    status.directControl!.activeTcp = null;
    const state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state,
      selectedStatus: selectedStatusForState(state), motionTargets: [status.directControl!],
      manualControlAllowed: true, manualControlUnavailableReason: null };
    const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    expect(markup).not.toContain('aria-label="Robot Control TCP"');
    expect(markup).not.toContain('No TCP: joint jog only.');
    expect(markup).toContain('Robot status unavailable.');
  });

  it.each(['commanded', 'none'] as const)(
    'keeps pneumatic %s authoring and exact preview approval behind owner eligibility during Stop', (outputMode) => {
      const status = statusFixture();
      Object.assign(status.directControlGripper!.targetDescriptor, { actuationKind: 'pneumatic', outputMode,
        completionPolicy: outputMode === 'none' ? 'no_output' : 'output_delivery',
        graspConfirmationPolicy: 'not_required' });
      status.observedGripper = null;
      status.observedGrippers = [];
      let state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
      state = robotControlReducer(state, { type: 'select_mode', mode: 'gripper' });
      state = robotControlReducer(state, { type: 'edit_gripper', position: 0.03 });
      state = robotControlReducer(state, { type: 'preview_started', generation: state.generation,
        intent: buildGripperCommandIntent(status, 0.03, 'manual-during-stop') });
      state = robotControlReducer(state, { type: 'preview_resolved', generation: state.generation,
        receipt: gripperReceiptFixture(status, 'manual-during-stop', 0.03) });
      expect(state.phase).toBe('ready');
      const panelController: ReturnType<typeof useRobotControl> = { ...useRobotControl(true), state, selectedStatus: status,
        manualControlAllowed: true, manualControlUnavailableReason: null };
      const program = { cycleState: 'running', state: { applied: null,
        controls: { executionSelections: { groups: [{ groupId: 'cell', state: 'stopping' }] } } },
      } as unknown as ProgramRuntimeController;
      const render = () => renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} program={program} />);
      expect(render()).not.toMatch(/class="approve-button"[^>]*disabled=""/);
      panelController.manualControlAllowed = false;
      panelController.manualControlUnavailableReason = 'Component not ready';
      expect(render()).toMatch(/class="approve-button"[^>]*disabled=""/);
      panelController.manualControlAllowed = true;
      panelController.state = { ...state, phase: 'executing', operation: {
        operationId: 'resident', operationRevision: 1, resident: true, state: 'executing', progress: 0,
        activeStopId: null, activeStopGeneration: null, terminalError: null,
      } };
      expect(render()).toMatch(/class="approve-button"[^>]*disabled=""/);
      panelController.state = { ...state, phase: 'blocked', preview: null };
      expect(render()).toMatch(/class="approve-button"[^>]*disabled=""/);
    },
  );

  it.each(['commanded', 'none'] as const)(
    'describes pneumatic %s completion as a command transaction', (outputMode) => {
      const previousMode = controller.state.mode;
      const descriptor = controller.state.status.directControlGripper!.targetDescriptor;
      const previous = { ...descriptor };
      try {
        controller.state.mode = 'gripper';
        Object.assign(descriptor, { actuationKind: 'pneumatic', outputMode,
          completionPolicy: outputMode === 'none' ? 'no_output' : 'output_delivery',
          graspConfirmationPolicy: 'not_required' });
        const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
        expect(markup).toContain('Completion covers the command transaction, not grasp verification.');
        expect(markup).not.toContain('Completion confirms the observed coordinate');
      } finally {
        Object.assign(descriptor, previous);
        controller.state.mode = previousMode;
      }
    },
  );

  it('preserves the position completion explanation', () => {
    const previousMode = controller.state.mode;
    try {
      controller.state.mode = 'gripper';
      const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
      expect(markup).toContain('Completion confirms the observed coordinate, not an object grasp.');
      expect(markup).not.toContain('Completion covers the command transaction');
    } finally { controller.state.mode = previousMode; }
  });

  it('offers Applied stored goals, never Draft goals, and invalidates removed sources', () => {
    const state = controller.state as unknown as Record<string, unknown>;
    const previous = { ...state };
    const node = (id: string) => ({ instance_id: id, node_id: 'motion.move_to_joint_target',
      parameters: { target_role: 'arm', goal: [{ joint_name: 'joint_1', position_rad: 0.2 }] }, children: [] });
    const applied = { graphRevision: 'sha256:graph', retainedBundleId: 'sha256:bundle',
      program: { tree: node('applied-goal') }, programBinding: { target_role_bindings: [
        { target_role_id: 'arm', controllable_component_id: 'robot-1', command_target_id: 'arm' },
      ] } };
    const program = { state: { applied, controlPending: null } } as unknown as ProgramRuntimeController;
    try {
      state.mode = 'move_to';
      state.moveToDraft = { source: { id: 'node:applied-goal', kind: 'joint', positionsRad: [0.2] } };
      let markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={useRobotControl(true)} program={program} />);
      expect(markup).toContain('Stored joints · applied-goal');
      expect(markup).not.toContain('draft-goal');
      applied.graphRevision = 'replaced';
      markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={useRobotControl(true)} program={program} />);
      expect(markup).not.toContain('Stored joints · applied-goal');
      expect(markup).toMatch(/class="approve-button"[^>]*disabled=""/);
    } finally {
      Object.assign(state, previous);
      delete state.moveToDraft;
    }
  });

  it('uses runtime object poses, not Draft poses or carried-object authored positions', () => {
    const state = controller.state as unknown as Record<string, unknown>;
    const previous = { ...state };
    const providers = controller.state.status.directControl.targetDescriptor.contracts.supportingProviders;
    providers.push({ ...providers[0], capabilityId: 'planning.motion.pose_goal' });
    const scene = { access: { projection_leases_open: true }, snapshot: { objects: [
      { scene_resource_id: 'runtime-object', placement: { kind: 'cell_fixed',
        pose_in_cell: { position_m: { x: 0.3, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1] } } },
      { scene_resource_id: 'carried-object', placement: { kind: 'carried' } },
    ] } } as unknown as LogicalSceneProjection;
    try {
      state.mode = 'move_to';
      state.moveToDraft = { source: { id: 'scene:runtime-object', kind: 'pose',
        pose: { position: [0.3, 0, 0], quaternion: [0, 0, 0, 1] } } };
      const panelController = useRobotControl(true);
      const render = () => renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController}
        runtimeScene={scene} controlBasePoseInCell={{ position: [0, 0, 0], quaternion: [0, 0, 0, 1] }} />);
      expect(render()).toContain('Object · runtime-object');
      expect(render()).not.toContain('Object · draft-object');
      expect(render()).not.toContain('Object · carried-object');
      expect(render()).not.toMatch(/class="approve-button"[^>]*disabled=""/);
      const placement = scene.snapshot!.objects[0].placement;
      if (placement.kind !== 'cell_fixed') throw new Error('invalid fixture');
      placement.pose_in_cell.position_m.x = 0.6;
      expect(render()).toMatch(/class="approve-button"[^>]*disabled=""/);
      placement.pose_in_cell.position_m.x = 0.3;
      scene.access.projection_leases_open = false;
      expect(render()).toMatch(/class="approve-button"[^>]*disabled=""/);
    } finally {
      providers.pop();
      Object.assign(state, previous);
      delete state.moveToDraft;
    }
  });
  it('warns that unapplied edits do not affect manual motion', () => {
    const mutable = controller as typeof controller & { unappliedChanges?: boolean };
    mutable.unappliedChanges = true;
    try {
      expect(renderToStaticMarkup(<RobotControlPanel enabled />))
        .toContain('Robots use the current runtime Scene and Applied Deployment.');
    } finally {
      delete mutable.unappliedChanges;
    }
  });
  it('shows a compact robot selector only when the owner offers multiple targets', () => {
    const second = structuredClone(controller.state.status.directControl) as Record<string, unknown>;
    second.controllableComponentId = 'robot-2';
    controller.motionTargets.push(
      controller.state.status.directControl as unknown as Record<string, unknown>,
      second,
    );
    try {
      const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
      expect(markup).toContain('aria-label="Robot target"');
      expect(markup).toContain('robot-1 · arm');
      expect(markup).toContain('robot-2 · arm');
    } finally {
      controller.motionTargets.length = 0;
    }
    expect(renderToStaticMarkup(<RobotControlPanel enabled />))
      .not.toContain('aria-label="Robot target"');
  });

  it('offers a tool component (mounted on the robot flange) next to the robot', () => {
    const tool = structuredClone(controller.state.status.directControlGripper) as unknown as Record<string, unknown>;
    tool.controllableComponentId = 'lite_gripper';
    controller.gripperTargets.push(
      controller.state.status.directControlGripper as unknown as Record<string, unknown>,
      tool,
    );
    try {
      const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
      expect(markup).toContain('aria-label="Robot target"');
      expect(markup).toContain('robot-1 · arm');
      expect(markup).toContain('lite_gripper · gripper');
      // The robot's own gripper is not offered as a separate component.
      expect(markup).not.toContain('>robot-1 · gripper</option>');
    } finally {
      controller.gripperTargets.length = 0;
    }
  });

  it('presents an expected target rejection locally without calling the robot blocked', () => {
    const state = controller.state as unknown as Record<string, unknown>;
    const previous = { ...state };
    try {
      state.phase = 'blocked';
      state.preview = null;
      state.intent = null;
      state.previewIssue = 'The robot would collide at the target.';
      const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
      expect(markup).toContain('robot-control-preview-notice');
      expect(markup).toContain('role="status"');
      expect(markup).toContain('Target not executable.');
      expect(markup).toContain('would collide at the target');
      expect(markup).not.toContain('Preview is blocked');
      expect(markup).not.toContain('Retry preview');
      expect(markup).not.toContain('role="alert"');
      expect(markup).toContain('aria-label="Hide preview notice"');
      expect(markup).toMatch(/class="approve-button"[^>]*disabled=""/);
    } finally {
      Object.assign(state, previous);
      delete state.previewIssue;
    }
  });

  it('keeps the candidate actions outside the bounded editor scroll region', () => {
    const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    const editor = markup.indexOf('data-testid="robot-control-editor-scroll"');
    const footer = markup.indexOf('data-testid="robot-control-candidate-footer"');

    expect(markup).toContain('data-testid="robot-control-panel"');
    expect(editor).toBeGreaterThan(0);
    expect(footer).toBeGreaterThan(editor);
    expect(markup).toContain('<h3>Preview</h3>');
    expect(markup).not.toContain('<h2>Robot Control</h2>');
    expect(markup).not.toContain('Addressed operation');
    expect(markup).not.toContain('No-effect result');
    expect(markup).not.toContain('source epoch');
    expect(markup).not.toContain('observed 0.00°');
    expect(markup).not.toContain('>Ready</span>');
    expect(markup).toContain('robot-control-target-pending');
    expect(markup).toContain('aria-label="Step (deg)"');
    expect(markup).toContain('step="5"');
    expect(markup).toContain('aria-label="Preview information"');
    expect(markup).not.toContain('Edit a target to preview its validated result.');
    expect(markup).not.toContain('Edits validate a no-motion preview after 150 ms.');
    expect(markup).toContain('>Approve</button>');
    expect(markup).toContain('>Stop operation</button>');
    expect(markup).toContain('title="Discard only the local no-motion preview."');
    expect(markup).not.toContain('Revert only discards the local preview.');
  });

  it('keeps common motion settings compact and Cartesian fidelity controls advanced', () => {
    const state = controller.state as unknown as Record<string, unknown>;
    const previousMode = state.mode;
    try {
      state.mode = 'cartesian_path';
      const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
      expect(markup).toContain('robot-control-motion-settings-grid');
      expect(markup).toContain('Vel. (%)');
      expect(markup).toContain('Acc. (%)');
      expect(markup).toContain('TCP Sp. (m/s)');
      expect(markup).toContain('<summary title="Cartesian path fidelity and optional tighter per-joint limits.">Advanced</summary>');
      expect(markup).toContain('Lin. Tol. (m)');
      expect(markup).not.toContain('Blend');
      expect(markup).toContain('aria-label="Manual Cartesian linear path tolerance (m)"');
    } finally {
      state.mode = previousMode;
    }
  });

  it('renders the eligible exact-operation Reset control without Pause or Resume', () => {
    const state = controller.state as unknown as Record<string, unknown>;
    state.phase = 'executing';
    state.operation = {
      operationId: 'operation-1',
      operationRevision: 4,
      resident: true,
      state: 'executing',
      progress: 0.4,
      activeStopId: null,
      activeStopGeneration: null,
      terminalError: null,
    };
    let markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    expect(markup).not.toMatch(/>Pause<\/button>/);
    expect(markup).not.toMatch(/>Resume<\/button>/);
    expect(markup).toContain('Reset operation');

    state.phase = 'completed';
    state.operation = {
      ...(state.operation as Record<string, unknown>),
      state: 'failed',
      progress: 1,
      terminalError: 'contained failure',
    };
    markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    expect(markup).toMatch(
      /aria-label="Reset operation" title="Release this exact terminal direct operation[^>]*>Reset operation<\/button>/,
    );
    expect(markup).toContain('Reset operation required.');
    expect(markup).toContain('History acknowledgement does not release it.');
    expect(markup.indexOf('aria-label="Reset operation"'))
      .toBeGreaterThan(markup.indexOf('data-testid="robot-control-candidate-footer"'));
    state.operation = { ...(state.operation as Record<string, unknown>), resident: false };
    markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    expect(markup).not.toContain('Reset operation required.');

    state.phase = 'ready';
    state.operation = null;
  });

  it('never resets typed inputs on Scene changes', () => {
    const source = readFileSync(new URL('./RobotControlPanel.tsx', import.meta.url), 'utf8');
    const start = source.indexOf('const inputResetKey = [');
    const key = source.slice(start, source.indexOf("].join('\\u0000');", start));
    expect(start).toBeGreaterThan(0);
    expect(key).not.toMatch(/scene|revision_id|content_hash/i);
  });
});
