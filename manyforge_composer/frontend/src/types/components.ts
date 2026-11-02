// Component model v2 wire types (CONTRACTS §1, §2, §4, §5, §7, §9, §10, §11).
// Field names are the contract's, unchanged; the backend owns every value and
// the GUI only presents it (R4).

/** A typed error as it crosses every layer: a catalogue code and a message. */
export interface CodedError {
  code: string;
  message: string;
}

export const COMPONENT_LIFECYCLES = ['offline', 'not_prepared', 'prepared', 'enabled', 'fault'] as const;
export type ComponentLifecycle = typeof COMPONENT_LIFECYCLES[number];

export const COMPONENT_ACTIVITIES = ['idle', 'busy', 'unknown'] as const;
export type ComponentActivity = typeof COMPONENT_ACTIVITIES[number];

export const STREAM_FRESHNESS = ['fresh', 'stale', 'none'] as const;
export type StreamFreshness = typeof STREAM_FRESHNESS[number];

/** §3: the capabilities an adapter may declare. */
export const COMPONENT_CAPABILITIES = [
  'reset_position', 'power_off', 'reboot', 'io_manual_write', 'io_output_readback',
  // §14: simulator reset (Enable plays, Disable pauses).
  'sim_reset',
] as const;
export type ComponentCapability = string;

/** §2: the four lifecycle actions, offered on every component (Prepare not while enabled). */
export const LIFECYCLE_ACTIONS = ['prepare', 'enable', 'disable', 'recover'] as const;
export type ComponentAction = string;

export const FAULT_OPERATOR_ACTIONS = ['enable', 'recover', 'none'] as const;
export type FaultOperatorAction = typeof FAULT_OPERATOR_ACTIONS[number];

/** §4. */
export interface Fault {
  code: string;
  message: string;
  clearable: boolean;
  operator_action: FaultOperatorAction;
  vendor_code?: string;
  vendor_text?: string;
  /** §16: on an owner's fault, the members it concerns. */
  concerns?: string[];
  since_ns: number;
}

export const ACTION_OUTCOMES = ['succeeded', 'failed', 'unknown'] as const;
export type ActionOutcome = typeof ACTION_OUTCOMES[number];

export interface ActionError extends CodedError {
  vendor_code?: string;
  vendor_text?: string;
}

/** §2. */
export interface ActionResult {
  action: ComponentAction;
  outcome: ActionOutcome;
  error?: ActionError;
  started_at_ns: number;
  finished_at_ns: number;
}

export interface ComponentStream {
  stream_id: string;
  freshness: StreamFreshness;
  age_ns?: number;
}

/** §1: the one view of a component. */
export interface ComponentState {
  component_id: string;
  /** §16: the machine (owner component) this component is a member of. */
  owner_id?: string;
  /** §16: its member slot (arm, controller_io, tool_io, ...). */
  role?: string;
  lifecycle: ComponentLifecycle;
  fault?: Fault;
  reason?: CodedError;
  activity: ComponentActivity;
  streams: ComponentStream[];
  capabilities: ComponentCapability[];
  action?: { action: ComponentAction; started_at_ns: number };
  last_action?: ActionResult;
  /** §1: the adapter's read-only status lines (e.g. xArm mode, state, error codes); display only. */
  details?: ComponentDetail[];
  observed_at_ns: number;
}

/** §1: one read-only status line an adapter reports. */
export interface ComponentDetail {
  name: string;
  value: string;
}

/** §1, §16: a member's reason while its owner isn't usable. */
export const OWNER_UNAVAILABLE = 'owner_unavailable';
/** §2, §16: a member action refused while its owner isn't prepared. */
export const OWNER_NOT_PREPARED = 'owner_not_prepared';

export const LATCH_CAUSE_KINDS = ['fault', 'offline', 'activity_unknown'] as const;
export type LatchCauseKind = typeof LATCH_CAUSE_KINDS[number];

/** §5. */
export interface CellLatch {
  latched: boolean;
  causes: { component_id: string; kind: LatchCauseKind; fault?: Fault }[];
}

export const SIGNAL_SOURCES = ['device', 'manyforge_write', 'outside', 'last_write'] as const;
export type SignalSource = typeof SIGNAL_SOURCES[number];

/**
 * §7. `controller_owned` is the per-output flag of §7 ("Per output"); it is
 * present on outputs only.
 */
export interface SignalSample {
  signal_id: string;
  value: boolean | number | null;
  source: SignalSource;
  age_ns: number;
  fresh: boolean;
  controller_owned?: boolean;
}

/** §7. */
export interface SetOutputResult {
  outcome: ActionOutcome;
  error?: CodedError;
}

/** §7: the core-held signal derived for every eligible output. */
export const CHANGED_OUTSIDE_SUFFIX = '.changed_outside';

/**
 * The wire `signal_id` of a Deployment signal (core `IoSignalId`
 * {controllable_component_id, name}). The only place the GUI spells it.
 */
export function ioSignalId(controllableComponentId: string, signalName: string): string {
  return `${controllableComponentId}/${signalName}`;
}

/** §10. */
export interface RunBlockedReason extends CodedError {
  component_id?: string;
  rule_id?: string;
  signal_id?: string;
  /** `rule_holds`: the hold's scope (§8): everything, all_motion or execution_groups. */
  scope?: string;
  /** `rule_holds` with scope execution_groups: the components it holds. */
  component_ids?: string[];
}

export interface RunReadiness {
  can_execute: boolean;
  can_resume: boolean;
  /** Motion may start or continue (a group's Run/Resume): no blocking reason, whatever the run state. */
  motion_clear: boolean;
  blocked_reasons: RunBlockedReason[];
}

export const RUN_END_REASONS = ['completed', 'failed', 'stopped', 'cell_fault'] as const;
export type RunEndReason = typeof RUN_END_REASONS[number];

/** §9. */
export interface ProgramRunEnd {
  end_reason: RunEndReason;
  details: {
    node_instance_id?: string;
    error?: CodedError;
    stopped_by?: 'operator' | 'rule';
    rule_id?: string;
    component_id?: string;
  };
}

export const APPLY_DISPOSITIONS = ['add', 'remove', 'replace', 'retain'] as const;
export type ApplyDisposition = typeof APPLY_DISPOSITIONS[number];

/** §11: a Program role or target the previewed Deployment no longer resolves. */
export interface ApplyUnresolvedReference {
  component_role_id: string;
  target_role_id: string | null;
  controllable_component_id: string;
  command_target_id: string | null;
  reason: string;
  detail: string;
}

/** §11: `POST /api/deployment/preview` (and the Revert preview). */
export interface ApplyPreview {
  components: {
    component_id: string;
    disposition: ApplyDisposition;
    reasons: string[];
    restarts: boolean;
  }[];
  /** What the Preview was computed from; sent back verbatim with its Apply. */
  scope: Record<string, unknown>;
  /** Whether the Program stays executable after this Apply. */
  program: {
    executable: boolean;
    program_id: string | null;
    unresolved_references: ApplyUnresolvedReference[];
    affected_nodes: string[];
    affected_execution_groups: string[];
  };
  attachment_changed_components: string[];
  /** Kept in the cell, detached at their authored position. */
  detached_components: string[];
}

/** §11: Apply, Load and New. */
export interface ApplyResult {
  outcome: 'succeeded' | 'failed';
  components: {
    component_id: string;
    disposition: ApplyDisposition;
    /** Absent for a removed component. */
    lifecycle_after?: ComponentLifecycle;
    /** This component's failed step; the Apply continued with the others. */
    error?: CodedError;
  }[];
  /** The first failure. */
  error?: CodedError;
}

/** §11: one Deployment field's effective value. */
export interface EffectiveField {
  value: unknown;
  default: unknown;
  set: boolean;
}

export interface EffectiveValues {
  fields: Record<string, EffectiveField>;
}

/** §8. */
export const RULE_ON_TRIP = ['stop', 'notify'] as const;
export type RuleOnTrip = typeof RULE_ON_TRIP[number];
export const RULE_AFTER_STOP = ['hold', 'fail_node'] as const;
export type RuleAfterStop = typeof RULE_AFTER_STOP[number];
export const RULE_STATES = ['inactive', 'clear', 'tripped'] as const;
export type RuleState = typeof RULE_STATES[number];

/** The pushed cell facts of `/ws/control` (§1, §5, §7, §10); null: not read yet. */
export interface CellState {
  components: readonly ComponentState[] | null;
  latch: CellLatch | null;
  io: readonly SignalSample[] | null;
  readiness: RunReadiness | null;
}
