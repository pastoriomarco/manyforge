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
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v3',
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
  selectMode: vi.fn(),
  editJoint: vi.fn(),
  editPose: vi.fn(),
  editGripper: vi.fn(),
  approve: vi.fn(),
  revert: vi.fn(),
  stop: vi.fn(),
  fullStop: vi.fn(),
  dismissFullStopError: vi.fn(),
  pause: vi.fn(),
  resume: vi.fn(),
  reset: vi.fn(),
  setRuntimeScale: vi.fn(),
  retry: vi.fn(),
}));

vi.mock('../hooks/useRobotControl', () => ({
  useRobotControl: () => controller,
}));

describe('RobotControlPanel layout', () => {
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

    state.phase = 'completed';
    state.operation = {
      ...(state.operation as Record<string, unknown>),
      state: 'failed',
      progress: 1,
      terminalError: 'contained failure',
    };
    markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    expect(markup).toMatch(
      /title="Release this exact terminal direct operation[^>]*>Reset operation<\/button>/,
    );

    state.phase = 'ready';
    state.operation = null;
  });
});
