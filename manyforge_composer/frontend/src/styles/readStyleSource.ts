import { readFileSync } from 'node:fs';

// Test support only (never imported by the application): the stylesheet as
// one text in cascade (import) order, with --mf-* tokens resolved to their
// values, so source-level assertions check the values that take effect.

const stylesRoot = new URL('./', import.meta.url);

export function styleFiles(): string[] {
  const entry = readFileSync(new URL('index.css', stylesRoot), 'utf8');
  return [...entry.matchAll(/@import\s+['"]\.\/([^'"]+)['"]/g)].map((match) => match[1]);
}

export function styleTokens(): Map<string, string> {
  const tokens = new Map<string, string>();
  const text = readFileSync(new URL('tokens.css', stylesRoot), 'utf8');
  for (const match of text.matchAll(/(--mf-[\w-]+):\s*([^;]+);/g)) {
    tokens.set(match[1], match[2].trim());
  }
  return tokens;
}

export function resolveStyleTokens(text: string, tokens = styleTokens()): string {
  let resolved = text;
  for (let pass = 0; pass < 3; pass += 1) {
    resolved = resolved.replace(/var\((--mf-[\w-]+)\)/g, (whole, name: string) => tokens.get(name) ?? whole);
  }
  return resolved;
}

export function readStyleSource(): string {
  const tokens = styleTokens();
  return styleFiles()
    .filter((file) => file !== 'tokens.css')
    .map((file) => resolveStyleTokens(readFileSync(new URL(file, stylesRoot), 'utf8'), tokens))
    .join('\n');
}
