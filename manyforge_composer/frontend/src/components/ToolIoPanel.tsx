import { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react';

import { api } from '../api/client';
import {
  ioSignalBindingsFromProgramBinding,
  type IoSignalBinding,
  type IoSignalReading,
  type IoSignalSelector,
  type MockIoInputReceipt,
} from '../api/parsers/ioSignals';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import type { JsonValue } from '../types/api';
import { globalRuleStateText, type GlobalRuleStatus } from '../utils/globalRuleStatus';
import {
  IO_DRIVER_FRESHNESS_NOTE,
  IO_LIVENESS_LABELS,
  IO_LIVENESS_UNPROVEN_WARNING,
  IO_SOURCE_PROFILE_LABELS,
  declaredIoSources,
  ioSourceKey,
  type DeclaredIoSource,
  type IoLivenessMode,
} from '../utils/ioSignalConfiguration';

/** Display cadence while the panel is open; nothing polls when it is closed. */
export const IO_PANEL_POLL_INTERVAL_MS = 500;

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

export interface ToolIoSignalState {
  reading: IoSignalReading | null;
  error: string | null;
  sending: boolean;
  /** The last Mock toggle receipt: model accepted vs delivered observation. */
  receipt?: MockIoInputReceipt | null;
  /**
   * The latest reported source epoch: an unavailable (e.g. Disabled) Mock
   * reading carries none, yet its model may still be set; the host fences a
   * stale epoch.
   */
  lastSourceEpoch?: string | null;
}

const EMPTY_STATE: ToolIoSignalState = { reading: null, error: null, sending: false };

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

function ageText(ns: number | null): string {
  if (ns === null) return '—';
  return ns < 1_000_000_000 ? `${Math.round(ns / 1_000_000)} ms` : `${(ns / 1_000_000_000).toFixed(1)} s`;
}

/**
 * The input's liveness: the owner's descriptor projection (SX-2194) when read,
 * else the declared source's mode; null when neither is known.
 */
function livenessOf(signal: ToolIoSignal, reading: IoSignalReading | null): IoLivenessMode | null {
  return reading?.liveness?.mode ?? signal.declared?.liveness ?? null;
}

function sourceText(signal: ToolIoSignal, reading: IoSignalReading | null): string {
  const id = reading?.sourceId ?? signal.declared?.sourceId ?? 'unknown source';
  if (signal.declared === null) return id;
  const topic = signal.declared.topic === null ? '' : ` · ${signal.declared.topic}`;
  return `${IO_SOURCE_PROFILE_LABELS[signal.declared.profile]} · ${id}${topic}`;
}

export interface ToolIoPanelViewProps {
  unavailableReason: string | null;
  signals: ToolIoSignal[];
  states: Record<string, ToolIoSignalState>;
  /** Null when the declared sources cannot be shown (Draft differs from Applied). */
  sourcesNotice: string | null;
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

/** Each global rule's live state; latched rules keep their recovery notice. */
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
      {rules === null ? <p className="hint">Global rule states are not known yet.</p> : rules.map((rule) => (
        <div className="runtime-control-heading" key={rule.ruleId} data-global-rule={rule.ruleId}
          data-global-rule-state={rule.state} data-global-rule-current={String(current)}>
          <span>{rule.ruleId}</span>
          <span className={`status-pill${!current ? ''
            : rule.state === 'clear' ? ' status-pill-connected'
              : rule.state === 'tripped' || rule.state === 'latched' ? ' status-pill-danger' : ''}`}>
            {current ? globalRuleStateText(rule) : `last known: ${globalRuleStateText(rule)} (not current)`}
          </span>
        </div>
      ))}
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

function ToolIoSignalsView({
  unavailableReason,
  signals,
  states,
  sourcesNotice,
  onSetMock,
}: ToolIoPanelViewProps) {
  if (unavailableReason !== null) {
    return <p className="panel-state">{unavailableReason}</p>;
  }
  if (signals.length === 0) {
    return <p className="panel-state">The running cell has no bound I/O signals.</p>;
  }
  const components = [...new Set(signals.map((signal) => signal.controllableComponentId))];
  return (
    <div className="dpanel" aria-label="Tool and I/O signals">
      {sourcesNotice !== null && <p className="hint">{sourcesNotice}</p>}
      {components.map((controllableComponentId) => (
        <section className="runtime-control-section" key={controllableComponentId} aria-label={`${controllableComponentId} signals`}>
          <div className="runtime-control-heading"><strong>{controllableComponentId}</strong></div>
          {signals.filter((signal) => signal.controllableComponentId === controllableComponentId).map((signal) => {
            const key = toolIoSignalKey(signal);
            const state = states[key] ?? EMPTY_STATE;
            const { reading } = state;
            const receipt = state.receipt ?? null;
            const output = signal.direction === 'output';
            const mock = signal.declared?.profile === 'mock';
            // A delivered reading is current; otherwise the accepted model value is.
            const current = reading?.quality === 'fresh'
              ? reading.value
              : receipt?.modelValue ?? reading?.value ?? null;
            const requested = current !== true;
            const sourceEpoch = reading?.sourceEpoch ?? state.lastSourceEpoch ?? null;
            const disabledReason = output
              ? 'Outputs are not implemented.'
              : !mock
                ? signal.declared === null
                  ? 'The declared source is unknown, so this input cannot be simulated.'
                  : 'Only Mock inputs can be set; this is a real source.'
                : sourceEpoch === null
                  ? 'Waiting for the Mock source reading.'
                  : state.sending ? 'Setting the Mock input…' : null;
            return (
              <div className="runtime-control-record" key={key} data-io-signal={signal.signalName}
                data-io-quality={reading?.quality ?? 'pending'}>
                <div className="runtime-control-heading">
                  <strong>{signal.signalName}</strong>
                  <small>{signal.targetId} · {signal.resourceRoleId} · {output ? 'output' : 'input'}</small>
                </div>
                {output ? (
                  <p className="hint">Output: not implemented; it is not read or written.</p>
                ) : (
                  <div className="deployment-resolved-grid">
                    <span><small>Value</small><strong data-io-value>
                      {reading === null || reading.value === null ? '—' : String(reading.value)}
                      {reading !== null && reading.value !== null && reading.quality !== 'fresh' ? ' (last reported)' : ''}
                    </strong></span>
                    <span><small>Quality</small><strong>
                      <span className={`status-pill${reading?.quality === 'fresh'
                        ? ' status-pill-connected'
                        : reading === null ? '' : ' status-pill-danger'}`}>{reading?.quality ?? 'reading…'}</span>
                    </strong></span>
                    <span><small>Receipt age</small><strong>{ageText(reading?.receiptAgeNs ?? null)}</strong></span>
                    <span><small>Source</small><strong>{sourceText(signal, reading)}</strong></span>
                    {(() => {
                      const liveness = livenessOf(signal, reading);
                      return liveness === null ? null : (
                        <span><small>Liveness</small><strong data-io-liveness={liveness}>
                          {liveness === 'change_only'
                            ? <span className="status-pill status-pill-danger">Liveness unproven</span>
                            : IO_LIVENESS_LABELS[liveness]}
                        </strong></span>
                      );
                    })()}
                  </div>
                )}
                {!output && livenessOf(signal, reading) === 'change_only' && (
                  <p className="inline-message severity-warning" role="note">{IO_LIVENESS_UNPROVEN_WARNING}</p>
                )}
                {!output && (signal.declared?.profile === 'ur_io_states' || signal.declared?.profile === 'ros2_control_gpio') && (
                  <p className="hint">{IO_DRIVER_FRESHNESS_NOTE}</p>
                )}
                {reading?.errorCode != null && <p className="hint">Source error: {reading.errorCode}</p>}
                {receipt !== null && (() => {
                  const outcome = receiptText(receipt);
                  return (
                    <p className={`inline-message ${outcome.warning ? 'severity-warning' : 'severity-info'}`}
                      role="status" data-io-receipt={outcome.warning ? 'publication_error'
                        : receipt.publishedCursor === null ? 'not_delivered' : 'delivered'}>{outcome.text}</p>
                  );
                })()}
                {state.error !== null && <p className="inline-message severity-error" role="alert">{state.error}</p>}
                {!output && (
                  <div className="runtime-control-actions">
                    <button type="button" disabled={disabledReason !== null} title={disabledReason ?? undefined}
                      aria-label={`Set ${signal.signalName} ${requested ? 'true' : 'false'}`}
                      onClick={() => onSetMock(signal, requested)}>
                      Set {requested ? 'true' : 'false'}
                    </button>
                  </div>
                )}
              </div>
            );
          })}
        </section>
      ))}
      <p className="hint">Setting a Mock input simulates its source; it is not a hardware command and is allowed at any time.</p>
    </div>
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
  /** The execution owner epoch the host fences reads by. */
  ownerEpoch: string | null;
  /** The running cell's retained bundle. */
  retainedBundleId: string | null;
  /**
   * The owner's `targets.io_signal_bindings`; absent when the owner does not
   * publish it, then the running cell's workspace binding supplies the rows.
   */
  ioSignalBindings?: IoSignalBinding[];
  pollIntervalMs?: number;
  /** The control-state source carrying the global rule states. */
  controlSource?: HeadlessStateSource | null;
}

const subscribeNothing = () => () => undefined;
const unknownRules = () => null;
const rulesNotCurrent = () => false;

/**
 * Tool & I/O: reads every bound input at a display cadence while mounted
 * (the inspector body is mounted only while this panel is open).
 */
export function ToolIoPanel({
  enabled,
  ownerEpoch,
  retainedBundleId,
  ioSignalBindings,
  pollIntervalMs = IO_PANEL_POLL_INTERVAL_MS,
  controlSource,
}: ToolIoPanelProps) {
  // Pushed with the control state: no request of its own.
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
  const facts = useDeploymentIoFacts(enabled && ownerEpoch !== null, retainedBundleId);
  const bindings = ioSignalBindings ?? facts.bindings;
  const unavailableReason = !enabled
    ? 'Tool and I/O signals require the configured headless runtime.'
    : ownerEpoch === null || retainedBundleId === null
      ? 'Waiting for the execution owner.'
      : bindings === null
        ? 'No applied Program binding of the running cell names I/O signals yet.'
        : null;
  const ready = unavailableReason === null;
  const signals = useMemo(() => toolIoSignals(bindings ?? [], facts.sources), [bindings, facts.sources]);
  // Polling follows the bound selectors only (by value), so re-created rows
  // or arriving source facts never restart it.
  const inputsKey = JSON.stringify(toolIoSignals(bindings ?? [], null)
    .filter((signal) => signal.direction === 'input'));
  const inputs = useMemo(() => JSON.parse(inputsKey) as ToolIoSignal[], [inputsKey]);
  const [states, setStates] = useState<Record<string, ToolIoSignalState>>({});
  const patch = useCallback((key: string, next: Partial<ToolIoSignalState>) => {
    setStates((current) => {
      const previous = current[key] ?? EMPTY_STATE;
      const lastSourceEpoch = next.reading?.sourceEpoch ?? previous.lastSourceEpoch ?? null;
      return { ...current, [key]: { ...previous, ...next, lastSourceEpoch } };
    });
  }, []);

  useEffect(() => {
    setStates({});
    if (!ready || ownerEpoch === null || inputs.length === 0) return undefined;
    const abort = new AbortController();
    let timer: ReturnType<typeof setTimeout> | null = null;
    const poll = async () => {
      if (typeof document === 'undefined' || document.visibilityState !== 'hidden') {
        await Promise.all(inputs.map(async (signal) => {
          try {
            const reading = await api.execution.readIoSignal(selectorOf(ownerEpoch, signal), abort.signal);
            if (!abort.signal.aborted) patch(toolIoSignalKey(signal), { reading, error: null });
          } catch (error) {
            if (!abort.signal.aborted) patch(toolIoSignalKey(signal), { error: message(error) });
          }
        }));
      }
      if (!abort.signal.aborted) timer = setTimeout(() => { void poll(); }, pollIntervalMs);
    };
    void poll();
    return () => {
      abort.abort();
      if (timer !== null) clearTimeout(timer);
    };
  }, [inputs, ownerEpoch, patch, pollIntervalMs, ready]);

  const setMock = useCallback((signal: ToolIoSignal, value: boolean) => {
    const key = toolIoSignalKey(signal);
    const sourceEpoch = states[key]?.reading?.sourceEpoch ?? states[key]?.lastSourceEpoch ?? null;
    if (ownerEpoch === null || sourceEpoch === null) return;
    patch(key, { sending: true, error: null });
    // One explicit request; an unknown outcome is shown and refreshed by the
    // next read, never replayed. An accepted model whose source is not
    // publishing (unavailable observation) is a receipt, not a failure.
    api.execution.setMockIoInput(selectorOf(ownerEpoch, signal), sourceEpoch, value)
      .then((receipt) => patch(key, { reading: receipt.reading, receipt, sending: false }))
      .catch((error: unknown) => patch(key, { error: message(error), sending: false }));
  }, [ownerEpoch, patch, states]);

  return (
    <ToolIoPanelView
      unavailableReason={unavailableReason}
      signals={signals}
      states={states}
      sourcesNotice={facts.notice}
      onSetMock={setMock}
      globalRules={controlSource == null ? undefined : globalRules}
      globalRulesCurrent={globalRulesCurrent}
    />
  );
}
