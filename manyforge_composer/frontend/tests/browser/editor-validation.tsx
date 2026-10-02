// Mounted production editors; no backend mutations, hand-written DOM or timers.
import { act, StrictMode, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { ProgramSchemaValueEditor } from '../../src/components/property/ProgramSchemaValueEditor';
import { ProgramPropertyEditor } from '../../src/components/ProgramPropertyEditor';
import { parseCatalogResponse } from '../../src/api/parsers';
import type { CatalogEntry, CatalogJsonSchema, JsonValue, ProgramAuthoringDocument } from '../../src/types/api';

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const results = { passed: [] as string[], error: '', done: false, renders: 0, reports: 0 };
Object.assign(window, { editorValidationTests: results });
const root = createRoot(document.getElementById('root')!);
const viewerReady = new URLSearchParams(location.search).has('viewer') ? (async () => {
  const { StructuralViewer } = await import('../../src/components/StructuralViewer');
  await import('../../src/styles/index.css');
  const host = document.createElement('div');
  host.style.cssText = 'position:fixed;right:0;top:0;width:45vw;height:95vh';
  document.body.append(host);
  document.getElementById('root')!.style.width = '50vw';
  await act(async () => createRoot(host).render(<StructuralViewer viewerActive
    manifest={{ status: 'loading' }} onRetry={() => {}}
    modelPresentation={{ status: 'unavailable', message: 'Isolated editor fixture; no robot model.' }}
    sceneObjects={[{
      scene_resource_id: 'fixture-box', collision_shapes: [{ type: 'box', dimensions_m: { x: 0.2, y: 0.2, z: 0.2 } }],
      placement: { kind: 'cell_fixed', cell_id: 'fixture', pose_in_cell: {
        position_m: { x: 0, y: 0, z: 0.1 }, orientation_xyzw: [0, 0, 0, 1],
      } }, provider_evidence: null, support_contacts: [],
    }]} />));
})() : Promise.resolve();
const entry: CatalogEntry = {
  id: 'test.editor', name: 'Editor', category: 'logic', description: '', stability: 'stable',
  kind: 'leaf', childrenMin: 0, childrenMax: 0, parameters: [], requiredCapabilities: [],
  execution: { blocking: true, cancellable: false, ownerApi: 'program' },
  runners: { py_trees: 'test' }, tags: [], minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] }, ownerContext: null, outcomes: {}, eventFamily: null,
};
const node = { node_id: entry.id, instance_id: 'edit', parameters: {}, children: [] };
const program: ProgramAuthoringDocument = {
  api_version: 'manyforge.program.v2', program_id: 'editor_fixture', name: 'Editor fixture',
  program_profile: 'composable-cell-v1', catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.4.0' },
  component_roles: {}, target_roles: {}, schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [node], positions: {},
};
const context = { entry, node, program, parameterId: 'field' };
const number: CatalogJsonSchema = { type: 'number', minimum: 0 };
const object: CatalogJsonSchema = {
  type: 'object', properties: { a: number, b: number }, required: ['a', 'b'], additionalProperties: false,
};
const array: CatalogJsonSchema = { type: 'array', items: number };
const tuple: CatalogJsonSchema = { type: 'array', minItems: 2, maxItems: 2, items: number };
const map: CatalogJsonSchema = { type: 'object', additionalProperties: number };
const reference: CatalogJsonSchema = { type: 'object', properties: {
  type: { type: 'string', enum: ['pose', 'frame_role'] }, frame_role_id: { type: 'string' },
}, required: ['type'] };
let current: JsonValue | undefined;
let invalid: string | null = null;
let commits = 0;
let notify = () => {};
let reset = () => {};

function check(ok: unknown, message: string): void {
  if (!ok) throw new Error(message);
}
function count(kind: 'renders' | 'reports') {
  results[kind]++;
  // Fail inside the feedback path: an infinite act() cannot starve a timeout.
  check(results[kind] < (kind === 'renders' ? 80 : 40), `${kind} did not settle`);
}
function Harness({ schema, initial }: { schema: CatalogJsonSchema; initial: JsonValue | undefined }) {
  count('renders');
  const [value, setValue] = useState(initial);
  const [, setError] = useState({ message: null as string | null });
  const [epoch, setEpoch] = useState(0);
  const [, refresh] = useState(0);
  const [inventory, setInventory] = useState({ jointNames: ['joint_a', 'joint_b'], targetRole: 'arm', unavailableReason: null });
  current = value;
  notify = () => { setValue(structuredClone(value)); setInventory(structuredClone(inventory)); refresh((n) => n + 1); };
  reset = () => setEpoch((n) => n + 1);
  return <ProgramSchemaValueEditor key={epoch} schema={schema} value={value} path="field"
    context={{ ...context, sourceJointInventory: inventory }} disabled={false}
    onCommit={(next) => { commits++; setValue(structuredClone(next)); }}
    onInvalidChange={(message) => {
      count('reports'); invalid = message;
      // Deliberately non-idempotent parent and a new callback each render.
      setError({ message });
    }} />;
}
async function mount(schema: CatalogJsonSchema, value: JsonValue | undefined) {
  await act(async () => root.render(null));
  results.renders = 0; results.reports = 0; commits = 0; invalid = null;
  await act(async () => root.render(<StrictMode><Harness schema={schema} initial={value} /></StrictMode>));
}
function input(selector = 'input[type=number]', index = 0) {
  const found = document.querySelectorAll<HTMLInputElement>(selector)[index];
  check(found, `Missing input ${selector}[${index}]`);
  return found;
}
async function type(field: HTMLInputElement, text: string) {
  await act(async () => {
    field.focus();
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(field, text);
    field.dispatchEvent(new Event('input', { bubbles: true }));
  });
  await act(async () => field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })));
}
async function button(label: string, index = 0) {
  const found = [...document.querySelectorAll<HTMLButtonElement>('button')].filter((b) => b.textContent === label)[index];
  check(found, `Missing button ${label}`);
  await act(async () => found!.click());
}
async function select(selector: string, value: string) {
  await act(async () => {
    const field = document.querySelector<HTMLSelectElement>(selector)!;
    check(field, `Missing select ${selector}`);
    field.value = value;
    field.dispatchEvent(new Event('change', { bubbles: true }));
  });
}
async function test(name: string, run: () => Promise<void>) {
  await run(); results.passed.push(name);
}

async function suite() {
  await viewerReady;
  for (const [name, schema, value] of [
    ['array', array, [1, 2]], ['object', object, { a: 1, b: 2 }],
    ['map', map, { a: 1, b: 2 }], ['tuple', tuple, [1, 2]],
  ] as const) {
    await test(`${name}: effects settle; sibling errors survive commits and equal readback`, async () => {
      await mount(schema, value as JsonValue);
      const reports = results.reports;
      await act(async () => { notify(); notify(); });
      check(results.reports === reports, 'Callback changes reported unchanged validation');
      await type(input(), '-1'); await type(input('input[type=number]', 1), '-2');
      check(invalid !== null && commits === 0, 'Invalid text mutated the Program');
      await type(input(), '3');
      check(invalid !== null && input('input[type=number]', 1).value === '-2', 'Sibling error/text was lost');
      check(commits === 1, 'Valid sibling was not committed');
      await act(async () => notify());
      check(invalid !== null, 'Readback cleared local invalidity');
      await type(input('input[type=number]', 1), '4');
      check(invalid === null && commits === 2, 'Correction did not clear invalidity');
    });
  }
  await test('array reorder/removal keeps surviving text and error with the row', async () => {
    await mount(array, [1, 2]);
    const first = input(); const second = input('input[type=number]', 1);
    await type(first, '-1'); await type(second, '-2');
    check(first.value === '-1' && second.value === '-2' && invalid !== null, 'Input setup lost invalid values');
    await button('Later');
    check(input() === second && input('input[type=number]', 1) === first, 'Reorder remounted or misassigned rows');
    check(first.value === '-1' && second.value === '-2' && invalid !== null,
      `Reorder lost text: ${JSON.stringify({ first: first.value, second: second.value, invalid, current })}`);
    await button('Remove');
    check(input() === first && first.value === '-1' && invalid !== null,
      `Removal lost the surviving draft: ${JSON.stringify({ sameInput: input() === first, text: input().value, oldText: first.value, invalid, current })}`);
    await type(first, '5'); check(invalid === null, 'Removed row left a stale error');
  });
  await test('map key/value errors remain independent; rename preserves unfinished value', async () => {
    await mount(map, { a: 1, b: 2 });
    const value = input();
    await type(value, '-1');
    await type(input('input:not([type])'), 'b');
    check(invalid !== null && commits === 0, 'Duplicate key accepted');
    await type(input('input:not([type])'), 'renamed');
    check(input() === value && value.value === '-1' && invalid !== null, 'Rename discarded value editor');
    await type(value, '6'); check(invalid === null, 'Renamed key left a stale error');
    check(JSON.stringify(current) === JSON.stringify({ renamed: 6, b: 2 }), 'Rename committed the wrong value');
  });
  await test('nested missing required defaults and explicit reset', async () => {
    await mount({ type: 'object', properties: { nested: object, list: array }, required: ['nested', 'list'] }, {});
    await type(input(), '-1');
    await act(async () => notify());
    check(invalid !== null && input().value === '-1', 'Missing child default reset local text');
    await act(async () => reset());
    check(invalid === null && input().value === '0' && commits === 0, 'Explicit discard failed');
    await button('Add row'); await type(input('input[type=number]', 2), '-2');
    await act(async () => notify());
    check(invalid !== null && input('input[type=number]', 2).value === '-2', 'Missing array reset local text');
    await mount(object, { a: 1, b: 2 });
    check(invalid === null, 'Node switch retained old invalidity');
  });
  await test('absent specialized reference preserves its staged variant on rerender', async () => {
    await mount(reference, undefined);
    await select('select[aria-label="Motion reference type"]', 'frame_role');
    check(invalid !== null, 'Incomplete reference was not reported');
    await act(async () => notify());
    check(document.querySelector<HTMLSelectElement>('select')!.value === 'frame_role' && invalid !== null,
      'Unrelated render reset the staged variant');
    check(commits === 0, 'Staging committed manufactured defaults');
  });
  await test('nested specialized draft survives keyed-row effect replay', async () => {
    await mount({ type: 'array', items: reference }, [{ type: 'pose' }, { type: 'pose' }]);
    const field = document.querySelector<HTMLSelectElement>('select[aria-label="Motion reference type"]')!;
    await select('select[aria-label="Motion reference type"]', 'frame_role');
    await button('Later');
    check(field.value === 'frame_role' && invalid !== null, 'Reorder reset staged reference');
  });
  await test('absent joint goal preserves staged selection on equal inventory readback', async () => {
    await mount({ type: 'array', minItems: 1, items: { type: 'object',
      properties: { joint_name: { type: 'string' }, position_rad: { type: 'number' } },
      required: ['joint_name', 'position_rad'],
    } }, undefined);
    await act(async () => input('input[type=checkbox]').click());
    check(invalid !== null, 'Missing joint position was not reported');
    await act(async () => notify());
    check(input('input[type=checkbox]').checked && invalid !== null, 'Readback reset the staged joint selection');
    await type(input(), '0.5');
    check(invalid === null && commits === 1, 'Complete joint selection was not committed');
  });

  // The actual property panel proves its commit-error/local-error ownership,
  // and exercises the served catalog rather than an imitation of Move-to-pose.
  const catalog = parseCatalogResponse(await (await fetch('./editor-catalog.json')).json());
  const pose = catalog.entries.find((item) => item.id === 'motion.move_to_pose')!;
  check(pose, 'Move-to-pose missing from catalog');
  for (const complete of [false, true]) {
    await test(`actual Move-to-pose panel (${complete ? 'complete' : 'incomplete'}): bounded renders, Discard and sibling protection`, async () => {
      await act(async () => root.render(null));
      results.renders = 0; results.reports = 0; commits = 0;
      let blocked = false;
      let populate = () => {};
      function Panel() {
        count('renders');
        const [draft, setDraft] = useState<ProgramAuthoringDocument>({ ...program, forest: [{
          ...node, node_id: pose.id, parameters: {
            ...(complete ? { target_role: 'arm', base_frame_role: 'base', tcp_frame_role: 'tcp',
              goal: { frame_ref: { kind: 'cell_frame' }, position_m: [0, 0, 0], orientation: { rpy_deg: [0, 0, 0] } } } : {}),
            per_joint_dynamics_limits: {},
          },
        }] });
        const [, setBlocked] = useState(false);
        populate = () => setDraft((previous) => ({ ...previous, forest: [{ ...previous.forest[0],
          parameters: { ...previous.forest[0].parameters,
            per_joint_dynamics_limits: { joint_a: { max_velocity_rad_s: 1 }, joint_b: { max_velocity_rad_s: 2 } },
          },
        }] }));
        return <ProgramPropertyEditor roots={draft.forest} selectedId="edit" catalogEntries={catalog.entries}
          program={draft} busy={false} onUpdateNodePolicies={() => {}}
          onInvalidEditChange={(value) => { count('reports'); blocked = value; setBlocked(value); }}
          onMutate={(_command, args) => {
            commits++;
            setDraft((previous) => ({ ...previous, forest: [{ ...previous.forest[0], parameters: args.parameters as Record<string, JsonValue> }] }));
          }} />;
      }
      await act(async () => root.render(<StrictMode><Panel /></StrictMode>));
      const selector = '#property-edit-per_joint_dynamics_limits input[type=number]';
      check(document.querySelector('#property-edit-per_joint_dynamics_limits .program-schema-map'), 'Wrong catalog/editor dispatch');
      check(document.querySelectorAll(selector).length === 0 && commits === 0, 'Empty map mutated while inspecting');
      await act(async () => populate());
      await type(input(selector), '-1'); await type(input(selector, 1), '-2');
      await type(input(selector), '3');
      check(blocked && input(selector, 1).value === '-2', 'Parent cleared a sibling error after commit');
      await button('Discard edit');
      check(!blocked, 'Discard did not reopen local editing');
      if (complete) {
        await select('select[aria-label="Frame reference kind"]', 'target_frame');
        check(blocked, 'Incomplete frame reference was not reported');
        await act(async () => populate());
        check(blocked && document.querySelector<HTMLSelectElement>('select[aria-label="Frame reference kind"]')!.value === 'target_frame',
          'Equal sibling readback reset a staged frame reference');
        await button('Discard edit');
      }
    });
  }
}

function run() {
  results.done = false; results.error = ''; results.passed = [];
  return suite().catch((error: unknown) => {
    results.error = error instanceof AggregateError ? error.errors.map(String).join('\n')
      : error instanceof Error ? `${error.message}\n${error.stack ?? ''}` : String(error);
  })
    .finally(() => { results.done = true; });
}
Object.assign(window, { repeatEditorValidation: run });
void run();
