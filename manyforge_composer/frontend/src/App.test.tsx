import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import App from './App';

describe('Composer shell continuity', () => {
  it('renders one compact application spine without duplicate authority surfaces', () => {
    const started = performance.now();
    const markup = renderToStaticMarkup(<App />);
    const elapsedMs = performance.now() - started;

    const workspace = markup.indexOf('aria-label="Program authoring workspace"');
    const commands = markup.indexOf('aria-label="Viewer and operations rail"');
    const viewer = markup.indexOf('id="viewer-title"');
    const sceneAuthoring = markup.indexOf('id="scene-authoring-title"');
    const runtime = markup.indexOf('id="program-runtime-title"');

    expect(markup).toContain('aria-label="ManyForge Composer"');
    expect(markup).toContain('aria-label="Workbench sections"');
    expect(markup).toContain('>Program</button>');
    expect(markup).toContain('>Deployment</button>');
    expect(markup).toContain('aria-label="Program document controls"');
    expect(markup).toContain('class="runtime-status-overlay"');
    expect(workspace).toBeGreaterThan(0);
    expect(commands).toBeGreaterThan(workspace);
    expect(viewer).toBeGreaterThan(commands);
    expect(sceneAuthoring).toBeGreaterThan(viewer);
    expect(runtime).toBeGreaterThan(sceneAuthoring);
    expect(markup).toContain('Runtime, alarms, and history');
    expect(markup).not.toContain('Catalog and artifact library');
    expect(markup).not.toContain('Artifacts · Program / Deployment');
    expect(markup).not.toContain('Headless runtime truth is connected');
    expect(markup).not.toContain('class="app-footer"');
    expect(markup.match(/class="runtime-status-overlay"/g)).toHaveLength(1);
    expect(markup.match(/role="separator"/g)).toHaveLength(1);
    expect(elapsedMs).toBeLessThan(2000);
  });

  it('keeps the canvas fixed and gives scrolling to bounded side surfaces', () => {
    const markup = renderToStaticMarkup(<App />);
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const source = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const workspaceSource = readFileSync(
      new URL('./components/ProgramWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const sceneSource = readFileSync(
      new URL('./components/SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(markup).toContain('aria-label="Collapse viewer and operations pane"');
    expect(markup).toContain('aria-label="Expand Operations pane"');
    expect(markup).toContain('aria-label="Collapse node palette"');
    expect(markup).toContain('aria-label="Collapse Program inspector"');
    expect(css).toContain('.program-inspector-scroll');
    expect(css).toContain('.program-palette-list');
    expect(css).toContain('.program-tree-react-flow');
    expect(css).toMatch(/\.program-tree-react-flow\s*\{[\s\S]*?height: 100%;[\s\S]*?min-height: 0;/);
    expect(css).toMatch(/\.program-inspector-scroll\s*\{[\s\S]*?overflow-y: auto;/);
    expect(css).toMatch(/\.workbench-workspace,\s*\n\.workbench-pane\s*\{[\s\S]*?overflow: hidden;/);
    expect(source).toContain('role="separator"');
    expect(source).toContain('setPointerCapture');
    expect(source).toContain("event.key === 'ArrowLeft'");
    expect(source).toContain("event.key === 'ArrowRight'");
    expect(source).toContain("event.key === 'Home'");
    expect(source).toContain("event.key === 'End'");
    expect(source).toContain('target.current?.focus()');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(sceneSource.match(/<StructuralViewer/g)).toHaveLength(1);
    expect(workspaceSource).toContain("'declarations'");
    expect(workspaceSource).toContain('<ProgramDeclarationsEditor');
    expect(source).not.toContain('<CatalogPanel');
    expect(source).not.toContain('<ArtifactBrowser');
    expect(source).not.toContain('<UnavailableBanner');
    expect(source).not.toContain('type="range"');
  });

  it('keeps document, deployment, status, and artifact workflows in their owned locations', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const controlsSource = readFileSync(
      new URL('./components/ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );

    expect(appSource).toContain('<ProgramDocumentControls');
    expect(appSource).toContain('<DeploymentCompositionPanel');
    expect(appSource).toContain('<RuntimeStatusOverlay');
    expect(controlsSource).toContain('<ArtifactBrowser lockedKind="program"');
    expect(controlsSource).toContain('Program Apply never changes the deployment.');
    expect(controlsSource).not.toContain('Update Draft');
  });
});
