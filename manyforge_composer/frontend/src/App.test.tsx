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
    const commands = markup.indexOf('aria-label="3D viewer and live controls"');
    const viewer = markup.indexOf('id="viewer-title"');

    expect(markup).toContain('aria-label="ManyForge Composer"');
    expect(markup).toContain('aria-label="Workbench sections"');
    expect(markup).toContain('>Program</button>');
    expect(markup).toContain('>Scene</button>');
    expect(markup).toContain('>Deployment</button>');
    expect(markup).toContain('aria-label="Program document controls"');
    expect(markup).toContain('class="runtime-status-overlay"');
    expect(workspace).toBeGreaterThan(0);
    expect(commands).toBeGreaterThan(workspace);
    expect(viewer).toBeGreaterThan(commands);
    expect(markup).not.toContain('id="program-runtime-title"');
    expect(markup).toContain('Runtime / history');
    expect(markup).toContain('>AI</button>');
    expect(markup).not.toContain('class="live-controls-scroll"');
    expect(markup).not.toContain('Catalog and artifact library');
    expect(markup).not.toContain('Artifacts · Program / Deployment');
    expect(markup).not.toContain('Headless runtime truth is connected');
    expect(markup).not.toContain('class="app-footer"');
    expect(markup.match(/class="runtime-status-overlay"/g)).toHaveLength(1);
    expect(markup.match(/role="separator"/g)).toHaveLength(3);
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
    const treeSource = readFileSync(
      new URL('./components/ProgramTreeCanvas.tsx', import.meta.url),
      'utf8',
    );
    const dividerSource = readFileSync(
      new URL('./components/PaneDivider.tsx', import.meta.url),
      'utf8',
    );
    const overlaySource = readFileSync(
      new URL('./components/OverlayPanel.tsx', import.meta.url),
      'utf8',
    );

    expect(markup).not.toContain('aria-label="Collapse viewer and operations pane"');
    expect(markup).not.toContain('aria-label="Expand Operations pane"');
    expect(markup).toContain('aria-label="Resize viewer and live controls pane"');
    expect(markup).toContain('aria-label="Resize central inspector pane"');
    expect(markup).toContain('aria-label="Resize Program tools pane"');
    expect(markup).toContain('aria-label="Collapse node palette"');
    expect(workspaceSource).toContain('Minimize Program inspector');
    expect(workspaceSource).toContain('Maximize Program inspector');
    expect(workspaceSource).toContain('<header className="central-inspector-header">');
    expect(workspaceSource).not.toContain('program-inspector-tabs');
    expect(source).toContain('viewer-inspector-slot');
    expect(source).not.toContain('central-inspector-slot viewer-slot');
    expect(source).toContain('setViewerInspectorMinimized(!alreadyMaximized)');
    expect(source).toContain('setProgramInspectorMinimized(!alreadyMaximized)');
    expect(source).toContain('axis="y"');
    expect(source).toContain('className="central-inspector-divider"');
    expect(css).toContain('.program-inspector-scroll');
    expect(css).toContain('.program-palette-list');
    expect(css).toContain('.program-tree-react-flow');
    expect(css).toMatch(/\.program-tree-react-flow\s*\{[\s\S]*?height: 100%;[\s\S]*?min-height: 0;/);
    expect(css).toMatch(/\.program-inspector-scroll\s*\{[\s\S]*?overflow-y: auto;/);
    expect(css).toContain('.workbench-inspector');
    expect(css).toContain('.central-inspector-slot');
    expect(css).toContain('--inspector-control-height: 1.9rem');
    expect(css).toContain('.central-inspector-header > strong');
    expect(css).toContain("input:not([type='checkbox']):not([type='range'])");
    expect(source).toContain('<PaneDivider');
    expect(dividerSource).toContain('role="separator"');
    expect(dividerSource).toContain('setPointerCapture');
    expect(dividerSource).toContain("axis === 'x' ? 'ArrowLeft' : 'ArrowUp'");
    expect(dividerSource).toContain("axis === 'x' ? 'ArrowRight' : 'ArrowDown'");
    expect(dividerSource).toContain("'ArrowUp'");
    expect(dividerSource).toContain("'ArrowDown'");
    expect(dividerSource).toContain("event.key === 'Home'");
    expect(dividerSource).toContain("event.key === 'End'");
    expect(overlaySource).toContain("event.key !== 'Escape'");
    expect(overlaySource).toContain('returnFocusRef?.current?.focus()');
    expect(overlaySource).toMatch(/event\.preventDefault\(\);\s*close\(\);/);
    expect(treeSource).toContain('<ProgramNoticeRail');
    expect(workspaceSource).toContain('PROGRAM_INFO_NOTICE_TIMEOUT_MS = 4_000');
    expect(workspaceSource).toContain('onDismissAllNotices={dismissAllNotices}');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(sceneSource.match(/<StructuralViewer/g)).toHaveLength(1);
    expect(sceneSource).toContain('createPortal(editorSurface, editorTarget)');
    expect(workspaceSource).toContain("'params'");
    expect(workspaceSource).toContain('<ProgramDeclarationsNavigator');
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
    expect(appSource).toContain('error={headlessState.error}');
    expect(appSource).not.toContain("source: 'composer.headless_projection'");
    expect(appSource).toContain('expanded={runtimePanelExpanded}');
    expect(appSource).toMatch(
      /const openRuntimeAttention[\s\S]*setRuntimePanelExpanded\(false\)[\s\S]*setGlobalPanel\('runtime'\)/,
    );
    expect(appSource).not.toContain('panelClassName="runtime-observability-overlay"');
    expect(controlsSource).toContain('<ArtifactBrowser lockedKind="program"');
    expect(controlsSource).toContain('Its Deployment will not change.');
    expect(controlsSource).not.toContain('Update Draft');
  });
});
