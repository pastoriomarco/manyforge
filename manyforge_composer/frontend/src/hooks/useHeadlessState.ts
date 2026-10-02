import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import { viewerModelRefreshKey } from '../state/nodeEnvironmentRefresh';
import { EMPTY_OPERATION_PROGRESS, parseOperationProgress, type OperationProgressSample } from '../utils/operationProgress';
import {
  parseApiErrorDto,
  parseAuthoringWorkspaceIdentity,
  parseComposerHeadlessState,
  parseComponentOverviewProjection,
  parseExecutionControlStatus,
  parseHeadlessRuntimeSnapshot,
  parseOperatorAlarmSnapshot,
  parseProgramCycleStatus,
} from '../api/parsers';
import type {
  AuthoringWorkspaceIdentity,
  ComposerHeadlessState,
} from '../types/headless';
import type {
  ExecutionObservationFrame,
  ExecutionObservationTarget,
  ExecutionObservationReader,
  ExecutionObservationTransportPhase,
  ExecutionObservationTransportStatus,
} from '../types/viewerObservation';
import {
  recordViewerObservationApplied,
  recordViewerObservationReceived,
  setViewerQualificationTransport,
  type ViewerQualificationTransportState,
} from '../utils/viewerPerformance';

const RECONNECT_DELAY_MS = 1000;
// The server already coalesces the shared shell projection. Retain a small
// browser-side batch for React, but do not add a visible tenth of a second to
// execution-control settlement.
export const HEADLESS_SHELL_PUBLICATION_MS = 50;

const EXECUTION_OBSERVATION_SCHEMA = 'manyforge.execution_observation_frame.v3';
const EXECUTION_OBSERVATION_CREDIT_SCHEMA = 'manyforge.execution_observation_credit.v1';
const EXECUTION_OBSERVATION_RENEWAL_MS = 15_000;
const observationTextEncoder = new TextEncoder();
function isObservationText(value: unknown): value is string {
  // At most three UTF-8 bytes per UTF-16 code unit. Ordinary short identifiers
  // need no per-coordinate buffer allocation to enforce the wire byte limit.
  return typeof value === 'string' && value.length > 0 && value.length <= 2048
    && (value.length <= 682 || observationTextEncoder.encode(value).length <= 2048);
}

function sameExecutionObservationFrame(
  current: ExecutionObservationFrame,
  candidate: ExecutionObservationFrame,
): boolean {
  return current.ownerEpoch === candidate.ownerEpoch
    && current.sourceEpoch === candidate.sourceEpoch
    && current.sourceSequence === candidate.sourceSequence
    && current.cellId === candidate.cellId
    && current.retainedBundleId === candidate.retainedBundleId
    && current.graphRevision === candidate.graphRevision
    && current.targets.length === candidate.targets.length
    && current.targets.every((target, index) => {
      const next = candidate.targets[index];
      return target.controllableComponentId === next.controllableComponentId
        && target.commandTargetId === next.commandTargetId
        && target.sourceEpoch === next.sourceEpoch
        && sameStrings(target.sourceJointNames, next.sourceJointNames)
        && sameStrings(target.compiledJointNames, next.compiledJointNames)
        && sameStrings(target.coordinateKinds, next.coordinateKinds)
        && target.positionsSi.length === next.positionsSi.length
        && target.positionsSi.every((value, joint) => value === next.positionsSi[joint]);
    });
}

export function parseExecutionObservationFrame(raw: unknown): ExecutionObservationFrame {
  const object = (value: unknown, keys: string[]): Record<string, unknown> => {
    if (value === null || typeof value !== 'object' || Array.isArray(value)
      || Object.keys(value).length !== keys.length
      || Object.keys(value).some((key) => !keys.includes(key))) {
      throw new Error('Execution observation frame has an invalid shape.');
    }
    return value as Record<string, unknown>;
  };
  const record = object(raw, [
    'cell_id',
    'graph_revision',
    'owner_epoch',
    'received_at_monotonic_us',
    'retained_bundle_id',
    'schema_version',
    'source_epoch',
    'source_sequence',
    'targets',
  ]);
  const text = (key: string, source = record): string => {
    const value = source[key];
    if (!isObservationText(value)) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return value;
  };
  const integer = (key: string, source = record): number => {
    const value = source[key];
    if (!Number.isSafeInteger(value) || (value as number) < 0) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return value as number;
  };
  const strings = (key: string, source: Record<string, unknown>): string[] => {
    const value = source[key];
    if (!Array.isArray(value) || value.length === 0 || value.length > 1024
      || value.some((item) => !isObservationText(item))
      || new Set(value).size !== value.length) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return [...value] as string[];
  };
  if (record.schema_version !== EXECUTION_OBSERVATION_SCHEMA) {
    throw new Error('Execution observation schema is unsupported.');
  }
  // H1a-IF.2: a cell may have no observable coordinate at all.
  if (!Array.isArray(record.targets) || record.targets.length > 64) {
    throw new Error('Execution observation target count is invalid.');
  }
  const targets: ExecutionObservationTarget[] = record.targets.map((rawTarget) => {
    const target = object(rawTarget, ['controllable_component_id', 'command_target_id',
      'operation_capability_id', 'source_epoch', 'source_sequence', 'source_joint_names',
      'compiled_joint_names', 'coordinate_kinds', 'positions_si']);
    const sourceJointNames = strings('source_joint_names', target);
    const compiledJointNames = strings('compiled_joint_names', target);
    const kinds = target.coordinate_kinds;
    const positions = target.positions_si;
    if (!Array.isArray(positions) || !Array.isArray(kinds)
      || positions.some((value) => typeof value !== 'number' || !Number.isFinite(value))
      || kinds.some((kind) => kind !== 'angular' && kind !== 'linear')
      || positions.length !== sourceJointNames.length || kinds.length !== sourceJointNames.length
      || compiledJointNames.length !== sourceJointNames.length) {
      throw new Error('Execution observation joint vectors are invalid.');
    }
    return Object.freeze({
      controllableComponentId: text('controllable_component_id', target),
      commandTargetId: text('command_target_id', target),
      operationCapabilityId: text('operation_capability_id', target),
      sourceEpoch: text('source_epoch', target),
      sourceSequence: integer('source_sequence', target),
      sourceJointNames, compiledJointNames,
      coordinateKinds: [...kinds] as Array<'angular' | 'linear'>,
      positionsSi: [...positions] as number[],
    });
  });
  const addresses = targets.map((target) => JSON.stringify([target.controllableComponentId, target.commandTargetId]));
  const compiled = targets.flatMap((target) => target.compiledJointNames);
  if (new Set(addresses).size !== addresses.length || new Set(compiled).size !== compiled.length || compiled.length > 1024) {
    throw new Error('Execution observation targets overlap or exceed the coordinate bound.');
  }
  return Object.freeze({
    schemaVersion: EXECUTION_OBSERVATION_SCHEMA,
    ownerEpoch: text('owner_epoch'),
    cellId: text('cell_id'),
    retainedBundleId: text('retained_bundle_id'),
    graphRevision: text('graph_revision'),
    sourceEpoch: text('source_epoch'),
    sourceSequence: integer('source_sequence'),
    receivedAtMonotonicUs: integer('received_at_monotonic_us'),
    targets,
  });
}

export interface HeadlessStateResult {
  snapshot: ComposerHeadlessState | null;
  error: string | null;
  transportStatus: ControlTransportStatus;
  transportError: string | null;
  semanticStatus: ControlSemanticStatus;
  semanticReason: string | null;
  reconnect: () => void;
}

export interface HeadlessStateAdmission {
  accepted: boolean;
  reason: string | null;
}

const CONTROL_PROTOCOL_VERSION = 'manyforge.execution_control.v2';
const EXECUTION_COMMAND_SCHEMA = 'manyforge.execution_command.v1';
const HTTP_ADMITTED_COMMANDS = new Set<ExecutionControlCommandKind>([
  'program_run', 'program_motion_start', 'program_motion_resume',
  'program_resume_full_stop', 'operation_start', 'operation_resume',
]);
const CONTROL_HEARTBEAT_PERIOD_NS = 250_000_000;
const CONTROL_HEARTBEAT_DELAYED_NS = 500_000_000;
const CONTROL_HEARTBEAT_STALE_NS = 1_000_000_000;
const CONTROL_COMMAND_DELIVERY_NS = 1_000_000_000;
const CONTROL_MAXIMUM_MESSAGE_BYTES = 64 * 1024;
const CONTROL_MAXIMUM_MESSAGES_PER_SECOND = 64;

function canonicalPositiveU64(value: unknown): value is string {
  if (typeof value !== 'string' || !/^[1-9]\d*$/.test(value)) return false;
  try {
    return BigInt(value) <= 18_446_744_073_709_551_615n;
  } catch {
    return false;
  }
}

export type ExecutionControlCommandKind =
  | 'program_run'
  | 'program_motion_start'
  | 'program_motion_stop'
  | 'program_motion_resume'
  | 'program_full_stop'
  | 'program_stop'
  | 'program_reset'
  | 'program_resume_full_stop'
  | 'program_discard_full_stop'
  | 'program_runtime_override_stage'
  | 'program_runtime_override_activate'
  | 'program_runtime_override_clear'
  | 'program_runtime_override_promote'
  | 'program_runtime_blackboard_write'
  | 'program_manual_result_stage'
  | 'program_manual_handover'
  | 'gripper_confirm'
  | 'operation_start'
  | 'operation_stop'
  | 'operation_pause'
  | 'operation_resume'
  | 'operation_reset'
  | 'operation_acknowledge_completed'
  | 'cell_full_stop'
  | 'execution_recovery'
  | 'stop_recovery'
  | 'runtime_scale';

const EXECUTION_OWNER_FENCED_COMMANDS = new Set<ExecutionControlCommandKind>([
  'operation_start',
  'operation_stop',
  'operation_pause',
  'operation_resume',
  'operation_reset',
  'operation_acknowledge_completed',
  'cell_full_stop',
  'stop_recovery',
  'runtime_scale',
]);

export function executionCommandPayload(
  commandKind: ExecutionControlCommandKind,
  payload: Record<string, unknown>,
  executionOwnerEpoch: string | null | undefined,
): Record<string, unknown> {
  if (!EXECUTION_OWNER_FENCED_COMMANDS.has(commandKind)) return payload;
  if (typeof executionOwnerEpoch !== 'string' || executionOwnerEpoch === '') {
    throw new Error('Execution owner identity is unavailable.');
  }
  return {
    ...payload,
    expected_execution_owner_epoch: executionOwnerEpoch,
  };
}

interface ControlFreshnessContext {
  context_id: string;
  receiver_incarnation: string;
  semantic_owner_epoch: string;
  execution_control_revision: number;
  issued_server_sequence: string;
  valid_for_ns: number;
}

interface CurrentExecutionControl {
  receiver_incarnation: string;
  semantic_owner_epoch: string;
  execution_control_revision: number;
  freshness: ControlFreshnessContext;
}

export type ControlTransportStatus =
  | 'connecting'
  | 'connected'
  | 'delayed'
  | 'stale'
  | 'disconnected';
export type ControlSemanticStatus = 'current' | 'synchronizing' | 'unavailable';
export type ControlClientIncidentKind =
  | 'browser_render_delayed'
  | 'command_delivery_unconfirmed'
  | 'control_disconnected'
  | 'history_detail_request_failed'
  | 'history_request_failed'
  | 'viewer_model_unavailable'
  | 'viewer_scene_unverified'
  | 'viewer_observation_stale'
  | 'viewer_geometry_unavailable';

interface ClientIncidentState {
  id: string;
  kind: ControlClientIncidentKind;
  reasonCode: string;
  startedMs: number;
  occurrenceCount: number;
  maximumDelayNs: number | null;
  activationSent: boolean;
}

export interface ExecutionCommandReceipt {
  correlationId: string;
  disposition: 'received' | 'rejected' | 'joined' | 'already_current' | 'no_effect';
  code: string;
  result: unknown;
  receivedServerSequence: string;
  receivedMonotonicNs: string;
  emittedMonotonicNs: string;
}

export interface RuntimeFrameIdentity {
  ownerEpoch: string;
  generation: number;
}

export function sameAuthoringWorkspaceIdentity(
  left: AuthoringWorkspaceIdentity,
  right: AuthoringWorkspaceIdentity,
): boolean {
  return left.workspaceEpoch === right.workspaceEpoch
    && left.workspaceRevision === right.workspaceRevision
    && left.programDraftId === right.programDraftId
    && left.programDraftRevision === right.programDraftRevision
    && left.deploymentDraftId === right.deploymentDraftId
    && left.deploymentDraftRevision === right.deploymentDraftRevision;
}

export function admitHeadlessState(
  current: ComposerHeadlessState | null,
  candidate: ComposerHeadlessState,
): HeadlessStateAdmission {
  if (current === null) return { accepted: true, reason: null };
  if (candidate.receiverEpoch === current.receiverEpoch) {
    if (candidate.snapshotSequence < current.snapshotSequence) {
      return { accepted: false, reason: 'Headless snapshot sequence regressed.' };
    }
    if (candidate.snapshotSequence === current.snapshotSequence
      && !sameAuthoringWorkspaceIdentity(
        candidate.authoringWorkspace,
        current.authoringWorkspace,
      )) {
      return {
        accepted: false,
        reason: 'Headless authoring workspace identity changed without a local sequence advance.',
      };
    }
  }
  return { accepted: true, reason: null };
}

function websocketUrl(path: '/ws/control' | '/ws/scene/viewer'): string {
  const resolved = new URL(
    resolveStructuralAssetUrl(path),
    window.location.href,
  );
  resolved.protocol = resolved.protocol === 'https:' ? 'wss:' : 'ws:';
  return resolved.toString();
}

export function browserStableIdentity(storageKey: string, prefix: string): string {
  const valid = (value: string | null): value is string =>
    value !== null && new RegExp(`^${prefix}\\.[A-Za-z0-9_-]{8,256}$`).test(value);
  try {
    const stored = globalThis.localStorage?.getItem(storageKey);
    if (valid(stored)) return stored;
    const random = globalThis.crypto?.randomUUID?.()
      ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const value = `${prefix}.${random}`;
    globalThis.localStorage?.setItem(storageKey, value);
    return value;
  } catch {
    const random = globalThis.crypto?.randomUUID?.()
      ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    return `${prefix}.${random}`;
  }
}

function sameStrings(current: string[], candidate: string[]): boolean {
  return current.length === candidate.length
    && current.every((value, index) => value === candidate[index]);
}

function sameSourceSection(
  current: ComposerHeadlessState['runtime']['sections']['manager'],
  candidate: ComposerHeadlessState['runtime']['sections']['manager'],
  includeSourceGeneration: boolean,
): boolean {
  return current.available === candidate.available
    && current.freshness === candidate.freshness
    && sameStrings(current.knownGaps, candidate.knownGaps)
    && current.sourceOwnerEpoch === candidate.sourceOwnerEpoch
    && current.sourceOwnerId === candidate.sourceOwnerId
    && (!includeSourceGeneration
      || current.sourceGeneration === candidate.sourceGeneration);
}

export function sameViewerProjection(
  current: ComposerHeadlessState | null,
  candidate: ComposerHeadlessState,
): boolean {
  if (current === null) return false;
  return current.receiverEpoch === candidate.receiverEpoch
    // Manager completion can arrive after the final Scene update. Publish
    // the same identity/readiness facts that wake the structural model reads,
    // without waking the viewer for source counters or observation ages.
    && viewerModelRefreshKey(current) === viewerModelRefreshKey(candidate)
    && current.runtime.projectionOwnerEpoch === candidate.runtime.projectionOwnerEpoch
    && current.runtime.projectionOwnerId === candidate.runtime.projectionOwnerId
    && current.runtime.complete === candidate.runtime.complete
    && sameStrings(current.runtime.reasons, candidate.runtime.reasons)
    && sameStrings(
      current.runtime.unavailableSections,
      candidate.runtime.unavailableSections,
    )
    && sameSourceSection(
      current.runtime.sections.logicalScene,
      candidate.runtime.sections.logicalScene,
      true,
    )
    && sameSourceSection(
      current.runtime.sections.sceneRuntime,
      candidate.runtime.sections.sceneRuntime,
      true,
    );
}

type Listener = () => void;

interface HeadlessSourceQualificationRecord {
  enabled: boolean;
  disposed: boolean;
  socketActive: boolean;
}

const headlessSourceRecords = new Set<HeadlessSourceQualificationRecord>();
let headlessSocketCreations = 0;
let headlessMaxActiveSockets = 0;

function headlessQualificationSnapshot(): ViewerQualificationTransportState {
  const activeSources = [...headlessSourceRecords].filter(
    (record) => record.enabled && !record.disposed,
  );
  const activeSockets = activeSources.filter((record) => record.socketActive).length;
  return {
    socketPath: '/ws/scene/viewer',
    sourceCount: activeSources.length,
    activeSockets,
    socketCreations: headlessSocketCreations,
    maxActiveSockets: headlessMaxActiveSockets,
  };
}

function publishHeadlessQualification(): void {
  setViewerQualificationTransport(headlessQualificationSnapshot());
}

export function getHeadlessStateQualificationSnapshot(): ViewerQualificationTransportState {
  return headlessQualificationSnapshot();
}

/**
 * One low-rate shell transport plus one isolated bounded viewer transport.
 * This source performs parsing, admission, and scheduling only.
 */
export class HeadlessStateSource implements ExecutionObservationReader {
  private enabled = false;
  private enabledGeneration = 0;
  private disposed = false;
  private serial = 0;
  private socket: WebSocket | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private controlHeartbeatTimer: ReturnType<typeof setTimeout> | null = null;
  private controlSessionId: string | null = null;
  private controlReceiverIncarnation: string | null = null;
  private controlSnapshotBarrierPending = true;
  private controlServerSequence = 0n;
  private controlClientSequence = 0;
  private controlHeartbeatConfirmed = false;
  private lastControlEchoSequence = 0;
  private lastReceivedClientSequence = 0n;
  private controlHeartbeatFreshness: ControlFreshnessContext | null = null;
  private commandDeliveryFaultSessionId: string | null = null;
  private controlTransportStatus: ControlTransportStatus = 'disconnected';
  private controlTransportError: string | null = null;
  private controlSemanticStatus: ControlSemanticStatus = 'synchronizing';
  private controlSemanticReason: string | null = 'control_projection_synchronizing';
  private executionControl: CurrentExecutionControl | null = null;
  private executionTargetsWire: unknown = null;
  private ordinaryControlPending: string | null = null;
  private runtimeScaleControlPending: string | null = null;
  private readonly commandReceipts = new Map<string, {
    resolve: (receipt: ExecutionCommandReceipt) => void;
    reject: (reason: Error) => void;
    clientSequence: bigint;
    deliveryTimer: ReturnType<typeof setTimeout> | null;
  }>();
  private readonly clientIncidents = new Map<ControlClientIncidentKind, ClientIncidentState>();
  private observationSerial = 0;
  private observationSocket: WebSocket | null = null;
  private observationReconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private observationRenewalTimer: ReturnType<typeof setTimeout> | null = null;
  private viewerDemandGeneration = 0;
  private viewerDemand = false;
  private shellTimer: ReturnType<typeof setTimeout> | null = null;
  private viewerFrame: number | ReturnType<typeof setTimeout> | null = null;
  private observationFrame: number | ReturnType<typeof setTimeout> | null = null;
  private latest: ComposerHeadlessState | null = null;
  private latestExecutionObservation: ExecutionObservationFrame | null = null;
  private appliedExecutionObservation: ExecutionObservationFrame | null = null;
  private presentedViewer: ComposerHeadlessState | null = null;
  private presentedFrame: RuntimeFrameIdentity | null = null;
  private latestError: string | null = null;
  private observationTransportStatus: ExecutionObservationTransportStatus = {
    phase: 'disabled',
    message: null,
  };
  private shellResult: HeadlessStateResult;
  private viewerResult: HeadlessStateResult;
  private readonly qualificationRecord: HeadlessSourceQualificationRecord = {
    enabled: false,
    disposed: false,
    socketActive: false,
  };
  private readonly shellListeners = new Set<Listener>();
  private operationProgress: readonly OperationProgressSample[] = EMPTY_OPERATION_PROGRESS;
  private readonly operationProgressListeners = new Set<Listener>();
  readonly getOperationProgress = () => this.operationProgress;
  readonly subscribeOperationProgress = (listener: Listener) => {
    this.operationProgressListeners.add(listener);
    return () => this.operationProgressListeners.delete(listener);
  };
  private readonly viewerListeners = new Set<Listener>();
  private readonly executionObservationListeners = new Set<Listener>();
  private readonly observationTransportListeners = new Set<Listener>();
  constructor() {
    headlessSourceRecords.add(this.qualificationRecord);
    publishHeadlessQualification();
    this.shellResult = this.result();
    this.viewerResult = this.shellResult;
  }

  readonly subscribeShell = (listener: Listener) => {
    this.shellListeners.add(listener);
    return () => this.shellListeners.delete(listener);
  };

  readonly subscribeViewer = (listener: Listener) => {
    this.viewerListeners.add(listener);
    return () => this.viewerListeners.delete(listener);
  };

  readonly subscribeExecutionObservation = (listener: Listener) => {
    this.executionObservationListeners.add(listener);
    return () => this.executionObservationListeners.delete(listener);
  };

  readonly getShellSnapshot = () => this.shellResult;
  readonly getViewerSnapshot = () => this.viewerResult;
  readonly getLatestExecutionObservation = () => this.latestExecutionObservation;
  readonly markExecutionObservationApplied = (
    frame: ExecutionObservationFrame,
  ): void => {
    if (frame !== this.latestExecutionObservation) return;
    if (frame === this.appliedExecutionObservation) {
      // The frame may have been acknowledged as unrenderable while the
      // structural model was refreshing. Once that same frame becomes
      // renderable, clear the synchronization banner even though no second
      // transport credit is needed.
      this.publishObservationTransportStatus('live', null);
      return;
    }
    this.appliedExecutionObservation = frame;
    recordViewerObservationApplied({
      ownerEpoch: frame.ownerEpoch,
      sourceEpoch: frame.sourceEpoch,
      sourceSequence: frame.sourceSequence,
      receivedAtComposerMonotonicUs: frame.receivedAtMonotonicUs,
    });
    this.sendObservationCredit(frame);
    this.publishObservationTransportStatus('live', null);
  };
  readonly getExecutionObservationTransportStatus = () =>
    this.observationTransportStatus;
  readonly rejectExecutionObservation = (frame: ExecutionObservationFrame): void => {
    if (frame !== this.latestExecutionObservation || this.observationSocket === null) return;
    // A model/runtime Apply can leave one already-delivered frame fenced to a
    // model the viewer cannot currently project. A credit is still required
    // to release the bounded stream; closing and immediately reconnecting
    // would turn an ordinary model refresh into a reconnect storm. Discard the
    // frame at the transport boundary, keep the socket, and let the next
    // render-compatible frame (or the same cached frame after the model
    // refreshes) drive the measured viewer.
    this.publishObservationTransportStatus(
      'synchronizing',
      'Updating the live robot view for the current model.',
    );
    this.cancelObservationFrame();
    this.appliedExecutionObservation = frame;
    this.sendObservationCredit(frame);
  };
  readonly subscribeExecutionObservationTransport = (listener: Listener) => {
    this.observationTransportListeners.add(listener);
    return () => this.observationTransportListeners.delete(listener);
  };
  readonly getPresentedRuntimeFrame = (): RuntimeFrameIdentity | null =>
    this.presentedFrame === null ? null : { ...this.presentedFrame };

  readonly sendExecutionCommand = (
    commandKind: ExecutionControlCommandKind,
    payload: Record<string, unknown>,
    correlationId: string,
  ): Promise<ExecutionCommandReceipt> => {
    const socket = this.socket;
    const sessionId = this.controlSessionId;
    const control = this.executionControl;
    const stop = [
      'program_motion_stop', 'program_full_stop', 'program_stop',
      'operation_stop', 'cell_full_stop',
    ].includes(commandKind);
    const runtimeScale = commandKind === 'runtime_scale';
    if (
      socket === null
      || socket.readyState !== WebSocket.OPEN
      || sessionId === null
      || control === null
      || !this.controlHeartbeatConfirmed
      || this.controlTransportError !== null
      || (!stop && !this.controlProjectionIsCurrent())
    ) {
      return Promise.reject(new Error('Control connection is not synchronized.'));
    }
    if (!stop && (
      runtimeScale
        ? this.runtimeScaleControlPending !== null
        : this.ordinaryControlPending !== null
    )) {
      return Promise.reject(new Error(
        runtimeScale
          ? 'Another runtime velocity update is pending.'
          : 'Another ordinary control command is pending.',
      ));
    }
    let fencedPayload: Record<string, unknown>;
    try {
      fencedPayload = executionCommandPayload(
        commandKind,
        payload,
        this.latest?.executionControlStatus?.ownerEpoch,
      );
    } catch (error: unknown) {
      return Promise.reject(error);
    }
    if (HTTP_ADMITTED_COMMANDS.has(commandKind)) {
      // Composer checks the shared Draft and submits under its authoring
      // serialization lock. Stop and all observation remain on the WS lane.
      this.ordinaryControlPending = correlationId;
      return (async (): Promise<ExecutionCommandReceipt> => {
        const abort = new AbortController();
        const deadline = setTimeout(() => abort.abort(), 5_000);
        let acknowledged = false;
        try {
          const response = await fetch(resolveStructuralAssetUrl('/api/control/commands'), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
            signal: abort.signal,
            body: JSON.stringify({
              command_envelope: {
                schema_version: EXECUTION_COMMAND_SCHEMA,
                command_kind: commandKind,
                correlation_id: correlationId,
                source_label: 'operator_ui',
                control_session_id: null,
                client_sequence: null,
                freshness: control.freshness,
                expected_semantic_owner_epoch: control.semantic_owner_epoch,
                expected_execution_control_revision: control.execution_control_revision,
              },
              payload: fencedPayload,
            }),
          });
          const body: unknown = await response.json();
          if (!response.ok) {
            // A definite client rejection has no submitted effect. A server
            // error can occur after submission and remains uncertain.
            acknowledged = response.status >= 400 && response.status < 500;
            const error = parseApiErrorDto(body);
            throw new Error(
              error?.message || error?.code
                || `Execution command rejected (HTTP ${response.status}).`,
            );
          }
          if (body === null || typeof body !== 'object' || Array.isArray(body)) {
            throw new Error('Execution command response is invalid.');
          }
          const record = body as Record<string, unknown>;
          const receipt = record.receipt;
          if (receipt === null || typeof receipt !== 'object' || Array.isArray(receipt)) {
            throw new Error('Execution command receipt is missing.');
          }
          const value = receipt as Record<string, unknown>;
          if (value.schema_version !== 'manyforge.execution_command_receipt.v1'
            || value.correlation_id !== correlationId
            || value.control_session_id !== null
            || value.client_sequence !== null
            || value.receiver_incarnation !== this.controlReceiverIncarnation
            || (value.disposition !== 'rejected' && (
              value.semantic_owner_epoch !== control.semantic_owner_epoch
              || value.execution_control_revision !== control.execution_control_revision
            ))
            || !canonicalPositiveU64(value.received_server_sequence)
            || !canonicalPositiveU64(value.received_monotonic_ns)
            || !['received', 'rejected', 'joined', 'already_current', 'no_effect']
              .includes(String(value.disposition))
            || typeof value.code !== 'string') {
            throw new Error('Execution command receipt identity is invalid.');
          }
          acknowledged = true;
          if (value.disposition === 'rejected') {
            const cause = record.result;
            const detail = cause !== null && typeof cause === 'object'
              && !Array.isArray(cause) ? cause as Record<string, unknown> : null;
            throw new Error(`${value.code}: ${typeof detail?.message === 'string'
              ? detail.message : value.code}`);
          }
          return {
            correlationId,
            disposition: value.disposition as ExecutionCommandReceipt['disposition'],
            code: value.code as string,
            result: record.result,
            receivedServerSequence: value.received_server_sequence,
            receivedMonotonicNs: value.received_monotonic_ns,
            emittedMonotonicNs: value.received_monotonic_ns,
          };
        } catch (error: unknown) {
          if (!acknowledged) {
            this.controlTransportError = 'Command delivery is unconfirmed; commands are disabled.';
            this.commandDeliveryFaultSessionId = sessionId;
            this.reportClientIncident(
              'command_delivery_unconfirmed', true, 'http_response_unconfirmed', 5_000_000_000,
            );
            this.refreshControlCondition();
            socket.close(4002, 'command delivery unconfirmed');
            throw new Error(this.controlTransportError, { cause: error });
          }
          throw error;
        } finally {
          clearTimeout(deadline);
          if (this.ordinaryControlPending === correlationId) {
            this.ordinaryControlPending = null;
          }
        }
      })();
    }
    this.controlClientSequence += 1;
    const clientSequence = String(this.controlClientSequence);
    const command = {
      schema_version: CONTROL_PROTOCOL_VERSION,
      message_kind: 'command',
      control_session_id: sessionId,
      client_sequence: clientSequence,
      command: {
        command_envelope: {
          schema_version: EXECUTION_COMMAND_SCHEMA,
          command_kind: commandKind,
          correlation_id: correlationId,
          source_label: 'operator_ui',
          control_session_id: sessionId,
          client_sequence: clientSequence,
          freshness: control.freshness,
          expected_semantic_owner_epoch: control.semantic_owner_epoch,
          expected_execution_control_revision: control.execution_control_revision,
        },
        payload: fencedPayload,
      },
    };
    return new Promise<ExecutionCommandReceipt>((resolve, reject) => {
      const pending = {
        resolve,
        reject,
        clientSequence: BigInt(clientSequence),
        deliveryTimer: null as ReturnType<typeof setTimeout> | null,
      };
      this.commandReceipts.set(correlationId, pending);
      if (runtimeScale) this.runtimeScaleControlPending = correlationId;
      else if (!stop) this.ordinaryControlPending = correlationId;
      try {
        socket.send(JSON.stringify(command));
        pending.deliveryTimer = setTimeout(() => {
          if (this.commandReceipts.get(correlationId) !== pending) return;
          const delivered = this.lastReceivedClientSequence >= pending.clientSequence;
          if (delivered) {
            // The server watermark is the transport acknowledgement. Owner
            // work may legitimately continue (for example, Reset settlement)
            // while lifecycle state is already arriving independently.
            pending.deliveryTimer = null;
            return;
          }
          this.commandReceipts.delete(correlationId);
          if (this.ordinaryControlPending === correlationId) {
            this.ordinaryControlPending = null;
          }
          if (this.runtimeScaleControlPending === correlationId) {
            this.runtimeScaleControlPending = null;
          }
          const error = new Error(
            'Command delivery is unconfirmed; commands are disabled.',
          );
          pending.reject(error);
          this.controlTransportError = error.message;
          this.commandDeliveryFaultSessionId = sessionId;
          this.reportClientIncident(
            'command_delivery_unconfirmed',
            true,
            'command_watermark_missing',
            CONTROL_COMMAND_DELIVERY_NS,
          );
          this.refreshControlCondition();
          // Recover only through a new current snapshot. The backend may have
          // received the command even though this session could not prove it.
          socket.close(4002, 'command delivery unconfirmed');
        }, CONTROL_COMMAND_DELIVERY_NS / 1_000_000);
      } catch (reason) {
        this.commandReceipts.delete(correlationId);
        if (this.ordinaryControlPending === correlationId) {
          this.ordinaryControlPending = null;
        }
        if (this.runtimeScaleControlPending === correlationId) {
          this.runtimeScaleControlPending = null;
        }
        reject(reason instanceof Error ? reason : new Error('Command delivery failed.'));
      }
    });
  };

  readonly reportClientIncident = (
    kind: ControlClientIncidentKind,
    active: boolean,
    reasonCode: string,
    observedDelayNs: number | null = null,
    /** performance.now() when the condition began, if before this report. */
    startedMs?: number,
  ): void => {
    const existing = this.clientIncidents.get(kind);
    if (active) {
      if (existing !== undefined) {
        existing.occurrenceCount += 1;
        existing.maximumDelayNs = observedDelayNs === null
          ? existing.maximumDelayNs
          : Math.max(existing.maximumDelayNs ?? 0, observedDelayNs);
        return;
      }
      const incident: ClientIncidentState = {
        id: `client-incident.${globalThis.crypto?.randomUUID?.()
          ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`}`,
        kind,
        reasonCode,
        startedMs: startedMs !== undefined && startedMs <= performance.now()
          ? startedMs
          : performance.now(),
        occurrenceCount: 1,
        maximumDelayNs: observedDelayNs,
        activationSent: false,
      };
      this.clientIncidents.set(kind, incident);
      this.sendClientIncidentTransition(incident, 'activated');
      return;
    }
    if (existing === undefined) return;
    this.sendClientIncidentTransition(existing, 'resolved');
    this.clientIncidents.delete(kind);
  };

  readonly markViewerPresented = (snapshot: ComposerHeadlessState | null): void => {
    if (snapshot === null || !snapshot.runtime.complete) {
      this.presentedViewer = null;
      this.presentedFrame = null;
      return;
    }
    this.presentedViewer = snapshot;
    this.presentedFrame = {
      ownerEpoch: snapshot.runtime.projectionOwnerEpoch,
      generation: snapshot.runtime.projectionGeneration,
    };
    this.advancePresentedFrame(this.latest);
  };

  setViewerDemand(active: boolean): void {
    if (this.disposed) return;
    const generation = ++this.viewerDemandGeneration;
    if (active) {
      if (this.viewerDemand) return;
      this.viewerDemand = true;
      if (this.enabled) {
        this.publishObservationTransportStatus(
          'connecting',
          'Connecting the live measured-state viewer.',
        );
        this.connectObservation();
      }
      return;
    }
    // Deferring release by one task collapses React Strict Mode's synthetic
    // setup/cleanup/setup probe into one logical socket and source demand.
    queueMicrotask(() => {
      if (generation !== this.viewerDemandGeneration || !this.viewerDemand) return;
      this.viewerDemand = false;
      this.stopObservationConnection('Composer viewer is not visible');
      this.publishObservationTransportStatus('disabled', null);
    });
  }

  setEnabled(enabled: boolean): void {
    if (this.disposed) return;
    const generation = ++this.enabledGeneration;
    if (!enabled) {
      // React Strict Mode immediately follows its synthetic cleanup with the
      // matching setup. Delay release by one microtask so that probe remains
      // one logical source connection; a real disable still closes promptly.
      queueMicrotask(() => {
        if (
          generation !== this.enabledGeneration
          || this.disposed
          || !this.enabled
        ) return;
        this.applyEnabled(false);
      });
      return;
    }
    if (this.enabled) return;
    this.applyEnabled(true);
  }

  private applyEnabled(enabled: boolean): void {
    this.enabled = enabled;
    this.qualificationRecord.enabled = enabled;
    this.stopConnection('Composer headless source disabled');
    this.stopObservationConnection('Composer viewer source disabled');
    this.cancelStatePublications();
    this.latest = null;
    this.clearExecutionObservation();
    this.presentedViewer = null;
    this.presentedFrame = null;
    this.latestError = null;
    if (enabled) {
      this.connect();
      if (this.viewerDemand) {
        this.publishObservationTransportStatus(
          'connecting',
          'Connecting the live measured-state viewer.',
        );
        this.connectObservation();
      } else {
        this.publishObservationTransportStatus('disabled', null);
      }
    } else {
      this.publishObservationTransportStatus('disabled', null);
      this.publishImmediate();
    }
    publishHeadlessQualification();
  }

  readonly reconnect = (): void => {
    if (!this.enabled || this.disposed) return;
    this.stopConnection('Composer client reconnect');
    this.stopObservationConnection('Composer client reconnect');
    this.connect();
    if (this.viewerDemand) {
      this.publishObservationTransportStatus(
        'reconnecting',
        'Reconnecting the live measured-state viewer.',
      );
      this.connectObservation();
    } else {
      this.publishObservationTransportStatus('disabled', null);
    }
  };

  dispose(): void {
    this.disposed = true;
    this.enabled = false;
    this.enabledGeneration += 1;
    this.viewerDemand = false;
    this.viewerDemandGeneration += 1;
    this.qualificationRecord.enabled = false;
    this.stopConnection('Composer client unmounted');
    this.stopObservationConnection('Composer client unmounted');
    this.cancelStatePublications();
    this.shellListeners.clear();
    this.operationProgressListeners.clear();
    this.viewerListeners.clear();
    this.executionObservationListeners.clear();
    this.publishObservationTransportStatus('disabled', null);
    this.observationTransportListeners.clear();
    headlessSourceRecords.delete(this.qualificationRecord);
    publishHeadlessQualification();
  }

  private stopConnection(reason: string): void {
    this.serial += 1;
    if (this.reconnectTimer !== null) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.clearControlHeartbeatWatch();
    const socket = this.socket;
    this.socket = null;
    this.clearControlSession(reason);
    this.controlTransportStatus = this.enabled && !this.disposed
      ? 'connecting'
      : 'disconnected';
    socket?.close(1000, reason);
    this.latest = null;
    this.presentedViewer = null;
    this.presentedFrame = null;
    this.refreshControlCondition();
  }

  private clearControlSession(reason: string): void {
    this.operationProgress = EMPTY_OPERATION_PROGRESS;
    for (const listener of this.operationProgressListeners) listener();
    this.controlSessionId = null;
    this.controlReceiverIncarnation = null;
    this.controlSnapshotBarrierPending = true;
    this.controlServerSequence = 0n;
    this.executionControl = null;
    this.executionTargetsWire = null;
    this.controlClientSequence = 0;
    this.controlHeartbeatConfirmed = false;
    this.lastControlEchoSequence = 0;
    this.lastReceivedClientSequence = 0n;
    this.controlHeartbeatFreshness = null;
    this.controlTransportError = null;
    this.controlSemanticStatus = 'synchronizing';
    this.controlSemanticReason = 'control_projection_synchronizing';
    this.ordinaryControlPending = null;
    this.runtimeScaleControlPending = null;
    for (const pending of this.commandReceipts.values()) {
      if (pending.deliveryTimer !== null) clearTimeout(pending.deliveryTimer);
      pending.reject(new Error(`Command delivery unconfirmed; ${reason}.`));
    }
    this.commandReceipts.clear();
  }

  private stopObservationConnection(reason: string): void {
    this.observationSerial += 1;
    if (this.observationReconnectTimer !== null) {
      clearTimeout(this.observationReconnectTimer);
    }
    this.observationReconnectTimer = null;
    if (this.observationRenewalTimer !== null) {
      clearTimeout(this.observationRenewalTimer);
      this.observationRenewalTimer = null;
    }
    const socket = this.observationSocket;
    this.observationSocket = null;
    if (socket !== null) {
      this.qualificationRecord.socketActive = false;
      publishHeadlessQualification();
    }
    this.discardUnappliedExecutionObservation();
    socket?.close(1000, reason);
  }

  private connect(): void {
    if (!this.enabled || this.disposed) return;
    this.controlTransportStatus = 'connecting';
    this.controlTransportError = null;
    this.refreshControlCondition();
    const serial = ++this.serial;
    const socket = new WebSocket(websocketUrl('/ws/control'));
    this.socket = socket;
    socket.onopen = () => {
      if (
        serial !== this.serial
        || this.socket !== socket
        || !this.enabled
        || this.disposed
      ) return;
      const clientIncarnation = browserStableIdentity(
        'manyforge.composer.control_client_incarnation',
        'control-client',
      );
      socket.send(JSON.stringify({
        schema_version: CONTROL_PROTOCOL_VERSION,
        message_kind: 'client_hello',
        supported_versions: [CONTROL_PROTOCOL_VERSION],
        client_incarnation: clientIncarnation,
        source_label: 'operator_ui',
      }));
    };
    socket.onmessage = (message) => {
      if (serial !== this.serial) return;
      try {
        const raw: unknown = JSON.parse(String(message.data));
        if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) {
          throw new Error('Control message must be an object.');
        }
        const record = raw as Record<string, unknown>;
        if (record.schema_version !== CONTROL_PROTOCOL_VERSION) {
          throw new Error('Control protocol version is unsupported.');
        }
        const kind = record.message_kind;
        if (kind === 'server_hello') {
          if (this.controlSessionId !== null
            || typeof record.control_session_id !== 'string'
            || typeof record.receiver_incarnation !== 'string'
            || record.heartbeat_period_ns !== CONTROL_HEARTBEAT_PERIOD_NS
            || record.heartbeat_delayed_ns !== CONTROL_HEARTBEAT_DELAYED_NS
            || record.heartbeat_stale_ns !== CONTROL_HEARTBEAT_STALE_NS
            || record.maximum_message_bytes !== CONTROL_MAXIMUM_MESSAGE_BYTES
            || record.maximum_messages_per_second
              !== CONTROL_MAXIMUM_MESSAGES_PER_SECOND) {
            throw new Error('Control server hello is invalid.');
          }
          this.controlSessionId = record.control_session_id;
          this.controlReceiverIncarnation = record.receiver_incarnation;
          this.controlSnapshotBarrierPending = true;
          this.controlServerSequence = 0n;
          this.controlClientSequence = 0;
          this.controlHeartbeatConfirmed = false;
          this.lastControlEchoSequence = 0;
          this.lastReceivedClientSequence = 0n;
          this.controlHeartbeatFreshness = null;
          this.controlTransportError = null;
          this.controlSemanticStatus = 'synchronizing';
          this.controlSemanticReason = 'control_projection_synchronizing';
          for (const incident of this.clientIncidents.values()) {
            incident.activationSent = false;
            this.sendClientIncidentTransition(incident, 'activated');
          }
          this.refreshControlCondition();
          this.scheduleControlHeartbeatWatch(serial, socket);
          return;
        }
        if (this.controlSessionId === null
          || record.control_session_id !== this.controlSessionId
          || !canonicalPositiveU64(record.server_sequence)
          || BigInt(record.server_sequence) <= this.controlServerSequence) {
          throw new Error('Control server sequence or session is invalid.');
        }
        this.controlServerSequence = BigInt(record.server_sequence);
        if (kind === 'heartbeat_challenge') {
          const semanticStatus = record.semantic_status;
          const semanticReason = record.semantic_reason;
          if (!['current', 'synchronizing', 'unavailable'].includes(
            String(semanticStatus),
          )) throw new Error('Control heartbeat semantic status is invalid.');
          const current = semanticStatus === 'current';
          if ((current && semanticReason !== null)
            || (!current && (typeof semanticReason !== 'string'
              || semanticReason.length === 0))) {
            throw new Error('Control heartbeat semantic reason is invalid.');
          }
          if (
            !canonicalPositiveU64(record.challenge_sequence)
          ) throw new Error('Control heartbeat is invalid.');
          let freshness: ControlFreshnessContext | null = null;
          if (current) {
            if (record.freshness === null
              || typeof record.freshness !== 'object'
              || Array.isArray(record.freshness)) {
              throw new Error('Current heartbeat omitted freshness.');
            }
            freshness = record.freshness as unknown as ControlFreshnessContext;
            if (
              typeof freshness.context_id !== 'string'
              || typeof freshness.receiver_incarnation !== 'string'
              || typeof freshness.semantic_owner_epoch !== 'string'
              || !Number.isSafeInteger(freshness.execution_control_revision)
              || !canonicalPositiveU64(freshness.issued_server_sequence)
              || freshness.valid_for_ns !== 750_000_000
            ) throw new Error('Control freshness context is invalid.');
            if (this.controlMatchesFreshness(this.executionControl, freshness)) {
              this.executionControl = { ...this.executionControl, freshness };
            }
          } else if (record.freshness !== null) {
            throw new Error('Non-current heartbeat carried command freshness.');
          }
          const semanticChanged = this.controlSemanticStatus !== semanticStatus
            || this.controlSemanticReason !== semanticReason;
          this.controlSemanticStatus = semanticStatus as ControlSemanticStatus;
          this.controlSemanticReason = semanticReason as string | null;
          this.controlHeartbeatFreshness = freshness;
          const received = record.received_client_sequence;
          this.admitControlWatermark(received);
          const confirmedNow = this.lastControlEchoSequence > 0
            && canonicalPositiveU64(received)
            && BigInt(received) >= BigInt(this.lastControlEchoSequence);
          // Echo before publishing React state or durable diagnostic
          // transitions. A semantic revision can legitimately reach this
          // cached heartbeat context before its replaceable shell delta; that
          // brief mismatch is synchronization, never a broken transport.
          this.controlClientSequence += 1;
          this.lastControlEchoSequence = this.controlClientSequence;
          socket.send(JSON.stringify({
            schema_version: CONTROL_PROTOCOL_VERSION,
            message_kind: 'heartbeat_echo',
            control_session_id: this.controlSessionId,
            client_sequence: String(this.controlClientSequence),
            challenge_sequence: record.challenge_sequence,
          }));
          if (confirmedNow) {
            this.scheduleControlHeartbeatWatch(serial, socket);
            if (!this.controlHeartbeatConfirmed) {
              this.controlHeartbeatConfirmed = true;
              this.controlTransportStatus = 'connected';
              this.controlTransportError = null;
              this.reportClientIncident(
                'control_disconnected',
                false,
                'control_websocket_recovered',
              );
              this.resolveCommandDeliveryFaultAfterResynchronization();
              this.refreshControlCondition();
              if (this.latest !== null) {
                this.scheduleShell();
                this.scheduleViewer();
              }
            }
          }
          if (semanticChanged || confirmedNow) this.refreshControlCondition();
          return;
        }
        if (kind === 'command_receipt') {
          const receiptValue = record.receipt;
          if (receiptValue === null || typeof receiptValue !== 'object'
            || Array.isArray(receiptValue)) {
            throw new Error('Control command receipt is invalid.');
          }
          const receipt = receiptValue as Record<string, unknown>;
          const correlationId = receipt.correlation_id;
          const disposition = receipt.disposition;
          const code = receipt.code;
          if (typeof correlationId !== 'string' || typeof code !== 'string'
            || receipt.schema_version !== 'manyforge.execution_command_receipt.v1'
            || receipt.control_session_id !== this.controlSessionId
            || !canonicalPositiveU64(record.received_client_sequence)
            || receipt.receiver_incarnation !== this.controlReceiverIncarnation
            || !canonicalPositiveU64(receipt.client_sequence)
            || BigInt(record.received_client_sequence as string)
              < BigInt(receipt.client_sequence as string)
            || !canonicalPositiveU64(receipt.received_server_sequence)
            || !canonicalPositiveU64(receipt.received_monotonic_ns)
            || typeof receipt.semantic_owner_epoch !== 'string'
            || !Number.isSafeInteger(receipt.execution_control_revision)
            || (receipt.execution_control_revision as number) < 1
            || !canonicalPositiveU64(record.emitted_monotonic_ns)
            || BigInt(record.emitted_monotonic_ns)
              < BigInt(receipt.received_monotonic_ns)
            || !['received', 'rejected', 'joined', 'already_current', 'no_effect']
              .includes(String(disposition))) {
            throw new Error('Control command receipt is invalid.');
          }
          const pending = this.commandReceipts.get(correlationId);
          if (pending !== undefined
              && pending.clientSequence !== BigInt(receipt.client_sequence as string)) {
            throw new Error('Control receipt watermark or command identity is invalid.');
          }
          this.admitControlWatermark(record.received_client_sequence);
          if (pending !== undefined) {
            this.commandReceipts.delete(correlationId);
            if (pending.deliveryTimer !== null) clearTimeout(pending.deliveryTimer);
            if (this.ordinaryControlPending === correlationId) {
              this.ordinaryControlPending = null;
            }
            if (this.runtimeScaleControlPending === correlationId) {
              this.runtimeScaleControlPending = null;
            }
            const parsed = {
              correlationId,
              disposition: disposition as ExecutionCommandReceipt['disposition'],
              code,
              result: record.result,
              receivedServerSequence: receipt.received_server_sequence,
              receivedMonotonicNs: receipt.received_monotonic_ns,
              emittedMonotonicNs: record.emitted_monotonic_ns,
            };
            if (parsed.disposition === 'rejected') {
              const cause = record.result;
              const detail = cause !== null && typeof cause === 'object'
                && !Array.isArray(cause)
                && (cause as Record<string, unknown>).code === code
                && (cause as Record<string, unknown>).correlation_id === correlationId
                && typeof (cause as Record<string, unknown>).message === 'string'
                ? (cause as Record<string, string>).message
                : code;
              pending.reject(new Error(`${code}: ${detail}`));
            } else {
              pending.resolve(parsed);
            }
          }
          return;
        }
        if (kind === 'resync_required') {
          throw new Error('Control session requires a current resynchronization.');
        }
        if (kind !== 'snapshot' && kind !== 'delta') {
          throw new Error('Control server message kind is unsupported.');
        }
        if (this.controlSnapshotBarrierPending && kind !== 'snapshot') {
          throw new Error('Control delta preceded its snapshot barrier.');
        }
        const ownerState = record.control !== null;
        if ((ownerState && !canonicalPositiveU64(record.state_sequence))
          || (!ownerState && (kind !== 'delta' || record.event_class !== 'cold'
            || record.state_sequence !== undefined
            || record.received_client_sequence !== undefined))) {
          throw new Error('Control state delivery sequence is invalid.');
        }
        if (ownerState) this.admitControlWatermark(record.received_client_sequence);
        this.applyControlState(record, kind, socket);
      } catch (reason) {
        this.publishError(
          reason instanceof Error
            ? reason.message
            : 'Invalid headless WebSocket projection.',
        );
        socket.close(4002, 'invalid headless projection');
      }
    };
    socket.onerror = () => {
      if (serial !== this.serial || this.socket !== socket) return;
      this.controlTransportStatus = 'disconnected';
      this.publishError('Headless WebSocket transport failed.');
    };
    socket.onclose = () => {
      if (serial !== this.serial || !this.enabled || this.disposed) return;
      this.reportClientIncident(
        'control_disconnected',
        true,
        'control_websocket_closed',
      );
      if (this.socket === socket) {
        this.socket = null;
      }
      this.clearControlHeartbeatWatch();
      this.clearControlSession('control connection closed');
      this.controlTransportStatus = 'disconnected';
      // An actual close establishes a new snapshot barrier even if this
      // session had already been classified as delayed or stale.
      this.publishError('Headless projection disconnected.');
      this.reconnectTimer = setTimeout(() => {
        if (serial === this.serial) this.connect();
      }, RECONNECT_DELAY_MS);
    };
  }

  private admitControlWatermark(value: unknown): void {
    if (value !== null && !canonicalPositiveU64(value)) {
      throw new Error('Control client watermark is invalid.');
    }
    const watermark = value === null ? 0n : BigInt(value as string);
    if (watermark < this.lastReceivedClientSequence) {
      throw new Error('Control client watermark regressed.');
    }
    this.lastReceivedClientSequence = watermark;
  }

  private applyControlState(
    record: Record<string, unknown>,
    kind: 'snapshot' | 'delta',
    socket: WebSocket,
  ): void {
    try {
      const controlValue = record.control;
      if (controlValue !== null && (typeof controlValue !== 'object'
        || Array.isArray(controlValue))) {
        throw new Error('Control projection is unavailable.');
      }
      if (controlValue !== null) {
        const control = controlValue as unknown as CurrentExecutionControl;
        if (typeof control.receiver_incarnation !== 'string'
          || typeof control.semantic_owner_epoch !== 'string'
          || !Number.isSafeInteger(control.execution_control_revision)
          || control.execution_control_revision < 1
          || control.freshness === null || typeof control.freshness !== 'object') {
          throw new Error('Control projection identity is invalid.');
        }
        const heartbeatFreshness = this.controlHeartbeatFreshness;
        this.executionControl = heartbeatFreshness !== null
          && this.controlMatchesFreshness(control, heartbeatFreshness)
          ? { ...control, freshness: heartbeatFreshness }
          : control;
      } else if (this.executionControl === null) {
        throw new Error('Control delta preceded its control snapshot.');
      }
      const shell = record.shell;
      if (shell === null || typeof shell !== 'object' || Array.isArray(shell)) {
        throw new Error('Control state message omitted its shell.');
      }
      const shellRecord = { ...shell } as Record<string, unknown>;
      const progressPresent = 'operation_progress' in shellRecord;
      const progress = progressPresent
        ? parseOperationProgress(shellRecord.operation_progress) : null;
      delete shellRecord.operation_progress;
      const progressOnly = kind === 'delta' && progressPresent
        && Object.keys(shellRecord).every((name) => name === 'complete');
      let candidate: ComposerHeadlessState;
      let candidateExecutionTargetsWire = this.executionTargetsWire;
      if (kind === 'snapshot') {
        candidate = parseComposerHeadlessState(shellRecord);
        candidateExecutionTargetsWire = shellRecord.execution_targets;
      } else {
        if (this.latest === null) {
          throw new Error('Control delta preceded its snapshot barrier.');
        }
        if (shellRecord.complete !== false) {
          throw new Error('Partial control delta claimed to be complete.');
        }
        const localIdentityFields = [
          'schema_version', 'authoring_workspace', 'receiver_epoch',
          'snapshot_sequence',
        ];
        const localIdentityCount = localIdentityFields.filter(
          (name) => name in shellRecord,
        ).length;
        if (localIdentityCount !== 0
          && localIdentityCount !== localIdentityFields.length) {
          throw new Error('Control delta contains a partial Composer identity.');
        }
        const hasLocalIdentity = localIdentityCount === localIdentityFields.length;
        let authoringWorkspace = this.latest.authoringWorkspace;
        let receiverEpoch = this.latest.receiverEpoch;
        let snapshotSequence = this.latest.snapshotSequence;
        if (hasLocalIdentity) {
          const workspace = parseAuthoringWorkspaceIdentity(
            shellRecord.authoring_workspace,
            'controlDelta.authoring_workspace',
          );
          if (typeof shellRecord.receiver_epoch !== 'string'
            || !Number.isSafeInteger(shellRecord.snapshot_sequence)
            || shellRecord.schema_version
              !== 'manyforge.composer_headless_state.v2') {
            throw new Error('Control delta shell identity is invalid.');
          }
          authoringWorkspace = workspace;
          receiverEpoch = shellRecord.receiver_epoch;
          snapshotSequence = shellRecord.snapshot_sequence as number;
        }
        const permitted = new Set([
          'schema_version', 'complete', 'runtime', 'alarms',
          'component_overview', 'authoring_admission', 'execution_targets',
          'execution_control_state', 'program_cycle', 'authoring_workspace',
          'receiver_epoch', 'snapshot_sequence',
        ]);
        if (Object.keys(shellRecord).some((name) => !permitted.has(name))) {
          throw new Error('Control delta shell shape is invalid.');
        }
        const targetsPresent = 'execution_targets' in shellRecord;
        const executionStatePresent = 'execution_control_state' in shellRecord;
        if (targetsPresent && !executionStatePresent) {
          throw new Error('Control delta replaced targets without their control capture.');
        }
        let authoringAdmission = this.latest.authoringAdmission;
        if ('authoring_admission' in shellRecord) {
          const value = shellRecord.authoring_admission;
          if (value === null || typeof value !== 'object' || Array.isArray(value)) {
            throw new Error('Control delta authoring admission is invalid.');
          }
          const admission = value as Record<string, unknown>;
          if (Object.keys(admission).length !== 2
            || !('authoring_allowed' in admission)
            || !('admission_generation' in admission)
            || typeof admission.authoring_allowed !== 'boolean'
            || !Number.isSafeInteger(admission.admission_generation)
            || (admission.admission_generation as number) < 1) {
            throw new Error('Control delta authoring admission is invalid.');
          }
          authoringAdmission = {
            authoringAllowed: admission.authoring_allowed,
            admissionGeneration: admission.admission_generation as number,
          };
        }
        let executionControlStatus = this.latest.executionControlStatus;
        if (executionStatePresent) {
          const executionTargets = targetsPresent
            ? shellRecord.execution_targets
            : this.executionTargetsWire;
          executionControlStatus = executionTargets === null
            && shellRecord.execution_control_state === null
            ? null
            : parseExecutionControlStatus(
                executionTargets,
                shellRecord.execution_control_state,
              );
          if (targetsPresent) candidateExecutionTargetsWire = executionTargets;
        }
        candidate = {
          ...this.latest,
          ...('runtime' in shellRecord
            ? { runtime: parseHeadlessRuntimeSnapshot(shellRecord.runtime) }
            : {}),
          ...('alarms' in shellRecord
            ? { alarms: parseOperatorAlarmSnapshot(shellRecord.alarms) }
            : {}),
          ...('component_overview' in shellRecord
            ? {
                componentOverview: parseComponentOverviewProjection(
                  shellRecord.component_overview,
                  'headlessState.component_overview',
                ),
              }
            : {}),
          ...('program_cycle' in shellRecord
            ? { programCycle: parseProgramCycleStatus(shellRecord.program_cycle) }
            : {}),
          authoringAdmission,
          executionControlStatus,
          authoringWorkspace,
          receiverEpoch,
          snapshotSequence,
        };
      }
      const admission = admitHeadlessState(
        this.controlSnapshotBarrierPending ? null : this.latest,
        candidate,
      );
      if (!admission.accepted) {
        this.publishError(admission.reason);
        socket.close(4002, 'non-monotonic headless projection');
        return;
      }
      this.controlSnapshotBarrierPending = false;
      this.executionTargetsWire = candidateExecutionTargetsWire;
      const viewerChanged = !progressOnly && !sameViewerProjection(this.latest, candidate);
      this.latest = candidate;
      if (progress !== null || kind === 'snapshot') {
        this.operationProgress = progress ?? EMPTY_OPERATION_PROGRESS;
        for (const listener of this.operationProgressListeners) listener();
      }
      if (canonicalPositiveU64(record.state_sequence)) {
        socket.send(JSON.stringify({
          schema_version: CONTROL_PROTOCOL_VERSION,
          message_kind: 'state_applied',
          control_session_id: this.controlSessionId,
          state_sequence: record.state_sequence,
        }));
      }
      this.refreshControlCondition();
      this.advancePresentedFrame(candidate);
      this.resolveCommandDeliveryFaultAfterResynchronization();
      if (this.controlHeartbeatConfirmed) {
        if (viewerChanged) this.scheduleViewer();
        // Progress wakes only its indicator; semantic owner changes retain
        // the normal shell path without another browser-side reduction.
        if (!progressOnly) this.scheduleShell(record.event_class === 'critical');
      }
    } catch (reason) {
      this.publishError(
        reason instanceof Error
          ? reason.message
          : 'Invalid headless WebSocket projection.',
      );
      socket.close(4002, 'invalid headless projection');
    }
  }

  private clearControlHeartbeatWatch(): void {
    if (this.controlHeartbeatTimer !== null) {
      clearTimeout(this.controlHeartbeatTimer);
      this.controlHeartbeatTimer = null;
    }
  }

  private resolveCommandDeliveryFaultAfterResynchronization(): void {
    if (
      this.commandDeliveryFaultSessionId === null
      || this.controlSessionId === null
      || this.controlSessionId === this.commandDeliveryFaultSessionId
      || !this.controlHeartbeatConfirmed
      || this.controlSnapshotBarrierPending
      || this.latest === null
    ) return;
    this.commandDeliveryFaultSessionId = null;
    this.reportClientIncident(
      'command_delivery_unconfirmed',
      false,
      'control_snapshot_resynchronized',
    );
  }

  private sendClientIncidentTransition(
    incident: ClientIncidentState,
    phase: 'activated' | 'resolved',
  ): void {
    const socket = this.socket;
    const sessionId = this.controlSessionId;
    if (socket === null || socket.readyState !== WebSocket.OPEN || sessionId === null) {
      return;
    }
    if (phase === 'resolved' && !incident.activationSent) return;
    this.controlClientSequence += 1;
    const elapsedNs = Math.max(
      0,
      Math.round((performance.now() - incident.startedMs) * 1_000_000),
    );
    try {
      socket.send(JSON.stringify({
        schema_version: CONTROL_PROTOCOL_VERSION,
        message_kind: 'client_incident',
        control_session_id: sessionId,
        client_sequence: String(this.controlClientSequence),
        incident_id: incident.id,
        incident_kind: incident.kind,
        phase,
        occurrence_count: incident.occurrenceCount,
        maximum_delay_ns: incident.maximumDelayNs ?? elapsedNs,
        reason_code: phase === 'activated' ? incident.reasonCode : 'client_incident_recovered',
      }));
      if (phase === 'activated') incident.activationSent = true;
    } catch {
      // The connection classifier owns transport failure. Keep an unsent
      // activation in memory so a later session can report it once.
      if (phase === 'activated') incident.activationSent = false;
    }
  }

  private scheduleControlHeartbeatWatch(serial: number, socket: WebSocket): void {
    this.clearControlHeartbeatWatch();
    this.controlHeartbeatTimer = setTimeout(() => {
      if (serial !== this.serial || this.socket !== socket) return;
      this.controlHeartbeatConfirmed = false;
      // Require an echo sent after this classification and its subsequent
      // server confirmation. An old challenge queued in a paused browser
      // must not briefly re-enable commands.
      this.lastControlEchoSequence = this.controlClientSequence + 1;
      this.controlTransportStatus = 'delayed';
      this.controlTransportError = 'Control connection is delayed; commands are disabled.';
      this.refreshControlCondition();
      this.controlHeartbeatTimer = setTimeout(() => {
        if (serial !== this.serial || this.socket !== socket) return;
        this.controlHeartbeatTimer = null;
        this.controlHeartbeatConfirmed = false;
        this.controlTransportStatus = 'stale';
        this.controlTransportError = 'Control connection is stale; commands are disabled.';
        this.refreshControlCondition();
        // Keep the live transport. The server keeps one challenge
        // outstanding and can recover the session without reconnect churn;
        // it separately reclaims a genuinely dead peer after a bounded wait.
      }, (CONTROL_HEARTBEAT_STALE_NS - CONTROL_HEARTBEAT_DELAYED_NS) / 1_000_000);
    }, CONTROL_HEARTBEAT_DELAYED_NS / 1_000_000);
  }

  private connectObservation(): void {
    if (!this.enabled || !this.viewerDemand || this.disposed) return;
    const serial = ++this.observationSerial;
    const socket = new WebSocket(websocketUrl('/ws/scene/viewer'));
    this.observationSocket = socket;
    headlessSocketCreations += 1;
    publishHeadlessQualification();
    socket.onopen = () => {
      if (
        serial !== this.observationSerial
        || this.observationSocket !== socket
        || !this.enabled
        || !this.viewerDemand
        || this.disposed
      ) return;
      this.qualificationRecord.socketActive = true;
      const activeSockets = headlessQualificationSnapshot().activeSockets;
      headlessMaxActiveSockets = Math.max(headlessMaxActiveSockets, activeSockets);
      publishHeadlessQualification();
      this.publishObservationTransportStatus(
        'waiting',
        'Waiting for the first current measured-state sample.',
      );
      // A fresh transport always asks for the current complete frame. It does
      // not request or replay motion missed while hidden or disconnected.
      this.sendObservationCredit(null);
    };
    socket.onmessage = (message) => {
      if (serial !== this.observationSerial) return;
      try {
        const candidate = parseExecutionObservationFrame(
          JSON.parse(String(message.data)) as unknown,
        );
        const current = this.latestExecutionObservation;
        if (
          current !== null
          && candidate.ownerEpoch === current.ownerEpoch
          && candidate.sourceEpoch === current.sourceEpoch
          && candidate.sourceSequence < current.sourceSequence
        ) {
          socket.close(4002, 'non-monotonic execution observation');
          return;
        }
        if (
          current !== null
          && candidate.sourceSequence === current.sourceSequence
          && candidate.ownerEpoch === current.ownerEpoch
          && candidate.sourceEpoch === current.sourceEpoch
        ) {
          if (!sameExecutionObservationFrame(current, candidate)) {
            socket.close(4002, 'changed execution observation at one sequence');
            return;
          }
          this.publishObservationTransportStatus('live', null);
          if (current === this.appliedExecutionObservation) this.sendObservationCredit(current);
          else this.scheduleExecutionObservation();
          return;
        }
        recordViewerObservationReceived(
          {
            ownerEpoch: candidate.ownerEpoch,
            sourceEpoch: candidate.sourceEpoch,
            sourceSequence: candidate.sourceSequence,
            receivedAtComposerMonotonicUs: candidate.receivedAtMonotonicUs,
          },
          current !== null && current !== this.appliedExecutionObservation,
        );
        this.latestExecutionObservation = candidate;
        this.publishObservationTransportStatus('live', null);
        this.scheduleExecutionObservation();
      } catch {
        this.publishObservationTransportStatus(
          'reconnecting',
          'The live measured-state viewer received an invalid sample and is reconnecting.',
        );
        socket.close(4002, 'invalid execution observation');
      }
    };
    socket.onerror = () => {
      if (
        serial !== this.observationSerial
        || this.observationSocket !== socket
      ) return;
      this.qualificationRecord.socketActive = false;
      publishHeadlessQualification();
      this.publishObservationTransportStatus(
        'reconnecting',
        'The live measured-state viewer transport failed and is reconnecting.',
      );
    };
    socket.onclose = () => {
      if (
        serial !== this.observationSerial
        || !this.enabled
        || !this.viewerDemand
        || this.disposed
      ) return;
      if (this.observationSocket === socket) {
        this.observationSocket = null;
        this.qualificationRecord.socketActive = false;
        publishHeadlessQualification();
      }
      // Clear the transport reference before notifying presentation readers.
      // A discarded prior-model frame must not recursively try to close the
      // socket that has already emitted this close event.
      this.discardUnappliedExecutionObservation();
      const synchronizing = this.observationTransportStatus.phase === 'synchronizing';
      if (!synchronizing && this.observationTransportStatus.phase !== 'reconnecting') {
        this.publishObservationTransportStatus(
          'reconnecting',
          'The live measured-state viewer disconnected and is reconnecting.',
        );
      }
      this.observationReconnectTimer = setTimeout(() => {
        if (
          serial === this.observationSerial
          && this.viewerDemand
        ) this.connectObservation();
      }, synchronizing ? 0 : RECONNECT_DELAY_MS);
    };
  }

  private sendObservationCredit(
    frame: ExecutionObservationFrame | null,
  ): void {
    const socket = this.observationSocket;
    if (
      socket === null
      || (
        typeof socket.readyState === 'number'
        && typeof WebSocket.OPEN === 'number'
        && socket.readyState !== WebSocket.OPEN
      )
      || typeof socket.send !== 'function'
      || !this.enabled
      || !this.viewerDemand
      || this.disposed
    ) return;
    socket.send(JSON.stringify({
      last_applied: frame === null ? null : {
        owner_epoch: frame.ownerEpoch,
        source_epoch: frame.sourceEpoch,
        source_sequence: frame.sourceSequence,
      },
      schema_version: EXECUTION_OBSERVATION_CREDIT_SCHEMA,
    }));
    if (this.observationRenewalTimer !== null) {
      clearTimeout(this.observationRenewalTimer);
    }
    this.observationRenewalTimer = setTimeout(() => {
      this.observationRenewalTimer = null;
      this.sendObservationCredit(this.appliedExecutionObservation);
    }, EXECUTION_OBSERVATION_RENEWAL_MS);
  }

  private result(): HeadlessStateResult {
    const semantic = this.semanticCondition();
    return {
      snapshot: this.latest,
      error: this.latestError,
      transportStatus: this.controlTransportStatus,
      transportError: this.controlTransportError,
      semanticStatus: semantic.status,
      semanticReason: semantic.reason,
      reconnect: this.reconnect,
    };
  }

  private advancePresentedFrame(candidate: ComposerHeadlessState | null): void {
    if (
      candidate === null
      || this.presentedViewer === null
      || !candidate.runtime.complete
      || !sameViewerProjection(this.presentedViewer, candidate)
    ) return;
    this.presentedFrame = {
      ownerEpoch: candidate.runtime.projectionOwnerEpoch,
      generation: candidate.runtime.projectionGeneration,
    };
  }

  private publishError(error: string | null): void {
    this.controlTransportError = error;
    if (error !== null) this.controlHeartbeatConfirmed = false;
    this.refreshControlCondition();
  }

  private controlMatchesFreshness(
    control: CurrentExecutionControl | null,
    freshness: ControlFreshnessContext | null,
  ): control is CurrentExecutionControl {
    return control !== null
      && freshness !== null
      && freshness.receiver_incarnation === control.receiver_incarnation
      && freshness.semantic_owner_epoch === control.semantic_owner_epoch
      && freshness.execution_control_revision
        === control.execution_control_revision;
  }

  private controlProjectionIsCurrent(): boolean {
    return this.controlSemanticStatus === 'current'
      && this.controlMatchesFreshness(
        this.executionControl,
        this.controlHeartbeatFreshness,
      );
  }

  private semanticCondition(): {
    status: ControlSemanticStatus;
    reason: string | null;
  } {
    if (this.controlSemanticStatus !== 'current') {
      return {
        status: this.controlSemanticStatus,
        reason: this.controlSemanticReason,
      };
    }
    return this.controlProjectionIsCurrent()
      ? { status: 'current', reason: null }
      : { status: 'synchronizing', reason: 'control_projection_synchronizing' };
  }

  private refreshControlCondition(): void {
    let error = this.enabled ? this.controlTransportError : null;
    const semantic = this.semanticCondition();
    if (this.enabled && error === null && semantic.status === 'synchronizing') {
      error = 'Control state is synchronizing; commands are disabled.';
    } else if (this.enabled && error === null && semantic.status === 'unavailable') {
      error = `Control state is unavailable (${semantic.reason
        ?? 'owner unavailable'}); commands are disabled.`;
    }
    if (
      error === this.latestError
      && this.shellResult.transportStatus === this.controlTransportStatus
      && this.shellResult.transportError === this.controlTransportError
      && this.shellResult.semanticStatus === semantic.status
      && this.shellResult.semanticReason === semantic.reason
    ) return;
    // Transport and semantic availability remove command authority, but they
    // do not erase the last coherent backend presentation or viewer scene.
    this.latestError = error;
    this.cancelStatePublications();
    this.publishImmediate();
  }

  private publishImmediate(
    viewerSnapshot: ComposerHeadlessState | null = this.latest,
  ): void {
    const next = this.result();
    this.shellResult = next;
    this.viewerResult = viewerSnapshot === this.latest
      ? next
      : { ...next, snapshot: viewerSnapshot };
    for (const listener of this.shellListeners) listener();
    for (const listener of this.viewerListeners) listener();
  }

  private flushShell(): void {
    this.shellTimer = null;
    this.shellResult = this.result();
    for (const listener of this.shellListeners) listener();
  }

  private scheduleShell(immediate = false): void {
    if (immediate) {
      if (this.shellTimer !== null) clearTimeout(this.shellTimer);
      this.flushShell();
      return;
    }
    if (this.shellTimer !== null) return;
    this.shellTimer = setTimeout(() => this.flushShell(), HEADLESS_SHELL_PUBLICATION_MS);
  }

  private scheduleViewer(): void {
    if (this.viewerFrame !== null) return;
    if (typeof window !== 'undefined' && typeof window.requestAnimationFrame === 'function') {
      this.viewerFrame = window.requestAnimationFrame(() => this.flushViewer());
    } else {
      this.viewerFrame = setTimeout(() => this.flushViewer(), 16);
    }
  }

  private scheduleExecutionObservation(): void {
    if (this.observationFrame !== null) return;
    if (typeof window !== 'undefined' && typeof window.requestAnimationFrame === 'function') {
      this.observationFrame = window.requestAnimationFrame(
        () => this.flushExecutionObservation(),
      );
    } else {
      this.observationFrame = setTimeout(() => this.flushExecutionObservation(), 16);
    }
  }

  private flushExecutionObservation(): void {
    this.observationFrame = null;
    for (const listener of this.executionObservationListeners) listener();
  }

  private clearExecutionObservation(): void {
    if (this.latestExecutionObservation === null) return;
    this.latestExecutionObservation = null;
    this.appliedExecutionObservation = null;
    this.cancelObservationFrame();
    for (const listener of this.executionObservationListeners) listener();
  }

  private discardUnappliedExecutionObservation(): void {
    this.cancelObservationFrame();
    if (this.latestExecutionObservation === this.appliedExecutionObservation) return;
    this.latestExecutionObservation = this.appliedExecutionObservation;
    for (const listener of this.executionObservationListeners) listener();
  }

  private publishObservationTransportStatus(
    phase: ExecutionObservationTransportPhase,
    message: string | null,
  ): void {
    if (
      this.observationTransportStatus.phase === phase
      && this.observationTransportStatus.message === message
    ) return;
    this.observationTransportStatus = Object.freeze({ phase, message });
    for (const listener of this.observationTransportListeners) listener();
  }

  private flushViewer(): void {
    this.viewerFrame = null;
    this.viewerResult = this.result();
    for (const listener of this.viewerListeners) listener();
  }

  private cancelViewerFrame(): void {
    if (this.viewerFrame === null) return;
    if (typeof this.viewerFrame === 'number'
      && typeof window !== 'undefined'
      && typeof window.cancelAnimationFrame === 'function') {
      window.cancelAnimationFrame(this.viewerFrame);
    } else {
      clearTimeout(this.viewerFrame as ReturnType<typeof setTimeout>);
    }
    this.viewerFrame = null;
  }

  private cancelObservationFrame(): void {
    if (this.observationFrame === null) return;
    if (typeof this.observationFrame === 'number'
      && typeof window !== 'undefined'
      && typeof window.cancelAnimationFrame === 'function') {
      window.cancelAnimationFrame(this.observationFrame);
    } else {
      clearTimeout(this.observationFrame as ReturnType<typeof setTimeout>);
    }
    this.observationFrame = null;
  }

  private cancelStatePublications(): void {
    if (this.shellTimer !== null) clearTimeout(this.shellTimer);
    this.shellTimer = null;
    this.cancelViewerFrame();
    // Cold/control changes must not strand an independently credited joint
    // frame. Observation teardown owns cancellation of that separate plane.
  }
}

export function useHeadlessStateSource(enabled: boolean): HeadlessStateSource {
  const [source] = useState(() => new HeadlessStateSource());
  useEffect(() => {
    source.setEnabled(enabled);
    return () => source.setEnabled(false);
  }, [enabled, source]);
  return source;
}

export function useShellHeadlessState(source: HeadlessStateSource): HeadlessStateResult {
  return useSyncExternalStore(
    source.subscribeShell,
    source.getShellSnapshot,
    source.getShellSnapshot,
  );
}

export function useViewerHeadlessState(
  source: HeadlessStateSource,
  active: boolean,
): HeadlessStateResult {
  const inactive = useRef<HeadlessStateResult>({
    snapshot: null,
    error: null,
    transportStatus: 'disconnected',
    transportError: null,
    semanticStatus: 'synchronizing',
    semanticReason: 'control_projection_synchronizing',
    reconnect: source.reconnect,
  });
  useEffect(() => {
    const updateDemand = () => {
      source.setViewerDemand(
        active
        && (typeof document === 'undefined' || document.visibilityState === 'visible'),
      );
    };
    updateDemand();
    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', updateDemand);
    }
    return () => {
      if (typeof document !== 'undefined') {
        document.removeEventListener('visibilitychange', updateDemand);
      }
      source.setViewerDemand(false);
    };
  }, [active, source]);
  return useSyncExternalStore(
    active ? source.subscribeViewer : () => () => undefined,
    active ? source.getViewerSnapshot : () => inactive.current,
    active ? source.getViewerSnapshot : () => inactive.current,
  );
}

export function useExecutionObservationTransport(
  source: HeadlessStateSource,
): ExecutionObservationTransportStatus {
  return useSyncExternalStore(
    source.subscribeExecutionObservationTransport,
    source.getExecutionObservationTransportStatus,
    source.getExecutionObservationTransportStatus,
  );
}
