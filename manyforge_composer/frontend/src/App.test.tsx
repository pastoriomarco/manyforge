import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import App from './App';

describe('Composer shell continuity', () => {
  it('renders one integrated workbench spine within the initial budget', () => {
    const started = performance.now();
    const markup = renderToStaticMarkup(<App />);
    const elapsedMs = performance.now() - started;

    const navigation = markup.indexOf('aria-label="Catalog and artifact library"');
    const workspace = markup.indexOf('aria-label="Program authoring workspace"');
    const viewer = markup.indexOf('id="viewer-title"');
    const sceneAuthoring = markup.indexOf('id="scene-authoring-title"');
    const commands = markup.indexOf('aria-label="Viewer and operations rail"');
    const runtime = markup.indexOf('id="program-runtime-title"');
    const footer = markup.indexOf('class="app-footer"');
    expect(markup).toContain('ManyForge Composer');
    expect(markup).toContain('Structural mode');
    expect(navigation).toBeGreaterThan(0);
    expect(workspace).toBeGreaterThan(0);
    expect(commands).toBeGreaterThan(workspace);
    expect(viewer).toBeGreaterThan(commands);
    expect(sceneAuthoring).toBeGreaterThan(viewer);
    expect(runtime).toBeGreaterThan(sceneAuthoring);
    expect(footer).toBeGreaterThan(runtime);
    expect(markup).toContain('Node catalog');
    expect(markup).toContain('Artifacts · Program / Deployment');
    expect(markup).toContain('Runtime, alarms, and history');
    expect(markup).not.toContain('role="tablist"');
    expect(markup).not.toContain('workspace-tab-');
    expect(markup.match(/id="viewer-title"/g)).toHaveLength(1);
    expect(markup.match(/aria-label="Library width"/g)).toHaveLength(1);
    expect(markup.match(/aria-label="Operations rail width"/g)).toHaveLength(1);
    expect(markup).toContain('Composer owns local drafts and presentation only.');
    expect(elapsedMs).toBeLessThan(2000);
  });

  it('exposes bounded resize, collapse, and responsive presentation semantics', () => {
    const markup = renderToStaticMarkup(<App />);
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const source = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const sceneSource = readFileSync(
      new URL('./components/SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(markup).toContain('min="220" max="420"');
    expect(markup).toContain('min="480" max="720"');
    expect(markup).toContain('aria-label="Collapse library pane"');
    expect(markup).toContain('aria-label="Collapse viewer and operations pane"');
    expect(css).toContain('@media (max-width: 76rem)');
    expect(css).toContain('.workbench-workspace');
    expect(css).toContain('.visual-operations-stack > .viewer-panel');
    expect(css).toContain('.program-authoring-empty');
    expect(source).toContain('const viewerActive = !commandsCollapsed');
    expect(source).toContain('target.current?.focus()');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(sceneSource.match(/<StructuralViewer/g)).toHaveLength(1);
    expect(source).not.toContain('workspaceTab');
    expect(source).not.toContain('mobilePane');
    expect(source).not.toContain('setWorkspaceTab');
    expect(source).not.toContain('useEffect');
    expect(source).not.toContain('onMount');
    expect(css).toContain('.pane-resize-control {\n    display: none;');
  });

  it('keeps the vertical authoring-to-viewer document below the wide breakpoint', () => {
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const breakpoint = css.match(
      /@media \(max-width: ([0-9.]+)rem\) \{\s*\.workbench,/,
    );

    expect(breakpoint).not.toBeNull();
    expect(Number(breakpoint?.[1]) * 16).toBeGreaterThanOrEqual(1210);
    expect(css).toContain('grid-template-rows: auto auto auto');
    expect(css).not.toContain('data-mobile-pane');
  });
});
