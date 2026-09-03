export interface ExecutionObservationFrame {
  schemaVersion: 'manyforge.execution_observation_frame.v1';
  ownerEpoch: string;
  cellId: string;
  retainedBundleId: string;
  graphRevision: string;
  controllableComponentId: string;
  commandTargetId: string;
  sourceEpoch: string;
  sourceSequence: number;
  receivedAtMonotonicUs: number;
  sourceJointNames: string[];
  compiledJointNames: string[];
  positionsRad: number[];
}

export interface ExecutionObservationReader {
  getLatestExecutionObservation: () => ExecutionObservationFrame | null;
  subscribeExecutionObservation: (listener: () => void) => () => void;
  markExecutionObservationApplied: (frame: ExecutionObservationFrame) => void;
}

export type ExecutionObservationTransportPhase =
  | 'disabled'
  | 'connecting'
  | 'waiting'
  | 'live'
  | 'reconnecting';

export interface ExecutionObservationTransportStatus {
  phase: ExecutionObservationTransportPhase;
  message: string | null;
}
