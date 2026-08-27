import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import * as THREE from 'three';
import { describe, expect, it } from 'vitest';

import {
  DEFAULT_WIREFRAME_VISIBLE,
  VIEWER_CLICK_MAX_DURATION_MS,
  frameableContentBounds,
  framedCameraPresentation,
  supportsIndependentSceneScale,
  viewerClickWithinDuration,
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
    expect(source).toContain('manifest.data.links.length} links · {manifest.data.joints.length} joints');
    expect(source).not.toContain('className="viewer-legend"');
  });

  it('keeps camera and transform commands in the compact viewer header', () => {
    const css = readFileSync(new URL('../index.css', import.meta.url), 'utf8');
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const header = source.indexOf('className="panel-header viewer-header"');
    const stage = source.indexOf('className="viewer-stage"', header);
    const controls = source.indexOf('aria-label="Scene viewport controls"', header);
    const options = source.indexOf('className="viewer-options"', header);

    expect(header).toBeGreaterThan(0);
    expect(controls).toBeLessThan(options);
    expect(options).toBeLessThan(stage);
    expect(source.indexOf('aria-label="Frame all visible content"', header)).toBeLessThan(stage);
    expect(source.indexOf('aria-label="Frame selected object"', header)).toBeLessThan(stage);
    expect(source.indexOf('aria-label="Reset camera"', header)).toBeLessThan(stage);
    expect(css).toMatch(/\.viewer-header-controls\s*\{[^}]*flex:\s*1 1 auto/s);
    expect(css).toMatch(/\.viewer-options\s*\{[^}]*margin-left:\s*auto/s);
    expect(source).toContain('className="viewer-camera-icon"');
  });

  it('frames content without changing the current viewing direction', () => {
    const box = new THREE.Box3(
      new THREE.Vector3(9, -2, 1),
      new THREE.Vector3(11, 4, 5),
    );
    const current = {
      position: [5, -7, 8] as [number, number, number],
      target: [1, 2, 3] as [number, number, number],
    };
    const framed = framedCameraPresentation(box, current, 48, 16 / 9);
    const oldDirection = new THREE.Vector3(...current.position)
      .sub(new THREE.Vector3(...current.target))
      .normalize();
    const newDirection = new THREE.Vector3(...framed.camera.position)
      .sub(new THREE.Vector3(...framed.camera.target))
      .normalize();

    expect(framed.camera.target).toEqual([10, 1, 3]);
    expect(newDirection.distanceTo(oldDirection)).toBeLessThan(1e-12);
  });

  it('excludes transform interaction helpers from frame-all bounds', () => {
    const content = new THREE.Group();
    content.add(new THREE.Mesh(new THREE.BoxGeometry(2, 2, 2)));
    const transformHelper = new THREE.Group() as THREE.Group & {
      isTransformControls: boolean;
    };
    transformHelper.isTransformControls = true;
    transformHelper.add(new THREE.Mesh(new THREE.BoxGeometry(1_000, 1_000, 1_000)));
    content.add(transformHelper);

    const size = frameableContentBounds(content).getSize(new THREE.Vector3());

    expect(size.toArray()).toEqual([2, 2, 2]);
  });

  it('uses direct R3F semantic clicks instead of a second selection engine', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).not.toContain('function ViewerSelectionSurface');
    expect(source).not.toContain('event.intersections');
    expect(source).toContain('onClick={(event) => onSelect(event, link.name)}');
    expect(source).toContain(
      'onClick={(event) => onSelect(event, object.scene_resource_id)}',
    );
    expect(source).toContain('onClickCapture={clearBeforeSemanticClick}');
    expect(source).toContain('className="viewer-canvas-input-surface"');
    expect(source).not.toContain('onPointerMissed=');
    expect(source).not.toContain('new THREE.Raycaster()');
    expect(source).toContain("const ignoreViewerRaycast: THREE.Object3D['raycast']");
    expect(source.match(/raycast=\{ignoreViewerRaycast\}/g)).toHaveLength(5);
    expect(source).not.toContain('transformRevision');
    expect(source).not.toContain('markTransformChange');
    expect(source).toContain('transformEnabled={transformEnabled && objectEditorActive}');
    expect(source).toContain('editable={candidateTcpEditable && robotEditorActive');
  });

  it('accepts a release through 250 ms and rejects longer holds', () => {
    expect(VIEWER_CLICK_MAX_DURATION_MS).toBe(250);
    expect(viewerClickWithinDuration({
      startedAtMs: 100,
      releasedAtMs: 350,
    })).toBe(true);
    expect(viewerClickWithinDuration({
      startedAtMs: 100,
      releasedAtMs: 351,
    })).toBe(false);
    expect(viewerClickWithinDuration({
      startedAtMs: 101,
      releasedAtMs: 100,
    })).toBe(false);
  });

  it('routes authored and measured render layers through the same selection callbacks', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source.match(/onSelect={selectStructure}/g)).toHaveLength(2);
    expect(source.match(/onSelect={selectSceneResource}/g)).toHaveLength(2);
  });

  it('retains wheel camera presentation without React state commits', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('createRetainedCameraPresentationStore');
    expect(source).toContain('cameraPresentation.write(presentation)');
    expect(source).toContain('onPresentation(presentation)');
    expect(source).toContain("addEventListener?.('change', capture)");
    const captureStart = source.indexOf('function CaptureCameraPresentation');
    const captureEnd = source.indexOf('function ViewerCanvas', captureStart);
    expect(source.slice(captureStart, captureEnd)).not.toContain('useFrame');
    expect(source.slice(captureStart, captureEnd)).not.toContain('latestPresentationRef');
    expect(source).not.toContain('setCameraPresentation(presentation)');
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
      'fit={camera === null && !retainedCameraPresentation.framed}',
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
