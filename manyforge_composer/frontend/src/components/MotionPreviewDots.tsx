import { useEffect, useRef, useState } from 'react';
import type { MotionPreviewPath } from '../types/robotControl';

type Sample = MotionPreviewPath['samples'][number];
export type PreviewSampleColor = 'clear' | 'allowed' | 'clearance' | 'refused' | 'unknown';
interface DotRun { start: number; end: number; color: PreviewSampleColor }

/** The colour describes the sampled collision check, not the spaces between samples. */
export function previewSampleColor(sample: Sample): PreviewSampleColor {
  if (sample.contacts.some((contact) => contact.disposition === 'refused')) return 'refused';
  if ((sample.clearanceViolations?.length ?? 0) > 0) return 'clearance';
  if (sample.status === 'unknown' || sample.contacts.some((contact) => contact.disposition === 'unknown')) return 'unknown';
  if (sample.contacts.some((contact) => contact.disposition === 'allowed')) return 'allowed';
  return 'clear';
}

/** Merge only adjacent same-colour samples when the track is too narrow for individual dots. */
export function previewDotRuns(samples: readonly Sample[], width: number): DotRun[] {
  const merge = width > 0 && samples.length * 12 > width;
  const runs: DotRun[] = [];
  samples.forEach((sample, index) => {
    const color = previewSampleColor(sample);
    const last = runs[runs.length - 1];
    if (merge && last !== undefined && last.color === color) last.end = index;
    else runs.push({ start: index, end: index, color });
  });
  return runs;
}

export function previewRunSampleAt(start: number, end: number, left: number, width: number, x: number, keyboard: boolean): number {
  const fraction = keyboard || width <= 0 ? 0.5 : Math.max(0, Math.min(1, (x - left) / width));
  return start + Math.round(fraction * (end - start));
}

export function MotionPreviewDots({ samples, selectedIndex, onSelect }: {
  samples: readonly Sample[];
  selectedIndex: number;
  onSelect: (index: number) => void;
}) {
  const track = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(0);
  useEffect(() => {
    const element = track.current;
    if (element === null) return;
    const measure = () => setWidth(element.clientWidth);
    measure();
    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', measure);
      return () => window.removeEventListener('resize', measure);
    }
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  const runs = previewDotRuns(samples, width);
  const denominator = Math.max(1, samples.length - 1);
  return <div ref={track} className="motion-preview-dots" role="group" aria-label="Preview sample markers">
    {runs.map((run) => {
      const first = samples[run.start];
      const last = samples[run.end];
      const label = run.start === run.end
        ? `Sample ${run.start + 1}, ${first.timeFromStartS.toFixed(2)} s, ${run.color}`
        : `Samples ${run.start + 1}–${run.end + 1}, ${first.timeFromStartS.toFixed(2)}–${last.timeFromStartS.toFixed(2)} s, ${run.color}`;
      return <button type="button" key={`${run.start}-${run.end}`} className={`motion-preview-dot is-${run.color}`}
        style={{ left: `${run.start / denominator * 100}%`, width: run.start === run.end ? undefined : `${(run.end - run.start) / denominator * 100}%` }}
        aria-label={label} title={label} aria-current={selectedIndex >= run.start && selectedIndex <= run.end ? 'true' : undefined}
        onClick={(event) => {
          if (run.start === run.end) { onSelect(run.start); return; }
          const bounds = event.currentTarget.getBoundingClientRect();
          onSelect(previewRunSampleAt(run.start, run.end, bounds.left, bounds.width, event.clientX, event.detail === 0));
        }} />;
    })}
  </div>;
}
