import type {
  ToolDeclaredTcp,
  ToolEnd,
  ToolExportResponse,
  ToolPreviewMeshAsset,
  ToolPreviewResponse,
  ToolPreviewShape,
  ToolPreviewTcp,
} from '../types/toolParameters';
import {
  arrayAt,
  booleanAt,
  fail,
  jsonRecordAt,
  nullableStringAt,
  objectAt,
  quaternionAt,
  stringArrayAt,
  stringAt,
  vector3At,
} from './parsers/shared';

const END_KINDS = ['jaw_pair', 'spring_slider', 'tip'] as const;

function meshAsset(value: unknown, path: string): ToolPreviewMeshAsset {
  const row = objectAt(value, path);
  const url = nullableStringAt(row.url, `${path}.url`);
  if (url !== null) {
    if (row.error !== null) return fail(`${path}.error`, 'null with a url');
    return { url, error: null };
  }
  const error = objectAt(row.error, `${path}.error`);
  return { url: null, error: {
    code: stringAt(error.code, `${path}.error.code`),
    message: stringAt(error.message, `${path}.error.message`),
  } };
}

function shape(value: unknown, path: string): ToolPreviewShape {
  const row = objectAt(value, path);
  const geometry = jsonRecordAt(row.geometry, `${path}.geometry`);
  const kinds = Object.keys(geometry);
  if (kinds.length !== 1 || !['box', 'cylinder', 'sphere', 'mesh'].includes(kinds[0])) {
    return fail(`${path}.geometry`, 'one of box, cylinder, sphere, mesh');
  }
  return {
    link: stringAt(row.link, `${path}.link`),
    part: stringAt(row.part, `${path}.part`),
    geometry: { [kinds[0]]: jsonRecordAt(geometry[kinds[0]], `${path}.geometry.${kinds[0]}`) },
    position_m: vector3At(row.position_m, `${path}.position_m`),
    orientation_xyzw: quaternionAt(row.orientation_xyzw, `${path}.orientation_xyzw`),
    mesh_asset: kinds[0] === 'mesh' ? meshAsset(row.mesh_asset, `${path}.mesh_asset`) : null,
  };
}

function frame(value: unknown, path: string): ToolPreviewTcp {
  const row = objectAt(value, path);
  return {
    id: stringAt(row.id, `${path}.id`),
    tool_end: nullableStringAt(row.tool_end, `${path}.tool_end`),
    position_m: vector3At(row.position_m, `${path}.position_m`),
    orientation_xyzw: quaternionAt(row.orientation_xyzw, `${path}.orientation_xyzw`),
  };
}

function end(value: unknown, path: string): ToolEnd {
  const row = objectAt(value, path);
  const kind = stringAt(row.kind, `${path}.kind`);
  if (!(END_KINDS as readonly string[]).includes(kind)) return fail(`${path}.kind`, END_KINDS.join(', '));
  return {
    id: stringAt(row.id, `${path}.id`),
    kind: kind as ToolEnd['kind'],
    actuated: booleanAt(row.actuated, `${path}.actuated`),
    collision_body_role_ids: stringArrayAt(row.collision_body_role_ids, `${path}.collision_body_role_ids`),
    tcp_ids: stringArrayAt(row.tcp_ids, `${path}.tcp_ids`),
  };
}

function tcp(value: unknown, path: string): ToolDeclaredTcp {
  const row = objectAt(value, path);
  return {
    id: stringAt(row.id, `${path}.id`),
    frame: stringAt(row.frame, `${path}.frame`),
    tool_end: nullableStringAt(row.tool_end, `${path}.tool_end`),
    contact_body_role_ids: stringArrayAt(row.contact_body_role_ids, `${path}.contact_body_role_ids`),
  };
}

export function parseToolPreviewResponse(value: unknown): ToolPreviewResponse {
  const record = objectAt(value, 'toolPreview');
  if (record.schemaVersion !== 'manyforge.composer_tool_preview.v1') {
    return fail('toolPreview.schemaVersion', 'manyforge.composer_tool_preview.v1');
  }
  if (!booleanAt(record.ok, 'toolPreview.ok')) {
    const error = objectAt(record.error, 'toolPreview.error');
    return {
      ok: false,
      error: {
        code: stringAt(error.code, 'toolPreview.error.code'),
        message: stringAt(error.message, 'toolPreview.error.message'),
        field: nullableStringAt(error.field, 'toolPreview.error.field'),
      },
      meshFolder: nullableStringAt(record.meshFolder, 'toolPreview.meshFolder'),
    };
  }
  const preview = objectAt(record.preview, 'toolPreview.preview');
  return {
    ok: true,
    shapes: arrayAt(preview.shapes, 'toolPreview.preview.shapes')
      .map((item, index) => shape(item, `toolPreview.preview.shapes[${index}]`)),
    tcpFrames: arrayAt(preview.tcps, 'toolPreview.preview.tcps')
      .map((item, index) => frame(item, `toolPreview.preview.tcps[${index}]`)),
    toolEnds: arrayAt(record.toolEnds, 'toolPreview.toolEnds')
      .map((item, index) => end(item, `toolPreview.toolEnds[${index}]`)),
    collisionBodies: stringArrayAt(record.collisionBodies, 'toolPreview.collisionBodies'),
    tcps: arrayAt(record.tcps, 'toolPreview.tcps')
      .map((item, index) => tcp(item, `toolPreview.tcps[${index}]`)),
    meshFolder: nullableStringAt(record.meshFolder, 'toolPreview.meshFolder'),
  };
}

export function parseToolExportResponse(value: unknown): ToolExportResponse {
  const record = objectAt(value, 'toolExport');
  if (record.schemaVersion !== 'manyforge.composer_tool_export.v1') {
    return fail('toolExport.schemaVersion', 'manyforge.composer_tool_export.v1');
  }
  return { written: stringArrayAt(record.written, 'toolExport.written') };
}
