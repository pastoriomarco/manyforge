import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import {
  DEFAULT_WIREFRAME_VISIBLE,
  supportsIndependentSceneScale,
  ViewerFailureBoundary,
} from './StructuralViewer';
import type { LogicalSceneObject } from '../types/api';

function sceneObject(
  shape: LogicalSceneObject['collision_shapes'][number],
): LogicalSceneObject {
  return {
    scene_resource_id: 'part',
    collision_shapes: [shape],
    placement: {
      kind: 'cell_fixed',
      cell_id: 'cell',
      pose_in_cell: {
        position_m: { x: 0, y: 0, z: 0 },
        orientation_xyzw: [0, 0, 0, 1],
      },
    },
    provider_evidence: null,
    support_contacts: [],
  };
}

describe('viewer failure isolation', () => {
  it('offers unconstrained scale only for shapes the gizmo can represent safely', () => {
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'box', dimensions_m: { x: 1, y: 1, z: 1 },
    }))).toBe(true);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'mesh', resource_uri: 'package://part', scale: { x: 1, y: 1, z: 1 },
    }))).toBe(true);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'sphere', radius_m: 1,
    }))).toBe(false);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'cylinder', radius_m: 1, height_m: 1,
    }))).toBe(false);
  });

  it('replaces only the failed viewer subtree with its fallback', () => {
    const boundary = new ViewerFailureBoundary({
      children: <div>viewport</div>,
      fallback: <div>isolated; authoring remains usable</div>,
    });
    boundary.state = ViewerFailureBoundary.getDerivedStateFromError();

    expect(renderToStaticMarkup(boundary.render())).toContain(
      'isolated; authoring remains usable',
    );
  });

  it('defaults scene presentation to solid geometry and keeps wireframe optional', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(DEFAULT_WIREFRAME_VISIBLE).toBe(false);
    expect(source).toContain('<span>Wireframe overlays</span>');
    expect(source).toContain('wireframe={wireframe}');
    expect(source).toContain('depthWrite={!wireframe}');
    expect(source).toContain(
      'setViewerQualificationWireframeDefault(DEFAULT_WIREFRAME_VISIBLE)',
    );
  });

  it('keeps viewer layers in one compact tools menu at narrow widths', () => {
    const css = readFileSync(new URL('../index.css', import.meta.url), 'utf8');
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(css).toContain('.viewer-options-popover');
    expect(source.match(/type="checkbox"/g)).toHaveLength(4);
    expect(source).toContain('<summary>Tools</summary>');
    expect(source).toContain('Measured-state overlay');
    expect(source).toContain('Model and source facts');
  });

  it('frames robot, scene, measured overlays, and point clouds together', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const bounds = source.indexOf('<Bounds');
    const boundsEnd = source.indexOf('</Bounds>', bounds);

    expect(bounds).toBeGreaterThan(0);
    expect(source.indexOf('<RetainedNeutralModel', bounds)).toBeLessThan(boundsEnd);
    expect(source.indexOf('<LogicalSceneObjects', bounds)).toBeLessThan(boundsEnd);
    expect(source.indexOf('<MeasuredLogicalSceneObjects', bounds)).toBeLessThan(boundsEnd);
    expect(source.indexOf('<PointCloudPoints', bounds)).toBeLessThan(boundsEnd);
    expect(source).toContain('viewer-stage viewer-fallback-stage');
    expect(source).toContain(
      'fit={camera === null && !initialCameraPresentation.framed}',
    );
  });

  it('isolates individual retained and logical-scene mesh failures', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('<AssetErrorBoundary resetKey={visual.mesh_url}');
    expect(source).toContain('<AssetErrorBoundary resetKey={collision.mesh_url}');
    expect(source).toContain('<AssetErrorBoundary resetKey={url} onError={isolateError}>');
    expect(source).toContain('One mesh is unavailable; the remaining scene is still usable.');
  });

  it('shares disposable object, link, and frame selection with camera tools', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(viewer).toContain('selectedStructureName');
    expect(viewer).toContain('scene.getObjectByName(`measured:${request.target.id}`)');
    expect(viewer).toContain('scene.getObjectByName(`link:${request.target.id}`)');
    expect(workspace).toContain("kind: 'owner_object'");
    expect(workspace).toContain("kind: 'program_resource'");
    expect(workspace).toContain("kind: 'structure'");
  });

  it('routes pose and Cartesian TCP gizmo edits through the existing candidate controller', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(viewer).toContain('candidateTcpEditable');
    expect(viewer).toContain('onEditCandidateTcpPose');
    expect(viewer).toContain("transformMode === 'rotate' ? 'rotate' : 'translate'");
    expect(workspace).toContain("robotControl.state.mode === 'pose_goal'");
    expect(workspace).toContain("robotControl.state.mode === 'cartesian_path'");
    expect(workspace).toContain('onEditCandidatePose={robotControl.editPose}');
  });

  it('suspends only viewer observers while retaining outer presentation state', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('const pointCloudEnabled = viewerActive');
    expect(source).toContain('{viewerActive && (');
    expect(source).toContain('<BoundSnapshotResponder');
    expect(source).not.toContain("sceneOwnerIdentity?.generation ?? -1");
    expect(source.indexOf('const [pointCloudVisible')).toBeLessThan(
      source.indexOf('{viewerActive && ('),
    );
  });

  it('keeps qualification observation opt-in and demand-framed', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('Profiler id="manyforge-structural-viewer"');
    expect(source).toContain('<ViewerPerformanceProbe />');
    expect(source).toContain('recordViewerRenderFrame(nowMs)');
    expect(source).toContain('recordViewerRenderWork(');
    expect(source).toContain('startViewerPerformanceMonitoring()');
    expect(source).toContain('stopViewerPerformanceMonitoring()');
    expect(source).toContain('connectViewerQualification()');
    expect(source).toContain('frameloop="demand"');
    expect(source).not.toContain('frameloop="always"');
  });
});
