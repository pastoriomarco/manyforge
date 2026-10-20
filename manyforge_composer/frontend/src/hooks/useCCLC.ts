import { useCallback, useEffect, useRef, useState } from 'react';

import { api, ApiError } from '../api/client';
import type {
  CCLCExpectedActiveConfiguration,
  CCLCComponentSnapshot,
  CCLCOwnerIdentity,
  CCLCLifecycleIntent,
  CCLCLifecycleRequestBody,
  CCLCRequestContext,
  CCLCRouteRequestBody,
  CCLCRouteTemplateWire,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';
import type { ExecutionIdentity } from '../types/robotControl';
import { readinessErrorPresentation } from '../state/componentLifecyclePresentation';

const POLL_INTERVAL_MS = 250;
const OBSERVATION_INTERVAL_MS = 2000;
let fallbackIdentitySequence = 0;

// Only the activation observer consumes this marker. Other reads keep their
// ordinary error path, and mutations are never wrapped or replayed.
class ComponentReadChanged extends Error {
  constructor(error: ApiError) {
    super(error.message, { cause: error });
  }
}

export interface ActivationSequenceObservation {
  /** Applied configuration, configured IDs, CCLC/execution owners and publication. */
  identity: string;
  components: CCLCComponentSnapshot[];
}

export interface CCLCController {
  controllableComponentId: string;
  snapshot: CCLCComponentSnapshot | null;
  routeTemplates: CCLCRouteTemplateSet | null;
  transaction: CCLCTransactionStatus | null;
  pendingTransaction: { kind: 'lifecycle' | 'route'; id: string } | null;
  pendingActivationRequest: CCLCLifecycleRequestBody | null;
  busy: boolean;
  readUnavailableReason: string | null;
  error: string | null;
  setControllableComponentId: (value: string) => void;
  refresh: () => Promise<void>;
  reconcileTransaction: () => Promise<void>;
  dismissError: () => void;
  submitLifecycle: (intent: CCLCLifecycleIntent) => Promise<void>;
  activate: () => Promise<void>;
  activateSequence: (expectedIdentity?: string) => Promise<boolean>;
  /** Ordinary Disable of each listed component, in the given order, each with a
   * positive Disabled observation; stops at the first failure (Q107 Apply).
   * On success, `consumedAdmissionGeneration` is the authoring admission the
   * last Disable used (null when nothing was disabled): a following request
   * must wait for a newer one. */
  deactivateComponents: (componentIds: readonly string[], expectedIdentity: string) => Promise<DeactivationResult>;
  readActivationSequence: (signal?: AbortSignal) => Promise<ActivationSequenceObservation>;
  cancelActivation: () => void;
  activationProgress: string | null;
  componentHealth: Record<string, { snapshot: CCLCComponentSnapshot | null; error: string | null }>;
  activationUnresolved: boolean;
  activationRefusal: { controllableComponentId: string; transactionId: string; error: NonNullable<CCLCTransactionStatus['error']> } | null;
  submitRoute: (template: CCLCRouteTemplateWire) => Promise<void>;
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
    || execution.activePublicationGeneration !== snapshot.publicationGeneration
    || snapshot.readiness.requestOwnerEpoch !== owner.requestOwnerEpoch
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
    controllable_component_id: snapshot.controllableComponentId,
    intent,
    expected_graph_revision: snapshot.graphRevision,
    expected_component_lifecycle_generation: snapshot.lifecycleGeneration,
    expected_admission_generation: admissionGeneration,
    expected_control_domains: snapshot.domains.map((domain) => {
      const expected_active_control_configuration: CCLCExpectedActiveConfiguration =
        domain.activeConfiguration === null
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
          };
      // Every expected domain carries its required kind; neither variant
      // carries the response `resources`.
      switch (domain.kind) {
        case 'io_provider':
          return {
            kind: 'io_provider' as const,
            control_domain_id: domain.controlDomainId,
            control_domain_revision: domain.controlDomainRevision,
            provider: domain.provider,
            expected_active_control_configuration,
          };
        case 'controller':
          return {
            kind: 'controller' as const,
            control_domain_id: domain.controlDomainId,
            control_domain_revision: domain.controlDomainRevision,
            controller_topology_id: domain.controllerTopologyId,
            topology_revision: domain.topologyRevision,
            expected_active_control_configuration,
            expected_control_bindings: domain.currentControlBindings.map((binding) => ({
              control_binding_id: binding.controlBindingId,
              control_binding_revision: binding.controlBindingRevision,
              control_route_id: binding.controlRouteId,
            })),
          };
        default: {
          const unknown: never = domain;
          throw new Error(`unknown CCLC domain kind: ${String((unknown as { kind: unknown }).kind)}`);
        }
      }
    }),
    reason: 'operator-confirmed Composer request',
    confirmed: true,
  };
}

export function routeRequestFromTemplate(
  templates: CCLCRouteTemplateSet,
  template: CCLCRouteTemplateWire,
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
  waitForNextRead: () => Promise<void> = async () => {},
  showReadiness: (snapshot: CCLCComponentSnapshot) => void = () => {},
): Promise<void> {
  const initial = await read();
  const check = async () => {
    const current = await read();
    if (current.ownerEpoch !== initial.ownerEpoch
      || current.snapshot.controllableComponentId !== initial.snapshot.controllableComponentId
      || current.snapshot.retainedBundleId !== initial.snapshot.retainedBundleId
      || current.snapshot.graphRevision !== initial.snapshot.graphRevision) {
      throw new Error('Activation stopped: owner or applied configuration changed.');
    }
    if (current.snapshot.publicationGeneration !== initial.snapshot.publicationGeneration
      || current.snapshot.activePublicPointerDigest !== initial.snapshot.activePublicPointerDigest) {
      throw new Error('Activation stopped: applied publication changed.');
    }
    return current.snapshot;
  };
  const ready = async () => {
    while (true) {
      const current = await check();
      showReadiness(current);
      // Active retains the established no-effect activation skip. Health remains displayed.
      if (current.lifecycleState === 'active' || current.readiness.ready) return current;
      await waitForNextRead();
    }
  };
  let current = await ready();
  if (current.lifecycleState === 'active') return;
  if (current.lifecycleState === 'declared' || current.lifecycleState === 'disabled') {
    await submit('prepare');
    current = await ready();
  }
  if (current.lifecycleState !== 'prepared') throw new Error(`Cannot activate a component that is ${current.lifecycleState}.`);
  await submit('enable');
  if ((await check()).lifecycleState !== 'active') throw new Error('Activation has no positive Active observation.');
}

export async function activateOrderedComponents(
  componentIds: readonly string[],
  activate: (controllableComponentId: string) => Promise<void>,
): Promise<void> {
  for (const id of componentIds) await activate(id);
}

export async function awaitActivationCompletion(
  acceptedStatus: CCLCTransactionStatus | null,
  readStatus: () => Promise<CCLCTransactionStatus | null>,
  waitForNextRead: () => Promise<void>,
  label = 'Activation',
): Promise<void> {
  let status = acceptedStatus;
  while (status === null || status.terminalResult === null) {
    if (status?.reconciliationRequired) {
      throw new Error(`${label} outcome requires reconciliation.`);
    }
    await waitForNextRead();
    status = await readStatus();
    if (status === null) throw new Error(`Accepted ${label.toLowerCase()} transaction has no retained status.`);
  }
  if (status.terminalResult !== 'succeeded' || status.reconciliationRequired) {
    throw new Error(status.error?.message ?? `${label} failed: ${status.terminalResult}.`);
  }
}

export type DeactivationResult =
  | { succeeded: true; consumedAdmissionGeneration: number | null }
  | { succeeded: false };

/** Lifecycle states in which a configuration Apply may not change a component (Q107). */
export const DEACTIVATION_REQUIRED_STATES: readonly CCLCComponentSnapshot['lifecycleState'][] = [
  'prepared', 'active', 'degraded', 'faulted',
];

export function waitForObservation(signal: AbortSignal): Promise<void> {
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
  configuredComponentIds?: readonly string[],
): CCLCController {
  const [controllableComponentId, setComponentIdValue] = useState('');
  const [snapshot, setSnapshot] = useState<CCLCComponentSnapshot | null>(null);
  const [routeTemplates, setRouteTemplates] = useState<CCLCRouteTemplateSet | null>(null);
  const [transaction, setTransaction] = useState<CCLCTransactionStatus | null>(null);
  const [pendingTransaction, setPendingTransaction] = useState<{
    kind: 'lifecycle' | 'route'; id: string;
  } | null>(null);
  const [busy, setBusy] = useState(false);
  const busyRef = useRef(false);
  const [pendingActivationRequest, setPendingActivationRequest] = useState<CCLCLifecycleRequestBody | null>(null);
  const activationRequest = useRef<CCLCLifecycleRequestBody | null>(null);
  const acceptedActivation = useRef<{ request: CCLCLifecycleRequestBody; transactionId: string } | null>(null);
  const [componentHealth, setComponentHealth] = useState<CCLCController['componentHealth']>({});
  const [readUnavailableReason, setReadUnavailableReason] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [activationProgress, setActivationProgress] = useState<string | null>(null);
  const [activationRefusal, setActivationRefusal] = useState<CCLCController['activationRefusal']>(null);
  const activationAbort = useRef<AbortController | null>(null);
  const passiveAbort = useRef<AbortController | null>(null);
  const admissionRef = useRef(admissionGeneration);
  const selectedRef = useRef(controllableComponentId);
  selectedRef.current = controllableComponentId;
  const configuredKey = JSON.stringify(configuredComponentIds ?? (controllableComponentId ? [controllableComponentId] : []));
  const [healthComponentKey, setHealthComponentKey] = useState(configuredKey);
  if (healthComponentKey !== configuredKey) {
    setHealthComponentKey(configuredKey);
    const ids = JSON.parse(configuredKey) as string[];
    setComponentHealth((previous) => Object.fromEntries(ids.map((id) => [id,
      previous[id] ?? { snapshot: null, error: 'Current component health has not been read.' }])));
  }
  const configuredRef = useRef(configuredKey);
  const currentReads = useRef(new Map<string, object>());
  configuredRef.current = configuredKey;
  admissionRef.current = admissionGeneration;
  useEffect(() => () => activationAbort.current?.abort(new Error('Activation stopped: client disconnected.')), [enabled]);

  const setControllableComponentId = useCallback((value: string) => {
    activationAbort.current?.abort(new Error('Activation stopped: selected component changed.'));
    setComponentIdValue(value);
    setSnapshot(null);
    setRouteTemplates(null);
    setReadUnavailableReason(null);
  }, []);

  const readCurrent = useCallback(async (
    signal?: AbortSignal, addressedComponent?: string, publishSelected = true,
    validate?: (snapshot: CCLCComponentSnapshot, ownerEpoch: string, executionOwnerEpoch: string) => void,
    observeComponentReadChanges = false,
  ) => {
    if (!enabled) throw new Error('CCLC client is unavailable');
    const identity = (addressedComponent ?? controllableComponentId).trim();
    if (!identity) throw new Error('Enter a component identity first');
    const marker = {};
    currentReads.current.set(identity, marker);
    try {
      const execution = await api.execution.identity(signal);
      signal?.throwIfAborted();
      const unavailableReason = cclcControlModeUnavailableReason(
        execution.controlManagementMode,
      );
      if (unavailableReason !== null) throw new Error(unavailableReason);
      const [owner, nextSnapshot] = await Promise.all([
        api.cclc.owner(signal),
        observeComponentReadChanges ? api.cclc.component(identity, signal).catch((reason: unknown) => {
          if (observeComponentReadChanges && reason instanceof ApiError && reason.status === 409
            && reason.structuredError?.code === 'component_lifecycle_facts_stale'
            && reason.structuredError.details?.reason === 'component_read_changed') {
            throw new ComponentReadChanged(reason);
          }
          throw reason;
        }) : api.cclc.component(identity, signal),
      ]);
      signal?.throwIfAborted();
      const ownerEpoch = cclcRequestOwnerEpoch(owner, nextSnapshot, execution);
      if (nextSnapshot.controllableComponentId !== identity) throw new Error('CCLC component identity does not match its request');
      validate?.(nextSnapshot, ownerEpoch, execution.ownerEpoch);
      let nextTemplates: CCLCRouteTemplateSet = {
        controllableComponentId: nextSnapshot.controllableComponentId,
        retainedBundleId: nextSnapshot.retainedBundleId,
        graphRevision: nextSnapshot.graphRevision,
        templates: [],
      };
      if (publishSelected && lifecycleSupportsRouteSelection(nextSnapshot.lifecycleState)) {
        nextTemplates = await api.cclc.routeTemplates(identity, signal);
        if (
          nextSnapshot.controllableComponentId !== nextTemplates.controllableComponentId
          || nextSnapshot.retainedBundleId !== nextTemplates.retainedBundleId
          || nextSnapshot.graphRevision !== nextTemplates.graphRevision
        ) {
          throw new Error('CCLC component and route projections are torn');
        }
      }
      signal?.throwIfAborted();
      if (currentReads.current.get(identity) !== marker) throw new Error('CCLC read was superseded');
      setComponentHealth((previous) => ({ ...previous, [identity]: { snapshot: nextSnapshot, error: null } }));
      if (publishSelected && selectedRef.current === identity) {
        setSnapshot(nextSnapshot);
        setRouteTemplates(nextTemplates);
        setReadUnavailableReason(null);
      }
      return { snapshot: nextSnapshot, templates: nextTemplates, ownerEpoch, executionOwnerEpoch: execution.ownerEpoch };
    } catch (reason) {
      if (!(reason instanceof ComponentReadChanged)
        && !signal?.aborted && currentReads.current.get(identity) === marker) {
        const message = reason instanceof Error ? reason.message : 'CCLC state is unavailable';
        setComponentHealth((previous) => ({ ...previous, [identity]: { snapshot: null, error: message } }));
        if (selectedRef.current === identity) {
          setSnapshot(null);
          setRouteTemplates(null);
          setReadUnavailableReason(message);
        }
      }
      throw reason;
    } finally {
      if (currentReads.current.get(identity) === marker) currentReads.current.delete(identity);
    }
  }, [controllableComponentId, enabled]);

  const refresh = useCallback(async () => {
    if (activationAbort.current !== null) return;
    busyRef.current = true;
    setBusy(true);
    setError(null);
    try {
      await readCurrent();
    } catch (reason) {
      // The addressed reader owns failure invalidation and obsolete-read guards.
      void reason;
    } finally {
      busyRef.current = false;
      setBusy(false);
    }
  }, [readCurrent]);

  const submitLifecycle = useCallback(async (intent: CCLCLifecycleIntent) => {
    if (activationAbort.current !== null) return;
    busyRef.current = true;
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
      busyRef.current = false;
      setBusy(false);
    }
  }, [admissionGeneration, readCurrent]);

  // A finite, fresh read of ALL Applied components; never infer readiness from
  // the passive display cache or retry a stale read on the Execute path.
  const readActivationSequence = useCallback(async (signal?: AbortSignal): Promise<ActivationSequenceObservation> => {
    if (activationAbort.current !== null || activationRequest.current !== null || acceptedActivation.current !== null
      || (pendingTransaction !== null && (transaction?.terminalResult == null || transaction.reconciliationRequired)) || busyRef.current) {
      throw new Error('Component lifecycle work is busy or requires reconciliation.');
    }
    passiveAbort.current?.abort();
    busyRef.current = true;
    setBusy(true);
    try {
      const configured = configuredRef.current;
      const applied = (await api.deploymentWorkspace.get(signal)).managerApplied;
      const order = applied.activationOrder;
      if (order === null) throw new Error('Applied activation order is unavailable; no captured Deployment.');
      const owner = await api.cclc.owner(signal);
      let executionOwnerEpoch: string | null = null;
      let publication: { generation: number; pointerDigest: string } | null = null;
      const components: CCLCComponentSnapshot[] = [];
      for (const id of order.componentIds) {
        const current = await readCurrent(signal, id, false, (next, ownerEpoch, hostOwnerEpoch) => {
          if (ownerEpoch !== owner.requestOwnerEpoch || next.retainedBundleId !== order.retainedBundleId
            || next.graphRevision !== order.graphRevision
            || (executionOwnerEpoch !== null && executionOwnerEpoch !== hostOwnerEpoch)
            || (publication !== null && (next.publicationGeneration !== publication.generation
              || next.activePublicPointerDigest !== publication.pointerDigest))) {
            throw new Error('Activation stopped: owner or applied configuration changed.');
          }
          executionOwnerEpoch ??= hostOwnerEpoch;
          publication ??= { generation: next.publicationGeneration, pointerDigest: next.activePublicPointerDigest };
        });
        components.push(current.snapshot);
      }
      const fresh = (await api.deploymentWorkspace.get(signal)).managerApplied;
      signal?.throwIfAborted();
      if (configuredRef.current !== configured || JSON.stringify(fresh) !== JSON.stringify(applied)) {
        throw new Error('Activation stopped: applied configuration changed.');
      }
      return { identity: JSON.stringify([configured, applied, owner.requestOwnerEpoch, executionOwnerEpoch, publication]), components };
    } finally {
      busyRef.current = false;
      setBusy(false);
    }
  }, [busy, pendingTransaction, readCurrent, transaction]);

  const runActivation = useCallback(async (sequence: boolean, expectedIdentity?: string): Promise<boolean> => {
    if (activationAbort.current !== null || activationRequest.current !== null || acceptedActivation.current !== null
      || (pendingTransaction !== null && (transaction?.terminalResult == null || transaction.reconciliationRequired)) || busyRef.current) {
      setError('Component lifecycle work is busy or requires reconciliation.');
      return false;
    }
    const abort = new AbortController();
    passiveAbort.current?.abort();
    activationAbort.current = abort;
    busyRef.current = true;
    setBusy(true);
    setError(null);
    setActivationRefusal(null);
    let refusal: CCLCController['activationRefusal'] = null;
    try {
      const applied = sequence ? (await api.deploymentWorkspace.get(abort.signal)).managerApplied : null;
      const order = applied?.activationOrder ?? null;
      if (sequence && order === null) throw new Error('Applied activation order is unavailable; no captured Deployment.');
      const owner = await api.cclc.owner(abort.signal);
      abort.signal.throwIfAborted();
      const capturedConfigured = configuredRef.current;
      const ids = order === null ? [controllableComponentId] : order.componentIds;
      let executionOwnerEpoch: string | null = null;
      let publication: { generation: number; pointerDigest: string } | null = null;
      const finalReads: Array<() => Promise<{ snapshot: CCLCComponentSnapshot }>> = [];
      await activateOrderedComponents(ids, async (id) => {
        let binding: { controllableComponentId: string; retainedBundleId: string; graphRevision: string; ownerEpoch: string;
          publicationGeneration: number; activePublicPointerDigest: string } | null = !sequence && snapshot !== null
          ? { controllableComponentId: snapshot.controllableComponentId, retainedBundleId: snapshot.retainedBundleId,
            graphRevision: snapshot.graphRevision, ownerEpoch: snapshot.readiness.requestOwnerEpoch,
            publicationGeneration: snapshot.publicationGeneration, activePublicPointerDigest: snapshot.activePublicPointerDigest } : null;
        const verify = async () => {
          while (true) {
            try {
              abort.signal.throwIfAborted();
              if (configuredRef.current !== capturedConfigured) throw new Error('Activation stopped: configured components changed.');
              if (applied !== null) {
                const fresh = (await api.deploymentWorkspace.get(abort.signal)).managerApplied;
                if (JSON.stringify(fresh) !== JSON.stringify(applied)) {
                  throw new Error('Activation stopped: applied configuration changed.');
                }
              }
              const current = await readCurrent(abort.signal, id, !sequence, (next, ownerEpoch, hostOwnerEpoch) => {
                if (publication !== null && (next.publicationGeneration !== publication.generation
                  || next.activePublicPointerDigest !== publication.pointerDigest)) {
                  throw new Error('Activation stopped: applied publication changed.');
                }
                publication ??= { generation: next.publicationGeneration, pointerDigest: next.activePublicPointerDigest };
                if (executionOwnerEpoch !== null && hostOwnerEpoch !== executionOwnerEpoch) {
                  throw new Error('Activation stopped: execution owner changed.');
                }
                executionOwnerEpoch ??= hostOwnerEpoch;
                if (ownerEpoch !== owner.requestOwnerEpoch
                  || (order !== null && (next.retainedBundleId !== order.retainedBundleId
                    || next.graphRevision !== order.graphRevision
                    || !order.componentIds.includes(id)))) {
                  throw new Error('Activation stopped: owner or component identity changed.');
                }
                const identity = {
                  controllableComponentId: next.controllableComponentId,
                  retainedBundleId: next.retainedBundleId,
                  graphRevision: next.graphRevision,
                  ownerEpoch,
                  publicationGeneration: next.publicationGeneration,
                  activePublicPointerDigest: next.activePublicPointerDigest,
                };
                if (binding !== null && (identity.controllableComponentId !== binding.controllableComponentId
                  || identity.retainedBundleId !== binding.retainedBundleId
                  || identity.graphRevision !== binding.graphRevision
                  || identity.ownerEpoch !== binding.ownerEpoch
                  || identity.publicationGeneration !== binding.publicationGeneration
                  || identity.activePublicPointerDigest !== binding.activePublicPointerDigest)) {
                  throw new Error('Activation stopped: owner or applied configuration changed.');
                }
                binding ??= identity;
                if (expectedIdentity !== undefined
                  && JSON.stringify([capturedConfigured, applied, owner.requestOwnerEpoch,
                    executionOwnerEpoch, publication]) !== expectedIdentity) {
                  throw new Error('Activation stopped: owner or applied configuration changed.');
                }
              }, true);
              abort.signal.throwIfAborted();
              return current;
            } catch (reason) {
              if (!(reason instanceof ComponentReadChanged)) throw reason;
              await waitForObservation(abort.signal);
            }
          }
        };
        finalReads.push(verify);
        setActivationProgress(`Activating ${id}`);
        await activateThroughOrdinaryLifecycle(verify, async (intent) => {
          let current = await verify();
          while (!current.snapshot.readiness.ready) {
            setActivationProgress(`${id}: Not ready — ${readinessErrorPresentation(current.snapshot.readiness.error) ?? 'owner prerequisites are unmet'}`);
            await waitForObservation(abort.signal);
            current = await verify();
          }
          if (current.snapshot.readiness.nextIntent !== intent) throw new Error('Activation stopped: lifecycle step changed.');
          abort.signal.throwIfAborted();
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
          acceptedActivation.current = { request, transactionId: result.transactionId };
          if (result.status !== null && result.status.transactionId !== result.transactionId) {
            throw new Error('Accepted CCLC transaction identity changed');
          }
          const rememberRefusal = (status: CCLCTransactionStatus | null) => {
            if (intent === 'prepare' && status?.transactionId === result.transactionId
              && status.terminalResult === 'failed_known_no_effect' && !status.reconciliationRequired
              && status.error?.code === 'component_lifecycle_transition_missing'
              && status.error.details.reason === 'no_transition_satisfied') {
              refusal = { controllableComponentId: request.controllable_component_id,
                transactionId: status.transactionId, error: status.error };
              setActivationRefusal(refusal);
            }
            if (status?.terminalResult != null && !status.reconciliationRequired) acceptedActivation.current = null;
          };
          rememberRefusal(result.status);
          activationRequest.current = null;
          setPendingActivationRequest(null);
          abort.signal.throwIfAborted();
          await awaitActivationCompletion(result.status, async () => {
            const status = await api.cclc.lifecycleStatus(result.transactionId!, abort.signal);
            abort.signal.throwIfAborted();
            if (status !== null && status.transactionId !== result.transactionId) throw new Error('Accepted CCLC transaction identity changed');
            setTransaction(status);
            rememberRefusal(status);
            return status;
          }, () => waitForObservation(abort.signal));
          acceptedActivation.current = null;
          const state = (await verify()).snapshot.lifecycleState;
          if (state !== (intent === 'prepare' ? 'prepared' : 'active')) {
            throw new Error(`Activation has no positive ${intent === 'prepare' ? 'Prepared' : 'Active'} observation.`);
          }
        }, () => waitForObservation(abort.signal), (current) => {
          setActivationProgress(current.lifecycleState === 'active' ? `${id}: Active`
            : current.readiness.ready ? `Activating ${id}: ${current.readiness.nextIntent}`
              : `${id}: Not ready — ${readinessErrorPresentation(current.readiness.error) ?? 'owner prerequisites are unmet'}`);
        });
      });
      if (expectedIdentity !== undefined) {
        for (const verify of finalReads) {
          if ((await verify()).snapshot.lifecycleState !== 'active') {
            throw new Error('Execute stopped: no exact all-Active component observation.');
          }
        }
      }
      abort.signal.throwIfAborted();
      setActivationProgress(sequence ? 'Activation sequence completed.' : 'Component is Active.');
      return true;
    } catch (reason) {
      setActivationProgress(activationRequest.current === null
        ? 'Activation stopped. Submit a new explicit command after resolving the cause.'
        : 'Activation stopped with an unknown request outcome. Reconcile the exact request before a new activation.');
      setError(refusal !== null ? null : reason instanceof Error ? reason.message : 'Activation outcome is unknown.');
      return false;
    } finally {
      activationAbort.current = null;
      busyRef.current = false;
      setBusy(false);
    }
  }, [busy, controllableComponentId, pendingTransaction, readCurrent, snapshot, transaction]);

  // The mirror of the activation sequence for a configuration Apply that changes
  // components (Q107): the ordinary Disable per component, never anything else.
  const deactivateComponents = useCallback(async (
    componentIds: readonly string[], expectedIdentity: string,
  ): Promise<DeactivationResult> => {
    if (activationAbort.current !== null || activationRequest.current !== null || acceptedActivation.current !== null
      || (pendingTransaction !== null && (transaction?.terminalResult == null || transaction.reconciliationRequired)) || busyRef.current) {
      setError('Component lifecycle work is busy or requires reconciliation.');
      return { succeeded: false };
    }
    let consumedAdmissionGeneration: number | null = null;
    const abort = new AbortController();
    passiveAbort.current?.abort();
    activationAbort.current = abort;
    busyRef.current = true;
    setBusy(true);
    setError(null);
    setActivationRefusal(null);
    try {
      const applied = (await api.deploymentWorkspace.get(abort.signal)).managerApplied;
      const order = applied.activationOrder;
      if (order === null) throw new Error('Applied activation order is unavailable; no captured Deployment.');
      const owner = await api.cclc.owner(abort.signal);
      const capturedConfigured = configuredRef.current;
      let executionOwnerEpoch: string | null = null;
      let publication: { generation: number; pointerDigest: string } | null = null;
      const verify = async (id: string) => {
        while (true) {
          try {
            abort.signal.throwIfAborted();
            if (configuredRef.current !== capturedConfigured) throw new Error('Deactivation stopped: configured components changed.');
            const fresh = (await api.deploymentWorkspace.get(abort.signal)).managerApplied;
            if (JSON.stringify(fresh) !== JSON.stringify(applied)) {
              throw new Error('Deactivation stopped: applied configuration changed.');
            }
            const current = await readCurrent(abort.signal, id, false, (next, ownerEpoch, hostOwnerEpoch) => {
              if (ownerEpoch !== owner.requestOwnerEpoch || next.retainedBundleId !== order.retainedBundleId
                || next.graphRevision !== order.graphRevision
                || (executionOwnerEpoch !== null && executionOwnerEpoch !== hostOwnerEpoch)
                || (publication !== null && (next.publicationGeneration !== publication.generation
                  || next.activePublicPointerDigest !== publication.pointerDigest))) {
                throw new Error('Deactivation stopped: owner or applied configuration changed.');
              }
              executionOwnerEpoch ??= hostOwnerEpoch;
              publication ??= { generation: next.publicationGeneration, pointerDigest: next.activePublicPointerDigest };
              if (JSON.stringify([capturedConfigured, applied, owner.requestOwnerEpoch,
                executionOwnerEpoch, publication]) !== expectedIdentity) {
                throw new Error('Deactivation stopped: owner or applied configuration changed.');
              }
            }, true);
            abort.signal.throwIfAborted();
            return current;
          } catch (reason) {
            if (!(reason instanceof ComponentReadChanged)) throw reason;
            await waitForObservation(abort.signal);
          }
        }
      };
      for (const id of componentIds) {
        if (!order.componentIds.includes(id)) throw new Error(`Deactivation stopped: ${id} is not an Applied component.`);
        const current = await verify(id);
        if (!DEACTIVATION_REQUIRED_STATES.includes(current.snapshot.lifecycleState)) {
          setActivationProgress(`${id}: ${current.snapshot.lifecycleState}`);
          continue;
        }
        setActivationProgress(`Deactivating ${id}`);
        const generation = admissionRef.current;
        if (generation === null) throw new Error('Current authoring admission is unavailable.');
        const request = lifecycleRequestFromSnapshot(
          current.snapshot, 'disable', requestContext(current.ownerEpoch, `deactivate:${id}`), generation,
        );
        consumedAdmissionGeneration = generation;
        // Retain the exact readback address BEFORE dispatch; abort is not proof
        // that the owner did not accept the effect.
        activationRequest.current = request;
        setPendingActivationRequest(request);
        setPendingTransaction(null);
        setTransaction(null);
        const result = await api.cclc.submitLifecycle(request, abort.signal);
        if (result.transactionId === null || result.incidentRecovery !== null) {
          throw new Error(`Deactivation request was ${result.replayStatus}.`);
        }
        setPendingTransaction({ kind: 'lifecycle', id: result.transactionId });
        setTransaction(result.status);
        acceptedActivation.current = { request, transactionId: result.transactionId };
        if (result.status !== null && result.status.transactionId !== result.transactionId) {
          throw new Error('Accepted CCLC transaction identity changed');
        }
        activationRequest.current = null;
        setPendingActivationRequest(null);
        await awaitActivationCompletion(result.status, async () => {
          const status = await api.cclc.lifecycleStatus(result.transactionId!, abort.signal);
          abort.signal.throwIfAborted();
          if (status !== null && status.transactionId !== result.transactionId) throw new Error('Accepted CCLC transaction identity changed');
          setTransaction(status);
          return status;
        }, () => waitForObservation(abort.signal), 'Deactivation');
        acceptedActivation.current = null;
        if ((await verify(id)).snapshot.lifecycleState !== 'disabled') {
          throw new Error(`Deactivation of ${id} has no positive Disabled observation.`);
        }
        setActivationProgress(`${id}: disabled`);
      }
      setActivationProgress('Deactivation completed.');
      return { succeeded: true, consumedAdmissionGeneration };
    } catch (reason) {
      setActivationProgress(activationRequest.current === null
        ? 'Deactivation stopped. Submit a new explicit command after resolving the cause.'
        : 'Deactivation stopped with an unknown request outcome. Reconcile the exact request before a new command.');
      setError(reason instanceof Error ? reason.message : 'Deactivation outcome is unknown.');
      return { succeeded: false };
    } finally {
      activationAbort.current = null;
      busyRef.current = false;
      setBusy(false);
    }
  }, [pendingTransaction, readCurrent, transaction]);

  const submitRoute = useCallback(async (template: CCLCRouteTemplateWire) => {
    if (activationAbort.current !== null) return;
    busyRef.current = true;
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
      busyRef.current = false;
      setBusy(false);
    }
  }, [admissionGeneration, readCurrent]);

  const reconcileTransaction = useCallback(async () => {
    if (activationAbort.current !== null) return;
    const sentRequest = activationRequest.current;
    if (sentRequest === null && pendingTransaction === null) return;
    busyRef.current = true;
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
        acceptedActivation.current = { request: sentRequest, transactionId: result.transactionId };
        setPendingTransaction(addressed);
        setTransaction(result.status);
        activationRequest.current = null;
        setPendingActivationRequest(null);
      }
      if (addressed === null) return;
      const accepted = acceptedActivation.current;
      if (accepted !== null) {
        const owner = await api.cclc.owner();
        if (owner.requestOwnerEpoch !== accepted.request.request_context.request_owner_epoch
          || addressed.id !== accepted.transactionId) throw new Error('Accepted activation owner or transaction changed; outcome remains unknown.');
      }
      const next = addressed.kind === 'lifecycle'
        ? await api.cclc.lifecycleStatus(addressed.id)
        : await api.cclc.routeStatus(addressed.id);
      if (next === null) {
        throw new Error('Accepted CCLC transaction has no retained status');
      }
      if (next.transactionId !== addressed.id) throw new Error('Accepted CCLC transaction identity changed');
      setTransaction(next);
      if (next.terminalResult !== null && !next.reconciliationRequired) acceptedActivation.current = null;
      if (next.terminalResult !== null) await readCurrent();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'CCLC reconciliation failed');
    } finally {
      busyRef.current = false;
      setBusy(false);
    }
  }, [pendingTransaction, readCurrent]);

  // Keep presenting the owner's current target progress and readiness without
  // an operator acknowledgement. This only re-reads CCLC; it decides nothing.
  const transactionOpen = pendingTransaction !== null && (transaction?.terminalResult == null || transaction.reconciliationRequired);
  const observing = enabled && JSON.parse(configuredKey).length > 0 && !busy && !transactionOpen;
  useEffect(() => {
    return () => activationAbort.current?.abort(new Error('Activation stopped: configured components changed.'));
  }, [configuredKey]);
  useEffect(() => {
    if (!observing) return undefined;
    let active = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;
    const controller = new AbortController();
    passiveAbort.current = controller;
    const observe = async () => {
      for (const id of JSON.parse(configuredKey) as string[]) {
        if (!active || activationAbort.current !== null) return;
        try {
          await readCurrent(controller.signal, id, id === selectedRef.current);
        } catch (reason) {
          if (!active || controller.signal.aborted) return;
          // The addressed reader invalidates only its own current row.
          void reason;
        }
      }
      if (active) timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    };
    timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    return () => {
      active = false;
      controller.abort();
      if (passiveAbort.current === controller) passiveAbort.current = null;
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [configuredKey, observing, readCurrent]);

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
        if (next.transactionId !== pendingTransaction.id) throw new Error('Accepted CCLC transaction identity changed');
        const accepted = acceptedActivation.current;
        if (accepted !== null) {
          const owner = await api.cclc.owner();
          if (!active) return;
          if (owner.requestOwnerEpoch !== accepted.request.request_context.request_owner_epoch
            || next.transactionId !== accepted.transactionId) throw new Error('Accepted activation owner or transaction changed; outcome remains unknown.');
          if (next.terminalResult !== null && !next.reconciliationRequired) acceptedActivation.current = null;
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
    controllableComponentId,
    snapshot,
    routeTemplates,
    transaction,
    pendingTransaction,
    pendingActivationRequest,
    busy,
    readUnavailableReason,
    error,
    setControllableComponentId,
    refresh,
    reconcileTransaction,
    dismissError: () => setError(null),
    submitLifecycle,
    activate: async () => { await runActivation(false); },
    activateSequence: (expectedIdentity) => runActivation(true, expectedIdentity),
    deactivateComponents,
    readActivationSequence,
    cancelActivation: () => activationAbort.current?.abort(new Error('Activation sequence cancelled; accepted work remains observable.')),
    activationProgress,
    componentHealth: Object.fromEntries((JSON.parse(configuredKey) as string[]).map((id) => [id,
      componentHealth[id] ?? { snapshot: null, error: 'Current component health has not been read.' }])),
    activationUnresolved: activationRequest.current !== null || acceptedActivation.current !== null || transactionOpen,
    activationRefusal,
    submitRoute,
  };
}
