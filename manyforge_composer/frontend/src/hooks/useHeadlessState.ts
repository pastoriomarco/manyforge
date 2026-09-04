import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import {
  parseComposerHeadlessState,
  parseExecutionControlStatus,
  parseProgramCycleStatus,
} from '../api/parsers';
import type { ComposerHeadlessState } from '../types/headless';
import type {
  ExecutionObservationFrame,
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

const EXECUTION_OBSERVATION_SCHEMA = 'manyforge.execution_observation_frame.v1';
const EXECUTION_OBSERVATION_CREDIT_SCHEMA = 'manyforge.execution_observation_credit.v1';
const EXECUTION_OBSERVATION_RENEWAL_MS = 15_000;

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
    && current.controllableComponentId === candidate.controllableComponentId
    && current.commandTargetId === candidate.commandTargetId
    && sameStrings(current.sourceJointNames, candidate.sourceJointNames)
    && sameStrings(current.compiledJointNames, candidate.compiledJointNames)
    && current.positionsRad.length === candidate.positionsRad.length
    && current.positionsRad.every((value, index) => value === candidate.positionsRad[index]);
}

export function parseExecutionObservationFrame(raw: unknown): ExecutionObservationFrame {
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) {
    throw new Error('Execution observation frame must be an object.');
  }
  const record = raw as Record<string, unknown>;
  const expected = [
    'cell_id',
    'command_target_id',
    'compiled_joint_names',
    'controllable_component_id',
    'graph_revision',
    'owner_epoch',
    'positions_rad',
    'received_at_monotonic_us',
    'retained_bundle_id',
    'schema_version',
    'source_epoch',
    'source_joint_names',
    'source_sequence',
  ].sort();
  if (Object.keys(record).sort().some((key, index) => key !== expected[index])
    || Object.keys(record).length !== expected.length) {
    throw new Error('Execution observation frame has an invalid shape.');
  }
  const text = (key: string): string => {
    const value = record[key];
    if (typeof value !== 'string' || value.length === 0) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return value;
  };
  const integer = (key: string): number => {
    const value = record[key];
    if (!Number.isSafeInteger(value) || (value as number) < 0) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return value as number;
  };
  const strings = (key: string): string[] => {
    const value = record[key];
    if (!Array.isArray(value) || value.length === 0 || value.length > 1024
      || value.some((item) => typeof item !== 'string' || item.length === 0)
      || new Set(value).size !== value.length) {
      throw new Error(`Execution observation ${key} is invalid.`);
    }
    return [...value] as string[];
  };
  const sourceJointNames = strings('source_joint_names');
  const compiledJointNames = strings('compiled_joint_names');
  const rawPositions = record.positions_rad;
  if (!Array.isArray(rawPositions)
    || rawPositions.some((value) => typeof value !== 'number' || !Number.isFinite(value))
    || rawPositions.length !== sourceJointNames.length
    || compiledJointNames.length !== sourceJointNames.length) {
    throw new Error('Execution observation joint vectors are invalid.');
  }
  if (record.schema_version !== EXECUTION_OBSERVATION_SCHEMA) {
    throw new Error('Execution observation schema is unsupported.');
  }
  return Object.freeze({
    schemaVersion: EXECUTION_OBSERVATION_SCHEMA,
    ownerEpoch: text('owner_epoch'),
    cellId: text('cell_id'),
    retainedBundleId: text('retained_bundle_id'),
    graphRevision: text('graph_revision'),
    controllableComponentId: text('controllable_component_id'),
    commandTargetId: text('command_target_id'),
    sourceEpoch: text('source_epoch'),
    sourceSequence: integer('source_sequence'),
    receivedAtMonotonicUs: integer('received_at_monotonic_us'),
    sourceJointNames,
    compiledJointNames,
    positionsRad: [...rawPositions] as number[],
  });
}

export interface HeadlessStateResult {
  snapshot: ComposerHeadlessState | null;
  error: string | null;
  reconnect: () => void;
}

export interface HeadlessStateAdmission {
  accepted: boolean;
  reason: string | null;
}

const CONTROL_PROTOCOL_VERSION = 'manyforge.execution_control.v1';
const EXECUTION_COMMAND_SCHEMA = 'manyforge.execution_command.v1';
const CONTROL_HEARTBEAT_PERIOD_NS = 250_000_000;
const CONTROL_HEARTBEAT_DELAYED_NS = 500_000_000;
const CONTROL_HEARTBEAT_STALE_NS = 1_000_000_000;
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
  | 'program_motion_stop'
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
  | 'operation_start'
  | 'operation_stop'
  | 'operation_pause'
  | 'operation_resume'
  | 'operation_reset'
  | 'operation_acknowledge_completed'
  | 'pick_sequence_start'
  | 'place_sequence_start'
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
  'pick_sequence_start',
  'place_sequence_start',
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

export function admitHeadlessState(
  current: ComposerHeadlessState | null,
  candidate: ComposerHeadlessState,
): HeadlessStateAdmission {
  if (current === null) return { accepted: true, reason: null };
  if (candidate.receiverEpoch === current.receiverEpoch) {
    if (candidate.snapshotSequence <= current.snapshotSequence) {
      return { accepted: false, reason: 'Headless snapshot sequence regressed.' };
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

export function sameHeadlessProjection(
  current: ComposerHeadlessState | null,
  candidate: ComposerHeadlessState,
): boolean {
  if (current === null) return false;
  return current.receiverEpoch === candidate.receiverEpoch
    && current.runtime.projectionOwnerEpoch === candidate.runtime.projectionOwnerEpoch
    && current.runtime.projectionOwnerId === candidate.runtime.projectionOwnerId
    && current.runtime.complete === candidate.runtime.complete
    && sameStrings(current.runtime.reasons, candidate.runtime.reasons)
    && sameStrings(
      current.runtime.unavailableSections,
      candidate.runtime.unavailableSections,
    )
    && sameSourceSection(
      current.runtime.sections.manager,
      candidate.runtime.sections.manager,
      false,
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
    )
    && current.alarms.ownerEpoch === candidate.alarms.ownerEpoch
    && current.alarms.generation === candidate.alarms.generation
    && sameComponentOverviewPresentation(
      current.componentOverview,
      candidate.componentOverview,
    )
    && current.authoringAdmission.authoringAllowed
      === candidate.authoringAdmission.authoringAllowed
    && current.authoringAdmission.admissionGeneration
      === candidate.authoringAdmission.admissionGeneration
    && current.executionControlStatus?.ownerEpoch
      === candidate.executionControlStatus?.ownerEpoch
    && current.executionControlStatus?.activePublicationGeneration
      === candidate.executionControlStatus?.activePublicationGeneration
    && current.executionControlStatus?.residentOperations.length
      === candidate.executionControlStatus?.residentOperations.length
    && (current.executionControlStatus?.residentOperations ?? [])
      .every((operation, index) => {
      const next = candidate.executionControlStatus?.residentOperations[index];
      return next !== undefined
        && operation.operationId === next.operationId
        && operation.operationRevision === next.operationRevision
        && operation.state === next.state
        && operation.resident === next.resident;
    })
    && current.executionControlStatus?.runtimeScaleControl?.controlGeneration
      === candidate.executionControlStatus?.runtimeScaleControl?.controlGeneration
    && current.executionControlStatus?.runtimeScaleControl?.requestedScale
      === candidate.executionControlStatus?.runtimeScaleControl?.requestedScale
    && current.programCycle.programRunnerOwnerEpoch
      === candidate.programCycle.programRunnerOwnerEpoch
    && current.programCycle.cycleGeneration === candidate.programCycle.cycleGeneration
    && current.programCycle.state === candidate.programCycle.state
    && current.programCycle.programRunId === candidate.programCycle.programRunId
    && current.programCycle.cycleClaimGeneration
      === candidate.programCycle.cycleClaimGeneration
    && current.programCycle.terminalOrSettlementReference
      === candidate.programCycle.terminalOrSettlementReference
    && current.programCycle.runRevision === candidate.programCycle.runRevision
    && current.programCycle.nodeStatusRevision
      === candidate.programCycle.nodeStatusRevision
    && current.programCycle.runtimeControlRevision
      === candidate.programCycle.runtimeControlRevision
    && current.programCycle.recoveryOffer?.offerId
      === candidate.programCycle.recoveryOffer?.offerId
    && current.programCycle.recoveryOffer?.offerRevision
      === candidate.programCycle.recoveryOffer?.offerRevision
    && current.draftIdentity.draftId === candidate.draftIdentity.draftId
    && current.draftIdentity.draftRevision === candidate.draftIdentity.draftRevision;
}

export function sameComponentOverviewPresentation(
  current: ComposerHeadlessState['componentOverview'],
  candidate: ComposerHeadlessState['componentOverview'],
): boolean {
  const currentBasis = current.basis;
  const candidateBasis = candidate.basis;
  return current.readiness === candidate.readiness
    && current.summaryLabel === candidate.summaryLabel
    && current.profileLabel === candidate.profileLabel
    && current.authorityLabel === candidate.authorityLabel
    && current.userMessage === candidate.userMessage
    && sameStrings(current.capabilities, candidate.capabilities)
    && current.components.length === candidate.components.length
    && current.components.every((component, index) => {
      const next = candidate.components[index];
      return next !== undefined
        && component.id === next.id
        && component.label === next.label
        && component.status === next.status
        && component.detail === next.detail
        && component.required === next.required
        && sameStrings(component.requiredFor, next.requiredFor)
        && component.ownerId === next.ownerId
        && component.ownerEpoch === next.ownerEpoch
        && component.generation === next.generation
        && component.reasonCode === next.reasonCode
        && component.scope === next.scope
        && sameStrings(component.contributorIds, next.contributorIds);
    })
    && currentBasis.runtimeProjectionOwnerEpoch
      === candidateBasis.runtimeProjectionOwnerEpoch
    // The aggregate runtime generation is a transport heartbeat when every
    // identified contributor below is unchanged.  It remains retained in the
    // latest source frame for diagnostics but must not wake the shell/viewer.
    && currentBasis.managerSourceOwnerEpoch === candidateBasis.managerSourceOwnerEpoch
    && currentBasis.logicalSceneSourceOwnerEpoch
      === candidateBasis.logicalSceneSourceOwnerEpoch
    && currentBasis.logicalSceneSourceGeneration
      === candidateBasis.logicalSceneSourceGeneration
    && currentBasis.sceneRuntimeSourceOwnerEpoch
      === candidateBasis.sceneRuntimeSourceOwnerEpoch
    && currentBasis.sceneRuntimeSourceGeneration
      === candidateBasis.sceneRuntimeSourceGeneration
    && currentBasis.alarmOwnerEpoch === candidateBasis.alarmOwnerEpoch
    && currentBasis.alarmGeneration === candidateBasis.alarmGeneration
    && currentBasis.alarmStateRevision === candidateBasis.alarmStateRevision
    && currentBasis.admissionGeneration === candidateBasis.admissionGeneration
    && currentBasis.programRunnerOwnerEpoch === candidateBasis.programRunnerOwnerEpoch
    && currentBasis.programCycleGeneration === candidateBasis.programCycleGeneration
    && currentBasis.appliedProgramOwnerEpoch === candidateBasis.appliedProgramOwnerEpoch
    && currentBasis.appliedProgramGeneration === candidateBasis.appliedProgramGeneration
    && currentBasis.executionOwnerEpoch === candidateBasis.executionOwnerEpoch
    && currentBasis.executionPublicationGeneration
      === candidateBasis.executionPublicationGeneration;
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
  private controlServerSequence = 0n;
  private controlClientSequence = 0;
  private controlHeartbeatConfirmed = false;
  private lastControlEchoSequence = 0;
  private executionControl: CurrentExecutionControl | null = null;
  private ordinaryControlPending: string | null = null;
  private runtimeScaleControlPending: string | null = null;
  private readonly commandReceipts = new Map<string, {
    resolve: (receipt: ExecutionCommandReceipt) => void;
    reject: (reason: Error) => void;
  }>();
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
  private readonly viewerListeners = new Set<Listener>();
  private readonly executionObservationListeners = new Set<Listener>();
  private readonly observationTransportListeners = new Set<Listener>();
  constructor() {
    headlessSourceRecords.add(this.qualificationRecord);
    publishHeadlessQualification();
    this.shellResult = { snapshot: null, error: null, reconnect: this.reconnect };
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
    if (
      frame !== this.latestExecutionObservation
      || frame === this.appliedExecutionObservation
    ) return;
    this.appliedExecutionObservation = frame;
    recordViewerObservationApplied({
      ownerEpoch: frame.ownerEpoch,
      sourceEpoch: frame.sourceEpoch,
      sourceSequence: frame.sourceSequence,
      receivedAtComposerMonotonicUs: frame.receivedAtMonotonicUs,
    });
    this.sendObservationCredit(frame);
  };
  readonly getExecutionObservationTransportStatus = () =>
    this.observationTransportStatus;
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
      this.commandReceipts.set(correlationId, { resolve, reject });
      if (runtimeScale) this.runtimeScaleControlPending = correlationId;
      else if (!stop) this.ordinaryControlPending = correlationId;
      try {
        socket.send(JSON.stringify(command));
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
    this.cancelPublications();
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
    this.cancelPublications();
    this.shellListeners.clear();
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
    socket?.close(1000, reason);
    this.latest = null;
    this.presentedViewer = null;
    this.presentedFrame = null;
    this.publishImmediate();
  }

  private clearControlSession(reason: string): void {
    this.controlSessionId = null;
    this.controlReceiverIncarnation = null;
    this.controlServerSequence = 0n;
    this.executionControl = null;
    this.controlClientSequence = 0;
    this.controlHeartbeatConfirmed = false;
    this.lastControlEchoSequence = 0;
    this.ordinaryControlPending = null;
    this.runtimeScaleControlPending = null;
    for (const pending of this.commandReceipts.values()) {
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
    socket?.close(1000, reason);
  }

  private connect(): void {
    if (!this.enabled || this.disposed) return;
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
          this.controlServerSequence = 0n;
          this.controlClientSequence = 0;
          this.controlHeartbeatConfirmed = false;
          this.lastControlEchoSequence = 0;
          this.scheduleControlSynchronizationWatch(serial, socket);
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
          if (
            !canonicalPositiveU64(record.challenge_sequence)
            || record.freshness === null
            || typeof record.freshness !== 'object'
            || Array.isArray(record.freshness)
          ) throw new Error('Control heartbeat is invalid.');
          const freshness = record.freshness as unknown as ControlFreshnessContext;
          if (
            typeof freshness.context_id !== 'string'
            || typeof freshness.receiver_incarnation !== 'string'
            || typeof freshness.semantic_owner_epoch !== 'string'
            || !Number.isSafeInteger(freshness.execution_control_revision)
            || !canonicalPositiveU64(freshness.issued_server_sequence)
            || freshness.valid_for_ns !== 750_000_000
          ) throw new Error('Control freshness context is invalid.');
          if (this.executionControl !== null) {
            this.executionControl = { ...this.executionControl, freshness };
          }
          const received = record.received_client_sequence;
          const confirmedNow = this.lastControlEchoSequence > 0
            && canonicalPositiveU64(received)
            && BigInt(received) >= BigInt(this.lastControlEchoSequence);
          if (confirmedNow) {
            this.scheduleControlHeartbeatWatch(serial, socket);
            if (!this.controlHeartbeatConfirmed) {
              this.controlHeartbeatConfirmed = true;
              this.latestError = null;
              if (this.latest !== null) {
                this.scheduleShell();
                this.scheduleViewer();
              }
            }
          }
          this.controlClientSequence += 1;
          this.lastControlEchoSequence = this.controlClientSequence;
          socket.send(JSON.stringify({
            schema_version: CONTROL_PROTOCOL_VERSION,
            message_kind: 'heartbeat_echo',
            control_session_id: this.controlSessionId,
            client_sequence: String(this.controlClientSequence),
            challenge_sequence: record.challenge_sequence,
          }));
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
            || receipt.client_sequence !== record.received_client_sequence
            || receipt.receiver_incarnation !== this.controlReceiverIncarnation
            || !canonicalPositiveU64(receipt.client_sequence)
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
          if (pending !== undefined) {
            this.commandReceipts.delete(correlationId);
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
              pending.reject(new Error(code));
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
        const controlValue = record.control;
        if (controlValue === null || typeof controlValue !== 'object'
          || Array.isArray(controlValue)) {
          throw new Error('Control projection is unavailable.');
        }
        const control = controlValue as unknown as CurrentExecutionControl;
        if (typeof control.receiver_incarnation !== 'string'
          || typeof control.semantic_owner_epoch !== 'string'
          || !Number.isSafeInteger(control.execution_control_revision)
          || control.freshness === null || typeof control.freshness !== 'object') {
          throw new Error('Control projection identity is invalid.');
        }
        this.executionControl = control;
        const shell = record.shell;
        if (shell === null || typeof shell !== 'object' || Array.isArray(shell)) {
          throw new Error('Control state message omitted its shell.');
        }
        const shellRecord = shell as Record<string, unknown>;
        let candidate: ComposerHeadlessState;
        if ('runtime' in shellRecord) {
          candidate = parseComposerHeadlessState(shellRecord);
        } else {
          if (this.latest === null || !('program_cycle' in shellRecord)) {
            throw new Error('Control delta preceded its snapshot barrier.');
          }
          const draft = shellRecord.draft_identity;
          if (draft === null || typeof draft !== 'object' || Array.isArray(draft)) {
            throw new Error('Control delta omitted its Draft identity.');
          }
          const draftRecord = draft as Record<string, unknown>;
          if (typeof draftRecord.draft_id !== 'string'
            || !Number.isSafeInteger(draftRecord.draft_revision)
            || typeof shellRecord.receiver_epoch !== 'string'
            || !Number.isSafeInteger(shellRecord.snapshot_sequence)) {
            throw new Error('Control delta shell identity is invalid.');
          }
          candidate = {
            ...this.latest,
            programCycle: parseProgramCycleStatus(shellRecord.program_cycle),
            ...(shellRecord.execution_targets !== undefined
              && shellRecord.execution_control_state !== undefined
              ? {
                  executionControlStatus: parseExecutionControlStatus(
                    shellRecord.execution_targets,
                    shellRecord.execution_control_state,
                  ),
                }
              : {}),
            draftIdentity: {
              draftId: draftRecord.draft_id,
              draftRevision: draftRecord.draft_revision as number,
            },
            receiverEpoch: shellRecord.receiver_epoch,
            snapshotSequence: shellRecord.snapshot_sequence as number,
          };
        }
        const admission = admitHeadlessState(this.latest, candidate);
        if (!admission.accepted) {
          this.publishError(admission.reason);
          socket.close(1002, 'non-monotonic headless projection');
          return;
        }
        const shellChanged = !sameHeadlessProjection(this.latest, candidate);
        const viewerChanged = !sameViewerProjection(this.latest, candidate);
        this.latest = candidate;
        if (kind === 'snapshot' && !this.controlHeartbeatConfirmed) {
          this.scheduleControlHeartbeatWatch(serial, socket);
        }
        if (this.controlHeartbeatConfirmed) this.latestError = null;
        this.advancePresentedFrame(candidate);
        if (this.controlHeartbeatConfirmed) {
          if (viewerChanged) this.scheduleViewer();
          if (shellChanged) this.scheduleShell();
        }
      } catch (reason) {
        this.publishError(
          reason instanceof Error
            ? reason.message
            : 'Invalid headless WebSocket projection.',
        );
        socket.close(1002, 'invalid headless projection');
      }
    };
    socket.onerror = () => {
      if (serial !== this.serial || this.socket !== socket) return;
      this.publishError('Headless WebSocket transport failed.');
    };
    socket.onclose = () => {
      if (serial !== this.serial || !this.enabled || this.disposed) return;
      if (this.socket === socket) {
        this.socket = null;
      }
      this.clearControlHeartbeatWatch();
      this.clearControlSession('control connection closed');
      // An actual close establishes a new snapshot barrier even if this
      // session had already been classified as delayed or stale.
      this.publishError('Headless projection disconnected.');
      this.reconnectTimer = setTimeout(() => {
        if (serial === this.serial) this.connect();
      }, RECONNECT_DELAY_MS);
    };
  }

  private clearControlHeartbeatWatch(): void {
    if (this.controlHeartbeatTimer !== null) {
      clearTimeout(this.controlHeartbeatTimer);
      this.controlHeartbeatTimer = null;
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
      this.latestError = 'Control connection is delayed; commands are disabled.';
      this.cancelPublications();
      this.publishImmediate();
      this.controlHeartbeatTimer = setTimeout(() => {
        if (serial !== this.serial || this.socket !== socket) return;
        this.controlHeartbeatTimer = null;
        this.controlHeartbeatConfirmed = false;
        this.latestError = 'Control connection is stale; commands are disabled.';
        this.cancelPublications();
        this.publishImmediate();
        // Keep the live transport. The server keeps one challenge
        // outstanding and can recover the session without reconnect churn;
        // it separately reclaims a genuinely dead peer after a bounded wait.
      }, (CONTROL_HEARTBEAT_STALE_NS - CONTROL_HEARTBEAT_DELAYED_NS) / 1_000_000);
    }, CONTROL_HEARTBEAT_DELAYED_NS / 1_000_000);
  }

  private scheduleControlSynchronizationWatch(
    serial: number,
    socket: WebSocket,
  ): void {
    this.clearControlHeartbeatWatch();
    this.controlHeartbeatTimer = setTimeout(() => {
      if (serial !== this.serial || this.socket !== socket) return;
      this.controlHeartbeatTimer = null;
      this.controlHeartbeatConfirmed = false;
      this.latestError = 'Control state synchronization is delayed; commands are disabled.';
      this.cancelPublications();
      this.publishImmediate();
      // Reconnecting cannot make a semantic owner become current. Keep this
      // one transport open so the backend source can recover without churn.
    }, CONTROL_HEARTBEAT_STALE_NS / 1_000_000);
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
          socket.close(1002, 'non-monotonic execution observation');
          return;
        }
        if (
          current !== null
          && candidate.sourceSequence === current.sourceSequence
          && candidate.ownerEpoch === current.ownerEpoch
          && candidate.sourceEpoch === current.sourceEpoch
        ) {
          if (!sameExecutionObservationFrame(current, candidate)) {
            socket.close(1002, 'changed execution observation at one sequence');
            return;
          }
          this.publishObservationTransportStatus('live', null);
          this.appliedExecutionObservation = current;
          this.sendObservationCredit(current);
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
        socket.close(1002, 'invalid execution observation');
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
      if (this.observationTransportStatus.phase !== 'reconnecting') {
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
      }, RECONNECT_DELAY_MS);
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
    return {
      snapshot: this.latest,
      error: this.latestError,
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
    // Commands must lose authority immediately, but a transport interruption
    // must not reinterpret the last coherent owner scene as an empty scene.
    // Keep the structural viewer snapshot. The independently bounded viewer
    // transport owns dynamic-observation validity and reconnects separately.
    const retainedViewerSnapshot = error === null
      ? null
      : this.viewerResult.snapshot;
    this.latestError = error;
    if (error !== null) {
      this.latest = null;
      this.presentedViewer = null;
      this.presentedFrame = null;
    }
    this.cancelPublications();
    this.publishImmediate(retainedViewerSnapshot);
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

  private scheduleShell(): void {
    if (this.shellTimer !== null) return;
    this.shellTimer = setTimeout(() => {
      this.shellTimer = null;
      this.shellResult = this.result();
      for (const listener of this.shellListeners) listener();
    }, HEADLESS_SHELL_PUBLICATION_MS);
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

  private cancelPublications(): void {
    if (this.shellTimer !== null) clearTimeout(this.shellTimer);
    this.shellTimer = null;
    this.cancelViewerFrame();
    this.cancelObservationFrame();
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
