import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useEditLocked } from '../store/cycleStore';
import { useProgramStore } from '../store/programStore';
import { useSceneStateStore } from '../store/sceneStateStore';
import type {
  ProgramSceneResource,
  ProgramCompatibility,
  SceneObjectState,
} from '../types/api';

type SceneShapeType = 'box' | 'sphere' | 'cylinder' | 'mesh';
type SupportContactMode = 'contact_release' | 'bounded_penetration' | 'contact_process';
type ContactReleaseEvidenceMode = 'signed_penetration' | 'binary_contact';
type SupportContactModality = ContactReleaseEvidenceMode | 'bounded_penetration' | 'contact_process';
const EMPTY_SCENE_FRAMES: import('../types/api').SceneFrameState[] = [];

interface SupportContactRow {
  rowKey: string;
  supportedObjectId: string;
  supportObjectId: string;
  contact: Record<string, unknown>;
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h3 className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-400">
        {title}
      </h3>
      <div className="space-y-2">{children}</div>
    </div>
  );
}

function NumericInput({
  label,
  value,
  onChange,
  step = '0.001',
  disabled = false,
}: {
  label: string;
  value: number;
  onChange: (value: number) => void;
  step?: string;
  disabled?: boolean;
}) {
  // Internally a draft-style input: local state during typing, commit on
  // blur/Enter, revert on Escape. Same pattern as NumericParamEditor in
  // ../property/PrimitiveParameterEditors.tsx — see DraftInputs.tsx for
  // the full rationale on skipNextCommit. Step is kept for visual hints
  // but ignored at parse time (we accept any finite number on commit).
  const [draft, setDraft] = useState(() => (Number.isFinite(value) ? String(value) : '0'));
  const [isFocused, setIsFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(Number.isFinite(value) ? String(value) : '0');
  }, [value, isFocused]);

  const commitDraft = () => {
    const trimmed = draft.trim();
    if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
      setDraft(Number.isFinite(value) ? String(value) : '0');
      return;
    }
    const next = Number(trimmed);
    if (!Number.isFinite(next)) {
      setDraft(Number.isFinite(value) ? String(value) : '0');
      return;
    }
    if (next !== value) {
      onChange(next);
    }
    setDraft(String(next));
  };

  return (
    <label className="block">
      <span className="text-[11px] text-gray-400">{label}</span>
      <input
        type="text"
        inputMode="decimal"
        step={step}
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            skipNextCommit.current = false;
            setDraft(Number.isFinite(value) ? String(value) : '0');
            return;
          }
          commitDraft();
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
            return;
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            skipNextCommit.current = true;
            setDraft(Number.isFinite(value) ? String(value) : '0');
            event.currentTarget.blur();
          }
        }}
        disabled={disabled}
        className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
      />
    </label>
  );
}

function TextInput({
  label,
  value,
  onChange,
  disabled = false,
  placeholder,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  disabled?: boolean;
  placeholder?: string;
}) {
  // Same draft-style pattern as NumericInput above. The component owns
  // the local draft while focused so per-keystroke renders don't
  // cascade into the program store and every other panel subscriber.
  const [draft, setDraft] = useState(value);
  const [isFocused, setIsFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(value);
  }, [value, isFocused]);

  return (
    <label className="block">
      <span className="text-[11px] text-gray-400">{label}</span>
      <input
        type="text"
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            skipNextCommit.current = false;
            setDraft(value);
            return;
          }
          if (draft !== value) {
            onChange(draft);
          }
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
            return;
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            skipNextCommit.current = true;
            setDraft(value);
            event.currentTarget.blur();
          }
        }}
        disabled={disabled}
        placeholder={placeholder}
        className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
      />
    </label>
  );
}

function readPoseVector(
  pose: Record<string, unknown> | null | undefined,
  key: 'position_m' | 'orientation_xyzw' | 'orientation_rpy_deg',
  expectedLength: number,
): number[] | null {
  const raw = pose?.[key];
  if (!Array.isArray(raw)) {
    return null;
  }
  const values = raw.map((entry) => Number(entry));
  if (values.length !== expectedLength || !values.every((entry) => Number.isFinite(entry))) {
    console.warn(`Invalid pose vector for ${key}; expected ${expectedLength} finite values`, raw);
    return null;
  }
  return values;
}

function buildPoseOrientationQuaternion(
  pose: Record<string, unknown> | null | undefined,
): number[] {
  const quaternion = readPoseVector(pose, 'orientation_xyzw', 4);
  if (quaternion != null) {
    return quaternion;
  }

  const rpyDegrees = readPoseVector(pose, 'orientation_rpy_deg', 3);
  if (rpyDegrees != null) {
    return rpyDegreesToQuaternion(rpyDegrees);
  }

  return [0, 0, 0, 1];
}

function degToRad(value: number): number {
  return (value * Math.PI) / 180.0;
}

function radToDeg(value: number): number {
  return (value * 180.0) / Math.PI;
}

function quaternionToRpyDegrees(quaternion: number[]): number[] {
  const [x, y, z, w] = quaternion;

  const sinrCosp = 2 * (w * x + y * z);
  const cosrCosp = 1 - 2 * (x * x + y * y);
  const roll = Math.atan2(sinrCosp, cosrCosp);

  const sinp = 2 * (w * y - z * x);
  const pitch = Math.abs(sinp) >= 1 ? Math.sign(sinp) * Math.PI / 2 : Math.asin(sinp);

  const sinyCosp = 2 * (w * z + x * y);
  const cosyCosp = 1 - 2 * (y * y + z * z);
  const yaw = Math.atan2(sinyCosp, cosyCosp);

  return [roll, pitch, yaw].map((value) => Number(radToDeg(value).toFixed(6)));
}

function rpyDegreesToQuaternion(rpyDegrees: number[]): number[] {
  const [roll, pitch, yaw] = rpyDegrees.map(degToRad);
  const cy = Math.cos(yaw * 0.5);
  const sy = Math.sin(yaw * 0.5);
  const cp = Math.cos(pitch * 0.5);
  const sp = Math.sin(pitch * 0.5);
  const cr = Math.cos(roll * 0.5);
  const sr = Math.sin(roll * 0.5);

  const w = cr * cp * cy + sr * sp * sy;
  const x = sr * cp * cy - cr * sp * sy;
  const y = cr * sp * cy + sr * cp * sy;
  const z = cr * cp * sy - sr * sp * cy;
  return [x, y, z, w];
}

function sceneResourcePose(resource: ProgramSceneResource): {
  frameId: string;
  position: number[];
  rpyDegrees: number[];
} {
  const pose = resource.pose ?? {};
  return {
    frameId: typeof pose.frame_id === 'string' && pose.frame_id.trim() ? pose.frame_id : 'base_link',
    position: readPoseVector(pose, 'position_m', 3) ?? [0, 0, 0],
    rpyDegrees: quaternionToRpyDegrees(buildPoseOrientationQuaternion(pose)),
  };
}

function buildSceneResource(
  objectId: string,
  shape: Record<string, unknown>,
  frameId: string,
  position: number[],
  rpyDegrees: number[],
  metadata?: Record<string, unknown>,
): ProgramSceneResource {
  return {
    objectId,
    shape,
    pose: {
      frame_id: frameId,
      position_m: [...position],
      orientation_xyzw: rpyDegreesToQuaternion(rpyDegrees),
    },
    ...(metadata ? { metadata } : {}),
  };
}

function cloneMetadata(metadata: Record<string, unknown> | null | undefined): Record<string, unknown> {
  return structuredClone(metadata ?? {});
}

function supportContactRowsFromResources(resources: ProgramSceneResource[]): SupportContactRow[] {
  return resources.flatMap((resource) => {
    const contacts = resource.metadata?.support_contacts;
    if (!Array.isArray(contacts)) {
      return [];
    }
    return contacts.flatMap((entry, index) => {
      if (entry == null || typeof entry !== 'object' || Array.isArray(entry)) {
        return [];
      }
      const contact = entry as Record<string, unknown>;
      const supportObjectId = typeof contact.support_object_id === 'string'
        ? contact.support_object_id
        : '';
      return [{
        rowKey: `${resource.objectId}:${index}`,
        supportedObjectId: resource.objectId,
        supportObjectId,
        contact: structuredClone(contact),
      }];
    });
  });
}

export function sanitizedSupportContact(
  supportObjectId: string,
  contact: Record<string, unknown>,
): Record<string, unknown> {
  const next = structuredClone(contact);
  next.support_object_id = supportObjectId;
  const mode = contactMode(next);
  next.mode = mode;
  if (mode === 'bounded_penetration') {
    const maxPenetration = Number(next.max_contact_penetration_m);
    return {
      support_object_id: supportObjectId,
      mode,
      max_contact_penetration_m: Number.isFinite(maxPenetration) ? maxPenetration : 0,
    };
  }
  if (mode === 'contact_process') {
    return { support_object_id: supportObjectId, mode };
  }
  delete next.max_contact_penetration_m;
  if (contactReleaseEvidenceMode(next) === 'binary_contact') {
    delete next.enforce_max_start_penetration;
    delete next.max_start_penetration_m;
    delete next.release_tolerance_m;
    delete next.min_release_progress_m;
    delete next.require_strictly_decreasing_penetration;
  } else if (next.enforce_max_start_penetration === false) {
    delete next.max_start_penetration_m;
  }
  return next;
}

function applySupportContactRows(
  resources: ProgramSceneResource[],
  rows: SupportContactRow[],
): ProgramSceneResource[] {
  const grouped = new Map<string, Record<string, unknown>[]>();
  for (const row of rows) {
    const contacts = grouped.get(row.supportedObjectId) ?? [];
    contacts.push(sanitizedSupportContact(row.supportObjectId, row.contact));
    grouped.set(row.supportedObjectId, contacts);
  }
  return resources.map((resource) => {
    const metadata = cloneMetadata(resource.metadata);
    const contacts = grouped.get(resource.objectId) ?? [];
    if (contacts.length > 0) {
      metadata.support_contacts = contacts;
    } else {
      delete metadata.support_contacts;
    }
    return { ...resource, metadata };
  });
}

function duplicateSupportContact(rows: SupportContactRow[]): string | null {
  const seen = new Set<string>();
  for (const row of rows) {
    const key = `${row.supportedObjectId}\u0000${row.supportObjectId}`;
    if (seen.has(key)) {
      return `${row.supportedObjectId} -> ${row.supportObjectId}`;
    }
    seen.add(key);
  }
  return null;
}

function contactMode(contact: Record<string, unknown>): SupportContactMode {
  if (contact.mode === 'bounded_penetration') {
    return 'bounded_penetration';
  }
  return contact.mode === 'contact_process' ? 'contact_process' : 'contact_release';
}

export function supportContactModality(
  contact: Record<string, unknown>,
): SupportContactModality {
  const mode = contactMode(contact);
  return mode === 'contact_release' ? contactReleaseEvidenceMode(contact) : mode;
}

export function contactReleaseEvidenceMode(
  contact: Record<string, unknown>,
): ContactReleaseEvidenceMode {
  return contact.release_evidence_mode === 'binary_contact'
    ? 'binary_contact'
    : 'signed_penetration';
}

function contactNumber(
  contact: Record<string, unknown>,
  field: string,
  fallback: number,
): number {
  const value = Number(contact[field]);
  return Number.isFinite(value) ? value : fallback;
}

function contactBoolean(
  contact: Record<string, unknown>,
  field: string,
  fallback: boolean,
): boolean {
  return typeof contact[field] === 'boolean' ? contact[field] : fallback;
}

function replaceContactField(
  contact: Record<string, unknown>,
  field: string,
  value: unknown,
): Record<string, unknown> {
  const next = { ...contact };
  next[field] = value;
  return next;
}

function renameSupportContactReference(
  resource: ProgramSceneResource,
  previousObjectId: string,
  nextObjectId: string,
): ProgramSceneResource {
  const contacts = resource.metadata?.support_contacts;
  if (!Array.isArray(contacts)) {
    return resource;
  }
  const metadata = cloneMetadata(resource.metadata);
  metadata.support_contacts = contacts.map((entry) => {
    if (entry == null || typeof entry !== 'object' || Array.isArray(entry)) {
      return entry;
    }
    const contact = { ...(entry as Record<string, unknown>) };
    if (contact.support_object_id === previousObjectId) {
      contact.support_object_id = nextObjectId;
    }
    return contact;
  });
  return { ...resource, metadata };
}

function removeSupportContactsInvolving(
  resources: ProgramSceneResource[],
  objectId: string,
): ProgramSceneResource[] {
  const rows = supportContactRowsFromResources(resources).filter(
    (row) => row.supportedObjectId !== objectId && row.supportObjectId !== objectId,
  );
  return applySupportContactRows(resources, rows);
}

function supportContactSummary(row: SupportContactRow): string {
  return `${row.supportedObjectId} supported by ${row.supportObjectId}`;
}

function liveSupportContactSummary(contact: Record<string, unknown>): string {
  const supportObjectId = typeof contact.support_object_id === 'string'
    ? contact.support_object_id
    : 'unknown';
  const mode = contactMode(contact);
  if (mode === 'bounded_penetration') {
    const maxPenetration = contactNumber(
      contact,
      'max_contact_penetration_m',
      0,
    );
    return `${supportObjectId} (contact-tolerant, max ${maxPenetration.toFixed(3)} m)`;
  }
  if (mode === 'contact_process') {
    return `${supportObjectId} (contact process, reserved)`;
  }
  const evidence = contactReleaseEvidenceMode(contact) === 'binary_contact'
    ? 'lightweight binary'
    : 'precise penetration';
  if (contactReleaseEvidenceMode(contact) === 'binary_contact') {
    return `${supportObjectId} (release, ${evidence})`;
  }
  const enforceMax = contact.enforce_max_start_penetration !== false;
  const maxStart = Number(contact.max_start_penetration_m);
  const maxText = enforceMax && Number.isFinite(maxStart)
    ? `max ${maxStart.toFixed(3)} m`
    : 'start cap off';
  return `${supportObjectId} (release, ${evidence}, ${maxText})`;
}

function contactForModality(
  supportObjectId: string,
  contact: Record<string, unknown>,
  modality: SupportContactModality,
): Record<string, unknown> {
  if (modality === 'bounded_penetration') {
    return sanitizedSupportContact(supportObjectId, {
      ...contact,
      mode: 'bounded_penetration',
      max_contact_penetration_m: contactNumber(
        contact,
        'max_contact_penetration_m',
        0,
      ),
    });
  }
  if (modality === 'contact_process') {
    return sanitizedSupportContact(supportObjectId, {
      ...contact,
      mode: 'contact_process',
    });
  }
  return sanitizedSupportContact(supportObjectId, {
    ...defaultSupportContact(supportObjectId),
    ...contact,
    mode: 'contact_release',
    release_evidence_mode: modality,
  });
}

export function defaultSupportContact(supportObjectId: string): Record<string, unknown> {
  return {
    support_object_id: supportObjectId,
    mode: 'contact_release',
    release_evidence_mode: 'signed_penetration',
    enforce_max_start_penetration: true,
    max_start_penetration_m: 0.0,
    release_tolerance_m: 0.00001,
    min_release_progress_m: 0.0,
    require_separation_by_end: true,
    require_strictly_decreasing_penetration: true,
    allow_recontact: false,
  };
}

function shapeSummary(shape: Record<string, unknown> | null | undefined): string {
  if (!shape?.type) {
    return 'unknown';
  }
  if (shape.type === 'box') {
    const dims = Array.isArray(shape.box_dimensions_m)
      ? shape.box_dimensions_m.map((value) => Number(value).toFixed(3)).join(', ')
      : '?';
    return `box (${dims})`;
  }
  if (shape.type === 'sphere') {
    const radius = Number(shape.sphere_radius_m ?? 0).toFixed(3);
    return `sphere (r=${radius})`;
  }
  if (shape.type === 'cylinder') {
    const radius = Number(shape.cylinder_radius_m ?? 0).toFixed(3);
    const height = Number(shape.cylinder_height_m ?? 0).toFixed(3);
    return `cylinder (r=${radius}, h=${height})`;
  }
  if (shape.type === 'mesh') {
    const uri = typeof shape.mesh_resource_uri === 'string' ? shape.mesh_resource_uri : '?';
    return `mesh (${uri})`;
  }
  return String(shape.type);
}

function sourceBadge(source: SceneObjectState['source']): { label: string; classes: string } {
  if (source === 'authored') {
    return { label: 'AUTHORED', classes: 'bg-emerald-900/50 text-emerald-200 border-emerald-700' };
  }
  if (source === 'attached') {
    return { label: 'ATTACHED', classes: 'bg-violet-900/50 text-violet-200 border-violet-700' };
  }
  if (source === 'sensed') {
    return { label: 'SENSED', classes: 'bg-sky-900/50 text-sky-200 border-sky-700' };
  }
  return { label: 'RUNTIME', classes: 'bg-amber-900/50 text-amber-200 border-amber-700' };
}

function AuthoredSceneCard({
  resource,
  frameOptions,
  disabled,
  viewerAvailable,
  objectFrameVisible,
  onUpdate,
  onRemove,
  onObjectFrameVisibilityChange,
}: {
  resource: ProgramSceneResource;
  frameOptions: string[];
  disabled: boolean;
  viewerAvailable: boolean;
  objectFrameVisible: boolean;
  onUpdate: (previousObjectId: string, resource: ProgramSceneResource) => void;
  onRemove: (objectId: string) => void;
  onObjectFrameVisibilityChange: (objectId: string, visible: boolean) => void;
}) {
  const resourceSignature = JSON.stringify(resource);
  const initialPose = sceneResourcePose(resource);
  const [objectId, setObjectId] = useState(resource.objectId);
  const [shapeType, setShapeType] = useState<SceneShapeType>(
    resource.shape?.type === 'sphere' || resource.shape?.type === 'cylinder' || resource.shape?.type === 'mesh'
      ? resource.shape.type
      : 'box',
  );
  const [boxDimensions, setBoxDimensions] = useState<number[]>(
    Array.isArray(resource.shape?.box_dimensions_m)
      ? (resource.shape.box_dimensions_m as number[]).map((value) => Number(value))
      : [0.05, 0.05, 0.1],
  );
  const [sphereRadius, setSphereRadius] = useState<number>(
    Number(resource.shape?.sphere_radius_m ?? 0.05),
  );
  const [cylinderRadius, setCylinderRadius] = useState<number>(
    Number(resource.shape?.cylinder_radius_m ?? 0.05),
  );
  const [cylinderHeight, setCylinderHeight] = useState<number>(
    Number(resource.shape?.cylinder_height_m ?? 0.1),
  );
  const [meshResourceUri, setMeshResourceUri] = useState<string>(
    typeof resource.shape?.mesh_resource_uri === 'string' ? resource.shape.mesh_resource_uri : '',
  );
  const [meshScale, setMeshScale] = useState<number[]>(
    Array.isArray(resource.shape?.mesh_scale)
      ? (resource.shape.mesh_scale as number[]).map((value) => Number(value))
      : [1, 1, 1],
  );
  const [frameId, setFrameId] = useState(initialPose.frameId);
  const [position, setPosition] = useState(initialPose.position);
  const [rpyDegrees, setRpyDegrees] = useState(initialPose.rpyDegrees);

  useEffect(() => {
    const pose = sceneResourcePose(resource);
    setObjectId(resource.objectId);
    setShapeType(
      resource.shape?.type === 'sphere' || resource.shape?.type === 'cylinder' || resource.shape?.type === 'mesh'
        ? resource.shape.type
        : 'box',
    );
    setBoxDimensions(
      Array.isArray(resource.shape?.box_dimensions_m)
        ? (resource.shape.box_dimensions_m as number[]).map((value) => Number(value))
        : [0.05, 0.05, 0.1],
    );
    setSphereRadius(Number(resource.shape?.sphere_radius_m ?? 0.05));
    setCylinderRadius(Number(resource.shape?.cylinder_radius_m ?? 0.05));
    setCylinderHeight(Number(resource.shape?.cylinder_height_m ?? 0.1));
    setMeshResourceUri(
      typeof resource.shape?.mesh_resource_uri === 'string' ? resource.shape.mesh_resource_uri : '',
    );
    setMeshScale(
      Array.isArray(resource.shape?.mesh_scale)
        ? (resource.shape.mesh_scale as number[]).map((value) => Number(value))
        : [1, 1, 1],
    );
    setFrameId(pose.frameId);
    setPosition(pose.position);
    setRpyDegrees(pose.rpyDegrees);
  }, [resource, resourceSignature]);

  const commit = useCallback(() => {
    const shape =
      shapeType === 'box'
        ? { type: 'box', box_dimensions_m: boxDimensions.map((value) => Number(value)) }
        : shapeType === 'sphere'
          ? { type: 'sphere', sphere_radius_m: Number(sphereRadius) }
          : shapeType === 'cylinder'
            ? {
                type: 'cylinder',
                cylinder_radius_m: Number(cylinderRadius),
                cylinder_height_m: Number(cylinderHeight),
              }
          : {
              type: 'mesh',
              mesh_resource_uri: meshResourceUri.trim(),
              mesh_scale: meshScale.map((value) => Number(value)),
            };
    onUpdate(
      resource.objectId,
      buildSceneResource(
        objectId.trim(),
        shape,
        frameId,
        position,
        rpyDegrees,
        resource.metadata,
      ),
    );
  }, [boxDimensions, cylinderHeight, cylinderRadius, frameId, meshResourceUri, meshScale, objectId, onUpdate, position, resource.objectId, resource.metadata, rpyDegrees, shapeType, sphereRadius]);

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-start justify-between gap-2">
        <div>
          <div className="text-sm font-medium text-gray-100">{resource.objectId}</div>
          <div className="text-[11px] text-gray-500">{shapeSummary(resource.shape)}</div>
        </div>
        <div className="flex flex-col items-end gap-2">
          <button
            type="button"
            onClick={() => onRemove(resource.objectId)}
            disabled={disabled}
            className="rounded border border-red-700 px-2 py-1 text-xs text-red-200 hover:bg-red-900/40 disabled:cursor-not-allowed disabled:opacity-50"
          >
            Remove
          </button>
          <label className="flex items-center gap-2 text-[11px] text-gray-400">
            <input
              type="checkbox"
              checked={objectFrameVisible}
              onChange={(event) => onObjectFrameVisibilityChange(resource.objectId, event.target.checked)}
              disabled={!viewerAvailable}
              className="h-3 w-3"
            />
            <span>Gizmo frame</span>
          </label>
        </div>
      </div>

      <TextInput
        label="Object ID"
        value={objectId}
        onChange={setObjectId}
        disabled={disabled}
      />

      <label className="mt-2 block">
        <span className="text-[11px] text-gray-400">Shape</span>
        <select
          value={shapeType}
          onChange={(event) => setShapeType(event.target.value as SceneShapeType)}
          disabled={disabled}
          className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
        >
          <option value="box">box</option>
          <option value="sphere">sphere</option>
          <option value="cylinder">cylinder</option>
          <option value="mesh">mesh</option>
        </select>
      </label>

      {shapeType === 'box' ? (
        <div className="mt-2 grid grid-cols-3 gap-2">
          {boxDimensions.map((value, index) => (
            <NumericInput
              key={`box-${resource.objectId}-${index}`}
              label={`Dim ${['x', 'y', 'z'][index]}`}
              value={value}
              onChange={(next) => {
                const updated = [...boxDimensions];
                updated[index] = next;
                setBoxDimensions(updated);
              }}
              disabled={disabled}
            />
          ))}
        </div>
      ) : shapeType === 'sphere' ? (
        <div className="mt-2">
          <NumericInput
            label="Radius"
            value={sphereRadius}
            onChange={setSphereRadius}
            disabled={disabled}
          />
        </div>
      ) : shapeType === 'cylinder' ? (
        <div className="mt-2 grid grid-cols-2 gap-2">
          <NumericInput
            label="Radius"
            value={cylinderRadius}
            onChange={setCylinderRadius}
            disabled={disabled}
          />
          <NumericInput
            label="Height"
            value={cylinderHeight}
            onChange={setCylinderHeight}
            disabled={disabled}
          />
        </div>
      ) : (
        <div className="mt-2 space-y-2">
          <TextInput
            label="Mesh URI"
            value={meshResourceUri}
            onChange={setMeshResourceUri}
            disabled={disabled}
          />
          <div className="grid grid-cols-3 gap-2">
            {meshScale.map((value, index) => (
              <NumericInput
                key={`mesh-scale-${resource.objectId}-${index}`}
                label={`Scale ${['x', 'y', 'z'][index]}`}
                value={value}
                onChange={(next) => {
                  const updated = [...meshScale];
                  updated[index] = next;
                  setMeshScale(updated);
                }}
                disabled={disabled}
              />
            ))}
          </div>
        </div>
      )}

      <label className="mt-2 block">
        <span className="text-[11px] text-gray-400">Reference Frame</span>
        <select
          value={frameId}
          onChange={(event) => setFrameId(event.target.value)}
          disabled={disabled}
          className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
        >
          {frameOptions.map((frameName) => (
            <option key={frameName} value={frameName}>
              {frameName}
            </option>
          ))}
        </select>
      </label>

      <div className="mt-2 grid grid-cols-3 gap-2">
        {position.map((value, index) => (
          <NumericInput
            key={`pos-${resource.objectId}-${index}`}
            label={`P${['x', 'y', 'z'][index]}`}
            value={value}
            onChange={(next) => {
              const updated = [...position];
              updated[index] = next;
              setPosition(updated);
            }}
            disabled={disabled}
          />
        ))}
      </div>

      <div className="mt-2 grid grid-cols-3 gap-2">
        {rpyDegrees.map((value, index) => (
          <NumericInput
            key={`rpy-${resource.objectId}-${index}`}
            label={['Roll°', 'Pitch°', 'Yaw°'][index]}
            value={value}
            onChange={(next) => {
              const updated = [...rpyDegrees];
              updated[index] = next;
              setRpyDegrees(updated);
            }}
            step="1"
            disabled={disabled}
          />
        ))}
      </div>

      <button
        type="button"
        onClick={commit}
        disabled={disabled || !objectId.trim()}
        className="mt-2 w-full rounded border border-gray-600 px-3 py-2 text-xs text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
      >
        Update Program Scene Draft
      </button>
    </div>
  );
}

function SupportContactEditor({
  sceneResources,
  disabled,
  onChange,
  onError,
}: {
  sceneResources: ProgramSceneResource[];
  disabled: boolean;
  onChange: (resources: ProgramSceneResource[]) => void;
  onError: (message: string | null) => void;
}) {
  const objectIds = useMemo(
    () => sceneResources.map((resource) => resource.objectId).filter(Boolean),
    [sceneResources],
  );
  const rows = useMemo(
    () => supportContactRowsFromResources(sceneResources),
    [sceneResources],
  );
  const [newSupportedObjectId, setNewSupportedObjectId] = useState('');
  const [newSupportObjectId, setNewSupportObjectId] = useState('');

  useEffect(() => {
    if (objectIds.length < 2) {
      setNewSupportedObjectId('');
      setNewSupportObjectId('');
      return;
    }
    const supported = objectIds.includes(newSupportedObjectId)
      ? newSupportedObjectId
      : objectIds[0];
    const support = objectIds.includes(newSupportObjectId) && newSupportObjectId !== supported
      ? newSupportObjectId
      : objectIds.find((objectId) => objectId !== supported) ?? '';
    if (supported !== newSupportedObjectId) {
      setNewSupportedObjectId(supported);
    }
    if (support !== newSupportObjectId) {
      setNewSupportObjectId(support);
    }
  }, [newSupportObjectId, newSupportedObjectId, objectIds]);

  const commitRows = useCallback(
    (nextRows: SupportContactRow[]) => {
      const duplicate = duplicateSupportContact(nextRows);
      if (duplicate != null) {
        onError(`Duplicate support contact policy: ${duplicate}`);
        return;
      }
      onChange(applySupportContactRows(sceneResources, nextRows));
      onError(null);
    },
    [onChange, onError, sceneResources],
  );

  const updateRow = useCallback(
    (rowKey: string, patch: Partial<SupportContactRow>) => {
      const nextRows = rows.map((row) => {
        if (row.rowKey !== rowKey) {
          return row;
        }
        const nextSupported = patch.supportedObjectId ?? row.supportedObjectId;
        const nextSupport = patch.supportObjectId ?? row.supportObjectId;
        if (nextSupported === nextSupport) {
          onError('A support contact cannot reference the same object on both sides.');
          return row;
        }
        return {
          ...row,
          ...patch,
          contact: patch.contact ?? row.contact,
        };
      });
      commitRows(nextRows);
    },
    [commitRows, onError, rows],
  );

  const updateContact = useCallback(
    (rowKey: string, field: string, value: unknown) => {
      const row = rows.find((candidate) => candidate.rowKey === rowKey);
      if (row == null) {
        return;
      }
      updateRow(rowKey, {
        contact: replaceContactField(row.contact, field, value),
      });
    },
    [rows, updateRow],
  );

  const removeRow = useCallback(
    (rowKey: string) => {
      commitRows(rows.filter((row) => row.rowKey !== rowKey));
    },
    [commitRows, rows],
  );

  const addRow = useCallback(() => {
    if (!newSupportedObjectId || !newSupportObjectId) {
      onError('Choose an existing supported object and support object.');
      return;
    }
    if (newSupportedObjectId === newSupportObjectId) {
      onError('A support contact cannot reference the same object on both sides.');
      return;
    }
    commitRows([
      ...rows,
      {
        rowKey: '__new__',
        supportedObjectId: newSupportedObjectId,
        supportObjectId: newSupportObjectId,
        contact: defaultSupportContact(newSupportObjectId),
      },
    ]);
  }, [commitRows, newSupportObjectId, newSupportedObjectId, onError, rows]);

  if (objectIds.length < 2) {
    return (
      <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
        Add at least two authored scene objects to define support contacts.
      </div>
    );
  }

  return (
    <div className="space-y-2">
      {rows.length === 0 && (
        <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
          No support contact policies in the current program draft.
        </div>
      )}
      {rows.map((row) => {
        const enforceMax = contactBoolean(row.contact, 'enforce_max_start_penetration', true);
        const mode = contactMode(row.contact);
        const modality = supportContactModality(row.contact);
        const releaseEvidenceMode = contactReleaseEvidenceMode(row.contact);
        const isRelease = mode === 'contact_release';
        const isBoundedPenetration = mode === 'bounded_penetration';
        const usesSignedPenetration = isRelease && releaseEvidenceMode === 'signed_penetration';
        return (
          <div key={row.rowKey} className="rounded border border-gray-700 bg-gray-900/40 p-3">
            <div className="mb-2 flex items-start justify-between gap-2">
              <div>
                <div className="text-sm font-medium text-gray-100">
                  {supportContactSummary(row)}
                </div>
                <div className="text-[11px] text-gray-500">
                  Geometric support-contact policy for release or bounded contact.
                </div>
              </div>
              <button
                type="button"
                onClick={() => removeRow(row.rowKey)}
                disabled={disabled}
                className="rounded border border-red-700 px-2 py-1 text-xs text-red-200 hover:bg-red-900/40 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Remove
              </button>
            </div>

            <div className="grid grid-cols-2 gap-2">
              <label className="block">
                <span className="text-[11px] text-gray-400">Supported object</span>
                <select
                  value={row.supportedObjectId}
                  onChange={(event) => {
                    const supportedObjectId = event.target.value;
                    const supportObjectId = row.supportObjectId === supportedObjectId
                      ? objectIds.find((objectId) => objectId !== supportedObjectId) ?? ''
                      : row.supportObjectId;
                    updateRow(row.rowKey, { supportedObjectId, supportObjectId });
                  }}
                  disabled={disabled}
                  className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                >
                  {objectIds
                    .filter((objectId) => objectId !== row.supportObjectId)
                    .map((objectId) => (
                      <option key={objectId} value={objectId}>
                        {objectId}
                      </option>
                    ))}
                </select>
              </label>
              <label className="block">
                <span className="text-[11px] text-gray-400">Support object</span>
                <select
                  value={row.supportObjectId}
                  onChange={(event) => updateRow(row.rowKey, { supportObjectId: event.target.value })}
                  disabled={disabled}
                  className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                >
                  {objectIds
                    .filter((objectId) => objectId !== row.supportedObjectId)
                    .map((objectId) => (
                      <option key={objectId} value={objectId}>
                        {objectId}
                      </option>
                    ))}
                </select>
              </label>
            </div>

            <label className="mt-2 block">
              <span className="text-[11px] text-gray-400">Validation modality</span>
              <select
                value={modality}
                onChange={(event) => updateRow(row.rowKey, {
                  contact: contactForModality(
                    row.supportObjectId,
                    row.contact,
                    event.target.value as SupportContactModality,
                  ),
                })}
                disabled={disabled}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
              >
                <option value="signed_penetration">Precise - signed penetration</option>
                <option value="binary_contact">Lightweight - binary contact</option>
                <option value="bounded_penetration">
                  Contact-tolerant - bounded penetration
                </option>
                <option value="contact_process">Contact process - reserved</option>
              </select>
            </label>

            <div className="mt-2 text-[11px] text-gray-500">
              Collision pairs are enumerated first. Distance is evaluated only
              for configured pairs that are currently colliding; lightweight
              mode evaluates no distance.
            </div>

            {isRelease && (usesSignedPenetration ? (
              <>
                <label className="mt-3 flex items-start gap-2 text-xs text-gray-300">
                  <input
                    type="checkbox"
                    checked={enforceMax}
                    onChange={(event) => {
                      const nextContact = { ...row.contact };
                      nextContact.enforce_max_start_penetration = event.target.checked;
                      if (event.target.checked) {
                        nextContact.max_start_penetration_m = contactNumber(row.contact, 'max_start_penetration_m', 0.0);
                      } else {
                        delete nextContact.max_start_penetration_m;
                      }
                      updateRow(row.rowKey, { contact: nextContact });
                    }}
                    disabled={disabled}
                    className="mt-0.5 h-3 w-3"
                  />
                  <span>Limit start penetration</span>
                </label>

                {enforceMax && (
                  <div className="mt-2">
                    <NumericInput
                      label="Max start penetration (m)"
                      value={contactNumber(row.contact, 'max_start_penetration_m', 0.0)}
                      onChange={(next) => updateContact(row.rowKey, 'max_start_penetration_m', next)}
                      disabled={disabled}
                    />
                  </div>
                )}

                <div className="mt-2 grid grid-cols-2 gap-2">
                  <NumericInput
                    label="Release tolerance (m)"
                    value={contactNumber(row.contact, 'release_tolerance_m', 0.00001)}
                    onChange={(next) => updateContact(row.rowKey, 'release_tolerance_m', next)}
                    disabled={disabled}
                  />
                  <NumericInput
                    label="Min release progress (m)"
                    value={contactNumber(row.contact, 'min_release_progress_m', 0.0)}
                    onChange={(next) => updateContact(row.rowKey, 'min_release_progress_m', next)}
                    disabled={disabled}
                  />
                </div>
              </>
            ) : (
              <div className="mt-2 rounded border border-amber-700/60 bg-amber-950/30 p-2 text-[11px] text-amber-200">
                Lightweight mode uses contact-pair identity only. It enforces the
                configured no-new-pair, re-contact, and final-separation rules,
                but does not prove that penetration improves while a pair remains
                in contact.
              </div>
            ))}

            {isBoundedPenetration && (
              <>
                <div className="mt-3">
                  <NumericInput
                    label="Max contact penetration (m)"
                    value={contactNumber(row.contact, 'max_contact_penetration_m', 0.0)}
                    onChange={(next) => updateContact(
                      row.rowKey,
                      'max_contact_penetration_m',
                      next,
                    )}
                    disabled={disabled}
                  />
                </div>
                <div className="mt-2 rounded border border-blue-700/60 bg-blue-950/30 p-2 text-[11px] text-blue-200">
                  Contact-tolerant mode permits this configured pair to be clear
                  initially, first appear later, persist, disappear, re-contact,
                  or remain at the end while its measured penetration stays
                  within the bound. It is a geometric guard and does not prove
                  force or insertion-process safety.
                </div>
              </>
            )}

            {mode === 'contact_process' && (
              <div className="mt-2 rounded border border-red-700/60 bg-red-950/30 p-2 text-[11px] text-red-200">
                Contact process is reserved for future force/controller-backed
                policy and currently fails closed.
              </div>
            )}

            {isRelease && (
              <div className="mt-3 space-y-2">
              {[
                ['require_separation_by_end', 'Require separation by end', true],
                ...(usesSignedPenetration
                  ? [['require_strictly_decreasing_penetration', 'Require net penetration progress', true]]
                  : []),
                ['allow_recontact', 'Allow re-contact', false],
              ].map(([field, label, fallback]) => (
                <label key={field as string} className="flex items-start gap-2 text-xs text-gray-300">
                  <input
                    type="checkbox"
                    checked={contactBoolean(row.contact, field as string, fallback as boolean)}
                    onChange={(event) => updateContact(row.rowKey, field as string, event.target.checked)}
                    disabled={disabled}
                    className="mt-0.5 h-3 w-3"
                  />
                  <span>{label as string}</span>
                </label>
              ))}
              </div>
            )}
          </div>
        );
      })}

      <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
        <div className="mb-2 text-sm font-medium text-gray-100">Add Support Contact</div>
        <div className="grid grid-cols-2 gap-2">
          <label className="block">
            <span className="text-[11px] text-gray-400">Supported object</span>
            <select
              value={newSupportedObjectId}
              onChange={(event) => {
                const supportedObjectId = event.target.value;
                setNewSupportedObjectId(supportedObjectId);
                if (newSupportObjectId === supportedObjectId) {
                  setNewSupportObjectId(objectIds.find((objectId) => objectId !== supportedObjectId) ?? '');
                }
              }}
              disabled={disabled}
              className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
            >
              {objectIds
                .filter((objectId) => objectId !== newSupportObjectId)
                .map((objectId) => (
                  <option key={objectId} value={objectId}>
                    {objectId}
                  </option>
                ))}
            </select>
          </label>
          <label className="block">
            <span className="text-[11px] text-gray-400">Support object</span>
            <select
              value={newSupportObjectId}
              onChange={(event) => setNewSupportObjectId(event.target.value)}
              disabled={disabled}
              className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
            >
              {objectIds
                .filter((objectId) => objectId !== newSupportedObjectId)
                .map((objectId) => (
                  <option key={objectId} value={objectId}>
                    {objectId}
                  </option>
                ))}
            </select>
          </label>
        </div>
        <button
          type="button"
          onClick={addRow}
          disabled={disabled || !newSupportedObjectId || !newSupportObjectId}
          className="mt-2 w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Add Contact Policy
        </button>
      </div>
    </div>
  );
}

function LiveSceneObjectCard({
  objectState,
  viewerAvailable,
  onObjectFrameVisibilityChange,
}: {
  objectState: SceneObjectState;
  viewerAvailable: boolean;
  onObjectFrameVisibilityChange: (objectId: string, visible: boolean) => void;
}) {
  const pose = objectState.pose_in_universe as Record<string, unknown> | undefined;
  const position = readPoseVector(pose, 'position_m', 3) ?? [0, 0, 0];
  const rpyDegrees = quaternionToRpyDegrees(
    readPoseVector(pose, 'orientation_xyzw', 4) ?? [0, 0, 0, 1],
  );
  const badge = sourceBadge(objectState.source);
  const supportContacts = Array.isArray(objectState.support_contacts)
    ? objectState.support_contacts
    : [];

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-start justify-between gap-2">
        <div>
          <div className="text-sm font-medium text-gray-100">{objectState.object_id}</div>
          <div className="text-[11px] text-gray-500">{shapeSummary(objectState.shape as Record<string, unknown> | null | undefined)}</div>
        </div>
        <div className="flex flex-col items-end gap-2">
          <span className={`rounded border px-2 py-1 text-[10px] font-semibold tracking-wide ${badge.classes}`}>
            {badge.label}
          </span>
          <label className="flex items-center gap-2 text-[11px] text-gray-400">
            <input
              type="checkbox"
              checked={objectState.viewer_frame_visible}
              onChange={(event) => onObjectFrameVisibilityChange(objectState.object_id, event.target.checked)}
              disabled={!viewerAvailable}
              className="h-3 w-3"
            />
            <span>Gizmo frame</span>
          </label>
        </div>
      </div>
      <div className="space-y-1 text-[11px] text-gray-400">
        <div>Persistable: {objectState.persistable ? 'yes' : 'no'}</div>
        <div>
          Attachment:{' '}
          {objectState.attached && objectState.attached_to
            ? `attached to ${objectState.attached_to}`
            : 'detached'}
        </div>
        <div>
          Position: [{position.map((value) => value.toFixed(3)).join(', ')}]
        </div>
        <div>
          RPY°: [{rpyDegrees.map((value) => value.toFixed(1)).join(', ')}]
        </div>
        {supportContacts.length > 0 && (
          <div>
            Support contacts: {supportContacts.map(liveSupportContactSummary).join('; ')}
          </div>
        )}
      </div>
    </div>
  );
}

export function ScenePanel() {
  const sceneResources = useProgramStore((state) => state.sceneResources);
  const setSceneResources = useProgramStore((state) => state.setSceneResources);
  const programCompatibility = useProgramStore(
    (state) => state.programCompatibility,
  );
  const setProgramCompatibility = useProgramStore(
    (state) => state.setProgramCompatibility,
  );
  const viewerSettings = useProgramStore((state) => state.viewerSettings);
  const setViewerFrameVisibility = useProgramStore(
    (state) => state.setViewerFrameVisibility,
  );
  const setViewerFrameGizmoSize = useProgramStore(
    (state) => state.setViewerFrameGizmoSize,
  );
  const setViewerObjectFrameVisibility = useProgramStore(
    (state) => state.setViewerObjectFrameVisibility,
  );
  const editLocked = useEditLocked();
  const sceneState = useSceneStateStore((state) => state.scene);
  const frameState = useSceneStateStore((state) => state.frames);
  const refreshSceneProjection = useSceneStateStore((state) => state.refresh);
  const loading = useSceneStateStore((state) => state.refreshInFlight);
  const sceneProjectionError = useSceneStateStore((state) => state.error);
  const projectedFrames = frameState?.frames ?? EMPTY_SCENE_FRAMES;
  const frames = useMemo(
    () => projectedFrames.map((frame) => ({
      ...frame,
      viewerVisible:
        viewerSettings?.frameVisibility[frame.name] ?? frame.viewerVisible,
    })),
    [projectedFrames, viewerSettings?.frameVisibility],
  );
  const viewerAvailable = frameState?.viewerAvailable ?? false;
  const frameGizmoSizeM = viewerSettings?.frameGizmoSizeM
    ?? frameState?.frameGizmoSizeM
    ?? 0.1;
  const [error, setError] = useState<string | null>(null);
  const [newObjectId, setNewObjectId] = useState('');
  const [newShapeType, setNewShapeType] = useState<SceneShapeType>('box');
  const [newBoxDimensions, setNewBoxDimensions] = useState<number[]>([0.05, 0.05, 0.1]);
  const [newSphereRadius, setNewSphereRadius] = useState(0.05);
  const [newCylinderRadius, setNewCylinderRadius] = useState(0.05);
  const [newCylinderHeight, setNewCylinderHeight] = useState(0.1);
  const [newMeshResourceUri, setNewMeshResourceUri] = useState('');
  const [newMeshScale, setNewMeshScale] = useState<number[]>([1, 1, 1]);
  const [newFrameId, setNewFrameId] = useState('base_link');
  const [newPosition, setNewPosition] = useState<number[]>([0.4, 0.0, 0.1]);
  const [newRpyDegrees, setNewRpyDegrees] = useState<number[]>([0, 0, 0]);
  const [newObjectOpen, setNewObjectOpen] = useState(false);

  const updateCompatibility = useCallback(
    (patch: Partial<ProgramCompatibility>) => {
      const current = programCompatibility ?? {
        lanes: ['manyforge-only'],
        assistant: 'optional',
        requires: [],
        tags: [],
      };
      setProgramCompatibility({ ...current, ...patch });
    },
    [programCompatibility, setProgramCompatibility],
  );

  const refresh = useCallback(async () => {
    setError(null);
    await refreshSceneProjection();
  }, [refreshSceneProjection]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const frameOptions = useMemo(() => {
    const names = new Set<string>(['universe', 'base_link', 'tool0']);
    frames.forEach((frame) => names.add(frame.name));
    return Array.from(names).sort();
  }, [frames]);
  const liveObjects = useMemo(
    () => (sceneState?.objects ?? []).map((objectState) => ({
      ...objectState,
      viewer_frame_visible:
        viewerSettings?.objectFrameVisibility[objectState.object_id]
        ?? objectState.viewer_frame_visible,
    })),
    [sceneState?.objects, viewerSettings?.objectFrameVisibility],
  );
  const liveObjectsById = useMemo(
    () => new Map(liveObjects.map((objectState) => [objectState.object_id, objectState])),
    [liveObjects],
  );

  const replaceSceneResource = useCallback(
    (previousObjectId: string, updated: ProgramSceneResource) => {
      const updatedObjectId = updated.objectId.trim();
      const nextScene = sceneResources.filter(
        (resource) => resource.objectId !== previousObjectId && resource.objectId !== updatedObjectId,
      ).map(
        (resource) => renameSupportContactReference(resource, previousObjectId, updatedObjectId),
      );
      nextScene.push({ ...updated, objectId: updatedObjectId });
      setSceneResources(nextScene);
      setError(null);
    },
    [sceneResources, setSceneResources],
  );

  const removeSceneResource = useCallback(
    (objectId: string) => {
      const relatedContacts = supportContactRowsFromResources(sceneResources).filter(
        (row) => row.supportedObjectId === objectId || row.supportObjectId === objectId,
      );
      if (relatedContacts.length > 0) {
        const contactList = relatedContacts.map((row) => `- ${supportContactSummary(row)}`).join('\n');
        const confirmed = window.confirm(
          `Object "${objectId}" participates in support contact policies.\n\n`
          + `Removing it will also remove these policies:\n${contactList}\n\n`
          + 'Continue?',
        );
        if (!confirmed) {
          return;
        }
      }
      const prunedScene = removeSupportContactsInvolving(sceneResources, objectId)
        .filter((resource) => resource.objectId !== objectId);
      setSceneResources(prunedScene);
      setError(null);
    },
    [sceneResources, setSceneResources],
  );

  const addSceneResource = useCallback(() => {
    const shape =
      newShapeType === 'box'
        ? { type: 'box', box_dimensions_m: newBoxDimensions.map((value) => Number(value)) }
        : newShapeType === 'sphere'
          ? { type: 'sphere', sphere_radius_m: Number(newSphereRadius) }
          : newShapeType === 'cylinder'
            ? {
                type: 'cylinder',
                cylinder_radius_m: Number(newCylinderRadius),
                cylinder_height_m: Number(newCylinderHeight),
              }
          : {
              type: 'mesh',
              mesh_resource_uri: newMeshResourceUri.trim(),
              mesh_scale: newMeshScale.map((value) => Number(value)),
            };
    const nextResource = buildSceneResource(newObjectId.trim(), shape, newFrameId, newPosition, newRpyDegrees);
    const nextScene = sceneResources.filter((resource) => resource.objectId !== nextResource.objectId);
    nextScene.push(nextResource);
    setSceneResources(nextScene);
    setNewObjectId('');
    setNewObjectOpen(false);
    setError(null);
  }, [
    newBoxDimensions,
    newCylinderHeight,
    newCylinderRadius,
    newFrameId,
    newMeshResourceUri,
    newMeshScale,
    newObjectId,
    newPosition,
    newRpyDegrees,
    newShapeType,
    newSphereRadius,
    sceneResources,
    setSceneResources,
  ]);

  const toggleFrameVisibility = useCallback(
    (frameName: string, visible: boolean) => {
      setViewerFrameVisibility(frameName, visible);
      setError(null);
    },
    [setViewerFrameVisibility],
  );

  const updateFrameGizmoSize = useCallback(
    (axesLengthM: number) => {
      if (!Number.isFinite(axesLengthM) || axesLengthM <= 0) {
        return;
      }
      setViewerFrameGizmoSize(axesLengthM);
      setError(null);
    },
    [setViewerFrameGizmoSize],
  );

  const toggleObjectFrameVisibility = useCallback(
    (objectId: string, visible: boolean) => {
      setViewerObjectFrameVisibility(objectId, visible);
      setError(null);
    },
    [setViewerObjectFrameVisibility],
  );

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-4">
      <div className="rounded border border-gray-700 bg-gray-900/60 p-3 text-xs text-gray-300 space-y-1">
        <div className="font-medium text-gray-100">Program Scene</div>
        <div>
          Scene edits in this tab change the authored program scene draft. Authored changes are
          applied to the live scene when the program is loaded and on Reset/Run — they are not
          mirrored implicitly while idle or stopped.
        </div>
        <div>
          Global <span className="font-mono text-gray-100">Apply</span>,{' '}
          <span className="font-mono text-gray-100">Discard</span>,{' '}
          <span className="font-mono text-gray-100">Revert</span>, and{' '}
          <span className="font-mono text-gray-100">Save</span> include scene changes too.
        </div>
        <div>
          Manual scene editing is disabled while running or stopping. Runtime behavior edits still appear below as
          live/runtime state and are not persisted automatically.
        </div>
        <div>
          Authored scene objects:{' '}
          <span className="font-mono text-gray-100">{sceneResources.length}</span>
        </div>
        {editLocked && (
          <div className="rounded bg-yellow-800 px-2 py-1 font-mono text-yellow-100">
            PROGRAM SCENE EDITING DISABLED WHILE EXECUTION IS ACTIVE
          </div>
        )}
        {(error ?? sceneProjectionError) && (
          <div className="rounded bg-red-900/60 px-2 py-1 text-red-200">
            {error ?? sceneProjectionError}
          </div>
        )}
      </div>

      <div className="flex gap-2">
        <button
          type="button"
          onClick={() => void refresh()}
          disabled={loading}
          className="flex-1 rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Refresh Live Scene
        </button>
      </div>

      <Section title="Program Compatibility">
        <label className="flex items-center gap-2 text-xs text-gray-300">
          <input
            type="checkbox"
            checked={programCompatibility !== null}
            onChange={(event) => {
              setProgramCompatibility(
                event.target.checked
                  ? {
                      lanes: ['manyforge-only'],
                      assistant: 'optional',
                      requires: [],
                      tags: [],
                    }
                  : null,
              );
            }}
            disabled={editLocked}
          />
          Enabled
        </label>
        {programCompatibility && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 space-y-2">
            <div className="grid grid-cols-2 gap-2">
              {(['manyforge-only', 'isaac-sim', 'real-robot'] as const).map((lane) => (
                <label key={lane} className="flex items-center gap-2 text-xs text-gray-300">
                  <input
                    type="checkbox"
                    checked={programCompatibility.lanes.includes(lane)}
                    onChange={(event) => {
                      const lanes = event.target.checked
                        ? Array.from(new Set([...programCompatibility.lanes, lane]))
                        : programCompatibility.lanes.filter((value) => value !== lane);
                      if (lanes.length > 0) updateCompatibility({ lanes });
                    }}
                    disabled={editLocked}
                  />
                  {lane}
                </label>
              ))}
            </div>
            <label className="block">
              <span className="text-[11px] text-gray-400">Assistant</span>
              <select
                value={programCompatibility.assistant}
                onChange={(event) => updateCompatibility({
                  assistant: event.target.value as ProgramCompatibility['assistant'],
                })}
                disabled={editLocked}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50"
              >
                <option value="required">required</option>
                <option value="optional">optional</option>
                <option value="forbidden">forbidden</option>
              </select>
            </label>
            <TextInput
              label="Required deployment tokens"
              value={programCompatibility.requires.join(', ')}
              onChange={(value) => updateCompatibility({
                requires: Array.from(new Set(
                  value.split(',').map((item) => item.trim()).filter(Boolean),
                )),
              })}
              disabled={editLocked}
            />
            <TextInput
              label="Tags"
              value={programCompatibility.tags.join(', ')}
              onChange={(value) => updateCompatibility({
                tags: Array.from(new Set(
                  value.split(',').map((item) => item.trim()).filter(Boolean),
                )),
              })}
              disabled={editLocked}
            />
          </div>
        )}
      </Section>

      <Section title="Viewer Frames">
        {!viewerAvailable && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            Viewer frame toggles are unavailable until frame state loads.
          </div>
        )}
        <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
          <NumericInput
            label="Frame gizmo size (m)"
            value={frameGizmoSizeM}
            onChange={updateFrameGizmoSize}
            step="0.01"
            disabled={!viewerAvailable}
          />
        </div>
        <div className="space-y-2">
          {frames.length === 0 && (
            <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
              No frames discovered yet.
            </div>
          )}
          {frames.map((frame) => (
            <label
              key={frame.name}
              className="flex items-start gap-3 rounded border border-gray-700 bg-gray-900/40 px-3 py-2"
            >
              <input
                type="checkbox"
                checked={frame.viewerVisible}
                onChange={(event) => toggleFrameVisibility(frame.name, event.target.checked)}
                disabled={!viewerAvailable}
                className="mt-0.5"
              />
              <div className="min-w-0 flex-1">
                <div className="text-sm text-gray-100">{frame.name}</div>
                <div className="text-[11px] text-gray-500">
                  {frame.roles.length > 0 ? frame.roles.join(', ') : 'model frame'}
                  {frame.poseAvailable ? ' • pose available' : ' • pose unavailable'}
                </div>
              </div>
            </label>
          ))}
        </div>
      </Section>

      <Section title="Add Program Scene Primitive">
        {!newObjectOpen ? (
          <button
            type="button"
            onClick={() => setNewObjectOpen(true)}
            disabled={editLocked}
            className="w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
          >
            New Object
          </button>
        ) : (
          <div className="space-y-2 rounded border border-gray-700 bg-gray-900/40 p-3">
            <TextInput
              label="Object ID"
              value={newObjectId}
              onChange={setNewObjectId}
              disabled={editLocked}
              placeholder="new_box"
            />
            <label className="block">
              <span className="text-[11px] text-gray-400">Shape</span>
              <select
                value={newShapeType}
                onChange={(event) => setNewShapeType(event.target.value as SceneShapeType)}
                disabled={editLocked}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
              >
                <option value="box">box</option>
                <option value="sphere">sphere</option>
                <option value="cylinder">cylinder</option>
                <option value="mesh">mesh</option>
              </select>
            </label>
            {newShapeType === 'box' ? (
              <div className="grid grid-cols-3 gap-2">
                {newBoxDimensions.map((value, index) => (
                  <NumericInput
                    key={`new-box-${index}`}
                    label={`Dim ${['x', 'y', 'z'][index]}`}
                    value={value}
                    onChange={(next) => {
                      const updated = [...newBoxDimensions];
                      updated[index] = next;
                      setNewBoxDimensions(updated);
                    }}
                    disabled={editLocked}
                  />
                ))}
              </div>
            ) : newShapeType === 'sphere' ? (
              <NumericInput
                label="Radius"
                value={newSphereRadius}
                onChange={setNewSphereRadius}
                disabled={editLocked}
              />
            ) : newShapeType === 'cylinder' ? (
              <div className="grid grid-cols-2 gap-2">
                <NumericInput
                  label="Radius"
                  value={newCylinderRadius}
                  onChange={setNewCylinderRadius}
                  disabled={editLocked}
                />
                <NumericInput
                  label="Height"
                  value={newCylinderHeight}
                  onChange={setNewCylinderHeight}
                  disabled={editLocked}
                />
              </div>
            ) : (
              <div className="space-y-2">
                <TextInput
                  label="Mesh URI"
                  value={newMeshResourceUri}
                  onChange={setNewMeshResourceUri}
                  disabled={editLocked}
                  placeholder="file:///workspace/scene/part.obj"
                />
                <div className="grid grid-cols-3 gap-2">
                  {newMeshScale.map((value, index) => (
                    <NumericInput
                      key={`new-mesh-scale-${index}`}
                      label={`Scale ${['x', 'y', 'z'][index]}`}
                      value={value}
                      onChange={(next) => {
                        const updated = [...newMeshScale];
                        updated[index] = next;
                        setNewMeshScale(updated);
                      }}
                      disabled={editLocked}
                    />
                  ))}
                </div>
              </div>
            )}
            <label className="block">
              <span className="text-[11px] text-gray-400">Reference Frame</span>
              <select
                value={newFrameId}
                onChange={(event) => setNewFrameId(event.target.value)}
                disabled={editLocked}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
              >
                {frameOptions.map((frameName) => (
                  <option key={frameName} value={frameName}>
                    {frameName}
                  </option>
                ))}
              </select>
            </label>
            <div className="grid grid-cols-3 gap-2">
              {newPosition.map((value, index) => (
                <NumericInput
                  key={`new-pos-${index}`}
                  label={`P${['x', 'y', 'z'][index]}`}
                  value={value}
                  onChange={(next) => {
                    const updated = [...newPosition];
                    updated[index] = next;
                    setNewPosition(updated);
                  }}
                  disabled={editLocked}
                />
              ))}
            </div>
            <div className="grid grid-cols-3 gap-2">
              {newRpyDegrees.map((value, index) => (
                <NumericInput
                  key={`new-rpy-${index}`}
                  label={['Roll°', 'Pitch°', 'Yaw°'][index]}
                  value={value}
                  onChange={(next) => {
                    const updated = [...newRpyDegrees];
                    updated[index] = next;
                    setNewRpyDegrees(updated);
                  }}
                  step="1"
                  disabled={editLocked}
                />
              ))}
            </div>
            <div className="flex gap-2">
              <button
                type="button"
                onClick={addSceneResource}
                disabled={editLocked || !newObjectId.trim()}
                className="flex-1 rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Add To Program Scene Draft
              </button>
              <button
                type="button"
                onClick={() => setNewObjectOpen(false)}
                disabled={editLocked}
                className="rounded border border-gray-600 px-3 py-2 text-sm text-gray-300 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Cancel
              </button>
            </div>
          </div>
        )}
      </Section>

      <Section title="Support Contacts">
        <SupportContactEditor
          sceneResources={sceneResources}
          disabled={editLocked}
          onChange={setSceneResources}
          onError={setError}
        />
      </Section>

      <Section title="Program Scene Objects">
        {sceneResources.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No authored scene objects in the current program draft.
          </div>
        )}
        {sceneResources.map((resource) => (
          <AuthoredSceneCard
            key={`${resource.objectId}:${JSON.stringify(resource.shape)}:${JSON.stringify(resource.pose)}`}
            resource={resource}
            frameOptions={frameOptions}
            disabled={editLocked}
            viewerAvailable={viewerAvailable}
            objectFrameVisible={liveObjectsById.get(resource.objectId)?.viewer_frame_visible ?? false}
            onUpdate={replaceSceneResource}
            onRemove={removeSceneResource}
            onObjectFrameVisibilityChange={toggleObjectFrameVisibility}
          />
        ))}
      </Section>

      <Section title="Live Runtime Scene">
        {liveObjects.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No live scene objects available.
          </div>
        )}
        {liveObjects.map((objectState) => (
          <LiveSceneObjectCard
            key={objectState.object_id}
            objectState={objectState}
            viewerAvailable={viewerAvailable}
            onObjectFrameVisibilityChange={toggleObjectFrameVisibility}
          />
        ))}
      </Section>
    </div>
  );
}
