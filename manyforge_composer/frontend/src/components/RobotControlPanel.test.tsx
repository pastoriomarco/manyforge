import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { RobotControlPanel } from './RobotControlPanel';

const controller = vi.hoisted(() => ({
  state: {
    status: {
      ownerEpoch: 'owner-1',
      cellId: 'cell-1',
      activePublicationGeneration: 1,
      graphRevision: 'sha256:graph',
      retainedBundleId: 'sha256:bundle',
      sceneRevision: {
        scene_id: 'scene-1',
        revision_id: 1,
        content_hash: 'sha256:scene',
      },
      directControl: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        baseFrameRoleId: 'base',
        tcpFrameRoleId: 'tcp',
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
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v5',
          ordinaryJerk: null,
          controllableComponentId: 'robot-1', commandTargetId: 'arm',
          componentRoleId: 'controlled_component', targetRoleId: 'command_target',
          frames: [
            { frameRoleId: 'base', compiledFrameName: 'base_link', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
            { frameRoleId: 'tcp', compiledFrameName: 'tool0', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
          ],
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
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v2',
          controllableComponentId: 'robot-1', commandTargetId: 'gripper', componentRoleId: 'controlled_component', targetRoleId: 'command_target',
          description: 'Test gripper', commandKind: 'gripper_position',
          actuationKind: 'position_commanded', transportKind: 'resolved_binding',
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
        fault: null,
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
      cartesianMaxBlendDeviationRad: 0.01, jointLimits: {} },
    generation: 1,
    intent: {},
    preview: {},
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
  fullStopPending: false,
  fullStopError: null,
  stopFaultRecoveryPending: false,
  stopFaultRecoveryError: null,
  motionTargets: [] as Array<Record<string, unknown>>,
  gripperTargets: [] as Array<Record<string, unknown>>,
  selectRobot: vi.fn(),
  selectTool: vi.fn(),
  selectMode: vi.fn(),
  editJoint: vi.fn(),
  editPose: vi.fn(),
  editGripper: vi.fn(),
  editMotionSettings: vi.fn(),
  approve: vi.fn(),
  revert: vi.fn(),
  stop: vi.fn(),
  fullStop: vi.fn(),
  recoverStopFault: vi.fn(),
  dismissFullStopError: vi.fn(),
  pause: vi.fn(),
  resume: vi.fn(),
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
      expect(markup).toContain('<summary title="Cartesian path fidelity, smoothing and optional tighter per-joint limits.">Advanced</summary>');
      expect(markup).toContain('Lin. Tol. (m)');
      expect(markup).toContain('Blend (rad)');
      expect(markup).toContain('aria-label="Manual Cartesian linear path tolerance (m)"');
      expect(markup).toContain('aria-label="Manual Maximum Cartesian joint-path blend deviation (rad); zero disables blending"');
    } finally {
      state.mode = previousMode;
    }
  });

  it('renders exact-operation Pause, Resume, and eligible Reset controls', () => {
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
    expect(markup).toMatch(/<button type="button">Pause<\/button>/);
    expect(markup).toMatch(/<button type="button" disabled="">Resume<\/button>/);
    expect(markup).toContain('Reset operation');

    state.operation = { ...(state.operation as Record<string, unknown>), state: 'paused' };
    markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    expect(markup).toContain('paused · 40%');
    expect(markup).not.toContain('Executing');
    expect(markup).toMatch(/<button type="button">Resume<\/button>/);

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
});
