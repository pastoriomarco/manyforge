import { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react';

import { api } from '../api/client';
import {
  ioSignalBindingsFromProgramBinding,
  type IoSignalBinding,
  type IoSignalSelector,
  type MockIoInputReceipt,
  type SystemSignalProjection,
} from '../api/parsers/ioSignals';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import {
  ageText,
  codedErrorText,
  SIGNAL_SOURCE_LABELS,
  signalValueText,
} from '../state/componentPresentation';
import type { JsonValue } from '../types/api';
import {
  CHANGED_OUTSIDE_SUFFIX,
  ioSignalId,
  type CellState,
  type ComponentState,
  type SetOutputResult,
  type SignalSample,
} from '../types/components';
import {
  globalRuleStateText,
  globalRuleUnavailableText,
  type GlobalRuleStatus,
} from '../utils/globalRuleStatus';
import {
  IO_DRIVER_FRESHNESS_NOTE,
  IO_LIVENESS_LABELS,
  IO_LIVENESS_UNPROVEN_WARNING,
  IO_SOURCE_PROFILE_LABELS,
  declaredIoSources,
  ioSourceKey,
  type DeclaredIoSource,
} from '../utils/ioSignalConfiguration';

/** One bound signal, addressed by its own Program binding. */
export interface ToolIoSignal {
  programBindingId: string;
  controllableComponentId: string;
  targetId: string;
  targetRoleId: string;
  resourceRoleId: string;
  signalName: string;
  direction: 'input' | 'output';
  declared: DeclaredIoSource | null;
}

/** One manual output write: sent once; its outcome is shown, never resent. */
export interface OutputWriteState {
  value: boolean;
  sending: boolean;
  result: SetOutputResult | null;
  /** A refusal or transport failure of the request itself. */
  error: string | null;
}

/** One Mock input toggle: the model receipt or its failure. */
export interface MockInputState {
  sending: boolean;
  receipt: MockIoInputReceipt | null;
  error: string | null;
}

/** What a Mock toggle did: model set, and whether that value reached the source. */
function receiptText(receipt: MockIoInputReceipt): { text: string; warning: boolean } {
  const model = `Model set to ${String(receipt.modelValue)}`;
  if (receipt.publicationError !== null) {
    return {
      warning: true,
      text: `${model}, but delivery to the source failed (${receipt.publicationError.code}: ${receipt.publicationError.message}).`,
    };
  }
  if (receipt.publishedCursor !== null) {
    return {
      warning: false,
      text: `${model}; delivered as record ${receipt.publishedCursor.recordSequence} of ${receipt.publishedCursor.sourceId}.`,
    };
  }
  return {
    warning: false,
    text: `${model}; not delivered now: the source is not publishing (e.g. Disabled). It is delivered when the source is enabled.`,
  };
}

/** The outcome line of one manual output write (CONTRACTS §7). */
function writeText(write: OutputWriteState): { text: string; severity: 'info' | 'warning' | 'error' } | null {
  const value = write.value ? 'on' : 'off';
  if (write.sending) return { text: `Writing ${value}…`, severity: 'info' };
  if (write.error !== null) return { text: write.error, severity: 'error' };
  if (write.result === null) return null;
  if (write.result.outcome === 'succeeded') return { text: `Written: ${value}.`, severity: 'info' };
  if (write.result.outcome === 'unknown') {
    return { text: `Write ${value}: no response, the outcome is unknown. It is not resent.`, severity: 'warning' };
  }
  return {
    text: write.result.error === undefined ? `Write ${value} failed.` : `Write ${value} failed: ${codedErrorText(write.result.error)}`,
    severity: 'error',
  };
}

/**
 * The one adapter from `targets.io_signal_bindings` rows to panel signals.
 * A signal bound under several target roles or bindings is listed once
 * (first row in canonical order); its declared source is joined when known.
 */
export function toolIoSignals(
  bindings: IoSignalBinding[],
  declared: Map<string, DeclaredIoSource> | null,
): ToolIoSignal[] {
  const seen = new Set<string>();
  return bindings.flatMap((target) => target.signalRoleBindings.flatMap((row) => {
    const identity = JSON.stringify([target.controllableComponentId, row.signalName]);
    if (seen.has(identity)) return [];
    seen.add(identity);
    return [{
      programBindingId: target.programBindingId,
      controllableComponentId: target.controllableComponentId,
      targetId: target.commandTargetId,
      targetRoleId: target.targetRoleId,
      resourceRoleId: row.resourceRoleId,
      signalName: row.signalName,
      direction: row.direction,
      declared: declared?.get(ioSourceKey(
        target.controllableComponentId, target.commandTargetId, row.resourceRoleId,
      )) ?? null,
    }];
  }));
}

export function toolIoSignalKey(
  signal: Pick<ToolIoSignal, 'programBindingId' | 'targetRoleId' | 'resourceRoleId'>,
): string {
  return JSON.stringify([signal.programBindingId, signal.targetRoleId, signal.resourceRoleId]);
}

function message(error: unknown): string {
  return error instanceof Error ? error.message : 'The request failed.';
}

function declaredSourceText(declared: DeclaredIoSource): string {
  const topic = declared.topic === null ? '' : ` · ${declared.topic}`;
  return `${IO_SOURCE_PROFILE_LABELS[declared.profile]} · ${declared.sourceId}${topic}`;
}

export interface ToolIoPanelViewProps {
  unavailableReason: string | null;
  signals: ToolIoSignal[];
  /** Generated owner signals, displayed from the target projection without requests. */
  systemSignals?: SystemSignalProjection[];
  /** The pushed samples by `signal_id`; null until the first `io_signals` event. */
  samples: ReadonlyMap<string, SignalSample> | null;
  /** The cell latch blocks manual I/O (CONTRACTS §5). */
  latched: boolean;
  /** Motion is executing in the cell: manual output writes wait (§7). */
  motionExecuting?: boolean;
  writes: Readonly<Record<string, OutputWriteState>>;
  mocks: Readonly<Record<string, MockInputState>>;
  /** Null when the declared sources can be shown; else why not (Draft differs from Applied). */
  sourcesNotice: string | null;
  onSetOutput: (signal: ToolIoSignal, value: boolean) => void;
  onSetMock: (signal: ToolIoSignal, value: boolean) => void;
  /**
   * The live global rule states (control-state shell); null while unknown,
   * absent when no control connection carries them.
   */
  globalRules?: readonly GlobalRuleStatus[] | null;
  /**
   * False while the control stream is delayed, stale or semantically not
   * current: the rows are then last known only, never shown as live.
   */
  globalRulesCurrent?: boolean;
}

/** Each global rule's live state (CONTRACTS §8: inactive, clear, tripped, or can't watch). */
export function GlobalRuleStatusSection({ rules, current = true }: {
  rules: readonly GlobalRuleStatus[] | null;
  current?: boolean;
}) {
  if (rules !== null && rules.length === 0) return null;
  return (
    <section className="runtime-control-section" aria-label="Global rules">
      <div className="runtime-control-heading"><strong>Global rules</strong></div>
      {rules !== null && !current
        && <p className="hint">Control state is not current; showing the last known rule states.</p>}
      {rules === null ? <p className="hint">Global rule states are not known yet.</p> : rules.map((rule) => {
        const unavailableText = current ? globalRuleUnavailableText(rule) : null;
        return (
          <div className="runtime-control-heading" key={rule.ruleId} data-global-rule={rule.ruleId}
            data-global-rule-state={rule.state} data-global-rule-current={String(current)}>
            <span>{rule.ruleId}</span>
            <div>
              <span className={`status-pill${!current ? ''
                : rule.state === 'clear' ? ' status-pill-connected'
                  : rule.state === 'tripped' || rule.state === 'unknown' ? ' status-pill-danger' : ''}`}>
                {current ? globalRuleStateText(rule) : `last known: ${globalRuleStateText(rule)} (not current)`}
              </span>
              {unavailableText !== null && <>
                <p className="hint">{unavailableText}</p>
                <details>
                  <summary>Details</summary>
                  <pre>{JSON.stringify(rule.unavailableReason?.details, null, 2)}</pre>
                </details>
              </>}
            </div>
          </div>
        );
      })}
    </section>
  );
}

/** The pure Tool & I/O view: one row per bound signal, grouped by component. */
export function ToolIoPanelView(props: ToolIoPanelViewProps) {
  const { globalRules } = props;
  return (
    <>
      {globalRules !== undefined
        && <GlobalRuleStatusSection rules={globalRules} current={props.globalRulesCurrent ?? true} />}
      <ToolIoSignalsView {...props} />
    </>
  );
}

const LATCH_REASON = codedErrorText({ code: 'cell_latched', message: 'The cell is latched.' });
const EXECUTING_REASON = codedErrorText({
  code: 'io_manual_write_motion_executing', message: 'Motion is executing.',
});

/**
 * True while motion executes in the cell: an arm or gripper component is
 * busy (§1, §7). An I/O component's own write, a sensor or an owner doesn't
 * count.
 */
export function motionExecuting(components: readonly ComponentState[] | null,
  motionComponentIds: ReadonlySet<string>): boolean {
  return components?.some((component) => component.activity === 'busy'
    && motionComponentIds.has(component.component_id)) ?? false;
}

/** Value, source and age of one pushed sample. */
function SampleFacts({ sample, known }: { sample: SignalSample | undefined; known: boolean }) {
  if (sample === undefined) {
    return (
      <div className="deployment-resolved-grid">
        <span><small>Value</small><strong data-io-value>—</strong></span>
        <span><small>Source</small><strong>{known ? 'no sample' : 'unavailable'}</strong></span>
      </div>
    );
  }
  return (
    <div className="deployment-resolved-grid">
      <span><small>Value</small><strong data-io-value>
        {signalValueText(sample)}{sample.fresh ? '' : ' (stale)'}
      </strong></span>
      <span><small>Source</small><strong data-io-source={sample.source}>
        {sample.source === 'outside'
          ? <span className="status-pill status-pill-danger">{SIGNAL_SOURCE_LABELS.outside}</span>
          : SIGNAL_SOURCE_LABELS[sample.source]}
      </strong></span>
      <span><small>Age</small><strong data-io-age>{ageText(sample.age_ns)}</strong></span>
    </div>
  );
}

function ToolIoSignalsView({
  unavailableReason,
  signals,
  systemSignals = [],
  samples,
  latched,
  motionExecuting: writesWait = false,
  writes,
  mocks,
  sourcesNotice,
  onSetOutput,
  onSetMock,
}: ToolIoPanelViewProps) {
  const outputsBlocked = latched ? LATCH_REASON : writesWait ? EXECUTING_REASON : null;
  if (unavailableReason !== null) {
    return <p className="panel-state">{unavailableReason}</p>;
  }
  if (signals.length === 0 && systemSignals.length === 0) {
    return <p className="panel-state">The running cell has no bound I/O signals.</p>;
  }
  const components = [...new Set(signals.map((signal) => signal.controllableComponentId))];
  const systemComponents = [...new Set(systemSignals.map((signal) => signal.controllableComponentId))];
  return (
    <div className="dpanel" aria-label="Tool and I/O signals">
      {sourcesNotice !== null && <p className="hint">{sourcesNotice}</p>}
      {outputsBlocked !== null && <p className="inline-message severity-warning" role="status">Outputs: {outputsBlocked}</p>}
      {components.map((controllableComponentId) => (
        <section className="runtime-control-section" key={controllableComponentId} aria-label={`${controllableComponentId} signals`}>
          <div className="runtime-control-heading"><strong>{controllableComponentId}</strong></div>
          {signals.filter((signal) => signal.controllableComponentId === controllableComponentId).map((signal) => {
            const key = toolIoSignalKey(signal);
            const id = ioSignalId(signal.controllableComponentId, signal.signalName);
            const sample = samples?.get(id);
            const changedOutside = samples?.get(`${id}${CHANGED_OUTSIDE_SUFFIX}`);
            const output = signal.direction === 'output';
            const liveness = output ? null : signal.declared?.liveness ?? null;
            return (
              <div className="runtime-control-record" key={key} data-io-signal={signal.signalName}
                data-io-fresh={sample === undefined ? 'pending' : String(sample.fresh)}>
                <div className="runtime-control-heading">
                  <strong>{signal.signalName}</strong>
                  <small>{signal.targetId} · {signal.resourceRoleId} · {output ? 'output' : 'input'}</small>
                </div>
                <SampleFacts sample={sample} known={samples !== null} />
                {signal.declared !== null && (
                  <p className="hint">Declared source: {declaredSourceText(signal.declared)}
                    {liveness === null ? '' : ` · ${IO_LIVENESS_LABELS[liveness]}`}</p>
                )}
                {output && changedOutside !== undefined && (
                  <div className="runtime-control-record" data-io-changed-outside={String(changedOutside.value === true)}>
                    <div className="runtime-control-heading">
                      <small>→ changed outside</small>
                      {changedOutside.value === true
                        ? <span className="status-pill status-pill-danger">changed outside ManyForge</span>
                        : <span className="status-pill">no outside change</span>}
                    </div>
                  </div>
                )}
                {liveness === 'change_only' && (
                  <p className="inline-message severity-warning" role="note">{IO_LIVENESS_UNPROVEN_WARNING}</p>
                )}
                {!output && signal.declared?.profile === 'ros2_control_gpio' && (
                  <p className="hint">{IO_DRIVER_FRESHNESS_NOTE}</p>
                )}
                {output
                  ? <OutputControls signal={signal} sample={sample} blockedReason={outputsBlocked}
                      write={writes[key]} onSetOutput={onSetOutput} />
                  : signal.declared?.profile === 'mock'
                    ? <MockInputControls signal={signal} sample={sample} mock={mocks[key]} onSetMock={onSetMock} />
                    : null}
              </div>
            );
          })}
        </section>
      ))}
      {systemSignals.length > 0 && (
        <section className="runtime-control-section" aria-label="System signals">
          <div className="runtime-control-heading">
            <strong>System signals</strong><small>Read only</small>
          </div>
          {systemComponents.map((controllableComponentId) => (
            <section className="runtime-control-section" key={controllableComponentId}
              aria-label={`${controllableComponentId} system signals`}>
              <div className="runtime-control-heading"><strong>{controllableComponentId}</strong></div>
              {systemSignals.filter((signal) => signal.controllableComponentId === controllableComponentId).map((signal) => (
                <div className="runtime-control-record" key={`${signal.commandTargetId}\u0000${signal.signalName}`}
                  data-system-signal={signal.signalName} data-system-source-kind={signal.sourceKind}>
                  <div className="runtime-control-heading">
                    <strong>{signal.signalName}</strong>
                    <small>{signal.commandTargetId} · input · system</small>
                  </div>
                  <div className="deployment-resolved-grid">
                    <span><small>Level</small><strong data-system-signal-level>{String(signal.level)}</strong></span>
                    <span><small>Type</small><strong>bool</strong></span>
                    <span><small>Source</small><strong>Carried object</strong></span>
                  </div>
                </div>
              ))}
            </section>
          ))}
        </section>
      )}
      {signals.length > 0 && (
        <p className="hint">Outputs can be written whenever no motion is executing; a write is sent once and never resent (the last write wins). Setting a Mock input simulates its source; it is not a hardware command.</p>
      )}
    </div>
  );
}

/** Manual write of one output (CONTRACTS §7); controller-owned outputs are read-only. */
function OutputControls({ signal, sample, blockedReason, write, onSetOutput }: {
  signal: ToolIoSignal;
  sample: SignalSample | undefined;
  /** Why outputs can't be written now (latch, executing run), or null. */
  blockedReason: string | null;
  write: OutputWriteState | undefined;
  onSetOutput: (signal: ToolIoSignal, value: boolean) => void;
}) {
  if (sample?.controller_owned === true) {
    return <p className="hint" data-io-controller-owned>Read only: the robot controller owns this output (its function is set on the controller).</p>;
  }
  const disabledReason = blockedReason ?? (write?.sending === true ? 'A write is in flight.' : null);
  const outcome = write === undefined ? null : writeText(write);
  return (
    <>
      {outcome !== null && (
        <p className={`inline-message severity-${outcome.severity}`}
          role={outcome.severity === 'error' ? 'alert' : 'status'}
          data-io-write={write?.sending ? 'sending' : write?.error !== null ? 'refused' : write?.result?.outcome}>
          {outcome.text}
        </p>
      )}
      <div className="runtime-control-actions">
        {[true, false].map((value) => (
          <button type="button" key={String(value)} disabled={disabledReason !== null}
            title={disabledReason ?? undefined}
            aria-label={`Write ${signal.signalName} ${value ? 'on' : 'off'}`}
            onClick={() => onSetOutput(signal, value)}>
            Set {value ? 'on' : 'off'}
          </button>
        ))}
      </div>
    </>
  );
}

function MockInputControls({ signal, sample, mock, onSetMock }: {
  signal: ToolIoSignal;
  sample: SignalSample | undefined;
  mock: MockInputState | undefined;
  onSetMock: (signal: ToolIoSignal, value: boolean) => void;
}) {
  const requested = sample?.value !== true;
  const receipt = mock?.receipt ?? null;
  return (
    <>
      {receipt !== null && (() => {
        const outcome = receiptText(receipt);
        return (
          <p className={`inline-message ${outcome.warning ? 'severity-warning' : 'severity-info'}`}
            role="status" data-io-receipt={outcome.warning ? 'publication_error'
              : receipt.publishedCursor === null ? 'not_delivered' : 'delivered'}>{outcome.text}</p>
        );
      })()}
      {mock?.error != null && <p className="inline-message severity-error" role="alert">{mock.error}</p>}
      <div className="runtime-control-actions">
        <button type="button" disabled={mock?.sending === true}
          title={mock?.sending === true ? 'Setting the Mock input…' : undefined}
          aria-label={`Set ${signal.signalName} ${requested ? 'true' : 'false'}`}
          onClick={() => onSetMock(signal, requested)}>
          Set {requested ? 'true' : 'false'}
        </button>
      </div>
    </>
  );
}

function selectorOf(ownerEpoch: string, signal: ToolIoSignal): IoSignalSelector {
  return {
    ownerEpoch,
    programBindingId: signal.programBindingId,
    targetRoleId: signal.targetRoleId,
    resourceRoleId: signal.resourceRoleId,
  };
}

interface DeploymentIoFacts {
  /** Declared sources, only while the Deployment Draft equals Applied. */
  sources: Map<string, DeclaredIoSource> | null;
  notice: string | null;
  /** Interim signal rows from the workspace binding of the running cell. */
  bindings: IoSignalBinding[] | null;
}

/** One read of the Deployment workspace per running activation. */
function useDeploymentIoFacts(enabled: boolean, retainedBundleId: string | null): DeploymentIoFacts {
  const [facts, setFacts] = useState<DeploymentIoFacts>({ sources: null, notice: null, bindings: null });
  useEffect(() => {
    setFacts({ sources: null, notice: null, bindings: null });
    if (!enabled || retainedBundleId === null) return undefined;
    const abort = new AbortController();
    api.deploymentWorkspace.get(abort.signal).then((workspace) => {
      const draft = workspace.draft;
      const applied = workspace.managerApplied.deploymentRevision;
      const current = draft.deploymentRevision !== null && draft.deploymentRevision === applied;
      const binding = workspace.binding;
      setFacts({
        sources: current ? declaredIoSources(draft.document as Record<string, JsonValue> | null) : null,
        notice: current
          ? null
          : 'The Deployment Draft differs from Applied, so declared sources are not shown and Mock inputs cannot be set here. Apply or discard the Deployment edits.',
        bindings: binding !== null && binding.retained_bundle_id === retainedBundleId
          ? ioSignalBindingsFromProgramBinding(binding)
          : null,
      });
    }).catch((error: unknown) => {
      if (!abort.signal.aborted) {
        setFacts({ sources: null, notice: `Declared sources are unavailable: ${message(error)}`, bindings: null });
      }
    });
    return () => abort.abort();
  }, [enabled, retainedBundleId]);
  return facts;
}

export interface ToolIoPanelProps {
  enabled: boolean;
  /** The execution owner epoch the host fences Mock input writes by. */
  ownerEpoch: string | null;
  /** The running cell's retained bundle. */
  retainedBundleId: string | null;
  /**
   * The owner's `targets.io_signal_bindings`; absent when the owner does not
   * publish it, then the running cell's workspace binding supplies the rows.
   */
  ioSignalBindings?: IoSignalBinding[];
  /** The owner's generated read-only system signal projection. */
  systemSignals?: SystemSignalProjection[];
  /** The control-state source carrying the I/O samples, the latch and the rule states. */
  controlSource?: HeadlessStateSource | null;
  /** The arm and gripper components (their direct-control targets' owners). */
  motionComponentIds?: ReadonlySet<string>;
}

const subscribeNothing = () => () => undefined;
const unknownRules = () => null;
const rulesNotCurrent = () => false;
const NO_CELL_STATE: CellState = { components: null, latch: null, io: null, readiness: null };
const noCellState = () => NO_CELL_STATE;
const NO_MOTION_COMPONENTS: ReadonlySet<string> = new Set();

/**
 * Tool & I/O: every bound signal's pushed sample (value, source, age), the
 * `changed outside` mark of eligible outputs, manual output writes and Mock
 * input toggles. Nothing polls (CONTRACTS §7).
 */
export function ToolIoPanel({
  enabled,
  ownerEpoch,
  retainedBundleId,
  ioSignalBindings,
  systemSignals: ownerSystemSignals,
  controlSource,
  motionComponentIds = NO_MOTION_COMPONENTS,
}: ToolIoPanelProps) {
  const globalRules = useSyncExternalStore(
    controlSource?.subscribeGlobalRules ?? subscribeNothing,
    controlSource?.getGlobalRules ?? unknownRules,
    unknownRules,
  );
  const globalRulesCurrent = useSyncExternalStore(
    controlSource?.subscribeGlobalRules ?? subscribeNothing,
    controlSource?.getGlobalRulesCurrent ?? rulesNotCurrent,
    rulesNotCurrent,
  );
  const cell = useSyncExternalStore(
    controlSource?.subscribeCellState ?? subscribeNothing,
    controlSource?.getCellState ?? noCellState,
    noCellState,
  );
  const samples = useMemo(() => (cell.io === null ? null
    : new Map(cell.io.map((sample) => [sample.signal_id, sample]))), [cell.io]);
  const facts = useDeploymentIoFacts(enabled && ownerEpoch !== null, retainedBundleId);
  const bindings = ioSignalBindings ?? facts.bindings;
  const systemSignals = enabled && ownerEpoch !== null && retainedBundleId !== null
    ? ownerSystemSignals ?? []
    : [];
  const unavailableReason = !enabled
    ? 'Tool and I/O signals require the configured headless runtime.'
    : ownerEpoch === null || retainedBundleId === null
      ? 'Waiting for the execution owner.'
      : bindings === null && systemSignals.length === 0
        ? 'No applied Program binding of the running cell names I/O signals yet.'
        : null;
  const signals = useMemo(() => toolIoSignals(bindings ?? [], facts.sources), [bindings, facts.sources]);
  const [writes, setWrites] = useState<Record<string, OutputWriteState>>({});
  const [mocks, setMocks] = useState<Record<string, MockInputState>>({});

  const setOutput = useCallback((signal: ToolIoSignal, value: boolean) => {
    const key = toolIoSignalKey(signal);
    setWrites((current) => ({ ...current, [key]: { value, sending: true, result: null, error: null } }));
    // One request; an unknown outcome is shown, never resent.
    api.io.setOutput(ioSignalId(signal.controllableComponentId, signal.signalName), value)
      .then((result) => setWrites((current) => ({ ...current, [key]: { value, sending: false, result, error: null } })))
      .catch((error: unknown) => setWrites((current) => ({
        ...current, [key]: { value, sending: false, result: null, error: message(error) },
      })));
  }, []);

  const setMock = useCallback((signal: ToolIoSignal, value: boolean) => {
    if (ownerEpoch === null) return;
    const key = toolIoSignalKey(signal);
    const selector = selectorOf(ownerEpoch, signal);
    setMocks((current) => ({ ...current, [key]: { sending: true, receipt: current[key]?.receipt ?? null, error: null } }));
    // The Mock source fences its toggle by its current source epoch, read
    // just before; one explicit toggle, never replayed.
    api.execution.readIoSignal(selector)
      .then((reading) => {
        if (reading.sourceEpoch === null) throw new Error('The Mock source has not reported yet.');
        return api.execution.setMockIoInput(selector, reading.sourceEpoch, value);
      })
      .then((receipt) => setMocks((current) => ({ ...current, [key]: { sending: false, receipt, error: null } })))
      .catch((error: unknown) => setMocks((current) => ({
        ...current, [key]: { sending: false, receipt: current[key]?.receipt ?? null, error: message(error) },
      })));
  }, [ownerEpoch]);

  return (
    <ToolIoPanelView
      unavailableReason={unavailableReason}
      signals={signals}
      systemSignals={systemSignals}
      samples={samples}
      latched={cell.latch?.latched === true}
      motionExecuting={motionExecuting(cell.components, motionComponentIds)}
      writes={writes}
      mocks={mocks}
      sourcesNotice={facts.notice}
      onSetOutput={setOutput}
      onSetMock={setMock}
      globalRules={controlSource == null ? undefined : globalRules}
      globalRulesCurrent={globalRulesCurrent}
    />
  );
}
