import { useEffect, useState } from 'react';
import { useShallow } from 'zustand/react/shallow';

import { followTargetController } from '../streaming/followTargetController';
import type { FollowTargetPhase } from '../streaming/followTargetController';
import { useCycleStore } from './cycleStore';
import { useDeploymentStore } from './deploymentStore';
import { useProgramStore } from './programStore';
import { useScenePolling, useSceneStateStore } from './sceneStateStore';

const ROBOT_COMMAND_SCENE_STALE_AFTER_MS = 2_000;

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
  streamingOwnsRobot: 'streaming owns robot',
} as const;

export interface RobotCommandAuthority {
  canCommand: boolean;
  reason: string | null;
  cycleState: string;
  followTargetPhase: FollowTargetPhase;
  unappliedDraftDomains: string[];
}

export interface DraftAuthorityStatus {
  hasUnappliedChanges: boolean;
  domains: string[];
  reason: string | null;
}

export interface RobotCommandAuthorityOptions {
  allowFollowTargetActive?: boolean;
  requireFreshScene?: boolean;
  requireAppliedDrafts?: boolean;
}

export interface RobotCommandAuthorityInputs {
  cycleState: string;
  followTargetPhase: FollowTargetPhase;
  draftAuthority: DraftAuthorityStatus;
  sceneError: string | null;
  lastUpdatedAtMs: number | null;
  nowMs: number;
  sceneStaleAfterMs?: number;
  options?: RobotCommandAuthorityOptions;
}

export function useFollowTargetPhase(): FollowTargetPhase {
  const [phase, setPhase] = useState<FollowTargetPhase>(() => followTargetController.phase());
  useEffect(() => followTargetController.subscribe(setPhase), []);
  return phase;
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
  followTargetPhase,
  draftAuthority,
  sceneError,
  lastUpdatedAtMs,
  nowMs,
  sceneStaleAfterMs = ROBOT_COMMAND_SCENE_STALE_AFTER_MS,
  options = {},
}: RobotCommandAuthorityInputs): RobotCommandAuthority {
  const {
    allowFollowTargetActive = false,
    requireFreshScene = true,
    requireAppliedDrafts = true,
  } = options;

  const blocked = (reason: string): RobotCommandAuthority => ({
    canCommand: false,
    reason,
    cycleState,
    followTargetPhase,
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
    if (lastUpdatedAtMs === null) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.waitingForSceneState);
    }
    if (nowMs - lastUpdatedAtMs > sceneStaleAfterMs) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale);
    }
  }

  if (cycleState === 'running') {
    return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot);
  }
  if (
    cycleState === 'direct_control'
    && (
      !allowFollowTargetActive
      || followTargetPhase === 'idle'
      || followTargetPhase === 'aborted'
    )
  ) {
    return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot);
  }
  if (
    !allowFollowTargetActive &&
    followTargetPhase !== 'idle' &&
    followTargetPhase !== 'aborted'
  ) {
    return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.streamingOwnsRobot);
  }
  return {
    canCommand: true,
    reason: null,
    cycleState,
    followTargetPhase,
    unappliedDraftDomains: draftAuthority.domains,
  };
}

export function useRobotCommandAuthority({
  allowFollowTargetActive = false,
  requireFreshScene = true,
  requireAppliedDrafts = true,
}: RobotCommandAuthorityOptions = {}): RobotCommandAuthority {
  // Authority decisions that depend on scene freshness must also own the
  // scene-state subscription. Otherwise a hidden/collapsed viewer could
  // leave global command buttons permanently blocked on "waiting for scene
  // state" even though the backend is healthy.
  useScenePolling();

  const cycleState = useCycleStore((s) => s.state);
  const sceneError = useSceneStateStore((s) => s.error);
  const lastUpdatedAtMs = useSceneStateStore((s) => s.lastUpdatedAtMs);
  const followTargetPhase = useFollowTargetPhase();
  const draftAuthority = useDraftAuthorityStatus();
  const [nowMs, setNowMs] = useState(() => Date.now());

  useEffect(() => {
    const timer = window.setInterval(() => setNowMs(Date.now()), 500);
    return () => window.clearInterval(timer);
  }, []);

  return evaluateRobotCommandAuthority({
    cycleState,
    followTargetPhase,
    draftAuthority,
    sceneError,
    lastUpdatedAtMs,
    nowMs,
    options: {
      allowFollowTargetActive,
      requireFreshScene,
      requireAppliedDrafts,
    },
  });
}
