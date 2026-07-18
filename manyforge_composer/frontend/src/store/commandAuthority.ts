import { useShallow } from 'zustand/react/shallow';

import { useCycleStore } from './cycleStore';
import { useDeploymentStore } from './deploymentStore';
import { useProgramStore } from './programStore';
import {
  useSceneStateStore,
  useViewerDemand,
  type ViewerConnectionState,
} from './sceneStateStore';

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

export const ROBOT_COMMAND_AUTHORITY_REASONS = {
  applyDraftChangesFirst: (domains: string[]): string =>
    domains.length > 0
      ? `apply draft changes first: ${domains.join(', ')}`
      : 'apply draft changes first',
  sceneUnavailable: (error: string): string => `scene unavailable: ${error}`,
  waitingForSceneState: 'waiting for scene state',
  sceneStateStale: 'scene state stale',
  cycleOwnsRobot: 'cycle owns robot',
} as const;

export interface RobotCommandAuthority {
  canCommand: boolean;
  reason: string | null;
  cycleState: string;
  unappliedDraftDomains: string[];
}

export interface DraftAuthorityStatus {
  hasUnappliedChanges: boolean;
  domains: string[];
  reason: string | null;
}

export interface RobotCommandAuthorityOptions {
  requireFreshScene?: boolean;
  requireAppliedDrafts?: boolean;
}

export interface RobotCommandAuthorityInputs {
  cycleState: string;
  draftAuthority: DraftAuthorityStatus;
  sceneError: string | null;
  sceneConnectionState: ViewerConnectionState;
  frameAvailable: boolean;
  options?: RobotCommandAuthorityOptions;
}

export function useDraftAuthorityStatus(): DraftAuthorityStatus {
  const programDraft = useProgramStore(
    useShallow((state) => ({
      nodes: state.nodes,
      edges: state.edges,
      flowToTree: state.flowToTree,
      appliedTree: state.appliedTree,
      sceneResources: state.sceneResources,
      appliedSceneResources: state.appliedSceneResources,
      programParameters: state.programParameters,
      appliedProgramParameters: state.appliedProgramParameters,
      blackboardNamespaces: state.blackboardNamespaces,
      appliedBlackboardNamespaces: state.appliedBlackboardNamespaces,
      blackboardKeys: state.blackboardKeys,
      appliedBlackboardKeys: state.appliedBlackboardKeys,
    })),
  );
  const deploymentDraft = useDeploymentStore(
    useShallow((state) => ({
      deployment: state.deployment,
      appliedDeployment: state.appliedDeployment,
    })),
  );

  // The selector includes nodes/edges so this hook re-renders when
  // flowToTree's source graph changes; flowToTree itself is a stable store
  // action.
  const draftTree = programDraft.flowToTree();
  const domains: string[] = [];
  if (signature(draftTree) !== signature(programDraft.appliedTree)) {
    domains.push('program graph');
  }
  if (signature(programDraft.sceneResources) !== signature(programDraft.appliedSceneResources)) {
    domains.push('scene resources');
  }
  if (
    signature(programDraft.programParameters)
    !== signature(programDraft.appliedProgramParameters)
  ) {
    domains.push('program parameters');
  }
  if (
    signature(programDraft.blackboardNamespaces)
    !== signature(programDraft.appliedBlackboardNamespaces)
    || signature(programDraft.blackboardKeys) !== signature(programDraft.appliedBlackboardKeys)
  ) {
    domains.push('blackboard');
  }
  if (signature(deploymentDraft.deployment) !== signature(deploymentDraft.appliedDeployment)) {
    domains.push('deployment');
  }

  return {
    hasUnappliedChanges: domains.length > 0,
    domains,
    reason: domains.length > 0
      ? ROBOT_COMMAND_AUTHORITY_REASONS.applyDraftChangesFirst(domains)
      : null,
  };
}

export function evaluateRobotCommandAuthority({
  cycleState,
  draftAuthority,
  sceneError,
  sceneConnectionState,
  frameAvailable,
  options = {},
}: RobotCommandAuthorityInputs): RobotCommandAuthority {
  const {
    requireFreshScene = true,
    requireAppliedDrafts = true,
  } = options;

  const blocked = (reason: string): RobotCommandAuthority => ({
    canCommand: false,
    reason,
    cycleState,
    unappliedDraftDomains: draftAuthority.domains,
  });

  if (requireAppliedDrafts && draftAuthority.hasUnappliedChanges) {
    return blocked(
      draftAuthority.reason
        ?? ROBOT_COMMAND_AUTHORITY_REASONS.applyDraftChangesFirst(draftAuthority.domains),
    );
  }

  if (requireFreshScene) {
    if (sceneError !== null) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.sceneUnavailable(sceneError));
    }
    if (sceneConnectionState === 'stale' || sceneConnectionState === 'disconnected') {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale);
    }
    if (!frameAvailable) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.waitingForSceneState);
    }
  }

  // The cycle owns the robot while a program is running, stopping, or an
  // intervention / direct-jog session is executing. Default Robot
  // Control authoring is a no-motion overlay; it does not command the
  // robot, so no streaming carve-out exists anymore.
  if (
    cycleState === 'running' ||
    cycleState === 'stopping' ||
    cycleState === 'direct_control'
  ) {
    return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot);
  }
  return {
    canCommand: true,
    reason: null,
    cycleState,
    unappliedDraftDomains: draftAuthority.domains,
  };
}

export function useRobotCommandAuthority({
  requireFreshScene = true,
  requireAppliedDrafts = true,
}: RobotCommandAuthorityOptions = {}): RobotCommandAuthority {
  // Authority decisions that depend on scene freshness must also own the
  // scene-state subscription. Otherwise a hidden/collapsed viewer could
  // leave global command buttons permanently blocked on "waiting for scene
  // state" even though the backend is healthy.
  useViewerDemand(requireFreshScene);

  const cycleState = useCycleStore((s) => s.state);
  const sceneError = useSceneStateStore((s) => s.error);
  const sceneConnectionState = useSceneStateStore((s) => s.connectionState);
  const frameAvailable = useSceneStateStore((s) => s.frameAvailable);
  const draftAuthority = useDraftAuthorityStatus();

  return evaluateRobotCommandAuthority({
    cycleState,
    draftAuthority,
    sceneError,
    sceneConnectionState,
    frameAvailable,
    options: {
      requireFreshScene,
      requireAppliedDrafts,
    },
  });
}
