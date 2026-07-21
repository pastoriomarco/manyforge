import type {
  ViewerFrameV1,
  ViewerManifestV1,
  ViewerPoseV1,
} from '../types/viewerProtocol.generated';
import { recordViewerTransformApply } from '../utils/viewerPerformance';

export interface ViewerFrameSnapshot {
  manifest: ViewerManifestV1;
  frame: ViewerFrameV1;
}

export type ViewerFrameApplier = (
  manifest: ViewerManifestV1,
  frame: ViewerFrameV1,
  previousFrame: ViewerFrameV1 | null,
  interpolationAlpha: number,
) => number | void;

export interface ViewerFrameApplyResult {
  applied: boolean;
  interpolationPending: boolean;
}

let latestManifest: ViewerManifestV1 | null = null;
let latestFrame: ViewerFrameV1 | null = null;
let interpolationFromFrame: ViewerFrameV1 | null = null;
let interpolationStartedAtMs: number | null = null;
let lastAppliedEpoch: string | null = null;
let lastAppliedSequence = -1;
let lastAppliedInterpolationAlpha = 1;
let lastWokenEpoch: string | null = null;
let lastWokenRenderRevision = -1;

const wakeSubscribers = new Set<() => void>();
const frameAppliers = new Set<ViewerFrameApplier>();

function assertFrameShape(manifest: ViewerManifestV1, frame: ViewerFrameV1): void {
  if (
    frame.producer_epoch !== manifest.producer_epoch
    || frame.manifest_revision !== manifest.manifest_revision
  ) {
    throw new Error('Viewer frame does not match the active manifest');
  }
  if (frame.entity_transforms.length !== manifest.entities.length) {
    throw new Error('Viewer entity transform count does not match manifest');
  }
  if (frame.frame_transforms.length !== manifest.frames.length) {
    throw new Error('Viewer frame transform count does not match manifest');
  }
  if (frame.robot_blocks.length !== manifest.robots.length) {
    throw new Error('Viewer robot block count does not match manifest');
  }
  manifest.robots.forEach((robot, index) => {
    const block = frame.robot_blocks[index];
    if (block.joint_positions.length !== robot.joint_names.length) {
      throw new Error('Viewer robot joint count does not match manifest');
    }
    if (block.link_transforms.length !== robot.link_order.length) {
      throw new Error('Viewer robot link count does not match manifest');
    }
  });
}

function notifyWakeSubscribers(): void {
  for (const subscriber of [...wakeSubscribers]) {
    subscriber();
  }
}

export function publishViewerManifest(manifest: ViewerManifestV1): boolean {
  if (
    latestManifest !== null
    && latestManifest.producer_epoch === manifest.producer_epoch
  ) {
    if (manifest.manifest_revision < latestManifest.manifest_revision) {
      throw new Error('Viewer manifest revision regressed');
    }
    if (
      manifest.manifest_revision === latestManifest.manifest_revision
      && manifest.content_hash !== latestManifest.content_hash
    ) {
      throw new Error('Viewer manifest revision was reused with different content');
    }
    if (manifest.manifest_revision === latestManifest.manifest_revision) {
      return false;
    }
  }
  const identityChanged = latestManifest === null
    || latestManifest.producer_epoch !== manifest.producer_epoch
    || latestManifest.manifest_revision !== manifest.manifest_revision;
  latestManifest = manifest;
  if (
    identityChanged
    && latestFrame !== null
    && (
      latestFrame.producer_epoch !== manifest.producer_epoch
      || latestFrame.manifest_revision !== manifest.manifest_revision
    )
  ) {
    latestFrame = null;
  }
  if (identityChanged) {
    interpolationFromFrame = null;
    interpolationStartedAtMs = null;
    lastAppliedEpoch = null;
    lastAppliedSequence = -1;
    lastAppliedInterpolationAlpha = 1;
    lastWokenEpoch = null;
    lastWokenRenderRevision = -1;
  }
  notifyWakeSubscribers();
  return true;
}

export function publishViewerFrame(frame: ViewerFrameV1, receivedAtMs = Date.now()): void {
  if (latestManifest === null) {
    throw new Error('Viewer frame arrived before its manifest');
  }
  assertFrameShape(latestManifest, frame);
  if (
    latestFrame !== null
    && latestFrame.producer_epoch === frame.producer_epoch
  ) {
    if (frame.frame_sequence < latestFrame.frame_sequence) {
      throw new Error('Viewer frame sequence regressed');
    }
    if (frame.frame_sequence === latestFrame.frame_sequence) {
      if (frame.render_revision !== latestFrame.render_revision) {
        throw new Error('Viewer frame sequence was reused with a different render revision');
      }
      return;
    }
  }
  const renderChanged = latestFrame !== null
    && latestFrame.producer_epoch === frame.producer_epoch
    && latestFrame.manifest_revision === frame.manifest_revision
    && latestFrame.render_revision !== frame.render_revision;
  interpolationFromFrame = renderChanged ? latestFrame : null;
  interpolationStartedAtMs = renderChanged ? receivedAtMs : null;
  latestFrame = frame;
  if (
    lastWokenEpoch !== frame.producer_epoch
    || lastWokenRenderRevision !== frame.render_revision
  ) {
    lastWokenEpoch = frame.producer_epoch;
    lastWokenRenderRevision = frame.render_revision;
    notifyWakeSubscribers();
  }
}

export function getLatestViewerSnapshot(): ViewerFrameSnapshot | null {
  if (latestManifest === null || latestFrame === null) return null;
  return { manifest: latestManifest, frame: latestFrame };
}

/**
 * Invalidate dynamic authority without discarding static topology.
 *
 * A transport disconnect makes the last pose unusable for authoring. Explicit
 * producer staleness remains in the latest frame as visible evidence while
 * command authority rejects it. Retaining the manifest lets a reconnect reuse
 * mesh topology when the producer epoch and manifest revision are unchanged.
 */
export function invalidateViewerFrame(): void {
  latestFrame = null;
  interpolationFromFrame = null;
  interpolationStartedAtMs = null;
  lastAppliedEpoch = null;
  lastAppliedSequence = -1;
  lastAppliedInterpolationAlpha = 1;
  lastWokenEpoch = null;
  lastWokenRenderRevision = -1;
  notifyWakeSubscribers();
}

export function subscribeViewerFrameWake(subscriber: () => void): () => void {
  wakeSubscribers.add(subscriber);
  return () => wakeSubscribers.delete(subscriber);
}

export function waitForViewerFrameGeneration(
  producerEpoch: string,
  frameSequence: number,
  timeoutMs: number,
): Promise<boolean> {
  const matches = (): boolean => {
    const snapshot = getLatestViewerSnapshot();
    return snapshot !== null
      && snapshot.frame.producer_epoch === producerEpoch
      && snapshot.frame.frame_sequence >= frameSequence;
  };
  if (matches()) return Promise.resolve(true);
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value: boolean): void => {
      if (settled) return;
      settled = true;
      globalThis.clearTimeout(timer);
      unsubscribe();
      resolve(value);
    };
    const unsubscribe = subscribeViewerFrameWake(() => {
      if (matches()) finish(true);
    });
    const timer = globalThis.setTimeout(
      () => finish(false),
      Math.max(1, Math.trunc(timeoutMs)),
    );
    // Close the subscribe-after-check race without waiting for another wake.
    if (matches()) finish(true);
  });
}

export function registerViewerFrameApplier(applier: ViewerFrameApplier): () => void {
  frameAppliers.add(applier);
  const snapshot = getLatestViewerSnapshot();
  if (snapshot !== null) {
    // A late-mounted binding starts at the current authoritative pose. It must
    // not replay a transition that began before the binding existed.
    applier(snapshot.manifest, snapshot.frame, null, 1);
    notifyWakeSubscribers();
  }
  return () => frameAppliers.delete(applier);
}

/**
 * Apply one capacity-one render sample immediately before R3F renders.
 *
 * The authoritative frame remains unchanged. Interpolation is a bounded,
 * render-only transition from the preceding self-contained frame; it never
 * extrapolates and is disabled as soon as the producer marks evidence stale.
 */
export function applyLatestViewerFrame(
  nowMs = Date.now(),
  maxInterpolationDurationMs = 0,
): ViewerFrameApplyResult {
  const snapshot = getLatestViewerSnapshot();
  if (snapshot === null) {
    return { applied: false, interpolationPending: false };
  }
  const { manifest, frame } = snapshot;
  const durationMs = (
    !frame.stale
    && interpolationFromFrame !== null
    && interpolationStartedAtMs !== null
    && Number.isFinite(maxInterpolationDurationMs)
  )
    ? Math.max(0, maxInterpolationDurationMs)
    : 0;
  const elapsedMs = interpolationStartedAtMs === null
    ? durationMs
    : Math.max(0, nowMs - interpolationStartedAtMs);
  const interpolationAlpha = durationMs > 0
    ? Math.min(1, elapsedMs / durationMs)
    : 1;
  const interpolationPending = interpolationAlpha < 1;
  if (
    lastAppliedEpoch === frame.producer_epoch
    && lastAppliedSequence === frame.frame_sequence
    && interpolationAlpha <= lastAppliedInterpolationAlpha
  ) {
    return { applied: false, interpolationPending };
  }
  lastAppliedEpoch = frame.producer_epoch;
  lastAppliedSequence = frame.frame_sequence;
  lastAppliedInterpolationAlpha = interpolationAlpha;
  let appliedTransforms = 0;
  for (const applier of [...frameAppliers]) {
    try {
      appliedTransforms += Math.max(0, Math.trunc(applier(
        manifest,
        frame,
        interpolationFromFrame,
        interpolationAlpha,
      ) ?? 0));
    } catch (error) {
      // A broken optional visual binding must not block the rest of the frame.
      console.warn('viewer frame binding failed', error);
    }
  }
  recordViewerTransformApply(appliedTransforms);
  if (!interpolationPending) {
    interpolationFromFrame = null;
    interpolationStartedAtMs = null;
  }
  return { applied: true, interpolationPending };
}

export function viewerRobotLinkPose(
  manifest: ViewerManifestV1,
  frame: ViewerFrameV1,
  robotIndex: number,
  linkName: string,
): ViewerPoseV1 | null {
  const robot = manifest.robots[robotIndex];
  const block = frame.robot_blocks[robotIndex];
  if (robot === undefined || block === undefined) return null;
  const linkIndex = robot.link_order.indexOf(linkName);
  return linkIndex >= 0 ? (block.link_transforms[linkIndex] ?? null) : null;
}

export function resetViewerFrameStore(): void {
  latestManifest = null;
  invalidateViewerFrame();
}

export const __viewerFrameStoreTest = {
  assertFrameShape,
  reset(): void {
    resetViewerFrameStore();
    wakeSubscribers.clear();
    frameAppliers.clear();
  },
};
