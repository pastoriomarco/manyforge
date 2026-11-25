import type { ScenarioUpgradeConversionKind, StructuredError } from '../types/api';
import { errorCatalogRow } from './commandRefusal';

/**
 * Load refusals of a Scenario saved in an older file format that an upgrade
 * may convert (OD-391). The Load dialog then asks the server which
 * conversions apply; a Scenario already current is never offered one.
 */
export const SCENARIO_UPGRADE_REFUSAL_CODES: ReadonlySet<string> = new Set([
  'scenario_document_dropped_fields',
  'scenario_program_profile_renamed',
  'program_format_unsupported',
  // Also an older Deployment format; the server's check decides.
  'scenario_deployment_intent_invalid',
]);

/** What each conversion changes, in plain words. */
export const SCENARIO_UPGRADE_CONVERSION_TEXT: Record<ScenarioUpgradeConversionKind, string> = {
  collision_rules: 'Collision rules replace the robots’ and moves’ clearance settings, the contact policies and the Scene objects’ support contacts (Program format v4, Deployment format v3).',
  scenario_identity: 'The Program and the Deployment no longer carry their own name and description; the Scenario keeps its folder name and its description.',
  program_profile: 'The Program profile composable-cell-v1 is renamed manyforge-cell-v1.',
  lane_labels: 'Lane labels are removed from the Program and the Deployment; each component carries its own lane.',
};

export const SCENARIO_UPGRADE_OFFER = 'This Scenario was saved by an earlier ManyForge version in file formats this version no longer reads. Upgrade and load converts it to the current formats, keeps a backup copy of its folder and then loads it.';

/**
 * The catalog's operator guidance for a refusal code, unless the shown
 * refusal already includes it; null without one.
 */
export function refusalGuidance(code: string | null, shown: string | null): string | null {
  if (code === null) return null;
  const guidance = errorCatalogRow(code)?.[1] ?? '';
  return guidance === '' || (shown ?? '').includes(guidance) ? null : guidance;
}

/**
 * A refused upgrade or check: the catalog's title with the server's reason,
 * and the catalog's guidance.
 */
export function upgradeRefusal(error: unknown): { text: string; guidance: string | null } {
  const structured = (error as { structuredError?: StructuredError | null } | null)
    ?.structuredError ?? null;
  if (structured !== null) {
    const title = errorCatalogRow(structured.code)?.[0] ?? structured.code;
    const text = structured.message === '' ? `${title}.` : `${title}: ${structured.message}`;
    return { text, guidance: refusalGuidance(structured.code, text) };
  }
  return {
    text: error instanceof Error && error.message.trim() !== ''
      ? error.message : 'The Scenario could not be upgraded.',
    guidance: null,
  };
}
