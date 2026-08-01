export interface SequencedFrame<T> {
  streamEpoch: bigint;
  sequence: bigint;
  frame: T | null;
}

export interface LatestFrameSlot<T> {
  streamEpoch: bigint | null;
  sequence: bigint;
  version: number;
  appliedVersion: number;
  frame: T | null;
}

export function emptyLatestFrameSlot<T>(): LatestFrameSlot<T> {
  return {
    streamEpoch: null,
    sequence: -1n,
    version: 0,
    appliedVersion: -1,
    frame: null,
  };
}

/**
 * Retain only the newest validated frame for render-time materialization.
 * Future I5-I9 thin clients and observer adapters decode owner-issued frames;
 * this reducer has no viewer-protocol knowledge or runtime authority.
 */
export function publishLatestFrame<T>(
  state: LatestFrameSlot<T>,
  incoming: SequencedFrame<T>,
): LatestFrameSlot<T> {
  if (
    state.streamEpoch === incoming.streamEpoch
    && incoming.sequence <= state.sequence
  ) {
    return state;
  }
  return {
    ...state,
    streamEpoch: incoming.streamEpoch,
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
    streamEpoch: null,
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
