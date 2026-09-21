export interface ExecutionObservationTarget {
  controllableComponentId: string;
  commandTargetId: string;
  sourceEpoch: string;
  sourceSequence: number;
  sourceJointNames: string[];
  compiledJointNames: string[];
  coordinateKinds: Array<'angular' | 'linear'>;
  positionsSi: number[];
}

export interface ExecutionObservationFrame {
  schemaVersion: 'manyforge.execution_observation_frame.v2';
  ownerEpoch: string;
  cellId: string;
  retainedBundleId: string;
  graphRevision: string;
  sourceEpoch: string;
  sourceSequence: number;
  receivedAtMonotonicUs: number;
  targets: ExecutionObservationTarget[];
}

export interface ExecutionObservationReader {
  getLatestExecutionObservation: () => ExecutionObservationFrame | null;
  subscribeExecutionObservation: (listener: () => void) => () => void;
  markExecutionObservationApplied: (frame: ExecutionObservationFrame) => void;
  rejectExecutionObservation: (frame: ExecutionObservationFrame) => void;
}

export type ExecutionObservationTransportPhase =
  | 'disabled'
  | 'connecting'
  | 'waiting'
  | 'live'
  /**
   * The current frame belongs to the previous runtime/model identity. The
   * transport is being replaced deliberately; this is not a network fault.
   */
  | 'synchronizing'
  | 'reconnecting';

export interface ExecutionObservationTransportStatus {
  phase: ExecutionObservationTransportPhase;
  message: string | null;
}
