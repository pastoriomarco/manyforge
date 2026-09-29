import { useEffect, useState } from 'react';

/** Gap and inner padding of `.dseg`, as set in shared/panels.css. */
const SEGMENT_GAP_PX = 4;
const SEGMENT_PADDING_PX = 3;

/**
 * Largest options-per-row that fits, for a segmented choice that does not fit
 * on one row. Rows stay balanced: the last row is at most one option short,
 * so five options go 3 + 2, 2 + 2 + 1 or one per row (never 4 + 1), and six
 * go 3 + 3 (never 4 + 2).
 */
export function balancedOptionsPerRow(count: number, widest: number, inner: number): number {
  const fits = (perRow: number) => widest * perRow + SEGMENT_GAP_PX * (perRow - 1) <= inner;
  for (let perRow = count - 1; perRow >= 1; perRow -= 1) {
    const lastRow = count % perRow;
    if (lastRow !== 0 && lastRow < perRow - 1) continue;
    if (fits(perRow)) return perRow;
  }
  return 1;
}

/** One row with natural widths when all options fit; else balanced rows. */
function balanceSegments(root: HTMLElement): void {
  const segments = [...root.querySelectorAll<HTMLElement>('.dseg')];
  if (root.matches('.dseg')) segments.push(root);
  segments.forEach((segment) => {
    const options = [...segment.children] as HTMLElement[];
    segment.style.removeProperty('--dseg-basis');
    if (options.length < 2) return;
    options.forEach((option) => { option.style.flexGrow = '0'; });
    const widths = options.map((option) => option.getBoundingClientRect().width);
    options.forEach((option) => { option.style.removeProperty('flex-grow'); });
    const inner = segment.clientWidth - 2 * SEGMENT_PADDING_PX;
    const total = widths.reduce((sum, width) => sum + width, 0)
      + SEGMENT_GAP_PX * (widths.length - 1);
    if (total <= inner) return;
    const perRow = balancedOptionsPerRow(options.length, Math.max(...widths), inner);
    segment.style.setProperty(
      '--dseg-basis',
      `calc((100% - ${SEGMENT_GAP_PX * (perRow - 1)}px) / ${perRow})`,
    );
  });
}

/**
 * A single-line field or select whose value does not fit in its two-thirds
 * column takes its own row. The field being edited keeps its layout, so the row
 * never jumps while typing.
 */
let measureContext: CanvasRenderingContext2D | null | undefined;

/** Width of a select's chosen option text, plus its padding and arrow. */
function selectNeedsWidth(select: HTMLSelectElement): number | null {
  if (measureContext === undefined) {
    measureContext = document.createElement('canvas').getContext('2d');
  }
  const option = select.selectedOptions[0];
  if (measureContext === null || option === undefined) return null;
  const style = getComputedStyle(select);
  measureContext.font = `${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
  const padding = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
  const arrow = parseFloat(style.fontSize) * 1.5;
  return measureContext.measureText(option.text).width + padding + arrow;
}

function stackOverflowingFields(root: HTMLElement): void {
  root.querySelectorAll<HTMLInputElement | HTMLSelectElement>(
    '.dcard label input, .dcard label select',
  ).forEach((field) => {
    if (field instanceof HTMLInputElement
      && ['checkbox', 'radio', 'range', 'color'].includes(field.type)) return;
    const label = field.closest('label');
    if (label === null || field === document.activeElement) return;
    label.classList.remove('stacked');
    const needed = field instanceof HTMLSelectElement
      ? selectNeedsWidth(field)
      : field.scrollWidth;
    if (needed !== null && needed > field.clientWidth + 1) label.classList.add('stacked');
  });
}

/**
 * Lays out a card panel (`.dpanel`), or a single `.dseg`: balances the
 * segmented choices and stacks fields that do not fit. Runs when the panel opens, resizes, gains
 * or loses content, or a card is expanded. Returns the ref for the panel.
 */
export function useCardPanelLayout(): (element: HTMLElement | null) => void {
  const [root, setRoot] = useState<HTMLElement | null>(null);
  useEffect(() => {
    if (root === null) return undefined;
    let frame = 0;
    const layout = () => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(() => {
        stackOverflowingFields(root);
        balanceSegments(root);
      });
    };
    layout();
    const resize = new ResizeObserver(layout);
    resize.observe(root);
    const content = new MutationObserver(layout);
    content.observe(root, { childList: true, subtree: true });
    root.addEventListener('toggle', layout, true);
    return () => {
      cancelAnimationFrame(frame);
      resize.disconnect();
      content.disconnect();
      root.removeEventListener('toggle', layout, true);
    };
  }, [root]);
  return setRoot;
}
