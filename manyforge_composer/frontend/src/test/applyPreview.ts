import type { ApplyPreview } from '../types/components';

/** A Preview (CONTRACTS §11) with these component rows and nothing else changing. */
export function applyPreviewFixture(
  components: ApplyPreview['components'],
  extra: Partial<Omit<ApplyPreview, 'components'>> = {},
): ApplyPreview {
  return {
    components,
    scope: { deployment_draft_id: 'draft', deployment_draft_revision: 1 },
    program: {
      executable: true, program_id: 'program', unresolved_references: [],
      affected_nodes: [], affected_execution_groups: [],
    },
    attachment_changed_components: [],
    detached_components: [],
    ...extra,
  };
}
