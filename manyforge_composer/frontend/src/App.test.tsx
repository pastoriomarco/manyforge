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
    expect(markup.match(/id="viewer-title"/g)).toHaveLength(1);
    expect(markup.match(/role="separator"/g)).toHaveLength(2);
    expect(markup).toContain('aria-label="Resize Library pane"');
    expect(markup).toContain('aria-label="Resize Operations pane"');
    expect(markup).toContain('aria-valuemin="220"');
    expect(markup).toContain('aria-valuemax="420"');
    expect(markup).toContain('aria-valuemin="480"');
    expect(markup).toContain('aria-valuemax="720"');
    expect(markup).toContain('<details class="workbench-disclosure" open="">');
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

    expect(markup).toContain('aria-label="Collapse library pane"');
    expect(markup).toContain('aria-label="Collapse viewer and operations pane"');
    expect(markup).toContain('aria-label="Expand Library pane"');
    expect(markup).toContain('aria-label="Expand Operations pane"');
    expect(css).toContain('@media (max-width: 76rem)');
    expect(css).toContain('height: 100vh;');
    expect(css).toContain('flex: 1 1 auto;');
    expect(css).toContain('.pane-divider');
    expect(source).toContain('role="separator"');
    expect(css).toContain('.workbench-workspace');
    expect(css).toContain('.visual-operations-stack > .viewer-panel');
    expect(css).toContain('.program-authoring-empty');
    expect(source).toContain('const viewerActive = !commandsCollapsed');
    expect(source).toContain('setPointerCapture');
    expect(source).toContain("event.key === 'ArrowLeft'");
    expect(source).toContain("event.key === 'ArrowRight'");
    expect(source).toContain("event.key === 'Home'");
    expect(source).toContain("event.key === 'End'");
    expect(source).toContain('target.current?.focus()');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(sceneSource.match(/<StructuralViewer/g)).toHaveLength(1);
    expect(source).not.toContain('workspaceTab');
    expect(source).not.toContain('mobilePane');
    expect(source).not.toContain('setWorkspaceTab');
    expect(source).not.toContain('useEffect');
    expect(source).not.toContain('onMount');
    expect(css).toContain('  .pane-divider {\n    display: none;');
    expect(source).not.toContain('type="range"');
  });

  it('keeps the vertical authoring-to-viewer document below the wide breakpoint', () => {
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const breakpoint = [...css.matchAll(
      /@media \(max-width: ([0-9.]+)rem\) \{([\s\S]*?)\n\}(?=\s*@media|\s*$)/g,
    )].find((match) => match[2].includes('.workbench,'));

    expect(breakpoint).toBeDefined();
    expect(breakpoint?.[2]).toContain('.workbench,');
    expect(Number(breakpoint?.[1]) * 16).toBeGreaterThanOrEqual(1210);
    expect(css).toContain('grid-template-rows: auto auto auto');
    expect(css).not.toContain('data-mobile-pane');
  });
});
