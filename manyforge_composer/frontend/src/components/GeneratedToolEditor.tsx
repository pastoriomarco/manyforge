import { Suspense, useEffect, useMemo, useRef, useState } from 'react';
import { Canvas, useThree } from '@react-three/fiber';
import { OrbitControls, useGLTF } from '@react-three/drei';
import * as THREE from 'three';

import { VIEWPORT_COLORS } from '../styles/tokens';
import { api, resolveStructuralAssetUrl } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ToolPreviewResponse, ToolPreviewShape } from '../types/toolParameters';
import { declaredIoInputs, declaredIoOutputs } from '../utils/ioSignalConfiguration';
import { frameableContentBounds, framedCameraPresentation } from '../utils/viewerCamera';
import {
  ELEMENT_KIND_LABELS,
  ELEMENT_KINDS,
  GEOMETRY_KINDS,
  defaultContactBodies,
  elements,
  fieldPath,
  followedWiring,
  generatedToolRow,
  jawPairs,
  list,
  newElement,
  newGeometry,
  newPart,
  object,
  partNames,
  parametersPath,
  poseFields,
  poseValue,
  refusalAt,
  relativeMeshPath,
  releaseDepthStart,
  toolEnds,
  uniqueId,
  withValue,
  withWiring,
  type ElementKind,
  type GeometryKind,
  type JsonObject,
} from '../utils/generatedToolAuthoring';
import { AssetErrorBoundary } from './common/AssetErrorBoundary';
import { DraftTextInput } from './common/DraftInputs';
import { NumberDraft } from './DeploymentFocusedEditor';
import { MeshResourceBrowser } from './MeshResourceBrowser';
import { PneumaticIoEditor, pneumaticIoProblem } from './PneumaticIoEditor';

type Path = Array<string | number>;
type Refusal = { code: string; message: string; field: string | null } | null;

/** Edit context shared by the sections: the parameters, how to change them, the refusal. */
interface Edit {
  parameters: JsonObject;
  disabled: boolean;
  refusal: Refusal;
  /** The folder mesh paths are relative to (the Scenario folder), once the backend named it. */
  meshFolder: string | null;
  /** Set (or with `undefined` remove) one field of the parameters. */
  set: (path: Path, value: JsonValue | undefined) => void;
}

function FieldError({ edit, field }: { edit: Edit; field: string }) {
  return edit.refusal !== null && edit.refusal.field === field
    ? <small className="deployment-field-error inline-message severity-error" role="alert">{edit.refusal.message}</small>
    : null;
}

function at(value: JsonValue | undefined, path: Path): JsonValue | undefined {
  let current = value;
  for (const part of path) {
    if (Array.isArray(current) && typeof part === 'number') current = current[part];
    else current = object(current)?.[part as string];
  }
  return current;
}

function NumberField({ edit, label, path, unit, min, exclusiveMin, optional }: {
  edit: Edit; label: string; path: Path; unit?: string; min?: number; exclusiveMin?: number;
  /** An optional field: the value it starts from when set (absent: Load's default). */
  optional?: number;
}) {
  const value = at(edit.parameters, path);
  const field = fieldPath(...path);
  if (optional !== undefined && typeof value !== 'number') {
    return (
      <div className="deployment-focused-field">
        <span>{label}</span>
        <button type="button" disabled={edit.disabled}
          onClick={() => edit.set(path, optional)}>{`Set ${label.toLowerCase()}`}</button>
        <FieldError edit={edit} field={field} />
      </div>
    );
  }
  return (
    <div>
      <NumberDraft label={label} value={typeof value === 'number' ? value : 0} unit={unit} min={min}
        exclusiveMin={exclusiveMin} disabled={edit.disabled} onCommit={(next) => edit.set(path, next)} />
      {optional !== undefined && <button type="button" disabled={edit.disabled}
        onClick={() => edit.set(path, undefined)}>Use the default</button>}
      <FieldError edit={edit} field={field} />
    </div>
  );
}

function Vector({ edit, label, path, values, unit, onCommit }: {
  edit: Edit; label: string; path: Path; values: number[]; unit: string;
  onCommit: (values: [number, number, number]) => void;
}) {
  return (
    <fieldset className="deployment-initial-joints">
      <legend>{label}</legend>
      {(['x', 'y', 'z'] as const).map((axis, index) => (
        <NumberDraft key={axis} label={`${label} ${axis}`} value={values[index] ?? 0} unit={unit}
          disabled={edit.disabled} onCommit={(next) => {
            const copy = [values[0] ?? 0, values[1] ?? 0, values[2] ?? 0] as [number, number, number];
            copy[index] = next;
            onCommit(copy);
          }} />
      ))}
      <FieldError edit={edit} field={fieldPath(...path)} />
    </fieldset>
  );
}

function Pose({ edit, label, path }: { edit: Edit; label: string; path: Path }) {
  const { position, rpy } = poseFields(at(edit.parameters, path));
  return (
    <div className="deployment-focused-form-grid">
      <Vector edit={edit} label={`${label} position`} path={[...path, 'position_m']} values={position} unit="m"
        onCommit={(next) => edit.set(path, poseValue(next, rpy ?? [0, 0, 0]))} />
      {rpy === null ? (
        <p className="field-help">{label} orientation: a quaternion (xyzw) in the parameters; edit it in the file.</p>
      ) : (
        <Vector edit={edit} label={`${label} roll, pitch, yaw`} path={[...path, 'orientation']} values={rpy} unit="°"
          onCommit={(next) => edit.set(path, poseValue(position, next))} />
      )}
      <FieldError edit={edit} field={fieldPath(...path)} />
    </div>
  );
}

function Text({ edit, label, path, validate }: {
  edit: Edit; label: string; path: Path; validate?: (value: string) => string | null;
}) {
  const value = at(edit.parameters, path);
  return (
    <label className="deployment-focused-field">
      <span>{label}</span>
      <DraftTextInput aria-label={label} value={typeof value === 'string' ? value : ''} disabled={edit.disabled}
        validate={validate} onCommit={(next) => edit.set(path, next.trim())} />
      <FieldError edit={edit} field={fieldPath(...path)} />
    </label>
  );
}

const ID_PATTERN = /^[a-z][a-z0-9_]{0,63}$/;
const idProblem = (value: string) => (ID_PATTERN.test(value.trim())
  ? null : 'Use a lower-case id: a letter, then letters, digits or _.');

function geometryKind(geometry: JsonValue | undefined): GeometryKind | null {
  const kind = Object.keys(object(geometry) ?? {})[0];
  return (GEOMETRY_KINDS as readonly string[]).includes(kind) ? kind as GeometryKind : null;
}

function MeshPath({ edit, path }: { edit: Edit; path: Path }) {
  const [browserOpen, setBrowserOpen] = useState(false);
  const { meshFolder } = edit;
  return (
    <>
      <Text edit={edit} label="Mesh path" path={path}
        validate={(value) => (/\.(stl|dae)$/i.test(value.trim()) ? null : 'Name an .stl or .dae file.')} />
      <button
        type="button"
        className="quiet-button"
        aria-expanded={browserOpen}
        disabled={edit.disabled || meshFolder === null}
        title={meshFolder === null
          ? 'The mesh folder is not known yet.'
          : 'Pick the mesh from the approved mesh files; its path is written relative to the Scenario folder.'}
        onPointerDown={(event) => event.preventDefault()}
        onClick={() => setBrowserOpen((current) => !current)}
      >
        {browserOpen ? 'Close mesh browser' : 'Browse approved meshes'}
      </button>
      {browserOpen && meshFolder !== null && (
        <MeshResourceBrowser
          kind="tool_mesh"
          startPath={meshFolder}
          onSelect={(entryPath) => {
            edit.set(path, relativeMeshPath(meshFolder, entryPath));
            setBrowserOpen(false);
          }}
        />
      )}
    </>
  );
}

function Geometry({ edit, label, path }: { edit: Edit; label: string; path: Path }) {
  const kind = geometryKind(at(edit.parameters, path));
  return (
    <div className="deployment-focused-form-grid">
      <label className="deployment-focused-field">
        <span>{label}</span>
        <select aria-label={label} value={kind ?? ''} disabled={edit.disabled}
          onChange={(event) => edit.set(path, newGeometry(event.target.value as GeometryKind))}>
          {kind === null && <option value="">None</option>}
          {GEOMETRY_KINDS.map((item) => <option key={item} value={item}>{item === 'mesh' ? 'Mesh file' : item}</option>)}
        </select>
      </label>
      {kind === 'box' && (
        <Vector edit={edit} label="Size" path={[...path, 'box', 'size_m']} unit="m"
          values={list(at(edit.parameters, [...path, 'box', 'size_m'])).map(Number)}
          onCommit={(next) => edit.set([...path, 'box', 'size_m'], next)} />
      )}
      {(kind === 'cylinder' || kind === 'sphere') && (
        <NumberField edit={edit} label="Radius" path={[...path, kind, 'radius_m']} unit="m" exclusiveMin={0} />
      )}
      {kind === 'cylinder' && <NumberField edit={edit} label="Length" path={[...path, 'cylinder', 'length_m']} unit="m" exclusiveMin={0} />}
      {kind === 'mesh' && <MeshPath edit={edit} path={[...path, 'mesh', 'path']} />}
      <FieldError edit={edit} field={fieldPath(...path)} />
    </div>
  );
}

function Parts({ edit, label, path, minimum = 1 }: { edit: Edit; label: string; path: Path; minimum?: number }) {
  const parts = list(at(edit.parameters, path));
  return (
    <fieldset className="deployment-initial-joints">
      <legend>{label}</legend>
      {parts.map((_, index) => (
        <div key={index} className="deployment-initial-joints">
          <Text edit={edit} label={`${label} ${index + 1} name`} path={[...path, index, 'name']} validate={idProblem} />
          <Geometry edit={edit} label={`${label} ${index + 1} shape`} path={[...path, index, 'collision']} />
          <Pose edit={edit} label={`${label} ${index + 1}`} path={[...path, index, 'pose']} />
          <div className="button-row">
            <button type="button" disabled={edit.disabled || parts.length <= minimum}
              onClick={() => edit.set([...path, index], undefined)}>Remove part</button>
          </div>
        </div>
      ))}
      <div className="button-row">
        <button type="button" disabled={edit.disabled}
          onClick={() => edit.set([...path, parts.length],
            newPart(uniqueId('part', partNames(edit.parameters))))}>Add part</button>
      </div>
      <FieldError edit={edit} field={fieldPath(...path)} />
    </fieldset>
  );
}

function Element({ edit, chain, index }: { edit: Edit; chain: number; index: number }) {
  const path: Path = ['chains', chain, 'elements', index];
  const row = object(at(edit.parameters, path)) ?? {};
  const kind = (Object.keys(row)[0] ?? 'fixed') as ElementKind;
  const own: Path = [...path, kind];
  const fields = object(row[kind]) ?? {};
  const id = typeof fields.id === 'string' ? fields.id : '';
  const pads = object(fields.pads);
  return (
    <div className="deployment-initial-joints">
      <label className="deployment-focused-field">
        <span>Element type</span>
        <select aria-label={`Element ${id} type`} value={kind} disabled={edit.disabled}
          onChange={(event) => edit.set(path, newElement(event.target.value as ElementKind, id,
            partNames(edit.parameters)))}>
          {ELEMENT_KINDS.map((item) => <option key={item} value={item}>{ELEMENT_KIND_LABELS[item]}</option>)}
        </select>
      </label>
      <Text edit={edit} label={kind === 'fixed' ? 'Body id' : 'Tool end id'} path={[...own, 'id']} validate={idProblem} />
      <Pose edit={edit} label="Pose" path={[...own, 'pose']} />
      {kind !== 'jaw_pair' && <Parts edit={edit} label="Parts" path={[...own, 'parts']} />}
      {kind === 'spring_slider' && (
        <>
          <Vector edit={edit} label="Axis" path={[...own, 'axis']} unit=""
            values={list(fields.axis ?? [0, 0, 1]).map(Number)} onCommit={(next) => edit.set([...own, 'axis'], next)} />
          <NumberField edit={edit} label="Stroke" path={[...own, 'stroke_m']} unit="m" exclusiveMin={0} />
          <NumberField edit={edit} label="Trip depth" path={[...own, 'trip_depth_m']} unit="m" exclusiveMin={0} />
          <p className="field-help">Release depth (optional, below the trip depth): a Mock input that follows the slider turns false again only below it; without it, as soon as the compression is no longer past the trip depth.</p>
          <NumberField edit={edit} label="Release depth" path={[...own, 'release_depth_m']} unit="m" exclusiveMin={0}
            optional={releaseDepthStart(fields.trip_depth_m)} />
          <label className="toggle-control">
            <input type="checkbox" checked={fields.swept_space === true} disabled={edit.disabled}
              onChange={(event) => edit.set([...own, 'swept_space'], event.target.checked ? true : undefined)} />
            Swept space (a fixed collision body over the whole stroke)
          </label>
          <FieldError edit={edit} field={fieldPath(...own, 'swept_space')} />
        </>
      )}
      {kind === 'jaw_pair' && (
        <>
          <Parts edit={edit} label="Positive jaw" path={[...own, 'positive_jaw']} />
          <label className="toggle-control">
            <input type="checkbox" checked={fields.negative_jaw === undefined} disabled={edit.disabled}
              onChange={(event) => edit.set([...own, 'negative_jaw'], event.target.checked
                ? undefined : [newPart(uniqueId(`${id}_negative_jaw`, partNames(edit.parameters)))])} />
            Negative jaw mirrors the positive jaw
          </label>
          {fields.negative_jaw !== undefined && <Parts edit={edit} label="Negative jaw" path={[...own, 'negative_jaw']} />}
          <label className="toggle-control">
            <input type="checkbox" checked={pads !== null} disabled={edit.disabled}
              onChange={(event) => edit.set([...own, 'pads'], event.target.checked
                ? { positive: [newPart(uniqueId(`${id}_pad`, partNames(edit.parameters)))] } : undefined)} />
            Pads (only the pads touch the part)
          </label>
          {pads !== null && <Parts edit={edit} label="Pads" path={[...own, 'pads', 'positive']} />}
          <NumberField edit={edit} label="Min aperture" path={[...own, 'min_aperture_m']} unit="m" min={0} />
          <NumberField edit={edit} label="Max aperture" path={[...own, 'max_aperture_m']} unit="m" exclusiveMin={0} />
          <p className="field-help">Open and closed apertures default to the max and min aperture; with pads the closed aperture (the assumed grasp) is required.</p>
          <NumberField edit={edit} label="Open aperture" path={[...own, 'open_aperture_m']} unit="m" min={0}
            optional={Number(fields.max_aperture_m ?? 0)} />
          <NumberField edit={edit} label="Closed aperture" path={[...own, 'closed_aperture_m']} unit="m" min={0}
            optional={Number(fields.min_aperture_m ?? 0)} />
          <NumberField edit={edit} label="Motion time" path={[...own, 'command_duration_s']} unit="s" exclusiveMin={0}
            optional={0.8} />
        </>
      )}
      <FieldError edit={edit} field={fieldPath(...path)} />
      <div className="button-row">
        <button type="button" disabled={edit.disabled || list(at(edit.parameters, ['chains', chain, 'elements'])).length <= 1}
          onClick={() => edit.set(path, undefined)}>Remove element</button>
      </div>
    </div>
  );
}

function Chains({ edit }: { edit: Edit }) {
  const chains = list(edit.parameters.chains);
  const all = elements(edit.parameters);
  const [adding, setAdding] = useState<ElementKind>('fixed');
  const taken = () => [...all.map((element) => element.id), 'body', 'tool'];
  return (
    <section className="dcard" aria-labelledby="generated-tool-chains">
      <h4 id="generated-tool-chains">Chains</h4>
      <p className="field-help">Each chain is attached to the body or an element of an earlier chain; its fixed bodies lead to its tool ends (jaw pairs, spring sliders, probe tips).</p>
      {chains.map((value, chain) => {
        const row = object(value) ?? {};
        const attachTo = typeof row.attach_to === 'string' ? row.attach_to : 'body';
        const earlier = all.filter((element) => element.chain < chain);
        const onPair = earlier.find((element) => element.id === attachTo)?.kind === 'jaw_pair';
        return (
          <fieldset key={chain} className="deployment-initial-joints">
            <legend>Chain {chain + 1}</legend>
            <label className="deployment-focused-field">
              <span>Attached to</span>
              <select aria-label={`Chain ${chain + 1} attached to`} value={attachTo} disabled={edit.disabled}
                onChange={(event) => edit.set(['chains', chain], {
                  ...withValue(row, ['jaw'], undefined) as JsonObject, attach_to: event.target.value,
                })}>
                <option value="body">Body</option>
                {earlier.map((element) => <option key={element.id} value={element.id}>{element.id}</option>)}
              </select>
              <FieldError edit={edit} field={fieldPath('chains', chain, 'attach_to')} />
            </label>
            {onPair && (
              <label className="deployment-focused-field">
                <span>On</span>
                <select aria-label={`Chain ${chain + 1} jaw`} value={typeof row.jaw === 'string' ? row.jaw : ''}
                  disabled={edit.disabled}
                  onChange={(event) => edit.set(['chains', chain, 'jaw'], event.target.value || undefined)}>
                  <option value="">The pair frame</option>
                  <option value="positive">The positive jaw</option>
                  <option value="negative">The negative jaw</option>
                </select>
              </label>
            )}
            <Pose edit={edit} label="Chain pose" path={['chains', chain, 'pose']} />
            {list(row.elements).map((_, index) => <Element key={index} edit={edit} chain={chain} index={index} />)}
            <div className="button-row">
              <select aria-label={`Chain ${chain + 1} new element type`} value={adding} disabled={edit.disabled}
                onChange={(event) => setAdding(event.target.value as ElementKind)}>
                {ELEMENT_KINDS.map((item) => <option key={item} value={item}>{ELEMENT_KIND_LABELS[item]}</option>)}
              </select>
              <button type="button" disabled={edit.disabled}
                onClick={() => edit.set(['chains', chain, 'elements', list(row.elements).length],
                  newElement(adding, uniqueId(adding === 'jaw_pair' ? 'jaws' : adding, taken()),
                    partNames(edit.parameters)))}>Add element</button>
              <button type="button" disabled={edit.disabled}
                onClick={() => edit.set(['chains', chain], undefined)}>Remove chain</button>
            </div>
          </fieldset>
        );
      })}
      <div className="button-row">
        <button type="button" disabled={edit.disabled}
          onClick={() => edit.set(['chains', chains.length], {
            attach_to: 'body', elements: [newElement('fixed', uniqueId('fixed', taken()), partNames(edit.parameters))],
          })}>Add chain</button>
      </div>
      <FieldError edit={edit} field="chains" />
    </section>
  );
}

function Tcps({ edit, collisionBodies }: { edit: Edit; collisionBodies: string[] }) {
  const tcps = list(edit.parameters.tcps);
  const ends = toolEnds(edit.parameters);
  return (
    <section className="dcard" aria-labelledby="generated-tool-tcps">
      <h4 id="generated-tool-tcps">TCPs</h4>
      <p className="field-help">The first TCP (id tcp) is the tool&apos;s default. A TCP belongs to a tool end or to the body; its contact bodies default to its tool end&apos;s.</p>
      {tcps.map((value, index) => {
        const row = object(value) ?? {};
        const attachTo = typeof row.attach_to === 'string' ? row.attach_to : 'body';
        const contact = Array.isArray(row.contact_bodies) ? row.contact_bodies.map(String) : null;
        const fallback = defaultContactBodies(edit.parameters, attachTo);
        return (
          <fieldset key={index} className="deployment-initial-joints">
            <legend>{index === 0 ? 'Default TCP' : `TCP ${index + 1}`}</legend>
            <Text edit={edit} label={`TCP ${index + 1} id`} path={['tcps', index, 'id']} validate={idProblem} />
            <label className="deployment-focused-field">
              <span>Belongs to</span>
              <select aria-label={`TCP ${index + 1} belongs to`} value={attachTo} disabled={edit.disabled}
                onChange={(event) => edit.set(['tcps', index, 'attach_to'],
                  event.target.value === 'body' ? undefined : event.target.value)}>
                <option value="body">Body</option>
                {ends.map((end) => <option key={end.id} value={end.id}>{end.id} ({ELEMENT_KIND_LABELS[end.kind]})</option>)}
              </select>
              <FieldError edit={edit} field={fieldPath('tcps', index, 'attach_to')} />
            </label>
            <Pose edit={edit} label={`TCP ${index + 1}`} path={['tcps', index, 'pose']} />
            <fieldset className="deployment-initial-joints">
              <legend>Contact bodies</legend>
              <label className="toggle-control">
                <input type="checkbox" checked={contact === null} disabled={edit.disabled}
                  onChange={(event) => edit.set(['tcps', index, 'contact_bodies'],
                    event.target.checked ? undefined : fallback)} />
                {`Default: ${fallback.length === 0 ? 'none' : fallback.join(', ')}`}
              </label>
              {contact !== null && collisionBodies.map((body) => (
                <label key={body} className="toggle-control">
                  <input type="checkbox" checked={contact.includes(body)} disabled={edit.disabled}
                    onChange={(event) => edit.set(['tcps', index, 'contact_bodies'], event.target.checked
                      ? [...contact, body] : contact.filter((item) => item !== body))} />
                  {body}
                </label>
              ))}
              <FieldError edit={edit} field={fieldPath('tcps', index, 'contact_bodies')} />
            </fieldset>
            <div className="button-row">
              <button type="button" disabled={edit.disabled || index === 0}
                onClick={() => edit.set(['tcps', index], undefined)}>Remove TCP</button>
            </div>
          </fieldset>
        );
      })}
      <div className="button-row">
        <button type="button" disabled={edit.disabled}
          onClick={() => edit.set(['tcps', tcps.length], {
            id: uniqueId('tcp_2', tcps.map((tcp) => String(object(tcp)?.id ?? ''))),
          })}>Add TCP</button>
      </div>
      <FieldError edit={edit} field="tcps" />
    </section>
  );
}

function Mass({ edit }: { edit: Edit }) {
  const mass = edit.parameters.mass_kg;
  return (
    <section className="dcard" aria-labelledby="generated-tool-mass">
      <h4 id="generated-tool-mass">Mass</h4>
      {typeof mass !== 'number' ? (
        <>
          <p className="field-help">From the parts&apos; own masses (none: 0 kg). The robot&apos;s payload adds the tool&apos;s mass while it is mounted.</p>
          <div className="button-row">
            <button type="button" disabled={edit.disabled} onClick={() => edit.set(['mass_kg'], 0.1)}>Set the tool&apos;s mass</button>
          </div>
        </>
      ) : (
        <>
          <NumberField edit={edit} label="Mass" path={['mass_kg']} unit="kg" min={0} />
          <Vector edit={edit} label="Centre of mass" path={['center_of_mass_m']} unit="m"
            values={list(edit.parameters.center_of_mass_m ?? [0, 0, 0]).map(Number)}
            onCommit={(next) => edit.set(['center_of_mass_m'], next)} />
          <div className="button-row">
            <button type="button" disabled={edit.disabled} onClick={() => {
              edit.set(['mass_kg'], undefined);
            }}>Use the parts&apos; masses</button>
          </div>
        </>
      )}
      <FieldError edit={edit} field="mass_kg" />
    </section>
  );
}

function Wiring({ controller, componentId, index, wiring, ends, document, disabled }: {
  controller: DeploymentWorkspaceController;
  componentId: string;
  index: number;
  wiring: JsonObject[];
  ends: string[];
  document: Record<string, JsonValue>;
  disabled: boolean;
}) {
  const [error, setError] = useState<string | null>(null);
  const runtimePath: Path = ['components', index, 'configuration', 'runtime_configuration'];
  const runtime = object(at(document, runtimePath)) ?? {};
  const write = async (end: string, value: JsonValue | undefined, whole: boolean) => {
    if (whole && value !== undefined) {
      const problem = pneumaticIoProblem(value);
      if (problem !== null) { setError(problem); return null; }
    }
    setError(null);
    return controller.mutate(runtimePath, 'set',
      { ...runtime, gripper_target_configurations: withWiring(wiring, componentId, end, value) });
  };
  return (
    <section className="dcard" aria-labelledby="generated-tool-wiring">
      <h4 id="generated-tool-wiring">Wiring</h4>
      {ends.length === 0 ? (
        <p className="field-help">No actuated tool end: a jaw pair needs its valve outputs here.</p>
      ) : (
        <p className="field-help">Each jaw pair needs its valve outputs (one, or two for open and close) from the cell&apos;s declared output signals; its opened and closed sensors are optional. States, motion time and frames come from the parameters.</p>
      )}
      {ends.map((end) => (
        <div key={end} className="deployment-initial-joints">
          <strong>{`${componentId} · ${end}`}</strong>
          <PneumaticIoEditor value={wiring.find((row) => row.command_target_id === end)?.pneumatic_io}
            enabled disabled={disabled}
            inputs={declaredIoInputs(document)} outputs={declaredIoOutputs(document)}
            onChange={(value, path) => write(end, value, path === undefined)} />
        </div>
      ))}
      {error !== null && <p className="inline-message severity-error" role="alert">{error}</p>}
    </section>
  );
}

/** The tool's opacity in the preview unless drawn opaque: inner parts and TCP frames show through. */
export const TOOL_PREVIEW_OPACITY = 0.5;

/** A primitive part's material: the primitive colour at `opacity`. */
function PreviewMaterial({ opacity }: { opacity: number }) {
  return <meshStandardMaterial color={VIEWPORT_COLORS.primitive} transparent={opacity < 1}
    opacity={opacity} depthWrite={opacity >= 1} />;
}

/** A mesh part's GLB, shaded like the primitive parts (as the cell viewer shades converted STL files: no material, no normals). */
function PreviewMeshAsset({ url, opacity, onLoaded }: { url: string; opacity: number; onLoaded: () => void }) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => {
    const root = gltf.scene.clone(true);
    root.traverse((object) => {
      const mesh = object as THREE.Mesh;
      if (!mesh.isMesh) return;
      mesh.material = new THREE.MeshStandardMaterial({
        color: VIEWPORT_COLORS.primitive, metalness: 0, roughness: 1,
        flatShading: !mesh.geometry.hasAttribute('normal'), side: THREE.DoubleSide,
        transparent: opacity < 1, opacity, depthWrite: opacity >= 1,
      });
    });
    return root;
  }, [gltf.scene, opacity]);
  useEffect(() => () => clone.traverse((object) => {
    const mesh = object as THREE.Mesh;
    if (mesh.isMesh) (mesh.material as THREE.Material).dispose();
  }), [clone]);
  // Once loaded (Suspense resolved): the preview may frame the tool again.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(onLoaded, [gltf.scene]);
  return <primitive object={clone} />;
}

function PreviewShape({ shape, opacity, failedUrls, onMeshError, onMeshLoaded }: {
  shape: ToolPreviewShape;
  opacity: number;
  failedUrls: ReadonlySet<string>;
  onMeshError: (url: string) => void;
  onMeshLoaded: () => void;
}) {
  const [kind] = Object.keys(shape.geometry);
  const fields = shape.geometry[kind];
  const number = (name: string) => Number(fields[name]);
  const url = shape.mesh_asset?.url ?? null;
  return (
    <group position={shape.position_m} quaternion={shape.orientation_xyzw}>
      {kind === 'mesh' && (url === null || failedUrls.has(url)
        ? (
          <mesh><boxGeometry args={[0.02, 0.02, 0.02]} />
            <meshBasicMaterial color={VIEWPORT_COLORS.error} wireframe /></mesh>
        )
        : (
          <AssetErrorBoundary resetKey={url} onError={() => onMeshError(url)}>
            <Suspense fallback={null}>
              <PreviewMeshAsset url={url} opacity={opacity} onLoaded={onMeshLoaded} />
            </Suspense>
          </AssetErrorBoundary>
        ))}
      {kind === 'box' && (
        <mesh><boxGeometry args={list(fields.size_m).map(Number) as [number, number, number]} />
          <PreviewMaterial opacity={opacity} /></mesh>
      )}
      {kind === 'cylinder' && (
        // URDF cylinders run along z, three.js ones along y.
        <mesh rotation={[Math.PI / 2, 0, 0]}>
          <cylinderGeometry args={[number('radius_m'), number('radius_m'), number('length_m'), 24]} />
          <PreviewMaterial opacity={opacity} /></mesh>
      )}
      {kind === 'sphere' && (
        <mesh><sphereGeometry args={[number('radius_m'), 24, 16]} /><PreviewMaterial opacity={opacity} /></mesh>
      )}
    </group>
  );
}

const TOOL_CONTENT = 'tool-preview-content';

/** Frames the drawn tool (its bounding box) each time `request` changes, as the cell viewer's Frame all. */
function FitTool({ request }: { request: number }) {
  const { camera, controls, scene, invalidate } = useThree();
  useEffect(() => {
    const content = scene.getObjectByName(TOOL_CONTENT);
    const bounds = content === undefined ? null : frameableContentBounds(content);
    if (bounds === null || bounds.isEmpty()) return;
    const perspective = camera as THREE.PerspectiveCamera;
    const orbit = controls as { target?: THREE.Vector3; update?: () => void } | null;
    const target = orbit?.target ?? new THREE.Vector3();
    const framed = framedCameraPresentation(bounds, {
      position: [camera.position.x, camera.position.y, camera.position.z],
      target: [target.x, target.y, target.z],
    }, perspective.fov, perspective.aspect);
    camera.position.fromArray(framed.camera.position);
    orbit?.target?.fromArray(framed.camera.target);
    orbit?.update?.();
    invalidate();
  }, [request, camera, controls, scene, invalidate]);
  return null;
}

/** The generated tool in its mount frame (z out of the flange), jaws open, with its TCP frames. */
export function ToolPreview({ preview }: { preview: Extract<ToolPreviewResponse, { ok: true }> }) {
  // GLBs that failed to load, for this preview: the next preview retries them.
  const [failed, setFailed] = useState<{ preview: typeof preview; urls: ReadonlySet<string> } | null>(null);
  const failedUrls = failed?.preview === preview ? failed.urls : new Set<string>();
  const onMeshError = (url: string) => {
    (useGLTF as unknown as { clear?: (url: string) => void }).clear?.(resolveStructuralAssetUrl(url));
    setFailed((current) => ({
      preview, urls: new Set<string>(current?.preview === preview ? current.urls : []).add(url),
    }));
  };
  const [opaque, setOpaque] = useState(false);
  const opacity = opaque ? 1 : TOOL_PREVIEW_OPACITY;
  // Fit when the preview first shows the tool, and again as its meshes load,
  // until the camera is moved; then only on Fit.
  const [fitRequest, setFitRequest] = useState(1);
  const following = useRef(true);
  const onMeshLoaded = () => { if (following.current) setFitRequest((value) => value + 1); };
  const notDrawn = preview.shapes.flatMap((shape) => {
    const asset = shape.mesh_asset;
    if (asset === null) return [];
    const path = String(shape.geometry.mesh.path);
    if (asset.error !== null) return [`${path} (${asset.error.message})`];
    return failedUrls.has(asset.url) ? [`${path} (could not be loaded)`] : [];
  });
  return (
    <figure className="generated-tool-preview" aria-label="Tool preview">
      <div className="viewer-tool-strip generated-tool-preview-tools" aria-label="Tool preview controls">
        <button type="button" aria-label="Fit the tool in the preview" title="Fit the tool in the preview"
          onClick={() => setFitRequest((value) => value + 1)}>
          <svg className="viewer-camera-icon" viewBox="0 0 16 16" aria-hidden="true">
            <rect x="2.25" y="2.25" width="11.5" height="11.5" rx="1.5" />
            <path d="M8 2.25v11.5M2.25 8h11.5" />
          </svg>
        </button>
        <button type="button" className="viewer-text-control" aria-pressed={opaque}
          title={opaque ? 'Draw the tool 50% transparent' : 'Draw the tool opaque'}
          onClick={() => setOpaque((value) => !value)}>Opaque</button>
      </div>
      <div className="generated-tool-preview-canvas">
        <Canvas camera={{ position: [0.25, -0.25, 0.25], up: [0, 0, 1], near: 0.001, far: 10 }} frameloop="demand">
          <ambientLight intensity={0.6} />
          <directionalLight position={[1, -1, 2]} intensity={0.8} />
          <axesHelper args={[0.05]} />
          <group name={TOOL_CONTENT}>
            {preview.shapes.map((shape) => (
              <PreviewShape key={`${shape.link}/${shape.part}`} shape={shape} opacity={opacity}
                failedUrls={failedUrls} onMeshError={onMeshError} onMeshLoaded={onMeshLoaded} />
            ))}
          </group>
          {preview.tcpFrames.map((tcp) => (
            <group key={tcp.id} position={tcp.position_m} quaternion={tcp.orientation_xyzw}>
              <axesHelper args={[0.02]} />
            </group>
          ))}
          <OrbitControls makeDefault target={[0, 0, 0.05]} onStart={() => { following.current = false; }} />
          <FitTool request={fitRequest} />
        </Canvas>
      </div>
      <figcaption className="field-help">
        Mount frame at the origin; TCPs as small axes.
        {notDrawn.length > 0 && ` Meshes not drawn (red boxes): ${notDrawn.join('; ')}.`}
      </figcaption>
    </figure>
  );
}

/**
 * A Tool made from its parameters (`manyforge.tool`, OD-357), on its component
 * page: Body, Chains, TCPs, Wiring and Mass, a preview of the generated tool,
 * and Export. Every edit changes the Draft (Apply loads it); the backend's
 * check of the parameters shows on the refused field.
 */
export function GeneratedToolEditor({ controller, componentId }: {
  controller: DeploymentWorkspaceController;
  componentId: string;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const row = useMemo(() => generatedToolRow(document, componentId), [document, componentId]);
  const [preview, setPreview] = useState<ToolPreviewResponse | null>(null);
  const [exported, setExported] = useState<string | null>(null);
  const [copyMeshes, setCopyMeshes] = useState(false);
  const source = JSON.stringify(row?.source ?? null);
  useEffect(() => {
    if (row === null) return undefined;
    const abort = new AbortController();
    api.deploymentWorkspace.toolPreview(componentId, row.source, abort.signal)
      .then(setPreview)
      .catch((error: unknown) => {
        if (!abort.signal.aborted) {
          setPreview({ ok: false, meshFolder: null,
            error: { code: 'tool_preview_unavailable', message: String(error), field: null } });
        }
      });
    return () => abort.abort();
    // The row's parameters, by value.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [componentId, source]);
  if (row === null || document === null) return null;
  const disabled = !controller.mutationAvailable || controller.busy;
  const refusal = preview !== null && !preview.ok ? preview.error : null;
  const parameters = row.parameters;
  const edit: Edit | null = parameters === null ? null : {
    parameters,
    disabled,
    refusal,
    meshFolder: preview?.meshFolder ?? null,
    set: (path, value) => {
      const next = withValue(parameters, path, value) as JsonObject;
      void controller.mutate(parametersPath(row.index), 'set', next).then((response) => {
        const followed = response === null ? null : followedWiring(row.wiring, parameters, next);
        if (followed === null) return;
        const runtimePath: Path = ['components', row.index, 'configuration', 'runtime_configuration'];
        void controller.mutate(runtimePath, 'set',
          { ...(object(at(document, runtimePath)) ?? {}), gripper_target_configurations: followed });
      });
    },
  };
  const ends = preview?.ok ? preview.toolEnds.filter((end) => end.actuated).map((end) => end.id)
    : jawPairs(parameters).map((pair) => pair.id);
  const shownOnAField = refusal !== null && refusal.field !== null && parameters !== null
    && ['body', 'chains', 'tcps', 'mass_kg', 'center_of_mass_m'].some((field) => refusalAt(refusal.field, field));
  return (
    <>
      <section className="dcard" aria-labelledby="generated-tool-title">
        <h4 id="generated-tool-title">Tool</h4>
        <p className="field-help">One component, mounted as a whole: its body, the chains leading to its tool ends, its TCPs. Changes are Draft edits; Apply loads them.</p>
        {row.reference !== null && (
          <p>Parameters from the file <code>{row.reference}</code>; edit the file, or Export the model it generates.</p>
        )}
        {preview?.ok && <ToolPreview preview={preview} />}
        {refusal !== null && !shownOnAField && (
          <p className="inline-message severity-error" role="alert">{refusal.message}</p>
        )}
        {exported !== null && <p className="inline-message" role="status">{exported}</p>}
        <label className="toggle-control">
          <input type="checkbox" checked={copyMeshes} aria-label="Copy meshes into the export folder"
            onChange={(event) => setCopyMeshes(event.target.checked)} />
          Copy meshes into the export folder
        </label>
        <div className="dactions">
          <button type="button" disabled={refusal !== null}
            title={copyMeshes
              ? 'Write the generated URDF and SRDF into the project folder, under the component id, with copies of the meshes beside them.'
              : 'Write the generated URDF and SRDF into the project folder, under the component id; they name the meshes where they are.'}
            onClick={() => {
              setExported(null);
              api.deploymentWorkspace.toolExport(componentId, row.source, copyMeshes)
                .then((result) => setExported(`Exported ${result.written.join(', ')} into the project folder.`))
                .catch((error: unknown) => setExported(`Export failed: ${String(error)}`));
            }}>Export</button>
        </div>
      </section>
      {edit !== null && (
        <section className="dcard" aria-labelledby="generated-tool-body">
          <h4 id="generated-tool-body">Body</h4>
          <Parts edit={edit} label="Body part" path={['body']} />
        </section>
      )}
      {edit !== null && <Chains edit={edit} />}
      {edit !== null && <Tcps edit={edit} collisionBodies={preview?.ok ? preview.collisionBodies : []} />}
      <Wiring controller={controller} componentId={componentId} index={row.index} wiring={row.wiring}
        ends={ends} document={document} disabled={disabled} />
      {edit !== null && <Mass edit={edit} />}
    </>
  );
}
