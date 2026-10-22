import { StrictMode, act } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import type { AuthoringWorkspaceSnapshot, LogicalSceneObject, LogicalSceneProjection, ManagerActiveProjection,
  ProgramWorkspaceResponse, ProjectionRevision, SceneTransactionRequest } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { authoringWorkspaceIdentity } from '../state/authoringWorkspace';
import { managerApplyRecord } from '../test/managerApplyRecord';
import { useAuthoringWorkspace } from '../hooks/useAuthoringWorkspace';
import { useProgramWorkspace } from '../hooks/useProgramWorkspace';
import { ScenarioDocumentCommands } from './ScenarioDocumentCommands';
import { prepareSceneForClose } from '../state/scenarioClose';
import type { ScenarioReplacementIdentity } from '../hooks/useProgramWorkspace';
import type { ProgramResetExpectation, ProgramResetOutcome } from '../hooks/useProgramRuntime';

const apiMock = vi.hoisted(() => ({
  authoringWorkspace: { get: vi.fn() }, programWorkspace: { get: vi.fn(), clearClosedScenario: vi.fn() },
  manager: { status: vi.fn(), applyStatus: vi.fn() }, deploymentWorkspace: { get: vi.fn() },
  scenarioDocuments: { list: vi.fn(), load: vi.fn(), new: vi.fn() }, files: { browse: vi.fn() },
  scene: { commit: vi.fn() },
}));
vi.mock('../api/client', async (original) => ({
  ...await original<typeof import('../api/client')>(), api: apiMock,
}));

// Real React handlers plus both real authoring hooks. Only owner reads/effects
// are substituted; beginMutation and all local fences run unchanged.
type Props = Record<string, unknown> & { onClick?: (event: unknown) => void; onChange?: (event: unknown) => void };
type Node = Record<string, unknown> & { children: Node[]; parentNode: Node | null };
function host() {
  const document: Record<string, unknown> = { nodeType: 9, activeElement: null,
    addEventListener: () => undefined, removeEventListener: () => undefined };
  const node = (tag: string): Node => {
    const element: Node = { nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
      children: [], parentNode: null, addEventListener: () => undefined, removeEventListener: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined, focus: () => undefined,
      appendChild: (child: Node) => { element.children.push(child); child.parentNode = element; },
      insertBefore: (child: Node, before: Node) => {
        element.children.splice(element.children.indexOf(before), 0, child); child.parentNode = element;
      },
      removeChild: (child: Node) => {
        element.children.splice(element.children.indexOf(child), 1); child.parentNode = null;
      },
    };
    return element;
  };
  document.createElement = node;
  document.createElementNS = (_namespace: string, tag: string) => node(tag);
  document.createTextNode = (text: string) => ({ ...node('text'), nodeType: 3, nodeValue: text });
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {}, requestAnimationFrame: (fn: () => void) => fn() };
  document.defaultView = window;
  vi.stubGlobal('window', window); vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const root = node('div');
  const walk = (value: Node): Node[] => [value, ...value.children.flatMap(walk)];
  const all = () => walk(root).map((value) => value[Object.keys(value).find((key) => key.startsWith('__reactProps$'))!])
    .filter((value): value is Props => value !== undefined);
  const find = (match: (value: Props) => boolean) => {
    const value = all().find(match);
    if (value === undefined) throw new Error('Mounted Scenario control not found');
    return value;
  };
  return { root, all, find };
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
function active(empty: boolean, generation: number): ManagerActiveProjection {
  const bundle = empty ? 'bundle.empty' : 'bundle.current';
  const graph = empty ? 'graph.empty' : 'graph.current';
  return { canonicalEmpty: empty, retainedBundleId: bundle, graphRevision: graph,
    cellGraph: { controllable_components: empty ? [] : [{ controllable_component_id: 'arm' }] },
    publication: { publication_generation: generation, durable_public_pointer_content_digest: `pointer.${generation}`,
      projection_revision: { retained_bundle_id: bundle, graph_revision: graph,
        scene_revision: { scene_id: 'scene', revision_id: generation, content_hash: 'scene.digest' } },
      transaction: generation === 3 ? { kind: 'named_apply', apply_id: 'apply-3', apply_generation: 3 }
        : { kind: 'bootstrap', bootstrap_id: 'bootstrap', bootstrap_generation: generation },
    } };
}
function publication(): AuthoringWorkspaceSnapshot {
  return { schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v2',
    workspaceEpoch: 'workspace.1', workspaceRevision: 1,
    program: { draft: { draftId: 'draft.1', draftRevision: 1, programLoaded: false, authoring: null,
      candidateAvailable: false, programRevision: null, program: null, diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} } },
      saved: { available: false, matchesDraft: false, path: null, programRevision: null } },
    deployment: { draftId: 'deployment.1', draftRevision: 1, document: null, savedPath: null,
      savedDocument: null, differsFromSaved: false },
    project: { documentPath: '/projects/current', documentId: 'current', documentName: 'Current',
      documentDescription: '', sourceDiagnostics: [], sourceDocumentDigest: null,
      programRoleAssignment: null, sourceProgramDigest: null, sourceDeploymentDigest: null },
  };
}
function workspace(published: AuthoringWorkspaceSnapshot): ProgramWorkspaceResponse {
  return { schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: authoringWorkspaceIdentity(published), draft: published.program.draft,
    saved: published.program.saved, applicability: { applyReady: false, unavailableReason: null },
    applied: { available: false, matchesDraft: null, programRevision: null, unavailableReason: null },
    hasUnsavedChanges: published.program.draft.programLoaded && !published.program.saved.matchesDraft, hasUnappliedChanges: false };
}
function sceneRevision(current: ManagerActiveProjection, revision?: number): ProjectionRevision {
  const projection = current.publication.projection_revision as unknown as ProjectionRevision;
  return { ...projection, scene_revision: { ...projection.scene_revision,
    ...(revision === undefined ? {} : { revision_id: revision, content_hash: `scene.digest.${revision}` }) } };
}
function logicalScene(current: ManagerActiveProjection, objects: LogicalSceneObject[] = [], revision?: number): LogicalSceneProjection {
  const projection = sceneRevision(current, revision);
  return { api_version: 'manyforge.logical_scene_source.v1',
    access: { active_projection: projection, phase: 'active', projection_leases_open: true, scene_writes_open: true },
    snapshot: { carried_object_attachments: [], cell_id: 'cell', configured_collision_policy: { policy_id: 'free_space', revision: { revision_id: 1, content_hash: 'policy.1' },
      allowed_static_static_pairs: [], allowed_touch_link_pairs: [], support_contacts: [], contact_grasps: [],
      require_full_diagnostics: true }, objects,
      policy_epoch: 1, revision: projection.scene_revision, topology_epoch: 1,
      transaction_sequence: projection.scene_revision.revision_id } };
}
function fixedObject(id: string, support?: string): LogicalSceneObject {
  return { scene_resource_id: id, collision_shapes: [{ type: 'box', dimensions_m: { x: 1, y: 1, z: 1 } }],
    placement: { kind: 'cell_fixed', cell_id: 'cell', pose_in_cell: {
      position_m: { x: 0, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1] } },
    provider_evidence: null, support_contacts: support === undefined ? [] : [{ support_object_id: support }] };
}
function supportedObjects(): LogicalSceneObject[] {
  return [fixedObject('ground'), fixedObject('wall', 'ground'), fixedObject('graspable', 'wall')];
}
function snapshot(current: ManagerActiveProjection, published: AuthoringWorkspaceSnapshot, pending = false,
  options: { scene?: LogicalSceneProjection; admissionGeneration?: number } = {}): ComposerHeadlessState {
  const scene = options.scene ?? logicalScene(current);
  return { receiverEpoch: 'receiver.1', authoringWorkspace: authoringWorkspaceIdentity(published),
    authoringWorkspaceContainment: null, authoringAdmission: { authoringAllowed: true, admissionGeneration: options.admissionGeneration ?? 8 },
    programCycle: { state: 'idle_after_reset' },
    componentOverview: { readiness: 'ready', userMessage: 'Current', components: [], maintenanceOperation: null, basis: {} },
    runtime: { complete: true, sections: { logicalScene: { available: true, freshness: 'fresh', knownGaps: [],
      sourceOwnerId: 'scene', sourceOwnerEpoch: 'scene.1', sourceGeneration: scene.snapshot!.revision.revision_id, payload: scene },
      manager: { available: true, freshness: 'fresh', knownGaps: [],
      sourceOwnerId: 'manager', sourceOwnerEpoch: 'manager.1', sourceGeneration: 1,
      payload: { active: { canonical_empty: current.canonicalEmpty, retained_bundle_id: current.retainedBundleId,
        graph_revision: current.graphRevision, cell_graph: current.cellGraph, publication: current.publication },
        apply: { mutation_available: !pending, records: pending ? [managerApplyRecord()] : [],
          unavailable_reason_code: pending ? 'manager_apply_in_progress' : null } },
    } } },
  } as unknown as ComposerHeadlessState;
}

let unmount: (() => Promise<void>) | null = null;
afterEach(async () => { await unmount?.(); unmount = null; vi.unstubAllGlobals(); });
beforeEach(() => {
  vi.clearAllMocks();
  apiMock.files.browse.mockResolvedValue({ entries: [] });
  apiMock.scene.commit.mockImplementation(async (request: SceneTransactionRequest) => ({
    request_id: request.request_context.request_id, request_owner_epoch: request.request_context.request_owner_epoch,
    committed_projection_revision: { ...request.expected_projection_revision,
      scene_revision: { ...request.expected_projection_revision.scene_revision,
        revision_id: request.expected_projection_revision.scene_revision.revision_id + 1,
        content_hash: `scene.digest.${request.expected_projection_revision.scene_revision.revision_id + 1}` } },
  }));
  apiMock.scenarioDocuments.list.mockResolvedValue({ directory: '/projects', entries: [
    { sourceKind: 'saved', path: '/projects/other', scenarioId: 'other', name: 'Other', folderName: 'other', location: 'projects', displayStatus: 'ready' },
    { sourceKind: 'saved', path: '/projects/current', scenarioId: 'current', name: 'Current', folderName: 'current', location: 'projects', displayStatus: 'ready' },
  ] });
});
async function mount(empty = true, dirty = false,
  closeOperation?: (expected: ManagerActiveProjection, confirmed?: ScenarioReplacementIdentity, signal?: AbortSignal) => Promise<ManagerActiveProjection | null>) {
  const dom = host();
  const published = publication();
  published.program.draft.programLoaded = dirty;
  const predecessor = active(empty, 1);
  const closed = active(true, 3);
  let current = snapshot(predecessor, published);
  apiMock.authoringWorkspace.get.mockImplementation(async () => published);
  apiMock.programWorkspace.get.mockImplementation(async () => workspace(published));
  apiMock.programWorkspace.clearClosedScenario.mockImplementation(async () => workspace(published));
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active: predecessor } });
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: { scenarioId: 'current',
    activationOrder: { retainedBundleId: predecessor.retainedBundleId, graphRevision: predecessor.graphRevision } } });
  apiMock.scenarioDocuments.load.mockImplementation(async () => ({ status: 'applied', workspace: workspace(published) }));
  apiMock.scenarioDocuments.new.mockImplementation(async () => ({ status: 'applied', workspace: workspace(published) }));
  const close = deferred<ManagerActiveProjection | null>();
  const closeScenario = vi.fn(closeOperation ?? (() => close.promise));
  let showCommands = true;
  function Harness() {
    const authoring = useAuthoringWorkspace(current);
    const controller = useProgramWorkspace(current, authoring);
    return <>
      {showCommands && <ScenarioDocumentCommands controller={controller} closeScenario={closeScenario} />}
      <output aria-label="Program feedback">{controller.state.message}</output>
    </>;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.root as unknown as Element);
  const render = async () => { await act(async () => { root.render(<StrictMode><Harness /></StrictMode>); }); };
  await render();
  unmount = async () => { await act(() => root.unmount()); };
  const click = async (value: Props) => { await act(async () => value.onClick?.({ currentTarget: { focus: () => undefined } })); };
  const openLoad = async (id = 'other') => {
    await click(dom.find((p) => p['aria-label'] === 'Load Scenario'));
    await act(() => dom.find((p) => p.value === '').onChange?.({ target: { value: `saved:/projects/${id}` } }));
    const confirmation = dom.all().find((p) => p.type === 'checkbox');
    if (confirmation !== undefined) await act(() => confirmation.onChange?.({ target: { checked: true } }));
    await click(dom.find((p) => p.className === 'primary-action' && p.children === 'Load Scenario'));
    const ack = dom.all().find((p) => p.children === 'Close and continue');
    if (empty && !dirty) expect(ack).toBeUndefined();
    if (ack !== undefined) await click(ack);
  };
  const observe = async (value: ComposerHeadlessState) => { current = value; await render(); };
  const openClose = async () => {
    await click(dom.find((p) => p['aria-label'] === 'Close Scenario'));
    const acknowledgement = dom.all().find((p) => p.className === 'danger-button' && p.children === 'Close Scenario');
    if (!empty || dirty) {
      expect(closeScenario).not.toHaveBeenCalled();
      expect(acknowledgement).toBeDefined();
    } else expect(acknowledgement).toBeUndefined();
    if (acknowledgement !== undefined) await click(acknowledgement);
  };
  return { ...dom, published, predecessor, closed, close, closeScenario, click, openLoad, openClose, observe,
    get current() { return current; },
    hideCommands: async () => { showCommands = false; await render(); },
  };
}

it.each([true, false])('waits for actual post-Close admission before one Load, empty start=%s', async (empty) => {
  const test = await mount(empty);
  await test.openLoad();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(test.predecessor, expect.objectContaining({
    authoring: authoringWorkspaceIdentity(test.published),
  }), expect.any(AbortSignal));
  await test.observe(snapshot(test.predecessor, test.published, true));
  await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, true));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledWith(expect.objectContaining({
    expectedWorkspaceEpoch: 'workspace.1', expectedWorkspaceRevision: 1, deploymentDraftId: 'deployment.1',
    expectedDeploymentDraftRevision: 1, expectedAdmissionGeneration: 8,
  }), '/projects/other', true);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
});

it.each([true, false])('New uses the exact completed empty identity, empty start=%s', async (empty) => {
  const test = await mount(empty);
  await test.click(test.find((p) => p['aria-label'] === 'New'));
  await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
    .onChange?.({ target: { value: 'next_cell' } }));
  await test.click(test.find((p) => p.className === 'primary-action' && p.children === 'Create Scenario'));
  if (empty) expect(test.all().some((p) => p.children === 'Close and continue')).toBe(false);
  else await test.click(test.find((p) => p.children === 'Close and continue'));
  await test.observe(snapshot(test.predecessor, test.published, true));
  await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.new).toHaveBeenCalledTimes(1);
  expect(apiMock.scenarioDocuments.new).toHaveBeenCalledWith(expect.anything(), expect.objectContaining({
    expectedRetainedBundleId: test.closed.retainedBundleId, expectedGraphRevision: test.closed.graphRevision,
    directory: '/projects', name: 'next_cell', deploymentChoice: 'empty',
  }));
});

it.each(['draft', 'deployment', 'epoch', 'owner', 'publication', 'fault'])('refuses intervening %s without loading', async (change) => {
  const test = await mount(false);
  await test.openLoad();
  await act(async () => { test.close.resolve(test.closed); });
  const next = snapshot(test.closed, test.published);
  if (change === 'draft') next.authoringWorkspace.programDraftRevision += 1;
  if (change === 'deployment') next.authoringWorkspace.deploymentDraftRevision += 1;
  if (change === 'epoch') next.authoringWorkspace.workspaceEpoch = 'another';
  if (change === 'owner') next.runtime.sections.manager.sourceOwnerEpoch = 'manager.2';
  if (change === 'publication') {
    const changed = active(true, 4);
    (next.runtime.sections.manager.payload!.active as Record<string, unknown>).publication = changed.publication;
  }
  if (change === 'fault') {
    next.componentOverview.readiness = 'fault';
    next.componentOverview.userMessage = 'Actual owner fault';
  }
  await test.observe(next);
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.className === 'inline-message severity-error')).toBe(true);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
});

it('does not repeat an unresolved Load when later owner updates arrive', async () => {
  const test = await mount();
  apiMock.scenarioDocuments.load.mockRejectedValueOnce(new Error('Load reply lost'));
  await test.openLoad();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  expect(test.all().some((p) => typeof p.children === 'string' && p.children.includes('nothing was retried'))).toBe(true);
});

it('preserves a post-Close Program workspace read failure without sending Load', async () => {
  const test = await mount(false);
  await test.openLoad();
  await act(async () => { test.close.resolve(test.closed); });
  apiMock.programWorkspace.get.mockRejectedValueOnce(new Error('Applied workspace read failed'));
  await test.observe(snapshot(test.closed, test.published, true));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.children === 'Applied workspace read failed')).toBe(true);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
});

it('does not send a follow-up mutation when the confirmed component unmounts during Close', async () => {
  const test = await mount();
  await test.openLoad();
  await unmount!();
  unmount = null;
  await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
});

it('Load of the applied Scenario closes first exactly like a different Scenario', async () => {
  const test = await mount(false);
  await test.openLoad('current');
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(test.predecessor, expect.objectContaining({
    authoring: authoringWorkspaceIdentity(test.published),
  }), expect.any(AbortSignal));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledWith(expect.anything(), '/projects/current', true);
});

it.each(['load', 'new'])('cancels waiting without a follow-up, then allows later %s', async (kind) => {
  const test = await mount();
  await test.openLoad();
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(true);
  await act(async () => { test.close.resolve(test.closed); });
  const cancel = test.find((p) => p.children === 'Cancel');
  expect(cancel.disabled).toBe(false);
  await test.click(cancel);
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active: test.closed } });
  if (kind === 'load') await test.openLoad();
  else {
    await test.click(test.find((p) => p['aria-label'] === 'New'));
    await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
      .onChange?.({ target: { value: 'next_cell' } }));
    await test.click(test.find((p) => p.className === 'primary-action' && p.children === 'Create Scenario'));
  }
  expect(test.closeScenario).toHaveBeenCalledTimes(2);
  expect(kind === 'load' ? apiMock.scenarioDocuments.load : apiMock.scenarioDocuments.new).toHaveBeenCalledTimes(1);
});


it.each(['draft', 'deployment', 'epoch', 'owner', 'publication'])('refuses popup acknowledgement after %s changes before Close', async (change) => {
  const test = await mount(false);
  await test.click(test.find((p) => p['aria-label'] === 'Load Scenario'));
  await act(() => test.find((p) => p.value === '').onChange?.({ target: { value: 'saved:/projects/other' } }));
  await test.click(test.find((p) => p.className === 'primary-action' && p.children === 'Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  const next = snapshot(test.predecessor, test.published);
  if (change === 'draft') next.authoringWorkspace.programDraftRevision += 1;
  if (change === 'deployment') next.authoringWorkspace.deploymentDraftRevision += 1;
  if (change === 'epoch') next.authoringWorkspace.workspaceEpoch = 'another';
  if (change === 'owner') next.runtime.sections.manager.sourceOwnerEpoch = 'manager.2';
  if (change === 'publication') (next.runtime.sections.manager.payload!.active as Record<string, unknown>).publication = active(false, 4).publication;
  await test.observe(next);
  await test.click(test.find((p) => p.children === 'Close and continue'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(test.all().some((p) => typeof p.children === 'string' && p.children.includes('No replacement was requested'))).toBe(true);
});

it('blocks cancellation while the one replacement mutation is submitting', async () => {
  const test = await mount();
  const load = deferred<unknown>();
  apiMock.scenarioDocuments.load.mockImplementationOnce(() => load.promise);
  await test.openLoad();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(true);
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.all().some((p) => p.role === 'dialog')).toBe(true);
  await act(async () => { load.resolve({ status: 'applied', workspace: workspace(test.published) }); });
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
});


it.each(['load', 'new'])('canonical-empty with dirty Program Draft requires popup before %s', async (kind) => {
  const test = await mount(true, true);
  await test.click(test.find((p) => p['aria-label'] === (kind === 'new' ? 'New' : 'Load Scenario')));
  if (kind === 'new') await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
    .onChange?.({ target: { value: 'next_cell' } }));
  else await act(() => test.find((p) => p.value === '').onChange?.({ target: { value: 'saved:/projects/other' } }));
  await test.click(test.find((p) => p.className === 'primary-action' && p.children === (kind === 'new' ? 'Create Scenario' : 'Load Scenario')));
  expect(test.find((p) => p.children === 'Close Scenario?')).toBeDefined();
  expect(test.all().some((p) => typeof p.children === 'string' && p.children.includes('Draft changes will be discarded'))).toBe(true);
  expect(test.find((p) => p.children === 'Discard the current Draft and continue?')).toBeDefined();
  expect(test.all().some((p) => String(p.children).includes('Active targets are retired')
    || String(p.children).includes('Robot positions may be unknown'))).toBe(false);
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.className === 'primary-action' && p.children === (kind === 'new' ? 'Create Scenario' : 'Load Scenario')));
  await test.click(test.find((p) => p.children === 'Close and continue'));
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(test.predecessor, expect.objectContaining({
    authoring: authoringWorkspaceIdentity(test.published),
  }), expect.any(AbortSignal));
});


it.each([[true, false], [false, false], [true, true]])('standalone Close waits for exact admission then clears once, empty=%s dirty=%s', async (empty, dirty) => {
  const test = await mount(empty, dirty);
  await test.openClose();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(test.predecessor, expect.objectContaining({
    authoring: authoringWorkspaceIdentity(test.published), closeCycle: expect.anything(), closeScene: expect.anything(),
  }), expect.any(AbortSignal));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(true);
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.all().some((p) => p.role === 'dialog')).toBe(true);
  await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, true));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
    draftId: 'draft.1', expectedDraftRevision: 1, expectedWorkspaceEpoch: 'workspace.1',
    expectedWorkspaceRevision: 1, expectedAdmissionGeneration: 8,
  }), {
    managerOwnerId: 'manager', managerOwnerEpoch: 'manager.1',
    retainedBundleId: test.closed.retainedBundleId, graphRevision: test.closed.graphRevision,
    publication: test.closed.publication, deploymentDraftId: 'deployment.1', expectedDeploymentDraftRevision: 1,
    sceneOwnerId: 'scene', sceneOwnerEpoch: 'scene.1', sceneProjectionRevision: sceneRevision(test.closed),
  });
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledTimes(1);
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
});

it('temporary Close of a dirty empty Draft warns of editor clearing without robot retirement paragraphs', async () => {
  const test = await mount(true, true);
  await test.click(test.find((p) => p['aria-label'] === 'Close Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => typeof p.children === 'string'
    && p.children.includes('Program and Scene editors cleared')
    && p.children.includes('Draft changes will be discarded')
    && p.children.includes('Saved files are unchanged'))).toBe(true);
  expect(test.all().some((p) => String(p.children).includes('Active targets are retired')
    || String(p.children).includes('Robot positions may be unknown'))).toBe(false);
  expect(test.all().some((p) => p.type === 'checkbox')).toBe(false);
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
});

it.each(['draft', 'deployment', 'epoch', 'owner', 'publication', 'fault'])('standalone Close preserves editors after intervening %s', async (change) => {
  const test = await mount(false);
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  const next = snapshot(test.closed, test.published);
  if (change === 'draft') next.authoringWorkspace.programDraftRevision += 1;
  if (change === 'deployment') next.authoringWorkspace.deploymentDraftRevision += 1;
  if (change === 'epoch') next.authoringWorkspace.workspaceEpoch = 'another';
  if (change === 'owner') next.runtime.sections.manager.sourceOwnerEpoch = 'manager.2';
  if (change === 'publication') {
    // Same empty graph and bundle, different publication: equality of physical content is insufficient.
    (next.runtime.sections.manager.payload!.active as Record<string, unknown>).publication = active(true, 4).publication;
  }
  if (change === 'fault') {
    next.componentOverview.readiness = 'fault';
    next.componentOverview.userMessage = 'Actual owner fault';
  }
  await test.observe(next);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.className === 'inline-message severity-error')).toBe(true);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it('failed standalone Close never clears the editors', async () => {
  const test = await mount(false);
  await test.openClose();
  await act(async () => { test.close.resolve(null); });
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => typeof p.children === 'string'
    && p.children.includes('Scenario Close is refused or unresolved'))).toBe(true);
});

it('waiting standalone Close can be cancelled without a later editor clear', async () => {
  const test = await mount();
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(false);
  await test.click(test.find((p) => p.children === 'Cancel'));
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
});

it.each(['closing', 'waiting'])('unmounting standalone Close while %s prevents late editor clearing', async (phase) => {
  const test = await mount();
  await test.openClose();
  if (phase === 'waiting') await act(async () => { test.close.resolve(test.closed); });
  await unmount!();
  unmount = null;
  if (phase === 'closing') await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it('an unresolved editor-clear request is not resent on later owner publications', async () => {
  const test = await mount();
  apiMock.programWorkspace.clearClosedScenario.mockRejectedValueOnce(new Error('Editor-clear response lost'));
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledTimes(1);
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledTimes(1);
  expect(test.all().some((p) => typeof p.children === 'string' && p.children.includes('nothing was retried'))).toBe(true);
});

it.each(['load', 'new'])('internal Close before %s does not clear the authoring workspace separately', async (kind) => {
  const test = await mount(false);
  if (kind === 'load') await test.openLoad();
  else {
    await test.click(test.find((p) => p['aria-label'] === 'New'));
    await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
      .onChange?.({ target: { value: 'next_cell' } }));
    await test.click(test.find((p) => p.className === 'primary-action' && p.children === 'Create Scenario'));
    await test.click(test.find((p) => p.children === 'Close and continue'));
  }
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published));
  expect(kind === 'load' ? apiMock.scenarioDocuments.load : apiMock.scenarioDocuments.new).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it('removes three supported fixed Scene objects in dependency order with fresh revision and admission before every effect', async () => {
  const test = await mount(false);
  const objects = supportedObjects();
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, objects) }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit.mock.calls[0][0]).toMatchObject({
    expected_projection_revision: sceneRevision(test.closed), expected_admission_generation: 8,
    expected_draft_id: 'draft.1', expected_draft_revision: 1,
    expected_authoring_workspace: authoringWorkspaceIdentity(test.published),
    request_context: { request_owner_epoch: 'scene.1', source_label: 'composer_ui' },
    operation: { kind: 'remove_object', scene_resource_id: 'graspable' },
  });
  const remaining = objects.filter((object) => object.scene_resource_id !== 'graspable');
  // New admission alone is insufficient while the old Scene is still displayed.
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, objects), admissionGeneration: 9 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  // The new Scene alone is insufficient while the last generation is displayed.
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, remaining, 4), admissionGeneration: 8 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, remaining, 4), admissionGeneration: 9 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  expect(apiMock.scene.commit.mock.calls[1][0]).toMatchObject({
    expected_projection_revision: sceneRevision(test.closed, 4), expected_admission_generation: 9,
    expected_authoring_workspace: authoringWorkspaceIdentity(test.published),
    operation: { kind: 'remove_object', scene_resource_id: 'wall' },
  });
  const ground = remaining.filter((object) => object.scene_resource_id === 'ground');
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, ground, 5), admissionGeneration: 9 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, ground, 5), admissionGeneration: 10 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(3);
  expect(apiMock.scene.commit.mock.calls[2][0]).toMatchObject({
    expected_projection_revision: sceneRevision(test.closed, 5), expected_admission_generation: 10,
    expected_authoring_workspace: authoringWorkspaceIdentity(test.published),
    operation: { kind: 'remove_object', scene_resource_id: 'ground' },
  });
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, [], 6), admissionGeneration: 10 }));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  const final = snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, [], 6), admissionGeneration: 11 });
  await test.observe(final);
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
    expectedAdmissionGeneration: 11, expectedWorkspaceEpoch: 'workspace.1', expectedWorkspaceRevision: 1,
  }), expect.objectContaining({ sceneOwnerId: 'scene', sceneOwnerEpoch: 'scene.1',
    sceneProjectionRevision: sceneRevision(test.closed, 6), publication: test.closed.publication }));
  await test.observe(final);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(3);
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledTimes(1);
  expect(new Set(apiMock.scene.commit.mock.calls.map(([request]) => request.request_context.request_id)).size).toBe(3);
});

it.each(['stale', 'scene_owner', 'manager_owner', 'workspace', 'fault', 'revision', 'inventory'])('stops further Scene effects after intervening %s', async (change) => {
  const test = await mount(false);
  const objects = supportedObjects();
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, objects) }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  const remaining = objects.filter((object) => object.scene_resource_id !== 'graspable');
  const next = snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, remaining, 4), admissionGeneration: 9 });
  if (change === 'stale') next.runtime.sections.logicalScene.freshness = 'stale';
  if (change === 'scene_owner') next.runtime.sections.logicalScene.sourceOwnerEpoch = 'scene.2';
  if (change === 'manager_owner') next.runtime.sections.manager.sourceOwnerEpoch = 'manager.2';
  if (change === 'workspace') next.authoringWorkspace.workspaceRevision += 1;
  if (change === 'fault') {
    next.componentOverview.readiness = 'fault'; next.componentOverview.userMessage = 'Actual owner fault';
  }
  if (change === 'revision') next.runtime.sections.logicalScene.payload = { ...logicalScene(test.closed, remaining, 5) };
  if (change === 'inventory') next.runtime.sections.logicalScene.payload = { ...logicalScene(test.closed, [fixedObject('unexpected')], 4) };
  await test.observe(next);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  if (change === 'stale') await test.click(test.find((p) => p.children === 'Cancel'));
  else expect(test.all().some((p) => p.className === 'inline-message severity-error')).toBe(true);
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, remaining, 4), admissionGeneration: 9 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it('a failed second Scene removal reports the confirmed partial outcome and never clears or resends', async () => {
  const test = await mount(false);
  const objects = supportedObjects();
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, objects) }));
  apiMock.scene.commit.mockRejectedValueOnce(new Error('Owner refused wall removal'));
  const latest = snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, objects.filter((object) => object.scene_resource_id !== 'graspable'), 4), admissionGeneration: 9 });
  await test.observe(latest);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  expect(test.all().some((p) => typeof p.children === 'string'
    && p.children.includes('Owner refused wall removal') && p.children.includes('1 Scene object removal(s) confirmed'))).toBe(true);
  await test.observe(latest);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it('cancelling after one Scene removal fences every later publication and editor clear', async () => {
  const test = await mount(false);
  const objects = supportedObjects();
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, objects) }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(false);
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
  expect(test.find((p) => p['aria-label'] === 'Program feedback').children)
    .toContain('1 Scene object removal(s) confirmed; editors were kept');
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, [], 6), admissionGeneration: 11 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => p.role === 'dialog')).toBe(false);
});

it('retained carried attachment is visibly refused before any Scene removal or editor clear', async () => {
  const test = await mount(false);
  const attachmentRevision = 'sha256:' + 'a'.repeat(64);
  const carried: LogicalSceneObject = { ...fixedObject('held_part'), placement: {
    kind: 'carried', carried_object_attachment_id: 'attachment.1', carried_object_attachment_revision: attachmentRevision } };
  const scene = logicalScene(test.closed, [fixedObject('ground'), carried]);
  scene.snapshot!.carried_object_attachments = [{
    carried_object_attachment_id: 'attachment.1', carried_object_attachment_revision: attachmentRevision,
    scene_resource_id: 'held_part', controllable_component_id: 'arm', command_target_id: 'tool',
    carrier_frame_ref: { kind: 'component', controllable_component_id: 'arm', model_instance_id: 'robot', frame_role_id: 'tool' },
    pose_in_carrier_frame: { position_m: { x: 0, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1] }, touch_body_roles: [],
  }];
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false, { scene }));
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => typeof p.children === 'string'
    && p.children.includes('cannot clear carried Scene objects') && p.children.includes('editors were kept'))).toBe(true);
});

it('unmount during a Scene removal cannot continue after its late receipt', async () => {
  const test = await mount(false);
  const receipt = deferred<unknown>();
  apiMock.scene.commit.mockImplementationOnce(() => receipt.promise);
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, supportedObjects()) }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  const request = apiMock.scene.commit.mock.calls[0][0] as SceneTransactionRequest;
  expect(test.find((p) => p.children === 'Cancel').disabled).toBe(true);
  await unmount!(); unmount = null;
  await act(async () => { receipt.resolve({ request_id: request.request_context.request_id,
    request_owner_epoch: request.request_context.request_owner_epoch,
    committed_projection_revision: sceneRevision(test.closed, 4) }); });
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it.each(['cyclic', 'missing'])('an unsupported %s Scene support dependency sends no removal or editor clear', async (kind) => {
  const test = await mount(false);
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, kind === 'cyclic'
      ? [fixedObject('ground', 'wall'), fixedObject('wall', 'ground')] : [fixedObject('wall', 'missing')]) }));
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((p) => typeof p.children === 'string'
    && p.children.includes(kind === 'cyclic' ? 'cyclic support dependency' : 'unknown support dependency'))).toBe(true);
});


it('resets a nondefault collision policy once and requires its exact publication and admission before editor clear', async () => {
  const test = await mount(false);
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  const custom = logicalScene(test.closed);
  custom.snapshot!.configured_collision_policy.require_full_diagnostics = false;
  await test.observe(snapshot(test.closed, test.published, false, { scene: custom }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit.mock.calls[0][0]).toMatchObject({
    expected_authoring_workspace: authoringWorkspaceIdentity(test.published),
    expected_projection_revision: sceneRevision(test.closed), expected_admission_generation: 8,
    operation: { kind: 'set_collision_policy',
      expected_configured_policy_revision: { revision_id: 1, content_hash: 'policy.1' },
      configured_policy: { policy_id: 'free_space', require_full_diagnostics: true,
        allowed_static_static_pairs: [], allowed_touch_link_pairs: [], support_contacts: [], contact_grasps: [] } },
  });
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, [], 4), admissionGeneration: 8 }));
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published, false,
    { scene: logicalScene(test.closed, [], 4), admissionGeneration: 9 }));
  expect(apiMock.programWorkspace.clearClosedScenario).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
});

it('never resends an accepted policy reset whose published content is still nondefault', async () => {
  const test = await mount(false);
  await test.openClose();
  await act(async () => { test.close.resolve(test.closed); });
  const custom = logicalScene(test.closed);
  custom.snapshot!.configured_collision_policy.require_full_diagnostics = false;
  await test.observe(snapshot(test.closed, test.published, false, { scene: custom }));
  const wrong = logicalScene(test.closed, [], 4);
  wrong.snapshot!.configured_collision_policy.require_full_diagnostics = false;
  await test.observe(snapshot(test.closed, test.published, false, { scene: wrong, admissionGeneration: 9 }));
  await test.observe(snapshot(test.closed, test.published, false, { scene: logicalScene(test.closed, [], 4), admissionGeneration: 9 }));
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});


it.each(['reset', 'detach'])('unmounting only the initiating header during %s aborts its standalone continuation while its workspace owner stays mounted', async (phase) => {
  const resetReceipt = deferred<ProgramResetOutcome>();
  const detachReceipt = deferred<Awaited<ReturnType<typeof apiMock.scene.commit>>>();
  const nextCloseEffect = vi.fn();
  const reset = vi.fn<(options: { correlationId: string; expected: ProgramResetExpectation }) => Promise<ProgramResetOutcome>>(() => resetReceipt.promise);
  const test: Awaited<ReturnType<typeof mount>> = await mount(false, false, async (expected, confirmed, signal) => {
    await prepareSceneForClose({ expected, signal: signal!, preparation: { confirmed: confirmed!,
      readSnapshot: () => test.current, reset }, admissionGeneration: () => 8,
      waitForObservation: async () => {}, trackApply: () => {}, progress: () => {} });
    nextCloseEffect();
    return test.closed;
  });
  const initial = snapshot(test.predecessor, test.published);
  initial.programCycle = { ...initial.programCycle, programRunId: phase === 'reset' ? 'run.1' : null,
    state: phase === 'reset' ? 'terminal_awaiting_reset' : 'idle_after_reset',
    programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 7, cycleClaimGeneration: phase === 'reset' ? 2 : null,
    terminalOrSettlementReference: phase === 'reset' ? 'terminal.1' : null };
  if (phase === 'detach') {
    const payload = initial.runtime.sections.logicalScene.payload as unknown as LogicalSceneProjection;
    const attachmentRevision = `sha256:${'a'.repeat(64)}`;
    payload.snapshot!.objects = [{ ...fixedObject('held'), placement: { kind: 'carried',
      carried_object_attachment_id: 'attachment.held', carried_object_attachment_revision: attachmentRevision } }];
    payload.snapshot!.carried_object_attachments = [{ carried_object_attachment_id: 'attachment.held',
      carried_object_attachment_revision: attachmentRevision, scene_resource_id: 'held',
      controllable_component_id: 'arm', command_target_id: 'gripper', carrier_frame_ref: {
        kind: 'component', controllable_component_id: 'arm', model_instance_id: 'robot', frame_role_id: 'tool' },
      pose_in_carrier_frame: { position_m: { x: 0, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1] }, touch_body_roles: [] }];
    apiMock.scene.commit.mockImplementationOnce(() => detachReceipt.promise);
  }
  await test.observe(initial);
  await test.openClose();
  expect(phase === 'reset' ? reset : apiMock.scene.commit).toHaveBeenCalledTimes(1);
  const callerSignal = test.closeScenario.mock.calls[0][2]!;
  expect(callerSignal.aborted).toBe(false);
  await test.hideCommands();
  expect(callerSignal.aborted).toBe(true);
  await act(async () => {
    if (phase === 'reset') resetReceipt.resolve({ status: 'sent', reset: { requestId: 'reset.1',
      correlationId: reset.mock.calls[0][0].correlationId, ...reset.mock.calls[0][0].expected } });
    else {
      const transaction = apiMock.scene.commit.mock.calls[0][0] as SceneTransactionRequest;
      detachReceipt.resolve({ request_id: transaction.request_context.request_id,
        request_owner_epoch: transaction.request_context.request_owner_epoch,
        committed_projection_revision: { ...transaction.expected_projection_revision,
          scene_revision: { ...transaction.expected_projection_revision.scene_revision, revision_id: 2, content_hash: 'scene.2' } } });
    }
  });
  expect(nextCloseEffect).not.toHaveBeenCalled();
  expect(reset).toHaveBeenCalledTimes(phase === 'reset' ? 1 : 0);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(phase === 'detach' ? 1 : 0);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(test.all().some((props) => typeof props.children === 'string'
    && props.children.includes(phase === 'detach' ? 'Detached objects confirmed: held' : 'Close not completed'))).toBe(true);
});

async function terminalReplacement() {
  const receipt = deferred<ProgramResetOutcome>();
  const observation = deferred<void>();
  const lifecycleClose = vi.fn();
  const reset = vi.fn<(options: { correlationId: string; expected: ProgramResetExpectation }) => Promise<ProgramResetOutcome>>(() => receipt.promise);
  const test: Awaited<ReturnType<typeof mount>> = await mount(false, false, async (expected, confirmed, signal) => {
    await prepareSceneForClose({ expected, signal: signal!, admissionGeneration: () => 8,
      waitForObservation: () => observation.promise, trackApply: () => {}, progress: () => {},
      preparation: { confirmed: confirmed!, readSnapshot: () => test.current, reset },
    });
    lifecycleClose();
    return test.closed;
  });
  const initial = snapshot(test.predecessor, test.published);
  initial.programCycle = { ...initial.programCycle, state: 'terminal_awaiting_reset', programRunId: 'run.finished',
    programRunnerOwnerEpoch: 'program-runner-8ddc40cd176140d8917929de87f1cf2d', cycleGeneration: 7,
    cycleClaimGeneration: 23, terminalOrSettlementReference: 'terminal.finished' };
  initial.authoringAdmission.authoringAllowed = false;
  await test.observe(initial);
  const open = async (kind: 'new' | 'load') => {
    await test.click(test.find((p) => p['aria-label'] === (kind === 'new' ? 'New' : 'Load Scenario')));
    if (kind === 'new') {
      await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
        .onChange?.({ target: { value: 'next_cell' } }));
    } else await act(() => test.find((p) => p.value === '').onChange?.({ target: { value: 'saved:/projects/other' } }));
    await test.click(test.find((p) => p.className === 'primary-action'
      && p.children === (kind === 'new' ? 'Create Scenario' : 'Load Scenario')));
    const text = test.all().map((p) => Array.isArray(p.children)
      ? p.children.filter((v) => typeof v === 'string').join('') : p.children).filter((v) => typeof v === 'string').join(' ');
    expect(text).toContain('a finished, stopped or held Program is Reset first');
    expect(reset).not.toHaveBeenCalled();
    await test.click(test.find((p) => p.children === 'Close and continue'));
    expect(reset).toHaveBeenCalledTimes(1);
  };
  const idle = () => ({ ...test.current, authoringAdmission: { authoringAllowed: true, admissionGeneration: 9 },
    programCycle: { ...test.current.programCycle, state: 'idle_after_reset' as const, programRunId: null,
      cycleGeneration: 8, cycleClaimGeneration: null, causalCorrelationId: reset.mock.calls[0][0].correlationId } });
  const sent = (): ProgramResetOutcome => ({ status: 'sent', reset: { requestId: 'reset.1',
    correlationId: reset.mock.calls[0][0].correlationId, ...reset.mock.calls[0][0].expected } });
  return { test, open, reset, receipt, observation, lifecycleClose, idle, sent };
}

it.each(['new', 'load'] as const)('confirmed %s waits for one Reset and correlated idle, then uses its existing final branch without standalone clearing', async (kind) => {
  const step = await terminalReplacement();
  const { test } = step;
  await step.open(kind);
  const caller = test.closeScenario.mock.calls[0][2]!;
  expect(test.closeScenario.mock.calls).toEqual([[test.predecessor,
    expect.objectContaining({ authoring: authoringWorkspaceIdentity(test.published),
      closeCycle: test.current.programCycle, closeScene: test.current.runtime.sections.logicalScene }), caller]]);
  expect(caller).toBeInstanceOf(AbortSignal);
  expect(caller.aborted).toBe(false);
  expect(step.lifecycleClose).not.toHaveBeenCalled();
  await act(() => step.receipt.resolve(step.sent()));
  expect(step.lifecycleClose).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  await test.observe(step.idle());
  await act(() => step.observation.resolve());
  expect(step.lifecycleClose).toHaveBeenCalledTimes(1);
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published));
  expect(kind === 'load' ? apiMock.scenarioDocuments.load : apiMock.scenarioDocuments.new).toHaveBeenCalledTimes(1);
  expect(kind === 'load' ? apiMock.scenarioDocuments.new : apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(step.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it.each(['new', 'load'] as const)('Reset refusal during confirmed %s stops without Close, final replacement or hidden resend', async (kind) => {
  const step = await terminalReplacement();
  await step.open(kind);
  await act(() => step.receipt.resolve({ status: 'refused', message: 'Reset was refused by the current Program owner.' }));
  expect(step.lifecycleClose).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  expect(step.test.all().some((p) => typeof p.children === 'string' && p.children.includes('Reset was refused'))).toBe(true);
  await step.test.observe(step.test.current);
  expect(step.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});

it.each(['new', 'load'] as const)('header unmount during %s Reset aborts the caller before any Close or final replacement', async (kind) => {
  const step = await terminalReplacement();
  await step.open(kind);
  const caller = step.test.closeScenario.mock.calls[0][2]!;
  await step.test.hideCommands();
  expect(caller.aborted).toBe(true);
  await act(() => step.receipt.resolve(step.sent()));
  expect(step.lifecycleClose).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.new).not.toHaveBeenCalled();
  expect(step.reset).toHaveBeenCalledTimes(1);
  expect(step.test.all().some((p) => typeof p.children === 'string'
    && p.children.includes('Reset was submitted; its outcome is not confirmed.'))).toBe(true);
});

it('observed Deployment mutation while replacement Reset settles stops before Close with honest Reset-done feedback', async () => {
  const step = await terminalReplacement();
  await step.open('load');
  await act(() => step.receipt.resolve(step.sent()));
  const changed = step.idle();
  changed.authoringWorkspace = { ...changed.authoringWorkspace, workspaceRevision: 2, deploymentDraftRevision: 2 };
  await step.test.observe(changed);
  await act(() => step.observation.resolve());
  expect(step.lifecycleClose).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
  expect(step.reset).toHaveBeenCalledTimes(1);
  expect(step.test.all().some((p) => typeof p.children === 'string' && p.children.includes('Reset done; Close not completed'))).toBe(true);
});

it('empty idle Load needs no popup and its captured shared preparation sends no Reset before one ordinary Load', async () => {
  const reset = vi.fn<StandaloneReset>();
  type StandaloneReset = (options: { correlationId: string; expected: ProgramResetExpectation }) => Promise<ProgramResetOutcome>;
  const test: Awaited<ReturnType<typeof mount>> = await mount(true, false, async (expected, confirmed, signal) => {
    expect(confirmed).toBeDefined();
    await prepareSceneForClose({ expected, signal: signal!, admissionGeneration: () => 8,
      waitForObservation: async () => {}, trackApply: () => {}, progress: () => {},
      preparation: { confirmed: confirmed!, readSnapshot: () => test.current, reset },
    });
    return test.closed;
  });
  const initial = snapshot(test.predecessor, test.published);
  initial.programCycle = { ...initial.programCycle, state: 'idle_after_reset', programRunId: null,
    programRunnerOwnerEpoch: 'program-runner-8ddc40cd176140d8917929de87f1cf2d', cycleGeneration: 1,
    cycleClaimGeneration: null, terminalOrSettlementReference: null };
  await test.observe(initial);
  await test.openLoad(); // This helper also asserts that no confirmation popup appeared.
  expect(reset).not.toHaveBeenCalled();
  expect(apiMock.scenarioDocuments.load).not.toHaveBeenCalled();
  await test.observe(snapshot(test.closed, test.published));
  expect(apiMock.scenarioDocuments.load).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.programWorkspace.clearClosedScenario).not.toHaveBeenCalled();
});
