export interface SequencedFrame<T> {
  streamIdentity: string;
  sequence: bigint;
  frame: T | null;
}

export interface LatestFrameSlot<T> {
  streamIdentity: string | null;
  sequence: bigint;
  version: number;
  appliedVersion: number;
  frame: T | null;
}

export function emptyLatestFrameSlot<T>(): LatestFrameSlot<T> {
  return {
    streamIdentity: null,
    sequence: -1n,
    version: 0,
    appliedVersion: -1,
    frame: null,
  };
}

/**
 * Retain only the newest validated frame for render-time materialization.
 * Thin observer clients decode owner-issued frames; this reducer has no
 * viewer-protocol knowledge or runtime authority.
 */
export function publishLatestFrame<T>(
  state: LatestFrameSlot<T>,
  incoming: SequencedFrame<T>,
): LatestFrameSlot<T> {
  if (
    state.streamIdentity === incoming.streamIdentity
    && incoming.sequence <= state.sequence
  ) {
    return state;
  }
  return {
    ...state,
    streamIdentity: incoming.streamIdentity,
    sequence: incoming.sequence,
    version: state.version + 1,
    frame: incoming.frame,
  };
}

export function consumeLatestFrame<T>(state: LatestFrameSlot<T>): {
  state: LatestFrameSlot<T>;
  changed: boolean;
  frame: T | null;
} {
  if (state.appliedVersion === state.version) {
    return { state, changed: false, frame: state.frame };
  }
  return {
    state: { ...state, appliedVersion: state.version },
    changed: true,
    frame: state.frame,
  };
}

export function invalidateLatestFrame<T>(
  state: LatestFrameSlot<T>,
): LatestFrameSlot<T> {
  return {
    ...state,
    streamIdentity: null,
    sequence: -1n,
    version: state.version + 1,
    frame: null,
  };
}

export function ownsConnectionGeneration<T>(
  candidate: T,
  current: T | null,
  generation: number,
  currentGeneration: number,
): boolean {
  return current === candidate && generation === currentGeneration;
}
