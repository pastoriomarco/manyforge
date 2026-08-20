import { useCallback, useEffect, useState } from 'react';

import { api } from '../api/client';
import type {
  CCLCComponentSnapshot,
  CCLCLifecycleIntent,
  CCLCLifecycleRequestBody,
  CCLCRequestContext,
  CCLCRouteRequestBody,
  CCLCRouteTemplate,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';
import type { ExecutionOwnerStatus } from '../types/robotControl';

const POLL_INTERVAL_MS = 250;
let fallbackIdentitySequence = 0;

export interface CCLCController {
  componentId: string;
  snapshot: CCLCComponentSnapshot | null;
  routeTemplates: CCLCRouteTemplateSet | null;
  transaction: CCLCTransactionStatus | null;
  pendingTransaction: { kind: 'lifecycle' | 'route'; id: string } | null;
  confirmed: boolean;
  busy: boolean;
  error: string | null;
  setComponentId: (value: string) => void;
  setConfirmed: (value: boolean) => void;
  refresh: () => Promise<void>;
  submitLifecycle: (intent: CCLCLifecycleIntent) => Promise<void>;
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

function requestContext(
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
  snapshot: CCLCComponentSnapshot,
  execution: ExecutionOwnerStatus,
): string {
  if (
    execution.graphRevision !== snapshot.graphRevision
    || execution.retainedBundleId !== snapshot.retainedBundleId
  ) {
    throw new Error('CCLC component and execution-owner projections are torn');
  }
  return execution.ownerEpoch;
}

export function lifecycleSupportsRouteSelection(
  state: CCLCComponentSnapshot['lifecycleState'],
): boolean {
  return state === 'prepared' || state === 'active';
}

export function lifecycleRequestFromSnapshot(
  snapshot: CCLCComponentSnapshot,
  intent: CCLCLifecycleIntent,
  context: CCLCRequestContext,
): CCLCLifecycleRequestBody {
  return {
    request_context: context,
    controllable_component_id: snapshot.componentId,
    intent,
    expected_graph_revision: snapshot.graphRevision,
    expected_component_lifecycle_generation: snapshot.lifecycleGeneration,
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
): CCLCRouteRequestBody {
  if (!templates.templates.includes(template)) {
    throw new Error('route template is not from the current projection');
  }
  return {
    request_context: context,
    expected_retained_bundle_id: templates.retainedBundleId,
    expected_graph_revision: templates.graphRevision,
    desired_configuration: template.desired_configuration,
    expected_active_control_configuration:
      template.expected_active_control_configuration,
    component_expectations: template.component_expectations,
    confirmed: true,
  };
}

export function useCCLC(enabled: boolean): CCLCController {
  const [componentId, setComponentIdValue] = useState('');
  const [snapshot, setSnapshot] = useState<CCLCComponentSnapshot | null>(null);
  const [routeTemplates, setRouteTemplates] = useState<CCLCRouteTemplateSet | null>(null);
  const [transaction, setTransaction] = useState<CCLCTransactionStatus | null>(null);
  const [pendingTransaction, setPendingTransaction] = useState<{
    kind: 'lifecycle' | 'route'; id: string;
  } | null>(null);
  const [confirmed, setConfirmed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const setComponentId = useCallback((value: string) => {
    setComponentIdValue(value);
    setSnapshot(null);
    setRouteTemplates(null);
    setConfirmed(false);
  }, []);

  const readCurrent = useCallback(async (signal?: AbortSignal) => {
    if (!enabled) throw new Error('CCLC client is unavailable');
    const identity = componentId.trim();
    if (!identity) throw new Error('Enter a component identity first');
    const [nextSnapshot, execution] = await Promise.all([
      api.cclc.component(identity, signal),
      api.execution.status(signal),
    ]);
    const ownerEpoch = cclcRequestOwnerEpoch(nextSnapshot, execution);
    let nextTemplates: CCLCRouteTemplateSet = {
      componentId: nextSnapshot.componentId,
      retainedBundleId: nextSnapshot.retainedBundleId,
      graphRevision: nextSnapshot.graphRevision,
      templates: [],
    };
    if (lifecycleSupportsRouteSelection(nextSnapshot.lifecycleState)) {
      nextTemplates = await api.cclc.routeTemplates(identity, signal);
      if (
        nextSnapshot.componentId !== nextTemplates.componentId
        || nextSnapshot.retainedBundleId !== nextTemplates.retainedBundleId
        || nextSnapshot.graphRevision !== nextTemplates.graphRevision
      ) {
        throw new Error('CCLC component and route projections are torn');
      }
    }
    setSnapshot(nextSnapshot);
    setRouteTemplates(nextTemplates);
    return { snapshot: nextSnapshot, templates: nextTemplates, ownerEpoch };
  }, [componentId, enabled]);

  const refresh = useCallback(async () => {
    setBusy(true);
    setError(null);
    try {
      await readCurrent();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'CCLC refresh failed');
    } finally {
      setBusy(false);
    }
  }, [readCurrent]);

  const requireConfirmation = useCallback(() => {
    if (!confirmed) throw new Error('Explicit confirmation is required');
  }, [confirmed]);

  const submitLifecycle = useCallback(async (intent: CCLCLifecycleIntent) => {
    setBusy(true);
    setError(null);
    try {
      requireConfirmation();
      const current = await readCurrent();
      const result = await api.cclc.submitLifecycle(
        lifecycleRequestFromSnapshot(
          current.snapshot,
          intent,
          requestContext(current.ownerEpoch, `lifecycle:${intent}`),
        ),
      );
      if (result.transactionId === null) {
        throw new Error(`CCLC lifecycle request was ${result.replayStatus}`);
      }
      setTransaction(result.status);
      setPendingTransaction({ kind: 'lifecycle', id: result.transactionId });
      setConfirmed(false);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Lifecycle request failed');
    } finally {
      setBusy(false);
    }
  }, [readCurrent, requireConfirmation]);

  const submitRoute = useCallback(async (template: CCLCRouteTemplate) => {
    setBusy(true);
    setError(null);
    try {
      requireConfirmation();
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
        ),
      );
      if (result.transactionId === null) {
        throw new Error(`CCLC route request was ${result.replayStatus}`);
      }
      setTransaction(result.status);
      setPendingTransaction({ kind: 'route', id: result.transactionId });
      setConfirmed(false);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Route request failed');
    } finally {
      setBusy(false);
    }
  }, [readCurrent, requireConfirmation]);

  useEffect(() => {
    if (!enabled || pendingTransaction === null || transaction?.terminalResult) {
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
  }, [enabled, pendingTransaction, transaction?.terminalResult]);

  return {
    componentId,
    snapshot,
    routeTemplates,
    transaction,
    pendingTransaction,
    confirmed,
    busy,
    error,
    setComponentId,
    setConfirmed,
    refresh,
    submitLifecycle,
    submitRoute,
  };
}
