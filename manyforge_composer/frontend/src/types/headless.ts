export type SourceFreshness = 'fresh' | 'stale' | 'unavailable';

export interface HeadlessSourceSection {
  available: boolean;
  freshness: SourceFreshness;
  knownGaps: string[];
  payload: Record<string, unknown> | null;
  sourceGeneration: number | null;
  sourceOwnerEpoch: string | null;
  sourceOwnerId: string | null;
  receiverAgeNs: number | null;
}

export interface HeadlessRuntimeSnapshot {
  schemaVersion: 'manyforge.headless_runtime_snapshot.v1';
  complete: boolean;
  projectionGeneration: number;
  projectionOwnerEpoch: string;
  projectionOwnerId: string;
  reasons: string[];
  unavailableSections: string[];
  sections: {
    manager: HeadlessSourceSection;
    logicalScene: HeadlessSourceSection;
    sceneRuntime: HeadlessSourceSection;
  };
}

export type AlarmLifecycleState =
  | 'active_unacknowledged'
  | 'active_acknowledged'
  | 'returned_unacknowledged'
  | 'returned_acknowledged';

export type AlarmPriority = 'critical' | 'high' | 'medium' | 'low';

export interface AlarmSubject {
  subjectKind: string;
  subjectId: string;
  resourceIds: string[];
}

export interface AlarmMembership {
  conditionId: string;
  activationId: string;
  recordRevision: number;
}

export interface OperatorAlarmRecord extends AlarmMembership {
  activatedAtNs: number;
  definitionId: string;
  lifecycleState: AlarmLifecycleState;
  active: boolean;
  acknowledged: boolean;
  priority: AlarmPriority;
  alarmClass: string;
  title: string;
  message: string;
  consequence: string;
  operatorResponse: string;
  occurrenceCount: number;
  requiresMotionAttention: boolean;
  subject: AlarmSubject;
  details: Record<string, unknown>;
}

export interface OperatorAlarmSnapshot {
  schemaVersion: 'manyforge.operator_alarm_snapshot.v2';
  ownerId: string;
  ownerEpoch: string;
  generation: number;
  stateRevision: number;
  reconciled: boolean;
  healthState: string;
  liveCount: number;
  unacknowledgedCount: number;
  activeCount: number;
  activeAcknowledgedCount: number;
  motionAttentionCount: number;
  unacknowledgedMembership: AlarmMembership[];
  alarms: OperatorAlarmRecord[];
  truncated: boolean;
}

export interface ComposerHeadlessState {
  schemaVersion: 'manyforge.composer_headless_state.v1';
  clientId: string;
  complete: true;
  runtime: HeadlessRuntimeSnapshot;
  alarms: OperatorAlarmSnapshot;
  authoringAdmission: {
    authoringAllowed: boolean;
    admissionGeneration: number;
  };
  programCycle: {
    programRunnerOwnerEpoch: string;
    cycleGeneration: number;
    state: 'idle_after_reset' | 'starting' | 'running' | 'stopping'
      | 'terminal_awaiting_reset';
    programRunId: string | null;
    cycleClaimGeneration: number | null;
    terminalOrSettlementReference: string | null;
  };
  draftIdentity: { draftId: string; draftRevision: number };
  receiverEpoch: string;
  snapshotSequence: number;
}

export interface HistorianEvent {
  schemaVersion: 'manyforge.historian_event.v1';
  eventId: string;
  component: string;
  eventType: string;
  severity: string;
  occurredAtNs: number;
  occurredAtWall: string;
  payload: Record<string, unknown>;
}

export interface HistorianStoredEvent {
  sequence: number;
  event: HistorianEvent;
}

export interface HistorianPage {
  schemaVersion: 'manyforge.historian_page.v1';
  events: HistorianStoredEvent[];
  nextBeforeSequence: number | null;
  truncated: boolean;
}
