import type { ViewerModelPresentation } from './viewerModelReadiness';
import type { WorkbenchNotice } from './workbenchNotices';

export interface ViewerNoticeInput {
  modelPresentation: ViewerModelPresentation;
  staleScenePreview: boolean;
  /** A mesh of the displayed robot model failed while the rest rendered. */
  robotMeshFailure: string | null;
  /** The structural model itself failed to load or belongs to an old model. */
  assetFailure: { kind: 'stale_model' | 'unavailable'; message: string } | null;
  /** A mesh of an optional preview (target ghost/candidate) failed. */
  previewMeshFailure: string | null;
  /** The explicitly enabled point-cloud layer failed. */
  pointCloudFailure: string | null;
  /**
   * A requested reload of the model geometry is still running. The geometry
   * errors stay (and stay reported) until the reload verifiably succeeds.
   */
  geometryRetrying: boolean;
  retryModel: () => void;
  reloadModel: () => void;
}

/**
 * Viewer notices by what the operator can rely on. An unavailable, mismatched
 * or unverifiable view of the applied robot or Scene is an error: the
 * operator cannot judge the current situation from it. Only the expected,
 * related gap of a known controlled transition is marked
 * ``expectedDuringTransition``; a missing optional visual detail is a
 * warning. Retry/Reload only request fresh data: each error clears when its
 * source confirms valid current data, never on acknowledgement.
 */
export function viewerNotices(input: ViewerNoticeInput): WorkbenchNotice[] {
  const notices: WorkbenchNotice[] = [];
  const retryModel = {
    id: 'retry-model-projections',
    label: 'Retry model projections',
    title: 'Request fresh model projections. This does not apply or change runtime state.',
    run: input.retryModel,
  };
  const reloadModel = {
    id: 'reload-active-model',
    label: input.geometryRetrying ? 'Reloading…' : 'Reload active model',
    title: 'Clear cached meshes and reload the active model. This does not apply or change runtime state.',
    disabled: input.geometryRetrying,
    run: input.reloadModel,
  };
  const model = input.modelPresentation;
  if (model.status === 'synchronizing') {
    if (model.reason === 'identity_mismatch' || model.reason === 'projection_stale') {
      notices.push({
        id: 'viewer.model',
        severity: 'error',
        domain: 'viewer',
        title: 'Applied robot model not verified',
        message: model.message,
        actions: [retryModel],
        expectedDuringTransition: true,
        incidentKind: 'viewer_model_unavailable',
      });
    } else {
      notices.push({
        id: 'viewer.model',
        severity: 'info',
        domain: 'viewer',
        title: 'Loading Applied robot model',
        message: `${model.message} The view is not current until it loads.`,
        busy: true,
      });
    }
  } else if (model.status === 'unavailable') {
    notices.push({
      id: 'viewer.model',
      severity: 'error',
      domain: 'viewer',
      title: 'Applied robot model unavailable',
      message: model.message,
      actions: [retryModel],
      incidentKind: 'viewer_model_unavailable',
    });
  }
  if (input.staleScenePreview) {
    notices.push({
      id: 'viewer.scene',
      severity: 'error',
      domain: 'viewer',
      title: 'Applied Scene not verified',
      message: 'The last coherent Scene preview is shown; its current Manager identity is not verified. Do not rely on it for the current cell state.',
      expectedDuringTransition: true,
      incidentKind: 'viewer_scene_unverified',
    });
  }
  if (input.assetFailure !== null) {
    const stale = input.assetFailure.kind === 'stale_model';
    notices.push({
      id: 'viewer.asset',
      severity: 'error',
      domain: 'viewer',
      title: stale ? 'Active model changed' : 'Robot geometry unavailable',
      message: stale
        ? 'A keyed mesh belongs to an inactive model. Reload the active model.'
        : input.assetFailure.message,
      actions: [reloadModel],
      busy: input.geometryRetrying,
      expectedDuringTransition: stale,
      incidentKind: 'viewer_geometry_unavailable',
    });
  } else if (input.robotMeshFailure !== null && model.status === 'ready') {
    notices.push({
      id: 'viewer.robot-mesh',
      severity: 'error',
      domain: 'viewer',
      title: 'Robot mesh unavailable',
      message: `Part of the robot geometry is missing, so the displayed robot is incomplete. ${input.robotMeshFailure}`,
      actions: [reloadModel],
      busy: input.geometryRetrying,
      incidentKind: 'viewer_geometry_unavailable',
    });
  }
  if (input.previewMeshFailure !== null) {
    notices.push({
      id: 'viewer.preview-mesh',
      severity: 'warning',
      domain: 'viewer',
      title: 'Preview mesh unavailable',
      message: `An optional target preview mesh could not be rendered. ${input.previewMeshFailure}`,
    });
  }
  if (input.pointCloudFailure !== null) {
    notices.push({
      id: 'viewer.point-cloud',
      severity: 'warning',
      domain: 'viewer',
      title: 'Point-cloud layer unavailable',
      message: input.pointCloudFailure,
    });
  }
  return notices;
}
