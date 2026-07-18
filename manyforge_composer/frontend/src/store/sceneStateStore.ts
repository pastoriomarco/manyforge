import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import type {
  PoseSourceObjectState,
  PoseSourceStatusState,
  SceneFrameState,
  SceneFramesResponse,
  SceneObjectState,
  SceneRobotState,
  SceneShapeState,
  SceneStateResponse,
} from '../types/api';
import type {
  ViewerDiagnosticsV1,
  ViewerFrameV1,
  ViewerManifestV1,
} from '../types/viewerProtocol.generated';
import {
  recordViewerFrameGap,
  recordViewerMaterialization,
  recordViewerResyncRequest,
  recordViewerTopologyRebuild,
  recordViewerWebSocketMessage,
  getViewerRendererProfile,
  startViewerPerformanceMonitoring,
  stopViewerPerformanceMonitoring,
  subscribeViewerRendererProfile,
  takeViewerPerformanceWindow,
} from '../utils/viewerPerformance';
import {
  getLatestViewerSnapshot,
  invalidateViewerFrame,
  publishViewerFrame,
  publishViewerManifest,
  resetViewerFrameStore,
} from './viewerFrameStore';

export type ViewerConnectionState = 'disconnected' | 'connecting' | 'connected' | 'stale';

interface SceneStateStore {
  scene: SceneStateResponse | null;
  frames: SceneFramesResponse | null;
  manifest: ViewerManifestV1 | null;
  serverDiagnostics: ViewerDiagnosticsV1 | null;
  connectionState: ViewerConnectionState;
  frameAvailable: boolean;
  droppedFrames: number;
  error: string | null;
  lastUpdatedAtMs: number | null;
  refreshInFlight: boolean;
  refresh: () => Promise<void>;
}

const CONTROL_SCHEMA = 'manyforge.viewer.control.v1';
const HEARTBEAT_MS = 5_000;
const TRANSPORT_LIVENESS_TIMEOUT_MS = 16_000;
const UI_PROJECTION_PERIOD_MS = 500;
const DEFAULT_LAYERS = [
  'canonical',
  'measured_robot',
  'measured_objects',
  'measured_tcp',
  'candidates',
  'diagnostics',
];

function viewerWsUrl(): string {
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const apiBase = import.meta.env.VITE_API_URL as string | undefined;
  if (apiBase) {
    return apiBase.replace(/^https?:/, proto) + '/ws/scene/viewer';
  }
  return `${proto}//${window.location.host}/ws/scene/viewer`;
}

function applyViewerServerDiagnostics(diagnostics: ViewerDiagnosticsV1): void {
  const latest = getLatestViewerSnapshot();
  // A diagnostics record can be queued immediately before a successful frame.
  // Never let that older health verdict overwrite newer visual evidence.
  const supersededByFrame = latest !== null
    && latest.frame.timestamp_ns > diagnostics.timestamp_ns;
  const producerUnhealthy = !diagnostics.producer_healthy && !supersededByFrame;
  useSceneStateStore.setState({
    serverDiagnostics: diagnostics,
    connectionState: producerUnhealthy ? 'stale' : currentViewerConnectionState(),
  });
}

function mergePoseSources(
  manifest: ViewerManifestV1,
  frame: ViewerFrameV1,
): PoseSourceStatusState[] {
  const dynamicById = new Map(
    frame.pose_source_states.map((source) => [source.pose_source_id, source]),
  );
  return manifest.pose_sources.map((staticSource) => {
    const dynamic = dynamicById.get(staticSource.pose_source_id);
    const dynamicObjects = new Map(
      (dynamic?.objects ?? []).map((objectState) => [objectState.object_id, objectState]),
    );
    const objects = (staticSource.objects ?? []).map((staticObject) => ({
      ...staticObject,
      ...(dynamicObjects.get(staticObject.object_id) ?? {}),
    })) as PoseSourceObjectState[];
    return {
      ...staticSource,
      status: dynamic?.status ?? 'stale',
      received_at_ns: dynamic?.received_at_ns,
      measured_tcp: dynamic?.measured_tcp,
      measured_robot: dynamic?.measured_robot,
      objects,
    } as PoseSourceStatusState;
  });
}

function materializeShape(
  shape: ViewerManifestV1['entities'][number]['shape'],
): SceneShapeState | null {
  if (shape === null) return null;
  return {
    type: shape.type,
    box_dimensions_m: shape.box_dimensions_m ?? undefined,
    sphere_radius_m: shape.sphere_radius_m ?? undefined,
    cylinder_radius_m: shape.cylinder_radius_m ?? undefined,
    cylinder_height_m: shape.cylinder_height_m ?? undefined,
    mesh_resource_uri: shape.mesh_resource_uri ?? undefined,
    mesh_scale: shape.mesh_scale ?? undefined,
    mesh_asset_url: shape.mesh_asset_url,
    mesh_asset_status: shape.mesh_asset_status,
  };
}

function materializePoseRecord(
  pose: ViewerFrameV1['entity_transforms'][number],
): Record<string, unknown> | null {
  if (pose === null) return null;
  return {
    position_m: pose.position_m,
    orientation_xyzw: pose.orientation_xyzw,
  };
}

export function materializeViewerState(
  manifest: ViewerManifestV1,
  frame: ViewerFrameV1,
): { scene: SceneStateResponse; frames: SceneFramesResponse } {
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

  let robot: SceneRobotState | null = null;
  const linkPoses: Record<string, { position_m: number[]; orientation_xyzw: number[] }> = {};
  const robotManifest = manifest.robots[0];
  const robotFrame = frame.robot_blocks[0];
  if (robotManifest && robotFrame) {
    if (robotFrame.joint_positions.length !== robotManifest.joint_names.length) {
      throw new Error('Viewer robot joint count does not match manifest');
    }
    if (robotFrame.link_transforms.length !== robotManifest.link_order.length) {
      throw new Error('Viewer robot link count does not match manifest');
    }
    const jointPositions: Record<string, number> = {};
    robotManifest.joint_names.forEach((name, index) => {
      const value = robotFrame.joint_positions[index];
      if (typeof value === 'number') jointPositions[name] = value;
    });
    robotManifest.link_order.forEach((name, index) => {
      const pose = robotFrame.link_transforms[index];
      if (pose) linkPoses[name] = pose;
    });
    robot = {
      ...robotManifest,
      joint_positions: jointPositions,
      tcp_pose_in_base: robotFrame.tcp_pose_in_base,
      state_source: robotFrame.state_source,
      source_timestamp_ns: robotFrame.source_timestamp_ns,
      source_age_ns: robotFrame.source_age_ns,
    };
  }

  const objects: SceneObjectState[] = manifest.entities.map((entity, index) => ({
    ...entity,
    shape: materializeShape(entity.shape),
    pose_in_universe: materializePoseRecord(frame.entity_transforms[index]),
  }));
  const poseSources = mergePoseSources(manifest, frame);
  const diagnostics = {
    mode: 'viewer_data_plane_v1',
    served_from_visual_snapshot: true,
    total_ms: 0,
    timings_ms: {},
    slow_steps: {},
    snapshot_sequence_number: frame.source_sequence,
    snapshot_stale: frame.stale,
  };

  const sceneFrames: SceneFrameState[] = manifest.frames.map((item, index) => {
    const pose = frame.frame_transforms[index];
    return {
      name: item.name,
      roles: item.roles,
      viewerVisible: item.viewer_visible,
      poseAvailable: pose !== null,
      poseInWorld: pose,
    };
  });
  return {
    scene: {
      robot,
      objects,
      link_poses: linkPoses,
      grippers: frame.gripper_states,
      pose_sources: poseSources,
      visual_state_diagnostics: diagnostics,
      object_axes_gizmo_size_m: manifest.object_axes_gizmo_size_m,
      timestamp_ns: frame.timestamp_ns,
    },
    frames: {
      frames: sceneFrames,
      viewerAvailable: true,
      frameGizmoSizeM: manifest.frame_axes_gizmo_size_m,
      visualStateDiagnostics: diagnostics,
      timestampNs: frame.timestamp_ns,
    },
  };
}

export const useSceneStateStore = create<SceneStateStore>((set) => ({
  scene: null,
  frames: null,
  manifest: null,
  serverDiagnostics: null,
  connectionState: 'disconnected',
  frameAvailable: false,
  droppedFrames: 0,
  error: null,
  lastUpdatedAtMs: null,
  refreshInFlight: false,
  // Explicit debug/authoring refresh only. Animation uses the viewer channel.
  refresh: async () => {
    if (useSceneStateStore.getState().refreshInFlight) return;
    set({ refreshInFlight: true });
    try {
      const [scene, frames] = await Promise.all([api.scene.state(), api.scene.frames()]);
      set({ scene, frames, error: null, lastUpdatedAtMs: Date.now() });
    } catch (error) {
      set({ error: error instanceof Error ? error.message : 'Scene state unavailable' });
    } finally {
      set({ refreshInFlight: false });
    }
  },
}));

let websocket: WebSocket | null = null;
let reconnectTimer: number | null = null;
let heartbeatTimer: number | null = null;
let transportLivenessTimer: number | null = null;
let uiProjectionTimer: number | null = null;
let lastServerMessageAtMs: number | null = null;
let lastUiProjectionAtMs = Number.NEGATIVE_INFINITY;
let reconnectAttempts = 0;
let connectionGeneration = 0;
let subscriberCount = 0;
let activeSubscriberCount = 0;
let collapsedSubscriberCount = 0;
let active = false;
let documentVisible = typeof document === 'undefined' ? true : !document.hidden;
let windowFocused = typeof document === 'undefined' ? true : document.hasFocus();

function resolveViewerVisibility(
  visible: boolean,
  focused: boolean,
  activeSubscribers: number,
  collapsedSubscribers: number,
): 'focused' | 'visible' | 'collapsed' | 'hidden' {
  if (!visible) return 'hidden';
  if (activeSubscribers === 0 && collapsedSubscribers > 0) {
    return 'collapsed';
  }
  return focused ? 'focused' : 'visible';
}

function viewerVisibility(): 'focused' | 'visible' | 'collapsed' | 'hidden' {
  return resolveViewerVisibility(
    documentVisible,
    windowFocused,
    activeSubscriberCount,
    collapsedSubscriberCount,
  );
}

function sendLease(): void {
  if (websocket?.readyState !== WebSocket.OPEN) return;
  const visibility = viewerVisibility();
  const activeVisibility = visibility === 'focused' || visibility === 'visible';
  websocket.send(JSON.stringify({
    type: 'lease',
    schema_version: CONTROL_SCHEMA,
    visibility,
    requested_rate_hz: activeVisibility
      ? getViewerRendererProfile().requestedRateHz
      : 0,
    layers: DEFAULT_LAYERS,
  }));
  websocket.send(JSON.stringify(takeViewerPerformanceWindow(visibility)));
}

function stopTimers(): void {
  if (heartbeatTimer !== null) window.clearInterval(heartbeatTimer);
  if (transportLivenessTimer !== null) window.clearInterval(transportLivenessTimer);
  heartbeatTimer = null;
  transportLivenessTimer = null;
  if (uiProjectionTimer !== null) window.clearTimeout(uiProjectionTimer);
  uiProjectionTimer = null;
}

function transportLivenessExpired(
  lastMessageAtMs: number | null,
  nowMs: number,
  timeoutMs = TRANSPORT_LIVENESS_TIMEOUT_MS,
): boolean {
  return lastMessageAtMs !== null && nowMs - lastMessageAtMs > timeoutMs;
}

function currentViewerConnectionState(): ViewerConnectionState {
  return getLatestViewerSnapshot()?.frame.stale ? 'stale' : 'connected';
}

function projectLatestViewerState(): void {
  uiProjectionTimer = null;
  const snapshot = getLatestViewerSnapshot();
  if (snapshot === null) return;
  const startedAt = performance.now();
  const materialized = materializeViewerState(snapshot.manifest, snapshot.frame);
  recordViewerMaterialization(performance.now() - startedAt);
  lastUiProjectionAtMs = performance.now();
  useSceneStateStore.setState({
    ...materialized,
    error: null,
  });
}

function scheduleUiProjection(force = false): void {
  if (uiProjectionTimer !== null) return;
  const nowMs = performance.now();
  const sceneMissing = useSceneStateStore.getState().scene === null;
  const delayMs = force || sceneMissing
    ? 0
    : Math.max(0, UI_PROJECTION_PERIOD_MS - (nowMs - lastUiProjectionAtMs));
  if (delayMs === 0) {
    projectLatestViewerState();
    return;
  }
  uiProjectionTimer = window.setTimeout(projectLatestViewerState, delayMs);
}

function startTimers(): void {
  stopTimers();
  heartbeatTimer = window.setInterval(sendLease, HEARTBEAT_MS);
  transportLivenessTimer = window.setInterval(() => {
    const socket = websocket;
    if (
      socket?.readyState !== WebSocket.OPEN
      || !transportLivenessExpired(lastServerMessageAtMs, Date.now())
    ) {
      return;
    }
    useSceneStateStore.setState({ error: 'Viewer transport heartbeat timed out' });
    socket.close(4000, 'viewer transport heartbeat timed out');
  }, 1_000);
}

function requestResync(): void {
  if (websocket?.readyState !== WebSocket.OPEN) return;
  recordViewerResyncRequest();
  websocket.send(JSON.stringify({
    type: 'resync',
    schema_version: CONTROL_SCHEMA,
  }));
}

function ownsViewerConnection<T>(
  candidate: T,
  current: T | null,
  generation: number,
  currentGeneration: number,
): boolean {
  return candidate === current && generation === currentGeneration;
}

function connect(): void {
  if (!active || typeof window === 'undefined') return;
  if (
    websocket?.readyState === WebSocket.CONNECTING
    || websocket?.readyState === WebSocket.OPEN
  ) return;
  const generation = connectionGeneration;
  useSceneStateStore.setState({ connectionState: 'connecting' });
  const socket = new WebSocket(viewerWsUrl());
  websocket = socket;

  socket.onopen = () => {
    if (!ownsViewerConnection(socket, websocket, generation, connectionGeneration)) return;
    reconnectAttempts = 0;
    lastServerMessageAtMs = Date.now();
    useSceneStateStore.setState({
      connectionState: 'connected',
      error: null,
      lastUpdatedAtMs: Date.now(),
    });
    sendLease();
    startTimers();
  };

  socket.onmessage = (event) => {
    if (!ownsViewerConnection(socket, websocket, generation, connectionGeneration)) return;
    // Any inbound message proves transport liveness. Transform frames may be
    // intentionally silent forever while a scene remains unchanged.
    lastServerMessageAtMs = Date.now();
    const rawMessage = String(event.data);
    let payload: unknown;
    try {
      payload = JSON.parse(rawMessage);
    } catch {
      requestResync();
      return;
    }
    if (!payload || typeof payload !== 'object') return;
    const message = payload as Record<string, unknown>;
    recordViewerWebSocketMessage(rawMessage, message.type);
    if (message.type === 'lease_ack') {
      useSceneStateStore.setState({
        connectionState: currentViewerConnectionState(),
        lastUpdatedAtMs: Date.now(),
      });
      return;
    }
    if (message.type === 'manifest') {
      if (message.schema_version !== 'manyforge.viewer.manifest.v1') return;
      const manifest = message as unknown as ViewerManifestV1;
      let identityChanged: boolean;
      try {
        identityChanged = publishViewerManifest(manifest);
      } catch (error) {
        useSceneStateStore.setState({
          error: error instanceof Error ? error.message : 'Invalid viewer manifest',
        });
        requestResync();
        return;
      }
      if (!identityChanged) return;
      recordViewerTopologyRebuild();
      useSceneStateStore.setState({
        manifest,
        ...(identityChanged
          ? { connectionState: 'connected', frameAvailable: false }
          : {}),
      });
      return;
    }
    if (
      message.type === 'diagnostics'
      && message.schema_version === 'manyforge.viewer.diagnostics.v1'
    ) {
      applyViewerServerDiagnostics(message as unknown as ViewerDiagnosticsV1);
      return;
    }
    if (message.type !== 'frame' || message.schema_version !== 'manyforge.viewer.frame.v1') {
      return;
    }
    const frame = message as unknown as ViewerFrameV1;
    const previousFrame = getLatestViewerSnapshot()?.frame ?? null;
    const manifest = useSceneStateStore.getState().manifest;
    if (
      manifest === null
      || manifest.producer_epoch !== frame.producer_epoch
      || manifest.manifest_revision !== frame.manifest_revision
    ) {
      requestResync();
      return;
    }
    try {
      const gap = previousFrame
        && previousFrame.producer_epoch === frame.producer_epoch
        ? Math.max(0, frame.frame_sequence - previousFrame.frame_sequence - 1)
        : 0;
      recordViewerFrameGap(gap);
      const receivedAtMs = Date.now();
      publishViewerFrame(frame, receivedAtMs);
      useSceneStateStore.setState((state) => ({
        connectionState: frame.stale ? 'stale' : 'connected',
        frameAvailable: true,
        droppedFrames: state.droppedFrames + gap,
        error: null,
        lastUpdatedAtMs: receivedAtMs,
      }));
      scheduleUiProjection();
    } catch (error) {
      useSceneStateStore.setState({
        error: error instanceof Error ? error.message : 'Invalid viewer frame',
      });
      requestResync();
    }
  };

  socket.onclose = () => {
    // A collapsed/expanded remount can close an old socket after its successor
    // is already live. Stale callbacks must never stop the successor's timers
    // or schedule a second connection.
    if (!ownsViewerConnection(socket, websocket, generation, connectionGeneration)) return;
    websocket = null;
    lastServerMessageAtMs = null;
    stopTimers();
    if (!active) return;
    invalidateViewerFrame();
    useSceneStateStore.setState({
      connectionState: 'disconnected',
      frameAvailable: false,
    });
    reconnectAttempts += 1;
    const delay = Math.min(30_000, 500 * 2 ** (reconnectAttempts - 1));
    reconnectTimer = window.setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, delay);
  };
  socket.onerror = () => {
    if (ownsViewerConnection(socket, websocket, generation, connectionGeneration)) {
      socket.close();
    }
  };
}

function startSubscription(): void {
  if (active) return;
  active = true;
  connectionGeneration += 1;
  startViewerPerformanceMonitoring();
  connect();
}

function stopSubscription(): void {
  active = false;
  connectionGeneration += 1;
  stopTimers();
  if (reconnectTimer !== null) window.clearTimeout(reconnectTimer);
  reconnectTimer = null;
  const socket = websocket;
  websocket = null;
  socket?.close();
  stopViewerPerformanceMonitoring();
  resetViewerFrameStore();
  lastServerMessageAtMs = null;
  lastUiProjectionAtMs = Number.NEGATIVE_INFINITY;
  useSceneStateStore.setState({
    scene: null,
    frames: null,
    manifest: null,
    serverDiagnostics: null,
    connectionState: 'disconnected',
    frameAvailable: false,
    lastUpdatedAtMs: null,
  });
}

if (typeof document !== 'undefined') {
  subscribeViewerRendererProfile(sendLease);
  document.addEventListener('visibilitychange', () => {
    documentVisible = !document.hidden;
    sendLease();
  });
  window.addEventListener('focus', () => {
    windowFocused = true;
    sendLease();
  });
  window.addEventListener('blur', () => {
    windowFocused = false;
    sendLease();
  });
}

/** Register visual demand; the first subscriber opens the bounded channel. */
export function useViewerDemand(
  enabled = true,
  mode: 'active' | 'collapsed' = 'active',
): void {
  useEffect(() => {
    if (!enabled) return undefined;
    subscriberCount += 1;
    if (mode === 'collapsed') collapsedSubscriberCount += 1;
    else activeSubscriberCount += 1;
    if (subscriberCount === 1) startSubscription();
    else sendLease();
    return () => {
      subscriberCount -= 1;
      if (mode === 'collapsed') collapsedSubscriberCount -= 1;
      else activeSubscriberCount -= 1;
      if (subscriberCount === 0) stopSubscription();
      else sendLease();
    };
  }, [enabled, mode]);
}

/** Materialize the exact latest authoritative frame only at an action boundary. */
export function getAuthoritativeViewerState(): {
  scene: SceneStateResponse;
  frames: SceneFramesResponse;
} | null {
  const snapshot = getLatestViewerSnapshot();
  if (
    snapshot === null
    || snapshot.frame.stale
    || useSceneStateStore.getState().connectionState !== 'connected'
  ) return null;
  return materializeViewerState(snapshot.manifest, snapshot.frame);
}

export const __viewerStoreTest = {
  applyViewerServerDiagnostics,
  currentViewerConnectionState,
  materializeViewerState,
  ownsViewerConnection,
  transportLivenessExpired,
  viewerVisibility: resolveViewerVisibility,
};
