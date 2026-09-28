import type { RefObject } from 'react';

import { OverlayPanel } from './OverlayPanel';
import { ProgramCategoryIcon, ProgramHeldBadge, ProgramKindSymbol } from './programNodeGlyphs';

const STATUSES = [
  ['idle', 'Idle', 'Not reached yet in this run, or no run.'],
  ['running', 'Running', 'Executing now.'],
  ['held', 'Waiting for Run', 'Executing, but held by a Stop: its work (motion, gripper, Scene edits…) starts when Run is pressed in the Motion panel.'],
  ['success', 'Success', 'Finished successfully.'],
  ['failure', 'Failure', 'Failed; the parent decides what happens next.'],
] as const;

const FAMILIES = [
  ['control_flow', 'Control flow and operation control', 'Sequence, Fallback, Parallel, Retry…'],
  ['motion', 'Motion', 'Joint and Cartesian moves.'],
  ['gripper', 'Gripper', 'Gripper commands and checks.'],
  ['geometry', 'World', 'Scene, model, geometry and simulation.'],
  ['logic', 'Logic', 'Values, conditions, IO and assistant.'],
] as const;

const SHORTCUTS = [
  ['Ctrl+D', 'Duplicate the selected node'],
  ['Ctrl+C / Ctrl+V', 'Copy the selected subtree; paste after it (or on the canvas when nothing is selected)'],
  ['Delete / Backspace', 'Delete the selected node, or disconnect the selected link'],
  ['Arrow keys', 'Move between nodes; Enter or Space selects'],
  ['Double-click a link', 'Disconnect it'],
] as const;

/**
 * Legend and short how-to for the Program tree editor, opened by its "?"
 * button. Presentation only.
 */
export function ProgramCanvasHelp({
  open,
  onClose,
  returnFocusRef,
}: {
  open: boolean;
  onClose: () => void;
  returnFocusRef: RefObject<HTMLElement | null>;
}) {
  return (
    <OverlayPanel
      open={open}
      title="Tree editor help"
      titleId="program-canvas-help-title"
      kind="info"
      onClose={onClose}
      returnFocusRef={returnFocusRef}
    >
      <div className="program-canvas-help-panel">
        <details className="program-canvas-help-howto">
          <summary>How to use the tree editor</summary>
          <ul>
            <li><strong>Add a node:</strong> drag it from the node palette onto the canvas, or right-click empty canvas → <em>Add node here</em>. With a node selected, pick a <em>Node type</em> in the bar below the canvas and use <em>Add child</em> or <em>Add after</em>.</li>
            <li><strong>Connect:</strong> drag from a parent's output handle to a child's input handle. To detach a subtree, double-click its link or right-click it → <em>Disconnect</em>.</li>
            <li><strong>Reorder:</strong> drag a node past its siblings, or use <em>Move earlier</em> / <em>Move later</em>.</li>
            <li><strong>Edit:</strong> click a node to open it in Properties. Right-click it for Duplicate, Copy/Paste, Wrap in, Unwrap, Change type, Detach and Delete.</li>
            <li><strong>View:</strong> the toolbar switches layout direction, aligns the tree automatically, fits it to the view and sets the link style.</li>
            <li><strong>Apply:</strong> edits change only the Draft. Apply in the header makes them the running Program; Save writes the Scenario.</li>
          </ul>
        </details>

        <section aria-labelledby="program-canvas-help-status">
          <h4 id="program-canvas-help-status">Execution status (border)</h4>
          <ul className="program-canvas-help-legend">
            {STATUSES.map(([status, label, text]) => (
              <li key={status}>
                <span className={`program-canvas-help-swatch status-${status}`} aria-hidden="true">
                  {status === 'held' && <ProgramHeldBadge />}
                </span>
                <span><strong>{label}</strong> {text}</span>
              </li>
            ))}
            <li>
              <span className="program-canvas-help-swatch invalid" aria-hidden="true" />
              <span><strong>Invalid</strong> Required configuration is missing or wrong.</span>
            </li>
          </ul>
          <p>A link takes the colour of the node it leads to.</p>
        </section>

        <section aria-labelledby="program-canvas-help-family">
          <h4 id="program-canvas-help-family">Node families (icon colour)</h4>
          <ul className="program-canvas-help-legend">
            {FAMILIES.map(([category, label, text]) => (
              <li key={category}>
                <span className="program-canvas-help-icon"><ProgramCategoryIcon category={category} /></span>
                <span><strong>{label}</strong> {text}</span>
              </li>
            ))}
          </ul>
        </section>

        <section aria-labelledby="program-canvas-help-availability">
          <h4 id="program-canvas-help-availability">Availability (corner mark)</h4>
          <ul className="program-canvas-help-legend">
            <li>
              <span className="program-canvas-help-mark configuration_required"><ProgramKindSymbol kind="leaf" /></span>
              <span><strong>Configuration required</strong> Complete the node's required settings.</span>
            </li>
            <li>
              <span className="program-canvas-help-mark"><ProgramKindSymbol kind="leaf" /></span>
              <span><strong>Not executable</strong> It can be authored, but cannot run with the current Applied configuration; hover the node for the reason.</span>
            </li>
          </ul>
          <p>The mark's shape tells the node kind: dot for an action, diamond for a composite, outlined diamond for a decorator.</p>
        </section>

        <section aria-labelledby="program-canvas-help-collapse">
          <h4 id="program-canvas-help-collapse">Collapsing</h4>
          <ul>
            <li>Hover a node with children and click its chevron, or right-click it → <em>Collapse</em>. Its whole subtree is listed inside it, each nested node keeping its live status.</li>
            <li>Right-click a node or empty canvas for <em>Collapse all</em> and <em>Expand all</em>. On a listed node, <em>Show on canvas</em> expands the nodes above it.</li>
            <li>Collapsing only changes the view: the Program is not edited. While anything is collapsed the tree is laid out automatically; expanding everything restores your layout.</li>
          </ul>
        </section>

        <section aria-labelledby="program-canvas-help-keys">
          <h4 id="program-canvas-help-keys">Shortcuts</h4>
          <dl className="program-canvas-help-keys">
            {SHORTCUTS.map(([keys, text]) => (
              <div key={keys}><dt><kbd>{keys}</kbd></dt><dd>{text}</dd></div>
            ))}
          </dl>
        </section>
      </div>
    </OverlayPanel>
  );
}
