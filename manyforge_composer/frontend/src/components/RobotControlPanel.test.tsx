import { readFileSync } from 'node:fs';
import { act, useReducer } from 'react';
import { createRoot } from 'react-dom/client';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { formatClearanceDistanceMm, RobotControlPanel, RobotControlPanelView } from './RobotControlPanel';
import { DraftInputResetProvider } from './common/DraftInputs';
import { useRobotControl } from '../hooks/useRobotControl';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { LogicalSceneProjection } from '../types/api';
import { buildGripperCommandIntent, initialRobotControlState, robotControlReducer, selectedStatusForState } from '../state/robotControl';
import { gripperReceiptFixture, readyState, statusFixture } from '../testFixtures/robotControl';
import { tcpEntryIdentityKey } from '../types/robotControl';

it('rounds clearance distances around 10 mm and preserves negative values', () => {
  expect(formatClearanceDistanceMm(0.6133268110570969)).toBe('0.61 mm');
  expect(formatClearanceDistanceMm(9.999)).toBe('10 mm');
  expect(formatClearanceDistanceMm(10)).toBe('10.0 mm');
  expect(formatClearanceDistanceMm(-0.6133268110570969)).toBe('-0.61 mm');
  expect(formatClearanceDistanceMm(-10)).toBe('-10.0 mm');
});

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
        baseFrame: { componentId: 'robot-1', frameId: 'base' },
        baseFrameRoleId: 'base',
        tcpFrameRoleId: 'tcp',
        activeTcp: { entry: { componentId: 'robot-1', targetId: 'arm', frameComponentId: 'robot-1', frameRoleId: 'tcp' }, display: 'robot-1.tcp',
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
          schemaVersion: 'manyforge.manual_control_motion_descriptor.v8',
          ordinaryJerk: null,
          controllableComponentId: 'robot-1', commandTargetId: 'arm',
          endEffectorComponentId: null,
          frames: [
            { frameComponentId: 'robot-1', frameRoleId: 'base', compiledFrameName: 'base_link', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
            { frameComponentId: 'robot-1', frameRoleId: 'tcp', compiledFrameName: 'tool0', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
          ],
          tcps: [{
            entry: { componentId: 'robot-1', targetId: 'arm', frameComponentId: 'robot-1', frameRoleId: 'tcp' }, controllableComponentId: 'robot-1', commandTargetId: 'arm',
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
          schemaVersion: 'manyforge.manual_control_gripper_descriptor.v5',
          controllableComponentId: 'robot-1', commandTargetId: 'gripper',
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

function robotPanelHost() {
  const elements: Record<string, unknown>[] = [];
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string) => {
    const listeners = new Map<string, Array<(event: unknown) => void>>();
    const element: Record<string, unknown> = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, options: [],
      style: { setProperty: () => undefined, removeProperty: () => undefined },
      addEventListener: (type: string, listener: (event: unknown) => void) => {
        listeners.set(type, [...(listeners.get(type) ?? []), listener]);
      },
      removeEventListener: (type: string, listener: (event: unknown) => void) => {
        listeners.set(type, (listeners.get(type) ?? []).filter((candidate) => candidate !== listener));
      },
      dispatchEvent: (event: { type: string }) => {
        for (const listener of listeners.get(event.type) ?? []) listener(event);
        return true;
      },
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
      querySelectorAll: () => [], matches: () => false,
      focus: () => { document.activeElement = element; },
      blur: () => { document.activeElement = null; },
    };
    elements.push(element);
    return element;
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  class Observer {
    observe() {}
    disconnect() {}
  }
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('ResizeObserver', Observer);
  vi.stubGlobal('MutationObserver', Observer);
  vi.stubGlobal('requestAnimationFrame', (callback: () => void) => { callback(); return 0; });
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  const props = (element: Record<string, unknown>) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$')) ?? ''
  ] as Record<string, unknown> | undefined;
  const findElement = (label: string) => {
    const found = elements.filter((element) => props(element)?.['aria-label'] === label).at(-1);
    if (found === undefined) throw new Error(`Mounted Robot input not found: ${label}`);
    return found;
  };
  const findButton = (label: string) => {
    const found = elements.map((element) => ({ element, props: props(element) }))
      .filter(({ props: candidate }) => candidate?.type === 'button' && candidate.children === label).at(-1);
    if (found === undefined) throw new Error(`Mounted Robot button not found: ${label}`);
    return found;
  };
  return { document, host: node('div'), findElement, findButton, props };
}

afterEach(() => vi.unstubAllGlobals());

describe('RobotControlPanel layout', () => {
  it('renders manual control modes in their intended order', () => {
    const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    const modeNav = markup.match(/<nav class="robot-control-modes dseg"[^>]*>([\s\S]*?)<\/nav>/)?.[1] ?? '';
    const labels = Array.from(modeNav.matchAll(/<button[^>]*>([^<]+)<\/button>/g), (match) => match[1]);
    expect(labels).toEqual(['Joint', 'Pose plan', 'Cartesian', 'Move To', 'Gripper']);
  });

  it('keeps RobotNumberInput local rebases and global Scenario resets independent without blurring', async () => {
    const status = statusFixture();
    const state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    const base = useRobotControl(true);
    const panelController: ReturnType<typeof useRobotControl> = {
      ...base,
      state,
      selectedStatus: selectedStatusForState(state),
      motionTargets: [status.directControl!],
      gripperTargets: [status.directControlGripper!],
      manualControlAllowed: true,
      manualControlUnavailableReason: null,
      setManualPanelVisible: vi.fn(),
      reportAuthoringPending: vi.fn(),
    };
    const host = robotPanelHost();
    const root = createRoot(host.host as unknown as Element);
    const render = (scenarioKey: string) => act(() => root.render(
      <DraftInputResetProvider scope="scenario" resetKey={scenarioKey}>
        <RobotControlPanelView enabled controller={panelController} />
      </DraftInputResetProvider>,
    ));
    const inputLabel = 'Manual Velocity scale (%)';
    const inputProps = () => host.props(host.findElement(inputLabel))!;
    const typeInvalid = async () => {
      const input = host.findElement(inputLabel);
      (input.focus as () => void)();
      await act(() => (inputProps().onFocus as (event: unknown) => void)({}));
      (input as { value: string }).value = 'not-a-number';
      await act(() => (input.dispatchEvent as (event: unknown) => boolean)({
        type: 'input', inputType: 'insertText', isComposing: false,
      }));
    };
    try {
      await render('scenario-0');
      const input = host.findElement(inputLabel);
      await typeInvalid();
      expect(inputProps().value).toBe('not-a-number');
      expect(inputProps()['aria-invalid']).toBe(true);

      const revert = host.findButton('Revert');
      expect(revert.props?.disabled).not.toBe(true);
      await act(() => (revert.props?.onClick as (() => void) | undefined)?.());
      expect(host.findElement(inputLabel)).toBe(input);
      expect(inputProps().value).toBe('100');
      expect(inputProps()['aria-invalid']).not.toBe(true);

      await typeInvalid();
      expect(inputProps()['aria-invalid']).toBe(true);
      await render('scenario-1');
      expect(host.findElement(inputLabel)).toBe(input);
      expect(host.document.activeElement).toBe(input);
      expect(inputProps().value).toBe('100');
      expect(inputProps()['aria-invalid']).not.toBe(true);
    } finally {
      await act(() => root.unmount());
      vi.unstubAllGlobals();
    }
  });

  it('TCP-C: lists the robot\'s and the mounted tool\'s TCPs, the default selected', () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    status.directControl!.targetDescriptor.tcps = [
      { ...row, qualifiedName: 'lite_gripper.tcp' },
      { ...row, entry: { ...row.entry, frameRoleId: 'flange' }, frameRoleId: 'flange',
        qualifiedName: 'robot-a.flange', compiledFrameName: 'link_eef', isDefault: false },
      { ...row, entry: { componentId: 'lite_gripper', targetId: 'gripper', frameComponentId: 'lite_gripper', frameRoleId: 'cup_edge' }, controllableComponentId: 'lite_gripper',
        commandTargetId: 'gripper', frameRoleId: 'cup_edge', qualifiedName: 'lite_gripper.cup_edge',
        compiledFrameName: 'cup_edge_link', isDefault: false, mountedHere: true,
        mountedElsewhere: false },
      { ...row, entry: { componentId: 'workbench_tool', targetId: 'tool', frameComponentId: 'workbench_tool', frameRoleId: 'tcp' }, controllableComponentId: 'workbench_tool', commandTargetId: 'tool',
        frameRoleId: 'tcp', qualifiedName: 'workbench_tool.tcp', isDefault: false,
        mountedHere: false, mountedElsewhere: true },
      { ...row, entry: { componentId: 'second_target', targetId: 'tool', frameComponentId: 'second_target', frameRoleId: 'tcp' }, controllableComponentId: 'second_target', commandTargetId: 'tool',
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
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;robot-a&quot;,&quot;tcp&quot;]" selected="">lite_gripper.tcp (default)</option>');
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;robot-a&quot;,&quot;flange&quot;]">robot-a.flange</option>');
    expect(markup).toContain('<option value="[&quot;lite_gripper&quot;,&quot;gripper&quot;,&quot;lite_gripper&quot;,&quot;cup_edge&quot;]">lite_gripper.cup_edge</option>');
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
      entry: { componentId: 'lite_gripper', targetId: 'gripper', frameComponentId: 'lite_gripper', frameRoleId: 'cup_edge' },
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

  it('gives a Linear Move To the Cartesian tab\'s straight-line settings', () => {
    const status = statusFixture();
    const controller = useRobotControl(true);
    const ready = controller.state;
    const render = (moveToPath: 'free' | 'linear') => {
      const state = { ...ready, status, mode: 'move_to' as const, moveToKind: 'pose' as const, moveToPath };
      const panelController: ReturnType<typeof useRobotControl> = {
        ...controller, state, selectedStatus: status, motionTargets: [status.directControl!],
        manualControlAllowed: true, manualControlUnavailableReason: null,
      };
      return renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
    };
    expect(render('free')).not.toContain('Lin. Tol. (m)');
    expect(render('linear')).toContain('Lin. Tol. (m)');
    expect(render('linear')).toContain('TCP Ang. (deg/s)');
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
    expect(markup).toContain('<option value="[&quot;robot-a&quot;,&quot;arm&quot;,&quot;robot-a&quot;,&quot;flange&quot;]">robot-a.flange</option>');
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
      parameters: { target: { component_id: 'robot-1', command_target_id: 'arm' },
        goal: [{ joint_name: 'joint_1', position_rad: 0.2 }] }, children: [] });
    const applied = { graphRevision: 'sha256:graph', retainedBundleId: 'sha256:bundle',
      program: { tree: node('applied-goal') }, programBinding: { target_bindings: [
        { target: { component_id: 'robot-1', command_target_id: 'arm' } },
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
    expect(markup).not.toContain('Use in node');
    expect(markup).not.toContain('Use in node:');
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

it('shows sampled diagnostic controls, the exact guidance, and no approval', () => {
  const status = statusFixture();
  const state = { ...initialRobotControlState, status, phase: 'blocked' as const,
    diagnosticPreview: { sourceJointNames: ['joint'], pairs: [], samples: [
      { timeFromStartS: 0, positionsRad: [0], contacts: [], status: 'unknown' as const },
    ] }, previewSampleIndex: 0 };
  const panelController = { ...useRobotControl(true), state, selectedStatus: status,
    motionTargets: [status.directControl!], manualControlAllowed: true, manualControlUnavailableReason: null };
  const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} />);
  expect(markup).toContain('Play or drag along the planned path to see where parts touch.');
  expect(markup).toContain('Blue: checked, no contact. Yellow: contact your settings allow. Orange: closer than clearance. Red: contact not allowed, or still touching at the end. Grey: unknown.');
  expect(markup.match(/Approve is disabled\./g)).toHaveLength(1);
  expect(markup).toContain('Approve is disabled. Check the preview details.');
  expect(markup).toContain('aria-label="Preview sample"');
});

it('explains the shown sample contact using its resolved link and object', () => {
  const status = statusFixture();
  const state = { ...initialRobotControlState, status, phase: 'blocked' as const,
    diagnosticPreview: { sourceJointNames: ['joint'], pairs: [
      { bodyA: 'component/robot/model/model/body/arm/collision/0', bodyB: 'scene/part/shape/0' },
    ], samples: [
      { timeFromStartS: 0, positionsRad: [0], status: 'checked' as const, contacts: [] },
      { timeFromStartS: 1, positionsRad: [0], status: 'checked' as const, contacts: [{ pairIndex: 0, disposition: 'refused' as const }] },
    ] }, previewSampleIndex: 1 };
  const panelController = { ...useRobotControl(true), state, selectedStatus: status,
    motionTargets: [status.directControl!], manualControlAllowed: true, manualControlUnavailableReason: null };
  const sourceModel = { targets: [{ component_id: 'robot', collision_bodies: [
    { model_instance_id: 'model', collision_body_role_id: 'arm', source_link_names: ['link5'] },
  ] }], occurrences: [{ component_id: 'robot', model_instance_id: 'model', links: [
    { source_link_name: 'link5', viewer_link_name: 'mf1_link_abc123' },
  ] }] } as unknown as import('../types/api').SourceModelProjection;
  const objects = [{ scene_resource_id: 'part', collision_shapes: [{}] }] as unknown as import('../types/api').LogicalSceneObject[];
  const markup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController}
    previewSourceModel={sourceModel} previewSceneObjects={objects} />);
  expect(markup).toContain('robot · link5 ↔ part: collision (not allowed)');
  expect(markup).toContain('title="mf1_link_abc123"');
  expect(markup).not.toContain('mf1_link_abc123 ↔ part');
  const unmappedMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={panelController} previewSceneObjects={objects} />);
  expect(unmappedMarkup).toContain('component/robot/model/model/body/arm/collision/0 ↔ part');
  expect(unmappedMarkup).not.toContain('mf1_link_abc123');
  expect(markup).toContain('Blue: clear · Yellow: allowed · Orange: too close · Red: collision · Grey: unknown');

  const clearanceState = { ...state, diagnosticPreview: { ...state.diagnosticPreview,
    schemaVersion: 'manyforge.motion_preview_diagnostic.v2' as const, kind: 'collision_disabled_path' as const,
    reason: 'planning_failed', samples: [{ ...state.diagnosticPreview.samples[1], status: 'unknown' as const,
      contacts: [], clearanceViolations: [{ pairIndex: 0, distanceM: 0.0006133268110570969, requiredClearanceM: .001 }] }] }, previewSampleIndex: 0 };
  const clearanceMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: clearanceState }}
    previewSourceModel={sourceModel} previewSceneObjects={objects} />);
  expect(clearanceMarkup).toContain('robot · link5 ↔ part: closer than clearance (0.61 mm &lt; 1 mm)');
  expect(clearanceMarkup).toContain('title="mf1_link_abc123 · Exact distances: 0.6133268110570969 mm &lt; 1 mm"');
  expect(clearanceMarkup).toContain('Lower the manual Clearance in Speed &amp; acceleration → Advanced, or use an offset.');
  expect(clearanceMarkup).toContain('Approve is disabled');
  expect(clearanceMarkup).not.toContain('it collides here');
  const checkedClearance = { ...clearanceState, diagnosticPreview: { ...clearanceState.diagnosticPreview,
    samples: [{ ...clearanceState.diagnosticPreview.samples[0], status: 'checked' as const }] } };
  const checkedMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: checkedClearance }}
    previewSourceModel={sourceModel} previewSceneObjects={objects} />);
  expect(checkedMarkup).toContain('robot · link5 ↔ part: closer than clearance');
  expect(checkedMarkup).not.toContain('No contacts');
  expect(checkedMarkup).not.toContain('shows no contacts');

  const ikFallbackSamples = [
    { ...clearanceState.diagnosticPreview.samples[0], timeFromStartS: 0, positionsRad: [0],
      status: 'checked' as const, contacts: [], clearanceViolations: [] },
    { ...clearanceState.diagnosticPreview.samples[0], timeFromStartS: 1, positionsRad: [0.1],
      status: 'checked' as const, contacts: [{ pairIndex: 0, disposition: 'refused' as const }], clearanceViolations: [] },
  ];
  const ikFallback = { ...clearanceState, diagnosticPreview: { ...clearanceState.diagnosticPreview,
    kind: 'collision_disabled_ik_path' as const, reason: 'ik_pose_collision', samples: ikFallbackSamples }, previewSampleIndex: 1 };
  const ikFallbackMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: ikFallback }}
    previewSourceModel={sourceModel} previewSceneObjects={objects} />);
  expect(ikFallbackMarkup).toContain('No collision-free pose: showing a path to the pose planned with collisions ignored; it collides here. (ik_pose_collision)');
  expect(ikFallbackMarkup).toContain('robot · link5 ↔ part: collision (not allowed)');
  expect(ikFallbackMarkup).toContain('aria-label="Preview sample"');
  expect(ikFallbackMarkup).toMatch(/class="approve-button"[^>]*disabled=""/);
  const ikFallbackFirstSample = { ...ikFallback, previewSampleIndex: 0 };
  const firstSampleMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: ikFallbackFirstSample }}
    previewSourceModel={sourceModel} previewSceneObjects={objects} />);
  expect(firstSampleMarkup).toContain('No collision-free pose: showing a path to the pose planned with collisions ignored; collision evidence is shown below. (ik_pose_collision)');
  expect(firstSampleMarkup).toContain('No contacts');

  const moreObjects = ['a', 'b', 'c', 'd', 'e', 'f'].map((scene_resource_id) => ({ scene_resource_id, collision_shapes: [{}] })) as unknown as import('../types/api').LogicalSceneObject[];
  const many = { ...state, diagnosticPreview: { ...state.diagnosticPreview,
    pairs: moreObjects.map((object) => ({
      bodyA: 'component/robot/model/model/body/arm/collision/0', bodyB: `scene/${object.scene_resource_id}/shape/0`,
    })),
    samples: [state.diagnosticPreview.samples[0], { ...state.diagnosticPreview.samples[1],
      contacts: moreObjects.map((_, pairIndex) => ({ pairIndex, disposition: pairIndex === 3 ? 'refused' as const : 'allowed' as const })) }],
  } };
  const manyMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: many }}
    previewSourceModel={sourceModel} previewSceneObjects={moreObjects} />);
  expect(manyMarkup.indexOf('robot · link5 ↔ d: collision (not allowed)')).toBeLessThan(manyMarkup.indexOf('robot · link5 ↔ a: allowed contact'));
  expect(manyMarkup).toContain('and 2 more');
  expect(manyMarkup).not.toContain('robot · link5 ↔ e:');
  expect(manyMarkup).not.toContain('robot · link5 ↔ f:');

  many.previewSampleIndex = 0;
  const clearMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: many }} />);
  expect(clearMarkup).toContain('No contacts');
  const unknown = { ...many, diagnosticPreview: { ...many.diagnosticPreview, samples: [
    { ...many.diagnosticPreview.samples[0], status: 'unknown' as const },
  ] } };
  const unknownMarkup = renderToStaticMarkup(<RobotControlPanelView enabled controller={{ ...panelController, state: unknown }} />);
  expect(unknownMarkup).toContain('Not checked');
  expect(unknownMarkup).not.toContain('No contacts');
});


it('warns on a changed Scene only for a successful sampled path', () => {
  const ready = readyState();
  if (ready.preview?.schemaVersion !== 'manyforge.direct_control_motion_preview.v4') throw new Error('expected motion fixture');
  const state = { ...ready, preview: { ...ready.preview,
    pathPreview: { sourceJointNames: ['joint'], pairs: [], samples: [
      { timeFromStartS: 0, positionsRad: [0], contacts: [], status: 'checked' as const },
    ] } } };
  const panelController = { ...useRobotControl(true), state, selectedStatus: state.status,
    motionTargets: [state.status!.directControl!], manualControlAllowed: true, manualControlUnavailableReason: null };
  const scene = { snapshot: { revision: { ...state.preview.sceneRevision, revision_id: state.preview.sceneRevision.revision_id + 1 } } } as unknown as LogicalSceneProjection;
  const render = (value: ReturnType<typeof useRobotControl>) => renderToStaticMarkup(<RobotControlPanelView enabled controller={value} runtimeScene={scene} />);
  expect(render(panelController)).toContain('The Scene changed after this preview.');
  const diagnostic = { ...panelController, state: { ...state, phase: 'blocked' as const,
    preview: null, diagnosticPreview: state.preview.pathPreview } };
  expect(render(diagnostic)).not.toContain('The Scene changed after this preview.');
});


it('starts at the final sample, and Play and replay restart from sample zero', async () => {
  vi.useFakeTimers();
  const ready = readyState();
  const blocked = robotControlReducer(ready, { type: 'preview_rejected',
    generation: ready.generation, error: 'refused' });
  const path = { sourceJointNames: ['joint_1', 'joint_2'], pairs: [], samples: [
    { timeFromStartS: 0, positionsRad: [0, 0], contacts: [], status: 'unknown' as const },
    { timeFromStartS: 0.4, positionsRad: [0.1, 0.2], contacts: [], status: 'unknown' as const },
    { timeFromStartS: 1, positionsRad: [0.25, 0.5], contacts: [], status: 'unknown' as const },
  ] };
  const initial = robotControlReducer(blocked, { type: 'preview_diagnostic',
    generation: blocked.generation, diagnostic: path });
  const base = useRobotControl(true);
  const host = robotPanelHost();
  const window = host.document.defaultView as { setTimeout: typeof setTimeout; clearTimeout: typeof clearTimeout };
  window.setTimeout = setTimeout;
  window.clearTimeout = clearTimeout;
  const root = createRoot(host.host as unknown as Element);
  function MountedPreview() {
    const [state, dispatch] = useReducer(robotControlReducer, initial);
    return <RobotControlPanelView enabled controller={{
      ...base, state, selectedStatus: selectedStatusForState(state),
      motionTargets: [state.status!.directControl!], manualControlAllowed: true,
      manualControlUnavailableReason: null,
      setManualPanelVisible: vi.fn(), reportAuthoringPending: vi.fn(),
      selectPreviewSample: (index: number) => dispatch({ type: 'select_preview_sample', index }),
    }} />;
  }
  const sample = () => host.props(host.findElement('Preview sample'))!;
  const clickPlay = async () => {
    const button = host.findButton('Play');
    await act(() => (button.props?.onClick as () => void)());
  };
  try {
    await act(() => root.render(<MountedPreview />));
    expect(sample().value).toBe(2);
    await clickPlay();
    expect(sample().value).toBe(0);
    // Restart while already playing sample zero: the old timer must be reset
    // even though the selected index itself does not change.
    await act(() => vi.advanceTimersByTime(200));
    await clickPlay();
    expect(sample().value).toBe(0);
    await act(() => vi.advanceTimersByTime(399));
    expect(sample().value).toBe(0);
    await act(() => vi.advanceTimersByTime(1));
    expect(sample().value).toBe(1);
    // Restart from a later sample also immediately selects zero.
    await clickPlay();
    expect(sample().value).toBe(0);
    await act(() => vi.advanceTimersByTime(400));
    expect(sample().value).toBe(1);
    await act(() => (sample().onChange as (event: unknown) => void)({ target: { value: '2' } }));
    expect(sample().value).toBe(2);
    await act(() => vi.advanceTimersByTime(1000));
    expect(sample().value).toBe(2);
  } finally {
    await act(() => root.unmount());
    vi.useRealTimers();
  }
});
