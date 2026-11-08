import { readdirSync, readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';
import { readStyleSource, styleFiles, styleTokens } from './readStyleSource';

// Style system guard: colors live only in the two token files, every class the
// components use has a rule, and no rule prelude is written twice in one file.

const stylesRoot = new URL('./', import.meta.url);
const srcRoot = new URL('../', import.meta.url);

function read(url: URL): string {
  return readFileSync(url, 'utf8');
}

function withoutComments(css: string): string {
  return css.replace(/\/\*[\s\S]*?\*\//g, '');
}

function componentSources(): Array<[string, string]> {
  return readdirSync(srcRoot, { recursive: true, encoding: 'utf8' })
    .filter((path) => /\.tsx?$/.test(path) && !/\.test\.tsx?$/.test(path))
    .map((path) => [path, read(new URL(path, srcRoot))] as [string, string]);
}

// Structural and behavior hooks without their own look. Shrink this list;
// never grow it for a visible element.
const UNSTYLED_CLASS_HOOKS = new Set([
  'artifact-panel',
  'catalog-panel',
  'deployment-applied-comparison',
  'deployment-composition-panel',
  'history-correlation-filters',
  'history-retention',
  'overlay-close',
  'palette-error',
  'program-history-list',
  'program-motion-bindings',
  'program-motion-defaults',
  'program-notice-dismiss',
  'program-notice-dismiss-all',
  'program-notice-expand',
  'program-palette-category-toggle',
  'program-schema-tuple',
  'program-side-rail-left',
  'program-topology-node-type',
  'program-tree-action-row-placement',
  'program-tree-action-row-type',
  'runtime-detail-group',
  'scene-shape-fields',
  'transport-full-stop',
  'transport-group-stop',
  'transport-settings-button',
  'viewer-inspector-slot',
]);

// Kept because merging them would move a declaration across rules of equal
// specificity in between (see src/styles/README.md).
const REPEATED_PRELUDES = new Set([
  'surfaces/document-controls.css|.document-save-as-path, .confirmation-check',
  'surfaces/document-controls.css|.confirmation-check',
  'surfaces/program-canvas.css|.program-palette, .program-tree-surface',
  'surfaces/program-canvas.css|.program-canvas-toolbar',
  'surfaces/shell.css|.app-shell',
  'surfaces/shell.css|.workbench',
  'surfaces/shell.css|.workbench-pane > .panel, .workbench-workspace .panel',
]);

function preludes(css: string): string[] {
  const found: string[] = [];
  let context = '';
  let depth = 0;
  let buffer = '';
  for (const character of withoutComments(css)) {
    if (character === '{') {
      const prelude = buffer.trim().split(/\s+/).join(' ');
      if (prelude.startsWith('@')) {
        context = prelude;
      } else if (!/^(from|to|\d+%)/.test(prelude)) {
        found.push(`${context}|${prelude}`.replace(/^\|/, ''));
      }
      depth += 1;
      buffer = '';
    } else if (character === '}') {
      depth -= 1;
      if (depth === 0) context = '';
      buffer = '';
    } else if (character === ';') {
      buffer = '';
    } else {
      buffer += character;
    }
  }
  return found;
}

describe('style system', () => {
  const files = styleFiles();

  it('keeps raw colors in tokens.css and tokens.ts only', () => {
    const rawColor = /#[0-9a-fA-F]{3,8}\b|\b(?:rgba?|hsla?)\(|(?<![\w-])(?:white|black)(?![\w-])/;
    for (const file of files.filter((name) => name !== 'tokens.css')) {
      const css = withoutComments(read(new URL(file, stylesRoot)));
      expect(css.match(rawColor)?.[0], file).toBeUndefined();
    }
    for (const [path, text] of componentSources()) {
      if (path.replace(/\\/g, '/') === 'styles/tokens.ts') continue;
      expect(text.match(/['"`]#[0-9a-fA-F]{3,8}['"`]/)?.[0], path).toBeUndefined();
    }
  });

  it('gives every component class a rule', () => {
    const css = files.map((file) => read(new URL(file, stylesRoot))).join('\n');
    const defined = new Set([...withoutComments(css).matchAll(/\.([a-zA-Z_][\w-]*)/g)].map((match) => match[1]));
    const missing = new Set<string>();
    for (const [, text] of componentSources()) {
      for (const match of text.matchAll(/className=(?:"([^"]*)"|\{`([^`]*)`\})/g)) {
        const value = (match[1] ?? match[2] ?? '').replace(/\$\{[^}]*\}/g, ' ');
        for (const name of value.split(/\s+/)) {
          if (/^[a-z]\w*(?:-\w+)+$/.test(name) && !defined.has(name) && !UNSTYLED_CLASS_HOOKS.has(name)) {
            missing.add(name);
          }
        }
      }
    }
    expect([...missing]).toEqual([]);
  });

  it('gives dialog action buttons one shared confirmation size and color treatment', () => {
    const css = readStyleSource();
    const tokens = styleTokens();
    const sizing = css.match(/\.dialog-actions > button \{([^}]*)\}/)?.[1] ?? '';
    const sizingRules = [...css.matchAll(/\.dialog-actions > button \{/g)];
    const primary = css.match(
      /\.dialog-actions > \.primary-action:not\(:disabled\),\s*\.dialog-actions > \.transport-run:not\(:disabled\) \{([^}]*)\}/,
    )?.[1] ?? '';
    const primaryHover = css.match(
      /\.dialog-actions > \.primary-action:hover:not\(:disabled\),\s*\.dialog-actions > \.transport-run:hover:not\(:disabled\) \{([^}]*)\}/,
    )?.[1] ?? '';
    const danger = css.match(/\.danger-button:not\(:disabled\) \{([^}]*)\}/)?.[1] ?? '';

    expect(sizingRules).toHaveLength(1);
    expect(sizing).toContain('min-width: 0;');
    expect(sizing).toContain('min-height: 1.9rem;');
    expect(sizing).toContain('height: auto;');
    expect(sizing).toContain('padding: 0.3rem 0.8rem;');
    expect(sizing).toContain('font-size: 0.72rem;');
    expect(primary).toContain('border-color: ' + tokens.get('--mf-border-success-35') + ';');
    expect(primary).toContain('color: ' + tokens.get('--mf-text-success-82') + ';');
    expect(primary).toContain('background: ' + tokens.get('--mf-bg-success-15') + ';');
    expect(primaryHover).toContain('border-color: ' + tokens.get('--mf-border-success-51') + ';');
    expect(primaryHover).toContain('background: ' + tokens.get('--mf-bg-success-20') + ';');
    expect(danger).toContain('border-color: ' + tokens.get('--mf-border-danger-38') + ';');
    expect(danger).toContain('color: ' + tokens.get('--mf-text-danger-81') + ';');
    expect(danger).toContain('background: ' + tokens.get('--mf-bg-danger-16') + ';');
    expect(css).toContain('.dialog-actions > .primary-action:hover:not(:disabled)');
    expect(css).not.toContain('.transport-confirmation .dialog-actions > button');
  });

  it('keeps the selected Scenario description inside standard card spacing', () => {
    const css = readStyleSource();
    expect(css).toMatch(
      /\.dcard > \.scenario-status:not\(:first-child\) \{[^}]*margin: 0 0\.65rem 0\.65rem;[^}]*padding: 0\.4rem 0\.6rem;/s,
    );
    expect(css).toContain(':not(.scenario-status)');
  });

  it('writes each rule prelude once per file and context', () => {
    const repeated: string[] = [];
    const unbalanced: string[] = [];
    for (const file of files) {
      const seen = new Set<string>();
      for (const prelude of preludes(read(new URL(file, stylesRoot)))) {
        const key = `${file}|${prelude}`;
        if (prelude.split('(').length !== prelude.split(')').length) unbalanced.push(key);
        if (seen.has(prelude) && !REPEATED_PRELUDES.has(key)) repeated.push(key);
        seen.add(prelude);
      }
    }
    expect(repeated).toEqual([]);
    expect(unbalanced).toEqual([]);
  });
});
