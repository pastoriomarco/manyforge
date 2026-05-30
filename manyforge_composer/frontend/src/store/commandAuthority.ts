import { useEffect, useState } from 'react';
import { useShallow } from 'zustand/react/shallow';

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
  lastUpdatedAtMs: number | null;
  nowMs: number;
  sceneStaleAfterMs?: number;
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
  lastUpdatedAtMs,
  nowMs,
  sceneStaleAfterMs = ROBOT_COMMAND_SCENE_STALE_AFTER_MS,
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
    if (lastUpdatedAtMs === null) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.waitingForSceneState);
    }
    if (nowMs - lastUpdatedAtMs > sceneStaleAfterMs) {
      return blocked(ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale);
    }
  }

  // The cycle owns the robot while a program is running or an
  // intervention / direct-jog session is executing. Default Robot
  // Control authoring is a no-motion overlay; it does not command the
  // robot, so no streaming carve-out exists anymore.
  if (cycleState === 'running' || cycleState === 'direct_control') {
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
  useScenePolling();

  const cycleState = useCycleStore((s) => s.state);
  const sceneError = useSceneStateStore((s) => s.error);
  const lastUpdatedAtMs = useSceneStateStore((s) => s.lastUpdatedAtMs);
  const draftAuthority = useDraftAuthorityStatus();
  const [nowMs, setNowMs] = useState(() => Date.now());

  useEffect(() => {
    const timer = window.setInterval(() => setNowMs(Date.now()), 500);
    return () => window.clearInterval(timer);
  }, []);

  return evaluateRobotCommandAuthority({
    cycleState,
    draftAuthority,
    sceneError,
    lastUpdatedAtMs,
    nowMs,
    options: {
      requireFreshScene,
      requireAppliedDrafts,
    },
  });
}
