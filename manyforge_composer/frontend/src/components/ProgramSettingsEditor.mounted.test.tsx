import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { CatalogEntry, ProgramAuthoringDocument } from '../types/api';
import { ProgramSettingsEditor } from './ProgramSettingsEditor';

afterEach(() => vi.unstubAllGlobals());

// Same minimal React DOM host as ProgramGroupControls.mounted.test.tsx: real
// state and handlers, without adding a DOM dependency or browser stack.
type FakeNode = Record<string, unknown>;
type Props = Record<string, unknown> & { onClick?: () => void };
function mountedHost() {
  const elements: FakeNode[] = [];
  const document: FakeNode = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): FakeNode => {
    const element = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
    };
    elements.push(element);
    return element;
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const props = (element: FakeNode) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const find = (match: (p: Props) => boolean) => {
    const found = elements.map(props).find((p) => p !== undefined && match(p));
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find };
}

const runtime = {
  state: { currentScenario: null, applied: null },
} as unknown as ProgramRuntimeController;

const program: ProgramAuthoringDocument = {
  api_version: 'manyforge.program.v2',
  program_id: 'role_removal',
  program_profile: 'composable-cell-v1',
  name: 'Role removal',
  catalog: {
    api_version: 'manyforge.node_catalog.v2',
    content_version: '2.4.0',
  },
  component_roles: { robot: {}, spare: {} },
  target_roles: { arm: { component_role: 'robot', required_capabilities: [] } },
  schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [],
  positions: {},
};

async function mountSettings(draft: ProgramAuthoringDocument, catalogEntries: CatalogEntry[] = []) {
  const { host, find } = mountedHost();
  const onMutate = vi.fn(() => null);
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(
    <ProgramSettingsEditor
      program={draft}
      catalogEntries={catalogEntries}
      candidateRevision={null}
      busy={false}
      runtime={runtime}
      onMutate={onMutate}
    />,
  ));
  return { onMutate, find, unmount: () => act(() => root.unmount()) };
}

it('removes only an unused component role through the existing role replacement', async () => {
  const mounted = await mountSettings(program);
  try {
    const robot = mounted.find((p) => p['aria-label'] === 'Remove component role robot');
    const spare = mounted.find((p) => p['aria-label'] === 'Remove component role spare');
    expect(robot.disabled).toBe(true);
    expect(robot.title).toBe('target role arm');
    expect(spare.disabled).toBe(false);
    await act(() => spare.onClick?.());
    expect(mounted.onMutate).toHaveBeenCalledOnce();
    expect(mounted.onMutate).toHaveBeenCalledWith('program_draft_replace_roles', {
      component_roles: { robot: {} },
      target_roles: program.target_roles,
    });
  } finally { await mounted.unmount(); }
});

it('keeps a component role that a contact policy still names', async () => {
  const mounted = await mountSettings({
    ...program,
    contact_policies: [{
      api_version: 'manyforge.contact_policy.v1',
      policy_id: 'grasp',
      mode: 'contact_grasp',
      zones: [{
        zone_id: 'pad',
        body_pairs: [{
          body_a: { kind: 'component_model_body', component_role_id: 'spare', collision_body_role_id: 'pad' },
          body_b: { kind: 'runtime_object', runtime_object_id: 'cube' },
        }],
      }],
    }],
  });
  try {
    const spare = mounted.find((p) => p['aria-label'] === 'Remove component role spare');
    expect(spare.disabled).toBe(true);
    expect(spare.title).toBe('contact policy grasp');
  } finally { await mounted.unmount(); }
});

it('starts a logic-only Program with no component role and still offers role creation', () => {
  const markup = renderToStaticMarkup(
    <ProgramSettingsEditor
      program={{ ...program, component_roles: {}, target_roles: {} }}
      catalogEntries={[]}
      candidateRevision={null}
      busy={false}
      runtime={runtime}
      onMutate={vi.fn(() => null)}
    />,
  );

  expect(markup).toContain('Create component role');
  expect(markup).not.toContain('Remove component role');
  expect(markup).toContain('Select component role');
});

it('validates the Program corner blend radius as a non-negative distance', async () => {
  // Declared defaults stand in when no node of this Program carries the field.
  const motionEntry = {
    id: 'move', name: 'Move', category: 'motion', description: '', stability: 'stable',
    kind: 'leaf', childrenMin: 0, childrenMax: 0, parameters: [], requiredCapabilities: [],
    execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
    runners: { py_trees: 'move_v1' }, tags: [], minProgramProfile: 'composable-cell-v1',
    blackboard: { reads: [], writes: [] }, ownerContext: null, outcomes: {},
    eventFamily: 'manyforge.program.node',
  } as CatalogEntry;
  const defaults = { velocity_scale: 0.5, acceleration_scale: 0.5, translation_tol_m: 0.005, blend_radius_m: 0 };
  const mounted = await mountSettings({
    ...program,
    parameters: Object.entries(defaults).map(([parameter_id, value]) => ({
      parameter_id, type: 'float64', default: value,
    })),
  }, [motionEntry]);
  try {
    const radius = () => mounted.find((p) => p['aria-label'] === 'Program Cartesian corner blend radius');
    const submit = () => mounted.find((p) => p.children === 'Update motion defaults');
    expect(radius().value).toBe(0);
    const edit = (value: number) => act(() => (radius().onChange as (event: unknown) => void)(
      { currentTarget: { valueAsNumber: value } }));
    await edit(-0.001);
    await act(() => submit().onClick?.());
    expect(mounted.onMutate).not.toHaveBeenCalled();
    expect(mounted.find((p) => p.role === 'alert').children)
      .toContain('corner blend radius must be non-negative');
    await edit(0.02);
    await act(() => submit().onClick?.());
    expect(mounted.onMutate).toHaveBeenCalledWith('program_draft_update_motion_defaults',
      expect.objectContaining({ blend_radius_m: 0.02, translation_tol_m: 0.005 }));
  } finally { await mounted.unmount(); }
});
