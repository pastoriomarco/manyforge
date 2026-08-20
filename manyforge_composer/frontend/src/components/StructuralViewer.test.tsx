import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import {
  DEFAULT_WIREFRAME_VISIBLE,
  ViewerFailureBoundary,
} from './StructuralViewer';

describe('viewer failure isolation', () => {
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

  it('wraps native viewer controls instead of clipping them at narrow widths', () => {
    const css = readFileSync(new URL('../index.css', import.meta.url), 'utf8');
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(css).toMatch(/\.viewer-controls\s*\{\s*flex-wrap: wrap;/);
    expect(source.match(/type="checkbox"/g)).toHaveLength(3);
    expect(source).toContain('<button type="button" className="quiet-button"');
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
