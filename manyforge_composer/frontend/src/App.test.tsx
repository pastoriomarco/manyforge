import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import App from './App';

describe('Composer shell continuity', () => {
  it('renders one pane-based lane-neutral shell within the initial budget', () => {
    const started = performance.now();
    const markup = renderToStaticMarkup(<App />);
    const elapsedMs = performance.now() - started;

    const navigation = markup.indexOf('aria-label="Artifact and node navigation"');
    const workspace = markup.indexOf('aria-label="Authoring workspace"');
    const commands = markup.indexOf('aria-label="Commands and owner status"');
    const footer = markup.indexOf('class="app-footer"');
    expect(markup).toContain('ManyForge Composer');
    expect(markup).toContain('Structural mode');
    expect(navigation).toBeGreaterThan(0);
    expect(workspace).toBeGreaterThan(0);
    expect(commands).toBeGreaterThan(workspace);
    expect(footer).toBeGreaterThan(commands);
    expect(markup).toContain('role="tablist"');
    expect(markup).toContain('Scene / viewer');
    expect(markup).toContain('Commands / owner status');
    expect(markup.match(/id="viewer-title"/g)).toHaveLength(1);
    expect(markup.match(/aria-label="Navigation width"/g)).toHaveLength(1);
    expect(markup.match(/aria-label="Commands width"/g)).toHaveLength(1);
    expect(markup).toContain('Composer owns local drafts and presentation only.');
    expect(elapsedMs).toBeLessThan(2000);
  });

  it('exposes bounded resize, collapse, tab, and responsive presentation semantics', () => {
    const markup = renderToStaticMarkup(<App />);
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const source = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');

    expect(markup).toContain('min="240" max="520"');
    expect(markup).toContain('min="300" max="560"');
    expect(markup).toContain('aria-label="Collapse navigation pane"');
    expect(markup).toContain('aria-label="Collapse commands and status pane"');
    expect(markup).toContain('tabindex="-1"');
    expect(css).toContain('@media (max-width: 76rem)');
    expect(css).toContain('.workbench[data-mobile-pane="workspace"]');
    expect(css).toContain('[role="tabpanel"][hidden]');
    expect(source).toContain("'ArrowLeft', 'ArrowRight', 'Home', 'End'");
    expect(source).toContain('target.current?.focus()');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(source).not.toContain('useShellHeadlessState');
    expect(source).not.toContain('useViewerHeadlessState');
    expect(source).toContain("const viewerActive = workspaceTab === 'scene'");
    expect(source).toContain("mobilePane === 'workspace'");
  });

  it('switches to mobile panes throughout the 1153-1210px overflow interval', () => {
    const css = readFileSync(new URL('./index.css', import.meta.url), 'utf8');
    const breakpoint = css.match(
      /@media \(max-width: ([0-9.]+)rem\) \{\s*\.mobile-pane-tabs/,
    );

    expect(breakpoint).not.toBeNull();
    expect(Number(breakpoint?.[1]) * 16).toBeGreaterThanOrEqual(1210);
  });
});
