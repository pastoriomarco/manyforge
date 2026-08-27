import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

function source(path: string): string {
  return readFileSync(new URL(path, import.meta.url), 'utf8');
}

describe('UI4 application and viewport composition', () => {
  it('keeps one persistent viewer and moves detailed surfaces into the split inspector', () => {
    const app = source('../App.tsx');
    const viewer = source('./StructuralViewer.tsx');
    const styles = source('../index.css');

    expect(app.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(app).toContain('className="global-surface-launchers"');
    expect(app).toContain('>Runtime / history</button>');
    expect(app).toContain('>AI</button>');
    expect(app).not.toContain('className="live-controls-scroll"');
    expect(app).not.toContain('<details className="workbench-disclosure"');
    expect(viewer).toContain('>Robot</button>');
    expect(viewer).toContain('>Object</button>');
    expect(viewer).toContain("toggleInspectorPanel('tools')");
    expect(viewer).toContain('createPortal(');
    expect(viewer).toContain('Maximize viewer inspector');
    expect(viewer).not.toContain('<OverlayPanel');
    expect(viewer).toContain('{transportControls}');
    expect(viewer).toContain("inspectorPanel === 'robot' ? ' robot-inspector-body' : ''");
    expect(viewer).toContain("inspectorPanel === 'robot' && robotControlBlocked");
    expect(viewer).toContain('aria-label="Robot control information"');
    expect(styles).toContain('.central-inspector-body.robot-inspector-body');
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-panel \{[\s\S]*?height: 100%;[\s\S]*?flex-direction: column;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-body \{[\s\S]*?display: flex;[\s\S]*?flex-direction: column;[\s\S]*?overflow: hidden;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-editor \{[\s\S]*?flex: 1 1 auto;[\s\S]*?overflow-y: auto;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-preview \{[\s\S]*?flex: 0 1 auto;[\s\S]*?max-height: 45%;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-footer \{\s*flex: 0 0 auto;/);
    expect(styles).toMatch(/\.robot-control-actions \{[\s\S]*?justify-content: center;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-modes \{[\s\S]*?justify-content: center;/);
    expect(styles).toMatch(/\.central-inspector-actions \.inspector-info-button \{[\s\S]*?border-color: transparent;/);
    expect(styles).toMatch(/\.robot-control-phase \{[\s\S]*?justify-content: center;[\s\S]*?text-align: center;/);
    expect(styles).toContain('--inspector-control-height: 1.9rem;');
    expect(styles).toContain('--inspector-row-padding: 0.34rem;');
    expect(styles).toMatch(/\.central-inspector-section \.program-property \{[\s\S]*?padding: var\(--inspector-row-padding\) 0;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-joint \{[\s\S]*?padding: var\(--inspector-row-padding\) 0;/);
    expect(styles).toContain('.central-inspector-notice-warning');
  });

  it('keeps transport scopes explicit and uses the exact operation-scoped velocity owner', () => {
    const transport = source('./ProgramRuntimePanel.tsx');
    const robotControl = source('../hooks/useRobotControl.ts');
    const styles = source('../index.css');

    expect(transport).toContain("canResume ? 'Resume Motion' : 'Run'");
    expect(transport).toContain("canResume ? robot.resume() : program.run()");
    expect(transport).toContain('transport-stop-primary');
    expect(transport).toContain('transport-stop-disclosure');
    expect(transport).toContain('Stop Execution');
    expect(transport).toMatch(/transport-stop-group[\s\S]*transport-stop-primary[\s\S]*transport-stop-disclosure[\s\S]*transport-stop-menu/);
    expect(transport).not.toContain("motionPaused ? robot.resume() : robot.pause()");
    expect(transport).toContain('Full-cell Stop unavailable');
    expect(transport).toContain('does not publish the cell Stop generation');
    expect(transport).toContain("robot.state.operation?.state === 'paused'");
    expect(transport).toContain('robot.setRuntimeScale');
    expect(transport).toContain('robot.runtimeScaleStatus.supported');
    expect(transport).not.toContain('api.execution.fullStop');
    expect(transport).not.toContain('setVelocityScale');
    expect(robotControl).toContain('runtimeScaleQueued.current = bounded');
    expect(robotControl).toContain('runtimeScaleInFlight.current');
    expect(robotControl).toContain('clearRuntimeScaleQueue();');
    expect(robotControl).toContain('api.execution.runtimeScale(context.operationId');
    expect(styles).toMatch(/\.transport-stop-group \{[\s\S]*?position: relative;[\s\S]*?display: flex;/);
    expect(styles).toMatch(/\.transport-stop-menu \{[\s\S]*?position: absolute;[\s\S]*?bottom: calc\(100% \+ 0\.35rem\);/);
    expect(styles).toContain('--viewer-transport-control-height: 2.85rem;');
    expect(styles).toMatch(/\.viewer-transport > button,[\s\S]*?\.viewer-transport \.transport-stop-group > button \{[\s\S]*?height: var\(--viewer-transport-control-height\);[\s\S]*?font-size: 0\.84rem;/);
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
