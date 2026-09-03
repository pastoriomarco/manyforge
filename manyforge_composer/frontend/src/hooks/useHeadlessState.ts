import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import { parseComposerHeadlessState } from '../api/parsers';
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

function websocketUrl(path: '/ws/state' | '/ws/scene/viewer'): string {
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
    const socket = this.socket;
    this.socket = null;
    socket?.close(1000, reason);
    this.latest = null;
    this.presentedViewer = null;
    this.presentedFrame = null;
    this.publishImmediate();
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
    const socket = new WebSocket(websocketUrl('/ws/state'));
    this.socket = socket;
    socket.onopen = () => {
      if (
        serial !== this.serial
        || this.socket !== socket
        || !this.enabled
        || this.disposed
      ) return;
    };
    socket.onmessage = (message) => {
      if (serial !== this.serial) return;
      try {
        const raw: unknown = JSON.parse(String(message.data));
        if (raw !== null && typeof raw === 'object' && !Array.isArray(raw)) {
          const errorValue = (raw as Record<string, unknown>).error;
          if (
            errorValue !== null
            && typeof errorValue === 'object'
            && !Array.isArray(errorValue)
          ) {
            const record = errorValue as Record<string, unknown>;
            this.publishError(typeof record.message === 'string'
              ? record.message
              : 'Headless projection unavailable.');
            return;
          }
        }
        const candidate = parseComposerHeadlessState(raw);
        const admission = admitHeadlessState(this.latest, candidate);
        if (!admission.accepted) {
          this.publishError(admission.reason);
          socket.close(1002, 'non-monotonic headless projection');
          return;
        }
        const shellChanged = !sameHeadlessProjection(this.latest, candidate);
        const viewerChanged = !sameViewerProjection(this.latest, candidate);
        this.latest = candidate;
        this.latestError = null;
        this.advancePresentedFrame(candidate);
        if (viewerChanged) this.scheduleViewer();
        if (shellChanged) this.scheduleShell();
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
      if (this.latestError === null) {
        this.publishError('Headless projection disconnected.');
      }
      this.reconnectTimer = setTimeout(() => {
        if (serial === this.serial) this.connect();
      }, RECONNECT_DELAY_MS);
    };
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
