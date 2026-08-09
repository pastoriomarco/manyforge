import type {
  ExecutionOwnerStatus,
  JointMotionOperationIntent,
  MotionPreviewReceipt,
  OperationAcceptance,
  OperationObservation,
  OperationStateName,
} from '../types/robotControl';

export const ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS = 150;
export const ROBOT_CONTROL_OPERATION_POLL_MS = 500;

const TERMINAL_OPERATION_STATES = new Set<OperationStateName>([
  'completed',
  'stopped',
  'failed',
  'interrupted',
  'intervention_required',
  'cancelled_pre_dispatch',
]);

export type RobotControlPhase =
  | 'loading'
  | 'unavailable'
  | 'idle'
  | 'planning'
  | 'ready'
  | 'blocked'
  | 'approving'
  | 'executing'
  | 'stopping'
  | 'completed'
  | 'stopped'
  | 'failed'
  | 'interrupted'
  | 'intervention_required'
  | 'cancelled_pre_dispatch';

export interface RobotControlState {
  status: ExecutionOwnerStatus | null;
  phase: RobotControlPhase;
  draftPositionsRad: number[];
  generation: number;
  intent: JointMotionOperationIntent | null;
  preview: MotionPreviewReceipt | null;
  ghost: MotionPreviewReceipt | null;
  ghostStale: boolean;
  operation: OperationObservation | null;
  error: string | null;
}

export type RobotControlAction =
  | { type: 'load' }
  | { type: 'status_resolved'; status: ExecutionOwnerStatus }
  | { type: 'status_refreshed'; status: ExecutionOwnerStatus }
  | { type: 'load_failed'; error: string }
  | { type: 'edit'; positionsRad: number[] }
  | {
      type: 'preview_started';
      generation: number;
      intent: JointMotionOperationIntent;
    }
  | {
      type: 'preview_resolved';
      generation: number;
      receipt: MotionPreviewReceipt;
    }
  | { type: 'preview_rejected'; generation: number; error: string }
  | { type: 'approve_started' }
  | { type: 'approve_resolved'; acceptance: OperationAcceptance }
  | { type: 'approve_rejected'; error: string }
  | { type: 'operation_observed'; operation: OperationObservation }
  | { type: 'operation_poll_failed'; error: string }
  | { type: 'stop_started' }
  | { type: 'stop_rejected'; error: string }
  | { type: 'revert' }
  | { type: 'clear' };

export const initialRobotControlState: RobotControlState = {
  status: null,
  phase: 'loading',
  draftPositionsRad: [],
  generation: 0,
  intent: null,
  preview: null,
  ghost: null,
  ghostStale: false,
  operation: null,
  error: null,
};

export function operationStateIsTerminal(state: OperationStateName): boolean {
  return TERMINAL_OPERATION_STATES.has(state);
}

export function robotControlHasActiveOperation(
  state: RobotControlState,
): boolean {
  return state.phase === 'approving'
    || state.phase === 'executing'
    || state.phase === 'stopping';
}

function arraysEqual<T>(left: T[], right: T[]): boolean {
  return left.length === right.length
    && left.every((value, index) => value === right[index]);
}

function positionsAreValid(
  status: ExecutionOwnerStatus,
  positions: number[],
): boolean {
  return positions.length === status.directControl.joints.length
    && positions.every((position, index) => {
      const joint = status.directControl.joints[index];
      return Number.isFinite(position)
        && joint !== undefined
        && position >= joint.minPositionRad
        && position <= joint.maxPositionRad;
    });
}

function receiptMatchesCurrentDraft(
  state: RobotControlState,
  receipt: MotionPreviewReceipt,
): boolean {
  const intent = state.intent;
  const status = state.status;
  if (intent === null || status === null) return false;
  const participant = intent.participants[0];
  const expectedNames = participant.payload.motion.joint_names;
  return receipt.commitEligible
    && receipt.requestId === intent.request_context.request_id
    && receipt.requestOwnerEpoch === status.ownerEpoch
    && receipt.programId === intent.program_id
    && receipt.programRevision === intent.program_revision
    && receipt.programBindingId === intent.program_binding_id
    && receipt.programRunId === intent.program_run_id
    && receipt.graphRevision === intent.expected_graph_revision
    && receipt.retainedBundleId === intent.expected_retained_bundle_id
    && receipt.componentRoleId === participant.component_role_id
    && receipt.targetRoleId === participant.target_role_id
    && receipt.requiredCapabilityId === participant.required_capability_id
    && arraysEqual(receipt.sourceJointNames, expectedNames)
    && receipt.finalPositionsRad.length === expectedNames.length;
}

function phaseForOperation(operation: OperationObservation): RobotControlPhase {
  if (!operationStateIsTerminal(operation.state)) {
    return operation.state === 'stopping' ? 'stopping' : 'executing';
  }
  return operation.state as RobotControlPhase;
}

function resolveStatus(
  state: RobotControlState,
  status: ExecutionOwnerStatus,
  initialize: boolean,
): RobotControlState {
  if (state.status !== null && state.status.ownerEpoch !== status.ownerEpoch) {
    return {
      ...initialRobotControlState,
      status,
      phase: 'idle',
      draftPositionsRad: [...status.observedMotion.positionsRad],
      generation: state.generation + 1,
      error: 'The execution owner restarted. The prior preview was discarded.',
    };
  }
  if (initialize || state.status === null) {
    return {
      ...state,
      status,
      phase: 'idle',
      draftPositionsRad: [...status.observedMotion.positionsRad],
      intent: null,
      preview: null,
      ghost: null,
      ghostStale: false,
      operation: null,
      error: null,
    };
  }
  return { ...state, status };
}

export function robotControlReducer(
  state: RobotControlState,
  action: RobotControlAction,
): RobotControlState {
  switch (action.type) {
    case 'load':
      return state.status === null
        ? { ...state, phase: 'loading', error: null }
        : { ...state, error: null };
    case 'status_resolved':
      return resolveStatus(state, action.status, true);
    case 'status_refreshed':
      return resolveStatus(state, action.status, false);
    case 'load_failed':
      return state.status === null
        ? { ...state, phase: 'unavailable', error: action.error }
        : { ...state, error: action.error };
    case 'edit': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      if (!positionsAreValid(state.status, action.positionsRad)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          error: 'The authored joint target is outside its retained limits.',
        };
      }
      if (arraysEqual(state.draftPositionsRad, action.positionsRad)) {
        return state;
      }
      return {
        ...state,
        phase: 'planning',
        draftPositionsRad: [...action.positionsRad],
        generation: state.generation + 1,
        intent: null,
        preview: null,
        ghostStale: state.ghost !== null,
        operation: null,
        error: null,
      };
    }
    case 'preview_started':
      if (
        action.generation !== state.generation
        || state.phase !== 'planning'
      ) return state;
      return { ...state, intent: action.intent };
    case 'preview_resolved':
      if (
        action.generation !== state.generation
        || state.phase !== 'planning'
      ) return state;
      if (!receiptMatchesCurrentDraft(state, action.receipt)) {
        return {
          ...state,
          phase: 'blocked',
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          error: 'The preview evidence does not match the current draft.',
        };
      }
      return {
        ...state,
        phase: 'ready',
        preview: action.receipt,
        ghost: action.receipt,
        ghostStale: false,
        error: null,
      };
    case 'preview_rejected':
      if (action.generation !== state.generation) return state;
      return {
        ...state,
        phase: 'blocked',
        intent: null,
        preview: null,
        ghostStale: state.ghost !== null,
        error: action.error,
      };
    case 'approve_started':
      if (
        state.phase !== 'ready'
        || state.preview === null
        || state.intent === null
      ) return state;
      return { ...state, phase: 'approving', error: null };
    case 'approve_resolved':
      if (
        state.phase !== 'approving'
        || state.intent === null
        || action.acceptance.requestId
          !== state.intent.request_context.request_id
      ) return state;
      return {
        ...state,
        phase: 'executing',
        preview: null,
        ghostStale: true,
        operation: {
          operationId: action.acceptance.operationId,
          operationRevision: action.acceptance.operationRevision,
          state: 'queued',
          progress: 0,
          activeStopId: null,
          activeStopGeneration: null,
          terminalError: null,
        },
        error: null,
      };
    case 'approve_rejected':
      if (state.phase !== 'approving') return state;
      return {
        ...state,
        phase: 'blocked',
        preview: null,
        ghostStale: state.ghost !== null,
        error: action.error,
      };
    case 'operation_observed':
      if (
        state.operation === null
        || state.operation.operationId !== action.operation.operationId
        || action.operation.operationRevision
          < state.operation.operationRevision
      ) return state;
      return {
        ...state,
        phase: phaseForOperation(action.operation),
        operation: action.operation,
        ghostStale: state.ghost !== null,
        error: action.operation.terminalError,
      };
    case 'operation_poll_failed':
      return robotControlHasActiveOperation(state)
        ? { ...state, error: action.error }
        : state;
    case 'stop_started':
      return state.operation !== null && robotControlHasActiveOperation(state)
        ? { ...state, phase: 'stopping', error: null }
        : state;
    case 'stop_rejected':
      return state.operation !== null && state.phase === 'stopping'
        ? { ...state, phase: 'executing', error: action.error }
        : state;
    case 'revert':
      if (robotControlHasActiveOperation(state)) return state;
      return {
        ...state,
        phase: state.status === null ? 'unavailable' : 'idle',
        draftPositionsRad: state.status === null
          ? []
          : [...state.status.observedMotion.positionsRad],
        generation: state.generation + 1,
        intent: null,
        preview: null,
        ghost: null,
        ghostStale: false,
        operation: null,
        error: null,
      };
    case 'clear':
      return {
        ...initialRobotControlState,
        phase: 'unavailable',
        generation: state.generation + 1,
      };
  }
}

export function buildJointMotionIntent(
  status: ExecutionOwnerStatus,
  positionsRad: number[],
  requestId: string,
): JointMotionOperationIntent {
  if (!status.observedMotion.complete) {
    throw new Error('Authoritative joint state is incomplete.');
  }
  if (!positionsAreValid(status, positionsRad)) {
    throw new Error('The authored joint target is outside its retained limits.');
  }
  if (
    !status.directControl.frameRoleIds.includes(
      status.directControl.baseFrameRoleId,
    )
    || !status.directControl.frameRoleIds.includes(
      status.directControl.tcpFrameRoleId,
    )
  ) {
    throw new Error('The direct-control base/TCP frame contract is incomplete.');
  }
  if (
    !status.directControl.planningCapabilityIds.includes(
      'planning.motion.joint_goal',
    )
  ) {
    throw new Error('Joint-goal planning is unavailable for this target.');
  }
  if (requestId.trim() === '') {
    throw new Error('Robot Control request identity must not be empty.');
  }
  const jointNames = status.directControl.joints.map(
    (joint) => joint.sourceJointName,
  );
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: status.ownerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    program_id: status.directControl.programId,
    program_revision: status.directControl.programRevision,
    program_binding_id: status.directControl.programBindingId,
    program_run_id: status.directControl.programRunId,
    expected_graph_revision: status.graphRevision,
    expected_retained_bundle_id: status.retainedBundleId,
    expected_scene_revision: { ...status.sceneRevision },
    participants: [{
      component_role_id: status.directControl.componentRoleId,
      target_role_id: status.directControl.targetRoleId,
      required_capability_id: status.directControl.operationCapabilityId,
      action_span: null,
      payload: {
        kind: 'motion',
        move_id: `robot-control-${requestId}`,
        base_frame_role_id: status.directControl.baseFrameRoleId,
        tcp_frame_role_id: status.directControl.tcpFrameRoleId,
        tolerances: {
          joint_position_tol_rad: 0.05,
          translation_tol_m: 0.005,
          rotation_tol_rad: 0.05,
        },
        speed_profile: {
          velocity_scale: 1,
          acceleration_scale: 1,
          max_tcp_linear_speed_mps: null,
        },
        timeouts: {
          planning_timeout_s: 2,
          execution_timeout_s: 10,
        },
        cache_mode: 'default',
        planning_profile: 'balanced',
        target_mode: 'absolute',
        target_reference: null,
        offset_reference: null,
        motion: {
          kind: 'joint_goal',
          joint_names: jointNames,
          positions_rad: [...positionsRad],
        },
        contact_policy_id: null,
        per_joint_dynamics_limits: {},
        motion_interrupt_scopes: [],
        runtime_object_bindings: [],
      },
    }],
    coordination_policy: 'independent',
  };
}
