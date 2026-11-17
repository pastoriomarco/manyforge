import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  INITIAL_SCENE_TRACKING,
  acknowledgeReloadNotSent,
  dropInitialSceneExpectation,
  dropInitialSceneExpectationOf,
  expectInitialScene,
  initialSceneReloadReady,
  observeAppliedScene,
  observeDeploymentApplyEnd,
  observeInitialSceneCycle,
  type InitialSceneExpectation,
  type InitialSceneTracking,
} from '../state/initialSceneApplied';
import { usePublishWorkbenchNotices, type WorkbenchNotice } from '../state/workbenchNotices';
import type { ComposerHeadlessState } from '../types/headless';
import type { DeploymentWorkspaceController } from './useDeploymentWorkspace';
import type { ProgramRuntimeController } from './useProgramRuntime';

/**
 * Sends the one idle Reset with Reload initial scene that a Program or
 * Deployment Apply or Revert confirmed with that option, after it is Applied.
 * The Reset is the ordinary Program Reset: its refusal or failure is shown
 * where every Reset's is. Publishes the waiting and the not-sent notices.
 */
export function useInitialSceneReload({
  runtime,
  snapshot,
  authoringAllowed,
  deploymentApplyEnd,
  stoppedResetApply,
}: {
  runtime: ProgramRuntimeController;
  snapshot: ComposerHeadlessState | null;
  authoringAllowed: boolean | null;
  /** The last Deployment Apply or Revert command's end. */
  deploymentApplyEnd: DeploymentWorkspaceController['applyEnd'];
  /** The document of a Reset-then-Apply chain that stopped before its Apply. */
  stoppedResetApply: InitialSceneExpectation['target'] | null;
}) {
  const [tracking, setTracking] = useState<InitialSceneTracking>(INITIAL_SCENE_TRACKING);
  const { applied, observationError, loading } = runtime.state;
  const noneObserved = applied === null && observationError === null && !loading;
  useEffect(() => {
    setTracking((current) => observeAppliedScene(current, applied, noneObserved));
  }, [applied, noneObserved]);
  useEffect(() => {
    if (deploymentApplyEnd === null) return;
    setTracking((current) => observeDeploymentApplyEnd(current, deploymentApplyEnd.succeeded));
  }, [deploymentApplyEnd]);
  useEffect(() => {
    if (stoppedResetApply === null) return;
    setTracking((current) => dropInitialSceneExpectationOf(current, stoppedResetApply));
  }, [stoppedResetApply]);
  const cycle = snapshot?.programCycle ?? null;
  useEffect(() => {
    setTracking((current) => observeInitialSceneCycle(current, cycle));
  }, [cycle]);

  const latestRuntime = useRef(runtime);
  useEffect(() => {
    latestRuntime.current = runtime;
  });
  const ready = initialSceneReloadReady(tracking, {
    cycle,
    resetAllowed: runtime.runnerAllows('program_reset'),
    runnerBusy: runtime.runnerBusyReason !== null,
    commandPending: runtime.state.pending !== null,
    authoringAllowed,
  });
  useEffect(() => {
    if (!ready) return;
    setTracking((current) => ({ ...current, reload: null }));
    void latestRuntime.current.reset({ reloadInitialScene: true });
  }, [ready]);

  const expect = useCallback((expectation: InitialSceneExpectation) => {
    setTracking((current) => expectInitialScene(current, expectation));
  }, []);
  const drop = useCallback((expectation: InitialSceneExpectation) => {
    setTracking((current) => dropInitialSceneExpectation(current, expectation));
  }, []);
  const acknowledge = useCallback(() => {
    setTracking((current) => acknowledgeReloadNotSent(current));
  }, []);
  const reloadKind = tracking.reload?.kind ?? null;
  const notices = useMemo((): WorkbenchNotice[] => {
    if (reloadKind === 'waiting') {
      return [{
        id: 'initial-scene-reload.waiting',
        severity: 'info',
        domain: 'activation',
        title: 'Initial scene reload pending',
        message: 'A Reset with Reload initial scene is sent once the cycle is Idle and authoring is admitted.',
        busy: true,
      }];
    }
    if (reloadKind === 'not_sent') {
      return [{
        id: 'initial-scene-reload.not-sent',
        severity: 'warning',
        domain: 'activation',
        acknowledgeable: true,
        title: 'Initial scene not reloaded',
        message: 'A run or another command started first; the live Scene is unchanged. Reset with Reload initial scene to reload it.',
        actions: [{ id: 'acknowledge', label: 'Acknowledge', run: acknowledge }],
      }];
    }
    return [];
  }, [acknowledge, reloadKind]);
  usePublishWorkbenchNotices('initial-scene-reload', notices);
  return { tracking, expect, drop };
}

export type InitialSceneReloadController = ReturnType<typeof useInitialSceneReload>;
