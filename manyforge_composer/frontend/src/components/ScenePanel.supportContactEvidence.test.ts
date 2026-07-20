import { describe, expect, it } from 'vitest';
import {
  contactReleaseEvidenceMode,
  defaultSupportContact,
  sanitizedSupportContact,
  supportContactModality,
} from './ScenePanel';

describe('ScenePanel support-contact evidence authoring', () => {
  it('defaults new contacts to precise evidence', () => {
    const contact = defaultSupportContact('table');

    expect(contactReleaseEvidenceMode(contact)).toBe('signed_penetration');
    expect(supportContactModality(contact)).toBe('signed_penetration');
    expect(contact.release_evidence_mode).toBe('signed_penetration');
    expect(contact.release_tolerance_m).toBe(0.00001);
  });

  it('removes metric-only fields when lightweight evidence is serialized', () => {
    const contact = sanitizedSupportContact('table', {
      ...defaultSupportContact('table'),
      release_evidence_mode: 'binary_contact',
    });

    expect(contact).toMatchObject({
      support_object_id: 'table',
      release_evidence_mode: 'binary_contact',
      require_separation_by_end: true,
      allow_recontact: false,
    });
    expect(contact).not.toHaveProperty('enforce_max_start_penetration');
    expect(contact).not.toHaveProperty('max_start_penetration_m');
    expect(contact).not.toHaveProperty('release_tolerance_m');
    expect(contact).not.toHaveProperty('min_release_progress_m');
    expect(contact).not.toHaveProperty('require_strictly_decreasing_penetration');
  });

  it('serializes bounded penetration with only its mode-specific threshold', () => {
    const contact = sanitizedSupportContact('fixture', {
      ...defaultSupportContact('fixture'),
      mode: 'bounded_penetration',
      max_contact_penetration_m: 0.0015,
    });

    expect(supportContactModality(contact)).toBe('bounded_penetration');
    expect(contact).toEqual({
      support_object_id: 'fixture',
      mode: 'bounded_penetration',
      max_contact_penetration_m: 0.0015,
    });
  });
});
