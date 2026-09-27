import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';
import { readStyleSource } from '../styles/readStyleSource';

function source(path: string): string {
  return readFileSync(new URL(path, import.meta.url), 'utf8');
}

describe('UI4 application and viewport composition', () => {
  it('keeps one persistent viewer and moves detailed surfaces into the split inspector', () => {
    const app = source('../App.tsx');
    const viewer = source('./StructuralViewer.tsx');
    const styles = readStyleSource();

    expect(app.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(app).toContain('className="global-surface-launchers"');
    expect(app).toContain('Runtime / history');
    expect(app).toContain('global-attention-count');
    expect(app).toContain('>AI</button>');
    expect(app).not.toContain('className="live-controls-scroll"');
    expect(app).toContain('<WorkbenchNotificationStack');
    expect(app).toContain("errorRow={globalPanel === 'runtime' || errorItemCount > 0 ? (");
    expect(app).not.toContain('<details className="workbench-disclosure"');
    expect(viewer).toContain('>Robot</button>');
    expect(viewer).toContain('>Object</button>');
    expect(viewer).toContain("toggleInspectorPanel('tools')");
    expect(viewer).toContain('createPortal(');
    expect(viewer).toContain('Maximize viewer inspector');
    expect(viewer).not.toContain('<OverlayPanel');
    expect(viewer).toContain("typeof transportControls === 'function'");
    expect(viewer).toContain('}) : transportControls}');
    expect(viewer).toContain("inspectorPanel === 'robot' ? ' robot-inspector-body' : ''");
    expect(viewer).toContain("inspectorPanel === 'robot' && robotControlBlocked");
    expect(viewer).toContain('aria-label="Robot control information"');
    expect(viewer).toContain('{props.runtimeOverlay}');
    expect(styles).toContain('.central-inspector-body.robot-inspector-body');
    // Shared panel geometry applies in docked and floating hosts. The real
    // Firefox qualification checks the resulting pinned footer at each size;
    // these source checks must not require a duplicate inspector-only layer.
    expect(styles).toMatch(/\.robot-control-panel \{[^}]*height: 100%;[^}]*flex-direction: column;/);
    expect(styles).toMatch(/\.robot-control-body \{[^}]*display: flex;[^}]*flex-direction: column;[^}]*overflow: hidden;/);
    expect(styles).toMatch(/\.robot-control-editor \{[^}]*flex: 1 1 auto;[^}]*overflow: auto;/);
    expect(styles).toMatch(/\.robot-control-preview \{[^}]*flex: 0 0 auto;[^}]*max-height: min\(40vh, 24rem, 100%\);/);
    expect(styles).toMatch(/\.robot-control-preview-notice \{[^}]*position: absolute;/);
    expect(styles).toMatch(/\.robot-control-footer \{\s*flex: 0 0 auto;/);
    expect(styles).toMatch(/\.robot-control-actions \{[\s\S]*?justify-content: center;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-modes \{[\s\S]*?justify-content: center;/);
    expect(styles).toMatch(/\.central-inspector-actions \.inspector-info-button \{[\s\S]*?border-color: transparent;/);
    expect(styles).toMatch(/\.robot-control-phase \{[\s\S]*?justify-content: center;[\s\S]*?text-align: center;/);
    expect(styles).toContain('--inspector-control-height: 1.9rem;');
    expect(styles).toContain('--inspector-row-padding: 0.34rem;');
    expect(styles).toMatch(/\.central-inspector-section \.program-property \{[\s\S]*?padding: var\(--inspector-row-padding\) 0;/);
    expect(styles).toMatch(/\.viewer-inspector-section \.robot-control-joint \{[\s\S]*?padding: var\(--inspector-row-padding\) 0;/);
    // Inspector warnings use the shared warning notice (UI6-I-VC VC4).
    expect(styles).toContain('.notice.severity-warning');
    // Notifications sit below the floating viewer controls with their insets.
    expect(styles).toMatch(/\.workbench-notification-stack \{[\s\S]*?top: calc\(1\.9rem \+ 2 \* 0\.65rem\);[\s\S]*?left: 0\.65rem;/);
    expect(styles).toMatch(/\.workbench-notification-stack > \.runtime-observability-surface \{[^}]*position: static;/);
    expect(styles).not.toContain('.viewer-slot > .runtime-observability-surface');
  });

  it('keeps explicit group columns separate from unique cell controls', () => {
    const transport = source('./ProgramRuntimePanel.tsx');
    const robotControl = source('../hooks/useRobotControl.ts');
    const styles = readStyleSource();

    expect(transport).toContain('program.resumeMotion([group.groupId])');
    expect(transport).toContain('program.stopMotion([group.groupId])');
    expect(transport).toContain('transport-group-column');
    expect(transport).toContain('transport-unique-column');
    expect(transport).toContain('transport-group-settings');
    expect(transport).toContain("'Halting' : 'Halt'");
    expect(transport).toContain('program.fullStop()');
    expect(transport).toContain('program.resumeFullStop()');
    expect(transport).toContain('Cell Full Stop');
    expect(transport).not.toContain('transport-stop-disclosure');
    expect(transport).not.toContain("motionPaused ? robot.resume() : robot.pause()");
    expect(transport).toContain("robot.fullStop()");
    expect(transport).toContain('expectedStopGeneration');
    expect(transport).toContain('Latest certificate');
    expect(transport).toContain('program.setGroupRuntimeScale');
    expect(robotControl).toContain("sendExecutionCommand(");
    expect(robotControl).toContain("'cell_full_stop'");
    expect(transport).not.toContain('setVelocityScale');
    expect(robotControl).toContain('runtimeScaleQueued.current = bounded');
    expect(robotControl).toContain('runtimeScaleInFlight.current');
    expect(robotControl).toContain('clearRuntimeScaleQueue();');
    expect(robotControl).toContain("'runtime_scale'");
    expect(robotControl).not.toContain('api.execution.fullStop');
    expect(robotControl).not.toContain('api.execution.setRuntimeScaleControl');
    expect(robotControl).not.toContain('api.execution.runtimeScaleControl');
    expect(robotControl).not.toContain('programRuntimeScaleOffer');
    expect(transport).toContain('Scale current and subsequent moves');
    expect(styles).toMatch(/\.viewer-transport \{[^}]*grid-template-columns:[^;]*var\(--viewer-transport-motion-panel-width\)[^;]*minmax\(0, 1fr\)[^;]*var\(--viewer-transport-column-width\);/s);
    expect(styles).toMatch(/\.transport-control-layout \{[^}]*display: contents;/s);
    // Guidance and recovery moved to the one notification stack; nothing is
    // positioned between the Motion and Program panels any more.
    expect(styles).not.toContain('.transport-guidance');
    expect(transport).not.toContain('transport-guidance');
    expect(styles).toMatch(/\.viewer-transport \{[^}]*padding: 0;[^}]*overflow: visible;/s);
    expect(styles).not.toMatch(/\.viewer-transport \{[^}]*(?:border|background):/s);
    expect(styles).toMatch(/\.transport-motion-panel \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);/s);
    expect(styles).toMatch(/\.transport-group-grid \{[^}]*justify-content: flex-start;[^}]*padding: 0;[^}]*overflow-x: auto;/s);
    expect(styles).toMatch(/\.transport-panel-title \{[^}]*font-size: 0\.96rem;[^}]*line-height: 1\.2rem;/s);
    expect(styles).toMatch(/\.transport-unique-column \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);[^}]*margin-left: auto;/s);
    expect(styles).toContain('--viewer-transport-control-height: 2.85rem;');
    expect(styles).toContain('--viewer-transport-column-width: 7rem;');
    expect(styles).toMatch(/\.viewer-transport button \{[\s\S]*?height: var\(--viewer-transport-control-height\);[\s\S]*?font-size: 0\.84rem;/);
    expect(styles).toMatch(/\.transport-group-scale \{[\s\S]*?"label output"[\s\S]*?"slider slider";/);
    expect(styles).toMatch(/\.transport-group-scale input \{[\s\S]*?grid-area: slider;[\s\S]*?width: 100%;/);
    expect(styles).toMatch(/\.transport-group-scale \{[^}]*border: 1px solid #405764;[^}]*border-radius: 0\.45rem;/s);
    expect(styles).toMatch(/\.transport-unique-icons \{[\s\S]*?grid-template-columns: repeat\(2, minmax\(0, 1fr\)\);/);
  });

  it('locates lifecycle control under Deployment and binds selection to Manager graph identities', () => {
    const app = source('../App.tsx');
    const deployment = source('./DeploymentCompositionPanel.tsx');
    const lifecycle = source('./CCLCPanel.tsx');

    expect(app).not.toContain('<CCLCPanel');
    expect(deployment).toContain('<CCLCPanel');
    expect(deployment).toContain('admissionGeneration={snapshot?.authoringAdmission.admissionGeneration ?? null}');
    expect(deployment).toContain('candidate.controllable_component_id');
    expect(lifecycle).toContain('<select');
    expect(lifecycle).not.toContain('placeholder="lite6-arm"');
  });
});
