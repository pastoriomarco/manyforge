import type { JsonValue, QuaternionXyzwTuple, Vector3Tuple } from './api';

/** A mesh part's GLB (scale applied), or why it cannot be drawn. */
export type ToolPreviewMeshAsset =
  | { url: string; error: null }
  | { url: null; error: { code: string; message: string } };

/** One drawn part of a Tool, placed in its mount frame (jaws open). */
export interface ToolPreviewShape {
  link: string;
  part: string;
  /** One of `box`, `cylinder`, `sphere`, `mesh`, with its fields. */
  geometry: Record<string, Record<string, JsonValue>>;
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
  /** Set for a mesh part only. */
  mesh_asset: ToolPreviewMeshAsset | null;
}

export interface ToolPreviewTcp {
  id: string;
  tool_end: string | null;
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

/** A tool end the parameters declare: a jaw pair (actuated), spring slider or tip. */
export interface ToolEnd {
  id: string;
  kind: 'jaw_pair' | 'spring_slider' | 'tip';
  actuated: boolean;
  collision_body_role_ids: string[];
  tcp_ids: string[];
}

export interface ToolDeclaredTcp {
  id: string;
  frame: string;
  tool_end: string | null;
  contact_body_role_ids: string[];
}

/** A refusal of the parameters; `field` is the refused field's path when known. */
export interface ToolParametersRefusal {
  code: string;
  message: string;
  field: string | null;
}

/**
 * `meshFolder`: the folder the parameters' mesh paths are relative to (the
 * Scenario folder for inline parameters, else the parameter file's folder);
 * null when not known. The mesh picker writes paths relative to it.
 */
export type ToolPreviewResponse =
  | { ok: true; shapes: ToolPreviewShape[]; tcpFrames: ToolPreviewTcp[]; toolEnds: ToolEnd[];
      collisionBodies: string[]; tcps: ToolDeclaredTcp[]; meshFolder: string | null }
  | { ok: false; error: ToolParametersRefusal; meshFolder: string | null };

export interface ToolExportResponse {
  written: string[];
}
