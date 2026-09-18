import { readFileSync } from 'node:fs';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type { CycleRecoveryOffer, ProgramRunStateName } from '../types/api';
import type {
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from '../types/robotControl';
import { statusFixture } from '../testFixtures/robotControl';
import { ProgramRuntimePanel, ProgramTransportControls } from './ProgramRuntimePanel';

function ordinaryMotionStopOffer(): CycleRecoveryOffer {
  return {
    schemaVersion: 'manyforge.cycle_recovery_offer.v2',
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
    reasonCode: 'ordinary_operator_stop',
    evidenceRefs: [],
    policy: {},
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
        runtimeScaleUnavailableReason: null }],
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
    resume: vi.fn(),
    stop: vi.fn(),
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
    state: {
      catalog: null,
      currentScenario: null,
      applied: {
        schemaVersion: 'manyforge.applied_program_selection.v1',
        scenarioId: 'cell-pick',
        scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
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
        program: {
          api_version: 'manyforge.program.v2',
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          program_profile: 'composable-cell-v1',
          name: 'Next pick',
          catalog: {
            api_version: 'manyforge.node_catalog.v2',
            content_version: '2.3.0',
          },
          component_roles: { component: {} },
          target_roles: {},
          tree: {
            node_id: 'logic.check_bool',
            instance_id: 'root',
            parameters: { actual_value: true, expected_value: true },
            children: [],
          },
        },
        programBinding: {
          api_version: 'manyforge.program_binding.v1',
          program_binding_id: `sha256:${'c'.repeat(64)}`,
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          retained_bundle_id: `sha256:${'a'.repeat(64)}`,
          graph_revision: `sha256:${'b'.repeat(64)}`,
          node_environment: {
            catalog_id: 'manyforge.default',
            content_version: '2.3.0',
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
        schemaVersion: 'manyforge.program_run_status.v2',
        ownerId: 'program-runner.cell-a',
        ownerEpoch: 'program-runner.epoch.1',
        runRevision: 4,
        state,
        capture: {
          programRunId: 'program-run.1',
          appliedRecordDigest: `sha256:${'d'.repeat(64)}`,
          scenarioId: 'cell-pick',
          scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
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
        reasonCode: null,
        message: '',
      },
      controls: null,
      selectedScenarioId: null,
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
    selectScenario: vi.fn(),
    clearScenarioSelection: vi.fn(),
    dismissError: vi.fn(),
    run: vi.fn(),
    stopMotion: vi.fn(),
    startMotion: vi.fn(),
    resumeMotion: vi.fn(),
    fullStop: vi.fn(),
    resumeFullStop: vi.fn(),
    stop: vi.fn(),
    reset: vi.fn(),
    applyRecoveryAction: vi.fn(),
    stageObservationOverride: vi.fn(),
    activateRuntimeOverride: vi.fn(),
    clearRuntimeOverride: vi.fn(),
    promoteRuntimeOverride: vi.fn(),
    materializePromotion: vi.fn(),
    promotionMaterializationReason: vi.fn(() => null),
    writeRuntimeBlackboard: vi.fn(),
    stageManualResult: vi.fn(),
    groupRuntimeScales: {},
    setGroupRuntimeScale: vi.fn(),
    commitGroupRuntimeScale: vi.fn(),
  };
}

describe('Program runtime projection', () => {
  it('shows exact owner status and retains one-click Stop for one active run', () => {
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );

    expect(markup).toContain('Headless-owned execution');
    expect(markup).toContain('program-run.1');
    expect(markup).toContain('revision 4');
    expect(markup).toContain('next-pick');
    expect(markup).not.toContain('Selection for the next Run');
    expect(markup).toContain('Captured run Program');
    expect(markup).toContain('immutable for this run');
    expect(markup).toContain('Program Stop');
    expect(markup).not.toContain('Stop all');
    expect(markup).toContain('never sends Stop implicitly');
  });

  it('enables Execute only after the terminal cycle has been reset to Idle', () => {
    const active = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );
    const terminal = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller('succeeded')} />,
    );
    const resetIdle = renderToStaticMarkup(
      <ProgramRuntimePanel
        enabled
        controller={controller('succeeded', 'idle_after_reset')}
      />,
    );

    expect(active).toMatch(/disabled=""[^>]*>Execute Applied Program/);
    expect(terminal).toMatch(/disabled=""[^>]*>Execute Applied Program/);
    expect(terminal).toContain('Reset Program cycle');
    expect(resetIdle).not.toMatch(/disabled=""[^>]*>Execute Applied Program/);
  });

  it('offers the same Execute recovery from Program Halt in the detailed panel', () => {
    const fullStopped = controller('full_stopped');
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={fullStopped} />,
    );

    expect(markup).toContain('>Execute</button>');
    expect(markup).not.toMatch(/disabled=""[^>]*>Execute<\/button>/);
    expect(markup).toContain('Reset Program cycle');
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
    expect(unavailableMarkup).toContain('Runner loss never proves');

    const unknownCount = controller();
    if (unknownCount.state.run) {
      unknownCount.state.run.activeOperationCount = null;
    }
    const unknownMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unknownCount} />,
    );
    expect(unknownMarkup).toContain('<strong>unknown</strong>');
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
    settledMotionStop.state.run!.reasonCode = 'operation_stopped';
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

  it('projects Execute as active while initial motion is Ready for deliberate Run', () => {
    const executing = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    group.state = 'ready';
    group.stopAllowed = false;
    group.startAllowed = true;
    group.startUnavailableReason = null;
    group.resumeAllowed = false;
    group.resumeUnavailableReason = 'selection_not_stopped';
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

    expect(markup).toContain('title="all"');
    expect(markup).toMatch(/type="range"[^>]*value="0.35"/);
    expect(markup).toMatch(/type="range"[^>]*disabled=/);
  });

  it('blocks Program Resume for a resident manual operation in the other editor mode', () => {
    const program = controller();
    program.state.controls = ordinaryMotionStopControls();
    const robot = robotController();
    robot.currentStatus!.manualOperations = [{
      programRunId: robot.currentStatus!.directControlGripper.programRunId,
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

    const css = readFileSync(new URL('../index.css', import.meta.url), 'utf8');
    const transport = css.slice(
      css.indexOf('.viewer-transport {'),
      css.indexOf('.transport-velocity {'),
    );
    expect(transport).toContain('overflow: visible;');
    expect(transport).toMatch(/\.viewer-transport \{[^}]*padding: 0;[^}]*overflow: visible;/s);
    expect(transport).not.toMatch(/\.viewer-transport \{[^}]*(?:border|background):/s);
    expect(transport).toMatch(
      /\.viewer-transport \{[^}]*grid-template-columns:[^;]*var\(--viewer-transport-motion-panel-width\)[^;]*minmax\(0, 1fr\)[^;]*var\(--viewer-transport-column-width\);[^}]*column-gap: 1\.25rem;/s,
    );
    expect(transport).toMatch(
      /\.transport-guidance \{[^}]*grid-row: 1;[^}]*grid-column: 2;[^}]*align-self: end;/s,
    );
    expect(transport).toMatch(/\.transport-control-layout \{[^}]*display: contents;/s);
    expect(transport).toMatch(
      /\.transport-motion-panel \{[^}]*border: 1px solid #365463;[^}]*background: rgb\(8 23 32 \/ 94%\);/s,
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
      /\.transport-group-scale \{[^}]*border: 1px solid #385061;[^}]*border-radius: 0\.45rem;/s,
    );
    expect(transport).toMatch(
      /\.transport-unique-column \{[^}]*border: 1px solid #365463;[^}]*background: rgb\(8 23 32 \/ 94%\);[^}]*margin-left: auto;/s,
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
    expect(markup).toMatch(/aria-label="Open control settings(?:; [^"]+)?"/);
    expect(markup).toContain('class="transport-unique-icons"');
    expect(markup).toContain('robot_group controls');
    expect(markup).toContain('transport-hidden-group-count');

    const css = readFileSync(new URL('../index.css', import.meta.url), 'utf8');
    expect(css).toMatch(
      /\.transport-unique-icons \{[^}]*grid-template-columns: repeat\(2, minmax\(0, 1fr\)\);/s,
    );
    expect(css).toMatch(
      /\.transport-group-settings > button \{[^}]*width: 100%;[^}]*height: 100%;/s,
    );
    expect(css).toMatch(
      /\.transport-group-scale \{[^}]*font-size: 0\.84rem;/s,
    );
    expect(css).toMatch(
      /\.transport-settings-glyph \{[^}]*font-size: 1\.3rem;/s,
    );
    expect(css).toMatch(
      /\.transport-group-settings-menu \.transport-settings-option \{[^}]*grid-template-columns: 1rem minmax\(0, 1fr\);[^}]*opacity: 1;/s,
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
      programRunId: robot.state.status.directControlGripper.programRunId,
      operationId: 'faulted-gripper', operationRevision: 2, resident: true,
      state: 'intervention_required' as const, progress: 0.78,
      terminalError: 'Execution timeout exceeded', activeStopId: null, activeStopGeneration: null,
    };
    robot.state.status.manualOperations = [operation];
    const idle = controller('succeeded', 'idle_after_reset');
    idle.commandAllowed = vi.fn(() => false);
    const render = (enabled = true) => renderToStaticMarkup(<ProgramTransportControls
      enabled={enabled} program={idle} robot={robot} onOpenRobotControls={vi.fn()} />);
    const markup = render();
    expect(markup).toContain('Manual gripper recovery required');
    expect(markup).toContain('Execution timeout exceeded');
    expect(markup).toContain('Open gripper recovery');
    expect(markup).toContain('Robot control → Reset operation');
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    expect(render(false)).toContain('Control status unavailable.');
    expect(render(false)).not.toContain('Manual gripper recovery required');
    robot.currentStatus = null;
    expect(render()).toContain('Robot status unavailable.');
    expect(render()).not.toContain('Manual gripper recovery required');
    expect(render()).not.toContain('Open gripper recovery');
    robot.currentStatus = robot.state.status;
    operation.resident = false;
    expect(render()).not.toContain('recovery required');
    expect(render()).not.toContain('Open gripper recovery');
    for (const state of ['completed', 'stopped', 'cancelled_pre_dispatch'] as const) {
      robot.state.status.manualOperations = [{ ...operation, resident: true, state, terminalError: null }];
      expect(render()).not.toContain('Manual gripper recovery required');
    }
  });

  it('points Program faults to recovery and normal terminal runs to Reset', () => {
    const program = controller('intervention_required', 'recovery_required');
    program.state.run!.message = 'Participant requires reconciliation';
    program.cycleFailure = {
      code: 'program_runtime_control_outcome_unknown',
      command_kind: 'resume_motion',
      message: 'The Resume Motion outcome could not be confirmed. Reset is required.',
    };
    const render = () => renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robotController()} />);
    expect(render()).toContain('Program recovery required.');
    expect(render()).toContain('The Resume Motion outcome could not be confirmed. Reset is required.');
    expect(render()).not.toContain('Participant requires reconciliation');
    expect(render()).toContain('Program recovery required.');
    expect(render()).toContain('Review Program recovery');
    expect(render()).toMatch(/class="transport-reset transport-unique-control meaningful"(?![^>]*disabled)/);
    program.cycleState = 'terminal_awaiting_reset';
    expect(render()).toContain('Program finished. Use <strong>Reset</strong>');
    expect(render()).not.toContain('Program recovery required.');
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
    const render = () => renderToStaticMarkup(<ProgramTransportControls
      enabled program={controller()} robot={robot} />);
    expect(render()).toContain('Cell recovery required.');
    expect(render()).toContain('waiting for observation');
    robot.state.status.cellStopAction.fault = { ...fault, state: 'cleared' };
    expect(render()).not.toContain('Cell recovery required.');
  });
});
