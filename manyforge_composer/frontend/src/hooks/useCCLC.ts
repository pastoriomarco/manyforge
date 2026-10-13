import { useCallback, useEffect, useRef, useState } from 'react';

import { api } from '../api/client';
import type {
  CCLCComponentSnapshot,
  CCLCOwnerIdentity,
  CCLCLifecycleIntent,
  CCLCLifecycleRequestBody,
  CCLCRequestContext,
  CCLCRouteRequestBody,
  CCLCRouteTemplate,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';
import type { ExecutionIdentity } from '../types/robotControl';

const POLL_INTERVAL_MS = 250;
const OBSERVATION_INTERVAL_MS = 2000;
let fallbackIdentitySequence = 0;

export interface CCLCController {
  componentId: string;
  snapshot: CCLCComponentSnapshot | null;
  routeTemplates: CCLCRouteTemplateSet | null;
  transaction: CCLCTransactionStatus | null;
  pendingTransaction: { kind: 'lifecycle' | 'route'; id: string } | null;
  pendingActivationRequest: CCLCLifecycleRequestBody | null;
  busy: boolean;
  readUnavailableReason: string | null;
  error: string | null;
  setComponentId: (value: string) => void;
  refresh: () => Promise<void>;
  reconcileTransaction: () => Promise<void>;
  dismissError: () => void;
  submitLifecycle: (intent: CCLCLifecycleIntent) => Promise<void>;
  activate: () => Promise<void>;
  activateSequence: () => Promise<void>;
  cancelActivation: () => void;
  activationProgress: string | null;
  submitRoute: (template: CCLCRouteTemplate) => Promise<void>;
}

export function browserRequestIdentity(prefix: string): string {
  const browserCrypto = globalThis.crypto;
  if (typeof browserCrypto?.randomUUID === 'function') {
    return `${prefix}-${browserCrypto.randomUUID()}`;
  }
  if (typeof browserCrypto?.getRandomValues === 'function') {
    const words = new Uint32Array(4);
    browserCrypto.getRandomValues(words);
    const suffix = Array.from(words, (word) => word.toString(16).padStart(8, '0'))
      .join('');
    return `${prefix}-${suffix}`;
  }
  fallbackIdentitySequence = (fallbackIdentitySequence + 1) % 0x1_0000_0000;
  return `${prefix}-${Date.now().toString(36)}-${fallbackIdentitySequence.toString(36)}`;
}

export function requestContext(
  ownerEpoch: string,
  correlation: string,
): CCLCRequestContext {
  return {
    request_id: browserRequestIdentity('composer-cclc-request'),
    request_owner_epoch: ownerEpoch,
    source_label: 'manyforge_composer',
    correlation_id: correlation,
  };
}

export function cclcRequestOwnerEpoch(
  owner: CCLCOwnerIdentity,
  snapshot: CCLCComponentSnapshot,
  execution: ExecutionIdentity,
): string {
  if (
    execution.graphRevision !== snapshot.graphRevision
    || execution.retainedBundleId !== snapshot.retainedBundleId
  ) {
    throw new Error('CCLC component and execution-owner projections are torn');
  }
  return owner.requestOwnerEpoch;
}

export function lifecycleSupportsRouteSelection(
  state: CCLCComponentSnapshot['lifecycleState'],
): boolean {
  return state === 'prepared' || state === 'active';
}

export function cclcControlModeUnavailableReason(
  mode: ExecutionIdentity['controlManagementMode'],
): string | null {
  if (mode === 'component_lifecycle') return null;
  if (mode === 'direct') {
    return 'This Deployment uses direct native control; component lifecycle and route selection do not apply.';
  }
  return 'Component lifecycle control is still attaching to the active Deployment.';
}

export function lifecycleRequestFromSnapshot(
  snapshot: CCLCComponentSnapshot,
  intent: CCLCLifecycleIntent,
  context: CCLCRequestContext,
  admissionGeneration: number,
): CCLCLifecycleRequestBody {
  return {
    request_context: context,
    controllable_component_id: snapshot.componentId,
    intent,
    expected_graph_revision: snapshot.graphRevision,
    expected_component_lifecycle_generation: snapshot.lifecycleGeneration,
    expected_admission_generation: admissionGeneration,
    expected_control_domains: snapshot.domains.map((domain) => ({
      control_domain_id: domain.controlDomainId,
      control_domain_revision: domain.controlDomainRevision,
      controller_topology_id: domain.controllerTopologyId,
      topology_revision: domain.topologyRevision,
      expected_active_control_configuration: domain.activeConfiguration === null
        ? {
          presence: 'absent',
          active_control_configuration_id: null,
          active_control_configuration_generation: null,
        }
        : {
          presence: 'present',
          active_control_configuration_id: domain.activeConfiguration.id,
          active_control_configuration_generation:
            domain.activeConfiguration.generation,
        },
      expected_control_bindings: domain.currentControlBindings.map((binding) => ({
        control_binding_id: binding.controlBindingId,
        control_binding_revision: binding.controlBindingRevision,
        control_route_id: binding.controlRouteId,
      })),
    })),
    reason: 'operator-confirmed Composer request',
    confirmed: true,
  };
}

export function routeRequestFromTemplate(
  templates: CCLCRouteTemplateSet,
  template: CCLCRouteTemplate,
  context: CCLCRequestContext,
  admissionGeneration: number,
): CCLCRouteRequestBody {
  if (!templates.templates.includes(template)) {
    throw new Error('route template is not from the current projection');
  }
  return {
    request_context: context,
    expected_admission_generation: admissionGeneration,
    expected_retained_bundle_id: templates.retainedBundleId,
    expected_graph_revision: templates.graphRevision,
    desired_configuration: template.desired_configuration,
    expected_active_control_configuration:
      template.expected_active_control_configuration,
    component_expectations: template.component_expectations,
    confirmed: true,
  };
}

/** Browser composition only. Each effect and its completion stay CCLC-owned. */
export async function activateThroughOrdinaryLifecycle(
  read: () => Promise<{ snapshot: CCLCComponentSnapshot; ownerEpoch: string }>,
  submit: (intent: CCLCLifecycleIntent) => Promise<void>,
): Promise<void> {
  const initial = await read();
  const check = async () => {
    const current = await read();
    if (current.ownerEpoch !== initial.ownerEpoch
      || current.snapshot.componentId !== initial.snapshot.componentId
      || current.snapshot.retainedBundleId !== initial.snapshot.retainedBundleId
      || current.snapshot.graphRevision !== initial.snapshot.graphRevision) {
      throw new Error('Activation stopped: owner or applied configuration changed.');
    }
    if (current.snapshot.lifecycleProgress?.incident
      || current.snapshot.lifecycleProgress?.phase === 'outcome_uncertain') {
      throw new Error('Activation stopped: component requires intervention.');
    }
    return current.snapshot.lifecycleState;
  };
  let state = await check();
  if (state === 'active') return;
  if (state === 'declared' || state === 'disabled') {
    await submit('prepare');
    state = await check();
  }
  if (state !== 'prepared') throw new Error(`Cannot activate a component that is ${state}.`);
  await submit('enable');
  if (await check() !== 'active') throw new Error('Activation has no positive Active observation.');
}

export async function activateOrderedComponents(
  componentIds: readonly string[],
  activate: (componentId: string) => Promise<void>,
): Promise<void> {
  for (const id of componentIds) await activate(id);
}

export async function awaitActivationCompletion(
  acceptedStatus: CCLCTransactionStatus | null,
  readStatus: () => Promise<CCLCTransactionStatus | null>,
  waitForNextRead: () => Promise<void>,
): Promise<void> {
  let status = acceptedStatus;
  while (status === null || status.terminalResult === null) {
    if (status?.reconciliationRequired) {
      throw new Error('Activation outcome requires reconciliation.');
    }
    await waitForNextRead();
    status = await readStatus();
    if (status === null) throw new Error('Accepted activation transaction has no retained status.');
  }
  if (status.terminalResult !== 'succeeded' || status.reconciliationRequired) {
    throw new Error(status.error?.message ?? `Activation failed: ${status.terminalResult}.`);
  }
}

function waitForObservation(signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    signal.throwIfAborted();
    const abort = () => { clearTimeout(timer); reject(signal.reason); };
    const timer = setTimeout(() => {
      signal.removeEventListener('abort', abort);
      resolve();
    }, POLL_INTERVAL_MS);
    signal.addEventListener('abort', abort, { once: true });
  });
}

export function useCCLC(
  enabled: boolean,
  admissionGeneration: number | null,
): CCLCController {
  const [componentId, setComponentIdValue] = useState('');
  const [snapshot, setSnapshot] = useState<CCLCComponentSnapshot | null>(null);
  const [routeTemplates, setRouteTemplates] = useState<CCLCRouteTemplateSet | null>(null);
  const [transaction, setTransaction] = useState<CCLCTransactionStatus | null>(null);
  const [pendingTransaction, setPendingTransaction] = useState<{
    kind: 'lifecycle' | 'route'; id: string;
  } | null>(null);
  const [busy, setBusy] = useState(false);
  const [pendingActivationRequest, setPendingActivationRequest] = useState<CCLCLifecycleRequestBody | null>(null);
  const activationRequest = useRef<CCLCLifecycleRequestBody | null>(null);
  const [readUnavailableReason, setReadUnavailableReason] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [activationProgress, setActivationProgress] = useState<string | null>(null);
  const activationAbort = useRef<AbortController | null>(null);
  const admissionRef = useRef(admissionGeneration);
  admissionRef.current = admissionGeneration;
  useEffect(() => () => activationAbort.current?.abort(new Error('Activation stopped: client disconnected.')), [enabled]);

  const setComponentId = useCallback((value: string) => {
    setComponentIdValue(value);
    setSnapshot(null);
    setRouteTemplates(null);
    setReadUnavailableReason(null);
  }, []);

  const readCurrent = useCallback(async (
    signal?: AbortSignal, addressedComponent?: string, publishSelected = true,
  ) => {
    if (!enabled) throw new Error('CCLC client is unavailable');
    const identity = (addressedComponent ?? componentId).trim();
    if (!identity) throw new Error('Enter a component identity first');
    const execution = await api.execution.identity(signal);
    const unavailableReason = cclcControlModeUnavailableReason(
      execution.controlManagementMode,
    );
    if (unavailableReason !== null) throw new Error(unavailableReason);
    const [owner, nextSnapshot] = await Promise.all([
      api.cclc.owner(signal),
      api.cclc.component(identity, signal),
    ]);
    const ownerEpoch = cclcRequestOwnerEpoch(owner, nextSnapshot, execution);
    let nextTemplates: CCLCRouteTemplateSet = {
      componentId: nextSnapshot.componentId,
      retainedBundleId: nextSnapshot.retainedBundleId,
      graphRevision: nextSnapshot.graphRevision,
      templates: [],
    };
    if (publishSelected && lifecycleSupportsRouteSelection(nextSnapshot.lifecycleState)) {
      nextTemplates = await api.cclc.routeTemplates(identity, signal);
      if (
        nextSnapshot.componentId !== nextTemplates.componentId
        || nextSnapshot.retainedBundleId !== nextTemplates.retainedBundleId
        || nextSnapshot.graphRevision !== nextTemplates.graphRevision
      ) {
        throw new Error('CCLC component and route projections are torn');
      }
    }
    if (publishSelected) {
      setSnapshot(nextSnapshot);
      setRouteTemplates(nextTemplates);
      setReadUnavailableReason(null);
    }
    return { snapshot: nextSnapshot, templates: nextTemplates, ownerEpoch };
  }, [componentId, enabled]);

  const refresh = useCallback(async () => {
    if (activationAbort.current !== null) return;
    setBusy(true);
    setError(null);
    try {
      await readCurrent();
    } catch (reason) {
      setReadUnavailableReason(
        reason instanceof Error ? reason.message : 'CCLC state is unavailable',
      );
    } finally {
      setBusy(false);
    }
  }, [readCurrent]);

  const submitLifecycle = useCallback(async (intent: CCLCLifecycleIntent) => {
    if (activationAbort.current !== null) return;
    setBusy(true);
    setError(null);
    try {
      if (admissionGeneration === null) {
        throw new Error('Current authoring admission is unavailable');
      }
      const current = await readCurrent();
      const result = await api.cclc.submitLifecycle(
        lifecycleRequestFromSnapshot(
          current.snapshot,
          intent,
          requestContext(current.ownerEpoch, `lifecycle:${intent}`),
          admissionGeneration,
        ),
      );
      if (result.incidentRecovery !== null) {
        // Authorization-only Recover of an open incident: nothing to track;
        // re-read the closed incident and retained evidence.
        setTransaction(null);
        setPendingTransaction(null);
        await readCurrent();
        return;
      }
      if (result.transactionId === null) {
        throw new Error(`CCLC lifecycle request was ${result.replayStatus}`);
      }
      setTransaction(result.status);
      setPendingTransaction({ kind: 'lifecycle', id: result.transactionId });
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Lifecycle request failed');
    } finally {
      setBusy(false);
    }
  }, [admissionGeneration, readCurrent]);

  const runActivation = useCallback(async (sequence: boolean) => {
    if (activationAbort.current !== null || activationRequest.current !== null || busy) return;
    const abort = new AbortController();
    activationAbort.current = abort;
    setBusy(true);
    setError(null);
    try {
      const applied = sequence ? (await api.deploymentWorkspace.get(abort.signal)).managerApplied : null;
      const order = applied?.activationOrder ?? null;
      if (sequence && order === null) throw new Error('Applied activation order is unavailable; no captured Deployment.');
      const owner = await api.cclc.owner(abort.signal);
      const ids = order === null ? [componentId] : order.componentIds;
      await activateOrderedComponents(ids, async (id) => {
        let binding: { componentId: string; retainedBundleId: string; graphRevision: string; ownerEpoch: string } | null = null;
        const verify = async () => {
          abort.signal.throwIfAborted();
          if (applied !== null) {
            const fresh = (await api.deploymentWorkspace.get(abort.signal)).managerApplied;
            if (JSON.stringify(fresh) !== JSON.stringify(applied)) {
              throw new Error('Activation stopped: applied configuration changed.');
            }
          }
          const current = await readCurrent(abort.signal, id, !sequence);
          if (current.ownerEpoch !== owner.requestOwnerEpoch
            || (order !== null && (current.snapshot.retainedBundleId !== order.retainedBundleId
              || current.snapshot.graphRevision !== order.graphRevision
              || !order.componentIds.includes(id)))) {
            throw new Error('Activation stopped: owner or component identity changed.');
          }
          const identity = {
            componentId: current.snapshot.componentId,
            retainedBundleId: current.snapshot.retainedBundleId,
            graphRevision: current.snapshot.graphRevision,
            ownerEpoch: current.ownerEpoch,
          };
          if (binding !== null && (identity.componentId !== binding.componentId
            || identity.retainedBundleId !== binding.retainedBundleId
            || identity.graphRevision !== binding.graphRevision
            || identity.ownerEpoch !== binding.ownerEpoch)) {
            throw new Error('Activation stopped: owner or applied configuration changed.');
          }
          binding ??= identity;
          return current;
        };
        setActivationProgress(`Activating ${id}`);
        await activateThroughOrdinaryLifecycle(verify, async (intent) => {
          const current = await verify();
          const generation = admissionRef.current;
          if (generation === null) throw new Error('Current authoring admission is unavailable.');
          const request = lifecycleRequestFromSnapshot(
            current.snapshot, intent, requestContext(current.ownerEpoch, `activate:${id}:${intent}`), generation,
          );
          // Retain the exact readback address BEFORE dispatch; abort is not proof
          // that the owner did not accept the effect.
          activationRequest.current = request;
          setPendingActivationRequest(request);
          setPendingTransaction(null);
          setTransaction(null);
          const result = await api.cclc.submitLifecycle(request, abort.signal);
          if (result.transactionId === null || result.incidentRecovery !== null) {
            throw new Error(`Activation request was ${result.replayStatus}.`);
          }
          setPendingTransaction({ kind: 'lifecycle', id: result.transactionId });
          setTransaction(result.status);
          activationRequest.current = null;
          setPendingActivationRequest(null);
          await awaitActivationCompletion(result.status, async () => {
            const status = await api.cclc.lifecycleStatus(result.transactionId!, abort.signal);
            setTransaction(status);
            return status;
          }, () => waitForObservation(abort.signal));
          const state = (await verify()).snapshot.lifecycleState;
          if (state !== (intent === 'prepare' ? 'prepared' : 'active')) {
            throw new Error(`Activation has no positive ${intent === 'prepare' ? 'Prepared' : 'Active'} observation.`);
          }
        });
      });
      setActivationProgress(sequence ? 'Activation sequence completed.' : 'Component is Active.');
    } catch (reason) {
      setActivationProgress(activationRequest.current === null
        ? 'Activation stopped. Submit a new explicit command after resolving the cause.'
        : 'Activation stopped with an unknown request outcome. Reconcile the exact request before a new activation.');
      setError(reason instanceof Error ? reason.message : 'Activation outcome is unknown.');
    } finally {
      activationAbort.current = null;
      setBusy(false);
    }
  }, [busy, componentId, readCurrent]);

  const submitRoute = useCallback(async (template: CCLCRouteTemplate) => {
    if (activationAbort.current !== null) return;
    setBusy(true);
    setError(null);
    try {
      if (admissionGeneration === null) {
        throw new Error('Current authoring admission is unavailable');
      }
      const current = await readCurrent();
      const selected = current.templates.templates.find(
        (candidate) => candidate.desired_configuration.control_domain_id
          === template.desired_configuration.control_domain_id,
      );
      if (selected === undefined) {
        throw new Error('Selected route is absent from the fresh projection');
      }
      const result = await api.cclc.submitRoute(
        routeRequestFromTemplate(
          current.templates,
          selected,
          requestContext(
            current.ownerEpoch,
            `route:${selected.desired_configuration.control_domain_id}`,
          ),
          admissionGeneration,
        ),
      );
      if (result.transactionId === null) {
        throw new Error(`CCLC route request was ${result.replayStatus}`);
      }
      setTransaction(result.status);
      setPendingTransaction({ kind: 'route', id: result.transactionId });
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Route request failed');
    } finally {
      setBusy(false);
    }
  }, [admissionGeneration, readCurrent]);

  const reconcileTransaction = useCallback(async () => {
    if (activationAbort.current !== null) return;
    const sentRequest = activationRequest.current;
    if (sentRequest === null && pendingTransaction === null) return;
    setBusy(true);
    setError(null);
    try {
      let addressed = pendingTransaction;
      if (sentRequest !== null) {
        const result = await api.cclc.lifecycleRequestResult(sentRequest);
        if (result === null || result.transactionId === null) {
          throw new Error('Exact activation request has no retained acceptance; outcome remains unknown.');
        }
        addressed = { kind: 'lifecycle', id: result.transactionId };
        setPendingTransaction(addressed);
        setTransaction(result.status);
        activationRequest.current = null;
        setPendingActivationRequest(null);
      }
      if (addressed === null) return;
      const next = addressed.kind === 'lifecycle'
        ? await api.cclc.lifecycleStatus(addressed.id)
        : await api.cclc.routeStatus(addressed.id);
      if (next === null) {
        throw new Error('Accepted CCLC transaction has no retained status');
      }
      setTransaction(next);
      if (next.terminalResult !== null) await readCurrent();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'CCLC reconciliation failed');
    } finally {
      setBusy(false);
    }
  }, [pendingTransaction, readCurrent]);

  // Keep presenting the owner's current target progress and readiness without
  // an operator acknowledgement. This only re-reads CCLC; it decides nothing.
  const transactionOpen = pendingTransaction !== null && !transaction?.terminalResult;
  const observing = enabled && componentId.trim() !== '' && !busy && !transactionOpen;
  useEffect(() => {
    if (!observing) return undefined;
    let active = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;
    const controller = new AbortController();
    const observe = async () => {
      try {
        await readCurrent(controller.signal);
      } catch (reason) {
        if (!active) return;
        setSnapshot(null);
        setRouteTemplates(null);
        setReadUnavailableReason(
          reason instanceof Error ? reason.message : 'CCLC state is unavailable',
        );
      }
      if (active) timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    };
    timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    return () => {
      active = false;
      controller.abort();
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [observing, readCurrent]);

  useEffect(() => {
    if (!enabled || busy || pendingTransaction === null || transaction?.terminalResult) {
      return undefined;
    }
    let active = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;
    const poll = async () => {
      try {
        const next = pendingTransaction.kind === 'lifecycle'
          ? await api.cclc.lifecycleStatus(pendingTransaction.id)
          : await api.cclc.routeStatus(pendingTransaction.id);
        if (!active) return;
        if (next === null) {
          setError('Accepted CCLC transaction has no retained status');
          return;
        }
        setTransaction(next);
        if (next.terminalResult === null) {
          timeout = setTimeout(poll, POLL_INTERVAL_MS);
        } else {
          await readCurrent();
        }
      } catch (reason) {
        if (active) {
          setError(reason instanceof Error ? reason.message : 'CCLC status failed');
        }
      }
    };
    timeout = setTimeout(poll, 0);
    return () => {
      active = false;
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [busy, enabled, pendingTransaction, readCurrent, transaction?.terminalResult]);

  return {
    componentId,
    snapshot,
    routeTemplates,
    transaction,
    pendingTransaction,
    pendingActivationRequest,
    busy,
    readUnavailableReason,
    error,
    setComponentId,
    refresh,
    reconcileTransaction,
    dismissError: () => setError(null),
    submitLifecycle,
    activate: () => runActivation(false),
    activateSequence: () => runActivation(true),
    cancelActivation: () => activationAbort.current?.abort(new Error('Activation sequence cancelled; accepted work remains observable.')),
    activationProgress,
    submitRoute,
  };
}
