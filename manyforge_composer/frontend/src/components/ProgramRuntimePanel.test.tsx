import { readFileSync } from 'node:fs';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import { PROGRAM_RESET_READINESS_MESSAGE } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { CycleRecoveryOffer, ProgramRunStateName } from '../types/api';
import type {
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from '../types/robotControl';
import { statusFixture } from '../testFixtures/robotControl';
import { ACTIVE_CONFIGURATION, alarmSnapshot, gatingMember } from '../testFixtures/operatorAlarms';
import { motionAttentionBlock } from '../state/operatorAlarmMotionAttention';
import type { AlarmMotionAttentionMember } from '../types/headless';
import { executionGroupScopes, executionGroupTargetLabels } from '../state/programExecutionGroups';
import {
  deriveTransportGuidance,
  MotionControlSettings,
  ProgramRuntimePanel,
  ProgramTransportControls,
} from './ProgramRuntimePanel';
import { readStyleSource } from '../styles/readStyleSource';

function ordinaryMotionStopOffer(): CycleRecoveryOffer {
  return {
    schemaVersion: 'manyforge.cycle_recovery_offer.v4',
    offerId: `sha256:${'e'.repeat(64)}`,
    offerRevision: 1,
    generatedAtNs: 1,
    validUntilNs: null,
    kernelAssessmentRevision: 1,
    controlCommandEpoch: 1,
    operationId: 'operation.1',
    operationRevision: 1,
    programRunId: 'program-run.1',
    programBindingId: `sha256:${'c'.repeat(64)}`,
    nodeInstanceId: 'root',
    nodeAttemptId: 'attempt.1',
    participants: [],
    graphRevision: `sha256:${'b'.repeat(64)}`,
    retainedBundleId: `sha256:${'a'.repeat(64)}`,
    cycleGeneration: 1,
    programGeneration: 1,
    deploymentGeneration: 1,
    sceneAdmissionEpoch: 1,
    poseSourceApplyGeneration: 1,
    latchId: null,
    stopId: 'stop.1',
    stopGeneration: 1,
    stopCertificateId: 'stop-certificate.1',
    stopCertificateRevision: 1,
    status: 'retry_ready',
    recoveryClass: 'ordinary_operator_stop',
    actions: [{
      actionId: 'replan_from_stopped_state',
      labelKey: 'recovery.replan_from_stopped_state',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: true,
      disabledReasonCode: null,
    }, {
      actionId: 'reset_cycle',
      labelKey: 'recovery.reset_cycle',
      requiresConfirmation: true,
      performsBackendReset: true,
      enabled: true,
      disabledReasonCode: null,
    }],
    manualResult: {
      enabled: false,
      disabledReasonCode: 'manual_result_not_available',
    },
    primaryAction: 'replan_from_stopped_state',
    reason: {
      code: 'ordinary_operator_stop',
      message: 'Stopped by the operator.',
      details: null,
      origin: null,
    },
    evidenceRefs: [],
    policy: { allowIntervention: false, maxRetries: 0 },
    retriesUsed: 0,
  };
}

function ordinaryMotionStopControls(): NonNullable<
  ProgramRuntimeController['state']['controls']
> {
  return {
    ownerEpoch: 'program-runner.epoch.1',
    programRunId: 'program-run.1',
    recoveryOffer: ordinaryMotionStopOffer(),
    executionSelections: {
      selectionRevision: 1,
      groups: [{ groupId: 'all', nodeCount: 1, heldNodeCount: 1,
        state: 'held', stopAllowed: false,
        startAllowed: false, startUnavailableReason: 'selection_not_ready',
        resumeAllowed: true, resumeUnavailableReason: null,
        runtimeScaleSelector: {
          kind: 'singular',
          programBindingId: `sha256:${'c'.repeat(64)}`,
          componentRoleId: 'robot',
          targetRoleId: 'arm',
        },
        runtimeScaleUnavailableReason: null,
        resourceIds: ['robot.joint_1'] }],
    },
  } as unknown as NonNullable<ProgramRuntimeController['state']['controls']>;
}

function robotController(): RobotControlController {
  return {
    currentStatus: statusFixture(),
    manualControlAllowed: true,
    manualControlUnavailableReason: null,
    state: {
      phase: 'ready',
      operation: null,
      status: null,
    },
    controlPending: null,
    runtimeScaleStatus: null,
    runtimeScaleError: null,
    runtimeScaleDraft: 1,
    runtimeScalePending: false,
    fullStopError: null,
    stopFaultRecoveryPending: false,
    stopFaultRecoveryError: null,
    componentIncidents: [],
    componentIncidentsUnavailableReason: null,
    resume: vi.fn(),
    recoverStopFault: vi.fn(),
    setRuntimeScale: vi.fn(),
    commitRuntimeScale: vi.fn(),
  } as unknown as RobotControlController;
}

function runtimeScaleStatus(
  selector: RuntimeScaleControlSelector,
  requestedScale = 0.5,
): RuntimeScaleControlStatus {
  return {
    selector,
    controlGeneration: 2,
    requestedScale,
    supported: true,
    frozen: false,
    controlDomainId: 'domain-1',
    controlDomainRevision: `sha256:${'d'.repeat(64)}`,
    controlBindingId: 'binding-1',
    controlBindingRevision: `sha256:${'e'.repeat(64)}`,
    scalingDomainId: 'scaling-1',
    activeControlConfigurationId: 'active-1',
    activeControlConfigurationGeneration: 1,
    activeOperationId: null,
    activeOperationRevision: null,
    activeOperationScaleGeneration: null,
    effectiveScale: null,
    applicationState: null,
    reason: null,
  };
}

function controller(
  state: ProgramRunStateName = 'running',
  cycleState: ProgramRuntimeController['cycleState'] =
    state === 'succeeded' ? 'terminal_awaiting_reset' : 'running',
): ProgramRuntimeController {
  return {
    commandAllowed: vi.fn(() => true),
    commandDisabledReason: vi.fn(() => null),
    motionAttentionBlock: vi.fn(() => null),
    draftUnappliedReason: null,
    state: {
      currentScenario: null,
      applied: {
        schemaVersion: 'manyforge.applied_program_selection.v3',
        scenarioId: 'cell-pick',
        sourceDocumentDigest: `sha256:${'a'.repeat(64)}`,
        programId: 'next-pick',
        programRevision: `sha256:${'b'.repeat(64)}`,
        programBindingId: `sha256:${'c'.repeat(64)}`,
        retainedBundleId: `sha256:${'a'.repeat(64)}`,
        graphRevision: `sha256:${'b'.repeat(64)}`,
        nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
        recordDigest: `sha256:${'a'.repeat(64)}`,
        sourceOwnerId: 'headless.program-apply',
        sourceOwnerEpoch: 'headless.epoch.1',
        sourceGeneration: 1,
        assistantProjection: null,
        ownerIdentities: [{
          ownerRole: 'headless_projection',
          ownerId: 'headless.program-apply',
          ownerEpoch: 'headless.epoch.1',
        }],
        program: {
          api_version: 'manyforge.program.v2',
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          program_profile: 'composable-cell-v1',
          name: 'Next pick',
          catalog: {
            api_version: 'manyforge.node_catalog.v2',
            content_version: '2.4.0',
          },
          component_roles: { component: {} },
          target_roles: {},
          tree: {
            node_id: 'logic.check_condition',
            instance_id: 'root',
            parameters: { condition: true },
            children: [],
          },
        },
        programBinding: {
          api_version: 'manyforge.program_binding.v2',
          program_binding_id: `sha256:${'c'.repeat(64)}`,
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          retained_bundle_id: `sha256:${'a'.repeat(64)}`,
          graph_revision: `sha256:${'b'.repeat(64)}`,
          node_environment: {
            catalog_id: 'manyforge.default',
            content_version: '2.4.0',
            catalog_digest: `sha256:${'a'.repeat(64)}`,
            environment_digest: `sha256:${'c'.repeat(64)}`,
          },
          component_role_bindings: [],
          target_role_bindings: [],
          node_policies: [],
          contact_grasp_bindings: [],
          provenance: {
            binding_compiler_contract_digest: `sha256:${'a'.repeat(64)}`,
            role_assignment_digest: `sha256:${'c'.repeat(64)}`,
            source_records: [],
            resolved_output_records: [],
          },
        },
      },
      run: {
        schemaVersion: 'manyforge.program_run_status.v3',
        ownerId: 'program-runner.cell-a',
        ownerEpoch: 'program-runner.epoch.1',
        runRevision: 4,
        state,
        capture: {
          programRunId: 'program-run.1',
          appliedRecordDigest: `sha256:${'d'.repeat(64)}`,
          scenarioId: 'cell-pick',
          sourceDocumentDigest: `sha256:${'a'.repeat(64)}`,
          programId: 'pick',
          programRevision: `sha256:${'b'.repeat(64)}`,
          programBindingId: `sha256:${'c'.repeat(64)}`,
          retainedBundleId: `sha256:${'a'.repeat(64)}`,
          graphRevision: `sha256:${'b'.repeat(64)}`,
          nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
          nodePolicies: [],
          nodePolicyDigest: 'sha256:4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945',
          ownerIdentities: [{
            ownerRole: 'program_runner',
            ownerId: 'program-runner.cell-a',
            ownerEpoch: 'program-runner.epoch.1',
          }],
        },
        treeStatus: 'running',
        nodeStatusRevision: 2,
        nodeStatuses: { root: 'running' },
        activeOperationCount: 1,
        reason: null,
        message: '',
      },
      controls: null,
      nonExecutable: null,
      runnerAvailable: true,
      loading: false,
      observationError: null,
      pending: null,
      selectionPending: [],
      controlPending: null,
      error: null,
    },
    cycleState,
    cycleFailure: null,
    active: [
      'running', 'intervention_required', 'full_stopping', 'full_stopped',
      'stopping', 'finalizing',
    ]
      .includes(state),
    refresh: vi.fn(),
    dismissError: vi.fn(),
    run: vi.fn(),
    stopMotion: vi.fn(),
    startMotion: vi.fn(),
    resumeMotion: vi.fn(),
    fullStop: vi.fn(),
    resumeFullStop: vi.fn(),
    reset: vi.fn(),
    applyRecoveryAction: vi.fn(),
    writeRuntimeBlackboard: vi.fn(),
    persistentValues: { read: vi.fn(), edit: vi.fn(), clear: vi.fn() },
    stageManualResult: vi.fn(),
    groupRuntimeScales: {},
    setGroupRuntimeScale: vi.fn(),
    commitGroupRuntimeScale: vi.fn(),
  };
}

describe('Program runtime projection', () => {
  it('shows the genuine origin of a no-Program publication', () => {
    const view = controller();
    view.state.applied = null;
    view.state.run = null;
    view.state.nonExecutable = {
      deploymentArtifactId: 'deployment',
      deploymentRevision: `sha256:${'a'.repeat(64)}`,
      executable: false,
      programId: null,
      programRevision: null,
      origin: {
        kind: 'sealed_canonical_empty',
        cellId: 'empty-cell',
        deploymentArtifactId: 'deployment',
        deploymentRevision: `sha256:${'a'.repeat(64)}`,
        retainedBundleId: `sha256:${'b'.repeat(64)}`,
        graphRevision: `sha256:${'c'.repeat(64)}`,
        admissionDigest: `sha256:${'d'.repeat(64)}`,
      },
      message: null,
      unresolvedReferences: [],
      affectedNodes: [],
      affectedExecutionGroups: [],
    };
    const sealed = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={view} />);
    expect(sealed).toContain('empty-cell · no Program');
    expect(sealed).not.toContain('cell-pick');

    view.state.nonExecutable.origin = {
      kind: 'scenario',
      scenarioId: 'authored-cell',
      sourceDocumentDigest: `sha256:${'e'.repeat(64)}`,
    };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={view} />))
      .toContain('authored-cell · no Program');
  });

  it('shows exact owner status without duplicating the standard commands', () => {
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );

    expect(markup).toContain('Program execution');
    expect(markup).toContain('program-run.1');
    expect(markup).toContain('revision 4');
    expect(markup).toContain('next-pick');
    expect(markup).not.toContain('Selection for the next Run');
    expect(markup).toContain('Program in this run');
    expect(markup).toContain('fixed for this run');
    // Execute, Halt and Reset live in the Program panel; Program Stop is gone.
    expect(markup).not.toContain('Program Stop');
    expect(markup).not.toContain('Execute Applied Program');
    expect(markup).not.toContain('Reset Program cycle');
    // Advanced tools exist only with run controls; no empty section otherwise.
    expect(markup).not.toContain('Advanced: runtime blackboard');
    expect(markup).not.toContain('Runtime overrides');
  });

  it('offers typed runtime writes and shows the backend reason when they are refused', () => {
    const halted = controller('full_stopped');
    halted.state.applied!.program.blackboard = {
      namespaces: [],
      keys: [
        { key: 'flag', type: 'bool', access: 'read_write', persistence: 'run' },
        { key: 'layer', type: 'int64', access: 'read_write', persistence: 'persistent', initial: 0 },
        { key: 'limit', type: 'float64', access: 'read', persistence: 'run' },
        { key: 'reply', type: 'model_response', access: 'write', persistence: 'run' },
      ],
    };
    halted.commandAllowed = vi.fn((kind: string) => kind !== 'program_runtime_blackboard_write');
    halted.commandDisabledReason = vi.fn(() => 'program_run_state_full_stopped');
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={halted} />,
    );

    expect(markup).toContain('Advanced: runtime blackboard, stored values and audit');
    // Only writable keys with a typed editor; no raw JSON entry remains.
    expect(markup).toContain('<option value="flag">flag · bool</option>');
    expect(markup).toContain('<option value="layer">layer · int64</option>');
    expect(markup).not.toContain('limit · float64');
    expect(markup).not.toContain('reply · model_response');
    expect(markup).toContain('Writes are refused now: program run state full stopped.');
    expect(markup).toMatch(/<button type="button" disabled="">Write current-run value<\/button>/);
    const source = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    expect(source).not.toContain('parseRuntimeValue');
    expect(source).toContain('<PersistentKeyValuesSection');
  });

  it('never turns an unavailable or unknown owner fact into idle or zero work', () => {
    const unavailable = controller();
    unavailable.state.run = null;
    unavailable.state.runnerAvailable = false;
    const unavailableMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unavailable} />,
    );
    expect(unavailableMarkup).toContain('unavailable');
    expect(unavailableMarkup).toContain('execution state is unknown');
    expect(unavailableMarkup).toContain('this never proves that robot work stopped');
    expect(unavailableMarkup).toContain('<span>Motion</span><strong>Unknown</strong>');

    // A run without a motion projection is Unknown, never "not moving".
    const unknownMotion = controller();
    unknownMotion.state.controls = null;
    const unknownMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unknownMotion} />,
    );
    expect(unknownMarkup).toContain('<span>Motion</span><strong>Unknown</strong>');
  });

  it('shows the captured retry budget, with zero as Unlimited, and where to correct', () => {
    const held = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = {
      ...controls.recoveryOffer!,
      recoveryClass: 'node_failure_pause',
      nodeInstanceId: 'move',
      policy: { allowIntervention: true, maxRetries: 0 },
      retriesUsed: 300,
    };
    held.state.controls = controls;
    const unlimited = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={held} />);
    expect(unlimited).toContain('Retry limit Unlimited · 300 used');
    expect(unlimited).toContain('change a runtime value for this run under Advanced');
    expect(unlimited).toContain('Apply then Resets the run');
    expect(unlimited).not.toContain('Retry limit 0');
    controls.recoveryOffer = {
      ...controls.recoveryOffer!,
      policy: { allowIntervention: true, maxRetries: 3 },
      retriesUsed: 1,
    };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={held} />))
      .toContain('Retry limit 3 · 1 used');
  });

  it('distinguishes ordinary Motion Stop from whole-Program Stop', () => {
    const motionStopped = controller('running');
    motionStopped.state.controls = ordinaryMotionStopControls();
    const motionMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={motionStopped} />,
    );
    expect(motionMarkup).toContain('<strong>Stopped</strong>');

    const settledMotionStop = controller('intervention_required');
    settledMotionStop.state.controls = ordinaryMotionStopControls();
    settledMotionStop.state.run!.reason = {
      code: 'operation_stopped',
      message: 'native operation was interrupted',
      details: null,
      origin: null,
    };
    settledMotionStop.state.run!.message = 'native operation was interrupted';
    const settledMotionMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={settledMotionStop} />,
    );
    expect(settledMotionMarkup).toContain('<strong>Stopped</strong>');
    expect(settledMotionMarkup).toContain('Program motion stopped and held.');
    expect(settledMotionMarkup).not.toContain('native operation was interrupted');

    const executionStopped = renderToStaticMarkup(
      <ProgramRuntimePanel
        enabled
        controller={controller('stopped', 'terminal_awaiting_reset')}
      />,
    );
    expect(executionStopped).toContain('<strong>Stopped</strong>');

    const fullStopped = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller('full_stopped')} />,
    );
    expect(fullStopped).toContain('<strong>Stopped</strong>');

    const executionStoppedWithStaleOffer = controller(
      'stopped',
      'terminal_awaiting_reset',
    );
    executionStoppedWithStaleOffer.state.controls = ordinaryMotionStopControls();
    const staleOfferMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={executionStoppedWithStaleOffer} />,
    );
    expect(staleOfferMarkup).toContain('<strong>Stopped</strong>');
  });

  it('shows a run failure with its labelled details and failed node', () => {
    const failed = controller('failed', 'terminal_awaiting_reset');
    failed.state.run!.reason = {
      code: 'node_input_unavailable',
      message: "missing input duration_s from blackboard_key 'delay'",
      details: {
        input_problem: 'missing',
        path: 'duration_s',
        source_kind: 'blackboard_key',
        source_reference: 'delay',
      },
      origin: { kind: 'program_node', nodeInstanceId: 'wait', ownerApi: null },
    };
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={failed} />,
    );
    expect(markup).toContain(
      'node input unavailable: missing input duration_s from blackboard_key &#x27;delay&#x27;',
    );
    expect(markup).toContain(
      '<small> · Input: duration_s from blackboard key &#x27;delay&#x27; (missing)</small>',
    );
    expect(markup).toContain('<small> · Origin: node &#x27;wait&#x27;</small>');
  });

  it('shows a retained failed run that carries no details or origin', () => {
    const failed = controller('failed', 'terminal_awaiting_reset');
    failed.state.run!.reason = {
      code: 'program_tree_failed',
      message: 'Program failed.',
      details: null,
      origin: null,
    };
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={failed} />,
    );
    expect(markup).toContain('<span>program tree failed: Program failed.</span>');
    expect(markup).not.toContain('Origin:');
  });

  it('edits displayed groups in the Draft and applies them to the Motion panel after Apply', () => {
    const changes: string[][] = [];
    const scopes = executionGroupScopes([{
      node_id: 'parallel', instance_id: 'root', parameters: {}, children: [
        { node_id: 'motion.move_to_pose', instance_id: 'a', parameters: { target_role: 'arm' }, execution_groups: ['robot_a'], children: [] },
        { node_id: 'motion.move_to_pose', instance_id: 'b', parameters: {}, execution_groups: ['robot_b'], children: [] },
      ],
    }], ['robot_a', 'robot_b']);
    const settings = renderToStaticMarkup(
      <MotionControlSettings groups={{
        scopes,
        targetLabels: { arm: 'arm (component role: robot)' },
        hidden: undefined,
        pendingApply: true,
        onChange: (hidden) => changes.push(hidden),
      }} />,
    );
    expect(settings).toMatch(/<span>Displayed groups<\/span><span class="settings-scope" data-scope="program"[^>]*>Program<\/span>/);
    expect(settings).toContain('Changed in the Draft, not applied yet.');
    expect(settings).toMatch(/<span>Layout and confirmations<\/span><span class="settings-scope" data-scope="installation"[^>]*>Installation<\/span>/);
    expect(settings).toContain('Confirm before halting the Program');
    // Default: named groups shown, "all" not.
    expect(settings).toMatch(/<input type="checkbox"\/><span title="all">All groups/);
    expect(settings).toContain('Show All, or another covering group, before hiding this group.');
    expect(settings).toContain('disabled="" checked=""/><span title="robot_a"');
    expect(settings).toContain('arm (component role: robot)');

    const readOnly = renderToStaticMarkup(
      <MotionControlSettings groups={{
        scopes, hidden: ['robot_a'], pendingApply: false,
      }} />,
    );
    expect(readOnly).toContain('The Program Draft cannot be edited right now.');
    expect(readOnly).toMatch(/<fieldset aria-label="Displayed groups" disabled="">/);

    // The Applied Program's choice drives the Motion panel.
    const running = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    controls.executionSelections.groups = [
      group,
      { ...group, groupId: 'robot_a' },
      { ...group, groupId: 'robot_b' },
    ];
    running.state.controls = controls;
    running.state.applied!.program.viewer = { hidden_execution_groups: ['all', 'robot_b'] };
    const transport = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(transport).toContain('robot_a controls');
    expect(transport).not.toContain('robot_b controls');
    expect(transport).toMatch(/aria-label="Open viewer settings; 1 command groups hidden"/);
    running.state.applied!.program.tree = {
      node_id: 'motion.move_to_pose', instance_id: 'new-move', parameters: {}, children: [],
    };
    const uncovered = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(uncovered).toContain('all controls');
    expect(uncovered).toContain('leave some move, gripper or I/O nodes without a visible control');
    expect(uncovered).not.toContain('membership is unavailable');
    expect(running.state.applied!.program.viewer.hidden_execution_groups).toEqual(['all', 'robot_b']);
    expect(changes).toEqual([]);
  });

  it('keeps All visible, and says why, when the run was captured from another revision', () => {
    const running = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    controls.executionSelections.groups = [group, { ...group, groupId: 'robot_a' }];
    running.state.controls = controls;
    const applied = running.state.applied!;
    applied.program.viewer = { hidden_execution_groups: ['all'] };
    applied.program.execution_groups = { robot_a: { execution_policy: 'independent' } };
    applied.program.target_roles = { arm: { component_role: 'robot' } };
    applied.program.tree = {
      node_id: 'motion.move_to_pose', instance_id: 'a', parameters: { target_role: 'arm' },
      execution_groups: ['robot_a'], children: [],
    };
    applied.programBinding.target_role_bindings = [{
      target_role_id: 'arm', component_role_id: 'robot',
      controllable_component_id: 'ur10e', command_target_id: 'arm',
      frame_role_bindings: [], resource_role_bindings: [],
      collision_body_role_bindings: [], capability_bindings: [],
    }];
    const render = () => renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    const matched = render();
    expect(matched).toContain('robot_a controls');
    expect(matched).not.toContain('all controls');
    // Robot names only from the Applied binding of the captured revision.
    expect(matched).toContain('title="robot_a: arm (ur10e)"');

    running.state.run!.capture.programRevision = `sha256:${'9'.repeat(64)}`;
    const mismatched = render();
    expect(mismatched).toContain('all controls');
    expect(mismatched).toContain('robot_a controls');
    expect(mismatched).toContain('group membership is unavailable');
    expect(mismatched).toContain('different Program revision than the Applied Program');
    expect(mismatched).not.toContain('ur10e');
    expect(applied.program.viewer.hidden_execution_groups).toEqual(['all']);
  });

  it('labels Draft targets with component roles unless an authoritative binding names the robot', () => {
    const scopes = executionGroupScopes([{
      node_id: 'gripper.command_position', instance_id: 'g', parameters: { target_role: 'gripper' },
      execution_groups: ['tool'], children: [],
    }], ['tool']);
    const targetRoles = { gripper: { component_role: 'hand' } };
    const draft = renderToStaticMarkup(<MotionControlSettings groups={{
      scopes, targetLabels: executionGroupTargetLabels(targetRoles, null), hidden: undefined, pendingApply: false,
    }} />);
    expect(draft).toContain('1 hardware node · gripper (component role: hand)');
    const binding = controller().state.applied!.programBinding;
    binding.target_role_bindings = [{
      target_role_id: 'gripper', component_role_id: 'hand',
      controllable_component_id: 'robotiq', command_target_id: 'gripper',
      frame_role_bindings: [], resource_role_bindings: [],
      collision_body_role_bindings: [], capability_bindings: [],
    }];
    const applied = renderToStaticMarkup(<MotionControlSettings groups={{
      scopes, targetLabels: executionGroupTargetLabels(targetRoles, binding), hidden: undefined, pendingApply: false,
    }} />);
    expect(applied).toContain('gripper (robotiq)');
  });

  it('shows Retry during a failure pause, disabled until containment is confirmed', () => {
    const paused = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    const offer = controls.recoveryOffer!;
    offer.recoveryClass = 'operation_failure' as typeof offer.recoveryClass;
    offer.nodeInstanceId = 'move.1';
    offer.actions = [{
      actionId: 'retry_motion_from_current_state',
      labelKey: 'recovery.retry_motion_from_current_state',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: false,
      disabledReasonCode: 'failure_pause_containment_unconfirmed',
    }];
    paused.state.controls = controls;
    const render = () => renderToStaticMarkup(
      <ProgramTransportControls enabled program={paused} robot={robotController()} />,
    );

    // Resume must not bypass the pause while Retry is unavailable.
    expect(render()).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="Retry waits until stopping the Program&#x27;s other motion is confirmed."[^>]*>Retry<\/button>/,
    );
    offer.actions[0].enabled = true;
    offer.actions[0].disabledReasonCode = null;
    expect(render()).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*title="Retry move.1 and let all the Program&#x27;s motion continue."[^>]*>Retry<\/button>/,
    );
  });

  it('disables Resume and a motion-producing Retry from operator-alarm attention, naming the remedy', () => {
    const withAlarms = (
      program: ProgramRuntimeController,
      members: AlarmMotionAttentionMember[],
    ): ProgramRuntimeController => ({
      ...program,
      motionAttentionBlock: (resourceIds) => motionAttentionBlock(
        alarmSnapshot(members), resourceIds, ACTIVE_CONFIGURATION,
      ),
    });
    const stopped = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    stopped.state.controls = controls;
    const render = (program: ProgramRuntimeController) => renderToStaticMarkup(
      <ProgramTransportControls enabled program={program} robot={robotController()} />,
    );

    // Another robot's alarm leaves this selection's Resume available.
    expect(render(withAlarms(stopped, [
      gatingMember('other', 'active_unacknowledged', ['robot.2.joint_1']),
    ]))).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
    // An unacknowledged alarm on the selection's resources: acknowledge.
    expect(render(withAlarms(stopped, [
      gatingMember('arm', 'active_unacknowledged', ['robot.joint_1']),
    ]))).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="Motion is blocked by unacknowledged alarms. Acknowledge them, then try again."[^>]*>Resume<\/button>/,
    );
    // A cell-global condition interlock: resolve the cause.
    expect(render(withAlarms(stopped, [gatingMember('scope', 'active_acknowledged', [])]))).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm does not release it."[^>]*>Resume<\/button>/,
    );

    // Retry is tested against the held operation's exact resources.
    const paused = controller('intervention_required');
    const failed = ordinaryMotionStopControls();
    const offer = failed.recoveryOffer!;
    offer.recoveryClass = 'operation_failure' as typeof offer.recoveryClass;
    offer.nodeInstanceId = 'move.1';
    offer.participants = [{ operation_participant_id: 'p1', resource_ids: ['robot.joint_1'] }];
    offer.actions = [{
      actionId: 'retry_motion_from_current_state',
      labelKey: 'recovery.retry_motion_from_current_state',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: true,
      disabledReasonCode: null,
    }];
    paused.state.controls = failed;
    expect(render(withAlarms(paused, [
      gatingMember('arm', 'returned_unacknowledged', ['robot.joint_1']),
    ]))).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="Motion is blocked by unacknowledged alarms. Acknowledge them, then try again."[^>]*>Retry<\/button>/,
    );
    expect(render(withAlarms(paused, [
      gatingMember('other', 'returned_unacknowledged', ['robot.2.joint_1']),
    ]))).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Retry<\/button>/);
  });

  it('labels a group with stopped and never-started work Resume', () => {
    const mixed = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    controls.executionSelections.groups[0].state = 'mixed';
    mixed.state.controls = controls;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={mixed} robot={robotController()} />,
    );
    expect(markup).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
  });

  it('offers Resume and Reset, but not another Stop, after ordinary Motion Stop', () => {
    const motionStopped = controller('intervention_required');
    motionStopped.state.controls = ordinaryMotionStopControls();

    const markup = renderToStaticMarkup(
      <ProgramTransportControls
        enabled
        program={motionStopped}
        robot={robotController()}
      />,
    );

    expect(markup).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/,
    );
    expect(markup).toMatch(/class="transport-reset transport-unique-control meaningful"(?![^>]*disabled)[^>]*>Reset<\/button>/);
  });

  it('confirms Reset during execution only after all motion has stopped', () => {
    const motionStopped = controller('intervention_required');
    motionStopped.state.controls = ordinaryMotionStopControls();
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={motionStopped} robot={robotController()} />,
    );
    // While executing, Reset asks first instead of resetting at once.
    expect(markup).toMatch(
      /class="transport-reset[^"]*"[^>]*title="Stop all Program motion, then confirm ending this run and resetting the Program\."[^>]*>Reset<\/button>/,
    );
    const source = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    const flow = source.slice(
      source.indexOf('const requestProgramReset'),
      source.indexOf('const requestProgramHalt'),
    );
    // Not executing: reset at once. Executing: stop motion, then ask.
    expect(flow).toMatch(/if \(!executionActive\) \{\s*void program\.reset\(\);/);
    expect(flow).toContain("setResetIntent(preferences.confirmReset ? 'stopping' : 'automatic');");
    expect(flow).toContain("await program.stopMotion(['all'])");
    // Halted: motion is already contained, so no refused motion Stop is sent.
    expect(source).toContain('allGroup === undefined || allMotionQuiesced || programFullStopped');
    expect(source).toContain('state.runnerAvailable && executionSelectionIsQuiescent(allGroup)');
    expect(flow).toContain('if (programFullStopping) return;');
    expect(source).toContain(
      "if (resetIntent === 'stopping' && motionSettled) setResetIntent('confirm');",
    );
    expect(source).toContain('open={resetIntent === \'confirm\'}');
  });

  it('projects Execute as active while initial motion is Ready for deliberate Run', () => {
    const executing = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    group.state = 'ready';
    group.stopAllowed = false;
    group.startAllowed = true;
    group.startUnavailableReason = null;
    // Run and Resume are one command; the owner projects them equal.
    group.resumeAllowed = true;
    group.resumeUnavailableReason = null;
    executing.state.controls = controls;
    const selector = group.runtimeScaleSelector!;
    const status = runtimeScaleStatus(selector, 0.4);
    executing.groupRuntimeScales = {
      all: { selector, status, draft: 0.4, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={executing} robot={robotController()} />,
    );

    expect(markup).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*>Run<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/,
    );
    expect(markup).toMatch(/type="range"[^>]*value="0.4"/);
    expect(markup).not.toMatch(/type="range"[^>]*disabled=/);
    expect(markup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-full-stop transport-unique-control"(?![^>]*disabled)[^>]*>Halt<\/button>/,
    );
  });

  it('keeps Halt disabled until a Program is executing', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    expect(markup).toMatch(
      /class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*>Execute<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-full-stop transport-unique-control"[^>]*disabled=""[^>]*>Halt<\/button>/,
    );
  });

  it('keeps paired commands disabled through Execute and selected-Stop transitions', () => {
    const starting = controller('succeeded', 'idle_after_reset');
    starting.state.pending = 'run';
    const startingMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={starting} robot={robotController()} />,
    );
    expect(startingMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );
    expect(startingMarkup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stop<\/button>/,
    );

    const running = controller('running');
    const runningMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(runningMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );

    running.state.selectionPending = [{
      kind: 'motion-stop',
      groupIds: ['all'],
      requestId: 'stop-request.1',
      ownerEpoch: 'program-runner.epoch.1',
      programRunId: 'program-run.1',
    }];
    const stoppingMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(stoppingMarkup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopping<\/button>/,
    );
    expect(stoppingMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );
  });

  it.each(['stopping', 'mixed'] as const)('reflects selected group %s without redundant Stop or blocking active motion', (state) => {
    const running = controller('running');
    running.state.controls = ordinaryMotionStopControls();
    const group = running.state.controls.executionSelections!.groups[0];
    group.state = state;
    group.stopAllowed = state === 'mixed';
    group.resumeAllowed = false;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    if (state === 'stopping') {
      expect(markup).toMatch(/class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopping<\/button>/);
    } else {
      expect(markup).toMatch(/class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*>Stop<\/button>/);
    }
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
  });

  it.each([false, true])('shows the owner velocity scope while Run is starting (group=%s)', (group) => {
    const starting = controller('succeeded', 'idle_after_reset');
    starting.state.pending = 'run';
    starting.state.controls = ordinaryMotionStopControls();
    starting.state.controls.executionSelections.groups[0].state = 'running';
    const robot = robotController();
    const selector: RuntimeScaleControlSelector = group
      ? {
        kind: 'group', programBindingId: `sha256:${'c'.repeat(64)}`,
        groupTargets: [
          { componentRoleId: 'left', targetRoleId: 'arm' },
          { componentRoleId: 'right', targetRoleId: 'arm' },
        ],
      }
      : {
        kind: 'singular',
        programBindingId: `sha256:${'c'.repeat(64)}`,
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
      };
    const status = runtimeScaleStatus(selector);
    starting.state.controls.executionSelections.groups[0].runtimeScaleSelector = status.selector;
    starting.groupRuntimeScales = {
      all: { selector: status.selector, status, draft: 0.5, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={starting} robot={robot} />,
    );

    expect(markup).toMatch(/type="range"[^>]*value="0.5"/);
    expect(markup).not.toMatch(/type="range"[^>]*disabled=/);
    expect(markup).toContain('>Velocity<');
    expect(markup).toContain('Scale current and subsequent moves for all.');
  });

  it('keeps group velocity disabled until Execute creates a captured owner target', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    idle.state.applied!.program.tree = {
      node_id: 'motion.move_to_joint_target',
      instance_id: 'root-motion',
      parameters: { target_role: 'arm' },
      children: [],
    };
    idle.state.applied!.program.target_roles = { arm: {} };
    idle.state.applied!.programBinding.target_role_bindings = [{
      target_role_id: 'arm', component_role_id: 'robot',
      controllable_component_id: 'robot', command_target_id: 'arm',
      frame_role_bindings: [], resource_role_bindings: [],
      collision_body_role_bindings: [], capability_bindings: [],
    }];
    const selector: RuntimeScaleControlSelector = {
      kind: 'singular',
      programBindingId: idle.state.applied!.programBindingId,
      componentRoleId: 'robot',
      targetRoleId: 'arm',
    };
    const status = runtimeScaleStatus(selector, 0.35);
    idle.groupRuntimeScales = {
      all: { selector, status, draft: 0.35, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    // The idle Applied group names its robot from the Applied binding.
    expect(markup).toContain('title="all: arm (robot)"');
    expect(markup).toMatch(/type="range"[^>]*value="0.35"/);
    expect(markup).toMatch(/type="range"[^>]*disabled=/);
  });

  it('blocks Program Resume for a resident manual operation in the other editor mode', () => {
    const program = controller();
    program.state.controls = ordinaryMotionStopControls();
    const robot = robotController();
    robot.currentStatus!.manualOperations = [{
      programRunId: robot.currentStatus!.directControlGripper!.programRunId,
      operationId: 'manual-gripper', operationRevision: 1, resident: true,
      state: 'executing', progress: 0.5, activeStopId: null,
      activeStopGeneration: null, terminalError: null,
    }];
    expect(robot.state.operation).toBeNull();
    const blocked = renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />);
    expect(blocked).toMatch(/class="transport-group-resume"[^>]*disabled=""/);
    robot.currentStatus!.manualOperations[0].resident = false;
    const released = renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />);
    expect(released).toMatch(/class="transport-group-resume"(?![^>]*disabled)/);
  });

  it('does not delay Run while a coalesced velocity update is pending', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    const robot = robotController();
    robot.runtimeScalePending = true;

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robot} />,
    );

    expect(markup).toContain('class="transport-run transport-unique-control"');
    expect(markup).not.toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Run<\/button>/,
    );
  });

  it('keeps motion Stop disabled while Program Execute is still starting', () => {
    const starting = controller('succeeded', 'starting');
    starting.state.run = null;
    starting.state.pending = 'run';

    const markup = renderToStaticMarkup(
      <ProgramTransportControls
        enabled
        program={starting}
        robot={robotController()}
      />,
    );

    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stop<\/button>/,
    );
  });

  it('renders physically separate left group and right unique-control panels', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );
    expect(markup).toContain('class="transport-motion-panel"');
    expect(markup).toMatch(/class="transport-motion-panel"[^>]*>[\s\S]*class="transport-panel-title">Motion<\/h3>/);
    expect(markup).toMatch(/class="transport-unique-column"[^>]*>[\s\S]*class="transport-panel-title">Program<\/h3>/);

    const resumeIndex = markup.indexOf('class="transport-group-resume"');
    const stopIndex = markup.indexOf('class="danger-button transport-group-stop"');
    const groupNameIndex = markup.indexOf('class="transport-group-name"');
    const velocityIndex = markup.indexOf('class="transport-group-scale"');
    expect(resumeIndex).toBeGreaterThan(-1);
    expect(resumeIndex).toBeLessThan(stopIndex);
    expect(stopIndex).toBeLessThan(velocityIndex);
    expect(velocityIndex).toBeLessThan(groupNameIndex);
    expect(markup).toContain('>Halt</button>');

    const css = readStyleSource();
    const transport = css.slice(
      css.indexOf('.viewer-transport {'),
      css.indexOf('.transport-velocity {'),
    );
    expect(transport).toContain('overflow: visible;');
    expect(transport).toMatch(/\.viewer-transport \{[^}]*padding: 0;[^}]*overflow: visible;/s);
    expect(transport).not.toMatch(/\.viewer-transport \{[^}]*(?:border|background):/s);
    expect(transport).toMatch(/\.viewer-transport \{[^}]*pointer-events: none;/s);
    expect(css).toMatch(
      /\.viewer-transport \.transport-motion-panel,[\s\S]*\.transport-unique-column \{[^}]*pointer-events: auto;/s,
    );
    expect(transport).toMatch(
      /\.viewer-transport \{[^}]*grid-template-columns:[^;]*var\(--viewer-transport-motion-panel-width\)[^;]*minmax\(0, 1fr\)[^;]*var\(--viewer-transport-column-width\);[^}]*column-gap: 1\.25rem;/s,
    );
    expect(css).not.toContain('.transport-guidance');
    expect(transport).toMatch(/\.transport-control-layout \{[^}]*display: contents;/s);
    expect(transport).toMatch(
      /\.transport-motion-panel \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);/s,
    );
    expect(transport).toMatch(
      /\.transport-motion-panel \{[^}]*width: var\(--viewer-transport-motion-panel-width\);/s,
    );
    expect(transport).toMatch(
      /\.transport-group-grid \{[^}]*justify-content: flex-start;[^}]*padding: 0;[^}]*overflow-x: auto;/s,
    );
    expect(transport).toMatch(
      /\.transport-group-column \{[^}]*padding: 0;[^}]*border: 0;[^}]*background: transparent;/s,
    );
    expect(transport).toMatch(
      /\.transport-group-scale \{[^}]*border: 1px solid #405764;[^}]*border-radius: 0\.45rem;/s,
    );
    expect(transport).toMatch(
      /\.transport-unique-column \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);[^}]*margin-left: auto;/s,
    );
  });

  it('splits Info and group Settings when the Applied Program has a named group', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    idle.state.applied!.program.execution_groups = {
      robot_group: { execution_policy: 'independent' },
    };
    idle.state.applied!.program.tree.execution_groups = ['robot_group'];

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    expect(markup).toContain('aria-label="Program run information"');
    // The gear opens the 3D viewer settings, which hold these controls.
    expect(markup).toMatch(/aria-label="Open viewer settings(?:; [^"]+)?"/);
    expect(markup).toContain('class="transport-unique-icons"');
    expect(markup).toContain('robot_group controls');
    // The default layout hides only "all", which is not counted.
    expect(markup).not.toContain('transport-hidden-group-count');

    const css = readStyleSource();
    expect(css).toMatch(
      /\.transport-unique-icons \{[^}]*grid-template-columns: repeat\(2, minmax\(0, 1fr\)\);/s,
    );
    expect(css).toMatch(
      /\.transport-unique-icons > \.transport-settings-button \{[^}]*position: relative;/s,
    );
    expect(css).toMatch(
      /\.transport-group-scale \{[^}]*font-size: 0\.84rem;/s,
    );
    expect(css).toMatch(
      /\.transport-settings-glyph \{[^}]*font-size: 1\.3rem;/s,
    );
    expect(css).toMatch(
      /\.viewer-transport \.transport-run:hover:not\(:disabled\),[\s\S]*\.transport-group-resume:hover:not\(:disabled\)/,
    );
    expect(css).toMatch(
      /\.viewer-transport \.danger-button:hover:not\(:disabled\)/,
    );
    expect(css).toMatch(
      /\.transport-group-scale:has\(input:not\(:disabled\)\):hover/,
    );
    expect(css).toMatch(
      /\.transport-group-scale span \{[^}]*padding-left: 0\.18rem;/s,
    );
    expect(css).toMatch(
      /\.transport-group-scale output \{[^}]*padding-right: 0\.18rem;/s,
    );
  });

  it('exposes manual recovery from the owner projection even in another panel/mode', () => {
    const robot = robotController();
    robot.state.status = statusFixture();
    robot.currentStatus = robot.state.status;
    const operation = {
      programRunId: robot.state.status.directControlGripper!.programRunId,
      operationId: 'faulted-gripper', operationRevision: 2, resident: true,
      state: 'intervention_required' as const, progress: 0.78,
      terminalError: 'Execution timeout exceeded', activeStopId: null, activeStopGeneration: null,
    };
    robot.state.status.manualOperations = [operation];
    const idle = controller('succeeded', 'idle_after_reset');
    idle.commandAllowed = vi.fn(() => false);
    const openRecovery = vi.fn();
    const guidance = (enabled = true) => deriveTransportGuidance({
      enabled, program: idle, robot, motionActive: false,
      onOpenManualRecovery: openRecovery, onReviewProgramRecovery: vi.fn(),
    });
    const [manual] = guidance();
    expect(manual).toMatchObject({
      id: 'recovery.manual.faulted-gripper',
      severity: 'error',
      domain: 'recovery',
      title: 'Manual gripper recovery required',
    });
    expect(manual.message).toContain('Execution timeout exceeded');
    expect(manual.message).toContain('Robot control → Reset operation');
    // The existing guarded action travels with its message; there is no
    // acknowledgement and no second copy beside the transport controls.
    expect(manual.actions?.map((action) => action.label)).toEqual(['Open gripper recovery']);
    manual.actions?.[0].run();
    expect(openRecovery).toHaveBeenCalledWith(operation);
    const markup = renderToStaticMarkup(<ProgramTransportControls
      enabled program={idle} robot={robot} onOpenRobotControls={vi.fn()} />);
    expect(markup).not.toContain('Open gripper recovery');
    expect(markup).not.toContain('transport-guidance');
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    // Without a current control connection the connection condition reports
    // once; recovery facts are not presented as current.
    expect(guidance(false)).toEqual([]);
    robot.currentStatus = null;
    expect(guidance().map((notice) => notice.title)).toEqual(['Robot status unavailable']);
    robot.currentStatus = robot.state.status;
    operation.resident = false;
    expect(guidance()).toEqual([]);
    for (const state of ['completed', 'stopped', 'cancelled_pre_dispatch'] as const) {
      robot.state.status.manualOperations = [{ ...operation, resident: true, state, terminalError: null }];
      expect(guidance()).toEqual([]);
    }
  });

  it('shows post-Reset waiting in the existing notice stack and preserves fault priority', () => {
    const program = controller('succeeded', 'idle_after_reset');
    program.commandAllowed = vi.fn(() => false);
    program.commandDisabledReason = vi.fn(() => PROGRAM_RESET_READINESS_MESSAGE);
    const robot = robotController();
    const guidance = () => deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false,
      onReviewProgramRecovery: vi.fn(),
    });
    expect(guidance()).toEqual([expect.objectContaining({
      id: 'program.reset-readiness', severity: 'info', title: PROGRAM_RESET_READINESS_MESSAGE,
    })]);
    const markup = renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} onOpenRobotControls={vi.fn()} />);
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    program.commandDisabledReason = vi.fn(() => null);
    expect(guidance()).toEqual([]);
    program.commandDisabledReason = vi.fn(() => PROGRAM_RESET_READINESS_MESSAGE);
    program.cycleState = 'recovery_required';
    program.cycleFailure = { code: 'program_scene_preparation_failed', message: 'Scene convergence failed' };
    expect(guidance().map((notice) => notice.id)).toContain('recovery.program');
    expect(guidance().map((notice) => notice.id)).not.toContain('program.reset-readiness');
  });

  it('points Program faults to recovery and normal terminal runs to Reset', () => {
    const program = controller('intervention_required', 'recovery_required');
    program.state.run!.message = 'Participant requires reconciliation';
    program.cycleFailure = {
      code: 'program_runtime_control_outcome_unknown',
      command_kind: 'resume_motion',
      message: 'The Resume Motion outcome could not be confirmed. Reset is required.',
    };
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const review = vi.fn();
    const guidance = () => deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false,
      onReviewProgramRecovery: review,
    });
    const [recovery] = guidance();
    expect(recovery).toMatchObject({
      id: 'recovery.program', severity: 'error', title: 'Program recovery required',
      message: 'The Resume Motion outcome could not be confirmed. Reset is required.',
    });
    expect(recovery.actions?.map((action) => action.label)).toEqual(['Review Program recovery']);
    recovery.actions?.[0].run();
    expect(review).toHaveBeenCalledOnce();
    expect(renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} />))
      .toMatch(/class="transport-reset transport-unique-control meaningful"(?![^>]*disabled)/);
    program.cycleState = 'terminal_awaiting_reset';
    // A normal finished run is information, not a warning or error.
    program.cycleFailure = null;
    expect(guidance()).toEqual([expect.objectContaining({
      id: 'program.reset-required', severity: 'info', title: 'Program finished',
    })]);
  });

  it('names the held node failure and its details in the Program recovery notice', () => {
    const program = controller('intervention_required');
    const offer = ordinaryMotionStopOffer();
    program.state.controls = {
      ...ordinaryMotionStopControls(),
      recoveryOffer: {
        ...offer,
        recoveryClass: 'node_failure_pause',
        reason: {
          code: 'owner_request_failed',
          message: 'Move to pick was refused.',
          details: { reference_type: 'frame_role', reference_id: 'tool0', target_role_id: 'arm' },
          origin: { kind: 'owner', nodeInstanceId: 'move', ownerApi: 'kernel.operation' },
        },
      },
    };
    // Another branch's failure in the run status does not replace the held node's own.
    program.state.run!.reason = {
      code: 'program_tree_failed',
      message: 'The Program paused on a failed node.',
      details: null,
      origin: { kind: 'program_runner', nodeInstanceId: null, ownerApi: null },
    };
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const [recovery] = deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false, onReviewProgramRecovery: vi.fn(),
    });
    expect(recovery).toMatchObject({
      id: 'recovery.program',
      message: "Move to pick was refused. (Reference: frame_role 'tool0' of target role 'arm'; "
        + "Origin: kernel.operation for node 'move')",
    });
  });

  it('surfaces a cell-wide latch but never treats a cleared historical latch as blocking', () => {
    const robot = robotController();
    robot.state.status = statusFixture();
    robot.currentStatus = robot.state.status;
    const fault = {
      latchId: 'latch', latchGeneration: 1, state: 'latched' as const,
      trigger: 'physical_rest_unproven' as const, recoveryStatus: 'waiting_for_observation' as const,
      reasonCode: 'physical_rest_unproven', certificate: null,
    };
    robot.state.status.cellStopAction.fault = fault;
    const guidance = () => deriveTransportGuidance({
      enabled: true, program: controller(), robot, motionActive: false,
      onReviewProgramRecovery: vi.fn(),
    });
    const [cell] = guidance();
    expect(cell).toMatchObject({ id: 'recovery.cell', severity: 'error', title: 'Cell recovery required' });
    expect(cell.message).toContain('waiting for observation');
    expect(cell.actions?.[0]).toMatchObject({ label: 'Retry recovery', disabled: false });
    robot.state.status.cellStopAction.fault = { ...fault, state: 'cleared' };
    expect(guidance()).toEqual([]);
  });

  it('shows open component incidents before Recover cell and keeps it available', () => {
    const robot = robotController();
    robot.state.status = statusFixture();
    robot.currentStatus = robot.state.status;
    robot.state.status.cellStopAction.fault = {
      latchId: 'latch', latchGeneration: 1, state: 'latched' as const,
      trigger: 'physical_rest_unproven' as const, recoveryStatus: 'waiting_for_configuration' as const,
      reasonCode: 'physical_rest_unproven', certificate: null,
    };
    robot.componentIncidents = [{
      componentId: 'component.arm',
      incident: {
        incidentId: 'incident.1',
        causeClass: 'external_control',
        vendorCodes: ['state=5', 'mode=2'],
        detectedAtNs: 1_000,
        summary: 'Robot moved from its own interface',
        acknowledged: false,
      },
    }];
    const [cell] = deriveTransportGuidance({
      enabled: true, program: controller(), robot, motionActive: false,
      onReviewProgramRecovery: vi.fn(),
    });
    expect(cell.message).toContain(
      'Incident on component.arm: Robot moved from its own interface · cause: robot moved '
      + 'or changed mode from its own interface · vendor codes: state=5, mode=2.',
    );
    expect(cell.actions?.[0]).toMatchObject({ label: 'Recover cell', disabled: false });
  });

  it('classifies live-view observation by what the operator can rely on', () => {
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const guidance = (phase: 'synchronizing' | 'waiting' | 'reconnecting' | 'live') => (
      deriveTransportGuidance({
        enabled: true, program: controller(), robot, motionActive: false,
        observationTransport: { phase, message: null },
        onReviewProgramRecovery: vi.fn(),
      }).filter((notice) => notice.id === 'observation.live-view')
    );
    expect(guidance('synchronizing')[0]).toMatchObject({ severity: 'info', busy: true });
    expect(guidance('waiting')[0]).toMatchObject({ severity: 'info' });
    // Stale poses are an error; only a known controlled transition may
    // present them as information (see classifyWorkbenchNotices).
    expect(guidance('reconnecting')[0]).toMatchObject({
      severity: 'error',
      expectedDuringTransition: true,
      incidentKind: 'viewer_observation_stale',
    });
    expect(guidance('live')).toEqual([]);
  });

  it('starts fresh pose fields when the runtime blackboard switches key', async () => {
    // Minimal mounted React DOM host (see ProgramSchemaValueEditor.mounted.test.tsx).
    const created: Record<string, unknown>[] = [];
    const document: Record<string, unknown> = {
      nodeType: 9, activeElement: null, defaultView: {},
      addEventListener: () => undefined, removeEventListener: () => undefined,
      createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
    };
    const node = (tag: string): Record<string, unknown> => {
      const element: Record<string, unknown> = {
        nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
        namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
        addEventListener: () => undefined, removeEventListener: () => undefined,
        appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
        setAttribute: () => undefined, removeAttribute: () => undefined,
      };
      created.push(element);
      return element;
    };
    document.createElement = node;
    document.documentElement = node('html');
    const window = {
      document, HTMLIFrameElement: class {},
      addEventListener: () => undefined, removeEventListener: () => undefined,
      setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
    };
    document.defaultView = window;
    vi.stubGlobal('window', window);
    vi.stubGlobal('document', document);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    type Props = Record<string, unknown> & Partial<Record<
      'onFocus' | 'onChange' | 'onBlur', (event: unknown) => void>>;
    const props = (element: Record<string, unknown>) => element[
      Object.keys(element).find((name) => name.startsWith('__reactProps$'))!] as Props;
    const control = (match: (candidate: Props) => boolean) => created
      .filter((element) => Object.keys(element).some((name) => name.startsWith('__reactProps$')))
      .filter((element) => match(props(element))).at(-1)!;
    const positionX = () => control((candidate) => candidate['aria-label'] === 'Position x');
    const writeButton = () => control((candidate) => candidate.children === 'Write current-run value');

    const program = controller('running');
    program.state.applied!.program.blackboard = {
      namespaces: [],
      keys: [
        { key: 'pick_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
        { key: 'place_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
      ],
    };
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(node('div') as unknown as Element);
    try {
      await act(() => { root.render(<ProgramRuntimePanel enabled controller={program} />); });
      const chooseKey = (key: string) => act(() => {
        const target = { value: key };
        props(control((candidate) => Array.isArray(candidate.children)
          && (candidate.children[0] as { props?: { children?: unknown } } | undefined)
            ?.props?.children === 'Choose a key')).onChange!({ target, currentTarget: target });
      });
      await chooseKey('pick_pose');
      const target = { value: 'abc' };
      await act(() => { props(positionX()).onFocus!({ target, currentTarget: target }); });
      await act(() => { props(positionX()).onChange!({ target, currentTarget: target }); });
      await act(() => { props(positionX()).onBlur!({ target, currentTarget: target }); });
      expect(props(writeButton()).disabled).toBe(true);

      await chooseKey('place_pose');

      // What Write sends is what is shown: place_pose's neutral pose.
      expect(props(positionX()).value).toBe('0');
      expect(props(writeButton()).disabled).toBe(false);
    } finally {
      await act(() => { root.unmount(); });
      vi.unstubAllGlobals();
    }
  });
});


it('shows a failed accepted start and native comparison facts while Reset remains required', () => {
  const program = controller('failed', 'terminal_awaiting_reset');
  program.state.run = null; // Start failed before a host RunStatus existed.
  program.cycleFailure = {
    code: 'component_lifecycle_facts_stale',
    message: 'Program run program-cycle.2 started, then failed. Reset the Program cycle.',
    details: { admission_changed: 'true', admission_generation_before: '36', admission_generation_after: '37',
      snapshot_changed: 'false', program_run_id: 'program-cycle.2', claim_generation: 9, needs_reset: true },
  };
  const robot = robotController();
  robot.currentStatus = statusFixture();
  const [notice] = deriveTransportGuidance({
    enabled: true, program, robot, motionActive: false, onReviewProgramRecovery: vi.fn(),
  });
  expect(notice).toMatchObject({ id: 'program.reset-required', severity: 'error', title: 'Run failed' });
  expect(notice.message).toContain('program-cycle.2');
  expect(notice.message).toContain('Admission changed: true');
  expect(notice.message).toContain('Admission generation before: 36');
  expect(notice.message).toContain('Admission generation after: 37');
  expect(notice.message).toContain('Snapshot changed: false');
  expect(notice.message).toContain('Claim generation: 9');
});
