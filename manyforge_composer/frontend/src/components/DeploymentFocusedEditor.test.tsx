import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { SourceModelProjection } from '../types/api';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { statusFixture } from '../testFixtures/robotControl';

const digest = `sha256:${'1'.repeat(64)}`;

function controller(): DeploymentWorkspaceController {
  return {
    workspace: {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      draft: {
        id: 'deployment-draft-1',
        revision: 1,
        document: {
          api_version: 'manyforge.deployment.v1',
          deployment_artifact_id: 'cell-a',
          deployment_revision: digest,
          cell_id: 'cell-a',
          name: 'Cell A',
          graph: {
            control_bindings: [{ control_binding_id: 'arm_binding', control_binding_revision: digest,
              controllable_component_id: 'robot', command_target_id: 'arm', capability_id: 'execution.trajectory.execute' }],
            runtime_units: [{
              runtime_unit_id: 'robot.native_execution',
              configuration: {
                mode: 'simulated',
                execution_tick_hz: 100,
                collision_monitor_period_ms: 50,
                planner_rng_seed: 12345,
                initial_joint_positions_rad: [0, 1.57],
                direct_control_joint_limits: [],
                gripper_target_configurations: [{
                  controllable_component_id: 'robot',
                  command_target_id: 'gripper',
                  description: 'Native test gripper',
                  actuation_kind: 'position_commanded',
                  open_state_ref: 'open',
                  closed_state_ref: 'closed',
                  command_duration_s: 0.8,
                  touch_links: ['finger_link'],
                  mount_frame: 'tool0',
                  attach_frame: 'tool0',
                  tcp_frame: 'tool0',
                  auxiliary_frames: [],
                }],
                motion_target_limits: [{
                  controllable_component_id: 'robot',
                  command_target_id: 'arm',
                  maximum_tcp_linear_speed_mps: 0.15,
                }],
              },
            }],
          },
          policies: {
            stop_stage_parameters: [{
              control_binding_id: 'arm_binding', control_binding_revision: digest,
              stop_escalation_stage_id: 'motion_controlled_stop',
              provider_contract_ref: { provider_contract_role_id: 'stop_contract' },
              default_contract_parameters: {
                stop_mode: 'soft_on_path',
                deceleration_time_s: 1,
                allow_natural_completion_on_short_path: true,
                jerk_limits_enabled: true,
                per_resource_limits: [{
                  resource_id: 'robot.arm.coordinate.000000',
                  max_deceleration: {
                    coordinate_kind: 'angular_rad_s2',
                    value_rad_s2: 4,
                  },
                  settle_velocity_threshold: {
                    coordinate_kind: 'angular_rad_s',
                    value_rad_s: 0.01,
                  },
                }],
              },
            }],
          },
        },
        deploymentArtifactId: 'cell-a',
        deploymentRevision: digest,
        graphRevision: digest,
        differsFromSaved: false,
      },
      saved: { path: '/workspace/cell-a.deployment.yaml', deploymentRevision: digest },
      managerApplied: { deploymentRevision: digest, scenarioId: 'cell-a' },
      runAdmission: { ready: true, reason: null },
      binding: null,
      scenarioCandidates: [],
      savedScenarioCandidates: [],
      unavailableReasons: [],
    },
    loading: false,
    busy: false,
    error: null,
    feedback: null,
    pendingApplyConfirmation: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
    refresh: vi.fn(),
    clearFeedback: vi.fn(),
    dismissError: vi.fn(),
    open: vi.fn(),
    mutate: vi.fn(),
    updateControlledStop: vi.fn(),
    updateMotionTargetLimit: vi.fn(),
    revert: vi.fn(),
    save: vi.fn(),
    saveAs: vi.fn(),
    discard: vi.fn(),
    applyDraft: vi.fn(),
    confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  };
}

describe('DeploymentFocusedEditor', () => {
  it.each([
    { enabled: false, override: null, imported: null, maximum: '0' },
    { enabled: true, override: 10, imported: 12, maximum: '10' },
    { enabled: true, override: null, imported: 12, maximum: '12' },
    { enabled: true, override: null, imported: null, maximum: null },
  ])('bounds manual jerk from explicit Draft settings: %j', ({ enabled, override, imported, maximum }) => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: Record<string, unknown> }> };
    };
    const configuration = document.graph.runtime_units[0].configuration;
    const targets = configuration.motion_target_limits as Array<Record<string, unknown>>;
    targets[0].ordinary_motion_jerk = {
      enabled, preset_reference_time_s: 2,
      joint_limits: [{ source_joint_name: 'joint1',
        ...(override === null ? {} : { override_rad_s3: override }),
        ...(imported === null ? {} : { imported: { enabled: true, maximum_rad_s3: imported } }),
      }],
    };
    const descriptor = statusFixture().directControl.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.operationalLimit = {
      maximumTcpLinearSpeedMps: 0.15,
    };
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]} gripperDescriptors={[]} />);
    const input = markup.match(/Maximum jerk<\/span>[\s\S]*?(<input[^>]*>)/)?.[1];
    expect(input).toBeDefined();
    if (maximum === null) expect(input).not.toContain('max=');
    else expect(input).toContain(`max="${maximum}"`);
    // An applied preset is never used as an invented Draft ceiling.
    expect(input).not.toContain('disabled='); // clearing remains possible
  });

  it.each(['robot', 'other-robot'])('keeps manual jerk guards on the exact target: %s', (component) => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: Record<string, unknown> }> };
    };
    const configuration = document.graph.runtime_units[0].configuration;
    configuration.direct_control_joint_limits = [{ controllable_component_id: component,
      command_target_id: 'arm', source_joint_name: 'joint1', max_jerk_rad_s3: 8 }];
    const targets = configuration.motion_target_limits as Array<Record<string, unknown>>;
    targets[0].ordinary_motion_jerk = { enabled: true, preset_reference_time_s: 1, joint_limits: [] };
    const descriptor = statusFixture().directControl.targetDescriptor;
    descriptor.controllableComponentId = 'robot';
    descriptor.ordinaryJerk = { enabled: true, presetReferenceTimeS: 1, jointLimits: [{
      sourceJointName: 'joint1', source: 'preset', maximumRadS3: 12, presetRadS3: 12, imported: null,
    }] };
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[descriptor]} gripperDescriptors={[]} />);
    const selector = markup.match(/aria-label="robot \/ arm ordinary motion jerk"[\s\S]*?<\/select>/)?.[0];
    if (component === 'robot') {
      expect(selector).toContain('<option value="off" disabled="">');
      expect(markup).toContain('min="8"');
      expect(markup).toContain("Clear this target&#x27;s Robot Control jerk caps");
    } else {
      expect(selector).toContain('<option value="off">');
      expect(markup).not.toContain('min="8"');
      expect(markup).not.toContain('Clear this target');
    }
  });

  it('selects the exact described target instead of borrowing the first target limits', () => {
    const value = controller();
    const document = value.workspace!.draft.document as unknown as {
      graph: { runtime_units: Array<{ configuration: { motion_target_limits: unknown[] } }> };
    };
    document.graph.runtime_units[0].configuration.motion_target_limits.push({
      controllable_component_id: 'robot-b', command_target_id: 'arm',
      maximum_tcp_linear_speed_mps: 0.25,
    });
    const descriptor = statusFixture().directControl.targetDescriptor;
    descriptor.controllableComponentId = 'robot-b';
    const unrelatedDescriptor = { ...descriptor, controllableComponentId: 'robot-c' };
    const sourceModel = {
      occurrences: [
        { component_id: 'robot-b', model_instance_id: 'unrelated-tool',
          source_robot_name: 'Unrelated tool', source_model_name: 'Wrong model', model_revision: digest },
        { component_id: 'robot-b', model_instance_id: 'selected-arm',
          source_robot_name: 'Selected robot', source_model_name: 'Selected arm', model_revision: digest },
      ],
      targets: [{ component_id: 'robot-b', target_id: 'arm',
        model_groups: [{ model_instance_id: 'selected-arm', source_group_name: 'Selected group' }] }],
    } as SourceModelProjection;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'ready', data: sourceModel }}
      motionDescriptors={[unrelatedDescriptor, descriptor]} gripperDescriptors={[]} />);
    expect(markup).toContain('aria-label="Motion limits target"');
    expect(markup).toMatch(/<option[^>]*selected=""[^>]*>robot-b \/ arm<\/option>/);
    expect(markup).toContain('value="0.25"');
    expect(markup).not.toContain('value="0.15"');
    expect(markup).toContain('aria-label="Source model joint limits"');
    expect(markup).not.toContain('No current motion descriptor for this target.');
    expect(markup).toContain('Selected robot');
    expect(markup).toContain('Selected arm');
    expect(markup).toContain('Selected group');
    expect(markup).not.toContain('Unrelated tool');
    expect(markup).not.toContain('Wrong model');
  });

  it('does not show another component model or joint caps for an undescribed target', () => {
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={controller()}
      sourceModel={{ status: 'loading' }}
      motionDescriptors={[statusFixture().directControl.targetDescriptor]} gripperDescriptors={[]} />);
    expect(markup).toContain('No current motion descriptor for this target.');
    expect(markup).not.toContain('aria-label="Source model joint limits"');
    expect(markup).not.toContain('Robot Control joint caps');
    expect(markup).not.toContain('aria-label="Motion limits target"');
    expect(markup).toContain('value="0.15"');
  });

  it('distinguishes capability bindings and shows saved timing without inventing values for absent settings', () => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      graph: { control_bindings: Array<Record<string, unknown>> };
      policies: { stop_stage_parameters: Array<Record<string, unknown>>; stop_timing_parameters?: unknown[] };
    };
    document.graph.control_bindings.push({ ...document.graph.control_bindings[0],
      control_binding_id: 'arm_contact', capability_id: 'execution.contact.geometric' });
    document.policies.stop_stage_parameters.push({ ...document.policies.stop_stage_parameters[0], control_binding_id: 'arm_contact' });
    const render = () => renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    let markup = render();
    expect(markup).toContain('robot · arm · execution.trajectory.execute');
    expect(markup).toContain('robot · arm · execution.contact.geometric');
    expect(markup).toContain('Using qualified policy timings.');
    expect(markup).not.toContain('value="0.25"');
    document.policies.stop_timing_parameters = [{ control_binding_id: 'arm_binding',
      control_binding_revision: digest, timing: { command_latency_allowance_ns: 250_000_000,
        settling_allowance_ns: 750_000_000, operational_timeout_ns: 3_000_000_000 } }];
    markup = render();
    expect(markup).toContain('Command latency');
    expect(markup).toContain('Settling allowance');
    expect(markup).toContain('Stop timeout');
    expect(markup).toContain('value="0.25"');
    expect(markup).toContain('value="0.75"');
    expect(markup).not.toContain('Using qualified policy timings.');
  });

  it.each([false, true])('shows Spring brake-and-return timing and the saved natural-completion choice (%s)', (natural) => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      policies: { stop_stage_parameters: Array<{ default_contract_parameters: Record<string, unknown> }> };
    };
    const profile = document.policies.stop_stage_parameters[0].default_contract_parameters;
    profile.stop_mode = 'spring_back_hold';
    profile.allow_natural_completion_on_short_path = natural;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    expect(markup).toContain('Brake + return time');
    expect(markup).toContain('Allow natural completion');
    expect(markup).toContain('Off by default: brake and return');
    expect(markup).toMatch(natural ? /type="checkbox"[^>]*checked/ : /type="checkbox"(?![^>]*checked)[^>]*>/);
  });

  it('keeps an older unreviewed Stop contract read-only', () => {
    const value = controller();
    const document = value.workspace!.draft!.document as unknown as {
      policies: { stop_stage_parameters: Array<{ default_contract_parameters: Record<string, unknown> }> };
    };
    delete document.policies.stop_stage_parameters[0].default_contract_parameters.jerk_limits_enabled;
    const markup = renderToStaticMarkup(<DeploymentFocusedEditor controller={value}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />);
    expect(markup).toContain('older Stop contract needs explicit migration');
    expect(markup).toMatch(/<select[^>]+aria-label="Controlled Stop jerk limits"[^>]+disabled/);
    expect(markup).toMatch(/<button[^>]+disabled[^>]*>Update Controlled Stop Draft/);
  });

  it('presents canonical target, native runtime, and normalized Stop inputs', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={controller()}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[{
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v5',
          ordinaryJerk: { enabled: false, presetReferenceTimeS: 1, jointLimits: [{
            sourceJointName: 'joint1', source: 'off', maximumRadS3: null,
            presetRadS3: 12, imported: null,
          }] },
          controllableComponentId: 'robot',
          commandTargetId: 'arm',
          componentRoleId: 'robot',
          targetRoleId: 'arm',
          frames: [],
          joints: [{
            sourceJointName: 'joint1', compiledJointName: 'robot__joint1', unit: 'rad',
            minPositionRad: -1, maxPositionRad: 1, maxVelocityRadS: 2,
            maxAccelerationRadS2: 3,
          }],
          operationalJointLimits: [{
            sourceJointName: 'joint1', modelMaxVelocityRadS: 2,
            modelMaxAccelerationRadS2: 3, deploymentMaxVelocityRadS: null,
            deploymentMaxAccelerationRadS2: null, deploymentMaxJerkRadS3: null,
            effectiveMaxVelocityRadS: 2, effectiveMaxAccelerationRadS2: 3,
            effectiveMaxJerkRadS3: null,
          }],
          operationalLimit: {
            maximumTcpLinearSpeedMps: 0.15,
          },
          contracts: {
            operationCapabilityId: 'execution.trajectory.execute',
            operationCapabilityDefinitionRevision: digest,
            commandSchema: null, controlPath: null, supportingProviders: [],
          },
        }]}
        gripperDescriptors={[]}
      />,
    );

    expect(markup).toContain('Robot model and target limits');
    expect(markup).toContain('Maximum TCP linear speed');
    expect(markup).toContain('value="0.15"');
    expect(markup).toContain('Native runtime configuration');
    expect(markup).toContain('Execution cadence');
    expect(markup).toContain('Initial joint state');
    expect(markup).toContain('Robot Control joint caps');
    expect(markup).toMatch(/Maximum velocity<\/span>[\s\S]*?<input[^>]*max="2"/);
    expect(markup).toMatch(/Maximum acceleration<\/span>[\s\S]*?<input[^>]*max="3"/);
    expect(markup).toContain('Ordinary motion jerk');
    expect(markup).toContain('Preset reference time');
    expect(markup).toContain('Applied: off');
    expect(markup).toContain('Preset: 12');
    expect(markup).toContain('Maximum acceleration');
    expect(markup).toContain('Controlled Stop');
    expect(markup).toContain('Controlled Stop jerk limits');
    expect(markup).toContain('Joint jerk limits');
    expect(markup).toContain('ManyForge preset');
    expect(markup).toContain('Soft on path');
    expect(markup).toContain('Update Controlled Stop Draft');
    expect(markup).toContain('No current addressed gripper descriptor is available.');
  });

  it('joins one authored gripper row to exact owner semantics', () => {
    const markup = renderToStaticMarkup(
      <DeploymentFocusedEditor
        controller={controller()}
        sourceModel={{ status: 'loading' }}
        motionDescriptors={[]}
        gripperDescriptors={[{
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v2',
          controllableComponentId: 'robot', commandTargetId: 'gripper',
          componentRoleId: 'robot', targetRoleId: 'gripper',
          description: 'Native test gripper', commandKind: 'gripper_position',
          actuationKind: 'position_commanded', transportKind: 'resolved_binding',
          controlInterfaceContractId: 'native.gripper',
          stateAuthorityId: 'native.predicted', stateAuthorityClass: 'predicted',
          completionPolicy: 'goal_fenced_target_reached',
          graspConfirmationPolicy: 'binding_feedback_required',
          sourceGroupName: 'gripper', sourceCoordinateName: 'finger_joint',
          sourceKind: 'model_role', coordinateKind: 'linear_m', unit: 'm',
          minPosition: 0, maxPosition: 0.04,
          openStateRef: 'open', closedStateRef: 'closed', commandDurationS: 0.8,
          touchLinks: ['finger_link'], mountFrame: 'tool0', attachFrame: 'tool0',
          tcpFrame: 'tool0', auxiliaryFrames: [], mimicFollowers: [],
          namedPresets: [
            { name: 'closed', masterPosition: 0, coordinates: [] },
            { name: 'open', masterPosition: 0.04, coordinates: [] },
          ],
          contracts: {
            operationCapabilityId: 'tool.gripper.command',
            operationCapabilityDefinitionRevision: digest,
            commandSchema: { commandSchemaId: 'gripper.command_position', commandSchemaRevision: digest },
            controlPath: null, supportingProviders: [],
          },
        }]}
      />,
    );

    expect(markup).toContain('Deployment-owned tool inputs');
    expect(markup).toContain('Native test gripper');
    expect(markup).toContain('goal fenced target reached');
    expect(markup).toContain('binding feedback required');
    expect(markup).toContain('Update gripper Draft');
    expect(markup).toContain('finger_link');
  });
});
