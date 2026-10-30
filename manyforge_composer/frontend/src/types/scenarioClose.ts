import type { BoundedPublicRequestContext } from './cclc';

/** Complete owner-issued CAS rows; these do not change ordinary target_view. */
export interface LifecycleTargetRetirementScope {
  request_owner_epoch: string;
  publication_generation: number;
  active_public_pointer_digest: string;
}
export interface LifecycleTargetRetirementRow {
  controllable_component_id: string;
  requested_state: 'active' | 'prepared' | 'disabled';
  origin: 'startup' | 'operator' | 'program';
  target_generation: number;
  reached: boolean;
  recovered_cause: 'none' | 'availability' | 'emergency_stop' | 'protective_stop' | 'safeguard_stop'
    | 'fault' | 'configuration' | 'unknown' | 'collision' | 'external_control' | 'provider_lost' | null;
}
export interface LifecycleTargetRetirementIntent {
  request_context: BoundedPublicRequestContext;
  scope: LifecycleTargetRetirementScope;
  expected_targets: LifecycleTargetRetirementRow[];
}
export interface LifecycleTargetRetirementOffer {
  scope: LifecycleTargetRetirementScope;
  targets: LifecycleTargetRetirementRow[];
}
export interface LifecycleTargetRetirementResult {
  request_context: BoundedPublicRequestContext;
  replay_status: 'accepted_new' | 'replayed_exact';
  scope: LifecycleTargetRetirementScope;
  retired_targets: LifecycleTargetRetirementRow[];
}
export interface LifecycleOwnerSettlementResult {
  scope: LifecycleTargetRetirementScope;
  settled: true;
}
/** Passive sealed selection; it supplies no prepared activation or Apply proof. */
export interface CanonicalEmptySelection {
  request_owner_epoch: string;
  cell_id: string;
  deployment_artifact_id: string;
  deployment_revision: string;
  candidate_retained_bundle_id: string;
  candidate_graph_revision: string;
  admission_digest: string;
}
/** Item 37: one step of an operator Force close (Headless runs each step). */
/** `end` (SX-616): sent on every exit after the Full Stop; it ends the server's attachment exception. */
export type ForceCloseStep = 'stop' | 'close_operations' | 'end';
export interface ForceCloseRequest {
  request_context: { request_id: string; request_owner_epoch: string; source_label: string; correlation_id: string | null };
  step: ForceCloseStep;
  /** Step `stop` only: the cell's current expected Stop generation. */
  expected_stop_generation?: number;
}
/** What the Kernel step moved into history; its effect stays unknown. */
export interface ForceClosedOperation {
  operationId: string;
  operationRevision: number;
  state: string;
  containmentUncertain: boolean;
  targets: { controllableComponentId: string; commandTargetId: string; outputUnknown: boolean }[];
}
export interface ForceCloseResult {
  step: ForceCloseStep;
  stopGeneration: number;
  operations: ForceClosedOperation[];
  /** The cell-wide motion block of an unresolved operation closure was cleared. */
  motionBlockCleared: boolean;
}
