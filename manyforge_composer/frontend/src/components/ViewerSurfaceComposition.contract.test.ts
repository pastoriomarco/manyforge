import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

function source(path: string): string {
  return readFileSync(new URL(path, import.meta.url), 'utf8');
}

describe('UI4 application and viewport composition', () => {
  it('keeps one persistent viewer and moves detailed surfaces behind bounded launchers', () => {
    const app = source('../App.tsx');
    const viewer = source('./StructuralViewer.tsx');

    expect(app.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(app).toContain('className="global-surface-launchers"');
    expect(app).toContain('>Runtime / history</button>');
    expect(app).toContain('>AI</button>');
    expect(app).not.toContain('className="live-controls-scroll"');
    expect(app).not.toContain('<details className="workbench-disclosure"');
    expect(viewer).toContain('>Robot</button>');
    expect(viewer).toContain('>Object</button>');
    expect(viewer).toContain('<summary>Tools</summary>');
    expect(viewer).toContain('{transportControls}');
  });

  it('keeps transport scopes explicit and uses the exact operation-scoped velocity owner', () => {
    const transport = source('./ProgramRuntimePanel.tsx');
    const robotControl = source('../hooks/useRobotControl.ts');

    expect(transport).toContain('Stop motion');
    expect(transport).toContain('Stop Program');
    expect(transport).toContain('Full-cell Stop unavailable');
    expect(transport).toContain('does not publish the cell Stop generation');
    expect(transport).toContain("robot.state.operation?.state === 'paused'");
    expect(transport).toContain('robot.resume() : robot.pause()');
    expect(transport).toContain('robot.setRuntimeScale');
    expect(transport).toContain('robot.runtimeScaleStatus.supported');
    expect(transport).not.toContain('api.execution.fullStop');
    expect(transport).not.toContain('setVelocityScale');
    expect(robotControl).toContain('runtimeScaleQueued.current = bounded');
    expect(robotControl).toContain('runtimeScaleInFlight.current');
    expect(robotControl).toContain('clearRuntimeScaleQueue();');
    expect(robotControl).toContain('api.execution.runtimeScale(context.operationId');
  });

  it('locates lifecycle control under Deployment and binds selection to Manager graph identities', () => {
    const app = source('../App.tsx');
    const deployment = source('./DeploymentCompositionPanel.tsx');
    const lifecycle = source('./CCLCPanel.tsx');

    expect(app).not.toContain('<CCLCPanel');
    expect(deployment).toContain('<CCLCPanel enabled={enabled} componentIds={componentIds} />');
    expect(deployment).toContain('candidate.controllable_component_id');
    expect(lifecycle).toContain('<select');
    expect(lifecycle).not.toContain('placeholder="lite6-arm"');
  });
});
