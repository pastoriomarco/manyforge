import type { ProgramCycleStatus } from './api';
import type { Nanoseconds } from './nanoseconds';
import type { ExecutionOwnerStatus } from './robotControl';

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
  activatedAtNs: Nanoseconds;
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

export type ComponentOverviewReadiness =
  | 'ready'
  | 'waiting'
  | 'degraded'
  | 'fault'
  | 'offline';

export type ComponentOverviewComponentStatus =
  | 'healthy'
  | 'waiting'
  | 'warning'
  | 'fault'
  | 'offline';

export interface ComponentOverviewComponent {
  id: string;
  label: string;
  status: ComponentOverviewComponentStatus;
  detail: string;
  required: boolean;
  requiredFor: string[];
  ownerId: string | null;
  ownerEpoch: string | null;
  generation: number | null;
  reasonCode: string | null;
  scope: string;
  contributorIds: string[];
}

export interface ComponentOverviewProjection {
  schemaVersion: 'manyforge.component_overview.v1';
  overviewRevision: string;
  readiness: ComponentOverviewReadiness;
  summaryLabel: string;
  profileLabel: string;
  authorityLabel: string;
  userMessage: string;
  capabilities: string[];
  components: ComponentOverviewComponent[];
  basis: {
    runtimeProjectionOwnerEpoch: string | null;
    runtimeProjectionGeneration: number | null;
    managerSourceOwnerEpoch: string | null;
    logicalSceneSourceOwnerEpoch: string | null;
    logicalSceneSourceGeneration: number | null;
    sceneRuntimeSourceOwnerEpoch: string | null;
    sceneRuntimeSourceGeneration: number | null;
    alarmOwnerEpoch: string | null;
    alarmGeneration: number | null;
    alarmStateRevision: number | null;
    admissionGeneration: number | null;
    programRunnerOwnerEpoch: string | null;
    programCycleGeneration: number | null;
    appliedProgramOwnerEpoch: string | null;
    appliedProgramGeneration: number | null;
    executionOwnerEpoch: string | null;
    executionPublicationGeneration: number | null;
  };
}

export interface ComposerHeadlessState {
  schemaVersion: 'manyforge.composer_headless_state.v2';
  complete: true;
  runtime: HeadlessRuntimeSnapshot;
  alarms: OperatorAlarmSnapshot;
  componentOverview: ComponentOverviewProjection;
  authoringAdmission: {
    authoringAllowed: boolean;
    admissionGeneration: number;
  };
  executionControlStatus: ExecutionOwnerStatus | null;
  programCycle: ProgramCycleStatus;
  draftIdentity: { draftId: string; draftRevision: number };
  receiverEpoch: string;
  snapshotSequence: number;
}

export interface HistorianEvent {
  schemaVersion: 'manyforge.historian_event.v2';
  eventId: string;
  component: string;
  eventType: string;
  severity: string;
  occurredAtNs: string;
  occurredAtWall: string;
  payload: Record<string, unknown>;
}

export interface HistorianStoredEvent {
  sequence: number;
  event: HistorianEventSummary;
}

export interface HistorianDiscovery {
  schemaVersion: 'manyforge.historian_discovery.v1';
  kind: 'program_run_id' | 'component' | 'channel' | 'event_type';
  values: string[];
  truncated: boolean;
  nextAfter: string | null;
}

export interface HistorianPage {
  schemaVersion: 'manyforge.historian_summary_page.v2';
  events: HistorianStoredEvent[];
  nextBeforeSequence: number | null;
  truncated: boolean;
  timeWindow: { startUtcNs: string | null; endUtcNs: string | null };
  retention: HistorianRetention | null;
}

export interface HistorianRetention {
  schemaVersion: 'manyforge.historian_retention.v1';
  partitionRowLimit: string;
  partitionRecordByteLimit: string;
  databasePageByteLimit: string;
  durationGuaranteed: false;
  priorPruningKnown: boolean;
  partitions: Array<{
    partition: 'routine' | 'important';
    retainedCount: string;
    retainedBytes: string;
    prunedCount: string;
    prunedBytes: string;
    lastPrunedSequence: string | null;
    unknownTimeCount: string;
    regressedTimeCount: string;
    lossCounterSaturated: boolean;
    firstRecordedUtcNs: string | null;
    lastRecordedUtcNs: string | null;
  }>;
}

export interface HistorianEventSummary {
  schemaVersion: 'manyforge.historian_event_summary.v2';
  eventId: string;
  component: string;
  eventType: string;
  severity: string;
  occurredAtNs: string;
  occurredAtWall: string;
  message: string;
  programRunId: string | null;
  operationId: string | null;
  conditionId: string | null;
  conditionSeverity: string;
  occurredAtUtcNs: string | null;
  clockOwner: string | null;
  wallClockRegressed: boolean;
}

export type HistorianDetailSectionName =
  | 'incident'
  | 'executionDigest'
  | 'blackboard'
  | 'latency';

export interface HistorianDetailSection {
  available: boolean;
  reason: string | null;
  value: unknown;
}

export interface HistorianDetail {
  schemaVersion: 'manyforge.historian_detail.v2';
  sequence: number;
  event: HistorianEvent;
  sections: Record<HistorianDetailSectionName, HistorianDetailSection>;
}
