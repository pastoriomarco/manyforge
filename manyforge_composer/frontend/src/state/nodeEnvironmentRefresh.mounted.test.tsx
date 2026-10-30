import { act, createElement, useCallback, useEffect, useRef } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { api, apiErrorCode } from '../api/client';
import type { ControlSemanticStatus, ControlTransportStatus } from '../hooks/useHeadlessState';
import type { ComposerHeadlessState } from '../types/headless';
import { useStructuralResource } from './structuralResource';
import {
  isTransientManagerReadinessErrorCode,
  managerActivationSettlementKey,
  managerReadinessTransientRetryKey,
  nodeEnvironmentRefreshKey,
  refreshOnManagerActivationSettled,
  refreshTransientResourcesOnManagerReadiness,
  type ReadinessTransientRefreshAttempts,
} from './nodeEnvironmentRefresh';

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

interface PendingFetch {
  url: string;
  response: Deferred<Response>;
}

const pendingFetches: PendingFetch[] = [];
let latestStatuses = { catalog: 'unmounted', nodeEnvironment: 'unmounted' };
let root: { render: (children: unknown) => void; unmount: () => void } | null = null;

const isTransientReadError = (error: unknown) => (
  isTransientManagerReadinessErrorCode(apiErrorCode(error))
);

function managerSnapshot(
  bundleId: string,
  applyId: string,
  applyGeneration: number,
  incomplete: boolean,
): ComposerHeadlessState {
  const activeBundleId = incomplete ? 'sha256:old' : bundleId;
  const activeApplyId = incomplete ? 'apply.old' : applyId;
  const activeApplyGeneration = 1;
  const appliedRecord = {
    incomplete: false,
    apply_id: activeApplyId,
    apply_generation: activeApplyGeneration,
    committed: true,
    terminal_state: 'succeeded',
    prepared_activation: {
      retained_bundle_id: activeBundleId,
      graph_revision: 'sha256:graph',
    },
  };
  const records = [appliedRecord];
  if (incomplete) {
    records.push({
      incomplete: true,
      apply_id: applyId,
      apply_generation: applyGeneration,
      committed: false,
      terminal_state: 'unknown_effect',
      prepared_activation: {
        retained_bundle_id: bundleId,
        graph_revision: 'sha256:graph',
      },
    });
  } else if (applyId !== activeApplyId || applyGeneration !== activeApplyGeneration) {
    records.push({
      incomplete: false,
      apply_id: applyId,
      apply_generation: applyGeneration,
      committed: true,
      terminal_state: 'succeeded',
      prepared_activation: {
        retained_bundle_id: bundleId,
        graph_revision: 'sha256:graph',
      },
    });
  }
  return {
    receiverEpoch: 'receiver.1',
    runtime: {
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          sourceGeneration: 1,
          sourceOwnerEpoch: 'manager.1',
          sourceOwnerId: 'component-manager.cell',
          payload: {
            active: {
              retained_bundle_id: activeBundleId,
              graph_revision: 'sha256:graph',
              publication: { transaction: {
                kind: 'named_apply',
                apply_id: activeApplyId,
                apply_generation: activeApplyGeneration,
              } },
            },
            cell_id: 'cell',
            manager_ready: !incomplete,
            runtime: { converged: !incomplete },
            apply: { records },
          },
        },
      },
    },
    componentOverview: {
      maintenanceOperation: null,
      readiness: 'ready',
      capabilities: [],
      components: [],
    },
  } as unknown as ComposerHeadlessState;
}

function Host({
  snapshot,
  transportStatus = 'connected',
  semanticStatus = 'current',
}: {
  snapshot: ComposerHeadlessState;
  transportStatus?: ControlTransportStatus;
  semanticStatus?: ControlSemanticStatus;
}) {
  const [catalog, retryCatalog] = useStructuralResource(api.catalog.list, {
    failureMessage: 'Could not read the test catalog.',
    retryableWhen: isTransientReadError,
  });
  const [nodeEnvironment, retryNodeEnvironment] = useStructuralResource(
    api.nodes.environment,
    {
      failureMessage: 'Could not read the test node environment.',
      retryableWhen: isTransientReadError,
    },
  );
  const catalogLatestRequestId = useRef(0);
  const retryCatalogTracked = useCallback(() => {
    const requestId = retryCatalog();
    catalogLatestRequestId.current = requestId;
    return requestId;
  }, [retryCatalog]);
  const nodeEnvironmentLatestRequestId = useRef(0);
  const retryNodeEnvironmentTracked = useCallback(() => {
    const requestId = retryNodeEnvironment();
    nodeEnvironmentLatestRequestId.current = requestId;
    return requestId;
  }, [retryNodeEnvironment]);
  const environmentWakeKey = useRef<string | null>(null);
  const activationWakeKey = useRef<string | null>(null);
  const refreshDeployment = useCallback(() => undefined, []);
  const attempts = useRef<ReadinessTransientRefreshAttempts>({
    catalog: new Set(),
    nodeEnvironment: new Set(),
  });

  useEffect(() => {
    const key = nodeEnvironmentRefreshKey(snapshot);
    if (environmentWakeKey.current === null) {
      environmentWakeKey.current = key;
      return;
    }
    if (environmentWakeKey.current === key) return;
    environmentWakeKey.current = key;
    retryNodeEnvironmentTracked();
  }, [retryNodeEnvironmentTracked, snapshot]);

  useEffect(() => {
    activationWakeKey.current = refreshOnManagerActivationSettled(
      activationWakeKey.current,
      managerActivationSettlementKey(snapshot),
      [retryCatalogTracked, refreshDeployment],
    );
  }, [refreshDeployment, retryCatalogTracked, snapshot]);

  useEffect(() => {
    attempts.current = refreshTransientResourcesOnManagerReadiness(
      attempts.current,
      managerReadinessTransientRetryKey(snapshot, { transportStatus, semanticStatus }),
      {
        failureRequestId: catalog.status === 'error' ? catalog.requestId ?? null : null,
        latestRequestId: catalogLatestRequestId.current,
        retryable: catalog.status === 'error' && catalog.retryable === true,
        refresh: retryCatalogTracked,
      },
      {
        failureRequestId: nodeEnvironment.status === 'error'
          ? nodeEnvironment.requestId ?? null
          : null,
        latestRequestId: nodeEnvironmentLatestRequestId.current,
        retryable: nodeEnvironment.status === 'error' && nodeEnvironment.retryable === true,
        refresh: retryNodeEnvironmentTracked,
      },
    );
  }, [catalog, nodeEnvironment, retryCatalogTracked, retryNodeEnvironmentTracked, semanticStatus, snapshot, transportStatus]);

  latestStatuses = { catalog: catalog.status, nodeEnvironment: nodeEnvironment.status };
  return null;
}

function installDom(): Element {
  let document: Record<string, unknown>;
  const node = () => ({
    nodeType: 1,
    nodeName: 'DIV',
    tagName: 'DIV',
    namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document,
    style: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    appendChild: () => undefined,
    insertBefore: () => undefined,
    removeChild: () => undefined,
    setAttribute: () => undefined,
    removeAttribute: () => undefined,
  });
  document = {
    nodeType: 9,
    activeElement: null,
    defaultView: {},
    visibilityState: 'visible',
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    createElement: node,
  };
  document.documentElement = node();
  const window = {
    document,
    location: { href: 'http://composer.test/' },
    HTMLIFrameElement: class {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout.bind(globalThis),
    clearTimeout: globalThis.clearTimeout.bind(globalThis),
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return node() as unknown as Element;
}

async function render(
  snapshot: ComposerHeadlessState,
  transportStatus: ControlTransportStatus = 'connected',
  semanticStatus: ControlSemanticStatus = 'current',
): Promise<void> {
  await act(async () => { root?.render(createElement(Host, { snapshot, transportStatus, semanticStatus })); });
  await act(async () => { await Promise.resolve(); await Promise.resolve(); });
}

function resolveFailure(index: number, code: string): void {
  pendingFetches[index]!.response.resolve({
    ok: false,
    status: 503,
    statusText: 'Service Unavailable',
    json: async () => ({ detail: {
      code,
      message: code,
      details: {},
      producer: 'composer',
    } }),
  } as Response);
}

function resolveCatalog(request: PendingFetch): void {
  request.response.resolve({
    ok: true,
    status: 200,
    statusText: 'OK',
    json: async () => ({
      apiVersion: 'manyforge.node_catalog.v2',
      contentVersion: 'test',
      catalogId: 'test',
      entries: [],
      catalogDigest: 'sha256:catalog',
      environmentDigest: 'sha256:environment',
    }),
  } as Response);
}

function resolveEnvironment(request: PendingFetch): void {
  request.response.resolve({
    ok: true,
    status: 200,
    statusText: 'OK',
    json: async () => ({
      environmentDigest: 'sha256:environment',
      catalog: {},
      executableRegistry: [],
      executablePackages: [],
      sourceCoverage: [],
    }),
  } as Response);
}

function requestAt(url: string, occurrence: number): PendingFetch {
  return pendingFetches.filter((request) => request.url === url)[occurrence]!;
}

async function flush(): Promise<void> {
  await act(async () => { await Promise.resolve(); await Promise.resolve(); });
}

beforeEach(async () => {
  pendingFetches.length = 0;
  latestStatuses = { catalog: 'unmounted', nodeEnvironment: 'unmounted' };
  const host = installDom();
  vi.stubGlobal('fetch', vi.fn((input: RequestInfo | URL) => {
    const response = deferred<Response>();
    pendingFetches.push({ url: String(input), response });
    return response.promise;
  }));
  const { createRoot } = await import('react-dom/client');
  root = createRoot(host) as unknown as typeof root;
});

afterEach(async () => {
  if (root !== null) await act(async () => root?.unmount());
  root = null;
  vi.unstubAllGlobals();
});

describe('settled catalog/environment refresh with late transient failures', () => {
  it('keeps each settlement request separate and makes one follow-up fetch per resource', async () => {
    const pending = managerSnapshot('sha256:new', 'apply.new', 1, true);
    const settled = managerSnapshot('sha256:new', 'apply.new', 1, false);
    await render(pending);
    expect(pendingFetches.map((request) => request.url)).toEqual([
      '/api/catalog', '/api/nodes/environment',
    ]);

    resolveFailure(0, 'program_applied_selection_not_active');
    resolveFailure(1, 'program_applied_selection_not_active');
    await flush();
    expect(latestStatuses).toEqual({ catalog: 'error', nodeEnvironment: 'error' });

    await render(settled);
    expect(pendingFetches.filter((request) => request.url === '/api/catalog')).toHaveLength(2);
    expect(pendingFetches.filter((request) => request.url === '/api/nodes/environment')).toHaveLength(2);

    // These failures belong to the just-started settlement reads, not the old
    // errors observed in that same render.
    resolveFailure(2, 'headless_http_timeout');
    await flush();
    expect(pendingFetches.filter((request) => request.url === '/api/catalog')).toHaveLength(3);

    resolveFailure(3, 'headless_http_timeout');
    await flush();
    expect(pendingFetches.filter((request) => request.url === '/api/nodes/environment')).toHaveLength(3);

    const catalogFollowup = requestAt('/api/catalog', 2);
    const environmentFollowup = requestAt('/api/nodes/environment', 2);
    await act(async () => {
      resolveCatalog(catalogFollowup);
      resolveEnvironment(environmentFollowup);
      await Promise.resolve();
      await Promise.resolve();
    });
    await flush();
    expect(latestStatuses).toEqual({ catalog: 'ready', nodeEnvironment: 'ready' });
    expect(pendingFetches).toHaveLength(6);
    expect(pendingFetches.map((request) => request.url)).toEqual([
      '/api/catalog', '/api/nodes/environment',
      '/api/catalog', '/api/nodes/environment',
      '/api/catalog', '/api/nodes/environment',
    ]);
  });

  it('waits for connected, semantically current control before spending the retry', async () => {
    const settled = managerSnapshot('sha256:new', 'apply.new', 1, false);
    await render(settled, 'disconnected', 'current');
    expect(pendingFetches).toHaveLength(2);

    resolveFailure(0, 'program_applied_selection_not_active');
    resolveFailure(1, 'program_applied_selection_not_active');
    await flush();
    expect(latestStatuses).toEqual({ catalog: 'error', nodeEnvironment: 'error' });

    await render(settled, 'stale', 'current');
    await render(settled, 'connected', 'synchronizing');
    expect(pendingFetches).toHaveLength(2);

    await render(settled, 'connected', 'current');
    expect(pendingFetches.map((request) => request.url)).toEqual([
      '/api/catalog', '/api/nodes/environment',
      '/api/catalog', '/api/nodes/environment',
    ]);

    const catalogFollowup = requestAt('/api/catalog', 1);
    const environmentFollowup = requestAt('/api/nodes/environment', 1);
    await act(async () => {
      resolveCatalog(catalogFollowup);
      resolveEnvironment(environmentFollowup);
      await Promise.resolve();
      await Promise.resolve();
    });
    await flush();
    expect(latestStatuses).toEqual({ catalog: 'ready', nodeEnvironment: 'ready' });
    expect(pendingFetches).toHaveLength(4);
  });
});
