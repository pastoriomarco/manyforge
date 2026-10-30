import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { ViewerSceneEmptyState } from '../components/StructuralViewer';
import type {
  SceneUrdfManifest,
  SourceModelProjection,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { StructuralResourceState } from './structuralResource';
import {
  verifiedAppliedGraphHasNoModels,
  verifiedNoPhysicalComponents,
  viewerModelPresentation,
} from './viewerModelReadiness';

const CELL_ID = 'cell-a';
const BUNDLE_ID = 'sha256:bundle-a';
const GRAPH_REVISION = 'sha256:graph-a';

function snapshot(overrides: {
  available?: boolean;
  freshness?: 'fresh' | 'stale' | 'unavailable';
  managerReady?: boolean;
  converged?: boolean;
  activeBundleId?: string | null;
  activeGraphRevision?: string | null;
  applyRecords?: Array<{ incomplete: boolean }>;
} = {}): ComposerHeadlessState {
  return {
    authoringWorkspaceContainment: null,
    receiverEpoch: 'receiver.1',
    runtime: {
      complete: true,
      sections: {
        manager: {
          available: overrides.available ?? true,
          freshness: overrides.freshness ?? 'fresh',
          knownGaps: [],
          sourceGeneration: 2,
          sourceOwnerEpoch: 'manager.1',
          sourceOwnerId: 'manager-cell',
          receiverAgeNs: 0,
          payload: {
            active: {
              retained_bundle_id: overrides.activeBundleId === undefined
                ? BUNDLE_ID : overrides.activeBundleId,
              graph_revision: overrides.activeGraphRevision === undefined
                ? GRAPH_REVISION : overrides.activeGraphRevision,
            },
            apply: {
              records: overrides.applyRecords ?? [],
            },
            cell_id: CELL_ID,
            manager_ready: overrides.managerReady ?? true,
            runtime: { converged: overrides.converged ?? true },
          },
        },
        logicalScene: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'scene.1',
          sourceOwnerId: 'scene-cell',
          receiverAgeNs: 0,
        },
        sceneRuntime: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'scene-runtime.1',
          sourceOwnerId: 'scene-runtime-cell',
          receiverAgeNs: 0,
        },
      },
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      activeConfiguration: null,
      projectionGeneration: 4,
      projectionOwnerEpoch: 'headless.1',
      projectionOwnerId: 'headless-api',
      reasons: [],
      unavailableSections: [],
    },
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    alarms: {} as ComposerHeadlessState['alarms'],
    componentOverview: {} as ComposerHeadlessState['componentOverview'],
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 1 },
    heldRunDraftEditAllowed: false,
    executionControlStatus: null,
    programCycle: {} as ComposerHeadlessState['programCycle'],
    authoringWorkspace: {} as ComposerHeadlessState['authoringWorkspace'],
    snapshotSequence: 1,
  };
}

function sourceModel(
  retainedBundleId = BUNDLE_ID,
  graphRevision = GRAPH_REVISION,
): StructuralResourceState<SourceModelProjection> {
  return {
    status: 'ready',
    data: {
      cell_id: CELL_ID,
      retained_bundle_id: retainedBundleId,
      graph_revision: graphRevision,
    } as SourceModelProjection,
  };
}

function manifest(
  retainedBundleId = BUNDLE_ID,
  graphRevision = GRAPH_REVISION,
): StructuralResourceState<SceneUrdfManifest> {
  return {
    status: 'ready',
    data: {
      cell_id: CELL_ID,
      retained_bundle_id: retainedBundleId,
      graph_revision: graphRevision,
      model_key: 'model-a',
    } as SceneUrdfManifest,
  };
}

function select(
  state = snapshot(),
  source = sourceModel(),
  urdf = manifest(),
  acceptedPending = false,
) {
  return viewerModelPresentation(
    state,
    'connected',
    'current',
    acceptedPending,
    source,
    urdf,
  );
}

function emptySnapshot(canonicalEmpty = true): ComposerHeadlessState {
  const value = snapshot();
  value.runtime.activeConfiguration = { retainedBundleId: BUNDLE_ID, graphRevision: GRAPH_REVISION };
  const active = value.runtime.sections.manager.payload!.active as Record<string, unknown>;
  active.canonical_empty = canonicalEmpty;
  active.publication = { publication_generation: 1,
    projection_revision: { retained_bundle_id: BUNDLE_ID, graph_revision: GRAPH_REVISION } };
  active.cell_graph = {
    cell_id: CELL_ID, graph_revision: GRAPH_REVISION,
    model_instances: [], controllable_components: [], control_bindings: [], control_domains: [],
    // Canonical empty retains its execution-host infrastructure unit.
    runtime_units: [{ runtime_unit_id: 'robot_cell.ros2_control_execution' }],
  };
  return value;
}

describe('verifiedNoPhysicalComponents', () => {
  it.each([true, false])('uses positive empty facts without command admission, canonical=%s', (canonicalEmpty) => {
    const value = emptySnapshot(canonicalEmpty);
    value.executionControlStatus = null;
    expect(verifiedNoPhysicalComponents(value, 'connected')).toBe(true);
    expect(verifiedNoPhysicalComponents(value, 'stale')).toBe(false);
    expect(verifiedNoPhysicalComponents(value, 'disconnected')).toBe(false);
    expect(verifiedNoPhysicalComponents(null, 'connected')).toBe(false);
  });

  it.each([true, false])('never infers absence from invalid or nonempty facts, canonical=%s', (canonicalEmpty) => {
    const changes: Array<(value: ComposerHeadlessState) => void> = [
      (value) => { value.runtime.complete = false; },
      (value) => { value.runtime.activeConfiguration = null; },
      (value) => { value.runtime.activeConfiguration!.graphRevision = 'other'; },
      (value) => { value.runtime.sections.manager.available = false; },
      (value) => { value.runtime.sections.manager.freshness = 'stale'; },
      (value) => { value.runtime.sections.manager.knownGaps = ['lost_update']; },
      (value) => { value.runtime.sections.manager.sourceGeneration = null; },
      (value) => { value.runtime.sections.manager.sourceOwnerEpoch = null; },
      (value) => { value.runtime.sections.manager.payload = null; },
      (value) => { value.runtime.sections.manager.payload!.manager_ready = false; },
      (value) => { value.runtime.sections.manager.payload!.runtime = { converged: false }; },
      (value) => { value.runtime.sections.manager.payload!.apply = { records: [{ incomplete: true }] }; },
      (value) => { value.runtime.sections.manager.payload!.apply = {}; },
      (value) => { (value.runtime.sections.manager.payload!.active as Record<string, unknown>).canonical_empty = 'false'; },
      (value) => { delete (value.runtime.sections.manager.payload!.active as Record<string, unknown>).canonical_empty; },
      (value) => { (value.runtime.sections.manager.payload!.active as Record<string, unknown>).publication = {}; },
      (value) => { value.runtime.sections.manager.payload!.cell_id = 'other'; },
    ];
    for (const key of ['model_instances', 'controllable_components', 'control_bindings', 'control_domains']) {
      changes.push((value) => {
        const active = value.runtime.sections.manager.payload!.active as Record<string, unknown>;
        (active.cell_graph as Record<string, unknown>)[key] = [{}];
      });
      changes.push((value) => {
        const active = value.runtime.sections.manager.payload!.active as Record<string, unknown>;
        delete (active.cell_graph as Record<string, unknown>)[key];
      });
    }
    for (const change of changes) {
      const value = emptySnapshot(canonicalEmpty);
      change(value);
      expect(verifiedNoPhysicalComponents(value, 'connected')).toBe(false);
    }
  });
});

function renderVerifiedEmptyState(value: ComposerHeadlessState, manifestReady: boolean): string {
  return renderToStaticMarkup(createElement(ViewerSceneEmptyState, {
    noRobotScenario: verifiedAppliedGraphHasNoModels(value, 'connected'),
    visibleGeometryCount: 0,
    pointCloudEnabled: false,
    manifestReady,
    collisionCount: 0,
  }));
}

describe('verified Applied graph no-model presentation', () => {
  it('shows the neutral empty-Scenario label for an identity-verified empty graph', () => {
    const value = emptySnapshot();
    expect(verifiedAppliedGraphHasNoModels(value, 'connected')).toBe(true);
    const markup = renderVerifiedEmptyState(value, false);
    expect(markup).toContain('No robot in this Scenario');
    expect(markup).toContain('role="status"');
    expect(markup).not.toContain('No geometry selected for display');
  });

  it('recognizes an I/O-only graph with no models without weakening the physical-component proof', () => {
    const value = emptySnapshot(false);
    const active = value.runtime.sections.manager.payload!.active as Record<string, unknown>;
    const graph = active.cell_graph as Record<string, unknown>;
    graph.controllable_components = [{ controllable_component_id: 'io' }];
    graph.control_bindings = [{ binding_id: 'io.binding' }];
    graph.control_domains = [{ control_domain_id: 'io.domain' }];
    graph.runtime_units = [{ runtime_unit_id: 'io.runtime' }];

    expect(verifiedAppliedGraphHasNoModels(value, 'connected')).toBe(true);
    expect(verifiedNoPhysicalComponents(value, 'connected')).toBe(false);
    const markup = renderVerifiedEmptyState(value, false);
    expect(markup).toContain('No robot in this Scenario');
    expect(markup).not.toContain('No geometry selected for display');
  });

  it('keeps technical no-geometry guidance when the verified Applied graph has a model', () => {
    const value = emptySnapshot(false);
    const active = value.runtime.sections.manager.payload!.active as Record<string, unknown>;
    const graph = active.cell_graph as Record<string, unknown>;
    graph.model_instances = [{ model_instance_id: 'robot.model' }];

    expect(verifiedAppliedGraphHasNoModels(value, 'connected')).toBe(false);
    const markup = renderVerifiedEmptyState(value, true);
    expect(markup).toContain('No geometry selected for display');
    expect(markup).toContain('The retained link tree contains no renderable visual or collision geometry.');
    expect(markup).not.toContain('No robot in this Scenario');
  });

  it('requires a connected, current Manager publication and matching Applied identity', () => {
    const value = emptySnapshot();
    expect(verifiedAppliedGraphHasNoModels(value, 'stale')).toBe(false);
    expect(verifiedAppliedGraphHasNoModels(value, 'disconnected')).toBe(false);

    const wrongAppliedIdentity = emptySnapshot();
    wrongAppliedIdentity.runtime.activeConfiguration!.graphRevision = 'other.graph';
    expect(verifiedAppliedGraphHasNoModels(wrongAppliedIdentity, 'connected')).toBe(false);

    const incompleteApply = emptySnapshot();
    incompleteApply.runtime.sections.manager.payload!.apply = { records: [{ incomplete: true }] };
    expect(verifiedAppliedGraphHasNoModels(incompleteApply, 'connected')).toBe(false);
  });
});

describe('viewerModelPresentation', () => {
  it('admits only an exact ready Manager, source-model, and URDF identity', () => {
    const loaded = manifest();
    if (loaded.status !== 'ready') throw new Error('fixture manifest is not ready');
    expect(select()).toEqual({ status: 'ready', manifest: loaded.data });
  });

  it('hides cached robot models while a Scenario Load or Manager Apply is pending', () => {
    expect(select(snapshot(), sourceModel(), manifest(), true)).toMatchObject({
      status: 'synchronizing',
      reason: 'transition',
    });
    expect(select(snapshot({ applyRecords: [{ incomplete: true }] }))).toMatchObject({
      status: 'synchronizing',
      reason: 'transition',
    });
  });

  it('does not render the previous two-robot model after Manager selects a single-robot identity', () => {
    const result = select(
      snapshot({
        activeBundleId: 'sha256:single-robot-bundle',
        activeGraphRevision: 'sha256:single-robot-graph',
      }),
      sourceModel('sha256:dual-robot-bundle', 'sha256:dual-robot-graph'),
      manifest('sha256:dual-robot-bundle', 'sha256:dual-robot-graph'),
    );
    // Outside a transition this mismatch is an error (viewerNotices).
    expect(result).toMatchObject({ status: 'synchronizing', reason: 'identity_mismatch' });
    expect(result).not.toHaveProperty('manifest');
  });

  it('fails closed when Manager is not ready, converged, fresh, or available', () => {
    expect(select(snapshot({ managerReady: false }))).toMatchObject({
      status: 'unavailable',
    });
    expect(select(snapshot({ converged: false }))).toMatchObject({
      status: 'unavailable',
    });
    expect(select(snapshot({ freshness: 'stale' }))).toMatchObject({
      status: 'synchronizing',
      reason: 'projection_stale',
    });
    expect(select(snapshot({ available: false }))).toMatchObject({
      status: 'unavailable',
    });
  });

  it('fails closed when source or manifest identity cannot be compared', () => {
    expect(select(snapshot(), sourceModel('sha256:other'), manifest())).toMatchObject({
      status: 'synchronizing',
    });
    expect(select(snapshot(), sourceModel(), {
      status: 'error', message: 'manifest fetch failed',
    })).toMatchObject({ status: 'unavailable' });
  });

  it('hides the retained model when the live Manager control projection disconnects', () => {
    const result = viewerModelPresentation(
      snapshot(),
      'disconnected',
      'synchronizing',
      false,
      sourceModel(),
      manifest(),
    );
    expect(result).toMatchObject({ status: 'synchronizing' });
    expect(result).not.toHaveProperty('manifest');
  });

  it.each([
    ['delayed', 'transport_delayed'],
    ['stale', 'transport_stale'],
  ] as const)(
    'retains the verified model, marked not current, while transport is %s',
    (transport, reason) => {
      const loaded = manifest();
      if (loaded.status !== 'ready') throw new Error('fixture manifest is not ready');
      const result = viewerModelPresentation(
        snapshot(), transport, 'current', false, sourceModel(), loaded,
      );
      expect(result).toMatchObject({ status: 'retained', reason });
      // The same manifest object stays mounted: no rebuild on recovery.
      expect(result.status === 'retained' && result.manifest).toBe(loaded.data);
      expect(result.status === 'retained' && result.message).toContain(
        `Control connection ${transport}`,
      );
    },
  );

  it.each(['connecting', 'disconnected'] as const)(
    'still hides the model for a new or closed session (%s)',
    (transport) => {
      const result = viewerModelPresentation(
        snapshot(), transport, 'current', false, sourceModel(), manifest(),
      );
      expect(result).toMatchObject({ status: 'synchronizing', reason: 'connecting' });
      expect(result).not.toHaveProperty('manifest');
    },
  );

  it('never retains across an identity barrier, transition or stale projection', () => {
    const delayed = (
      state: ComposerHeadlessState | null,
      source = sourceModel(),
      urdf = manifest(),
      acceptedPending = false,
      semantic: 'current' | 'synchronizing' | 'unavailable' = 'current',
    ) => viewerModelPresentation(state, 'delayed', semantic, acceptedPending, source, urdf);
    for (const result of [
      // A Manager-selected identity that the cached model does not match.
      delayed(snapshot({ activeBundleId: 'sha256:bundle-b' })),
      delayed(snapshot(), sourceModel('sha256:bundle-b'), manifest('sha256:bundle-b')),
      // Accepted activation or a pending Manager Apply.
      delayed(snapshot(), sourceModel(), manifest(), true),
      delayed(snapshot({ applyRecords: [{ incomplete: true }] })),
      // Stale Manager projection, semantic synchronization, no snapshot.
      delayed(snapshot({ freshness: 'stale' })),
      delayed(snapshot(), sourceModel(), manifest(), false, 'synchronizing'),
      delayed(null),
      // Unavailable composition.
      delayed(snapshot({ managerReady: false })),
      delayed(snapshot(), sourceModel(), { status: 'error', message: 'gone' }),
    ]) {
      expect(result.status).not.toBe('retained');
      expect(result.status).not.toBe('ready');
      expect(result).not.toHaveProperty('manifest');
    }
    expect(delayed(snapshot({ activeBundleId: 'sha256:bundle-b' })))
      .toMatchObject({ reason: 'identity_mismatch' });
    expect(delayed(snapshot(), sourceModel(), manifest(), true))
      .toMatchObject({ reason: 'transition' });
  });
});
