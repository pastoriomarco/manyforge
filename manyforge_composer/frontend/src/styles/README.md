# Composer styles

Plain CSS, imported once from `src/main.tsx` through `index.css`. No UI kit,
no CSS-in-JS, no cascade layers.

## Files and order

`index.css` only `@import`s the other files. **Import order is the cascade
order:** when two rules have the same specificity, the later file wins.

| File | Holds |
|---|---|
| `tokens.css` | Every raw value: colors, font sizes, weights, radii, z-index, the monospace stack, Program node dimensions. |
| `base.css` | Element defaults (`button`, `input`, `select`, …), focus outlines, reduced motion, `.muted`, `.sr-only`. |
| `shared/buttons.css` | `primary-action`, `danger-button`, `quiet-button`, `text-button`, `icon-button`, `segmented-control`, `button-row`. |
| `shared/panels.css` | Generic panel chrome (`.panel`, `.panel-header`, `.panel-state`, `.panel-intro`). It comes before the surfaces, because surface classes on the same element refine it (for example `.viewer-header`). |
| `shared/status.css` | Status pills, readiness dots, inline error text. |
| `shared/notices.css` | Shared message looks: `notice` (box), `inline-message` (text) and `hint`, with `severity-info`, `severity-warning` and `severity-error`. |
| `shared/menus.css` | The right-click menu (`CanvasContextMenu`). |
| `surfaces/*.css` | One file per surface family: layout and the few looks that belong only to that surface. |

Cascade layers were not used: under `@layer`, a shared class rule could no
longer override an element default such as `button:hover`.

`tokens.ts` holds the colors that TypeScript needs (React Flow edges, node
categories, the three.js viewport). Values that also exist in CSS are listed
in `CSS_SHARED_TOKENS`, and `tokens.test.ts` checks that both files agree.

`readStyleSource.ts` is test support only. It returns all stylesheets in
import order with tokens resolved, so tests can assert the effective values.

## Tokens

- **Colors:** `--mf-<role>-<family>-<lightness>[-aNN]`.
  - role: `bg`, `border`, `text`, `shadow`.
  - family: `neutral`, `accent`, `danger`, `warning`, `success`, …
  - lightness: the HSL lightness in percent; `aNN` is the alpha in percent.
  - Example: `--mf-text-warning-79` is a light warning text color.
- **Semantic colors:**
  - `--mf-status-*` for execution status;
  - `--mf-availability-*` for Program node availability;
  - `--mf-category-*` for the Program node category families.
- **Scales:**
  - `--mf-font-size-NNN` (NNN is rem × 100);
  - `--mf-font-weight-*`, `--mf-radius-*`, `--mf-z-*`;
  - `--mf-font-mono`.
- The central inspector keeps its own `--inspector-*` density variables
  (`surfaces/inspector.css`).

Severity looks, reused everywhere:

| Severity | Text | Border | Background |
|---|---|---|---|
| Error | `--mf-text-danger-81` | `--mf-border-danger-38` | `--mf-bg-danger-16` |
| Warning | `--mf-text-warning-79` | `--mf-border-warning-37` | `--mf-bg-warning-13` |
| Info | `--mf-text-neutral-81` | `--mf-border-neutral-34` | `--mf-bg-neutral-15` |

## How to…

- **Change a color:** edit its token in `tokens.css`. If the token is listed in
  `CSS_SHARED_TOKENS`, change `tokens.ts` too.
- **Add a color:** first reuse an existing token with the same meaning. Add a
  new token only for a new meaning, and name it with the scheme above.
- **Style a new component:**
  - use the shared button classes and the severity looks above;
  - put layout in the surface file of its family, using tokens only.
  - Add a new file only for a new surface family, and import it in
    `index.css` after the files it must override.
- **Program nodes:** the approved design is D3 in UI6-I-VC; the reference page
  is `frontend/design/program-node-design.html`. Icons are in
  `src/components/programNodeGlyphs.tsx`.

## Guard

`styleSystem.contract.test.ts` fails when:

- a raw color appears outside `tokens.css` and `tokens.ts`;
- a class used in a component has no rule (small allowlist of behavior hooks);
- a rule prelude is written twice in one file and context.

A few repeated preludes are allowlisted because rules of equal specificity sit
between them, and merging would change which declaration wins.

`tests/style-inventory/inventory.py` writes the before/after inventory used
by UI6-I-VC.
