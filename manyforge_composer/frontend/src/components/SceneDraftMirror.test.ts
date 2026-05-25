/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

function sceneDraftMirrorSource(): string {
  const sourcePath = join(dirname(fileURLToPath(import.meta.url)), 'ScenePanel.tsx');
  const source = readFileSync(sourcePath, 'utf8');
  const match = source.match(
    /export function SceneDraftMirror\(\) \{[\s\S]*?\n\}\n\nexport function ScenePanel/,
  );
  if (!match) {
    throw new Error('SceneDraftMirror source block not found');
  }
  return match[0];
}

describe('SceneDraftMirror authority boundary', () => {
  it('delegates authored-scene reconciliation to the backend sync route', () => {
    const source = sceneDraftMirrorSource();

    expect(source).toContain('api.scene.syncAuthored');
    expect(source).not.toMatch(/\bapi\.scene\.state\(/);
    expect(source).not.toMatch(/\bapi\.scene\.edit\(/);
  });
});
