import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { resolveStructuralAssetUrl } from '../api/client';
import { parseComposerHeadlessState } from '../api/parsers';
import type { ComposerHeadlessState } from '../types/headless';
import {
  setViewerQualificationTransport,
  type ViewerQualificationTransportState,
} from '../utils/viewerPerformance';

const RECONNECT_DELAY_MS = 1000;
export const HEADLESS_SHELL_PUBLICATION_MS = 100;

export interface HeadlessStateResult {
  snapshot: ComposerHeadlessState | null;
  error: string | null;
  reconnect: () => void;
}

export interface HeadlessStateAdmission {
  accepted: boolean;
  reason: string | null;
}

function exactSnapshotEqual(left: unknown, right: unknown): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

function monotonicOwnerSection(
  label: string,
  currentOwnerId: string,
  currentOwnerEpoch: string,
  currentGeneration: number,
  candidateOwnerId: string,
  candidateOwnerEpoch: string,
  candidateGeneration: number,
  currentValue: unknown,
  candidateValue: unknown,
): string | null {
  if (candidateOwnerEpoch !== currentOwnerEpoch) return null;
  if (candidateOwnerId !== currentOwnerId) {
    return `${label} reused an owner epoch with a different owner ID.`;
  }
  if (candidateGeneration < currentGeneration) {
    return `${label} generation regressed within one owner epoch.`;
  }
  if (
    candidateGeneration === currentGeneration
    && !exactSnapshotEqual(candidateValue, currentValue)
  ) {
    return `${label} conflicted at an already observed owner generation.`;
  }
  return null;
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
  const runtimeReason = monotonicOwnerSection(
    'Headless runtime projection',
    current.runtime.projectionOwnerId,
    current.runtime.projectionOwnerEpoch,
    current.runtime.projectionGeneration,
    candidate.runtime.projectionOwnerId,
    candidate.runtime.projectionOwnerEpoch,
    candidate.runtime.projectionGeneration,
    current.runtime,
    candidate.runtime,
  );
  if (runtimeReason !== null) return { accepted: false, reason: runtimeReason };
  const alarmReason = monotonicOwnerSection(
    'Headless alarm projection',
    current.alarms.ownerId,
    current.alarms.ownerEpoch,
    current.alarms.generation,
    candidate.alarms.ownerId,
    candidate.alarms.ownerEpoch,
    candidate.alarms.generation,
    current.alarms,
    candidate.alarms,
  );
  return alarmReason === null
    ? { accepted: true, reason: null }
    : { accepted: false, reason: alarmReason };
}

function websocketUrl(clientId: string): string {
  const resolved = new URL(
    resolveStructuralAssetUrl('/ws/state'),
    window.location.href,
  );
  resolved.protocol = resolved.protocol === 'https:' ? 'wss:' : 'ws:';
  resolved.searchParams.set('client_id', clientId);
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

function browserClientId(): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `composer-browser.${random}`;
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
    socketPath: '/ws/state',
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
 * One transport and two bounded delivery lanes for copied owner facts.
 * This source performs parsing, admission, and scheduling only.
 */
export class HeadlessStateSource {
  private enabled = false;
  private disposed = false;
  private serial = 0;
  private socket: WebSocket | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private shellTimer: ReturnType<typeof setTimeout> | null = null;
  private viewerFrame: number | ReturnType<typeof setTimeout> | null = null;
  private latest: ComposerHeadlessState | null = null;
  private latestError: string | null = null;
  private shellResult: HeadlessStateResult;
  private viewerResult: HeadlessStateResult;
  private readonly qualificationRecord: HeadlessSourceQualificationRecord = {
    enabled: false,
    disposed: false,
    socketActive: false,
  };
  private readonly shellListeners = new Set<Listener>();
  private readonly viewerListeners = new Set<Listener>();
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

  readonly getShellSnapshot = () => this.shellResult;
  readonly getViewerSnapshot = () => this.viewerResult;

  setEnabled(enabled: boolean): void {
    if (this.disposed || enabled === this.enabled) return;
    this.enabled = enabled;
    this.qualificationRecord.enabled = enabled;
    this.stopConnection('Composer headless source disabled');
    this.cancelPublications();
    this.latest = null;
    this.latestError = null;
    if (enabled) {
      this.connect();
    } else {
      this.publishImmediate(null);
    }
    publishHeadlessQualification();
  }

  readonly reconnect = (): void => {
    if (!this.enabled || this.disposed) return;
    this.stopConnection('Composer client reconnect');
    this.connect();
  };

  dispose(): void {
    this.disposed = true;
    this.enabled = false;
    this.qualificationRecord.enabled = false;
    this.stopConnection('Composer client unmounted');
    this.cancelPublications();
    this.shellListeners.clear();
    this.viewerListeners.clear();
    headlessSourceRecords.delete(this.qualificationRecord);
    publishHeadlessQualification();
  }

  private stopConnection(reason: string): void {
    this.serial += 1;
    if (this.reconnectTimer !== null) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    const socket = this.socket;
    this.socket = null;
    if (socket !== null) {
      this.qualificationRecord.socketActive = false;
      publishHeadlessQualification();
    }
    socket?.close(1000, reason);
    this.latest = null;
    this.publishImmediate(this.result());
  }

  private connect(): void {
    if (!this.enabled || this.disposed) return;
    const serial = ++this.serial;
    const clientId = browserClientId();
    const socket = new WebSocket(websocketUrl(clientId));
    this.socket = socket;
    headlessSocketCreations += 1;
    publishHeadlessQualification();
    socket.onopen = () => {
      if (
        serial !== this.serial
        || this.socket !== socket
        || !this.enabled
        || this.disposed
      ) return;
      this.qualificationRecord.socketActive = true;
      const activeSockets = headlessQualificationSnapshot().activeSockets;
      headlessMaxActiveSockets = Math.max(headlessMaxActiveSockets, activeSockets);
      publishHeadlessQualification();
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
        const candidate = {
          ...parseComposerHeadlessState(raw),
          clientId,
        };
        const admission = admitHeadlessState(this.latest, candidate);
        if (!admission.accepted) {
          this.publishError(admission.reason);
          socket.close(1002, 'non-monotonic headless projection');
          return;
        }
        this.latest = candidate;
        this.latestError = null;
        this.scheduleViewer();
        this.scheduleShell();
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
      this.qualificationRecord.socketActive = false;
      publishHeadlessQualification();
      this.publishError('Headless WebSocket transport failed.');
    };
    socket.onclose = () => {
      if (serial !== this.serial || !this.enabled || this.disposed) return;
      if (this.socket === socket) {
        this.socket = null;
        this.qualificationRecord.socketActive = false;
        publishHeadlessQualification();
      }
      if (this.latestError === null) {
        this.publishError('Headless projection disconnected.');
      }
      this.reconnectTimer = setTimeout(() => {
        if (serial === this.serial) this.connect();
      }, RECONNECT_DELAY_MS);
    };
  }

  private result(): HeadlessStateResult {
    return {
      snapshot: this.latest,
      error: this.latestError,
      reconnect: this.reconnect,
    };
  }

  private publishError(error: string | null): void {
    this.latestError = error;
    if (error !== null) this.latest = null;
    this.cancelPublications();
    this.publishImmediate(this.result());
  }

  private publishImmediate(result: HeadlessStateResult | null): void {
    const next = result ?? {
      snapshot: this.latest,
      error: this.latestError,
      reconnect: this.reconnect,
    };
    this.shellResult = next;
    this.viewerResult = next;
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

  private cancelPublications(): void {
    if (this.shellTimer !== null) clearTimeout(this.shellTimer);
    this.shellTimer = null;
    this.cancelViewerFrame();
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
  return useSyncExternalStore(
    active ? source.subscribeViewer : () => () => undefined,
    active ? source.getViewerSnapshot : () => inactive.current,
    active ? source.getViewerSnapshot : () => inactive.current,
  );
}
